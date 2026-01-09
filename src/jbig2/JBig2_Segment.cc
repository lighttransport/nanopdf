// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_Segment.hh"

#include "JBig2_Image.hh"
#include "JBig2_SymbolDict.hh"
#include "JBig2_PatternDict.hh"
#include "JBig2_HuffmanTable.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_Segment::CJBig2_Segment() = default;

CJBig2_Segment::~CJBig2_Segment() = default;

}  // namespace jbig2
}  // namespace nanopdf
