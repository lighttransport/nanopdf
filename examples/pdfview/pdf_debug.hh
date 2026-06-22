// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pdf_debug — map a page region to the underlying PDF objects (text runs with
// their font + object number, image XObjects with placement, annotations) and
// serialize arbitrary PDF objects. Powers the interactive debug overlay and the
// MCP debug tools for LLM/VLM-assisted inspection.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <lightvg/types.h>
}

#include "text-geometry.hh"  // nanopdf::TextQuad

namespace pdfview {

class PdfDocument;

struct RegionHit {
  enum Kind { Text, Image, Annotation };
  Kind kind = Text;
  std::string label;          // one-line summary
  std::string detail;         // longer description (font subtype, dims, ...)
  int obj_num = -1;           // PDF object number, or -1 if not resolvable
  int gen_num = 0;
  nanopdf::TextQuad rect;      // bounding box in PDF user space (y-up, bottom-left)
};

// Objects whose content/placement intersects the page-space rect (x1,y1)-(x2,y2).
std::vector<RegionHit> inspect_region(PdfDocument& doc, int page_index,
                                      double x1, double y1, double x2, double y2);

// Pretty-print an arbitrary PDF object (depth-limited, values truncated).
std::string dump_object(PdfDocument& doc, uint32_t obj_num, uint16_t gen_num);

// Page-space rects where object @obj_num is drawn (image XObject placements).
std::vector<RegionHit> find_object_placements(PdfDocument& doc, int page_index,
                                              uint32_t obj_num);

}  // namespace pdfview
