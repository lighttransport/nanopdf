#!/bin/bash

# Build script for pdfsign example
# Run this after building nanopdf library in the main build directory

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Check if nanopdf library exists
if [ ! -f "../../build/libnanopdf.a" ]; then
    echo "Error: nanopdf library not found."
    echo "Please build nanopdf first:"
    echo "  cd ../.. && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

# Create build directory
mkdir -p build
cd build

# Configure and build
cmake ..
make -j$(nproc)

echo ""
echo "Build successful!"
echo ""
echo "Usage:"
echo "  ./build/pdfsign info <input.pdf>              # Show signature info"
echo "  ./build/pdfsign verify <input.pdf>            # Verify signatures"
echo "  ./build/pdfsign sign <in.pdf> <out.pdf> ...   # Sign a PDF"
echo ""
echo "Run './build/pdfsign --help' for more options."
