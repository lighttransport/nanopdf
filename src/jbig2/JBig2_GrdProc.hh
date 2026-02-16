// Copyright 2015 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_GRDPROC_HH_
#define NANOPDF_JBIG2_GRDPROC_HH_

#include <cstdint>
#include <memory>
#include <array>

namespace nanopdf {
namespace jbig2 {

class CJBig2_ArithDecoder;
class CJBig2_BitStream;
class CJBig2_Image;
class JBig2ArithCtx;

// Generic region decoder processor
// Implements template-based bitmap decoding with arithmetic coding
class CJBig2_GRDProc {
 public:
  CJBig2_GRDProc();
  ~CJBig2_GRDProc();

  // Decode arithmetic-coded generic region
  std::unique_ptr<CJBig2_Image> DecodeArith(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* gbContexts,
      int context_size);

  // Decode MMR-coded generic region (CCITT Group 4)
  std::unique_ptr<CJBig2_Image> DecodeMMR(CJBig2_BitStream* pStream);

  // Public parameters (set before calling Decode)
  bool MMR;           // Use MMR (Modified Modified READ) compression
  bool TPGDON;        // Typical prediction enabled
  bool USESKIP;       // Skip certain pixels
  uint8_t GBTEMPLATE; // Template number (0-3)
  uint32_t GBW;       // Width of region
  uint32_t GBH;       // Height of region
  CJBig2_Image* SKIP; // Skip mask image (optional)
  std::array<int8_t, 8> GBAT;  // Adaptive template offsets

 private:
  bool UseTemplate0Opt3() const;
  bool UseTemplate1Opt3() const;
  bool UseTemplate23Opt3() const;

  // Optimized decoding paths (byte-aligned templates)
  std::unique_ptr<CJBig2_Image> DecodeArithOpt3(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* gbContexts,
      int OPT);

  // Unoptimized decoding paths (pixel-by-pixel)
  std::unique_ptr<CJBig2_Image> DecodeArithTemplateUnopt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* gbContexts,
      int UNOPT);

  // Template 3 special cases
  std::unique_ptr<CJBig2_Image> DecodeArithTemplate3Opt3(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* gbContexts);

  std::unique_ptr<CJBig2_Image> DecodeArithTemplate3Unopt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* gbContexts);
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_GRDPROC_HH_
