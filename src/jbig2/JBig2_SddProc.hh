// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_SDDPROC_HH_
#define NANOPDF_JBIG2_SDDPROC_HH_

#include <cstdint>
#include <memory>
#include <vector>
#include <array>

namespace nanopdf {
namespace jbig2 {

class CJBig2_ArithDecoder;
class CJBig2_BitStream;
class CJBig2_HuffmanTable;
class CJBig2_Image;
class CJBig2_SymbolDict;
class JBig2ArithCtx;

// Symbol dictionary decoding procedure
// Decodes symbol dictionary segments according to JBIG2 spec section 6.5
class CJBig2_SDDProc {
 public:
  CJBig2_SDDProc();
  ~CJBig2_SDDProc();

  // Decode symbol dictionary using arithmetic coding
  std::unique_ptr<CJBig2_SymbolDict> DecodeArith(
      CJBig2_ArithDecoder* pArithDecoder,
      std::vector<JBig2ArithCtx>* gbContexts,
      std::vector<JBig2ArithCtx>* grContexts);

  // Decode symbol dictionary using Huffman coding
  std::unique_ptr<CJBig2_SymbolDict> DecodeHuffman(
      CJBig2_BitStream* pStream,
      std::vector<JBig2ArithCtx>* gbContexts,
      std::vector<JBig2ArithCtx>* grContexts);

  // Input symbols from referred segments
  std::vector<CJBig2_Image*> SDINSYMS;

  // Symbol dictionary flags
  bool SDHUFF;         // Use Huffman coding (false = arithmetic)
  bool SDREFAGG;       // Use refinement/aggregate coding
  uint8_t SDTEMPLATE;  // Template for generic region coding (0-3)
  uint8_t SDRTEMPLATE; // Template for refinement coding (0-1)

  // Number of symbols
  uint32_t SDNUMINSYMS;   // Number of input symbols
  uint32_t SDNUMNEWSYMS;  // Number of new symbols to decode
  uint32_t SDNUMEXSYMS;   // Number of exported symbols

  // Huffman table selection (for SDHUFF mode)
  uint8_t SDHUFFDH;   // Delta height table selection
  uint8_t SDHUFFDW;   // Delta width table selection
  uint8_t SDHUFFBMSIZE;  // Bitmap size table selection
  uint8_t SDHUFFAGGINST; // Aggregate instance table selection

  // Adaptive template pixels
  std::array<int8_t, 8> SDAT;   // Generic region AT pixels
  std::array<int8_t, 4> SDRAT;  // Refinement region AT pixels

 private:
  // Decode height class runs (for arithmetic mode)
  std::unique_ptr<CJBig2_SymbolDict> DecodeHeightClassDeltaHeight(
      CJBig2_ArithDecoder* pArithDecoder,
      std::vector<JBig2ArithCtx>* gbContexts,
      std::vector<JBig2ArithCtx>* grContexts);
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_SDDPROC_HH_
