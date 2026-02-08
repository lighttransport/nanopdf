#!/bin/bash
# Cross-compile nanopdf for Windows 32-bit using MinGW-w64
#
# Prerequisites:
#   sudo ./scripts/setup-mingw.sh
#
# Usage:
#   ./scripts/bootstrap-mingw-win32.sh
#   cd build-mingw-win32 && make

set -eu

BUILD_DIR=build-mingw-win32

rm -rf "${BUILD_DIR}"
cmake -B "${BUILD_DIR}" -S . \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32-cross.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DNANOPDF_USE_MINIZ=ON \
  -DNANOPDF_BUILD_TESTS=OFF

echo ""
echo "=== Configured for Windows i686 ==="
echo "  cd ${BUILD_DIR} && make"
