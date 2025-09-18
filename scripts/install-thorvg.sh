#!/bin/bash

# Script to install ThorVG library
# ThorVG is a lightweight vector graphics library

set -e

echo "Installing ThorVG..."

# Check if we're on Linux
if [[ "$OSTYPE" != "linux-gnu"* ]]; then
    echo "This script is designed for Linux. For other platforms, please install ThorVG manually."
    echo "Visit: https://github.com/thorvg/thorvg"
    exit 1
fi

# Create a temporary directory
TEMP_DIR=$(mktemp -d)
cd "$TEMP_DIR"

# Clone ThorVG repository
echo "Cloning ThorVG repository..."
git clone https://github.com/thorvg/thorvg.git
cd thorvg

# Build ThorVG using Meson
echo "Building ThorVG..."
meson setup build --prefix=/usr/local
ninja -C build

# Install (requires sudo)
echo "Installing ThorVG (requires sudo)..."
sudo ninja -C build install

# Update library cache
sudo ldconfig

# Clean up
cd /
rm -rf "$TEMP_DIR"

echo "ThorVG has been installed successfully!"
echo "You can now build nanopdf with ThorVG support using:"
echo "  cmake .. -DNANOPDF_USE_THORVG=ON"