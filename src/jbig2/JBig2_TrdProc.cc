// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_TrdProc.hh"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "JBig2_ArithDecoder.hh"
#include "JBig2_ArithIntDecoder.hh"
#include "JBig2_BitStream.hh"
#include "JBig2_GrrdProc.hh"
#include "JBig2_Image.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_TRDProc::CJBig2_TRDProc()
    : SBW(0),
      SBH(0),
      SBNUMINSTANCES(0),
      SBHUFF(false),
      SBREFINE(false),
      SBSTRIPS(0),
      SBNUMSYMS(0),
      SBDEFPIXEL(0),
      SBCOMBOP(JBIG2_COMPOSE_OR),
      TRANSPOSED(false),
      REFCORNER(0),
      SBDSOFFSET(0),
      SBRTEMPLATE(0),
      SBNUMSYMS_TOTAL(0),
      SBHUFFFS(0),
      SBHUFFDS(0),
      SBHUFFDT(0),
      SBHUFFRDW(0),
      SBHUFFRDH(0),
      SBHUFFRDX(0),
      SBHUFFRDY(0),
      SBHUFFRSIZE(0) {
  SBRAT.fill(0);
}

CJBig2_TRDProc::~CJBig2_TRDProc() = default;

void CJBig2_TRDProc::PlaceSymbol(CJBig2_Image* pDst,
                                 CJBig2_Image* pSrc,
                                 int32_t x, int32_t y,
                                 JBig2ComposeOp op) {
  if (!pDst || !pSrc) {
    return;
  }
  pDst->ComposeFrom(x, y, pSrc, op);
}

std::unique_ptr<CJBig2_Image> CJBig2_TRDProc::DecodeArith(
    CJBig2_ArithDecoder* pArithDecoder,
    std::vector<JBig2ArithCtx>* grContexts,
    CJBig2_ArithIaidDecoder* pIAID) {
  if (!CJBig2_Image::IsValidImageSize(SBW, SBH)) {
    return nullptr;
  }

  auto SBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(SBW, SBH));
  if (!SBREG->has_data()) {
    return nullptr;
  }

  SBREG->Fill(SBDEFPIXEL != 0);

  // Create integer decoders
  auto pIAFS = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIADS = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIADT = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIAIT = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARI = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDW = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDH = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDX = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDY = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());

  // Strip size
  int32_t STRIPT = -(1 << SBSTRIPS);
  int32_t FIRSTS = 0;
  uint32_t NINSTANCES = 0;

  // Refinement region decoder
  CJBig2_GRRDProc GRRD;
  GRRD.GRTEMPLATE = SBRTEMPLATE;
  GRRD.TPGRON = false;
  for (int i = 0; i < 4; ++i) {
    GRRD.GRAT[i] = SBRAT[i];
  }

  while (NINSTANCES < SBNUMINSTANCES) {
    // Decode DT (delta T)
    int32_t DT = 0;
    pIADT->Decode(pArithDecoder, &DT);
    STRIPT += DT * (1 << SBSTRIPS);

    // First symbol flag for the strip
    bool bFirst = true;
    int32_t CURS = 0;

    for (;;) {
      // Decode DS or FS
      if (bFirst) {
        int32_t DFS = 0;
        pIAFS->Decode(pArithDecoder, &DFS);
        FIRSTS += DFS;
        CURS = FIRSTS;
        bFirst = false;
      } else {
        int32_t IDS = 0;
        if (!pIADS->Decode(pArithDecoder, &IDS)) {
          break;  // OOB - end of strip
        }
        CURS += IDS + SBDSOFFSET;
      }

      if (NINSTANCES >= SBNUMINSTANCES) {
        break;
      }

      // Decode T offset within strip
      int32_t CURT = 0;
      if (SBSTRIPS != 0) {
        pIAIT->Decode(pArithDecoder, &CURT);
        CURT &= ((1 << SBSTRIPS) - 1);
      }
      int32_t TI = STRIPT + CURT;

      // Decode symbol ID
      uint32_t IDI = 0;
      pIAID->Decode(pArithDecoder, &IDI);

      if (IDI >= SBNUMSYMS_TOTAL) {
        return nullptr;  // Invalid symbol ID
      }

      CJBig2_Image* IBI = SBSYMS[IDI];
      if (!IBI) {
        ++NINSTANCES;
        continue;
      }

      // Get symbol dimensions
      int32_t WI = IBI->width();
      int32_t HI = IBI->height();

      // Apply refinement if enabled
      std::unique_ptr<CJBig2_Image> pRefImage;
      if (SBREFINE) {
        int32_t RI = 0;
        pIARI->Decode(pArithDecoder, &RI);

        if (RI != 0) {
          // Decode refinement adjustments
          int32_t RDW = 0, RDH = 0, RDX = 0, RDY = 0;
          pIARDW->Decode(pArithDecoder, &RDW);
          pIARDH->Decode(pArithDecoder, &RDH);
          pIARDX->Decode(pArithDecoder, &RDX);
          pIARDY->Decode(pArithDecoder, &RDY);

          WI += RDW;
          HI += RDH;

          if (WI < 0 || HI < 0) {
            return nullptr;
          }

          // Apply refinement
          GRRD.GRW = WI;
          GRRD.GRH = HI;
          GRRD.GRREFERENCEDX = (RDW / 2) + RDX;
          GRRD.GRREFERENCEDY = (RDH / 2) + RDY;
          GRRD.GRREFERENCE = IBI;

          pRefImage = GRRD.Decode(pArithDecoder, grContexts->data(),
                                  static_cast<int>(grContexts->size()));
          if (!pRefImage) {
            return nullptr;
          }
          IBI = pRefImage.get();
          WI = IBI->width();
          HI = IBI->height();
        }
      }

      // Calculate placement position based on reference corner
      int32_t SI, TI_FINAL;
      if (TRANSPOSED) {
        switch (REFCORNER) {
          case 0:  // Bottom-left
            SI = CURS;
            TI_FINAL = TI - WI + 1;
            break;
          case 1:  // Top-left
            SI = CURS;
            TI_FINAL = TI;
            break;
          case 2:  // Bottom-right
            SI = CURS - HI + 1;
            TI_FINAL = TI - WI + 1;
            break;
          case 3:  // Top-right
          default:
            SI = CURS - HI + 1;
            TI_FINAL = TI;
            break;
        }
        CURS += HI - 1;
      } else {
        switch (REFCORNER) {
          case 0:  // Bottom-left
            SI = TI - HI + 1;
            TI_FINAL = CURS;
            break;
          case 1:  // Top-left
            SI = TI;
            TI_FINAL = CURS;
            break;
          case 2:  // Bottom-right
            SI = TI - HI + 1;
            TI_FINAL = CURS - WI + 1;
            break;
          case 3:  // Top-right
          default:
            SI = TI;
            TI_FINAL = CURS - WI + 1;
            break;
        }
        CURS += WI - 1;
      }

      // Place the symbol
      if (TRANSPOSED) {
        PlaceSymbol(SBREG.get(), IBI, TI_FINAL, SI, SBCOMBOP);
      } else {
        PlaceSymbol(SBREG.get(), IBI, TI_FINAL, SI, SBCOMBOP);
      }

      ++NINSTANCES;
    }
  }

  return SBREG;
}

std::unique_ptr<CJBig2_Image> CJBig2_TRDProc::DecodeHuffman(
    CJBig2_BitStream* pStream,
    std::vector<JBig2ArithCtx>* grContexts) {
  // Huffman decoding for text regions
  // This is a simplified implementation that returns an empty region
  auto SBREG = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(SBW, SBH));
  if (SBREG->has_data()) {
    SBREG->Fill(SBDEFPIXEL != 0);
  }
  return SBREG;
}

}  // namespace jbig2
}  // namespace nanopdf
