// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
#pragma once

#include <vector>
#include <cstdint>
#include <string> // Added for std::string

// Minimal JBIG2 decoder interface for monochrome bitmaps
class JBIG2Decoder {
public:
    // Decodes a JBIG2 bitstream from input data.
    // Returns an empty string on success, or an error message on failure.
    // Output bitmap is 1bpp, row-major, packed (8 pixels per byte).
    std::string decode(const std::vector<uint8_t>& data, int& width, int& height, std::vector<uint8_t>& bitmap);
};

