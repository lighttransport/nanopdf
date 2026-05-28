#!/usr/bin/env bash
set -u

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS_DIR="${NANOPDF_LIGHTVG_STRESS_DIR:-/tmp/nanopdf-lightvg-stress-pdfs}"
BUILD_DIR="${NANOPDF_LIGHTVG_PROFILE_BUILD:-/tmp/nanopdf-lightvg-profile-build}"
RASTERIZE_BUILD_DIR="${NANOPDF_LIGHTVG_RASTERIZE_BUILD:-/tmp/nanopdf-lightvg-rasterize-build}"
OUT_DIR="${NANOPDF_LIGHTVG_PROFILE_OUT:-/tmp/nanopdf-lightvg-profile-output}"
DPI="${NANOPDF_LIGHTVG_PROFILE_DPI:-150}"
PAGE="${NANOPDF_LIGHTVG_PROFILE_PAGE:-1}"

mkdir -p "${STRESS_DIR}" "${BUILD_DIR}" "${RASTERIZE_BUILD_DIR}" "${OUT_DIR}"

download_pdf() {
  local name="$1"
  local url="$2"
  local path="${STRESS_DIR}/${name}.pdf"
  if [ -s "${path}" ]; then
    return 0
  fi
  echo "Downloading ${name}"
  curl -L -f -o "${path}" "${url}"
}

download_pdf "pdfjs-22060-plans" "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/22060_A1_01_Plans.pdf"
download_pdf "pdfjs-images" "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/images.pdf"
download_pdf "pdfjs-function-shading" "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/function_based_shading.pdf"
download_pdf "pdfjs-gradientfill" "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/gradientfill.pdf"
download_pdf "pdfjs-highlights" "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/highlights.pdf"
download_pdf "pdfjs-tracemonkey" "https://raw.githubusercontent.com/mozilla/pdf.js/master/web/compressed.tracemonkey-pldi-09.pdf"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNANOPDF_USE_LIGHTVG=ON \
  -DNANOPDF_USE_THORVG=OFF \
  -DNANOPDF_USE_BLEND2D=OFF \
  -DNANOPDF_BUILD_TESTS=OFF
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target nanopdf

cmake -S "${PROJECT_DIR}/examples/rasterize" -B "${RASTERIZE_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNANOPDF_BUILD_DIR="${BUILD_DIR}" \
  -DUSE_LIGHTVG=ON \
  -DUSE_THORVG=OFF
cmake --build "${RASTERIZE_BUILD_DIR}" -j"$(nproc)" --target rasterize

RASTERIZE="${RASTERIZE_BUILD_DIR}/rasterize"
CSV="${OUT_DIR}/lightvg-profile.csv"
LOG="${OUT_DIR}/lightvg-profile.log"

printf "case,elapsed_s,user_s,sys_s,maxrss_kb,exit_code\n" > "${CSV}"
: > "${LOG}"

run_case() {
  local name="$1"
  local input="${STRESS_DIR}/${name}.pdf"
  local output="${OUT_DIR}/${name}.tga"
  local status=0

  echo "Profiling ${name}"
  /usr/bin/time -f "${name},%e,%U,%S,%M,%x" -o "${CSV}" -a \
    "${RASTERIZE}" "${input}" "${output}" \
      --backend lightvg \
      --dpi "${DPI}" \
      --page "${PAGE}" \
      --format tga \
      --log-level 1 >> "${LOG}" 2>&1 || status=$?

  if [ "${status}" -ne 0 ]; then
    echo "  ${name} failed with exit code ${status}; see ${LOG}"
  fi
}

run_case "pdfjs-22060-plans"
run_case "pdfjs-images"
run_case "pdfjs-function-shading"
run_case "pdfjs-gradientfill"
run_case "pdfjs-highlights"
run_case "pdfjs-tracemonkey"

if [ "${NANOPDF_LIGHTVG_PERF_STAT:-0}" = "1" ] && command -v perf >/dev/null 2>&1; then
  PERF_LOG="${OUT_DIR}/lightvg-perf-stat.log"
  : > "${PERF_LOG}"
  for name in pdfjs-images pdfjs-function-shading pdfjs-tracemonkey; do
    echo "perf stat ${name}" >> "${PERF_LOG}"
    perf stat -d "${RASTERIZE}" "${STRESS_DIR}/${name}.pdf" "${OUT_DIR}/${name}.perf.tga" \
      --backend lightvg \
      --dpi "${DPI}" \
      --page "${PAGE}" \
      --format tga \
      --log-level 1 >> "${PERF_LOG}" 2>&1 || true
  done
fi

echo "CSV: ${CSV}"
echo "Log: ${LOG}"
