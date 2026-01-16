// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_TRDPROC_HH_
#define NANOPDF_JBIG2_TRDPROC_HH_

#include <cstdint>
#include <memory>
#include <vector>
#include <array>

#include "JBig2_Image.hh"

namespace nanopdf {
namespace jbig2 {

class CJBig2_ArithDecoder;
class CJBig2_ArithIaidDecoder;
class CJBig2_ArithIntDecoder;
class CJBig2_BitStream;
class JBig2ArithCtx;

// Text region decoding procedure
// Decodes text region segments according to JBIG2 spec section 6.4
class CJBig2_TRDProc {
 public:
  CJBig2_TRDProc();
  ~CJBig2_TRDProc();

  // Decode text region using arithmetic coding
  std::unique_ptr<CJBig2_Image> DecodeArith(
      CJBig2_ArithDecoder* pArithDecoder,
      std::vector<JBig2ArithCtx>* grContexts,
      CJBig2_ArithIaidDecoder* pIAID);

  // Decode text region using Huffman coding
  std::unique_ptr<CJBig2_Image> DecodeHuffman(
      CJBig2_BitStream* pStream,
      std::vector<JBig2ArithCtx>* grContexts);

  // Region parameters
  int32_t SBW;   // Region width
  int32_t SBH;   // Region height
  uint32_t SBNUMINSTANCES;  // Number of symbol instances

  // Text region flags
  bool SBHUFF;         // Use Huffman coding (false = arithmetic)
  bool SBREFINE;       // Use refinement coding
  uint8_t SBSTRIPS;    // Strip size (log2)
  uint8_t SBNUMSYMS;   // Number of symbols in dictionary
  uint8_t SBDEFPIXEL;  // Default pixel value (0 or 1)
  JBig2ComposeOp SBCOMBOP;  // Combining operator
  bool TRANSPOSED;     // Symbols are transposed
  uint8_t REFCORNER;   // Reference corner (0=BL, 1=TL, 2=BR, 3=TR)
  int8_t SBDSOFFSET;   // DS offset
  uint8_t SBRTEMPLATE; // Refinement template (0 or 1)

  // Symbol dictionary
  std::vector<CJBig2_Image*> SBSYMS;
  uint32_t SBNUMSYMS_TOTAL;  // Total symbols including input

  // Huffman table selection
  uint8_t SBHUFFFS;    // First S table
  uint8_t SBHUFFDS;    // DS table
  uint8_t SBHUFFDT;    // DT table
  uint8_t SBHUFFRDW;   // RDW table
  uint8_t SBHUFFRDH;   // RDH table
  uint8_t SBHUFFRDX;   // RDX table
  uint8_t SBHUFFRDY;   // RDY table
  uint8_t SBHUFFRSIZE; // RSIZE table

  // Adaptive template pixels
  std::array<int8_t, 4> SBRAT;  // Refinement AT pixels

 private:
  // Place a symbol instance on the region
  void PlaceSymbol(CJBig2_Image* pDst,
                   CJBig2_Image* pSrc,
                   int32_t x, int32_t y,
                   JBig2ComposeOp op);

  // Decode symbol instances
  std::unique_ptr<CJBig2_Image> DecodeSymbolInstances(
      CJBig2_ArithDecoder* pArithDecoder,
      std::vector<JBig2ArithCtx>* grContexts,
      CJBig2_ArithIaidDecoder* pIAID);
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_TRDPROC_HH_
