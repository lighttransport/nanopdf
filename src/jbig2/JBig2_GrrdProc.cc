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

#include <cstring>

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
  // Optimized template 0 refinement decoding
  // Preconditions: GRAT = {-1,-1,-1,-1}, GRREFERENCEDX == 0, widths match
  if (!GRREFERENCE || !GRREFERENCE->has_data())
    return DecodeTemplate0Unopt(pArithDecoder, grContexts);

  auto GRREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GRW, GRH));
  if (!GRREG->has_data())
    return nullptr;

  GRREG->Fill(false);
  int LTP = 0;

  int32_t iGRW = static_cast<int32_t>(GRW);
  int32_t stride = GRREG->stride();
  int32_t nLineBytes = iGRW >> 3;
  int32_t nBitsLeft = iGRW - (nLineBytes << 3);

  for (uint32_t h = 0; h < GRH; h++) {
    if (TPGRON) {
      if (pArithDecoder->IsComplete())
        return nullptr;
      LTP = LTP ^ pArithDecoder->Decode(&grContexts[0x0010]);
    }

    // Get line pointers for current output and reference
    const uint8_t* pLine0 = GRREG->GetLine(h - 1);  // GRREG line h-1
    uint8_t* pLineDest = GRREG->GetLine(h);           // GRREG line h (output)
    int32_t nRefOffset = h - GRREFERENCEDY;
    const uint8_t* pLineRef0 = GRREFERENCE->GetLine(nRefOffset - 1);  // ref h-1
    const uint8_t* pLineRef1 = GRREFERENCE->GetLine(nRefOffset);      // ref h
    const uint8_t* pLineRef2 = GRREFERENCE->GetLine(nRefOffset + 1);  // ref h+1

    if (!LTP) {
      uint32_t line0 = pLine0 ? pLine0[0] : 0;
      uint32_t line_cur = 0;
      uint32_t lineRef0 = pLineRef0 ? pLineRef0[0] : 0;
      uint32_t lineRef1 = pLineRef1 ? pLineRef1[0] : 0;
      uint32_t lineRef2 = pLineRef2 ? pLineRef2[0] : 0;

      for (int32_t cc = 0; cc < nLineBytes; cc++) {
        uint32_t line0_next = (cc + 1 < stride && pLine0) ? pLine0[cc + 1] : 0;
        uint32_t lineRef0_next = (cc + 1 < stride && pLineRef0) ? pLineRef0[cc + 1] : 0;
        uint32_t lineRef1_next = (cc + 1 < stride && pLineRef1) ? pLineRef1[cc + 1] : 0;
        uint32_t lineRef2_next = (cc + 1 < stride && pLineRef2) ? pLineRef2[cc + 1] : 0;

        uint8_t cVal = 0;
        for (int32_t k = 7; k >= 0; k--) {
          // Build 13-bit context for Template 0 with GRAT={-1,-1,-1,-1}
          uint32_t CONTEXT = 0;
          // lines[4]: ref h+1, pixels at w-1, w, w+1
          CONTEXT = ((lineRef2 >> (k + 1)) & 0x01) |        // ref(w-1, h+1)
                    (((lineRef2 >> k) & 0x01) << 1) |        // ref(w, h+1)
                    ((k < 7 ? ((lineRef2 >> (k - 1)) & 0x01) :
                              ((lineRef2_next >> 7) & 0x01)) << 2); // ref(w+1, h+1)
          // lines[3]: ref h, pixels at w-1, w, w+1
          CONTEXT |= (((lineRef1 >> (k + 1)) & 0x01) << 3) |  // ref(w-1, h)
                     (((lineRef1 >> k) & 0x01) << 4) |          // ref(w, h)
                     ((k < 7 ? ((lineRef1 >> (k - 1)) & 0x01) :
                               ((lineRef1_next >> 7) & 0x01)) << 5); // ref(w+1, h)
          // lines[2]: ref h-1, pixels at w, w+1
          CONTEXT |= (((lineRef0 >> k) & 0x01) << 6) |        // ref(w, h-1)
                     ((k < 7 ? ((lineRef0 >> (k - 1)) & 0x01) :
                               ((lineRef0_next >> 7) & 0x01)) << 7); // ref(w+1, h-1)
          // GRAT[2,3] adaptive pixel at ref(w-1, h-1)
          CONTEXT |= (((lineRef0 >> (k + 1)) & 0x01) << 8);  // ref(w-1, h-1) for GRAT={-1,-1}
          // lines[1]: current pixel (already decoded in this row)
          CONTEXT |= (((line_cur >> k) & 0x01) << 9);         // cur(w-1, h) - previous pixel
          // lines[0]: cur h-1, pixels at w, w+1
          CONTEXT |= (((line0 >> k) & 0x01) << 10) |          // cur(w, h-1)
                     ((k < 7 ? ((line0 >> (k - 1)) & 0x01) :
                               ((line0_next >> 7) & 0x01)) << 11); // cur(w+1, h-1)
          // GRAT[0,1] adaptive pixel at cur(w-1, h-1) = cur(w-1, h-1) already in line0
          CONTEXT |= (((line0 >> (k + 1)) & 0x01) << 12);    // cur(w-1, h-1) for GRAT={-1,-1}

          if (pArithDecoder->IsComplete())
            return nullptr;
          int bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
          cVal |= (bVal << k);
          // Update running line_cur for context of next pixel
          line_cur = (line_cur << 1) | bVal;
        }
        if (pLineDest)
          pLineDest[cc] = cVal;

        line0 = line0_next;
        lineRef0 = lineRef0_next;
        lineRef1 = lineRef1_next;
        lineRef2 = lineRef2_next;
      }

      // Handle remaining bits
      if (nBitsLeft > 0) {
        uint8_t cVal = 0;
        for (int32_t k = 7; k >= 8 - nBitsLeft; k--) {
          uint32_t CONTEXT = 0;
          CONTEXT = ((lineRef2 >> (k + 1)) & 0x01) |
                    (((lineRef2 >> k) & 0x01) << 1);
          if (k > 0)
            CONTEXT |= (((lineRef2 >> (k - 1)) & 0x01) << 2);
          CONTEXT |= (((lineRef1 >> (k + 1)) & 0x01) << 3) |
                     (((lineRef1 >> k) & 0x01) << 4);
          if (k > 0)
            CONTEXT |= (((lineRef1 >> (k - 1)) & 0x01) << 5);
          CONTEXT |= (((lineRef0 >> k) & 0x01) << 6);
          if (k > 0)
            CONTEXT |= (((lineRef0 >> (k - 1)) & 0x01) << 7);
          CONTEXT |= (((lineRef0 >> (k + 1)) & 0x01) << 8);
          CONTEXT |= (((line_cur >> k) & 0x01) << 9);
          CONTEXT |= (((line0 >> k) & 0x01) << 10);
          if (k > 0)
            CONTEXT |= (((line0 >> (k - 1)) & 0x01) << 11);
          CONTEXT |= (((line0 >> (k + 1)) & 0x01) << 12);

          if (pArithDecoder->IsComplete())
            return nullptr;
          int bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
          cVal |= (bVal << k);
          line_cur = (line_cur << 1) | bVal;
        }
        if (pLineDest)
          pLineDest[nLineBytes] = cVal;
      }
    } else {
      // LTP mode: copy from reference, only decode differing pixels
      if (pLineRef1 && pLineDest) {
        memcpy(pLineDest, pLineRef1, stride);
      } else if (pLineDest) {
        memset(pLineDest, 0, stride);
      }
    }
  }
  return GRREG;
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
  // Optimized template 1 refinement decoding
  // Preconditions: GRREFERENCEDX == 0, widths match
  if (!GRREFERENCE || !GRREFERENCE->has_data())
    return DecodeTemplate1Unopt(pArithDecoder, grContexts);

  auto GRREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(GRW, GRH));
  if (!GRREG->has_data())
    return nullptr;

  GRREG->Fill(false);
  int LTP = 0;

  int32_t iGRW = static_cast<int32_t>(GRW);
  int32_t stride = GRREG->stride();
  int32_t nLineBytes = iGRW >> 3;
  int32_t nBitsLeft = iGRW - (nLineBytes << 3);

  for (uint32_t h = 0; h < GRH; h++) {
    if (TPGRON) {
      if (pArithDecoder->IsComplete())
        return nullptr;
      LTP = LTP ^ pArithDecoder->Decode(&grContexts[0x0008]);
    }

    const uint8_t* pLine0 = GRREG->GetLine(h - 1);
    uint8_t* pLineDest = GRREG->GetLine(h);
    int32_t nRefOffset = h - GRREFERENCEDY;
    const uint8_t* pLineRef0 = GRREFERENCE->GetLine(nRefOffset - 1);
    const uint8_t* pLineRef1 = GRREFERENCE->GetLine(nRefOffset);

    if (!LTP) {
      uint32_t line0 = pLine0 ? pLine0[0] : 0;
      uint32_t line_cur = 0;
      uint32_t lineRef0 = pLineRef0 ? pLineRef0[0] : 0;
      uint32_t lineRef1 = pLineRef1 ? pLineRef1[0] : 0;

      for (int32_t cc = 0; cc < nLineBytes; cc++) {
        uint32_t line0_next = (cc + 1 < stride && pLine0) ? pLine0[cc + 1] : 0;
        uint32_t lineRef0_next = (cc + 1 < stride && pLineRef0) ? pLineRef0[cc + 1] : 0;
        uint32_t lineRef1_next = (cc + 1 < stride && pLineRef1) ? pLineRef1[cc + 1] : 0;

        uint8_t cVal = 0;
        for (int32_t k = 7; k >= 0; k--) {
          // Build 10-bit context for Template 1
          // line5: ref h, pixels at w-1, w, w+1
          uint32_t CONTEXT = ((lineRef1 >> (k + 1)) & 0x01) |        // ref(w-1, h)
                             (((lineRef1 >> k) & 0x01) << 1) |        // ref(w, h)
                             ((k < 7 ? ((lineRef1 >> (k - 1)) & 0x01) :
                                       ((lineRef1_next >> 7) & 0x01)) << 2); // ref(w+1, h)
          // line4: ref h-1, pixels at w, w+1
          CONTEXT |= (((lineRef0 >> k) & 0x01) << 3) |               // ref(w, h-1)
                     ((k < 7 ? ((lineRef0 >> (k - 1)) & 0x01) :
                               ((lineRef0_next >> 7) & 0x01)) << 4);  // ref(w+1, h-1)
          // line2: current pixel (previous in this row)
          CONTEXT |= (((line_cur >> k) & 0x01) << 5);                 // cur(w-1, h)
          // line1: cur h-1, pixels at w, w+1
          CONTEXT |= (((line0 >> k) & 0x01) << 6) |                   // cur(w, h-1)
                     ((k < 7 ? ((line0 >> (k - 1)) & 0x01) :
                               ((line0_next >> 7) & 0x01)) << 7);      // cur(w+1, h-1)
          // GRAT[0,1] adaptive pixel
          CONTEXT |= (((line0 >> (k + 1)) & 0x01) << 8);              // cur(w-1, h-1)

          if (pArithDecoder->IsComplete())
            return nullptr;
          int bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
          cVal |= (bVal << k);
          line_cur = (line_cur << 1) | bVal;
        }
        if (pLineDest)
          pLineDest[cc] = cVal;

        line0 = line0_next;
        lineRef0 = lineRef0_next;
        lineRef1 = lineRef1_next;
      }

      // Handle remaining bits
      if (nBitsLeft > 0) {
        uint8_t cVal = 0;
        for (int32_t k = 7; k >= 8 - nBitsLeft; k--) {
          uint32_t CONTEXT = ((lineRef1 >> (k + 1)) & 0x01) |
                             (((lineRef1 >> k) & 0x01) << 1);
          if (k > 0)
            CONTEXT |= (((lineRef1 >> (k - 1)) & 0x01) << 2);
          CONTEXT |= (((lineRef0 >> k) & 0x01) << 3);
          if (k > 0)
            CONTEXT |= (((lineRef0 >> (k - 1)) & 0x01) << 4);
          CONTEXT |= (((line_cur >> k) & 0x01) << 5);
          CONTEXT |= (((line0 >> k) & 0x01) << 6);
          if (k > 0)
            CONTEXT |= (((line0 >> (k - 1)) & 0x01) << 7);
          CONTEXT |= (((line0 >> (k + 1)) & 0x01) << 8);

          if (pArithDecoder->IsComplete())
            return nullptr;
          int bVal = pArithDecoder->Decode(&grContexts[CONTEXT]);
          cVal |= (bVal << k);
          line_cur = (line_cur << 1) | bVal;
        }
        if (pLineDest)
          pLineDest[nLineBytes] = cVal;
      }
    } else {
      // LTP mode: copy from reference
      if (pLineRef1 && pLineDest) {
        memcpy(pLineDest, pLineRef1, stride);
      } else if (pLineDest) {
        memset(pLineDest, 0, stride);
      }
    }
  }
  return GRREG;
}

}  // namespace jbig2
}  // namespace nanopdf
