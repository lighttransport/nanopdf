// Copyright 2014 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility

#ifndef NANOPDF_JBIG2_DEFINE_HH_
#define NANOPDF_JBIG2_DEFINE_HH_

#include <cstdint>

namespace nanopdf {
namespace jbig2 {

struct JBig2RegionInfo {
  int32_t width;
  int32_t height;
  int32_t x;
  int32_t y;
  uint8_t flags;
};

struct JBig2HuffmanCode {
  int32_t codelen;
  int32_t code;
};

constexpr int32_t kJBig2OOB = 1;

constexpr int32_t kJBig2MaxReferredSegmentCount = 64;
constexpr uint32_t kJBig2MaxExportSymbols = 65535;
constexpr uint32_t kJBig2MaxNewSymbols = 65535;
constexpr uint32_t kJBig2MaxPatternIndex = 65535;
constexpr int32_t kJBig2MaxImageSize = 65535;
constexpr uint32_t kJBig2MaxStreamSize = 256 * 1024 * 1024;  // 256 MB limit

}  // namespace jbig2
}  // namespace nanopdf

#endif  // NANOPDF_JBIG2_DEFINE_HH_
