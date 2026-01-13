// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_PatternDict.hh"

#include "JBig2_Image.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_PatternDict::CJBig2_PatternDict() = default;

CJBig2_PatternDict::~CJBig2_PatternDict() = default;

void CJBig2_PatternDict::AddPattern(std::unique_ptr<CJBig2_Image> pattern) {
  patterns_.push_back(std::move(pattern));
}

size_t CJBig2_PatternDict::NumPatterns() const {
  return patterns_.size();
}

CJBig2_Image* CJBig2_PatternDict::GetPattern(size_t index) const {
  return index < patterns_.size() ? patterns_[index].get() : nullptr;
}

}  // namespace jbig2
}  // namespace nanopdf
