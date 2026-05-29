// Runtime dispatch between the AVX2 and SSE4.1 fpnge code paths.
//
// Compiled with baseline ISA flags (no -mavx2/-msse4.1): it only *calls* the
// per-ISA encoders, which are safe to invoke once the CPU has been probed via
// __builtin_cpu_supports(). The encoders themselves live in fpnge_avx2.cc /
// fpnge_sse41.cc, each a separate translation unit built with the matching ISA.
#include "fpnge_dispatch.hh"

#include <cstdlib>
#include <cstring>

#include "fpnge.h"

namespace {

// The per-ISA instantiations of FPNGEEncode (renamed per translation unit).
// Each is declared only when its translation unit is actually compiled in
// (NANOPDF_FPNGE_HAVE_* set by CMake), so unbuilt paths never get referenced.
// On WASM only the SSE2 path is built (Emscripten maps SSE2 -> WASM SIMD128;
// SSE4.1/AVX2 have no WASM mapping).
#ifdef NANOPDF_FPNGE_HAVE_AVX2
extern "C" size_t fpnge_encode_avx2(size_t bytes_per_channel,
                                    size_t num_channels, const void* data,
                                    size_t width, size_t row_stride,
                                    size_t height, void* output,
                                    const FPNGEOptions* options);
#endif
#ifdef NANOPDF_FPNGE_HAVE_SSE41
extern "C" size_t fpnge_encode_sse41(size_t bytes_per_channel,
                                     size_t num_channels, const void* data,
                                     size_t width, size_t row_stride,
                                     size_t height, void* output,
                                     const FPNGEOptions* options);
#endif
#ifdef NANOPDF_FPNGE_HAVE_SSE2
extern "C" size_t fpnge_encode_sse2(size_t bytes_per_channel,
                                    size_t num_channels, const void* data,
                                    size_t width, size_t row_stride,
                                    size_t height, void* output,
                                    const FPNGEOptions* options);
#endif

// Probe the CPU once and cache the chosen path. NANOPDF_FPNGE_ISA can force a
// path ("avx2", "sse41"/"sse", "none"/"off") for testing or to cap the ISA; a
// forced path is still validated against the CPU's actual capabilities so we
// never dispatch to instructions the hardware lacks.
nanopdf::FpngeIsa detect_isa() {
  bool has_avx2 = false, has_sse41 = false, has_sse2 = false;
#if defined(__wasm_simd128__)
  // Emscripten lowers the SSE2 intrinsics used by the SSE2 path to WASM
  // SIMD128. The 256-bit AVX2 path and the SSE4.1-specific ops have no WASM
  // mapping, so SSE2 is the only available vector path under WASM SIMD.
  has_sse2 = true;
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||           \
    defined(_M_IX86)
#if defined(__x86_64__) || defined(_M_X64)
  has_sse2 = true;  // SSE2 is part of the x86-64 baseline
#endif
#if defined(__GNUC__) || defined(__clang__)
  has_avx2 = __builtin_cpu_supports("avx2");
  has_sse41 = __builtin_cpu_supports("sse4.1");
  has_sse2 = has_sse2 || __builtin_cpu_supports("sse2");
#endif
#endif

  // Never select a path whose translation unit was not compiled in.
#ifndef NANOPDF_FPNGE_HAVE_AVX2
  has_avx2 = false;
#endif
#ifndef NANOPDF_FPNGE_HAVE_SSE41
  has_sse41 = false;
#endif
#ifndef NANOPDF_FPNGE_HAVE_SSE2
  has_sse2 = false;
#endif

  if (const char* force = std::getenv("NANOPDF_FPNGE_ISA")) {
    if ((!std::strcmp(force, "none") || !std::strcmp(force, "off")))
      return nanopdf::FpngeIsa::None;
    if (!std::strcmp(force, "avx2") && has_avx2) return nanopdf::FpngeIsa::AVX2;
    if ((!std::strcmp(force, "sse41") || !std::strcmp(force, "sse")) &&
        has_sse41)
      return nanopdf::FpngeIsa::SSE41;
    if (!std::strcmp(force, "sse2") && has_sse2) return nanopdf::FpngeIsa::SSE2;
    // Unknown/unsupported request: fall through to automatic selection.
  }

  if (has_avx2) return nanopdf::FpngeIsa::AVX2;
  if (has_sse41) return nanopdf::FpngeIsa::SSE41;
  if (has_sse2) return nanopdf::FpngeIsa::SSE2;
  return nanopdf::FpngeIsa::None;
}

const nanopdf::FpngeIsa kActiveIsa = detect_isa();

}  // namespace

namespace nanopdf {

FpngeIsa fpnge_active_isa() { return kActiveIsa; }

bool fpnge_available() { return kActiveIsa != FpngeIsa::None; }

bool fpnge_encode_png(const uint8_t* data, int width, int height, int channels,
                      int stride, int level, std::vector<uint8_t>& out) {
  if (kActiveIsa == FpngeIsa::None || !data || width <= 0 || height <= 0 ||
      channels < 1 || channels > 4) {
    return false;
  }

  FPNGEOptions options;
  // fpnge levels are 1..5 (5 = best/slowest). Map the caller's 0..9 request:
  // 0 -> fpnge default; otherwise clamp into fpnge's range, treating high
  // requests as "best".
  int fpnge_level = 0;  // 0 => FPNGEFillOptions uses its default (4)
  if (level > 0) {
    if (level >= 9)
      fpnge_level = FPNGE_COMPRESS_LEVEL_BEST;
    else if (level >= 5)
      fpnge_level = FPNGE_COMPRESS_LEVEL_DEFAULT;
    else
      fpnge_level = level;
  }
  FPNGEFillOptions(&options, fpnge_level, FPNGE_CICP_NONE);

  const size_t bytes_per_channel = 1;
  out.resize(FPNGEOutputAllocSize(bytes_per_channel,
                                  static_cast<size_t>(channels),
                                  static_cast<size_t>(width),
                                  static_cast<size_t>(height)));

  size_t n = 0;
  switch (kActiveIsa) {
    case FpngeIsa::AVX2:
#ifdef NANOPDF_FPNGE_HAVE_AVX2
      n = fpnge_encode_avx2(bytes_per_channel, static_cast<size_t>(channels),
                            data, static_cast<size_t>(width),
                            static_cast<size_t>(stride),
                            static_cast<size_t>(height), out.data(), &options);
#endif
      break;
    case FpngeIsa::SSE41:
#ifdef NANOPDF_FPNGE_HAVE_SSE41
      n = fpnge_encode_sse41(bytes_per_channel, static_cast<size_t>(channels),
                             data, static_cast<size_t>(width),
                             static_cast<size_t>(stride),
                             static_cast<size_t>(height), out.data(), &options);
#endif
      break;
    case FpngeIsa::SSE2:
#ifdef NANOPDF_FPNGE_HAVE_SSE2
      n = fpnge_encode_sse2(bytes_per_channel, static_cast<size_t>(channels),
                            data, static_cast<size_t>(width),
                            static_cast<size_t>(stride),
                            static_cast<size_t>(height), out.data(), &options);
#endif
      break;
    case FpngeIsa::None:
      return false;
  }

  if (n == 0) {
    out.clear();
    return false;
  }
  out.resize(n);
  return true;
}

}  // namespace nanopdf
