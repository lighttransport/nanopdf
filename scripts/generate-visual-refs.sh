#!/bin/bash
# Generate reference images for visual regression testing using pdftoppm (poppler-utils).
# Usage: ./scripts/generate-visual-refs.sh [output_dir]
#
# Requires: poppler-utils (provides pdftoppm)
# Renders page 1 of each test PDF at 150 DPI to PNG.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DATA_DIR="${PROJECT_DIR}/data"
OUTPUT_DIR="${1:-${DATA_DIR}/visual_refs}"
DPI=150

# Check for pdftoppm
if ! command -v pdftoppm &>/dev/null; then
  echo "Error: pdftoppm not found. Install poppler-utils:" >&2
  echo "  sudo apt install poppler-utils    # Debian/Ubuntu" >&2
  echo "  brew install poppler              # macOS" >&2
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# List of test PDFs (all single-page)
PDF_FILES=(
  blank.pdf
  test_blendmodes.pdf
  test_curves.pdf
  test_image_transform.pdf
  test_linestyles.pdf
  test_softmask.pdf
  test_transforms.pdf
  test_winding.pdf
  # test_cmyk.pdf and the older graphics/clip/dash/pattern/gradient/
  # textmode fixtures are intentionally omitted from the default suite until
  # their PDFs and reference policy are restored together.
)

count=0
for pdf in "${PDF_FILES[@]}"; do
  pdf_path="${DATA_DIR}/${pdf}"
  if [ ! -f "${pdf_path}" ]; then
    echo "SKIP: ${pdf} (not found)"
    continue
  fi

  stem="${pdf%.pdf}"
  output_prefix="${OUTPUT_DIR}/${stem}-page1-150dpi"

  # pdftoppm renders to PPM by default; -png gives PNG output.
  # -f 1 -l 1 renders only page 1.
  # -r sets DPI.
  # pdftoppm appends "-01" to the prefix for single-page output with -f/-l.
  pdftoppm -png -f 1 -l 1 -r "${DPI}" "${pdf_path}" "${output_prefix}"

  # pdftoppm creates <prefix>-01.png; rename to <prefix>.png
  if [ -f "${output_prefix}-01.png" ]; then
    mv "${output_prefix}-01.png" "${output_prefix}.png"
  elif [ -f "${output_prefix}-1.png" ]; then
    mv "${output_prefix}-1.png" "${output_prefix}.png"
  fi

  if [ -f "${output_prefix}.png" ]; then
    echo "OK: ${stem}-page1-150dpi.png"
    count=$((count + 1))
  else
    echo "WARN: ${pdf} - no output generated"
  fi
done

echo ""
echo "Generated ${count} reference images in ${OUTPUT_DIR}"
