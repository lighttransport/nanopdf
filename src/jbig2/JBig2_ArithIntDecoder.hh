// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_ARITHINTDECODER_HH_
#define NANOPDF_JBIG2_ARITHINTDECODER_HH_

#include <cstdint>
#include <vector>

#include "JBig2_ArithDecoder.hh"

namespace nanopdf {
namespace jbig2 {

// Arithmetic integer decoder for arbitrary integers
class CJBig2_ArithIntDecoder {
 public:
  CJBig2_ArithIntDecoder();
  ~CJBig2_ArithIntDecoder();

  // Returns true on success, and false when an OOB condition occurs.
  // Many callers can tolerate OOB and do not check the return value.
  bool Decode(CJBig2_ArithDecoder* pArithDecoder, int* nResult);

 private:
  std::vector<JBig2ArithCtx> iax_;
};

// Arithmetic IAID (symbol ID) decoder with fixed bit length
class CJBig2_ArithIaidDecoder {
 public:
  explicit CJBig2_ArithIaidDecoder(unsigned char SBSYMCODELENA);
  ~CJBig2_ArithIaidDecoder();

  void Decode(CJBig2_ArithDecoder* pArithDecoder, uint32_t* nResult);

 private:
  std::vector<JBig2ArithCtx> iaid_;
  const unsigned char SBSYMCODELEN;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_ARITHINTDECODER_HH_
