// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_PATTERNDICT_HH_
#define NANOPDF_JBIG2_PATTERNDICT_HH_

#include <memory>
#include <vector>

namespace nanopdf {
namespace jbig2 {

class CJBig2_Image;

class CJBig2_PatternDict {
 public:
  CJBig2_PatternDict();
  ~CJBig2_PatternDict();

  void AddPattern(std::unique_ptr<CJBig2_Image> pattern);
  size_t NumPatterns() const;
  CJBig2_Image* GetPattern(size_t index) const;

  uint32_t HDPW() const { return HDPW_; }
  uint32_t HDPH() const { return HDPH_; }
  void SetDimensions(uint32_t HDPW, uint32_t HDPH) {
    HDPW_ = HDPW;
    HDPH_ = HDPH;
  }

 private:
  std::vector<std::unique_ptr<CJBig2_Image>> patterns_;
  uint32_t HDPW_{0};  // Pattern width
  uint32_t HDPH_{0};  // Pattern height
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_PATTERNDICT_HH_
