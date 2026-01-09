// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_HUFFMANTABLE_HH_
#define NANOPDF_JBIG2_HUFFMANTABLE_HH_

#include <cstdint>
#include <vector>

#include "JBig2_Define.hh"

namespace nanopdf {
namespace jbig2 {

class CJBig2_HuffmanTable {
 public:
  CJBig2_HuffmanTable();
  explicit CJBig2_HuffmanTable(size_t idx);
  ~CJBig2_HuffmanTable();

  bool InitTable(const std::vector<JBig2HuffmanCode>& LENCOUNT,
                 const std::vector<JBig2HuffmanCode>& FIRSTCODE,
                 const std::vector<int32_t>& PREFIXLENGTH,
                 const std::vector<int32_t>& VALUES,
                 size_t NTEMP);

  int Decode(class CJBig2_BitStream* pStream, int32_t* nResult);
  int DecodeOOB(CJBig2_BitStream* pStream, int32_t* nResult);

 private:
  size_t idx_;
  std::vector<int> CODES_;
  std::vector<int> PREFLEN_;
  std::vector<int> RANGELEN_;
  std::vector<int> RANGELOW_;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_HUFFMANTABLE_HH_
