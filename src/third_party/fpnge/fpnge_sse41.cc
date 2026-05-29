// SSE4.1 instantiation of the fpnge encoder. With __SSE4_1__ (and not
// __AVX2__) defined, fpnge.cc emits the 128-bit code path. fpnge's non-AVX2
// path relies on PSHUFB (SSSE3) and SSE4.1 ops, so this is the lowest ISA the
// algorithm supports — a pure SSE2 build is not feasible without gutting the
// Huffman table lookups. Built with -msse4.1 -mssse3 -mpclmul.
#define FPNGEEncode fpnge_encode_sse41
#include "fpnge.cc"
