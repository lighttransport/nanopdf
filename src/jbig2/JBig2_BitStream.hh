// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_BITSTREAM_HH_
#define NANOPDF_JBIG2_BITSTREAM_HH_

#include <cstdint>
#include <cstddef>

namespace nanopdf {
namespace jbig2 {

class CJBig2_BitStream {
 public:
  CJBig2_BitStream(const uint8_t* pSrcStream, size_t streamSize, uint64_t key);
  CJBig2_BitStream(const CJBig2_BitStream&) = delete;
  CJBig2_BitStream& operator=(const CJBig2_BitStream&) = delete;
  ~CJBig2_BitStream();

  // Read n bits into dwResult/nResult
  // Returns 0 on success, -1 on failure
  int32_t readNBits(uint32_t dwBits, uint32_t* dwResult);
  int32_t readNBits(uint32_t dwBits, int32_t* nResult);
  int32_t read1Bit(uint32_t* dwResult);
  int32_t read1Bit(bool* bResult);
  int32_t read1Byte(uint8_t* cResult);
  int32_t readInteger(uint32_t* dwResult);       // Read 32-bit big-endian
  int32_t readShortInteger(uint16_t* wResult);   // Read 16-bit big-endian

  void alignByte();
  uint8_t getCurByte() const;
  void incByteIdx();
  uint8_t getCurByte_arith() const;
  uint8_t getNextByte_arith() const;
  uint32_t getOffset() const;
  void setOffset(uint32_t dwOffset);
  void addOffset(uint32_t dwDelta);
  uint32_t getBitPos() const;
  void setBitPos(uint32_t dwBitPos);

  const uint8_t* getPointer() const;
  uint32_t getByteLeft() const;
  uint64_t getKey() const { return key_; }
  bool IsInBounds() const;
  size_t getBufSize() const { return size_; }

 private:
  void AdvanceBit();
  uint32_t LengthInBits() const;

  const uint8_t* data_;
  size_t size_;
  uint32_t byte_idx_;  // Must always be <= size_
  uint32_t bit_idx_;   // Must always be in [0..7]
  const uint64_t key_;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_BITSTREAM_HH_
