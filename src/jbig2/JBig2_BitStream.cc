// Copyright 2015 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_BitStream.hh"

#include "JBig2_Define.hh"

#include <algorithm>
#include <limits>

namespace nanopdf {
namespace jbig2 {

CJBig2_BitStream::CJBig2_BitStream(const uint8_t* pSrcStream,
                                   size_t streamSize,
                                   uint64_t key)
    : data_(pSrcStream),
      size_(std::min(streamSize, static_cast<size_t>(kJBig2MaxStreamSize))),
      byte_idx_(0),
      bit_idx_(0),
      key_(key) {}

CJBig2_BitStream::~CJBig2_BitStream() = default;

int32_t CJBig2_BitStream::readNBits(uint32_t dwBits, uint32_t* dwResult) {
  if (!IsInBounds()) {
    return -1;
  }

  uint32_t dwBitPos = getBitPos();
  if (dwBitPos > LengthInBits()) {
    return -1;
  }

  *dwResult = 0;
  if (dwBitPos + dwBits <= LengthInBits()) {
    dwBitPos = dwBits;
  } else {
    dwBitPos = LengthInBits() - dwBitPos;
  }

  for (; dwBitPos > 0; --dwBitPos) {
    *dwResult =
        (*dwResult << 1) | ((data_[byte_idx_] >> (7 - bit_idx_)) & 0x01);
    AdvanceBit();
  }
  return 0;
}

int32_t CJBig2_BitStream::readNBits(uint32_t dwBits, int32_t* nResult) {
  if (!IsInBounds()) {
    return -1;
  }

  uint32_t dwBitPos = getBitPos();
  if (dwBitPos > LengthInBits()) {
    return -1;
  }

  *nResult = 0;
  if (dwBitPos + dwBits <= LengthInBits()) {
    dwBitPos = dwBits;
  } else {
    dwBitPos = LengthInBits() - dwBitPos;
  }

  for (; dwBitPos > 0; --dwBitPos) {
    *nResult = (*nResult << 1) | ((data_[byte_idx_] >> (7 - bit_idx_)) & 0x01);
    AdvanceBit();
  }
  return 0;
}

int32_t CJBig2_BitStream::read1Bit(uint32_t* dwResult) {
  if (!IsInBounds()) {
    return -1;
  }

  *dwResult = (data_[byte_idx_] >> (7 - bit_idx_)) & 0x01;
  AdvanceBit();
  return 0;
}

int32_t CJBig2_BitStream::read1Bit(bool* bResult) {
  if (!IsInBounds()) {
    return -1;
  }

  *bResult = (data_[byte_idx_] >> (7 - bit_idx_)) & 0x01;
  AdvanceBit();
  return 0;
}

int32_t CJBig2_BitStream::read1Byte(uint8_t* cResult) {
  if (!IsInBounds()) {
    return -1;
  }

  *cResult = data_[byte_idx_];
  ++byte_idx_;
  return 0;
}

int32_t CJBig2_BitStream::readInteger(uint32_t* dwResult) {
  if (byte_idx_ + 3 >= size_) {
    return -1;
  }

  *dwResult = (static_cast<uint32_t>(data_[byte_idx_]) << 24) |
              (static_cast<uint32_t>(data_[byte_idx_ + 1]) << 16) |
              (static_cast<uint32_t>(data_[byte_idx_ + 2]) << 8) |
              static_cast<uint32_t>(data_[byte_idx_ + 3]);
  byte_idx_ += 4;
  return 0;
}

int32_t CJBig2_BitStream::readShortInteger(uint16_t* wResult) {
  if (byte_idx_ + 1 >= size_) {
    return -1;
  }

  *wResult = (static_cast<uint16_t>(data_[byte_idx_]) << 8) |
             static_cast<uint16_t>(data_[byte_idx_ + 1]);
  byte_idx_ += 2;
  return 0;
}

void CJBig2_BitStream::alignByte() {
  if (bit_idx_ != 0) {
    addOffset(1);
    bit_idx_ = 0;
  }
}

uint8_t CJBig2_BitStream::getCurByte() const {
  return IsInBounds() ? data_[byte_idx_] : 0;
}

void CJBig2_BitStream::incByteIdx() {
  addOffset(1);
}

uint8_t CJBig2_BitStream::getCurByte_arith() const {
  return IsInBounds() ? data_[byte_idx_] : 0xFF;
}

uint8_t CJBig2_BitStream::getNextByte_arith() const {
  return byte_idx_ + 1 < size_ ? data_[byte_idx_ + 1] : 0xFF;
}

uint32_t CJBig2_BitStream::getOffset() const {
  return byte_idx_;
}

void CJBig2_BitStream::setOffset(uint32_t dwOffset) {
  byte_idx_ = std::min(dwOffset, static_cast<uint32_t>(size_));
}

void CJBig2_BitStream::addOffset(uint32_t dwDelta) {
  // Check for overflow
  if (dwDelta > std::numeric_limits<uint32_t>::max() - byte_idx_) {
    byte_idx_ = static_cast<uint32_t>(size_);
    return;
  }

  uint32_t new_offset = byte_idx_ + dwDelta;
  setOffset(new_offset);
}

uint32_t CJBig2_BitStream::getBitPos() const {
  return (byte_idx_ << 3) + bit_idx_;
}

void CJBig2_BitStream::setBitPos(uint32_t dwBitPos) {
  byte_idx_ = dwBitPos >> 3;
  bit_idx_ = dwBitPos & 7;
}

const uint8_t* CJBig2_BitStream::getPointer() const {
  return data_ + byte_idx_;
}

uint32_t CJBig2_BitStream::getByteLeft() const {
  if (byte_idx_ > size_) {
    return 0;
  }
  return static_cast<uint32_t>(size_ - byte_idx_);
}

void CJBig2_BitStream::AdvanceBit() {
  if (bit_idx_ == 7) {
    ++byte_idx_;
    bit_idx_ = 0;
  } else {
    ++bit_idx_;
  }
}

bool CJBig2_BitStream::IsInBounds() const {
  return byte_idx_ < size_;
}

uint32_t CJBig2_BitStream::LengthInBits() const {
  // Check for overflow: size * 8
  if (size_ > std::numeric_limits<uint32_t>::max() / 8) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(size_) * 8;
}

}  // namespace jbig2
}  // namespace nanopdf
