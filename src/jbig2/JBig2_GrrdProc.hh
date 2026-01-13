// Copyright 2015 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_GRRDPROC_HH_
#define NANOPDF_JBIG2_GRRDPROC_HH_

#include <cstdint>
#include <memory>
#include <array>

namespace nanopdf {
namespace jbig2 {

class CJBig2_ArithDecoder;
class CJBig2_Image;
class JBig2ArithCtx;

// Generic refinement region decoder processor
// Refines an existing bitmap using a reference image
class CJBig2_GRRDProc {
 public:
  CJBig2_GRRDProc();
  ~CJBig2_GRRDProc();

  // Decode refinement region
  std::unique_ptr<CJBig2_Image> Decode(CJBig2_ArithDecoder* pArithDecoder,
                                       JBig2ArithCtx* grContexts,
                                       int context_size);

  // Public parameters (set before calling Decode)
  bool GRTEMPLATE;        // Template number (0 or 1)
  bool TPGRON;            // Typical prediction for refinement enabled
  uint32_t GRW;           // Width of refinement region
  uint32_t GRH;           // Height of refinement region
  int32_t GRREFERENCEDX;  // Reference X offset
  int32_t GRREFERENCEDY;  // Reference Y offset
  CJBig2_Image* GRREFERENCE;  // Reference image
  std::array<int8_t, 4> GRAT;  // Adaptive template offsets

 private:
  uint32_t DecodeTemplate0UnoptCalculateContext(const CJBig2_Image& GRREG,
                                                const uint32_t* lines,
                                                uint32_t w,
                                                uint32_t h) const;

  void DecodeTemplate0UnoptSetPixel(CJBig2_Image* GRREG,
                                    uint32_t* lines,
                                    uint32_t w,
                                    uint32_t h,
                                    int bVal);

  std::unique_ptr<CJBig2_Image> DecodeTemplate0Unopt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* grContexts);

  std::unique_ptr<CJBig2_Image> DecodeTemplate0Opt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* grContexts);

  std::unique_ptr<CJBig2_Image> DecodeTemplate1Unopt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* grContexts);

  std::unique_ptr<CJBig2_Image> DecodeTemplate1Opt(
      CJBig2_ArithDecoder* pArithDecoder,
      JBig2ArithCtx* grContexts);
};

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_GRRDPROC_HH_
