#!/bin/bash

# Script to install ThorVG library
# ThorVG is a lightweight vector graphics library

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_DIR="$PROJECT_ROOT/dist"

echo "Installing ThorVG to $INSTALL_DIR..."

# Check if we're on Linux
if [[ "$OSTYPE" != "linux-gnu"* ]]; then
    echo "This script is designed for Linux. For other platforms, please install ThorVG manually."
    echo "Visit: https://github.com/thorvg/thorvg"
    exit 1
fi

# Create install directory
mkdir -p "$INSTALL_DIR"

# Create a temporary directory
TEMP_DIR=$(mktemp -d)
cd "$TEMP_DIR"

# Clone ThorVG repository
echo "Cloning ThorVG repository..."
git clone https://github.com/thorvg/thorvg.git
cd thorvg

# Build ThorVG using Meson
echo "Building ThorVG..."
meson setup build --prefix="$INSTALL_DIR"
ninja -C build

# Install to local dist directory (no sudo required)
echo "Installing ThorVG to $INSTALL_DIR..."
ninja -C build install

# Clean up
cd /
rm -rf "$TEMP_DIR"

echo "ThorVG has been installed successfully to $INSTALL_DIR!"
echo "You can now build nanopdf with ThorVG support using:"
echo "  cmake .. -DNANOPDF_USE_THORVG=ON \\"
echo "    -DCMAKE_PREFIX_PATH=$INSTALL_DIR"
