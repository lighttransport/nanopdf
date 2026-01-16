// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_CONTEXT_HH_
#define NANOPDF_JBIG2_CONTEXT_HH_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <list>
#include <map>

#include "JBig2_Segment.hh"

namespace nanopdf {
namespace jbig2 {

class CJBig2_ArithDecoder;
class CJBig2_BitStream;
class CJBig2_Image;
class CJBig2_PatternDict;
class CJBig2_SymbolDict;
class JBig2ArithCtx;

// JBIG2 segment types
enum JBig2SegmentType {
  JBIG2_SYMBOL_DICT = 0,
  JBIG2_INTERMEDIATE_TEXT_REGION = 4,
  JBIG2_IMMEDIATE_TEXT_REGION = 6,
  JBIG2_IMMEDIATE_LOSSLESS_TEXT_REGION = 7,
  JBIG2_PATTERN_DICT = 16,
  JBIG2_INTERMEDIATE_HALFTONE_REGION = 20,
  JBIG2_IMMEDIATE_HALFTONE_REGION = 22,
  JBIG2_IMMEDIATE_LOSSLESS_HALFTONE_REGION = 23,
  JBIG2_INTERMEDIATE_GENERIC_REGION = 36,
  JBIG2_IMMEDIATE_GENERIC_REGION = 38,
  JBIG2_IMMEDIATE_LOSSLESS_GENERIC_REGION = 39,
  JBIG2_INTERMEDIATE_GENERIC_REFINEMENT_REGION = 40,
  JBIG2_IMMEDIATE_GENERIC_REFINEMENT_REGION = 42,
  JBIG2_IMMEDIATE_LOSSLESS_GENERIC_REFINEMENT_REGION = 43,
  JBIG2_PAGE_INFO = 48,
  JBIG2_END_OF_PAGE = 49,
  JBIG2_END_OF_STRIPE = 50,
  JBIG2_END_OF_FILE = 51,
  JBIG2_PROFILES = 52,
  JBIG2_TABLES = 53,
  JBIG2_EXTENSION = 62
};

// JBIG2 decoding status
enum JBig2Status {
  JBIG2_SUCCESS = 0,
  JBIG2_ERROR_TOO_SHORT = 1,
  JBIG2_ERROR_FATAL = 2,
  JBIG2_END_OF_PAGE_REACHED = 3,
  JBIG2_END_OF_FILE_REACHED = 4,
  JBIG2_ERROR_LIMIT = 5
};

// Decode result
struct JBig2DecodeResult {
  bool success{false};
  std::string error;
  std::unique_ptr<CJBig2_Image> image;
  uint32_t width{0};
  uint32_t height{0};
};

// Main JBIG2 decoding context
class CJBig2_Context {
 public:
  // Create context for PDF embedded JBIG2
  // globalsData/globalsSize can be null/0 if no globals
  CJBig2_Context(const uint8_t* globalsData, size_t globalsSize,
                 const uint8_t* data, size_t size,
                 bool isGlobal);

  ~CJBig2_Context();

  // Decode the JBIG2 stream and return the page image
  JBig2DecodeResult Decode();

  // Get the current page image (may be partial)
  CJBig2_Image* GetPage() const { return page_.get(); }

 private:
  // Parse all segment headers
  JBig2Status ParseSegmentHeaders();

  // Parse a single segment header
  JBig2Status ParseSegmentHeader(CJBig2_Segment* pSegment);

  // Process segment data
  JBig2Status ProcessSegment(CJBig2_Segment* pSegment);

  // Segment type handlers
  JBig2Status ProcessSymbolDict(CJBig2_Segment* pSegment);
  JBig2Status ProcessTextRegion(CJBig2_Segment* pSegment);
  JBig2Status ProcessPatternDict(CJBig2_Segment* pSegment);
  JBig2Status ProcessHalftoneRegion(CJBig2_Segment* pSegment);
  JBig2Status ProcessGenericRegion(CJBig2_Segment* pSegment);
  JBig2Status ProcessGenericRefinementRegion(CJBig2_Segment* pSegment);
  JBig2Status ProcessPageInfo(CJBig2_Segment* pSegment);
  JBig2Status ProcessEndOfPage(CJBig2_Segment* pSegment);
  JBig2Status ProcessEndOfStripe(CJBig2_Segment* pSegment);
  JBig2Status ProcessTables(CJBig2_Segment* pSegment);

  // Get symbol dictionaries from referred segments
  std::vector<CJBig2_SymbolDict*> GetReferencedSymbolDicts(
      CJBig2_Segment* pSegment);

  // Get all symbols from referred dictionaries
  std::vector<CJBig2_Image*> GetAllSymbols(
      const std::vector<CJBig2_SymbolDict*>& dicts);

  // Find segment by number
  CJBig2_Segment* FindSegment(uint32_t number);

  // Context sizes for different templates
  static int GetGBContextSize(uint8_t templ);
  static int GetGRContextSize(uint8_t templ);

  // Main data stream
  std::unique_ptr<CJBig2_BitStream> stream_;

  // Global data stream (if present)
  std::unique_ptr<CJBig2_BitStream> globals_stream_;

  // Parsed segments (from globals + main)
  std::vector<std::unique_ptr<CJBig2_Segment>> segments_;

  // Global segments (parsed from globals stream)
  std::vector<std::unique_ptr<CJBig2_Segment>> global_segments_;

  // Current page image
  std::unique_ptr<CJBig2_Image> page_;

  // Page info
  uint32_t page_width_{0};
  uint32_t page_height_{0};
  uint32_t page_stripe_height_{0};
  bool page_is_lossless_{false};

  // Arithmetic coding contexts
  std::vector<JBig2ArithCtx> gb_contexts_;  // Generic region
  std::vector<JBig2ArithCtx> gr_contexts_;  // Generic refinement

  // Is this a global context?
  bool is_global_;

  // Current segment index
  size_t current_segment_{0};

  // Error state
  std::string error_;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_CONTEXT_HH_
