// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Limited JPEG2000 (JPX) decoder for nanopdf
// Supports common PDF use cases:
// - Single-tile images
// - 5/3 reversible wavelet (lossless)
// - 9/7 irreversible wavelet (lossy)
// - 1-4 components (grayscale, RGB, RGBA, CMYK)
// - 8-bit and 16-bit samples

#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace nanopdf {
namespace jpx {

// JPEG2000 marker codes
constexpr uint16_t JPX_SOC = 0xFF4F;  // Start of codestream
constexpr uint16_t JPX_SOT = 0xFF90;  // Start of tile-part
constexpr uint16_t JPX_SOD = 0xFF93;  // Start of data
constexpr uint16_t JPX_EOC = 0xFFD9;  // End of codestream
constexpr uint16_t JPX_SIZ = 0xFF51;  // Image and tile size
constexpr uint16_t JPX_COD = 0xFF52;  // Coding style default
constexpr uint16_t JPX_COC = 0xFF53;  // Coding style component
constexpr uint16_t JPX_QCD = 0xFF5C;  // Quantization default
constexpr uint16_t JPX_QCC = 0xFF5D;  // Quantization component
constexpr uint16_t JPX_RGN = 0xFF5E;  // Region of interest
constexpr uint16_t JPX_POC = 0xFF5F;  // Progression order change
constexpr uint16_t JPX_PPM = 0xFF60;  // Packed packet headers, main
constexpr uint16_t JPX_PPT = 0xFF61;  // Packed packet headers, tile
constexpr uint16_t JPX_TLM = 0xFF55;  // Tile-part lengths
constexpr uint16_t JPX_PLM = 0xFF57;  // Packet length, main
constexpr uint16_t JPX_PLT = 0xFF58;  // Packet length, tile
constexpr uint16_t JPX_CRG = 0xFF63;  // Component registration
constexpr uint16_t JPX_COM = 0xFF64;  // Comment

// Wavelet transform types
enum class WaveletType {
  Reversible_5_3 = 1,   // Lossless
  Irreversible_9_7 = 0  // Lossy
};

// Progression order
enum class ProgressionOrder {
  LRCP = 0,  // Layer-Resolution-Component-Position
  RLCP = 1,  // Resolution-Layer-Component-Position
  RPCL = 2,  // Resolution-Position-Component-Layer
  PCRL = 3,  // Position-Component-Resolution-Layer
  CPRL = 4   // Component-Position-Resolution-Layer
};

// Image/tile size parameters (SIZ marker)
struct SizParams {
  uint16_t rsiz{0};       // Capabilities required
  uint32_t width{0};      // Image width
  uint32_t height{0};     // Image height
  uint32_t x_offset{0};   // Horizontal offset
  uint32_t y_offset{0};   // Vertical offset
  uint32_t tile_width{0}; // Tile width
  uint32_t tile_height{0};// Tile height
  uint32_t tile_x_offset{0};
  uint32_t tile_y_offset{0};
  uint16_t num_components{0};

  struct Component {
    uint8_t bit_depth{8};  // Ssiz: bit depth (+ sign bit info)
    uint8_t x_separation{1};
    uint8_t y_separation{1};
    bool is_signed{false};
  };
  std::vector<Component> components;
};

// Coding style parameters (COD marker)
struct CodParams {
  bool use_sop{false};        // SOP markers present
  bool use_eph{false};        // EPH markers present
  ProgressionOrder prog_order{ProgressionOrder::LRCP};
  uint16_t num_layers{1};
  uint8_t mct{0};             // Multiple component transform (0=none, 1=RCT/ICT)
  uint8_t num_decomp_levels{5};
  uint8_t codeblock_width{4}; // Exponent minus 2 (e.g., 4 means 64)
  uint8_t codeblock_height{4};
  uint8_t codeblock_style{0};
  WaveletType wavelet{WaveletType::Reversible_5_3};
  std::vector<uint8_t> precinct_sizes;  // Per resolution level
};

// Quantization parameters (QCD marker)
struct QcdParams {
  uint8_t quant_style{0};  // Quantization style
  uint8_t num_guard_bits{0};

  struct StepSize {
    uint16_t mantissa{0};
    uint8_t exponent{0};
  };
  std::vector<StepSize> step_sizes;
};

// Tile part header
struct TilePartHeader {
  uint16_t tile_index{0};
  uint8_t tile_part_index{0};
  uint8_t num_tile_parts{0};
  uint32_t tile_part_length{0};
};

// Decode result
struct DecodeResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> pixels;  // Output pixel data
  uint32_t width{0};
  uint32_t height{0};
  uint16_t num_components{0};
  uint8_t bit_depth{8};
};

// Main decoder class
class JPXDecoder {
public:
  JPXDecoder();
  ~JPXDecoder();

  // Decode JPEG2000 codestream
  DecodeResult decode(const uint8_t* data, size_t size);

  // Get image info without full decode
  bool get_info(const uint8_t* data, size_t size,
                uint32_t& width, uint32_t& height,
                uint16_t& num_components, uint8_t& bit_depth);

private:
  // Bit reader for entropy decoding
  class BitReader {
  public:
    BitReader(const uint8_t* data, size_t size);
    uint32_t read_bits(int n);
    bool read_bit();
    void align_to_byte();
    bool eof() const;
    size_t position() const { return pos_; }
    void seek(size_t pos) { pos_ = pos; bit_pos_ = 0; }

  private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_{0};
    int bit_pos_{0};
  };

  // MQ arithmetic decoder for entropy coding
  class MQDecoder {
  public:
    MQDecoder();
    void init(const uint8_t* data, size_t size);
    int decode(int cx);
    void reset();

  private:
    void bytein();
    int mps_exchange(int cx);
    int lps_exchange(int cx);
    void renormd();

    const uint8_t* data_{nullptr};
    size_t size_{0};
    size_t pos_{0};
    uint32_t c_{0};   // C register
    uint16_t a_{0};   // A register (interval)
    int ct_{0};       // Counter
    uint8_t cx_states_[19];  // Context states
    uint8_t cx_mps_[19];     // MPS for each context
  };

  // Parse main header markers
  bool parse_main_header(BitReader& reader);
  bool parse_siz(BitReader& reader, uint16_t length);
  bool parse_cod(BitReader& reader, uint16_t length);
  bool parse_qcd(BitReader& reader, uint16_t length);

  // Parse tile-part
  bool parse_tile_part(BitReader& reader);

  // Decode tile data
  bool decode_tile(const uint8_t* data, size_t size, int tile_idx);

  // Decode codeblock
  bool decode_codeblock(const uint8_t* data, size_t size,
                        int width, int height,
                        std::vector<int32_t>& coeffs);

  // Inverse wavelet transform
  void inverse_dwt_53(std::vector<int32_t>& data, int width, int height, int levels);
  void inverse_dwt_97(std::vector<float>& data, int width, int height, int levels);

  // Apply multiple component transform (color transform)
  void apply_inverse_mct(std::vector<std::vector<int32_t>>& components);

  // Convert coefficients to pixels
  void coeffs_to_pixels(const std::vector<std::vector<int32_t>>& components,
                        std::vector<uint8_t>& pixels);

  SizParams siz_;
  CodParams cod_;
  QcdParams qcd_;
  std::vector<TilePartHeader> tile_parts_;

  // Tile data storage
  std::vector<std::vector<int32_t>> tile_coeffs_;
};

}  // namespace jpx
}  // namespace nanopdf
