// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#include "JBig2_ArithDecoder.hh"

#include "JBig2_BitStream.hh"

#include <cassert>

namespace nanopdf {
namespace jbig2 {

namespace {

// QE probability estimation table for MQ arithmetic coder (46 states)
// Based on Annex E of JBIG2 specification (ISO/IEC 14492)
const JBig2ArithCtx::JBig2ArithQe kQeTable[46] = {
    {0x5601, 1, 1, true},    {0x3401, 2, 6, false},   {0x1801, 3, 9, false},
    {0x0AC1, 4, 12, false},  {0x0521, 5, 29, false},  {0x0221, 38, 33, false},
    {0x5601, 7, 6, true},    {0x5401, 8, 14, false},  {0x4801, 9, 14, false},
    {0x3801, 10, 14, false}, {0x3001, 11, 17, false}, {0x2401, 12, 18, false},
    {0x1C01, 13, 20, false}, {0x1601, 29, 21, false}, {0x5601, 15, 14, true},
    {0x5401, 16, 14, false}, {0x5101, 17, 15, false}, {0x4801, 18, 16, false},
    {0x3801, 19, 17, false}, {0x3401, 20, 18, false}, {0x3001, 21, 19, false},
    {0x2801, 22, 19, false}, {0x2401, 23, 20, false}, {0x2201, 24, 21, false},
    {0x1C01, 25, 22, false}, {0x1801, 26, 23, false}, {0x1601, 27, 24, false},
    {0x1401, 28, 25, false}, {0x1201, 29, 26, false}, {0x1101, 30, 27, false},
    {0x0AC1, 31, 28, false}, {0x09C1, 32, 29, false}, {0x08A1, 33, 30, false},
    {0x0521, 34, 31, false}, {0x0441, 35, 32, false}, {0x02A1, 36, 33, false},
    {0x0221, 37, 34, false}, {0x0141, 38, 35, false}, {0x0111, 39, 36, false},
    {0x0085, 40, 37, false}, {0x0049, 41, 38, false}, {0x0025, 42, 39, false},
    {0x0015, 43, 40, false}, {0x0009, 44, 41, false}, {0x0005, 45, 42, false},
    {0x0001, 45, 43, false}, {0x5601, 46, 46, false}};

const unsigned int kDefaultAValue = 0x8000;
const size_t kQeTableSize = sizeof(kQeTable) / sizeof(kQeTable[0]);

}  // namespace

JBig2ArithCtx::JBig2ArithCtx() : mps_(false), i_(0) {}

int JBig2ArithCtx::DecodeNLPS(const JBig2ArithQe& qe) {
  bool D = !mps_;
  if (qe.bSwitch) {
    mps_ = !mps_;
  }
  i_ = qe.NLPS;
  assert(i_ < kQeTableSize);
  return D ? 1 : 0;
}

int JBig2ArithCtx::DecodeNMPS(const JBig2ArithQe& qe) {
  i_ = qe.NMPS;
  assert(i_ < kQeTableSize);
  return MPS();
}

CJBig2_ArithDecoder::CJBig2_ArithDecoder(CJBig2_BitStream* pStream)
    : complete_(false), b_(0), c_(0), a_(0), ct_(0), stream_(pStream) {
  b_ = stream_->getCurByte_arith();
  c_ = (b_ ^ 0xff) << 16;
  BYTEIN();
  c_ = c_ << 7;
  ct_ = ct_ - 7;
  a_ = kDefaultAValue;
}

CJBig2_ArithDecoder::~CJBig2_ArithDecoder() = default;

int CJBig2_ArithDecoder::Decode(JBig2ArithCtx* pCX) {
  assert(pCX->I() < kQeTableSize);

  const JBig2ArithCtx::JBig2ArithQe& qe = kQeTable[pCX->I()];
  a_ -= qe.Qe;

  if ((c_ >> 16) < a_) {
    // MPS path
    if (a_ & kDefaultAValue) {
      return pCX->MPS();
    }

    const int D = a_ < qe.Qe ? pCX->DecodeNLPS(qe) : pCX->DecodeNMPS(qe);
    ReadValueA();
    return D;
  }

  // LPS path
  c_ -= a_ << 16;
  const int D = a_ < qe.Qe ? pCX->DecodeNMPS(qe) : pCX->DecodeNLPS(qe);
  a_ = qe.Qe;
  ReadValueA();
  return D;
}

void CJBig2_ArithDecoder::BYTEIN() {
  if (b_ == 0xff) {
    unsigned char B1 = stream_->getNextByte_arith();
    if (B1 > 0x8f) {
      ct_ = 8;
    } else {
      stream_->incByteIdx();
      b_ = B1;
      c_ = c_ + 0xfe00 - (b_ << 9);
      ct_ = 7;
    }
  } else {
    stream_->incByteIdx();
    b_ = stream_->getCurByte_arith();
    c_ = c_ + 0xff00 - (b_ << 8);
    ct_ = 8;
  }

  if (!stream_->IsInBounds()) {
    complete_ = true;
  }
}

void CJBig2_ArithDecoder::ReadValueA() {
  do {
    if (ct_ == 0) {
      BYTEIN();
    }
    a_ <<= 1;
    c_ <<= 1;
    --ct_;
  } while ((a_ & kDefaultAValue) == 0);
}

}  // namespace jbig2
}  // namespace nanopdf
