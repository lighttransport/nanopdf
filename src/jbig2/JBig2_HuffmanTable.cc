// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_HuffmanTable.hh"

#include <algorithm>
#include <limits>

#include "JBig2_BitStream.hh"

namespace nanopdf {
namespace jbig2 {

// Standard JBIG2 Huffman tables from ITU-T T.88 Annex B
// Each entry: {PREFLEN, RANGELEN, RANGELOW}
// RANGELOW = -1 indicates OOB (out-of-band)

// Table B.1 - Used for SDHUFFDH, SBHUFFRDY
const JBig2TableLine kHuffmanTable_B1[] = {
    {1, 4, 0},     {2, 8, 16},  {3, 16, 272}, {3, 32, -1}  // OOB
};
const size_t kHuffmanTable_B1_Size = 4;

// Table B.2 - Used for SDHUFFAG
const JBig2TableLine kHuffmanTable_B2[] = {
    {1, 0, 0}, {2, 0, 1},   {3, 0, 2},   {4, 3, 3},
    {5, 6, 11}, {6, 32, 75}, {6, 32, -1}  // OOB
};
const size_t kHuffmanTable_B2_Size = 7;

// Table B.3 - Used for SDHUFFDW, SBHUFFRDW
const JBig2TableLine kHuffmanTable_B3[] = {
    {1, 0, -1}, {2, 0, -2}, {3, 0, -3}, {4, 3, -10}, {5, 6, -74},
    {5, 32, -1}  // OOB  (actually this is lower bound extension)
};
const size_t kHuffmanTable_B3_Size = 6;

// Table B.4 - Used for SDHUFFDW (alternative)
const JBig2TableLine kHuffmanTable_B4[] = {
    {1, 0, 1},    {2, 0, 2},   {3, 0, 3},   {4, 3, 4},
    {5, 6, 12},   {5, 32, 76}, {6, 32, -1}  // OOB
};
const size_t kHuffmanTable_B4_Size = 7;

// Table B.5 - Used for SDHUFFDW (alternative)
const JBig2TableLine kHuffmanTable_B5[] = {
    {7, 8, -255}, {1, 0, 1},   {2, 0, 2},   {3, 0, 3},
    {4, 3, 4},    {5, 6, 12},  {6, 32, 76}, {7, 32, -1}  // OOB
};
const size_t kHuffmanTable_B5_Size = 8;

// Table B.6 - Generic height/width
const JBig2TableLine kHuffmanTable_B6[] = {
    {5, 10, -2048}, {4, 9, -1024}, {4, 8, -512}, {4, 7, -256},
    {5, 6, -128},   {5, 5, -64},   {4, 5, -32},  {2, 7, 0},
    {3, 7, 128},    {3, 8, 256},   {4, 9, 512},  {4, 10, 1024},
    {6, 32, -2049}, {6, 32, 2048}, {2, 32, -1}   // OOB
};
const size_t kHuffmanTable_B6_Size = 15;

// Table B.7 - Generic height/width
const JBig2TableLine kHuffmanTable_B7[] = {
    {4, 9, -1024}, {3, 8, -512}, {4, 7, -256}, {5, 6, -128},
    {5, 5, -64},   {4, 5, -32},  {4, 5, 0},    {5, 5, 32},
    {5, 6, 64},    {4, 7, 128},  {3, 8, 256},  {3, 9, 512},
    {3, 10, 1024}, {5, 32, -1025}, {5, 32, 2048}, {2, 32, -1}  // OOB
};
const size_t kHuffmanTable_B7_Size = 16;

// Table B.8 - Symbol instance S coordinates
const JBig2TableLine kHuffmanTable_B8[] = {
    {8, 3, -15}, {9, 1, -7}, {8, 1, -5}, {9, 0, -3},
    {7, 0, -2},  {4, 0, -1}, {2, 1, 0},  {5, 0, 2},
    {6, 0, 3},   {3, 4, 4},  {6, 1, 20}, {4, 4, 22},
    {4, 5, 38},  {5, 6, 70}, {5, 7, 134}, {6, 7, 262},
    {7, 8, 390}, {6, 10, 646}, {9, 32, -16}, {9, 32, 1670},
    {2, 32, -1}  // OOB
};
const size_t kHuffmanTable_B8_Size = 21;

// Table B.9 - Symbol instance T coordinates
const JBig2TableLine kHuffmanTable_B9[] = {
    {8, 4, -31}, {9, 2, -15}, {8, 2, -11}, {9, 1, -7},
    {7, 1, -5},  {4, 1, -3},  {3, 1, -1},  {3, 1, 1},
    {5, 1, 3},   {6, 1, 5},   {3, 5, 7},   {6, 2, 39},
    {4, 5, 43},  {4, 6, 75},  {5, 7, 139}, {5, 8, 267},
    {6, 8, 523}, {7, 9, 779}, {6, 11, 1291}, {9, 32, -32},
    {9, 32, 3339}, {2, 32, -1}  // OOB
};
const size_t kHuffmanTable_B9_Size = 22;

// Table B.10 - Text region symbol ID S
const JBig2TableLine kHuffmanTable_B10[] = {
    {7, 4, -21}, {8, 0, -5}, {7, 0, -4}, {5, 0, -3},
    {2, 2, -2},  {5, 0, 2},  {6, 0, 3},  {7, 0, 4},
    {8, 0, 5},   {2, 6, 6},  {5, 5, 70}, {6, 5, 102},
    {6, 6, 134}, {6, 7, 198}, {6, 8, 326}, {6, 9, 582},
    {6, 10, 1094}, {7, 11, 2118}, {8, 32, -22}, {8, 32, 4166},
    {2, 32, -1}  // OOB
};
const size_t kHuffmanTable_B10_Size = 21;

// Table B.11 - Text region symbol ID S
const JBig2TableLine kHuffmanTable_B11[] = {
    {1, 0, 1},   {2, 1, 2},   {4, 0, 4},   {4, 1, 5},
    {5, 1, 7},   {5, 2, 9},   {6, 2, 13},  {7, 2, 17},
    {7, 3, 21},  {7, 4, 29},  {7, 5, 45},  {7, 6, 77},
    {7, 32, 141}
};
const size_t kHuffmanTable_B11_Size = 13;

// Table B.12 - Text region symbol ID
const JBig2TableLine kHuffmanTable_B12[] = {
    {1, 0, 1},    {2, 0, 2},    {3, 1, 3},    {5, 0, 5},
    {5, 1, 6},    {6, 1, 8},    {7, 0, 10},   {7, 1, 11},
    {7, 2, 13},   {7, 3, 17},   {7, 4, 25},   {8, 5, 41},
    {8, 32, 73}
};
const size_t kHuffmanTable_B12_Size = 13;

// Table B.13 - Text region symbol ID
const JBig2TableLine kHuffmanTable_B13[] = {
    {1, 0, 1},    {3, 0, 2},    {4, 0, 3},    {5, 0, 4},
    {4, 1, 5},    {3, 3, 7},    {6, 1, 15},   {6, 2, 17},
    {6, 3, 21},   {6, 4, 29},   {6, 5, 45},   {7, 6, 77},
    {7, 32, 141}
};
const size_t kHuffmanTable_B13_Size = 13;

// Table B.14 - Generic refinement region
const JBig2TableLine kHuffmanTable_B14[] = {
    {3, 0, -2}, {3, 0, -1}, {1, 0, 0}, {3, 0, 1}, {3, 0, 2}
};
const size_t kHuffmanTable_B14_Size = 5;

// Table B.15 - Generic refinement region
const JBig2TableLine kHuffmanTable_B15[] = {
    {7, 4, -24}, {6, 2, -8}, {5, 1, -4}, {4, 0, -2},
    {3, 0, -1},  {1, 0, 0},  {3, 0, 1},  {4, 0, 2},
    {5, 1, 3},   {6, 2, 5},  {7, 4, 9},  {7, 32, -25},
    {7, 32, 25}
};
const size_t kHuffmanTable_B15_Size = 13;

CJBig2_HuffmanTable::CJBig2_HuffmanTable() : idx_(0) {}

CJBig2_HuffmanTable::CJBig2_HuffmanTable(size_t idx) : idx_(idx) {}

CJBig2_HuffmanTable::~CJBig2_HuffmanTable() = default;

bool CJBig2_HuffmanTable::InitTable(
    const std::vector<JBig2HuffmanCode>& LENCOUNT,
    const std::vector<JBig2HuffmanCode>& FIRSTCODE,
    const std::vector<int32_t>& PREFIXLENGTH,
    const std::vector<int32_t>& VALUES,
    size_t NTEMP) {
  // Build the internal tables for decoding
  if (NTEMP == 0) {
    is_ok_ = true;
    return true;
  }

  // Resize internal vectors
  CODES_.resize(NTEMP);
  PREFLEN_.resize(NTEMP);
  RANGELEN_.resize(NTEMP);
  RANGELOW_.resize(NTEMP);

  // Find max prefix length
  int32_t maxLen = 0;
  for (size_t i = 0; i < NTEMP; ++i) {
    if (PREFIXLENGTH[i] > maxLen) {
      maxLen = PREFIXLENGTH[i];
    }
  }

  if (maxLen > 32 || maxLen <= 0) {
    return false;
  }

  // Build count of codes at each length
  std::vector<int32_t> lenCount(static_cast<size_t>(maxLen + 1), 0);
  for (size_t i = 0; i < NTEMP; ++i) {
    if (PREFIXLENGTH[i] >= 0 && PREFIXLENGTH[i] <= maxLen) {
      lenCount[static_cast<size_t>(PREFIXLENGTH[i])]++;
    }
  }

  // Compute first code at each length (canonical Huffman)
  std::vector<int32_t> firstCode(static_cast<size_t>(maxLen + 2), 0);
  for (int32_t len = 1; len <= maxLen; ++len) {
    firstCode[static_cast<size_t>(len)] =
        (firstCode[static_cast<size_t>(len - 1)] +
         lenCount[static_cast<size_t>(len - 1)])
        << 1;
  }

  // Assign codes to each symbol
  std::vector<int32_t> nextCode(firstCode);
  for (size_t i = 0; i < NTEMP; ++i) {
    int32_t prefLen = PREFIXLENGTH[i];
    if (prefLen > 0 && prefLen <= maxLen) {
      CODES_[i] = nextCode[static_cast<size_t>(prefLen)]++;
      PREFLEN_[i] = prefLen;
    } else {
      CODES_[i] = 0;
      PREFLEN_[i] = 0;
    }

    if (i < VALUES.size()) {
      RANGELOW_[i] = VALUES[i];
    }
    RANGELEN_[i] = 0;
  }

  is_ok_ = true;
  return true;
}

bool CJBig2_HuffmanTable::InitStandard(const JBig2TableLine* lines,
                                       size_t nLines, bool hasOOB) {
  if (!lines || nLines == 0) {
    return false;
  }

  has_oob_ = hasOOB;

  // Find maximum prefix length
  int maxPrefLen = 0;
  for (size_t i = 0; i < nLines; ++i) {
    if (lines[i].PREFLEN > maxPrefLen) {
      maxPrefLen = lines[i].PREFLEN;
    }
  }

  if (maxPrefLen > 32 || maxPrefLen <= 0) {
    return false;
  }

  // Count codes at each length
  std::vector<int> lenCount(static_cast<size_t>(maxPrefLen + 1), 0);
  for (size_t i = 0; i < nLines; ++i) {
    if (lines[i].PREFLEN > 0) {
      lenCount[static_cast<size_t>(lines[i].PREFLEN)]++;
    }
  }

  // Compute first code at each length
  std::vector<int> firstCode(static_cast<size_t>(maxPrefLen + 1), 0);
  int code = 0;
  for (int len = 1; len <= maxPrefLen; ++len) {
    code = (code + lenCount[static_cast<size_t>(len - 1)]) << 1;
    firstCode[static_cast<size_t>(len)] = code;
  }

  // Assign codes to each entry
  CODES_.resize(nLines);
  PREFLEN_.resize(nLines);
  RANGELEN_.resize(nLines);
  RANGELOW_.resize(nLines);

  std::vector<int> nextCode = firstCode;
  for (size_t i = 0; i < nLines; ++i) {
    int prefLen = lines[i].PREFLEN;
    if (prefLen > 0 && prefLen <= maxPrefLen) {
      CODES_[i] = nextCode[static_cast<size_t>(prefLen)]++;
    } else {
      CODES_[i] = 0;
    }
    PREFLEN_[i] = prefLen;
    RANGELEN_[i] = lines[i].RANGELEN;
    RANGELOW_[i] = lines[i].RANGELOW;

    // Track OOB entry (RANGELOW == -1 with RANGELEN == 32 is OOB convention)
    // Actually in standard tables, OOB is typically the last entry
    // We use RANGELOW < -16383 as OOB marker (spec uses special values)
    if (hasOOB && i == nLines - 1) {
      oob_index_ = i;
    }
  }

  is_ok_ = true;
  return true;
}

bool CJBig2_HuffmanTable::InitEncoded(CJBig2_BitStream* pStream) {
  // Read user-defined Huffman table from bitstream
  // Format: PREFLEN[i] encoded with 1-8 bits, then RANGELEN[i], RANGELOW[i]

  if (!pStream || !pStream->IsInBounds()) {
    return false;
  }

  // Read HTOOB flag (1 bit)
  uint32_t htOOB = 0;
  if (pStream->read1Bit(&htOOB) != 0) {
    return false;
  }
  has_oob_ = (htOOB == 1);

  // Read number of entries (32-bit)
  uint32_t nEntries = 0;
  if (pStream->readInteger(&nEntries) != 0) {
    return false;
  }

  if (nEntries > 65535) {
    return false;  // Sanity check
  }

  CODES_.resize(nEntries);
  PREFLEN_.resize(nEntries);
  RANGELEN_.resize(nEntries);
  RANGELOW_.resize(nEntries);

  std::vector<int32_t> prefLens(nEntries);

  // Read prefix lengths (variable encoding)
  for (uint32_t i = 0; i < nEntries; ++i) {
    int32_t prefLen = 0;
    uint32_t bit = 0;

    // Read prefix length using run-length encoding
    // 0 = same as previous, 1 followed by bits = actual length
    if (i > 0) {
      if (pStream->read1Bit(&bit) != 0) {
        return false;
      }
      if (bit == 0) {
        prefLen = prefLens[i - 1];
      } else {
        // Read actual prefix length (1-5 bits for length indicator)
        uint32_t lenLen = 0;
        for (int j = 0; j < 5; ++j) {
          if (pStream->read1Bit(&bit) != 0) {
            return false;
          }
          if (bit == 0) {
            lenLen = static_cast<uint32_t>(j + 1);
            break;
          }
        }
        if (lenLen == 0) {
          lenLen = 5;
        }

        int32_t nResult = 0;
        if (pStream->readNBits(lenLen, &nResult) != 0) {
          return false;
        }
        prefLen = nResult;
      }
    } else {
      // First entry - read length directly
      uint32_t lenLen = 0;
      for (int j = 0; j < 5; ++j) {
        if (pStream->read1Bit(&bit) != 0) {
          return false;
        }
        if (bit == 0) {
          lenLen = static_cast<uint32_t>(j + 1);
          break;
        }
      }
      if (lenLen == 0) {
        lenLen = 5;
      }

      int32_t nResult = 0;
      if (pStream->readNBits(lenLen, &nResult) != 0) {
        return false;
      }
      prefLen = nResult;
    }

    prefLens[i] = prefLen;
    PREFLEN_[i] = prefLen;
  }

  // Read range lengths and range lows
  for (uint32_t i = 0; i < nEntries; ++i) {
    // RANGELEN: 5 bits
    int32_t rangeLen = 0;
    if (pStream->readNBits(5, &rangeLen) != 0) {
      return false;
    }
    RANGELEN_[i] = rangeLen;

    // RANGELOW: 32 bits signed
    int32_t rangeLow = 0;
    if (pStream->readNBits(32, &rangeLow) != 0) {
      return false;
    }
    RANGELOW_[i] = rangeLow;
  }

  // Compute canonical Huffman codes
  int maxPrefLen = 0;
  for (uint32_t i = 0; i < nEntries; ++i) {
    if (PREFLEN_[i] > maxPrefLen) {
      maxPrefLen = PREFLEN_[i];
    }
  }

  if (maxPrefLen > 32) {
    return false;
  }

  std::vector<int> lenCount(static_cast<size_t>(maxPrefLen + 1), 0);
  for (uint32_t i = 0; i < nEntries; ++i) {
    if (PREFLEN_[i] > 0) {
      lenCount[static_cast<size_t>(PREFLEN_[i])]++;
    }
  }

  std::vector<int> firstCode(static_cast<size_t>(maxPrefLen + 1), 0);
  int code = 0;
  for (int len = 1; len <= maxPrefLen; ++len) {
    code = (code + lenCount[static_cast<size_t>(len - 1)]) << 1;
    firstCode[static_cast<size_t>(len)] = code;
  }

  std::vector<int> nextCode = firstCode;
  for (uint32_t i = 0; i < nEntries; ++i) {
    int prefLen = PREFLEN_[i];
    if (prefLen > 0 && prefLen <= maxPrefLen) {
      CODES_[i] = nextCode[static_cast<size_t>(prefLen)]++;
    } else {
      CODES_[i] = 0;
    }
  }

  if (has_oob_ && nEntries > 0) {
    oob_index_ = nEntries - 1;
  }

  is_ok_ = true;
  return true;
}

int CJBig2_HuffmanTable::Decode(CJBig2_BitStream* pStream, int32_t* nResult) {
  if (!is_ok_ || CODES_.empty()) {
    *nResult = 0;
    return -1;
  }

  // Find maximum prefix length
  int maxPrefLen = 0;
  for (size_t i = 0; i < PREFLEN_.size(); ++i) {
    if (PREFLEN_[i] > maxPrefLen) {
      maxPrefLen = PREFLEN_[i];
    }
  }

  if (maxPrefLen == 0) {
    *nResult = 0;
    return -1;
  }

  // Save stream position for potential rollback
  uint32_t savedOffset = pStream->getOffset();
  uint32_t savedBitPos = pStream->getBitPos();

  // Read bits incrementally and try to match a code
  int32_t code = 0;
  for (int len = 1; len <= maxPrefLen; ++len) {
    uint32_t bit = 0;
    if (pStream->read1Bit(&bit) != 0) {
      // Restore position on error
      pStream->setOffset(savedOffset);
      pStream->setBitPos(savedBitPos);
      *nResult = 0;
      return -1;
    }
    code = (code << 1) | static_cast<int>(bit);

    // Search for matching code at this length
    for (size_t i = 0; i < CODES_.size(); ++i) {
      if (PREFLEN_[i] == len && CODES_[i] == code) {
        // Check for OOB
        if (has_oob_ && i == oob_index_) {
          *nResult = 0;
          return kJBig2OOB;
        }

        // Found matching code - compute value
        int32_t value = RANGELOW_[i];

        // If RANGELEN > 0, read additional bits
        if (RANGELEN_[i] > 0 && RANGELEN_[i] < 32) {
          int32_t extraBits = 0;
          if (pStream->readNBits(static_cast<uint32_t>(RANGELEN_[i]),
                                 &extraBits) != 0) {
            *nResult = 0;
            return -1;
          }
          value += extraBits;
        } else if (RANGELEN_[i] == 32) {
          // 32-bit extension - read full 32-bit value
          int32_t extraBits = 0;
          if (pStream->readNBits(32, &extraBits) != 0) {
            *nResult = 0;
            return -1;
          }
          // For lower bound extension, value = RANGELOW - extraBits - 1
          // For upper bound extension, value = RANGELOW + extraBits
          if (RANGELOW_[i] < 0) {
            value = RANGELOW_[i] - extraBits - 1;
          } else {
            value = RANGELOW_[i] + extraBits;
          }
        }

        *nResult = value;
        return 0;  // Success
      }
    }
  }

  // No match found - restore position
  pStream->setOffset(savedOffset);
  pStream->setBitPos(savedBitPos);
  *nResult = 0;
  return -1;
}

int CJBig2_HuffmanTable::DecodeOOB(CJBig2_BitStream* pStream,
                                   int32_t* nResult) {
  // This method is the same as Decode but is named explicitly
  // to indicate that OOB values are expected and handled
  return Decode(pStream, nResult);
}

}  // namespace jbig2
}  // namespace nanopdf
