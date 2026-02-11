#!/bin/bash

# Bootstrap script for building nanopdf with Emscripten
# Assumes emcmake and emcc are already available in PATH

set -e

# Get the script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Building nanopdf with Emscripten...${NC}"

# Check if emcmake is available
if ! command -v emcmake &> /dev/null; then
    echo -e "${RED}Error: emcmake not found. Please ensure Emscripten SDK is installed and activated.${NC}"
    echo "Visit https://emscripten.org/docs/getting_started/downloads.html for installation instructions."
    exit 1
fi

# Check if emcc is available
if ! command -v emcc &> /dev/null; then
    echo -e "${RED}Error: emcc not found. Please ensure Emscripten SDK is installed and activated.${NC}"
    exit 1
fi

# Display Emscripten version
echo -e "${YELLOW}Emscripten version:${NC}"
emcc --version

# Create build directory
BUILD_DIR="$PROJECT_ROOT/build_wasm"
if [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Build directory exists. Cleaning...${NC}"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake using Emscripten toolchain
echo -e "${GREEN}Configuring with CMake...${NC}"

# Emscripten-specific CMake options
CMAKE_OPTIONS=(
    -DCMAKE_BUILD_TYPE=Release
    -DNANOPDF_USE_CCACHE=OFF
    -DNANOPDF_BUILD_TESTS=OFF
    -DNANOPDF_BUILD_WASM=ON
    -DNANOPDF_USE_STB_TRUETYPE=ON
    -DNANOPDF_USE_NANOSTL=OFF
)

# All Emscripten-specific flags are handled in CMakeLists.txt
# Do NOT set CXXFLAGS/LDFLAGS here as they conflict with CMake's settings

# Run CMake with Emscripten toolchain
emcmake cmake "${CMAKE_OPTIONS[@]}" ..

echo -e "${GREEN}CMake configuration complete.${NC}"
echo ""
echo -e "${GREEN}Build directory: ${BUILD_DIR}${NC}"
echo -e "${GREEN}To build the project, run:${NC}"
echo "  cd $BUILD_DIR"
echo "  emmake make -j$(nproc)"
echo ""
echo -e "${YELLOW}For JavaScript/Web usage:${NC}"
echo "  The output will be in .wasm and .js format"
echo "  Include the generated .js file in your HTML/Node.js application"
echo ""
echo -e "${YELLOW}Example HTML usage:${NC}"
echo '  <script type="module">'
echo '    import Module from "./nanopdf.js";'
echo '    const module = await Module();'
echo '    // Use module functions here'
echo '  </script>'
echo ""
echo -e "${GREEN}To run the nanopdfjs demo:${NC}"
echo "  cd $PROJECT_ROOT/examples/nanopdfjs"
echo "  python3 -m http.server 8000"
echo "  # Then open http://localhost:8000 in your browser"