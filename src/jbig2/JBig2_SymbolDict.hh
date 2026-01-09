// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_SYMBOLDICT_HH_
#define NANOPDF_JBIG2_SYMBOLDICT_HH_

#include <memory>
#include <vector>

namespace nanopdf {
namespace jbig2 {

class CJBig2_Image;

class CJBig2_SymbolDict {
 public:
  CJBig2_SymbolDict();
  ~CJBig2_SymbolDict();

  std::unique_ptr<CJBig2_SymbolDict> DeepCopy() const;

  // Move images into this symbol dictionary
  void AddImage(std::unique_ptr<CJBig2_Image> image);

  size_t NumImages() const;
  CJBig2_Image* GetImage(size_t index) const;

 private:
  std::vector<std::unique_ptr<CJBig2_Image>> images_;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_SYMBOLDICT_HH_
