# fpnge (vendored)

Fast PNG encoder by Luca Versari — https://github.com/veluca93/fpnge
Licensed under Apache-2.0 (see headers in fpnge.cc / fpnge.h).

`fpnge.cc` is compiled twice via thin wrappers so both an AVX2 and an
SSE4.1 code path are produced from the single SIMD-abstracted source:

- `fpnge_avx2.cc`   -> `fpnge_encode_avx2`   (built with -mavx2 -mbmi2 -mpclmul)
- `fpnge_sse41.cc`  -> `fpnge_encode_sse41`  (built with -msse4.1 -mssse3 -mpclmul)

`fpnge_dispatch.{cc,hh}` selects between them at runtime via
__builtin_cpu_supports(). fpnge's non-AVX2 path requires SSE4.1/SSSE3
(it relies on PSHUFB for Huffman table lookups), so a pure-SSE2 build is
not provided; callers fall back to the stb_image_write+miniz encoder when
neither AVX2 nor SSE4.1 is available (or on non-x86 / MSVC).
