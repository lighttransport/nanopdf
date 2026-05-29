// Runtime-dispatched front end for the vendored fpnge PNG encoder.
//
// fpnge.cc is compiled twice (AVX2 and SSE4.1); this picks the best available
// path at runtime. Callers should treat a false return as "fpnge unavailable
// or failed" and fall back to their existing PNG encoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nanopdf {

// Which fpnge SIMD path runtime dispatch selected. Useful for logging/tests.
// SSE2 is the pure-SSE2 fallback (scalar PSHUFB etc.) — the x86-64 baseline.
enum class FpngeIsa { None, SSE2, SSE41, AVX2 };
FpngeIsa fpnge_active_isa();

// True if a usable fpnge path (AVX2 / SSE4.1 / SSE2) is present on this CPU.
bool fpnge_available();

// Encode an 8-bit interleaved image to a complete PNG byte stream in |out|.
// |channels| is 1/2/3/4 (G/GA/RGB/RGBA); pixel byte order is R,G,B,A.
// |stride| is the source row stride in bytes. |level| maps to fpnge's
// compression level (0 = fpnge default). Returns false (leaving |out|
// untouched) when no SIMD path is available or encoding fails.
bool fpnge_encode_png(const uint8_t* data, int width, int height, int channels,
                      int stride, int level, std::vector<uint8_t>& out);

}  // namespace nanopdf
