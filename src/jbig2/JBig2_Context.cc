// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_Context.hh"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "JBig2_ArithDecoder.hh"
#include "JBig2_ArithIntDecoder.hh"
#include "JBig2_BitStream.hh"
#include "JBig2_Define.hh"
#include "JBig2_GrdProc.hh"
#include "JBig2_GrrdProc.hh"
#include "JBig2_Image.hh"
#include "JBig2_PatternDict.hh"
#include "JBig2_SddProc.hh"
#include "JBig2_Segment.hh"
#include "JBig2_SymbolDict.hh"
#include "JBig2_TrdProc.hh"

namespace nanopdf {
namespace jbig2 {

namespace {

// Context sizes for different generic region templates
constexpr int kGBContextSize[] = {65536, 8192, 4096, 1024};
constexpr int kGRContextSize[] = {8192, 1024};

}  // namespace

CJBig2_Context::CJBig2_Context(const uint8_t* globalsData, size_t globalsSize,
                               const uint8_t* data, size_t size,
                               bool isGlobal)
    : is_global_(isGlobal) {
  // Create main stream
  if (data && size > 0) {
    stream_ = std::unique_ptr<CJBig2_BitStream>(
        new CJBig2_BitStream(data, size, 0));
  }

  // Create globals stream if present
  if (globalsData && globalsSize > 0) {
    globals_stream_ = std::unique_ptr<CJBig2_BitStream>(
        new CJBig2_BitStream(globalsData, globalsSize, 0));
  }

  // Initialize context arrays with default sizes
  gb_contexts_.resize(kGBContextSize[0]);
  gr_contexts_.resize(kGRContextSize[0]);
}

CJBig2_Context::~CJBig2_Context() = default;

int CJBig2_Context::GetGBContextSize(uint8_t templ) {
  if (templ > 3) templ = 0;
  return kGBContextSize[templ];
}

int CJBig2_Context::GetGRContextSize(uint8_t templ) {
  if (templ > 1) templ = 0;
  return kGRContextSize[templ];
}

JBig2DecodeResult CJBig2_Context::Decode() {
  JBig2DecodeResult result;

  if (!stream_) {
    result.error = "No JBIG2 data stream";
    return result;
  }

  // Parse global segments first (if present)
  if (globals_stream_) {
    std::swap(stream_, globals_stream_);
    JBig2Status status = ParseSegmentHeaders();
    if (status != JBIG2_SUCCESS && status != JBIG2_END_OF_FILE_REACHED) {
      result.error = "Failed to parse global segments: " + error_;
      std::swap(stream_, globals_stream_);
      return result;
    }

    // Process global segments
    for (auto& seg : segments_) {
      status = ProcessSegment(seg.get());
      if (status == JBIG2_ERROR_FATAL) {
        result.error = "Failed to process global segment: " + error_;
        std::swap(stream_, globals_stream_);
        return result;
      }
    }

    // Move to global_segments_
    global_segments_ = std::move(segments_);
    segments_.clear();

    std::swap(stream_, globals_stream_);
  }

  // Parse main stream segments
  JBig2Status status = ParseSegmentHeaders();
  if (status != JBIG2_SUCCESS && status != JBIG2_END_OF_FILE_REACHED &&
      status != JBIG2_END_OF_PAGE_REACHED) {
    result.error = "Failed to parse segments: " + error_;
    return result;
  }

  // Process all segments
  for (auto& seg : segments_) {
    status = ProcessSegment(seg.get());
    if (status == JBIG2_ERROR_FATAL) {
      result.error = "Failed to process segment: " + error_;
      return result;
    }
    if (status == JBIG2_END_OF_PAGE_REACHED) {
      break;
    }
  }

  // Return the page image
  if (page_) {
    result.success = true;
    result.width = page_->width();
    result.height = page_->height();
    result.image = std::move(page_);
  } else {
    result.error = "No page image generated";
  }

  return result;
}

JBig2Status CJBig2_Context::ParseSegmentHeaders() {
  if (!stream_) {
    return JBIG2_ERROR_FATAL;
  }

  while (stream_->getByteLeft() > 0) {
    auto pSegment = std::unique_ptr<CJBig2_Segment>(new CJBig2_Segment());

    JBig2Status status = ParseSegmentHeader(pSegment.get());
    if (status != JBIG2_SUCCESS) {
      if (status == JBIG2_END_OF_FILE_REACHED) {
        return status;
      }
      error_ = "Failed to parse segment header";
      return status;
    }

    segments_.push_back(std::move(pSegment));
  }

  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ParseSegmentHeader(CJBig2_Segment* pSegment) {
  // Read segment number (4 bytes)
  if (stream_->readInteger(&pSegment->number_) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Read segment header flags (1 byte)
  uint8_t flags;
  if (stream_->read1Byte(&flags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }
  pSegment->flags_.c = flags;

  // Get segment type
  uint8_t segType = pSegment->flags_.s.type;

  // Check for end of file segment
  if (segType == JBIG2_END_OF_FILE) {
    return JBIG2_END_OF_FILE_REACHED;
  }

  // Read referred-to segment count
  uint8_t refCountByte;
  if (stream_->read1Byte(&refCountByte) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  int32_t refCount;
  if ((refCountByte & 0xE0) == 0xE0) {
    // Long form: 4 + 3 bytes for count
    uint32_t longCount;
    if (stream_->readInteger(&longCount) != 0) {
      return JBIG2_ERROR_TOO_SHORT;
    }
    refCount = longCount & 0x1FFFFFFF;
    // Skip padding if needed
    uint32_t padding = (refCount + 9) % 4;
    if (padding > 0) {
      stream_->addOffset(4 - padding);
    }
  } else {
    refCount = (refCountByte >> 5) & 0x07;
  }

  pSegment->referred_to_segment_count_ = refCount;

  // Read referred-to segment numbers
  if (refCount > 0) {
    pSegment->referred_to_segment_numbers_.resize(refCount);

    // Determine size of segment number references
    int refSize = (pSegment->number_ <= 256) ? 1 :
                  (pSegment->number_ <= 65536) ? 2 : 4;

    for (int32_t i = 0; i < refCount; ++i) {
      uint32_t refNum = 0;
      if (refSize == 1) {
        uint8_t b;
        if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
        refNum = b;
      } else if (refSize == 2) {
        uint16_t w;
        if (stream_->readShortInteger(&w) != 0) return JBIG2_ERROR_TOO_SHORT;
        refNum = w;
      } else {
        if (stream_->readInteger(&refNum) != 0) return JBIG2_ERROR_TOO_SHORT;
      }
      pSegment->referred_to_segment_numbers_[i] = refNum;
    }
  }

  // Read page association
  if (pSegment->flags_.s.page_association_size) {
    if (stream_->readInteger(&pSegment->page_association_) != 0) {
      return JBIG2_ERROR_TOO_SHORT;
    }
  } else {
    uint8_t pageAssoc;
    if (stream_->read1Byte(&pageAssoc) != 0) {
      return JBIG2_ERROR_TOO_SHORT;
    }
    pSegment->page_association_ = pageAssoc;
  }

  // Read segment data length
  if (stream_->readInteger(&pSegment->data_length_) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Record data offset
  pSegment->data_offset_ = stream_->getOffset();
  pSegment->header_length_ = pSegment->data_offset_;

  // Skip to end of segment data (we'll process it later)
  if (pSegment->data_length_ != 0xFFFFFFFF) {
    stream_->addOffset(pSegment->data_length_);
  }

  pSegment->state_ = JBIG2_SEGMENT_DATA_UNPARSED;
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessSegment(CJBig2_Segment* pSegment) {
  if (pSegment->state_ == JBIG2_SEGMENT_PARSE_COMPLETE) {
    return JBIG2_SUCCESS;
  }

  // Position stream at segment data
  stream_->setOffset(pSegment->data_offset_);

  uint8_t segType = pSegment->flags_.s.type;
  JBig2Status status = JBIG2_SUCCESS;

  switch (segType) {
    case JBIG2_SYMBOL_DICT:
      status = ProcessSymbolDict(pSegment);
      break;
    case JBIG2_INTERMEDIATE_TEXT_REGION:
    case JBIG2_IMMEDIATE_TEXT_REGION:
    case JBIG2_IMMEDIATE_LOSSLESS_TEXT_REGION:
      status = ProcessTextRegion(pSegment);
      break;
    case JBIG2_PATTERN_DICT:
      status = ProcessPatternDict(pSegment);
      break;
    case JBIG2_INTERMEDIATE_HALFTONE_REGION:
    case JBIG2_IMMEDIATE_HALFTONE_REGION:
    case JBIG2_IMMEDIATE_LOSSLESS_HALFTONE_REGION:
      status = ProcessHalftoneRegion(pSegment);
      break;
    case JBIG2_INTERMEDIATE_GENERIC_REGION:
    case JBIG2_IMMEDIATE_GENERIC_REGION:
    case JBIG2_IMMEDIATE_LOSSLESS_GENERIC_REGION:
      status = ProcessGenericRegion(pSegment);
      break;
    case JBIG2_INTERMEDIATE_GENERIC_REFINEMENT_REGION:
    case JBIG2_IMMEDIATE_GENERIC_REFINEMENT_REGION:
    case JBIG2_IMMEDIATE_LOSSLESS_GENERIC_REFINEMENT_REGION:
      status = ProcessGenericRefinementRegion(pSegment);
      break;
    case JBIG2_PAGE_INFO:
      status = ProcessPageInfo(pSegment);
      break;
    case JBIG2_END_OF_PAGE:
      status = ProcessEndOfPage(pSegment);
      break;
    case JBIG2_END_OF_STRIPE:
      status = ProcessEndOfStripe(pSegment);
      break;
    case JBIG2_TABLES:
      status = ProcessTables(pSegment);
      break;
    case JBIG2_END_OF_FILE:
      return JBIG2_END_OF_FILE_REACHED;
    default:
      // Unknown segment type - skip
      break;
  }

  if (status == JBIG2_SUCCESS) {
    pSegment->state_ = JBIG2_SEGMENT_PARSE_COMPLETE;
  }

  return status;
}

JBig2Status CJBig2_Context::ProcessSymbolDict(CJBig2_Segment* pSegment) {
  // Read symbol dictionary flags (2 bytes)
  uint16_t wFlags;
  if (stream_->readShortInteger(&wFlags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  CJBig2_SDDProc SDD;
  SDD.SDHUFF = (wFlags & 0x0001) != 0;
  SDD.SDREFAGG = (wFlags & 0x0002) != 0;
  SDD.SDTEMPLATE = (wFlags >> 10) & 0x03;
  SDD.SDRTEMPLATE = (wFlags >> 12) & 0x01;
  SDD.SDHUFFDH = (wFlags >> 2) & 0x03;
  SDD.SDHUFFDW = (wFlags >> 4) & 0x03;
  SDD.SDHUFFBMSIZE = (wFlags >> 6) & 0x01;
  SDD.SDHUFFAGGINST = (wFlags >> 7) & 0x01;

  // Read adaptive template pixels if not Huffman
  if (!SDD.SDHUFF) {
    if (SDD.SDTEMPLATE == 0) {
      for (int i = 0; i < 8; ++i) {
        uint8_t b;
        if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
        SDD.SDAT[i] = static_cast<int8_t>(b);
      }
    } else {
      for (int i = 0; i < 2; ++i) {
        uint8_t b;
        if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
        SDD.SDAT[i] = static_cast<int8_t>(b);
      }
    }
  }

  // Read refinement template pixels
  if (SDD.SDREFAGG && SDD.SDRTEMPLATE == 0) {
    for (int i = 0; i < 4; ++i) {
      uint8_t b;
      if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
      SDD.SDRAT[i] = static_cast<int8_t>(b);
    }
  }

  // Read number of exported and new symbols
  if (stream_->readInteger(&SDD.SDNUMEXSYMS) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }
  if (stream_->readInteger(&SDD.SDNUMNEWSYMS) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Get input symbols from referred segments
  auto refDicts = GetReferencedSymbolDicts(pSegment);
  SDD.SDINSYMS = GetAllSymbols(refDicts);
  SDD.SDNUMINSYMS = static_cast<uint32_t>(SDD.SDINSYMS.size());

  // Resize context arrays
  gb_contexts_.resize(GetGBContextSize(SDD.SDTEMPLATE));
  gr_contexts_.resize(GetGRContextSize(SDD.SDRTEMPLATE));

  // Decode symbol dictionary
  std::unique_ptr<CJBig2_SymbolDict> pDict;
  if (SDD.SDHUFF) {
    pDict = SDD.DecodeHuffman(stream_.get(), &gb_contexts_, &gr_contexts_);
  } else {
    CJBig2_ArithDecoder arith(stream_.get());
    pDict = SDD.DecodeArith(&arith, &gb_contexts_, &gr_contexts_);
  }

  if (!pDict) {
    error_ = "Failed to decode symbol dictionary";
    return JBIG2_ERROR_FATAL;
  }

  pSegment->symbol_dict_ = std::move(pDict);
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessTextRegion(CJBig2_Segment* pSegment) {
  // Read region segment info header
  JBig2RegionInfo regionInfo;
  if (stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.width)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.height)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.x)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.y)) != 0 ||
      stream_->read1Byte(&regionInfo.flags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Read text region segment flags (2 bytes)
  uint16_t wFlags;
  if (stream_->readShortInteger(&wFlags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  CJBig2_TRDProc TRD;
  TRD.SBW = regionInfo.width;
  TRD.SBH = regionInfo.height;
  TRD.SBHUFF = (wFlags & 0x0001) != 0;
  TRD.SBREFINE = (wFlags & 0x0002) != 0;
  uint8_t SBHUFFLOG = static_cast<uint8_t>((wFlags >> 2) & 0x03);
  TRD.SBSTRIPS = SBHUFFLOG;
  TRD.REFCORNER = (wFlags >> 4) & 0x03;
  TRD.TRANSPOSED = (wFlags & 0x0040) != 0;
  TRD.SBCOMBOP = static_cast<JBig2ComposeOp>((wFlags >> 7) & 0x03);
  TRD.SBDEFPIXEL = (wFlags >> 9) & 0x01;
  TRD.SBDSOFFSET = static_cast<int8_t>((wFlags >> 10) & 0x1F);
  if (TRD.SBDSOFFSET & 0x10) {
    TRD.SBDSOFFSET |= 0xE0;  // Sign extend
  }
  TRD.SBRTEMPLATE = (wFlags >> 15) & 0x01;

  // Read Huffman table selections if needed
  if (TRD.SBHUFF) {
    uint16_t wHuffFlags;
    if (stream_->readShortInteger(&wHuffFlags) != 0) {
      return JBIG2_ERROR_TOO_SHORT;
    }
    TRD.SBHUFFFS = wHuffFlags & 0x03;
    TRD.SBHUFFDS = (wHuffFlags >> 2) & 0x03;
    TRD.SBHUFFDT = (wHuffFlags >> 4) & 0x03;
    TRD.SBHUFFRDW = (wHuffFlags >> 6) & 0x03;
    TRD.SBHUFFRDH = (wHuffFlags >> 8) & 0x03;
    TRD.SBHUFFRDX = (wHuffFlags >> 10) & 0x03;
    TRD.SBHUFFRDY = (wHuffFlags >> 12) & 0x03;
    TRD.SBHUFFRSIZE = (wHuffFlags >> 14) & 0x01;
  }

  // Read refinement template if needed
  if (TRD.SBREFINE && TRD.SBRTEMPLATE == 0) {
    for (int i = 0; i < 4; ++i) {
      uint8_t b;
      if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
      TRD.SBRAT[i] = static_cast<int8_t>(b);
    }
  }

  // Read number of symbol instances
  if (stream_->readInteger(&TRD.SBNUMINSTANCES) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Get symbols from referred segments
  auto refDicts = GetReferencedSymbolDicts(pSegment);
  TRD.SBSYMS = GetAllSymbols(refDicts);
  TRD.SBNUMSYMS_TOTAL = static_cast<uint32_t>(TRD.SBSYMS.size());

  // Calculate symbol code length
  uint32_t SBSYMCODELEN = 0;
  while ((1u << SBSYMCODELEN) < TRD.SBNUMSYMS_TOTAL) {
    ++SBSYMCODELEN;
  }
  if (SBSYMCODELEN == 0) {
    SBSYMCODELEN = 1;
  }

  // Resize refinement context
  gr_contexts_.resize(GetGRContextSize(TRD.SBRTEMPLATE));

  // Decode text region
  std::unique_ptr<CJBig2_Image> pImage;
  if (TRD.SBHUFF) {
    pImage = TRD.DecodeHuffman(stream_.get(), &gr_contexts_);
  } else {
    CJBig2_ArithDecoder arith(stream_.get());
    auto pIAID = std::unique_ptr<CJBig2_ArithIaidDecoder>(
        new CJBig2_ArithIaidDecoder(static_cast<uint8_t>(SBSYMCODELEN)));
    pImage = TRD.DecodeArith(&arith, &gr_contexts_, pIAID.get());
  }

  if (!pImage) {
    error_ = "Failed to decode text region";
    return JBIG2_ERROR_FATAL;
  }

  // Composite onto page if immediate
  uint8_t segType = pSegment->flags_.s.type;
  if (segType == JBIG2_IMMEDIATE_TEXT_REGION ||
      segType == JBIG2_IMMEDIATE_LOSSLESS_TEXT_REGION) {
    if (page_) {
      JBig2ComposeOp op = static_cast<JBig2ComposeOp>(regionInfo.flags & 0x03);
      page_->ComposeFrom(regionInfo.x, regionInfo.y, pImage.get(), op);
    }
  } else {
    // Store for later reference
    pSegment->image_ = std::move(pImage);
  }

  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessPatternDict(CJBig2_Segment* pSegment) {
  // Simplified - just skip pattern dictionaries for now
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessHalftoneRegion(CJBig2_Segment* pSegment) {
  // Simplified - just skip halftone regions for now
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessGenericRegion(CJBig2_Segment* pSegment) {
  // Read region segment info header
  JBig2RegionInfo regionInfo;
  if (stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.width)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.height)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.x)) != 0 ||
      stream_->readInteger(reinterpret_cast<uint32_t*>(&regionInfo.y)) != 0 ||
      stream_->read1Byte(&regionInfo.flags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Read generic region segment flags (1 byte)
  uint8_t cFlags;
  if (stream_->read1Byte(&cFlags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  CJBig2_GRDProc GRD;
  GRD.MMR = (cFlags & 0x01) != 0;
  GRD.GBTEMPLATE = (cFlags >> 1) & 0x03;
  GRD.TPGDON = (cFlags & 0x08) != 0;
  GRD.USESKIP = false;
  GRD.SKIP = nullptr;
  GRD.GBW = regionInfo.width;
  GRD.GBH = regionInfo.height;

  // Read adaptive template pixels if not MMR
  if (!GRD.MMR) {
    if (GRD.GBTEMPLATE == 0) {
      for (int i = 0; i < 8; ++i) {
        uint8_t b;
        if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
        GRD.GBAT[i] = static_cast<int8_t>(b);
      }
    } else {
      for (int i = 0; i < 2; ++i) {
        uint8_t b;
        if (stream_->read1Byte(&b) != 0) return JBIG2_ERROR_TOO_SHORT;
        GRD.GBAT[i] = static_cast<int8_t>(b);
      }
    }
  }

  // Resize context array
  gb_contexts_.resize(GetGBContextSize(GRD.GBTEMPLATE));

  // Decode generic region
  std::unique_ptr<CJBig2_Image> pImage;
  if (GRD.MMR) {
    pImage = GRD.DecodeMMR(stream_.get());
  } else {
    CJBig2_ArithDecoder arith(stream_.get());
    pImage = GRD.DecodeArith(&arith, gb_contexts_.data(),
                             static_cast<int>(gb_contexts_.size()));
  }

  if (!pImage) {
    error_ = "Failed to decode generic region";
    return JBIG2_ERROR_FATAL;
  }

  // Composite onto page if immediate
  uint8_t segType = pSegment->flags_.s.type;
  if (segType == JBIG2_IMMEDIATE_GENERIC_REGION ||
      segType == JBIG2_IMMEDIATE_LOSSLESS_GENERIC_REGION) {
    if (page_) {
      JBig2ComposeOp op = static_cast<JBig2ComposeOp>(regionInfo.flags & 0x03);
      page_->ComposeFrom(regionInfo.x, regionInfo.y, pImage.get(), op);
    }
  } else {
    pSegment->image_ = std::move(pImage);
  }

  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessGenericRefinementRegion(CJBig2_Segment* pSegment) {
  // Simplified - skip for now
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessPageInfo(CJBig2_Segment* pSegment) {
  // Read page bitmap width and height
  uint32_t width, height;
  if (stream_->readInteger(&width) != 0 ||
      stream_->readInteger(&height) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  page_width_ = width;
  page_height_ = height;

  // Read X and Y resolution (skip for now)
  uint32_t xRes, yRes;
  if (stream_->readInteger(&xRes) != 0 ||
      stream_->readInteger(&yRes) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Read page segment flags (1 byte)
  uint8_t pageFlags;
  if (stream_->read1Byte(&pageFlags) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  bool defPixel = (pageFlags & 0x04) != 0;
  page_is_lossless_ = (pageFlags & 0x01) != 0;

  // Read striping info (2 bytes)
  uint16_t stripeInfo;
  if (stream_->readShortInteger(&stripeInfo) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  page_stripe_height_ = stripeInfo & 0x7FFF;
  if (page_stripe_height_ == 0) {
    page_stripe_height_ = height;
  }

  // Create page image
  // Handle unknown height (0xFFFFFFFF) by starting with reasonable size
  uint32_t actualHeight = (height == 0xFFFFFFFF) ? page_stripe_height_ : height;

  if (!CJBig2_Image::IsValidImageSize(width, actualHeight)) {
    error_ = "Invalid page dimensions";
    return JBIG2_ERROR_FATAL;
  }

  page_ = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(width, actualHeight));
  if (!page_->has_data()) {
    error_ = "Failed to allocate page image";
    return JBIG2_ERROR_FATAL;
  }

  page_->Fill(defPixel);
  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessEndOfPage(CJBig2_Segment* pSegment) {
  return JBIG2_END_OF_PAGE_REACHED;
}

JBig2Status CJBig2_Context::ProcessEndOfStripe(CJBig2_Segment* pSegment) {
  // Read Y location
  uint32_t yLoc;
  if (stream_->readInteger(&yLoc) != 0) {
    return JBIG2_ERROR_TOO_SHORT;
  }

  // Expand page if needed
  if (page_ && page_height_ == 0xFFFFFFFF) {
    uint32_t newHeight = yLoc + 1;
    if (newHeight > static_cast<uint32_t>(page_->height())) {
      page_->Expand(newHeight, false);
    }
  }

  return JBIG2_SUCCESS;
}

JBig2Status CJBig2_Context::ProcessTables(CJBig2_Segment* pSegment) {
  // Huffman tables - skip for now
  return JBIG2_SUCCESS;
}

std::vector<CJBig2_SymbolDict*> CJBig2_Context::GetReferencedSymbolDicts(
    CJBig2_Segment* pSegment) {
  std::vector<CJBig2_SymbolDict*> result;

  for (uint32_t refNum : pSegment->referred_to_segment_numbers_) {
    CJBig2_Segment* pRef = FindSegment(refNum);
    if (pRef && pRef->symbol_dict_) {
      result.push_back(pRef->symbol_dict_.get());
    }
  }

  return result;
}

std::vector<CJBig2_Image*> CJBig2_Context::GetAllSymbols(
    const std::vector<CJBig2_SymbolDict*>& dicts) {
  std::vector<CJBig2_Image*> result;

  for (CJBig2_SymbolDict* dict : dicts) {
    if (!dict) continue;
    for (size_t i = 0; i < dict->NumImages(); ++i) {
      result.push_back(dict->GetImage(i));
    }
  }

  return result;
}

CJBig2_Segment* CJBig2_Context::FindSegment(uint32_t number) {
  // Search in main segments
  for (auto& seg : segments_) {
    if (seg->number_ == number) {
      return seg.get();
    }
  }

  // Search in global segments
  for (auto& seg : global_segments_) {
    if (seg->number_ == number) {
      return seg.get();
    }
  }

  return nullptr;
}

}  // namespace jbig2
}  // namespace nanopdf
