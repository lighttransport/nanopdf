// SSE2-only emulations of the SSSE3 / SSE4.1 / AVX2 intrinsics that fpnge's
// 128-bit code path relies on. Including this (with only <emmintrin.h> pulled
// in, so the real higher-ISA intrinsics are NOT declared) lets fpnge.cc compile
// and run on a CPU that has nothing beyond SSE2.
//
// Each helper has a distinct fpnge_sse2_* name; trailing function-like macros
// remap the intrinsic spellings used by fpnge.cc (directly or via its MM()
// token-paste macro) onto these helpers. Define order matters: the helper
// definitions come first so they are not themselves macro-substituted.
//
// PSHUFB and the byte/word horizontal/saturating ops are emulated with scalar
// loops over a 16-byte spill buffer (exactly the "scalar PSHUFB" approach):
// correct but slower than the native instruction, which is the point of having
// this path only as the last-resort tier below AVX2 and SSE4.1.
#ifndef FPNGE_SSE2_COMPAT_H
#define FPNGE_SSE2_COMPAT_H

#include <emmintrin.h>
#include <stdint.h>

namespace fpnge_sse2 {

// --- SSSE3: PSHUFB (scalar) -------------------------------------------------
// dst[i] = (idx[i] & 0x80) ? 0 : src[idx[i] & 0x0F]
static inline __m128i shuffle_epi8(__m128i src, __m128i idx) {
  alignas(16) uint8_t s[16], n[16], r[16];
  _mm_store_si128(reinterpret_cast<__m128i *>(s), src);
  _mm_store_si128(reinterpret_cast<__m128i *>(n), idx);
  for (int i = 0; i < 16; i++)
    r[i] = (n[i] & 0x80) ? 0 : s[n[i] & 0x0F];
  return _mm_load_si128(reinterpret_cast<const __m128i *>(r));
}

// --- SSSE3: MADDUBS (unsigned*signed bytes, add adjacent, saturate s16) -----
static inline __m128i maddubs_epi16(__m128i a, __m128i b) {
  alignas(16) uint8_t ua[16];
  alignas(16) int8_t sb[16];
  alignas(16) int16_t r[8];
  _mm_store_si128(reinterpret_cast<__m128i *>(ua), a);
  _mm_store_si128(reinterpret_cast<__m128i *>(sb), b);
  for (int j = 0; j < 8; j++) {
    int32_t v = int32_t(ua[2 * j]) * int32_t(sb[2 * j]) +
                int32_t(ua[2 * j + 1]) * int32_t(sb[2 * j + 1]);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    r[j] = int16_t(v);
  }
  return _mm_load_si128(reinterpret_cast<const __m128i *>(r));
}

// --- SSSE3: horizontal adds (wrapping) --------------------------------------
static inline __m128i hadd_epi16(__m128i a, __m128i b) {
  alignas(16) int16_t x[8], y[8], r[8];
  _mm_store_si128(reinterpret_cast<__m128i *>(x), a);
  _mm_store_si128(reinterpret_cast<__m128i *>(y), b);
  for (int i = 0; i < 4; i++) {
    r[i] = int16_t(x[2 * i] + x[2 * i + 1]);
    r[i + 4] = int16_t(y[2 * i] + y[2 * i + 1]);
  }
  return _mm_load_si128(reinterpret_cast<const __m128i *>(r));
}

static inline __m128i hadd_epi32(__m128i a, __m128i b) {
  alignas(16) int32_t x[4], y[4], r[4];
  _mm_store_si128(reinterpret_cast<__m128i *>(x), a);
  _mm_store_si128(reinterpret_cast<__m128i *>(y), b);
  r[0] = x[0] + x[1];
  r[1] = x[2] + x[3];
  r[2] = y[0] + y[1];
  r[3] = y[2] + y[3];
  return _mm_load_si128(reinterpret_cast<const __m128i *>(r));
}

// --- SSSE3: abs of signed bytes (vectorized via SSE2) -----------------------
static inline __m128i abs_epi8(__m128i x) {
  __m128i neg = _mm_cmpgt_epi8(_mm_setzero_si128(), x);  // 0xFF where x < 0
  return _mm_sub_epi8(_mm_xor_si128(x, neg), neg);       // (x ^ neg) - neg
}

// --- SSE4.1: per-byte blend on high bit of mask (vectorized via SSE2) -------
static inline __m128i blendv_epi8(__m128i a, __m128i b, __m128i mask) {
  __m128i sel = _mm_cmpgt_epi8(_mm_setzero_si128(), mask);  // high bit -> 0xFF
  return _mm_or_si128(_mm_and_si128(sel, b), _mm_andnot_si128(sel, a));
}

// --- SSE4.1: per-16-bit-lane immediate blend --------------------------------
static inline __m128i blend_epi16(__m128i a, __m128i b, const int imm) {
  alignas(16) int16_t m[8];
  for (int j = 0; j < 8; j++) m[j] = ((imm >> j) & 1) ? int16_t(-1) : 0;
  __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i *>(m));
  return _mm_or_si128(_mm_and_si128(mask, b), _mm_andnot_si128(mask, a));
}

// --- AVX2: per-lane variable shifts (scalar) --------------------------------
static inline __m128i sllv_epi32(__m128i v, __m128i cnt) {
  alignas(16) uint32_t vv[4], cc[4];
  _mm_store_si128(reinterpret_cast<__m128i *>(vv), v);
  _mm_store_si128(reinterpret_cast<__m128i *>(cc), cnt);
  for (int i = 0; i < 4; i++) vv[i] = cc[i] < 32 ? (vv[i] << cc[i]) : 0u;
  return _mm_load_si128(reinterpret_cast<const __m128i *>(vv));
}

static inline __m128i srlv_epi64(__m128i v, __m128i cnt) {
  alignas(16) uint64_t vv[2], cc[2];
  _mm_store_si128(reinterpret_cast<__m128i *>(vv), v);
  _mm_store_si128(reinterpret_cast<__m128i *>(cc), cnt);
  for (int i = 0; i < 2; i++) vv[i] = cc[i] < 64 ? (vv[i] >> cc[i]) : 0ull;
  return _mm_load_si128(reinterpret_cast<const __m128i *>(vv));
}

}  // namespace fpnge_sse2

// Remap the intrinsic spellings onto the SSE2 emulations. Safe because the
// SSE2 TU includes only <emmintrin.h>, so these names are otherwise undeclared.
#define _mm_shuffle_epi8(a, b) ::fpnge_sse2::shuffle_epi8((a), (b))
#define _mm_maddubs_epi16(a, b) ::fpnge_sse2::maddubs_epi16((a), (b))
#define _mm_hadd_epi16(a, b) ::fpnge_sse2::hadd_epi16((a), (b))
#define _mm_hadd_epi32(a, b) ::fpnge_sse2::hadd_epi32((a), (b))
#define _mm_abs_epi8(a) ::fpnge_sse2::abs_epi8((a))
#define _mm_blendv_epi8(a, b, m) ::fpnge_sse2::blendv_epi8((a), (b), (m))
#define _mm_blend_epi16(a, b, imm) ::fpnge_sse2::blend_epi16((a), (b), (imm))
#define _mm_extract_epi32(v, i)                                                \
  _mm_cvtsi128_si32(_mm_shuffle_epi32((v), (i)))
#define _mm_sllv_epi32(v, c) ::fpnge_sse2::sllv_epi32((v), (c))
#define _mm_srlv_epi64(v, c) ::fpnge_sse2::srlv_epi64((v), (c))

#endif  // FPNGE_SSE2_COMPAT_H
