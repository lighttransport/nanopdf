#!/bin/bash

# Build script for rasterize example with embedded fonts:
# - PDF Standard 14 substitute fonts
# - CJK fonts (Noto Sans/Serif JP)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NANOPDF_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

NANOPDF_BUILD_DIR="${NANOPDF_ROOT}/build-rasterize-embed-fonts"
RASTERIZE_BUILD_DIR="${SCRIPT_DIR}/build-embed-fonts"

echo "Building nanopdf + rasterize with embedded standard + CJK fonts..."
echo ""
echo "nanopdf build dir : ${NANOPDF_BUILD_DIR}"
echo "rasterize build dir: ${RASTERIZE_BUILD_DIR}"
echo ""

if [ ! -d "${NANOPDF_ROOT}/fonts/noto-sans-jp" ] || [ ! -d "${NANOPDF_ROOT}/fonts/noto-serif-jp" ]; then
  echo "Error: CJK font directories were not found:"
  echo "  ${NANOPDF_ROOT}/fonts/noto-sans-jp"
  echo "  ${NANOPDF_ROOT}/fonts/noto-serif-jp"
  exit 1
fi

# Configure + build nanopdf static library with ThorVG and embedded fonts.
cmake -S "${NANOPDF_ROOT}" -B "${NANOPDF_BUILD_DIR}" \
  -DNANOPDF_USE_THORVG=ON \
  -DNANOPDF_EMBED_FONTS=ON \
  -DNANOPDF_EMBED_CJK_FONTS=ON

cmake --build "${NANOPDF_BUILD_DIR}" -j"$(nproc)"

if [ ! -f "${NANOPDF_BUILD_DIR}/libnanopdf.a" ]; then
  echo "Error: nanopdf library build failed: ${NANOPDF_BUILD_DIR}/libnanopdf.a not found"
  exit 1
fi

# Configure + build rasterize example against the embedded-font nanopdf build.
cmake -S "${SCRIPT_DIR}" -B "${RASTERIZE_BUILD_DIR}" \
  -DNANOPDF_BUILD_DIR="${NANOPDF_BUILD_DIR}" \
  -DEMBED_FONTS=OFF

cmake --build "${RASTERIZE_BUILD_DIR}" -j"$(nproc)"

if [ ! -f "${RASTERIZE_BUILD_DIR}/rasterize" ]; then
  echo "Error: rasterize build failed: ${RASTERIZE_BUILD_DIR}/rasterize not found"
  exit 1
fi

echo ""
echo "Build successful."
echo "Executable:"
echo "  ${RASTERIZE_BUILD_DIR}/rasterize"
echo ""
echo "Example:"
echo "  ${RASTERIZE_BUILD_DIR}/rasterize input.pdf output.png --verbose"
