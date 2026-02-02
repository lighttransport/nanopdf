// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_SddProc.hh"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "JBig2_ArithDecoder.hh"
#include "JBig2_ArithIntDecoder.hh"
#include "JBig2_BitStream.hh"
#include "JBig2_GrdProc.hh"
#include "JBig2_GrrdProc.hh"
#include "JBig2_HuffmanTable.hh"
#include "JBig2_Image.hh"
#include "JBig2_SymbolDict.hh"

namespace nanopdf {
namespace jbig2 {

CJBig2_SDDProc::CJBig2_SDDProc()
    : SDHUFF(false),
      SDREFAGG(false),
      SDTEMPLATE(0),
      SDRTEMPLATE(0),
      SDNUMINSYMS(0),
      SDNUMNEWSYMS(0),
      SDNUMEXSYMS(0),
      SDHUFFDH(0),
      SDHUFFDW(0),
      SDHUFFBMSIZE(0),
      SDHUFFAGGINST(0) {
  SDAT.fill(0);
  SDRAT.fill(0);
}

CJBig2_SDDProc::~CJBig2_SDDProc() = default;

std::unique_ptr<CJBig2_SymbolDict> CJBig2_SDDProc::DecodeArith(
    CJBig2_ArithDecoder* pArithDecoder,
    std::vector<JBig2ArithCtx>* gbContexts,
    std::vector<JBig2ArithCtx>* grContexts) {
  auto pDict = std::unique_ptr<CJBig2_SymbolDict>(new CJBig2_SymbolDict());

  // Create integer decoders
  auto pIADH = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIADW = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIAAI = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIAEX = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDX = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());
  auto pIARDY = std::unique_ptr<CJBig2_ArithIntDecoder>(new CJBig2_ArithIntDecoder());

  // Collect all symbols (input + new)
  std::vector<std::unique_ptr<CJBig2_Image>> SDNEWSYMS;
  std::vector<uint32_t> SDNEWSYMWIDTHS;
  SDNEWSYMS.reserve(SDNUMNEWSYMS);

  int32_t HCHEIGHT = 0;
  uint32_t NSYMSDECODED = 0;

  // Generic region decoder for new symbols
  CJBig2_GRDProc GRD;
  GRD.MMR = false;
  GRD.GBTEMPLATE = SDTEMPLATE;
  GRD.TPGDON = false;
  GRD.USESKIP = false;
  GRD.SKIP = nullptr;
  for (int i = 0; i < 8; ++i) {
    GRD.GBAT[i] = SDAT[i];
  }

  // Refinement region decoder
  CJBig2_GRRDProc GRRD;
  GRRD.GRTEMPLATE = SDRTEMPLATE;
  GRRD.TPGRON = false;
  for (int i = 0; i < 4; ++i) {
    GRRD.GRAT[i] = SDRAT[i];
  }

  while (NSYMSDECODED < SDNUMNEWSYMS) {
    // Decode delta height
    int32_t HCDH = 0;
    pIADH->Decode(pArithDecoder, &HCDH);
    HCHEIGHT += HCDH;

    if (HCHEIGHT < 0 || static_cast<uint32_t>(HCHEIGHT) > 0x7fffffff) {
      return nullptr;
    }

    // Decode symbols in this height class
    uint32_t SYMWIDTH = 0;
    for (;;) {
      // Decode delta width (OOB indicates end of height class)
      int32_t DW = 0;
      if (!pIADW->Decode(pArithDecoder, &DW)) {
        break;  // OOB - end of height class
      }

      if (DW < 0 && static_cast<uint32_t>(-DW) > SYMWIDTH) {
        return nullptr;  // Invalid width
      }

      SYMWIDTH += DW;
      if (SYMWIDTH > 0x7fffffff) {
        return nullptr;
      }

      if (NSYMSDECODED >= SDNUMNEWSYMS) {
        return nullptr;  // Too many symbols
      }

      std::unique_ptr<CJBig2_Image> BS;

      if (SDREFAGG) {
        // Refinement/aggregate coding
        int32_t REFAGGNINST = 0;
        pIAAI->Decode(pArithDecoder, &REFAGGNINST);

        if (REFAGGNINST > 1) {
          // Multiple instances - decode as text region
          // Simplified: just decode as generic region for now
          GRD.GBW = SYMWIDTH;
          GRD.GBH = HCHEIGHT;
          BS = GRD.DecodeArith(pArithDecoder, gbContexts->data(),
                               static_cast<int>(gbContexts->size()));
        } else if (REFAGGNINST == 1) {
          // Single instance with refinement
          uint32_t SBSYMCODELEN = 0;
          uint32_t total_syms = SDNUMINSYMS + NSYMSDECODED;
          while ((1u << SBSYMCODELEN) < total_syms) {
            ++SBSYMCODELEN;
          }

          auto pIAID = std::unique_ptr<CJBig2_ArithIaidDecoder>(
              new CJBig2_ArithIaidDecoder(static_cast<uint8_t>(SBSYMCODELEN)));

          uint32_t IDI = 0;
          pIAID->Decode(pArithDecoder, &IDI);

          int32_t RDXI = 0, RDYI = 0;
          pIARDX->Decode(pArithDecoder, &RDXI);
          pIARDY->Decode(pArithDecoder, &RDYI);

          // Get reference symbol
          CJBig2_Image* IBOI = nullptr;
          if (IDI < SDNUMINSYMS) {
            IBOI = SDINSYMS[IDI];
          } else if (IDI - SDNUMINSYMS < SDNEWSYMS.size()) {
            IBOI = SDNEWSYMS[IDI - SDNUMINSYMS].get();
          }

          if (!IBOI) {
            return nullptr;
          }

          // Apply refinement
          GRRD.GRW = SYMWIDTH;
          GRRD.GRH = HCHEIGHT;
          GRRD.GRREFERENCEDX = RDXI;
          GRRD.GRREFERENCEDY = RDYI;
          GRRD.GRREFERENCE = IBOI;

          BS = GRRD.Decode(pArithDecoder, grContexts->data(),
                           static_cast<int>(grContexts->size()));
        } else {
          // Zero instances - create empty symbol
          BS = std::unique_ptr<CJBig2_Image>(new CJBig2_Image(SYMWIDTH, HCHEIGHT));
          BS->Fill(false);
        }
      } else {
        // Direct generic region decoding
        GRD.GBW = SYMWIDTH;
        GRD.GBH = HCHEIGHT;
        BS = GRD.DecodeArith(pArithDecoder, gbContexts->data(),
                             static_cast<int>(gbContexts->size()));
      }

      if (!BS) {
        return nullptr;
      }

      SDNEWSYMS.push_back(std::move(BS));
      SDNEWSYMWIDTHS.push_back(SYMWIDTH);
      ++NSYMSDECODED;
    }
  }

  // Export symbols
  // Build export flags
  uint32_t I = 0;
  uint32_t J = 0;
  int EXINDEX = 0;
  int CUREXFLAG = 0;
  uint32_t EXFLAGS_SIZE = SDNUMINSYMS + SDNUMNEWSYMS;
  std::vector<bool> EXFLAGS(EXFLAGS_SIZE, false);

  while (I < EXFLAGS_SIZE) {
    int32_t EXRUNLENGTH = 0;
    pIAEX->Decode(pArithDecoder, &EXRUNLENGTH);

    if (EXRUNLENGTH < 0 || I + EXRUNLENGTH > EXFLAGS_SIZE) {
      return nullptr;
    }

    if (EXRUNLENGTH > 0) {
      for (uint32_t k = 0; k < static_cast<uint32_t>(EXRUNLENGTH); ++k) {
        EXFLAGS[I + k] = (CUREXFLAG == 1);
      }
    }

    I += EXRUNLENGTH;
    CUREXFLAG = 1 - CUREXFLAG;
  }

  // Export flagged symbols
  for (uint32_t i = 0; i < EXFLAGS_SIZE; ++i) {
    if (EXFLAGS[i]) {
      if (i < SDNUMINSYMS) {
        // Copy from input symbols
        if (SDINSYMS[i]) {
          pDict->AddImage(std::unique_ptr<CJBig2_Image>(new CJBig2_Image(*SDINSYMS[i])));
        }
      } else {
        // Move from new symbols
        uint32_t idx = i - SDNUMINSYMS;
        if (idx < SDNEWSYMS.size() && SDNEWSYMS[idx]) {
          pDict->AddImage(std::move(SDNEWSYMS[idx]));
        }
      }
    }
  }

  return pDict;
}

std::unique_ptr<CJBig2_SymbolDict> CJBig2_SDDProc::DecodeHuffman(
    CJBig2_BitStream* pStream,
    std::vector<JBig2ArithCtx>* gbContexts,
    std::vector<JBig2ArithCtx>* grContexts) {
  // Huffman decoding for symbol dictionaries
  // This is a simplified implementation
  auto pDict = std::unique_ptr<CJBig2_SymbolDict>(new CJBig2_SymbolDict());

  // For now, return empty dictionary for Huffman-coded segments
  // Full implementation requires Huffman table decoding
  return pDict;
}

}  // namespace jbig2
}  // namespace nanopdf
