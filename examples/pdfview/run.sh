#!/usr/bin/env bash
# Build nanopdf (if needed) + the pdfview example, then optionally run it.
#
#   ./run.sh                 # build, then run with no document
#   ./run.sh document.pdf    # build, then open document.pdf
#   NANOPDF_BUILD_DIR=... ./run.sh   # use an existing nanopdf build dir
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
jobs="$(nproc 2>/dev/null || echo 4)"

nanopdf_build="${NANOPDF_BUILD_DIR:-$root/build}"
if [[ ! -f "$nanopdf_build/libnanopdf.a" ]]; then
  echo ">> building nanopdf into $nanopdf_build"
  cmake -S "$root" -B "$nanopdf_build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$nanopdf_build" --target nanopdf -j"$jobs"
fi

echo ">> building pdfview"
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release \
      -DNANOPDF_BUILD_DIR="$nanopdf_build"
cmake --build "$here/build" -j"$jobs"

echo ">> running pdfview"
exec "$here/build/pdfview" "$@"
