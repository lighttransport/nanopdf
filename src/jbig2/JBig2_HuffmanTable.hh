// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_HUFFMANTABLE_HH_
#define NANOPDF_JBIG2_HUFFMANTABLE_HH_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "JBig2_Define.hh"

namespace nanopdf {
namespace jbig2 {

// Structure for defining standard Huffman table entries
struct JBig2TableLine {
  int16_t PREFLEN;   // Prefix length
  int16_t RANGELEN;  // Range length (bits)
  int32_t RANGELOW;  // Range low value
};

class CJBig2_HuffmanTable {
 public:
  CJBig2_HuffmanTable();
  explicit CJBig2_HuffmanTable(size_t idx);
  ~CJBig2_HuffmanTable();

  // Initialize from raw table data (legacy interface)
  bool InitTable(const std::vector<JBig2HuffmanCode>& LENCOUNT,
                 const std::vector<JBig2HuffmanCode>& FIRSTCODE,
                 const std::vector<int32_t>& PREFIXLENGTH,
                 const std::vector<int32_t>& VALUES,
                 size_t NTEMP);

  // Initialize from standard table line entries
  // lines: Array of table lines
  // nLines: Number of lines in array
  // hasOOB: Whether the table includes an OOB entry
  bool InitStandard(const JBig2TableLine* lines, size_t nLines, bool hasOOB);

  // Initialize from encoded user-defined table in bitstream
  bool InitEncoded(class CJBig2_BitStream* pStream);

  // Decode a value from the bitstream
  // Returns 0 on success, -1 on error
  int Decode(CJBig2_BitStream* pStream, int32_t* nResult);

  // Decode with OOB support
  // Returns 0 on success with regular value
  // Returns kJBig2OOB if out-of-band decoded
  // Returns -1 on error
  int DecodeOOB(CJBig2_BitStream* pStream, int32_t* nResult);

  bool IsOK() const { return is_ok_; }
  bool HasOOB() const { return has_oob_; }

 private:
  size_t idx_;
  bool is_ok_{false};
  bool has_oob_{false};
  size_t oob_index_{0};  // Index of OOB entry in table
  std::vector<int> CODES_;
  std::vector<int> PREFLEN_;
  std::vector<int> RANGELEN_;
  std::vector<int> RANGELOW_;
};

// Standard JBIG2 Huffman tables (Table B.1 - B.15 from spec)
extern const JBig2TableLine kHuffmanTable_B1[];
extern const size_t kHuffmanTable_B1_Size;
extern const JBig2TableLine kHuffmanTable_B2[];
extern const size_t kHuffmanTable_B2_Size;
extern const JBig2TableLine kHuffmanTable_B3[];
extern const size_t kHuffmanTable_B3_Size;
extern const JBig2TableLine kHuffmanTable_B4[];
extern const size_t kHuffmanTable_B4_Size;
extern const JBig2TableLine kHuffmanTable_B5[];
extern const size_t kHuffmanTable_B5_Size;
extern const JBig2TableLine kHuffmanTable_B6[];
extern const size_t kHuffmanTable_B6_Size;
extern const JBig2TableLine kHuffmanTable_B7[];
extern const size_t kHuffmanTable_B7_Size;
extern const JBig2TableLine kHuffmanTable_B8[];
extern const size_t kHuffmanTable_B8_Size;
extern const JBig2TableLine kHuffmanTable_B9[];
extern const size_t kHuffmanTable_B9_Size;
extern const JBig2TableLine kHuffmanTable_B10[];
extern const size_t kHuffmanTable_B10_Size;
extern const JBig2TableLine kHuffmanTable_B11[];
extern const size_t kHuffmanTable_B11_Size;
extern const JBig2TableLine kHuffmanTable_B12[];
extern const size_t kHuffmanTable_B12_Size;
extern const JBig2TableLine kHuffmanTable_B13[];
extern const size_t kHuffmanTable_B13_Size;
extern const JBig2TableLine kHuffmanTable_B14[];
extern const size_t kHuffmanTable_B14_Size;
extern const JBig2TableLine kHuffmanTable_B15[];
extern const size_t kHuffmanTable_B15_Size;

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_HUFFMANTABLE_HH_
