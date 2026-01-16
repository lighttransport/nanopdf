// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Light Transport Entertainment Inc.
//
// CCITT Group 4 (T.6) Fax Encoder for PDF generation
// Implements ITU-T T.6 two-dimensional coding scheme

#ifndef NANOPDF_CCITT_ENCODER_HH_
#define NANOPDF_CCITT_ENCODER_HH_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nanopdf {
namespace ccitt {

// Huffman code structure
struct HuffmanCode {
  uint16_t code;
  uint8_t bits;
};

// White run length codes (Table 2/T.4)
// Format: {code, bits}
static const HuffmanCode kWhiteTermCodes[64] = {
    {0x35, 8},   // 0
    {0x07, 6},   // 1
    {0x07, 4},   // 2
    {0x08, 4},   // 3
    {0x0B, 4},   // 4
    {0x0C, 4},   // 5
    {0x0E, 4},   // 6
    {0x0F, 4},   // 7
    {0x13, 5},   // 8
    {0x14, 5},   // 9
    {0x07, 5},   // 10
    {0x08, 5},   // 11
    {0x08, 6},   // 12
    {0x03, 6},   // 13
    {0x34, 6},   // 14
    {0x35, 6},   // 15
    {0x2A, 6},   // 16
    {0x2B, 6},   // 17
    {0x27, 7},   // 18
    {0x0C, 7},   // 19
    {0x08, 7},   // 20
    {0x17, 7},   // 21
    {0x03, 7},   // 22
    {0x04, 7},   // 23
    {0x28, 7},   // 24
    {0x2B, 7},   // 25
    {0x13, 7},   // 26
    {0x24, 7},   // 27
    {0x18, 7},   // 28
    {0x02, 8},   // 29
    {0x03, 8},   // 30
    {0x1A, 8},   // 31
    {0x1B, 8},   // 32
    {0x12, 8},   // 33
    {0x13, 8},   // 34
    {0x14, 8},   // 35
    {0x15, 8},   // 36
    {0x16, 8},   // 37
    {0x17, 8},   // 38
    {0x28, 8},   // 39
    {0x29, 8},   // 40
    {0x2A, 8},   // 41
    {0x2B, 8},   // 42
    {0x2C, 8},   // 43
    {0x2D, 8},   // 44
    {0x04, 8},   // 45
    {0x05, 8},   // 46
    {0x0A, 8},   // 47
    {0x0B, 8},   // 48
    {0x52, 8},   // 49
    {0x53, 8},   // 50
    {0x54, 8},   // 51
    {0x55, 8},   // 52
    {0x24, 8},   // 53
    {0x25, 8},   // 54
    {0x58, 8},   // 55
    {0x59, 8},   // 56
    {0x5A, 8},   // 57
    {0x5B, 8},   // 58
    {0x4A, 8},   // 59
    {0x4B, 8},   // 60
    {0x32, 8},   // 61
    {0x33, 8},   // 62
    {0x34, 8},   // 63
};

// White make-up codes (Table 2/T.4)
static const HuffmanCode kWhiteMakeupCodes[40] = {
    {0x1B, 5},   // 64
    {0x12, 5},   // 128
    {0x17, 6},   // 192
    {0x37, 7},   // 256
    {0x36, 8},   // 320
    {0x37, 8},   // 384
    {0x64, 8},   // 448
    {0x65, 8},   // 512
    {0x68, 8},   // 576
    {0x67, 8},   // 640
    {0xCC, 9},   // 704
    {0xCD, 9},   // 768
    {0xD2, 9},   // 832
    {0xD3, 9},   // 896
    {0xD4, 9},   // 960
    {0xD5, 9},   // 1024
    {0xD6, 9},   // 1088
    {0xD7, 9},   // 1152
    {0xD8, 9},   // 1216
    {0xD9, 9},   // 1280
    {0xDA, 9},   // 1344
    {0xDB, 9},   // 1408
    {0x98, 9},   // 1472
    {0x99, 9},   // 1536
    {0x9A, 9},   // 1600
    {0x18, 6},   // 1664
    {0x9B, 9},   // 1728
    // Extended make-up codes (common to both colors)
    {0x08, 11},  // 1792
    {0x0C, 11},  // 1856
    {0x0D, 11},  // 1920
    {0x12, 12},  // 1984
    {0x13, 12},  // 2048
    {0x14, 12},  // 2112
    {0x15, 12},  // 2176
    {0x16, 12},  // 2240
    {0x17, 12},  // 2304
    {0x1C, 12},  // 2368
    {0x1D, 12},  // 2432
    {0x1E, 12},  // 2496
    {0x1F, 12},  // 2560
};

// Black terminating codes (Table 3/T.4)
static const HuffmanCode kBlackTermCodes[64] = {
    {0x37, 10},  // 0
    {0x02, 3},   // 1
    {0x03, 2},   // 2
    {0x02, 2},   // 3
    {0x03, 3},   // 4
    {0x03, 4},   // 5
    {0x02, 4},   // 6
    {0x03, 5},   // 7
    {0x05, 6},   // 8
    {0x04, 6},   // 9
    {0x04, 7},   // 10
    {0x05, 7},   // 11
    {0x07, 7},   // 12
    {0x04, 8},   // 13
    {0x07, 8},   // 14
    {0x18, 9},   // 15
    {0x17, 10},  // 16
    {0x18, 10},  // 17
    {0x08, 10},  // 18
    {0x67, 11},  // 19
    {0x68, 11},  // 20
    {0x6C, 11},  // 21
    {0x37, 11},  // 22
    {0x28, 11},  // 23
    {0x17, 11},  // 24
    {0x18, 11},  // 25
    {0xCA, 12},  // 26
    {0xCB, 12},  // 27
    {0xCC, 12},  // 28
    {0xCD, 12},  // 29
    {0x68, 12},  // 30
    {0x69, 12},  // 31
    {0x6A, 12},  // 32
    {0x6B, 12},  // 33
    {0xD2, 12},  // 34
    {0xD3, 12},  // 35
    {0xD4, 12},  // 36
    {0xD5, 12},  // 37
    {0xD6, 12},  // 38
    {0xD7, 12},  // 39
    {0x6C, 12},  // 40
    {0x6D, 12},  // 41
    {0xDA, 12},  // 42
    {0xDB, 12},  // 43
    {0x54, 12},  // 44
    {0x55, 12},  // 45
    {0x56, 12},  // 46
    {0x57, 12},  // 47
    {0x64, 12},  // 48
    {0x65, 12},  // 49
    {0x52, 12},  // 50
    {0x53, 12},  // 51
    {0x24, 12},  // 52
    {0x37, 12},  // 53
    {0x38, 12},  // 54
    {0x27, 12},  // 55
    {0x28, 12},  // 56
    {0x58, 12},  // 57
    {0x59, 12},  // 58
    {0x2B, 12},  // 59
    {0x2C, 12},  // 60
    {0x5A, 12},  // 61
    {0x66, 12},  // 62
    {0x67, 12},  // 63
};

// Black make-up codes (Table 3/T.4)
static const HuffmanCode kBlackMakeupCodes[40] = {
    {0x0F, 10},  // 64
    {0xC8, 12},  // 128
    {0xC9, 12},  // 192
    {0x5B, 12},  // 256
    {0x33, 12},  // 320
    {0x34, 12},  // 384
    {0x35, 12},  // 448
    {0x6C, 13},  // 512
    {0x6D, 13},  // 576
    {0x4A, 13},  // 640
    {0x4B, 13},  // 704
    {0x4C, 13},  // 768
    {0x4D, 13},  // 832
    {0x72, 13},  // 896
    {0x73, 13},  // 960
    {0x74, 13},  // 1024
    {0x75, 13},  // 1088
    {0x76, 13},  // 1152
    {0x77, 13},  // 1216
    {0x52, 13},  // 1280
    {0x53, 13},  // 1344
    {0x54, 13},  // 1408
    {0x55, 13},  // 1472
    {0x5A, 13},  // 1536
    {0x5B, 13},  // 1600
    {0x64, 13},  // 1664
    {0x65, 13},  // 1728
    // Extended make-up codes (common to both colors)
    {0x08, 11},  // 1792
    {0x0C, 11},  // 1856
    {0x0D, 11},  // 1920
    {0x12, 12},  // 1984
    {0x13, 12},  // 2048
    {0x14, 12},  // 2112
    {0x15, 12},  // 2176
    {0x16, 12},  // 2240
    {0x17, 12},  // 2304
    {0x1C, 12},  // 2368
    {0x1D, 12},  // 2432
    {0x1E, 12},  // 2496
    {0x1F, 12},  // 2560
};

// 2D mode codes for Group 4 (Table 1/T.6)
// Pass mode: 0001
// Horizontal mode: 001
// Vertical V(0): 1
// Vertical VR(1): 011
// Vertical VL(1): 010
// Vertical VR(2): 000011
// Vertical VL(2): 000010
// Vertical VR(3): 0000011
// Vertical VL(3): 0000010

// Bit writer class
class BitWriter {
 public:
  BitWriter() : bit_pos_(0), current_byte_(0) {}

  void write_bits(uint32_t value, int num_bits) {
    for (int i = num_bits - 1; i >= 0; --i) {
      current_byte_ = (current_byte_ << 1) | ((value >> i) & 1);
      bit_pos_++;
      if (bit_pos_ == 8) {
        data_.push_back(current_byte_);
        current_byte_ = 0;
        bit_pos_ = 0;
      }
    }
  }

  void flush() {
    if (bit_pos_ > 0) {
      current_byte_ <<= (8 - bit_pos_);
      data_.push_back(current_byte_);
      current_byte_ = 0;
      bit_pos_ = 0;
    }
  }

  const std::vector<uint8_t>& data() const { return data_; }
  std::vector<uint8_t>& data() { return data_; }

 private:
  std::vector<uint8_t> data_;
  int bit_pos_;
  uint8_t current_byte_;
};

// Get pixel value (0=white, 1=black)
inline bool get_pixel(const uint8_t* data, int width, int x) {
  if (x < 0 || x >= width) return false;  // White outside bounds
  int byte_idx = x / 8;
  int bit_idx = 7 - (x % 8);
  return (data[byte_idx] >> bit_idx) & 1;
}

// Find the next changing element position from start_pos
// Returns the position where the color changes from the current color
inline int find_changing_element(const uint8_t* data, int width, int start_pos,
                                 bool current_color) {
  for (int x = start_pos; x < width; ++x) {
    if (get_pixel(data, width, x) != current_color) {
      return x;
    }
  }
  return width;
}

// Find b1 and b2 positions for encoding
// b1: first changing element in reference line to right of a0 with opposite color
// b2: next changing element after b1
inline void find_b1_b2(const uint8_t* ref_line, int width, int a0,
                       bool a0_color, int* b1, int* b2) {
  // Start searching from a0+1
  int search_start = (a0 < 0) ? 0 : a0 + 1;

  // Find first pixel in reference line at or after search_start
  // that has opposite color to a0_color
  bool ref_color_at_start =
      (search_start < width) ? get_pixel(ref_line, width, search_start) : false;

  if (ref_color_at_start != a0_color) {
    // Reference line has opposite color at search position
    *b1 = search_start;
  } else {
    // Find where reference changes to opposite color
    *b1 = find_changing_element(ref_line, width, search_start, ref_color_at_start);
    // Then find where it changes back
    if (*b1 < width) {
      bool color_at_b1 = get_pixel(ref_line, width, *b1);
      if (color_at_b1 == a0_color) {
        // Need to find next change
        *b1 = find_changing_element(ref_line, width, *b1, color_at_b1);
      }
    }
  }

  if (*b1 >= width) {
    *b1 = width;
    *b2 = width;
    return;
  }

  // b2 is the next changing element after b1
  bool color_at_b1 = get_pixel(ref_line, width, *b1);
  *b2 = find_changing_element(ref_line, width, *b1 + 1, color_at_b1);
  if (*b2 > width) *b2 = width;
}

// Encode a run length for white or black
inline void encode_run_length(BitWriter& writer, int run_len, bool is_black) {
  const HuffmanCode* term_codes = is_black ? kBlackTermCodes : kWhiteTermCodes;
  const HuffmanCode* makeup_codes =
      is_black ? kBlackMakeupCodes : kWhiteMakeupCodes;

  // Output make-up codes for runs >= 64
  while (run_len >= 64) {
    int makeup_idx;
    if (run_len >= 2560) {
      makeup_idx = 39;  // 2560
      run_len -= 2560;
    } else if (run_len >= 1792) {
      // Extended codes (1792-2560)
      makeup_idx = 27 + (run_len - 1792) / 64;
      run_len = run_len % 64;
    } else {
      // Standard makeup codes (64-1728)
      makeup_idx = (run_len / 64) - 1;
      run_len = run_len % 64;
    }
    writer.write_bits(makeup_codes[makeup_idx].code,
                      makeup_codes[makeup_idx].bits);
  }

  // Output terminating code
  writer.write_bits(term_codes[run_len].code, term_codes[run_len].bits);
}

// Encode Pass mode (0001)
inline void encode_pass_mode(BitWriter& writer) { writer.write_bits(0x1, 4); }

// Encode Horizontal mode (001 + run codes)
inline void encode_horizontal_mode(BitWriter& writer, int a0a1, int a1a2,
                                   bool a0_color) {
  writer.write_bits(0x1, 3);  // 001
  encode_run_length(writer, a0a1, !a0_color);  // First run (a0 color)
  encode_run_length(writer, a1a2, a0_color);   // Second run (opposite color)
}

// Encode Vertical mode
inline void encode_vertical_mode(BitWriter& writer, int delta) {
  switch (delta) {
    case 0:
      writer.write_bits(0x1, 1);  // V(0): 1
      break;
    case 1:
      writer.write_bits(0x3, 3);  // VR(1): 011
      break;
    case -1:
      writer.write_bits(0x2, 3);  // VL(1): 010
      break;
    case 2:
      writer.write_bits(0x3, 6);  // VR(2): 000011
      break;
    case -2:
      writer.write_bits(0x2, 6);  // VL(2): 000010
      break;
    case 3:
      writer.write_bits(0x3, 7);  // VR(3): 0000011
      break;
    case -3:
      writer.write_bits(0x2, 7);  // VL(3): 0000010
      break;
  }
}

// Encode EOFB (End Of Facsimile Block) - two consecutive EOL codes
inline void encode_eofb(BitWriter& writer) {
  // EOFB = 000000000001 000000000001
  writer.write_bits(0x001, 12);
  writer.write_bits(0x001, 12);
}

// Main encoding function for CCITT Group 4 (T.6)
// Input: 1-bit packed image data (MSB first, 0=white, 1=black)
// Returns: encoded CCITT Group 4 data
inline bool encode_ccitt_g4(const uint8_t* image_data, int width, int height,
                            std::vector<uint8_t>& output,
                            std::string* error = nullptr) {
  if (width <= 0 || height <= 0) {
    if (error) *error = "Invalid image dimensions";
    return false;
  }

  int pitch = (width + 7) / 8;
  BitWriter writer;

  // Reference line (initially all white)
  std::vector<uint8_t> ref_line(pitch, 0x00);

  for (int row = 0; row < height; ++row) {
    const uint8_t* cur_line = image_data + row * pitch;

    int a0 = -1;
    bool a0_color = true;  // white

    while (a0 < width) {
      // Find a1: first changing element on current line to right of a0
      int a1;
      if (a0 < 0) {
        // Check if first pixel is black
        if (get_pixel(cur_line, width, 0)) {
          a1 = 0;
        } else {
          a1 = find_changing_element(cur_line, width, 0, false);
        }
      } else {
        a1 = find_changing_element(cur_line, width, a0 + 1,
                                   get_pixel(cur_line, width, a0));
      }
      if (a1 > width) a1 = width;

      // Find b1 and b2 on reference line
      int b1, b2;
      find_b1_b2(ref_line.data(), width, a0, a0_color, &b1, &b2);

      // Decide which mode to use
      if (b2 < a1) {
        // Pass mode: b2 lies to the left of a1
        encode_pass_mode(writer);
        a0 = b2;
        // a0_color stays the same
      } else {
        int delta = a1 - b1;
        if (delta >= -3 && delta <= 3) {
          // Vertical mode
          encode_vertical_mode(writer, delta);
          a0 = a1;
          a0_color = !a0_color;
        } else {
          // Horizontal mode
          // Find a2: next changing element after a1
          int a2 = find_changing_element(cur_line, width, a1 + 1,
                                         get_pixel(cur_line, width, a1));
          if (a2 > width) a2 = width;

          int a0a1 = a1 - (a0 < 0 ? 0 : a0);
          int a1a2 = a2 - a1;
          encode_horizontal_mode(writer, a0a1, a1a2, a0_color);
          a0 = a2;
          // a0_color stays the same (we encoded both runs)
        }
      }

      if (a0 >= width) break;
    }

    // Copy current line to reference line for next row
    std::memcpy(ref_line.data(), cur_line, pitch);
  }

  // Write EOFB
  encode_eofb(writer);

  writer.flush();
  output = std::move(writer.data());

  return true;
}

// Convert grayscale/RGB image to 1-bit monochrome using threshold
inline std::vector<uint8_t> convert_to_monochrome(const uint8_t* data,
                                                   int width, int height,
                                                   int channels,
                                                   uint8_t threshold = 128) {
  int pitch = (width + 7) / 8;
  std::vector<uint8_t> mono(pitch * height, 0);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* pixel = data + (y * width + x) * channels;
      uint8_t gray;
      if (channels >= 3) {
        // RGB to grayscale
        gray = static_cast<uint8_t>(0.299f * pixel[0] + 0.587f * pixel[1] +
                                    0.114f * pixel[2]);
      } else {
        gray = pixel[0];
      }

      // Black if below threshold (1-bit: 1=black, 0=white)
      if (gray < threshold) {
        int byte_idx = y * pitch + x / 8;
        int bit_idx = 7 - (x % 8);
        mono[byte_idx] |= (1 << bit_idx);
      }
    }
  }

  return mono;
}

}  // namespace ccitt
}  // namespace nanopdf

#endif  // NANOPDF_CCITT_ENCODER_HH_
