#!/bin/bash

# Build script for the rasterize example
# This script builds the rasterize example with ThorVG support

set -e

echo "Building nanopdf rasterize example..."
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if we're in the right directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Check for ThorVG in dist/ directory
NANOPDF_ROOT="$SCRIPT_DIR/../.."
THORVG_DIST_PKGCONFIG="$NANOPDF_ROOT/dist/lib/x86_64-linux-gnu/pkgconfig"
if [ -d "$THORVG_DIST_PKGCONFIG" ]; then
    export PKG_CONFIG_PATH="$THORVG_DIST_PKGCONFIG:$PKG_CONFIG_PATH"
    echo "Using ThorVG from dist/ directory"
fi

# Check if nanopdf is built
NANOPDF_BUILD_DIR="../../build"
if [ ! -f "$NANOPDF_BUILD_DIR/libnanopdf.a" ]; then
    echo -e "${YELLOW}Warning: nanopdf library not found.${NC}"
    echo "Building nanopdf first..."

    cd ../..
    if [ ! -d "build" ]; then
        mkdir build
    fi
    cd build

    # Build with ThorVG support if available (check dist/ or system)
    if [ -f "$NANOPDF_ROOT/dist/include/thorvg-1/thorvg.h" ] || pkg-config --exists thorvg 2>/dev/null; then
        echo "ThorVG found, building with ThorVG support..."
        cmake .. -DNANOPDF_USE_THORVG=ON
    else
        echo -e "${YELLOW}ThorVG not found, building without ThorVG support.${NC}"
        echo "To enable ThorVG support, install ThorVG and rebuild."
        cmake ..
    fi

    make -j$(nproc)
    cd "$SCRIPT_DIR"
fi

# Create build directory for example
if [ ! -d "build" ]; then
    mkdir build
fi

cd build

# Configure with CMake
echo "Configuring rasterize example..."
cmake .. > /dev/null 2>&1

# Build
echo "Building rasterize..."
make -j$(nproc)

if [ -f "rasterize" ]; then
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo ""
    echo "Executable created: build/rasterize"
    echo ""

    # Check if ThorVG is available
    if ldd rasterize | grep -q thorvg; then
        echo -e "${GREEN}✓ ThorVG support is enabled${NC}"
    else
        echo -e "${YELLOW}⚠ ThorVG support is not enabled${NC}"
        echo "  To enable ThorVG rendering:"
        echo "  1. Install ThorVG to dist/: ../../scripts/install-thorvg.sh"
        echo "     (or install system-wide)"
        echo "  2. Rebuild nanopdf with -DNANOPDF_USE_THORVG=ON"
        echo "  3. Run this script again"
    fi

    echo ""
    echo "Usage examples:"
    echo "  ./build/rasterize input.pdf output.png"
    echo "  ./build/rasterize input.pdf output.png -p 2 -w 1024"
    echo "  ./build/rasterize input.pdf output.png --all --dpi 150"
    echo ""
    echo "Run './build/rasterize --help' for more options"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi