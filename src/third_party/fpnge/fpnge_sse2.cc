// Pure-SSE2 instantiation of the fpnge encoder. FPNGE_FORCE_SSE2 selects the
// 128-bit code path with SSE2 emulations (scalar PSHUFB etc.) from
// fpnge_sse2_compat.h, so this object runs on any x86-64 CPU (SSE2 baseline)
// with no SSSE3/SSE4.1/AVX2 dependency. Built with only -msse2.
#define FPNGE_FORCE_SSE2
#define FPNGEEncode fpnge_encode_sse2
#include "fpnge.cc"
