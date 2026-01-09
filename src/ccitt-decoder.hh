// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
//      copyright notice, this list of conditions and the following disclaimer
//      in the documentation and/or other materials provided with the
//      distribution.
//    * Neither the name of Google Inc. nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Ported to nanopdf from PDFium's core/fxcodec/fax/faxmodule.cpp
// https://pdfium.googlesource.com/pdfium/

#ifndef NANOPDF_CCITT_DECODER_HH_
#define NANOPDF_CCITT_DECODER_HH_

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

namespace nanopdf {
namespace ccitt {

// Limit of image dimension. Use the same limit as the JBIG2 codecs.
static constexpr int kFaxMaxImageDimension = 65535;

// Lookup table for finding leading 1 bit position
static const uint8_t kOneLeadPos[256] = {
    8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Black run instruction table (Huffman codes for black runs)
static const uint8_t kFaxBlackRunIns[] = {
    0,          2,          0x02,       3,          0,          0x03,
    2,          0,          2,          0x02,       1,          0,
    0x03,       4,          0,          2,          0x02,       6,
    0,          0x03,       5,          0,          1,          0x03,
    7,          0,          2,          0x04,       9,          0,
    0x05,       8,          0,          3,          0x04,       10,
    0,          0x05,       11,         0,          0x07,       12,
    0,          2,          0x04,       13,         0,          0x07,
    14,         0,          1,          0x18,       15,         0,
    5,          0x08,       18,         0,          0x0f,       64,
    0,          0x17,       16,         0,          0x18,       17,
    0,          0x37,       0,          0,          10,         0x08,
    0x00,       0x07,       0x0c,       0x40,       0x07,       0x0d,
    0x80,       0x07,       0x17,       24,         0,          0x18,
    25,         0,          0x28,       23,         0,          0x37,
    22,         0,          0x67,       19,         0,          0x68,
    20,         0,          0x6c,       21,         0,          54,
    0x12,       1984 % 256, 1984 / 256, 0x13,       2048 % 256, 2048 / 256,
    0x14,       2112 % 256, 2112 / 256, 0x15,       2176 % 256, 2176 / 256,
    0x16,       2240 % 256, 2240 / 256, 0x17,       2304 % 256, 2304 / 256,
    0x1c,       2368 % 256, 2368 / 256, 0x1d,       2432 % 256, 2432 / 256,
    0x1e,       2496 % 256, 2496 / 256, 0x1f,       2560 % 256, 2560 / 256,
    0x24,       52,         0,          0x27,       55,         0,
    0x28,       56,         0,          0x2b,       59,         0,
    0x2c,       60,         0,          0x33,       320 % 256,  320 / 256,
    0x34,       384 % 256,  384 / 256,  0x35,       448 % 256,  448 / 256,
    0x37,       53,         0,          0x38,       54,         0,
    0x52,       50,         0,          0x53,       51,         0,
    0x54,       44,         0,          0x55,       45,         0,
    0x56,       46,         0,          0x57,       47,         0,
    0x58,       57,         0,          0x59,       58,         0,
    0x5a,       61,         0,          0x5b,       256 % 256,  256 / 256,
    0x64,       48,         0,          0x65,       49,         0,
    0x66,       62,         0,          0x67,       63,         0,
    0x68,       30,         0,          0x69,       31,         0,
    0x6a,       32,         0,          0x6b,       33,         0,
    0x6c,       40,         0,          0x6d,       41,         0,
    0xc8,       128,        0,          0xc9,       192,        0,
    0xca,       26,         0,          0xcb,       27,         0,
    0xcc,       28,         0,          0xcd,       29,         0,
    0xd2,       34,         0,          0xd3,       35,         0,
    0xd4,       36,         0,          0xd5,       37,         0,
    0xd6,       38,         0,          0xd7,       39,         0,
    0xda,       42,         0,          0xdb,       43,         0,
    20,         0x4a,       640 % 256,  640 / 256,  0x4b,       704 % 256,
    704 / 256,  0x4c,       768 % 256,  768 / 256,  0x4d,       832 % 256,
    832 / 256,  0x52,       1280 % 256, 1280 / 256, 0x53,       1344 % 256,
    1344 / 256, 0x54,       1408 % 256, 1408 / 256, 0x55,       1472 % 256,
    1472 / 256, 0x5a,       1536 % 256, 1536 / 256, 0x5b,       1600 % 256,
    1600 / 256, 0x64,       1664 % 256, 1664 / 256, 0x65,       1728 % 256,
    1728 / 256, 0x6c,       512 % 256,  512 / 256,  0x6d,       576 % 256,
    576 / 256,  0x72,       896 % 256,  896 / 256,  0x73,       960 % 256,
    960 / 256,  0x74,       1024 % 256, 1024 / 256, 0x75,       1088 % 256,
    1088 / 256, 0x76,       1152 % 256, 1152 / 256, 0x77,       1216 % 256,
    1216 / 256, 0xff};

// White run instruction table (Huffman codes for white runs)
static const uint8_t kFaxWhiteRunIns[] = {
    0,          0,          0,          6,          0x07,       2,
    0,          0x08,       3,          0,          0x0B,       4,
    0,          0x0C,       5,          0,          0x0E,       6,
    0,          0x0F,       7,          0,          6,          0x07,
    10,         0,          0x08,       11,         0,          0x12,
    128,        0,          0x13,       8,          0,          0x14,
    9,          0,          0x1b,       64,         0,          9,
    0x03,       13,         0,          0x07,       1,          0,
    0x08,       12,         0,          0x17,       192,        0,
    0x18,       1664 % 256, 1664 / 256, 0x2a,       16,         0,
    0x2B,       17,         0,          0x34,       14,         0,
    0x35,       15,         0,          12,         0x03,       22,
    0,          0x04,       23,         0,          0x08,       20,
    0,          0x0c,       19,         0,          0x13,       26,
    0,          0x17,       21,         0,          0x18,       28,
    0,          0x24,       27,         0,          0x27,       18,
    0,          0x28,       24,         0,          0x2B,       25,
    0,          0x37,       256 % 256,  256 / 256,  42,         0x02,
    29,         0,          0x03,       30,         0,          0x04,
    45,         0,          0x05,       46,         0,          0x0a,
    47,         0,          0x0b,       48,         0,          0x12,
    33,         0,          0x13,       34,         0,          0x14,
    35,         0,          0x15,       36,         0,          0x16,
    37,         0,          0x17,       38,         0,          0x1a,
    31,         0,          0x1b,       32,         0,          0x24,
    53,         0,          0x25,       54,         0,          0x28,
    39,         0,          0x29,       40,         0,          0x2a,
    41,         0,          0x2b,       42,         0,          0x2c,
    43,         0,          0x2d,       44,         0,          0x32,
    61,         0,          0x33,       62,         0,          0x34,
    63,         0,          0x35,       0,          0,          0x36,
    320 % 256,  320 / 256,  0x37,       384 % 256,  384 / 256,  0x4a,
    59,         0,          0x4b,       60,         0,          0x52,
    49,         0,          0x53,       50,         0,          0x54,
    51,         0,          0x55,       52,         0,          0x58,
    55,         0,          0x59,       56,         0,          0x5a,
    57,         0,          0x5b,       58,         0,          0x64,
    448 % 256,  448 / 256,  0x65,       512 % 256,  512 / 256,  0x67,
    640 % 256,  640 / 256,  0x68,       576 % 256,  576 / 256,  16,
    0x98,       1472 % 256, 1472 / 256, 0x99,       1536 % 256, 1536 / 256,
    0x9a,       1600 % 256, 1600 / 256, 0x9b,       1728 % 256, 1728 / 256,
    0xcc,       704 % 256,  704 / 256,  0xcd,       768 % 256,  768 / 256,
    0xd2,       832 % 256,  832 / 256,  0xd3,       896 % 256,  896 / 256,
    0xd4,       960 % 256,  960 / 256,  0xd5,       1024 % 256, 1024 / 256,
    0xd6,       1088 % 256, 1088 / 256, 0xd7,       1152 % 256, 1152 / 256,
    0xd8,       1216 % 256, 1216 / 256, 0xd9,       1280 % 256, 1280 / 256,
    0xda,       1344 % 256, 1344 / 256, 0xdb,       1408 % 256, 1408 / 256,
    0,          3,          0x08,       1792 % 256, 1792 / 256, 0x0c,
    1856 % 256, 1856 / 256, 0x0d,       1920 % 256, 1920 / 256, 10,
    0x12,       1984 % 256, 1984 / 256, 0x13,       2048 % 256, 2048 / 256,
    0x14,       2112 % 256, 2112 / 256, 0x15,       2176 % 256, 2176 / 256,
    0x16,       2240 % 256, 2240 / 256, 0x17,       2304 % 256, 2304 / 256,
    0x1c,       2368 % 256, 2368 / 256, 0x1d,       2432 % 256, 2432 / 256,
    0x1e,       2496 % 256, 2496 / 256, 0x1f,       2560 % 256, 2560 / 256,
    0xff,
};

// Get source buffer bit size
inline uint32_t GetSrcBitSize(const uint8_t* src_buf, size_t src_size) {
  return static_cast<uint32_t>(src_size * 8);
}

// Read next bit from source buffer
inline bool NextBit(const uint8_t* src_buf, size_t src_size, uint32_t* bitpos) {
  if (*bitpos / 8 >= src_size) return false;
  uint32_t pos = (*bitpos)++;
  return !!(src_buf[pos / 8] & (1 << (7 - pos % 8)));
}

// Find bit position in buffer
inline int FindBit(const uint8_t* data_buf, size_t data_size,
                   int max_pos, int start_pos, bool bit) {
  if (start_pos < 0) start_pos = 0;
  if (start_pos >= max_pos) return max_pos;

  const uint8_t bit_xor = bit ? 0x00 : 0xff;
  int bit_offset = start_pos % 8;
  if (bit_offset) {
    int byte_pos = start_pos / 8;
    if (static_cast<size_t>(byte_pos) >= data_size) return max_pos;
    uint8_t data = (data_buf[byte_pos] ^ bit_xor) & (0xff >> bit_offset);
    if (data) {
      int result = byte_pos * 8 + kOneLeadPos[data];
      return result < max_pos ? result : max_pos;
    }
    start_pos += 7;
  }

  int max_byte = (max_pos + 7) / 8;
  int byte_pos = start_pos / 8;

  // Try reading in bigger chunks for long runs
  static constexpr int kBulkReadSize = 8;
  if (max_byte >= kBulkReadSize && byte_pos < max_byte - kBulkReadSize) {
    static constexpr uint8_t skip_block_0[kBulkReadSize] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t skip_block_1[kBulkReadSize] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t* skip_block = bit ? skip_block_0 : skip_block_1;
    while (byte_pos < max_byte - kBulkReadSize &&
           static_cast<size_t>(byte_pos + kBulkReadSize) <= data_size &&
           std::memcmp(data_buf + byte_pos, skip_block, kBulkReadSize) == 0) {
      byte_pos += kBulkReadSize;
    }
  }

  while (byte_pos < max_byte && static_cast<size_t>(byte_pos) < data_size) {
    uint8_t data = data_buf[byte_pos] ^ bit_xor;
    if (data) {
      int result = byte_pos * 8 + kOneLeadPos[data];
      return result < max_pos ? result : max_pos;
    }
    ++byte_pos;
  }
  return max_pos;
}

// Find b1 and b2 positions for Group 4 2D decoding
inline void FaxG4FindB1B2(const uint8_t* ref_buf, size_t ref_size,
                          int columns, int a0, bool a0color,
                          int* b1, int* b2) {
  bool first_bit = (a0 < 0) ? true :
      (static_cast<size_t>(a0 / 8) < ref_size &&
       (ref_buf[a0 / 8] & (1 << (7 - a0 % 8))) != 0);
  *b1 = FindBit(ref_buf, ref_size, columns, a0 + 1, !first_bit);
  if (*b1 >= columns) {
    *b1 = *b2 = columns;
    return;
  }
  if (first_bit == !a0color) {
    *b1 = FindBit(ref_buf, ref_size, columns, *b1 + 1, first_bit);
    first_bit = !first_bit;
  }
  if (*b1 >= columns) {
    *b1 = *b2 = columns;
    return;
  }
  *b2 = FindBit(ref_buf, ref_size, columns, *b1 + 1, first_bit);
}

// Fill bits in destination buffer (set to black)
inline void FaxFillBits(uint8_t* dest_buf, size_t dest_size,
                        int columns, int startpos, int endpos) {
  startpos = std::max(startpos, 0);
  endpos = std::min(std::max(endpos, 0), columns);
  if (startpos >= endpos) return;

  uint32_t first_byte = startpos / 8;
  uint32_t last_byte = (endpos - 1) / 8;

  if (first_byte >= dest_size) return;
  if (last_byte >= dest_size) last_byte = static_cast<uint32_t>(dest_size - 1);

  if (first_byte == last_byte) {
    for (int i = startpos % 8; i <= (endpos - 1) % 8; ++i) {
      dest_buf[first_byte] -= 1 << (7 - i);
    }
    return;
  }

  for (int i = startpos % 8; i < 8; ++i) {
    dest_buf[first_byte] -= 1 << (7 - i);
  }
  for (int i = 0; i <= (endpos - 1) % 8; ++i) {
    dest_buf[last_byte] -= 1 << (7 - i);
  }
  if (last_byte > first_byte + 1) {
    std::memset(dest_buf + first_byte + 1, 0, last_byte - first_byte - 1);
  }
}

// Get run length from Huffman table
inline int FaxGetRun(const uint8_t* ins_array,
                     const uint8_t* src_buf, size_t src_size,
                     uint32_t* bitpos) {
  uint32_t bitsize = GetSrcBitSize(src_buf, src_size);
  uint32_t code = 0;
  int ins_off = 0;
  while (true) {
    uint8_t ins = ins_array[ins_off++];
    if (ins == 0xff) return -1;
    if (*bitpos >= bitsize) return -1;

    code <<= 1;
    if (src_buf[*bitpos / 8] & (1 << (7 - *bitpos % 8))) ++code;
    ++(*bitpos);

    int next_off = ins_off + ins * 3;
    for (; ins_off < next_off; ins_off += 3) {
      if (ins_array[ins_off] == code) {
        return ins_array[ins_off + 1] + ins_array[ins_off + 2] * 256;
      }
    }
  }
}

// Decode one row using Group 4 (T.6) 2D algorithm
inline void FaxG4GetRow(const uint8_t* src_buf, size_t src_size,
                        uint32_t* bitpos,
                        uint8_t* dest_buf, size_t dest_size,
                        const uint8_t* ref_buf, size_t ref_size,
                        int columns) {
  // See TABLE 1/T.6 "Code table" in ITU-T T.6.
  int a0 = -1;
  bool a0color = true;
  uint32_t bitsize = GetSrcBitSize(src_buf, src_size);

  while (true) {
    if (*bitpos >= bitsize) return;

    int a1, a2, b1, b2;
    FaxG4FindB1B2(ref_buf, ref_size, columns, a0, a0color, &b1, &b2);

    int v_delta = 0;
    if (!NextBit(src_buf, src_size, bitpos)) {
      if (*bitpos >= bitsize) return;

      bool bit1 = NextBit(src_buf, src_size, bitpos);
      if (*bitpos >= bitsize) return;

      bool bit2 = NextBit(src_buf, src_size, bitpos);
      if (bit1) {
        // Mode "Vertical", VR(1), VL(1)
        v_delta = bit2 ? 1 : -1;
      } else if (bit2) {
        // Mode "Horizontal"
        int run_len1 = 0;
        while (true) {
          int run = FaxGetRun(a0color ? kFaxWhiteRunIns : kFaxBlackRunIns,
                              src_buf, src_size, bitpos);
          if (run < 0) break;
          run_len1 += run;
          if (run < 64) break;
        }
        if (a0 < 0) ++run_len1;
        if (run_len1 < 0) return;

        a1 = a0 + run_len1;
        if (!a0color) {
          FaxFillBits(dest_buf, dest_size, columns, a0, a1);
        }

        int run_len2 = 0;
        while (true) {
          int run = FaxGetRun(a0color ? kFaxBlackRunIns : kFaxWhiteRunIns,
                              src_buf, src_size, bitpos);
          if (run < 0) break;
          run_len2 += run;
          if (run < 64) break;
        }
        if (run_len2 < 0) return;

        a2 = a1 + run_len2;
        if (a0color) {
          FaxFillBits(dest_buf, dest_size, columns, a1, a2);
        }

        a0 = a2;
        if (a0 < columns) continue;
        return;
      } else {
        if (*bitpos >= bitsize) return;

        if (NextBit(src_buf, src_size, bitpos)) {
          // Mode "Pass"
          if (!a0color) {
            FaxFillBits(dest_buf, dest_size, columns, a0, b2);
          }
          if (b2 >= columns) return;
          a0 = b2;
          continue;
        }

        if (*bitpos >= bitsize) return;

        bool next_bit1 = NextBit(src_buf, src_size, bitpos);
        if (*bitpos >= bitsize) return;

        bool next_bit2 = NextBit(src_buf, src_size, bitpos);
        if (next_bit1) {
          // Mode "Vertical", VR(2), VL(2)
          v_delta = next_bit2 ? 2 : -2;
        } else if (next_bit2) {
          if (*bitpos >= bitsize) return;
          // Mode "Vertical", VR(3), VL(3)
          v_delta = NextBit(src_buf, src_size, bitpos) ? 3 : -3;
        } else {
          if (*bitpos >= bitsize) return;
          // Extension or EOFB
          if (NextBit(src_buf, src_size, bitpos)) {
            *bitpos += 3;
            continue;
          }
          *bitpos += 5;
          return;
        }
      }
    }
    // else: Mode "Vertical", V(0)

    a1 = b1 + v_delta;
    if (!a0color) {
      FaxFillBits(dest_buf, dest_size, columns, a0, a1);
    }

    if (a1 >= columns) return;

    // The position of picture element must be monotonic increasing.
    if (a0 >= a1) return;

    a0 = a1;
    a0color = !a0color;
  }
}

// Skip EOL marker
inline void FaxSkipEOL(const uint8_t* src_buf, size_t src_size,
                       uint32_t* bitpos) {
  uint32_t bitsize = GetSrcBitSize(src_buf, src_size);
  uint32_t startbit = *bitpos;
  while (*bitpos < bitsize) {
    if (!NextBit(src_buf, src_size, bitpos)) continue;
    if (*bitpos - startbit <= 11) {
      *bitpos = startbit;
    }
    return;
  }
}

// Decode one row using Group 3 1D algorithm
inline void FaxGet1DLine(const uint8_t* src_buf, size_t src_size,
                         uint32_t* bitpos,
                         uint8_t* dest_buf, size_t dest_size,
                         int columns) {
  uint32_t bitsize = GetSrcBitSize(src_buf, src_size);
  bool color = true;
  int startpos = 0;

  while (true) {
    if (*bitpos >= bitsize) return;

    int run_len = 0;
    while (true) {
      int run = FaxGetRun(color ? kFaxWhiteRunIns : kFaxBlackRunIns,
                          src_buf, src_size, bitpos);
      if (run < 0) {
        while (*bitpos < bitsize) {
          if (NextBit(src_buf, src_size, bitpos)) return;
        }
        return;
      }
      run_len += run;
      if (run < 64) break;
    }
    if (!color) {
      FaxFillBits(dest_buf, dest_size, columns, startpos, startpos + run_len);
    }

    startpos += run_len;
    if (startpos >= columns) break;
    color = !color;
  }
}

// Invert buffer (for BlackIs1 parameter)
inline void InvertBuffer(uint8_t* buf, size_t size) {
  size_t i = 0;
  // Process 4 bytes at a time for efficiency
  for (; i + 4 <= size; i += 4) {
    uint32_t* ptr = reinterpret_cast<uint32_t*>(buf + i);
    *ptr = ~(*ptr);
  }
  // Handle remaining bytes
  for (; i < size; ++i) {
    buf[i] = ~buf[i];
  }
}

// Main decoding function
// K parameter: K < 0 = Group 4 (T.6), K = 0 = Group 3 1D (T.4),
//              K > 0 = Group 3 2D (T.4)
inline bool decode_ccitt_fax(const uint8_t* src_data, size_t src_size,
                             int width, int height, int K,
                             bool end_of_line, bool encoded_byte_align,
                             bool black_is_1,
                             std::vector<uint8_t>& output,
                             std::string* error = nullptr) {
  // Validate dimensions
  if (width <= 0 || width > kFaxMaxImageDimension ||
      height <= 0 || height > kFaxMaxImageDimension) {
    if (error) *error = "Invalid image dimensions";
    return false;
  }

  int pitch = (width + 7) / 8;
  output.resize(static_cast<size_t>(pitch) * height);

  std::vector<uint8_t> ref_buf(pitch, 0xff);
  std::vector<uint8_t> scanline_buf(pitch);

  uint32_t bitpos = 0;
  bool byte_align = encoded_byte_align;

  for (int row = 0; row < height; ++row) {
    uint32_t bitsize = GetSrcBitSize(src_data, src_size);

    FaxSkipEOL(src_data, src_size, &bitpos);
    if (bitpos >= bitsize) break;

    std::memset(scanline_buf.data(), 0xff, pitch);

    if (K < 0) {
      // Group 4 (T.6) - pure 2D
      FaxG4GetRow(src_data, src_size, &bitpos,
                  scanline_buf.data(), pitch,
                  ref_buf.data(), pitch, width);
      std::memcpy(ref_buf.data(), scanline_buf.data(), pitch);
    } else if (K == 0) {
      // Group 3 1D (T.4)
      FaxGet1DLine(src_data, src_size, &bitpos,
                   scanline_buf.data(), pitch, width);
    } else {
      // Group 3 2D (T.4) - mixed mode
      if (NextBit(src_data, src_size, &bitpos)) {
        FaxGet1DLine(src_data, src_size, &bitpos,
                     scanline_buf.data(), pitch, width);
      } else {
        FaxG4GetRow(src_data, src_size, &bitpos,
                    scanline_buf.data(), pitch,
                    ref_buf.data(), pitch, width);
      }
      std::memcpy(ref_buf.data(), scanline_buf.data(), pitch);
    }

    if (end_of_line) {
      FaxSkipEOL(src_data, src_size, &bitpos);
    }

    // Handle byte alignment
    if (byte_align && bitpos < bitsize) {
      uint32_t bitpos0 = bitpos;
      uint32_t bitpos1 = (bitpos + 7) & ~7u;
      bool should_align = true;
      while (should_align && bitpos0 < bitpos1) {
        int bit = (src_data[bitpos0 / 8] & (1 << (7 - bitpos0 % 8))) ? 1 : 0;
        if (bit != 0) {
          should_align = false;
        } else {
          ++bitpos0;
        }
      }
      if (should_align) {
        bitpos = bitpos1;
      }
    }

    // Invert if BlackIs1
    if (black_is_1) {
      InvertBuffer(scanline_buf.data(), pitch);
    }

    // Copy scanline to output
    std::memcpy(output.data() + static_cast<size_t>(row) * pitch,
                scanline_buf.data(), pitch);
  }

  return true;
}

}  // namespace ccitt
}  // namespace nanopdf

#endif  // NANOPDF_CCITT_DECODER_HH_
