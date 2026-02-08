#!/bin/bash
# Install MinGW-w64 cross-compilation toolchains on Ubuntu/Debian
#
# Provides:
#   x86_64-w64-mingw32-g++-posix  (win64)
#   i686-w64-mingw32-g++-posix    (win32)
#
# Usage:
#   sudo ./scripts/setup-mingw.sh

set -eu

echo "=== Installing MinGW-w64 cross-compilation packages ==="

apt-get update
apt-get install -y --no-install-recommends \
  gcc-mingw-w64-x86-64 \
  g++-mingw-w64-x86-64 \
  gcc-mingw-w64-i686 \
  g++-mingw-w64-i686 \
  mingw-w64-tools \
  cmake \
  make

echo ""
echo "=== Installed compilers ==="
echo "win64: $(x86_64-w64-mingw32-g++-posix --version | head -1)"
echo "win32: $(i686-w64-mingw32-g++-posix --version | head -1)"
echo ""
echo "=== Usage ==="
echo "  Win64: cmake -B build-mingw-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64-cross.cmake"
echo "  Win32: cmake -B build-mingw-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32-cross.cmake"
