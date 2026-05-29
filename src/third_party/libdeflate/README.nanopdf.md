# libdeflate (vendored, decompress subset)

From https://github.com/ebiggers/libdeflate (MIT, see COPYING).

Only the DEFLATE/zlib **decompression** path is compiled into nanopdf to
accelerate PDF FlateDecode streams (see CMakeLists.txt: NANOPDF_USE_LIBDEFLATE).
Compiled TUs: lib/zlib_decompress.c, lib/deflate_decompress.c, lib/adler32.c,
lib/utils.c, lib/x86/cpu_features.c, lib/arm/cpu_features.c. The full lib/ tree
(headers + arch impls) is kept so libdeflate's internal runtime ISA dispatch and
relative includes resolve unchanged. Compression sources are intentionally not
built. Used as a fast path with the existing miniz/zlib inflate as fallback.
