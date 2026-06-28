#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RASTERIZE="${NANOPDF_RASTERIZE_BIN:-${ROOT_DIR}/build-rasterize/rasterize}"
OUT_DIR="${NANOPDF_RASTER_BENCH_OUT:-/tmp/nanopdf-raster-bench}"
DPI="${NANOPDF_RASTER_BENCH_DPI:-300}"
FORMAT="${NANOPDF_RASTER_BENCH_FORMAT:-png}"
LOG_LEVEL="${NANOPDF_RASTER_BENCH_LOG_LEVEL:-1}"
FULL="${NANOPDF_RASTER_BENCH_FULL:-0}"

mkdir -p "${OUT_DIR}"

if [ ! -x "${RASTERIZE}" ]; then
  echo "error: rasterize binary not found or not executable: ${RASTERIZE}" >&2
  echo "build it with: cmake --build ${ROOT_DIR}/build-rasterize --target rasterize" >&2
  exit 1
fi

CSV="${OUT_DIR}/raster-optimization.csv"
LOG="${OUT_DIR}/raster-optimization.log"

printf "case,pdf,page,dpi,format,elapsed_s,user_s,sys_s,maxrss_kb,exit_code\n" > "${CSV}"
: > "${LOG}"

run_case() {
  local name="$1"
  local pdf="$2"
  local page="$3"
  local input="${ROOT_DIR}/${pdf}"
  local output="${OUT_DIR}/${name}.${FORMAT}"

  if [ ! -f "${input}" ]; then
    echo "error: missing input: ${input}" >&2
    exit 1
  fi

  echo "Benchmark ${name}"
  /usr/bin/time -f "${name},${pdf},${page},${DPI},${FORMAT},%e,%U,%S,%M,%x" \
    -o "${CSV}" -a \
    "${RASTERIZE}" "${input}" "${output}" \
      --backend lightvg \
      --page "${page}" \
      --dpi "${DPI}" \
      --format "${FORMAT}" \
      --log-level "${LOG_LEVEL}" >> "${LOG}" 2>&1
}

run_all() {
  local name="$1"
  local pdf="$2"
  local input="${ROOT_DIR}/${pdf}"
  local output="${OUT_DIR}/${name}.${FORMAT}"

  if [ ! -f "${input}" ]; then
    echo "error: missing input: ${input}" >&2
    exit 1
  fi

  echo "Benchmark ${name}"
  /usr/bin/time -f "${name},${pdf},all,${DPI},${FORMAT},%e,%U,%S,%M,%x" \
    -o "${CSV}" -a \
    "${RASTERIZE}" "${input}" "${output}" \
      --backend lightvg \
      --all \
      --dpi "${DPI}" \
      --format "${FORMAT}" \
      --log-level "${LOG_LEVEL}" >> "${LOG}" 2>&1
}

run_case "20260626h017350011-page1" "data/20260626h017350011.pdf" 1
run_case "dcsdd-page8" "data/dcsdd.pdf" 8
run_case "a64fx-page1" "data/A64FX_Microarchitecture_Manual_en_1.8.1.pdf" 1
run_case "a64fx-page20" "data/A64FX_Microarchitecture_Manual_en_1.8.1.pdf" 20
run_case "a64fx-page100" "data/A64FX_Microarchitecture_Manual_en_1.8.1.pdf" 100

if [ "${FULL}" = "1" ]; then
  run_all "dcsdd-all" "data/dcsdd.pdf"
  run_all "a64fx-all" "data/A64FX_Microarchitecture_Manual_en_1.8.1.pdf"
fi

echo "CSV: ${CSV}"
echo "Log: ${LOG}"
