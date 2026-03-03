#!/bin/bash

# Script to build and install ThorVG library
# ThorVG is a lightweight vector graphics library

set -e

THORVG_VERSION="${1:-v1.0.1}"

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_DIR="$PROJECT_ROOT/third_party/thorvg_install"

echo "Installing ThorVG $THORVG_VERSION to $INSTALL_DIR..."

# Check if we're on Linux
if [[ "$OSTYPE" != "linux-gnu"* ]]; then
    echo "This script is designed for Linux. For other platforms, please install ThorVG manually."
    echo "Visit: https://github.com/thorvg/thorvg"
    exit 1
fi

# Check dependencies
for cmd in meson ninja git; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "Error: '$cmd' not found. Please install it first."
        exit 1
    fi
done

# Create a temporary directory
TEMP_DIR=$(mktemp -d)
trap "rm -rf '$TEMP_DIR'" EXIT

cd "$TEMP_DIR"

# Clone ThorVG repository at specified version
echo "Cloning ThorVG $THORVG_VERSION..."
git clone --depth=1 --branch "$THORVG_VERSION" https://github.com/thorvg/thorvg.git
cd thorvg

# Build ThorVG using Meson
echo "Configuring ThorVG build..."
meson setup build \
    --prefix="$INSTALL_DIR" \
    --wipe \
    -Dengines=sw \
    -Dloaders=svg,lottie,png,jpg \
    -Dsavers= \
    -Dbindings= \
    -Ddefault_library=static \
    -Dbuildtype=release

echo "Building ThorVG..."
ninja -C build -j"$(nproc)"

echo "Installing ThorVG to $INSTALL_DIR..."
ninja -C build install

echo ""
echo "ThorVG $THORVG_VERSION installed successfully to $INSTALL_DIR"
echo ""
echo "Build nanopdf with ThorVG support:"
echo "  mkdir build && cd build"
echo "  cmake .. -DNANOPDF_USE_THORVG=ON"
echo "  make"
