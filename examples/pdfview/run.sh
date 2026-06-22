#!/usr/bin/env bash
# Build nanopdf + the pdfview example, then optionally run it.
#
#   ./run.sh                 # build, then run with no document
#   ./run.sh document.pdf    # build, then open document.pdf
#   ./run.sh --mcp file.pdf  # build, then run with the MCP server (port 3001)
#   NANOPDF_BUILD_DIR=... ./run.sh   # link an existing nanopdf build dir as-is
#
# The viewer only links nanopdf's LightVG backend, so the nanopdf library it
# links must be built WITHOUT ThorVG (otherwise the link fails on tvg:: symbols).
# By default this script builds a dedicated LightVG-only library in
# build-pdfview/ so it never collides with a ThorVG-enabled build/ you may have.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
jobs="$(nproc 2>/dev/null || echo 4)"

if [[ -n "${NANOPDF_BUILD_DIR:-}" ]]; then
  # Caller-provided nanopdf build: use it as-is (must be a LightVG build).
  nanopdf_build="$NANOPDF_BUILD_DIR"
  if [[ ! -f "$nanopdf_build/libnanopdf.a" ]]; then
    echo ">> building nanopdf into $nanopdf_build"
    cmake -S "$root" -B "$nanopdf_build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$nanopdf_build" --target nanopdf -j"$jobs"
  fi
else
  # Dedicated LightVG-only nanopdf build for the viewer (keeps in sync with the
  # current source; ThorVG OFF avoids the tvg:: link error).
  nanopdf_build="$root/build-pdfview"
  echo ">> building nanopdf (LightVG-only) into $nanopdf_build"
  cmake -S "$root" -B "$nanopdf_build" -DCMAKE_BUILD_TYPE=Release \
        -DNANOPDF_USE_THORVG=OFF -DNANOPDF_BUILD_TESTS=OFF
  cmake --build "$nanopdf_build" --target nanopdf -j"$jobs"
fi

echo ">> building pdfview"
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release \
      -DNANOPDF_BUILD_DIR="$nanopdf_build"
cmake --build "$here/build" -j"$jobs"

echo ">> running pdfview ($here/build/pdfview)"
exec "$here/build/pdfview" "$@"
