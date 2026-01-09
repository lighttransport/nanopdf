// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_Image.hh"

#include "JBig2_Define.hh"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

namespace nanopdf {
namespace jbig2 {

namespace {

const int kMaxImagePixels = INT_MAX - 31;
const int kMaxImageBytes = kMaxImagePixels / 8;

// Align to 32-bit boundary
inline int32_t AlignToBoundary32(int32_t value) {
  return (value + 31) & ~31;
}

inline int BitIndexToByte(int index) {
  return index / 8;
}

inline int BitIndexToAlignedByte(int index) {
  return index / 32 * 4;
}

uint32_t DoCompose(JBig2ComposeOp op, uint32_t val1, uint32_t val2) {
  switch (op) {
    case JBIG2_COMPOSE_OR:
      return val1 | val2;
    case JBIG2_COMPOSE_AND:
      return val1 & val2;
    case JBIG2_COMPOSE_XOR:
      return val1 ^ val2;
    case JBIG2_COMPOSE_XNOR:
      return ~(val1 ^ val2);
    case JBIG2_COMPOSE_REPLACE:
      return val1;
  }
  return val1;  // Default to REPLACE
}

uint32_t DoComposeWithMask(JBig2ComposeOp op,
                           uint32_t val1,
                           uint32_t val2,
                           uint32_t mask) {
  return (val2 & ~mask) | (DoCompose(op, val1, val2) & mask);
}

}  // namespace

CJBig2_Image::CJBig2_Image(int32_t w, int32_t h)
    : data_(nullptr), owns_data_(true), width_(0), height_(0), stride_(0) {
  if (w <= 0 || h <= 0 || w > kMaxImagePixels) {
    return;
  }

  int32_t stride_pixels = AlignToBoundary32(w);
  if (h > kMaxImagePixels / stride_pixels) {
    return;
  }

  width_ = w;
  height_ = h;
  stride_ = stride_pixels / 8;

  // Allocate and zero-initialize
  size_t total_bytes = static_cast<size_t>(stride_) * static_cast<size_t>(height_);
  data_.reset(new (std::nothrow) uint8_t[total_bytes]);
  if (data_) {
    std::memset(data_.get(), 0, total_bytes);
  }
}

CJBig2_Image::CJBig2_Image(int32_t w, int32_t h, int32_t stride, uint8_t* pBuf)
    : data_(nullptr), owns_data_(false), width_(0), height_(0), stride_(0) {
  if (w < 0 || h < 0) {
    return;
  }

  // Stride must be word-aligned
  if (stride < 0 || stride > kMaxImageBytes || stride % 4 != 0) {
    return;
  }

  int32_t stride_pixels = 8 * stride;
  if (stride_pixels < w || h > kMaxImagePixels / stride_pixels) {
    return;
  }

  if (stride > 0 && h > 0 && !pBuf) {
    return;
  }

  width_ = w;
  height_ = h;
  stride_ = stride;

  // External buffer - don't own it
  // Use unique_ptr with custom no-op deleter for external buffer
  data_ = std::unique_ptr<uint8_t[]>(pBuf);
  owns_data_ = false;
}

CJBig2_Image::CJBig2_Image(const CJBig2_Image& other)
    : data_(nullptr),
      owns_data_(true),
      width_(other.width_),
      height_(other.height_),
      stride_(other.stride_) {
  if (!other.data_ || other.width_ <= 0 || other.height_ <= 0) {
    return;
  }

  size_t total_bytes = static_cast<size_t>(stride_) * static_cast<size_t>(height_);
  data_.reset(new (std::nothrow) uint8_t[total_bytes]);
  if (data_) {
    std::memcpy(data_.get(), other.data_.get(), total_bytes);
  }
}

CJBig2_Image::~CJBig2_Image() {
  if (!owns_data_) {
    // Release without deleting
    data_.release();
  }
}

// static
bool CJBig2_Image::IsValidImageSize(int32_t w, int32_t h) {
  return w > 0 && w <= kJBig2MaxImageSize && h > 0 && h <= kJBig2MaxImageSize;
}

int CJBig2_Image::GetPixel(int32_t x, int32_t y) const {
  if (x < 0 || x >= width_) {
    return 0;
  }

  const uint8_t* line = GetLine(y);
  if (!line) {
    return 0;
  }

  int32_t m = BitIndexToByte(x);
  int32_t n = x & 7;
  return (line[m] >> (7 - n)) & 1;
}

void CJBig2_Image::SetPixel(int32_t x, int32_t y, int v) {
  if (x < 0 || x >= width_) {
    return;
  }

  uint8_t* line = GetLine(y);
  if (!line) {
    return;
  }

  int32_t m = BitIndexToByte(x);
  int32_t n = 1 << (7 - (x & 7));
  if (v) {
    line[m] |= n;
  } else {
    line[m] &= ~n;
  }
}

const uint8_t* CJBig2_Image::GetLine(int32_t y) const {
  size_t offset;
  if (!GetLineOffset(y, &offset)) {
    return nullptr;
  }
  return data_.get() + offset;
}

uint8_t* CJBig2_Image::GetLine(int32_t y) {
  size_t offset;
  if (!GetLineOffset(y, &offset)) {
    return nullptr;
  }
  return data_.get() + offset;
}

void CJBig2_Image::CopyLine(int32_t dest_y, int32_t src_y) {
  uint8_t* dest = GetLine(dest_y);
  if (!dest) {
    return;
  }

  const uint8_t* src = GetLine(src_y);
  if (!src) {
    std::memset(dest, 0, stride_);
    return;
  }
  std::memcpy(dest, src, stride_);
}

void CJBig2_Image::Fill(bool v) {
  if (!data_ || width_ <= 0 || height_ <= 0) {
    return;
  }

  size_t total_bytes = static_cast<size_t>(stride_) * static_cast<size_t>(height_);
  std::memset(data_.get(), v ? 0xFF : 0, total_bytes);
}

bool CJBig2_Image::ComposeTo(CJBig2_Image* pDst,
                             int64_t x,
                             int64_t y,
                             JBig2ComposeOp op) {
  return data_ &&
         ComposeToInternal(pDst, x, y, op, FX_RECT(0, 0, width_, height_));
}

bool CJBig2_Image::ComposeToWithRect(CJBig2_Image* pDst,
                                     int64_t x,
                                     int64_t y,
                                     const FX_RECT& rtSrc,
                                     JBig2ComposeOp op) {
  return data_ && ComposeToInternal(pDst, x, y, op, rtSrc);
}

bool CJBig2_Image::ComposeFrom(int64_t x,
                               int64_t y,
                               CJBig2_Image* pSrc,
                               JBig2ComposeOp op) {
  return data_ && pSrc->ComposeTo(this, x, y, op);
}

bool CJBig2_Image::ComposeFromWithRect(int64_t x,
                                       int64_t y,
                                       CJBig2_Image* pSrc,
                                       const FX_RECT& rtSrc,
                                       JBig2ComposeOp op) {
  return data_ && pSrc->ComposeToWithRect(this, x, y, rtSrc, op);
}

std::unique_ptr<CJBig2_Image> CJBig2_Image::SubImage(int32_t x,
                                                     int32_t y,
                                                     int32_t w,
                                                     int32_t h) const {
  if (!IsValidImageSize(w, h)) {
    return nullptr;
  }

  auto image = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(w, h));
  if (!image->has_data()) {
    return nullptr;
  }

  // Use fast path if possible
  if (x % 8 == 0) {
    SubImageFast(x, y, w, h, image.get());
  } else {
    SubImageSlow(x, y, w, h, image.get());
  }

  return image;
}

void CJBig2_Image::Expand(int32_t h, bool v) {
  if (h <= height_ || h > kJBig2MaxImageSize) {
    return;
  }

  // Reallocate with new height
  int32_t new_height = h;
  size_t new_total_bytes =
      static_cast<size_t>(stride_) * static_cast<size_t>(new_height);
  size_t old_total_bytes =
      static_cast<size_t>(stride_) * static_cast<size_t>(height_);

  std::unique_ptr<uint8_t[]> new_data(new (std::nothrow) uint8_t[new_total_bytes]);
  if (!new_data) {
    return;
  }

  // Copy old data
  if (data_) {
    std::memcpy(new_data.get(), data_.get(), old_total_bytes);
  }

  // Fill new area
  std::memset(new_data.get() + old_total_bytes, v ? 0xFF : 0,
              new_total_bytes - old_total_bytes);

  data_ = std::move(new_data);
  height_ = new_height;
  owns_data_ = true;
}

bool CJBig2_Image::GetLineOffset(int32_t y, size_t* offset) const {
  if (!data_ || y < 0 || y >= height_) {
    return false;
  }

  *offset = static_cast<size_t>(y) * static_cast<size_t>(stride_);
  return true;
}

void CJBig2_Image::SubImageFast(int32_t x,
                                int32_t y,
                                int32_t w,
                                int32_t h,
                                CJBig2_Image* image) const {
  int32_t x_byte = x / 8;

  for (int32_t line = 0; line < h; ++line) {
    int32_t src_y = y + line;
    if (src_y < 0 || src_y >= height_) {
      continue;
    }

    const uint8_t* src_line = GetLine(src_y);
    uint8_t* dest_line = image->GetLine(line);

    if (src_line && dest_line) {
      int32_t copy_bytes = std::min((w + 7) / 8, stride_ - x_byte);
      if (copy_bytes > 0) {
        std::memcpy(dest_line, src_line + x_byte, copy_bytes);
      }
    }
  }
}

void CJBig2_Image::SubImageSlow(int32_t x,
                                int32_t y,
                                int32_t w,
                                int32_t h,
                                CJBig2_Image* image) const {
  // Pixel-by-pixel copy for non-byte-aligned x
  for (int32_t dest_y = 0; dest_y < h; ++dest_y) {
    for (int32_t dest_x = 0; dest_x < w; ++dest_x) {
      int pixel = GetPixel(x + dest_x, y + dest_y);
      image->SetPixel(dest_x, dest_y, pixel);
    }
  }
}

bool CJBig2_Image::ComposeToInternal(CJBig2_Image* pDst,
                                     int64_t x_in,
                                     int64_t y_in,
                                     JBig2ComposeOp op,
                                     const FX_RECT& rtSrc) {
  if (!pDst || !pDst->data_ || rtSrc.IsEmpty()) {
    return false;
  }

  // Simplified composition - full implementation would handle all edge cases
  int32_t src_left = std::max(rtSrc.left, 0);
  int32_t src_top = std::max(rtSrc.top, 0);
  int32_t src_right = std::min(rtSrc.right, width_);
  int32_t src_bottom = std::min(rtSrc.bottom, height_);

  int64_t dest_left = x_in + src_left - rtSrc.left;
  int64_t dest_top = y_in + src_top - rtSrc.top;

  // Clip to destination bounds
  if (dest_left < 0) {
    src_left -= static_cast<int32_t>(dest_left);
    dest_left = 0;
  }
  if (dest_top < 0) {
    src_top -= static_cast<int32_t>(dest_top);
    dest_top = 0;
  }

  int32_t copy_width = src_right - src_left;
  int32_t copy_height = src_bottom - src_top;

  if (dest_left + copy_width > pDst->width_) {
    copy_width = pDst->width_ - static_cast<int32_t>(dest_left);
  }
  if (dest_top + copy_height > pDst->height_) {
    copy_height = pDst->height_ - static_cast<int32_t>(dest_top);
  }

  if (copy_width <= 0 || copy_height <= 0) {
    return true;  // Nothing to copy
  }

  // Pixel-by-pixel composition (slow but correct)
  for (int32_t row = 0; row < copy_height; ++row) {
    for (int32_t col = 0; col < copy_width; ++col) {
      int src_pixel = GetPixel(src_left + col, src_top + row);
      int dest_pixel = pDst->GetPixel(static_cast<int32_t>(dest_left + col),
                                      static_cast<int32_t>(dest_top + row));

      int result;
      switch (op) {
        case JBIG2_COMPOSE_OR:
          result = src_pixel | dest_pixel;
          break;
        case JBIG2_COMPOSE_AND:
          result = src_pixel & dest_pixel;
          break;
        case JBIG2_COMPOSE_XOR:
          result = src_pixel ^ dest_pixel;
          break;
        case JBIG2_COMPOSE_XNOR:
          result = !(src_pixel ^ dest_pixel);
          break;
        case JBIG2_COMPOSE_REPLACE:
        default:
          result = src_pixel;
          break;
      }

      pDst->SetPixel(static_cast<int32_t>(dest_left + col),
                     static_cast<int32_t>(dest_top + row),
                     result);
    }
  }

  return true;
}

}  // namespace jbig2
}  // namespace nanopdf
