// Copyright 2015 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_GrdProc.hh"

#include "JBig2_ArithDecoder.hh"
#include "JBig2_BitStream.hh"
#include "JBig2_Image.hh"
#include "../ccitt-decoder.hh"

#include <cstring>

namespace nanopdf {
namespace jbig2 {

namespace {

// Template optimization constants for different modes
const uint16_t kOptConstant1[3] = {0x9b25, 0x0795, 0x00e5};
const uint16_t kOptConstant2[3] = {0x0006, 0x0004, 0x0001};
const uint16_t kOptConstant3[3] = {0xf800, 0x1e00, 0x0380};
const uint16_t kOptConstant4[3] = {0x0000, 0x0001, 0x0003};
const uint16_t kOptConstant5[3] = {0x07f0, 0x01f8, 0x007c};
const uint16_t kOptConstant6[3] = {0x7bf7, 0x0efb, 0x01bd};
const uint16_t kOptConstant7[3] = {0x0800, 0x0200, 0x0080};
const uint16_t kOptConstant8[3] = {0x0010, 0x0008, 0x0004};
const uint16_t kOptConstant9[3] = {0x000c, 0x0009, 0x0007};
const uint16_t kOptConstant10[3] = {0x0007, 0x000f, 0x0007};
const uint16_t kOptConstant11[3] = {0x001f, 0x001f, 0x000f};
const uint16_t kOptConstant12[3] = {0x000f, 0x0007, 0x0003};

}  // namespace

CJBig2_GRDProc::CJBig2_GRDProc()
    : MMR(false),
      TPGDON(false),
      USESKIP(false),
      GBTEMPLATE(0),
      GBW(0),
      GBH(0),
      SKIP(nullptr) {
  GBAT[0] = 3;
  GBAT[1] = -1;
  GBAT[2] = -3;
  GBAT[3] = -1;
  GBAT[4] = 2;
  GBAT[5] = -2;
  GBAT[6] = -2;
  GBAT[7] = -2;
}

CJBig2_GRDProc::~CJBig2_GRDProc() = default;

bool CJBig2_GRDProc::UseTemplate0Opt3() const {
  return (GBAT[0] == 3) && (GBAT[1] == -1) && (GBAT[2] == -3) &&
         (GBAT[3] == -1) && (GBAT[4] == 2) && (GBAT[5] == -2) &&
         (GBAT[6] == -2) && (GBAT[7] == -2) && !USESKIP;
}

bool CJBig2_GRDProc::UseTemplate1Opt3() const {
  return (GBAT[0] == 3) && (GBAT[1] == -1) && !USESKIP;
}

bool CJBig2_GRDProc::UseTemplate23Opt3() const {
  return (GBAT[0] == 2) && (GBAT[1] == -1) && !USESKIP;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeArith(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* gbContexts,
    int context_size) {
  if (!CJBig2_Image::IsValidImageSize(GBW, GBH)) {
    return std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  }

  switch (GBTEMPLATE) {
    case 0:
      return UseTemplate0Opt3()
                 ? DecodeArithOpt3(pArithDecoder, gbContexts, 0)
                 : DecodeArithTemplateUnopt(pArithDecoder, gbContexts, 0);
    case 1:
      return UseTemplate1Opt3()
                 ? DecodeArithOpt3(pArithDecoder, gbContexts, 1)
                 : DecodeArithTemplateUnopt(pArithDecoder, gbContexts, 1);
    case 2:
      return UseTemplate23Opt3()
                 ? DecodeArithOpt3(pArithDecoder, gbContexts, 2)
                 : DecodeArithTemplateUnopt(pArithDecoder, gbContexts, 2);
    default:
      return UseTemplate23Opt3()
                 ? DecodeArithTemplate3Opt3(pArithDecoder, gbContexts)
                 : DecodeArithTemplate3Unopt(pArithDecoder, gbContexts);
  }
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeArithOpt3(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* gbContexts,
    int OPT) {
  auto GBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  if (!GBREG->has_data()) {
    return nullptr;
  }

  int LTP = 0;
  uint8_t* pLine = GBREG->data();
  int32_t nStride = GBREG->stride();
  int32_t nStride2 = nStride << 1;
  int32_t nLineBytes = ((GBW + 7) >> 3) - 1;
  int32_t nBitsLeft = GBW - (nLineBytes << 3);
  uint32_t height = OPT == 0 ? GBH & 0x7fffffff : GBH;

  for (uint32_t h = 0; h < height; ++h) {
    if (TPGDON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&gbContexts[kOptConstant1[OPT]]);
    }

    if (LTP) {
      GBREG->CopyLine(h, h - 1);
    } else {
      if (h > 1) {
        uint8_t* pLine1 = pLine - nStride2;
        uint8_t* pLine2 = pLine - nStride;
        uint32_t line1 = (*pLine1++) << kOptConstant2[OPT];
        uint32_t line2 = *pLine2++;
        uint32_t CONTEXT =
            (line1 & kOptConstant3[OPT]) |
            ((line2 >> kOptConstant4[OPT]) & kOptConstant5[OPT]);

        for (int32_t cc = 0; cc < nLineBytes; ++cc) {
          line1 = (line1 << 8) | ((*pLine1++) << kOptConstant2[OPT]);
          line2 = (line2 << 8) | (*pLine2++);
          uint8_t cVal = 0;

          for (int32_t k = 7; k >= 0; --k) {
            if (pArithDecoder->IsComplete()) {
              return nullptr;
            }
            int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
            cVal |= bVal << k;
            CONTEXT =
                (((CONTEXT & kOptConstant6[OPT]) << 1) | bVal |
                 ((line1 >> k) & kOptConstant7[OPT]) |
                 ((line2 >> (k + kOptConstant4[OPT])) & kOptConstant8[OPT]));
          }
          pLine[cc] = cVal;
        }

        line1 <<= 8;
        line2 <<= 8;
        uint8_t cVal1 = 0;
        for (int32_t k = 0; k < nBitsLeft; ++k) {
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
          cVal1 |= bVal << (7 - k);
          CONTEXT = (((CONTEXT & kOptConstant6[OPT]) << 1) | bVal |
                     ((line1 >> (7 - k)) & kOptConstant7[OPT]) |
                     ((line2 >> (7 + kOptConstant4[OPT] - k)) &
                      kOptConstant8[OPT]));
        }
        pLine[nLineBytes] = cVal1;
      } else {
        uint8_t* pLine2 = pLine - nStride;
        uint32_t line2 = (h & 1) ? (*pLine2++) : 0;
        uint32_t CONTEXT =
            ((line2 >> kOptConstant4[OPT]) & kOptConstant5[OPT]);

        for (int32_t cc = 0; cc < nLineBytes; ++cc) {
          if (h & 1) {
            line2 = (line2 << 8) | (*pLine2++);
          }
          uint8_t cVal = 0;
          for (int32_t k = 7; k >= 0; --k) {
            if (pArithDecoder->IsComplete()) {
              return nullptr;
            }
            int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
            cVal |= bVal << k;
            CONTEXT =
                (((CONTEXT & kOptConstant6[OPT]) << 1) | bVal |
                 ((line2 >> (k + kOptConstant4[OPT])) & kOptConstant8[OPT]));
          }
          pLine[cc] = cVal;
        }

        line2 <<= 8;
        uint8_t cVal1 = 0;
        for (int32_t k = 0; k < nBitsLeft; ++k) {
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
          cVal1 |= bVal << (7 - k);
          CONTEXT = (((CONTEXT & kOptConstant6[OPT]) << 1) | bVal |
                     (((line2 >> (7 + kOptConstant4[OPT] - k))) &
                      kOptConstant8[OPT]));
        }
        pLine[nLineBytes] = cVal1;
      }
    }
    pLine += nStride;
  }
  return GBREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeArithTemplateUnopt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* gbContexts,
    int UNOPT) {
  auto GBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  if (!GBREG->has_data()) {
    return nullptr;
  }

  GBREG->Fill(false);
  int LTP = 0;
  uint8_t MOD2 = UNOPT % 2;
  uint8_t DIV2 = UNOPT / 2;
  uint8_t SHIFT = 4 - UNOPT;

  for (uint32_t h = 0; h < GBH; h++) {
    if (TPGDON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&gbContexts[kOptConstant1[UNOPT]]);
    }

    if (LTP) {
      GBREG->CopyLine(h, h - 1);
      continue;
    }

    uint32_t line1 = GBREG->GetPixel(1 + MOD2, h - 2);
    line1 |= GBREG->GetPixel(MOD2, h - 2) << 1;
    if (UNOPT == 1) {
      line1 |= GBREG->GetPixel(0, h - 2) << 2;
    }

    uint32_t line2 = GBREG->GetPixel(2 - DIV2, h - 1);
    line2 |= GBREG->GetPixel(1 - DIV2, h - 1) << 1;
    if (UNOPT < 2) {
      line2 |= GBREG->GetPixel(0, h - 1) << 2;
    }

    uint32_t line3 = 0;
    for (uint32_t w = 0; w < GBW; w++) {
      int bVal = 0;
      if (!USESKIP || !SKIP->GetPixel(w, h)) {
        if (pArithDecoder->IsComplete()) {
          return nullptr;
        }

        uint32_t CONTEXT = line3;
        CONTEXT |= GBREG->GetPixel(w + GBAT[0], h + GBAT[1]) << SHIFT;
        CONTEXT |= line2 << (SHIFT + 1);
        CONTEXT |= line1 << kOptConstant9[UNOPT];
        if (UNOPT == 0) {
          CONTEXT |= GBREG->GetPixel(w + GBAT[2], h + GBAT[3]) << 10;
          CONTEXT |= GBREG->GetPixel(w + GBAT[4], h + GBAT[5]) << 11;
          CONTEXT |= GBREG->GetPixel(w + GBAT[6], h + GBAT[7]) << 15;
        }
        bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
        if (bVal) {
          GBREG->SetPixel(w, h, bVal);
        }
      }
      line1 = ((line1 << 1) | GBREG->GetPixel(w + 2 + MOD2, h - 2)) &
              kOptConstant10[UNOPT];
      line2 = ((line2 << 1) | GBREG->GetPixel(w + 3 - DIV2, h - 1)) &
              kOptConstant11[UNOPT];
      line3 = ((line3 << 1) | bVal) & kOptConstant12[UNOPT];
    }
  }
  return GBREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeArithTemplate3Opt3(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* gbContexts) {
  auto GBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  if (!GBREG->has_data()) {
    return nullptr;
  }

  int LTP = 0;
  uint8_t* pLine = GBREG->data();
  int32_t nStride = GBREG->stride();
  int32_t nLineBytes = ((GBW + 7) >> 3) - 1;
  int32_t nBitsLeft = GBW - (nLineBytes << 3);

  for (uint32_t h = 0; h < GBH; h++) {
    if (TPGDON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&gbContexts[0x0195]);
    }

    if (LTP) {
      GBREG->CopyLine(h, h - 1);
    } else {
      if (h > 0) {
        uint8_t* pLine1 = pLine - nStride;
        uint32_t line1 = *pLine1++;
        uint32_t CONTEXT = (line1 >> 1) & 0x03f0;

        for (int32_t cc = 0; cc < nLineBytes; cc++) {
          line1 = (line1 << 8) | (*pLine1++);
          uint8_t cVal = 0;
          for (int32_t k = 7; k >= 0; k--) {
            if (pArithDecoder->IsComplete()) {
              return nullptr;
            }
            int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
            cVal |= bVal << k;
            CONTEXT = ((CONTEXT & 0x01f7) << 1) | bVal |
                      ((line1 >> (k + 1)) & 0x0010);
          }
          pLine[cc] = cVal;
        }

        line1 <<= 8;
        uint8_t cVal1 = 0;
        for (int32_t k = 0; k < nBitsLeft; k++) {
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
          cVal1 |= bVal << (7 - k);
          CONTEXT = ((CONTEXT & 0x01f7) << 1) | bVal |
                    ((line1 >> (8 - k)) & 0x0010);
        }
        pLine[nLineBytes] = cVal1;
      } else {
        uint32_t CONTEXT = 0;
        for (int32_t cc = 0; cc < nLineBytes; cc++) {
          uint8_t cVal = 0;
          for (int32_t k = 7; k >= 0; k--) {
            if (pArithDecoder->IsComplete()) {
              return nullptr;
            }
            int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
            cVal |= bVal << k;
            CONTEXT = ((CONTEXT & 0x01f7) << 1) | bVal;
          }
          pLine[cc] = cVal;
        }

        uint8_t cVal1 = 0;
        for (int32_t k = 0; k < nBitsLeft; k++) {
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          int bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
          cVal1 |= bVal << (7 - k);
          CONTEXT = ((CONTEXT & 0x01f7) << 1) | bVal;
        }
        pLine[nLineBytes] = cVal1;
      }
    }
    pLine += nStride;
  }
  return GBREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeArithTemplate3Unopt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* gbContexts) {
  auto GBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  if (!GBREG->has_data()) {
    return nullptr;
  }

  GBREG->Fill(false);
  int LTP = 0;

  for (uint32_t h = 0; h < GBH; h++) {
    if (TPGDON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&gbContexts[0x0195]);
    }

    if (LTP == 1) {
      GBREG->CopyLine(h, h - 1);
    } else {
      uint32_t line1 = GBREG->GetPixel(1, h - 1);
      line1 |= GBREG->GetPixel(0, h - 1) << 1;
      uint32_t line2 = 0;

      for (uint32_t w = 0; w < GBW; w++) {
        int bVal;
        if (USESKIP && SKIP->GetPixel(w, h)) {
          bVal = 0;
        } else {
          uint32_t CONTEXT = line2;
          CONTEXT |= GBREG->GetPixel(w + GBAT[0], h + GBAT[1]) << 4;
          CONTEXT |= line1 << 5;
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          bVal = pArithDecoder->Decode(&gbContexts[CONTEXT]);
        }
        if (bVal) {
          GBREG->SetPixel(w, h, bVal);
        }
        line1 = ((line1 << 1) | GBREG->GetPixel(w + 2, h - 1)) & 0x1f;
        line2 = ((line2 << 1) | bVal) & 0x0f;
      }
    }
  }
  return GBREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRDProc::DecodeMMR(
    CJBig2_BitStream* pStream) {
  // Validate dimensions
  if (GBW == 0 || GBH == 0 || GBW > 65535 || GBH > 65535) {
    return std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  }

  // Align to byte boundary before MMR data
  pStream->alignByte();

  // Get remaining data from stream
  const uint8_t* src_data = pStream->getPointer();
  size_t src_size = pStream->getByteLeft();

  if (!src_data || src_size == 0) {
    return std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  }

  // Decode using CCITT Group 4 (MMR) algorithm
  // K = -1 means Group 4, no EOL markers, no byte alignment, black_is_1 = true
  std::vector<uint8_t> decoded_data;
  bool success = ccitt::decode_ccitt_fax(
      src_data, src_size,
      static_cast<int>(GBW), static_cast<int>(GBH),
      -1,      // K < 0 = Group 4 (T.6) - pure 2D
      false,   // end_of_line - no EOL markers in JBIG2 MMR
      false,   // encoded_byte_align - no byte alignment between rows
      true,    // black_is_1 - JBIG2 uses 1 for black pixels
      decoded_data);

  if (!success || decoded_data.empty()) {
    // Fallback to empty image
    return std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));
  }

  // Create output image
  auto pImage = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GBW, GBH));

  // Copy decoded data to image
  // CJBig2_Image uses pitch = (width + 31) / 32 * 4 (32-bit aligned rows)
  // CCITT decoder uses pitch = (width + 7) / 8 (byte aligned)
  int ccitt_pitch = (static_cast<int>(GBW) + 7) / 8;
  int jbig2_pitch = pImage->stride();

  for (uint32_t y = 0; y < GBH; ++y) {
    const uint8_t* src_row = decoded_data.data() + y * ccitt_pitch;
    uint8_t* dst_row = pImage->data() + y * jbig2_pitch;

    // Copy the bytes that contain image data
    std::memcpy(dst_row, src_row, ccitt_pitch);

    // Clear any padding bytes in the wider stride
    if (jbig2_pitch > ccitt_pitch) {
      std::memset(dst_row + ccitt_pitch, 0, jbig2_pitch - ccitt_pitch);
    }
  }

  // Advance stream position past the MMR data
  // MMR data ends with EOFB (000000000001 repeated twice) or when all rows decoded
  // For simplicity, consume all remaining bytes
  pStream->setOffset(static_cast<uint32_t>(pStream->getBufSize()));

  return pImage;
}

}  // namespace jbig2
}  // namespace nanopdf
