// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_SymbolDict.hh"

#include "JBig2_Image.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_SymbolDict::CJBig2_SymbolDict() = default;

CJBig2_SymbolDict::~CJBig2_SymbolDict() = default;

std::unique_ptr<CJBig2_SymbolDict> CJBig2_SymbolDict::DeepCopy() const {
  auto new_dict = std::unique_ptr<CJBig2_SymbolDict>(new CJBig2_SymbolDict());
  for (const auto& image : images_) {
    if (image) {
      new_dict->AddImage(std::unique_ptr<CJBig2_Image>(
          new CJBig2_Image(*image)));
    }
  }
  return new_dict;
}

void CJBig2_SymbolDict::AddImage(std::unique_ptr<CJBig2_Image> image) {
  images_.push_back(std::move(image));
}

size_t CJBig2_SymbolDict::NumImages() const {
  return images_.size();
}

CJBig2_Image* CJBig2_SymbolDict::GetImage(size_t index) const {
  return index < images_.size() ? images_[index].get() : nullptr;
}

}  // namespace jbig2
}  // namespace nanopdf
