// AVX2 instantiation of the fpnge encoder. fpnge.cc is SIMD-abstracted via
// MM()/MMSI() macros: with __AVX2__ defined it emits the 256-bit code path.
// The single exported symbol FPNGEEncode is renamed so this object and the
// SSE4.1 object can coexist; fpnge_dispatch.cc picks one at runtime.
// Built with -mavx2 -mbmi2 -mpclmul (see CMakeLists.txt).
#define FPNGEEncode fpnge_encode_avx2
#include "fpnge.cc"
