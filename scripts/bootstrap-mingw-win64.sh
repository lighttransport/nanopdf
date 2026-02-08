#!/bin/bash
# Cross-compile nanopdf for Windows 64-bit using MinGW-w64
#
# Prerequisites:
#   sudo ./scripts/setup-mingw.sh
#
# Usage:
#   ./scripts/bootstrap-mingw-win64.sh
#   cd build-mingw-win64 && make

set -eu

BUILD_DIR=build-mingw-win64

rm -rf "${BUILD_DIR}"
cmake -B "${BUILD_DIR}" -S . \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64-cross.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DNANOPDF_USE_MINIZ=ON \
  -DNANOPDF_BUILD_TESTS=OFF

echo ""
echo "=== Configured for Windows x86_64 ==="
echo "  cd ${BUILD_DIR} && make"
