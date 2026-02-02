// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_ArithIntDecoder.hh"

#include <cstddef>
#include <limits>

namespace nanopdf {
namespace jbig2 {

namespace {

inline int ShiftOr(int val, int bitwise_or_val) {
  return (val << 1) | bitwise_or_val;
}

struct ArithIntDecodeData {
  int nNeedBits;
  int nValue;
};

// Decoding table from JBIG2 specification Annex A
const ArithIntDecodeData kArithIntDecodeData[6] = {
    {2, 0},
    {4, 4},
    {6, 20},
    {8, 84},
    {12, 340},
    {32, 4436},
};

const size_t kArithIntDecodeDataSize =
    sizeof(kArithIntDecodeData) / sizeof(kArithIntDecodeData[0]);

size_t RecursiveDecode(CJBig2_ArithDecoder* decoder,
                       std::vector<JBig2ArithCtx>* context,
                       int* prev,
                       size_t depth) {
  static const size_t kDepthEnd = kArithIntDecodeDataSize - 1;
  if (depth == kDepthEnd) {
    return kDepthEnd;
  }

  JBig2ArithCtx* pCX = &(*context)[*prev];
  int D = decoder->Decode(pCX);
  *prev = ShiftOr(*prev, D);
  if (!D) {
    return depth;
  }
  return RecursiveDecode(decoder, context, prev, depth + 1);
}

}  // namespace

CJBig2_ArithIntDecoder::CJBig2_ArithIntDecoder() {
  iax_.resize(512);
}

CJBig2_ArithIntDecoder::~CJBig2_ArithIntDecoder() = default;

bool CJBig2_ArithIntDecoder::Decode(CJBig2_ArithDecoder* pArithDecoder,
                                    int* nResult) {
  // This decoding algorithm is explained in "Annex A - Arithmetic Integer
  // Decoding Procedure" on page 113 of the JBIG2 specification (ISO/IEC FCD
  // 14492).
  int PREV = 1;
  const int S = pArithDecoder->Decode(&iax_[PREV]);
  PREV = ShiftOr(PREV, S);

  const size_t nDecodeDataIndex =
      RecursiveDecode(pArithDecoder, &iax_, &PREV, 0);

  int nTemp = 0;
  for (int i = 0; i < kArithIntDecodeData[nDecodeDataIndex].nNeedBits; ++i) {
    int D = pArithDecoder->Decode(&iax_[PREV]);
    PREV = ShiftOr(PREV, D);
    if (PREV >= 256) {
      PREV = (PREV & 511) | 256;
    }
    nTemp = ShiftOr(nTemp, D);
  }

  // Manual overflow checking (replaces FX_SAFE_INT32)
  int base_value = kArithIntDecodeData[nDecodeDataIndex].nValue;

  // Check if addition would overflow
  if (nTemp > 0 && base_value > std::numeric_limits<int>::max() - nTemp) {
    *nResult = 0;
    return false;
  }
  if (nTemp < 0 && base_value < std::numeric_limits<int>::min() - nTemp) {
    *nResult = 0;
    return false;
  }

  int nValue = base_value + nTemp;

  if (S == 1 && nValue > 0) {
    nValue = -nValue;
  }

  *nResult = nValue;
  return S != 1 || nValue != 0;
}

CJBig2_ArithIaidDecoder::CJBig2_ArithIaidDecoder(unsigned char SBSYMCODELENA)
    : SBSYMCODELEN(SBSYMCODELENA) {
  iaid_.resize(static_cast<size_t>(1) << SBSYMCODELEN);
}

CJBig2_ArithIaidDecoder::~CJBig2_ArithIaidDecoder() = default;

void CJBig2_ArithIaidDecoder::Decode(CJBig2_ArithDecoder* pArithDecoder,
                                     uint32_t* nResult) {
  int PREV = 1;
  for (unsigned char i = 0; i < SBSYMCODELEN; ++i) {
    JBig2ArithCtx* pCX = &iaid_[PREV];
    int D = pArithDecoder->Decode(pCX);
    PREV = ShiftOr(PREV, D);
  }
  *nResult = PREV - (1 << SBSYMCODELEN);
}

}  // namespace jbig2
}  // namespace nanopdf
