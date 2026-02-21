#!/bin/bash
# Cross-compile nanopdf for Windows 64-bit using llvm-mingw
#
# Prerequisites:
#   Download llvm-mingw and extract to ~/local/llvm-mingw-20251216-ucrt-ubuntu-22.04-x86_64
#   Or set LLVM_MINGW_DIR environment variable to the install path.
#
# Usage:
#   ./scripts/bootstrap-llvm-mingw-win64.sh
#   cd build-llvm-mingw-win64 && make

set -eu

BUILD_DIR=build-llvm-mingw-win64

rm -rf "${BUILD_DIR}"
cmake -B "${BUILD_DIR}" -S . \
  -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-cross.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DNANOPDF_USE_MINIZ=ON \
  -DNANOPDF_BUILD_TESTS=OFF

echo ""
echo "=== Configured for Windows x86_64 (llvm-mingw) ==="
echo "  cd ${BUILD_DIR} && make"
