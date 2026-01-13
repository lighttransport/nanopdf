// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_ARITHDECODER_HH_
#define NANOPDF_JBIG2_ARITHDECODER_HH_

#include <cstdint>

namespace nanopdf {
namespace jbig2 {

class CJBig2_BitStream;

// Arithmetic coding context for adaptive binary coding
class JBig2ArithCtx {
 public:
  struct JBig2ArithQe {
    uint16_t Qe;       // Probability estimate
    uint8_t NMPS;      // Next index for MPS (More Probable Symbol)
    uint8_t NLPS;      // Next index for LPS (Less Probable Symbol)
    bool bSwitch;      // Whether to switch MPS/LPS
  };

  JBig2ArithCtx();

  int DecodeNLPS(const JBig2ArithQe& qe);
  int DecodeNMPS(const JBig2ArithQe& qe);

  unsigned int MPS() const { return mps_ ? 1 : 0; }
  unsigned int I() const { return i_; }

 private:
  bool mps_;          // More probable symbol (0 or 1)
  unsigned int i_;    // Current state index into QE table
};

// MQ arithmetic decoder for binary symbols
class CJBig2_ArithDecoder {
 public:
  explicit CJBig2_ArithDecoder(CJBig2_BitStream* pStream);
  ~CJBig2_ArithDecoder();

  // Decode a single binary symbol using the given context
  int Decode(JBig2ArithCtx* pCX);

  bool IsComplete() const { return complete_; }

 private:
  void BYTEIN();
  void ReadValueA();

  bool complete_;
  uint8_t b_;           // Current byte
  unsigned int c_;      // Code register
  unsigned int a_;      // Interval register
  unsigned int ct_;     // Counter for renormalization
  CJBig2_BitStream* stream_;
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_ARITHDECODER_HH_
