// Copyright 2015 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_GrrdProc.hh"

#include "JBig2_ArithDecoder.hh"
#include "JBig2_Image.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_GRRDProc::CJBig2_GRRDProc()
    : GRTEMPLATE(false),
      TPGRON(false),
      GRW(0),
      GRH(0),
      GRREFERENCEDX(0),
      GRREFERENCEDY(0),
      GRREFERENCE(nullptr) {
  GRAT[0] = -1;
  GRAT[1] = -1;
  GRAT[2] = -1;
  GRAT[3] = -1;
}

CJBig2_GRRDProc::~CJBig2_GRRDProc() = default;

std::unique_ptr<CJBig2_Image> CJBig2_GRRDProc::Decode(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* grContexts,
    int context_size) {
  if (!CJBig2_Image::IsValidImageSize(GRW, GRH)) {
    return std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GRW, GRH));
  }

  if (!GRTEMPLATE) {
    if ((GRAT[0] == -1) && (GRAT[1] == -1) && (GRAT[2] == -1) &&
        (GRAT[3] == -1) && (GRREFERENCEDX == 0) &&
        (GRW == static_cast<uint32_t>(GRREFERENCE->width()))) {
      return DecodeTemplate0Opt(pArithDecoder, grContexts);
    }
    return DecodeTemplate0Unopt(pArithDecoder, grContexts);
  }

  if ((GRREFERENCEDX == 0) &&
      (GRW == static_cast<uint32_t>(GRREFERENCE->width()))) {
    return DecodeTemplate1Opt(pArithDecoder, grContexts);
  }

  return DecodeTemplate1Unopt(pArithDecoder, grContexts);
}

std::unique_ptr<CJBig2_Image> CJBig2_GRRDProc::DecodeTemplate0Unopt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* grContexts) {
  auto GRREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GRW, GRH));
  if (!GRREG->has_data()) {
    return nullptr;
  }

  GRREG->Fill(false);
  int LTP = 0;

  for (uint32_t h = 0; h < GRH; h++) {
    if (TPGRON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&grContexts[0x0010]);
    }

    uint32_t lines[5];
    lines[0] = GRREG->GetPixel(1, h - 1);
    lines[0] |= GRREG->GetPixel(0, h - 1) << 1;
    lines[1] = 0;
    lines[2] = GRREFERENCE->GetPixel(-GRREFERENCEDX + 1, h - GRREFERENCEDY - 1);
    lines[2] |= GRREFERENCE->GetPixel(-GRREFERENCEDX, h - GRREFERENCEDY - 1) << 1;
    lines[3] = GRREFERENCE->GetPixel(-GRREFERENCEDX + 1, h - GRREFERENCEDY);
    lines[3] |= GRREFERENCE->GetPixel(-GRREFERENCEDX, h - GRREFERENCEDY) << 1;
    lines[3] |= GRREFERENCE->GetPixel(-GRREFERENCEDX - 1, h - GRREFERENCEDY) << 2;
    lines[4] = GRREFERENCE->GetPixel(-GRREFERENCEDX + 1, h - GRREFERENCEDY + 1);
    lines[4] |= GRREFERENCE->GetPixel(-GRREFERENCEDX, h - GRREFERENCEDY + 1) << 1;
    lines[4] |= GRREFERENCE->GetPixel(-GRREFERENCEDX - 1, h - GRREFERENCEDY + 1) << 2;

    if (!LTP) {
      for (uint32_t w = 0; w < GRW; w++) {
        uint32_t CONTEXT =
            DecodeTemplate0UnoptCalculateContext(*GRREG, lines, w, h);
        if (pArithDecoder->IsComplete()) {
          return nullptr;
        }
        int bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
        DecodeTemplate0UnoptSetPixel(GRREG.get(), lines, w, h, bVal);
      }
    } else {
      for (uint32_t w = 0; w < GRW; w++) {
        int bVal = GRREFERENCE->GetPixel(w, h);
        if (!(TPGRON && (bVal == GRREFERENCE->GetPixel(w - 1, h - 1)) &&
              (bVal == GRREFERENCE->GetPixel(w, h - 1)) &&
              (bVal == GRREFERENCE->GetPixel(w + 1, h - 1)) &&
              (bVal == GRREFERENCE->GetPixel(w - 1, h)) &&
              (bVal == GRREFERENCE->GetPixel(w + 1, h)) &&
              (bVal == GRREFERENCE->GetPixel(w - 1, h + 1)) &&
              (bVal == GRREFERENCE->GetPixel(w, h + 1)) &&
              (bVal == GRREFERENCE->GetPixel(w + 1, h + 1)))) {
          uint32_t CONTEXT =
              DecodeTemplate0UnoptCalculateContext(*GRREG, lines, w, h);
          if (pArithDecoder->IsComplete()) {
            return nullptr;
          }
          bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
        }
        DecodeTemplate0UnoptSetPixel(GRREG.get(), lines, w, h, bVal);
      }
    }
  }
  return GRREG;
}

uint32_t CJBig2_GRRDProc::DecodeTemplate0UnoptCalculateContext(
    const CJBig2_Image& GRREG,
    const uint32_t* lines,
    uint32_t w,
    uint32_t h) const {
  uint32_t CONTEXT = lines[4];
  CONTEXT |= lines[3] << 3;
  CONTEXT |= lines[2] << 6;
  CONTEXT |= GRREFERENCE->GetPixel(w - GRREFERENCEDX + GRAT[2],
                                   h - GRREFERENCEDY + GRAT[3])
             << 8;
  CONTEXT |= lines[1] << 9;
  CONTEXT |= lines[0] << 10;
  CONTEXT |= GRREG.GetPixel(w + GRAT[0], h + GRAT[1]) << 12;
  return CONTEXT;
}

void CJBig2_GRRDProc::DecodeTemplate0UnoptSetPixel(CJBig2_Image* GRREG,
                                                   uint32_t* lines,
                                                   uint32_t w,
                                                   uint32_t h,
                                                   int bVal) {
  GRREG->SetPixel(w, h, bVal);
  lines[0] = ((lines[0] << 1) | GRREG->GetPixel(w + 2, h - 1)) & 0x03;
  lines[1] = ((lines[1] << 1) | bVal) & 0x01;
  lines[2] =
      ((lines[2] << 1) |
       GRREFERENCE->GetPixel(w - GRREFERENCEDX + 2, h - GRREFERENCEDY - 1)) &
      0x03;
  lines[3] = ((lines[3] << 1) | GRREFERENCE->GetPixel(w - GRREFERENCEDX + 2,
                                                      h - GRREFERENCEDY)) &
             0x07;
  lines[4] =
      ((lines[4] << 1) |
       GRREFERENCE->GetPixel(w - GRREFERENCEDX + 2, h - GRREFERENCEDY + 1)) &
      0x07;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRRDProc::DecodeTemplate0Opt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* grContexts) {
  // Optimized template 0 decoding (stubbed for now)
  // Falls back to unoptimized path
  return DecodeTemplate0Unopt(pArithDecoder, grContexts);
}

std::unique_ptr<CJBig2_Image> CJBig2_GRRDProc::DecodeTemplate1Unopt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* grContexts) {
  auto GRREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GRW, GRH));
  if (!GRREG->has_data()) {
    return nullptr;
  }

  GRREG->Fill(false);
  int LTP = 0;

  for (uint32_t h = 0; h < GRH; h++) {
    if (TPGRON) {
      if (pArithDecoder->IsComplete()) {
        return nullptr;
      }
      LTP = LTP ^ pArithDecoder->Decode(&grContexts[0x0008]);
    }

    uint32_t line1 = GRREG->GetPixel(1, h - 1);
    line1 |= GRREG->GetPixel(0, h - 1) << 1;
    uint32_t line2 = 0;
    uint32_t line4 = GRREFERENCE->GetPixel(-GRREFERENCEDX + 1, h - GRREFERENCEDY - 1);
    line4 |= GRREFERENCE->GetPixel(-GRREFERENCEDX, h - GRREFERENCEDY - 1) << 1;
    uint32_t line5 = GRREFERENCE->GetPixel(-GRREFERENCEDX + 1, h - GRREFERENCEDY);
    line5 |= GRREFERENCE->GetPixel(-GRREFERENCEDX, h - GRREFERENCEDY) << 1;
    line5 |= GRREFERENCE->GetPixel(-GRREFERENCEDX - 1, h - GRREFERENCEDY) << 2;

    for (uint32_t w = 0; w < GRW; w++) {
      int bVal;
      if (!LTP) {
        uint32_t CONTEXT = line5;
        CONTEXT |= line4 << 3;
        CONTEXT |= line2 << 5;
        CONTEXT |= line1 << 6;
        CONTEXT |= GRREG->GetPixel(w + GRAT[0], h + GRAT[1]) << 8;
        if (pArithDecoder->IsComplete()) {
          return nullptr;
        }
        bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
      } else {
        bVal = GRREFERENCE->GetPixel(w, h);
      }

      if (bVal) {
        GRREG->SetPixel(w, h, bVal);
      }

      line1 = ((line1 << 1) | GRREG->GetPixel(w + 2, h - 1)) & 0x03;
      line2 = ((line2 << 1) | bVal) & 0x01;
      line4 = ((line4 << 1) |
               GRREFERENCE->GetPixel(w - GRREFERENCEDX + 2, h - GRREFERENCEDY - 1)) &
              0x03;
      line5 = ((line5 << 1) |
               GRREFERENCE->GetPixel(w - GRREFERENCEDX + 2, h - GRREFERENCEDY)) &
              0x07;
    }
  }
  return GRREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_GRRDProc::DecodeTemplate1Opt(
    CJBig2_ArithDecoder* pArithDecoder,
    JBig2ArithCtx* grContexts) {
  // Optimized template 1 decoding (stubbed for now)
  // Falls back to unoptimized path
  return DecodeTemplate1Unopt(pArithDecoder, grContexts);
}

}  // namespace jbig2
}  // namespace nanopdf
