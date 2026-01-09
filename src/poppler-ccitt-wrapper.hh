// SPDX-License-Identifier: GPL-2.0-or-later
// Wrapper to use Poppler's CCITT decoder in nanopdf

#ifndef NANOPDF_POPPLER_CCITT_WRAPPER_HH
#define NANOPDF_POPPLER_CCITT_WRAPPER_HH

#include <vector>
#include <cstdint>
#include <cstddef>

namespace nanopdf {
namespace poppler_ccitt {

// Decode CCITT compressed data using Poppler's decoder
// Returns decoded data as vector of bytes
std::vector<uint8_t> decode_ccitt_poppler(const uint8_t* data, size_t size,
                                           int encoding,      // K parameter
                                           bool endOfLine,
                                           bool byteAlign,
                                           int columns,
                                           int rows,
                                           bool endOfBlock,
                                           bool blackIs1);

} // namespace poppler_ccitt
} // namespace nanopdf

#endif // NANOPDF_POPPLER_CCITT_WRAPPER_HH
