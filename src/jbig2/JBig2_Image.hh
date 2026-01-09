// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_IMAGE_HH_
#define NANOPDF_JBIG2_IMAGE_HH_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace nanopdf {
namespace jbig2 {

// Simple rectangle structure
struct FX_RECT {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;

  FX_RECT() : left(0), top(0), right(0), bottom(0) {}
  FX_RECT(int32_t l, int32_t t, int32_t r, int32_t b)
      : left(l), top(t), right(r), bottom(b) {}

  int32_t Width() const { return right - left; }
  int32_t Height() const { return bottom - top; }
  bool IsEmpty() const { return Width() <= 0 || Height() <= 0; }
};

enum JBig2ComposeOp {
  JBIG2_COMPOSE_OR = 0,
  JBIG2_COMPOSE_AND = 1,
  JBIG2_COMPOSE_XOR = 2,
  JBIG2_COMPOSE_XNOR = 3,
  JBIG2_COMPOSE_REPLACE = 4
};

class CJBig2_Image {
 public:
  CJBig2_Image(int32_t w, int32_t h);
  CJBig2_Image(int32_t w, int32_t h, int32_t stride, uint8_t* pBuf);
  CJBig2_Image(const CJBig2_Image& other);
  ~CJBig2_Image();

  static bool IsValidImageSize(int32_t w, int32_t h);

  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  int32_t stride() const { return stride_; }

  bool has_data() const { return data_ != nullptr; }
  uint8_t* data() const { return data_.get(); }

  int GetPixel(int32_t x, int32_t y) const;
  void SetPixel(int32_t x, int32_t y, int v);

  // Get pointer to line y (returns nullptr if out of bounds)
  const uint8_t* GetLine(int32_t y) const;
  uint8_t* GetLine(int32_t y);

  void CopyLine(int32_t dest_y, int32_t src_y);
  void Fill(bool v);

  bool ComposeFrom(int64_t x, int64_t y, CJBig2_Image* pSrc, JBig2ComposeOp op);
  bool ComposeFromWithRect(int64_t x,
                           int64_t y,
                           CJBig2_Image* pSrc,
                           const FX_RECT& rtSrc,
                           JBig2ComposeOp op);

  std::unique_ptr<CJBig2_Image> SubImage(int32_t x,
                                         int32_t y,
                                         int32_t w,
                                         int32_t h) const;
  void Expand(int32_t h, bool v);

  bool ComposeTo(CJBig2_Image* pDst, int64_t x, int64_t y, JBig2ComposeOp op);
  bool ComposeToWithRect(CJBig2_Image* pDst,
                         int64_t x,
                         int64_t y,
                         const FX_RECT& rtSrc,
                         JBig2ComposeOp op);

 private:
  bool GetLineOffset(int32_t y, size_t* offset) const;

  void SubImageFast(int32_t x,
                    int32_t y,
                    int32_t w,
                    int32_t h,
                    CJBig2_Image* image) const;
  void SubImageSlow(int32_t x,
                    int32_t y,
                    int32_t w,
                    int32_t h,
                    CJBig2_Image* image) const;
  bool ComposeToInternal(CJBig2_Image* pDst,
                         int64_t x_in,
                         int64_t y_in,
                         JBig2ComposeOp op,
                         const FX_RECT& rtSrc);

  std::unique_ptr<uint8_t[]> data_;
  bool owns_data_;  // True if we own the data, false if external buffer
  int32_t width_;   // 1-bit pixels
  int32_t height_;  // lines
  int32_t stride_;  // bytes, must be multiple of 4
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_IMAGE_HH_
