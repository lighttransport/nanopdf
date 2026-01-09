// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_BLEND2D

#include "blend2d-backend.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// For PNG saving
#include "stb_image_write.h"

// External C variable for PNG compression level
extern "C" int stbi_write_png_compression_level;

namespace nanopdf {

// Helper function to convert glyph name to Unicode codepoint
static uint32_t glyph_name_to_unicode(const std::string& name) {
  // Standard glyph names mapping (subset)
  static const std::map<std::string, uint32_t> glyph_map = {
    {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022},
    {"numbersign", 0x0023}, {"dollar", 0x0024}, {"percent", 0x0025},
    {"ampersand", 0x0026}, {"quotesingle", 0x0027}, {"parenleft", 0x0028},
    {"parenright", 0x0029}, {"asterisk", 0x002A}, {"plus", 0x002B},
    {"comma", 0x002C}, {"hyphen", 0x002D}, {"period", 0x002E},
    {"slash", 0x002F}, {"zero", 0x0030}, {"one", 0x0031},
    {"two", 0x0032}, {"three", 0x0033}, {"four", 0x0034},
    {"five", 0x0035}, {"six", 0x0036}, {"seven", 0x0037},
    {"eight", 0x0038}, {"nine", 0x0039}, {"colon", 0x003A},
    {"semicolon", 0x003B}, {"less", 0x003C}, {"equal", 0x003D},
    {"greater", 0x003E}, {"question", 0x003F}, {"at", 0x0040},
    // Uppercase letters
    {"A", 0x0041}, {"B", 0x0042}, {"C", 0x0043}, {"D", 0x0044},
    {"E", 0x0045}, {"F", 0x0046}, {"G", 0x0047}, {"H", 0x0048},
    {"I", 0x0049}, {"J", 0x004A}, {"K", 0x004B}, {"L", 0x004C},
    {"M", 0x004D}, {"N", 0x004E}, {"O", 0x004F}, {"P", 0x0050},
    {"Q", 0x0051}, {"R", 0x0052}, {"S", 0x0053}, {"T", 0x0054},
    {"U", 0x0055}, {"V", 0x0056}, {"W", 0x0057}, {"X", 0x0058},
    {"Y", 0x0059}, {"Z", 0x005A},
    // Lowercase letters
    {"a", 0x0061}, {"b", 0x0062}, {"c", 0x0063}, {"d", 0x0064},
    {"e", 0x0065}, {"f", 0x0066}, {"g", 0x0067}, {"h", 0x0068},
    {"i", 0x0069}, {"j", 0x006A}, {"k", 0x006B}, {"l", 0x006C},
    {"m", 0x006D}, {"n", 0x006E}, {"o", 0x006F}, {"p", 0x0070},
    {"q", 0x0071}, {"r", 0x0072}, {"s", 0x0073}, {"t", 0x0074},
    {"u", 0x0075}, {"v", 0x0076}, {"w", 0x0077}, {"x", 0x0078},
    {"y", 0x0079}, {"z", 0x007A},
  };

  auto it = glyph_map.find(name);
  if (it != glyph_map.end()) {
    return it->second;
  }

  // Try uniXXXX format
  if (name.length() == 7 && name.substr(0, 3) == "uni") {
    try {
      return static_cast<uint32_t>(std::stoul(name.substr(3), nullptr, 16));
    } catch (...) {
      return 0;
    }
  }

  // Single character name
  if (name.length() == 1) {
    return static_cast<uint32_t>(name[0]);
  }

  return 0;
}

// ICC Profile parsing helpers
enum class ICCColorSpaceType {
  Unknown,
  Gray,
  RGB,
  CMYK,
  Lab
};

struct ICCProfileInfo {
  uint32_t size{0};
  uint32_t version{0};
  ICCColorSpaceType color_space{ICCColorSpaceType::Unknown};
  std::string description;
  bool is_srgb{false};
  bool is_adobe_rgb{false};
};

// Parse ICC profile header to extract basic info
static ICCProfileInfo parse_icc_profile(const std::vector<uint8_t>& data) {
  ICCProfileInfo info;

  if (data.size() < 128) {
    return info;  // ICC header is at least 128 bytes
  }

  // Profile size (bytes 0-3, big-endian)
  info.size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

  // Profile version (bytes 8-11)
  info.version = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

  // Color space signature (bytes 16-19)
  uint32_t cs_sig = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];

  // 'RGB ' = 0x52474220, 'CMYK' = 0x434D594B, 'GRAY' = 0x47524159, 'Lab ' = 0x4C616220
  if (cs_sig == 0x52474220) {
    info.color_space = ICCColorSpaceType::RGB;
  } else if (cs_sig == 0x434D594B) {
    info.color_space = ICCColorSpaceType::CMYK;
  } else if (cs_sig == 0x47524159) {
    info.color_space = ICCColorSpaceType::Gray;
  } else if (cs_sig == 0x4C616220) {
    info.color_space = ICCColorSpaceType::Lab;
  }

  // Check for sRGB by looking at profile description tag
  // Tag table starts at byte 128, first 4 bytes = tag count
  if (data.size() > 132) {
    uint32_t tag_count = (data[128] << 24) | (data[129] << 16) | (data[130] << 8) | data[131];

    // Each tag entry is 12 bytes: signature(4) + offset(4) + size(4)
    for (uint32_t i = 0; i < tag_count && (132 + i * 12 + 11) < data.size(); i++) {
      size_t entry_offset = 132 + i * 12;
      uint32_t tag_sig = (data[entry_offset] << 24) | (data[entry_offset + 1] << 16) |
                         (data[entry_offset + 2] << 8) | data[entry_offset + 3];
      uint32_t tag_offset = (data[entry_offset + 4] << 24) | (data[entry_offset + 5] << 16) |
                            (data[entry_offset + 6] << 8) | data[entry_offset + 7];
      uint32_t tag_size = (data[entry_offset + 8] << 24) | (data[entry_offset + 9] << 16) |
                          (data[entry_offset + 10] << 8) | data[entry_offset + 11];

      // 'desc' = 0x64657363 - profile description
      if (tag_sig == 0x64657363 && tag_offset + tag_size <= data.size()) {
        // Read description (simplified - just check for keywords)
        std::string desc;
        for (size_t j = tag_offset; j < tag_offset + tag_size && j < data.size(); j++) {
          if (data[j] >= 32 && data[j] < 127) {
            desc += static_cast<char>(data[j]);
          }
        }
        info.description = desc;

        // Check for common profile names
        if (desc.find("sRGB") != std::string::npos ||
            desc.find("IEC 61966-2.1") != std::string::npos) {
          info.is_srgb = true;
        } else if (desc.find("Adobe RGB") != std::string::npos) {
          info.is_adobe_rgb = true;
        }
      }
    }
  }

  return info;
}

// Convert ICC-based pixel data to sRGB
// This is a simplified conversion that handles common cases
static void convert_icc_to_srgb(
    const std::vector<uint8_t>& src_data,
    std::vector<uint8_t>& dst_data,
    int width, int height,
    const ColorSpace& color_space) {

  ICCProfileInfo profile_info;
  if (!color_space.icc_profile_data.empty()) {
    profile_info = parse_icc_profile(color_space.icc_profile_data);
  }

  int num_components = color_space.num_components;
  if (num_components == 0) {
    // Guess from ICC profile
    switch (profile_info.color_space) {
      case ICCColorSpaceType::Gray: num_components = 1; break;
      case ICCColorSpaceType::CMYK: num_components = 4; break;
      case ICCColorSpaceType::Lab: num_components = 3; break;
      default: num_components = 3; break;
    }
  }

  dst_data.resize(width * height * 3);  // Output is always RGB

  if (num_components == 1) {
    // Grayscale - simple expansion
    for (int i = 0; i < width * height; i++) {
      uint8_t gray = (i < static_cast<int>(src_data.size())) ? src_data[i] : 0;
      dst_data[i * 3] = gray;
      dst_data[i * 3 + 1] = gray;
      dst_data[i * 3 + 2] = gray;
    }
  } else if (num_components == 3) {
    // RGB profile
    if (profile_info.is_srgb) {
      // sRGB - direct copy
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          dst_data[i * 3] = src_data[src_idx];
          dst_data[i * 3 + 1] = src_data[src_idx + 1];
          dst_data[i * 3 + 2] = src_data[src_idx + 2];
        }
      }
    } else if (profile_info.is_adobe_rgb) {
      // Adobe RGB to sRGB approximate conversion
      // Adobe RGB has gamma 2.2 and wider gamut
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          // Simplified conversion: apply gamut compression
          float r = src_data[src_idx] / 255.0f;
          float g = src_data[src_idx + 1] / 255.0f;
          float b = src_data[src_idx + 2] / 255.0f;

          // Adobe RGB to XYZ (D65)
          float x = 0.5767309f * r + 0.1855540f * g + 0.1881852f * b;
          float y = 0.2973769f * r + 0.6273491f * g + 0.0752741f * b;
          float z = 0.0270343f * r + 0.0706872f * g + 0.9911085f * b;

          // XYZ to sRGB
          float sr = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
          float sg = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
          float sb = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;

          // Clamp and convert to 8-bit
          dst_data[i * 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sr)) * 255);
          dst_data[i * 3 + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sg)) * 255);
          dst_data[i * 3 + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sb)) * 255);
        }
      }
    } else {
      // Unknown RGB profile - assume sRGB-like, direct copy
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          dst_data[i * 3] = src_data[src_idx];
          dst_data[i * 3 + 1] = src_data[src_idx + 1];
          dst_data[i * 3 + 2] = src_data[src_idx + 2];
        }
      }
    }
  } else if (num_components == 4) {
    // CMYK profile - convert to RGB
    for (int i = 0; i < width * height; i++) {
      int src_idx = i * 4;
      if (src_idx + 3 < static_cast<int>(src_data.size())) {
        float c = src_data[src_idx] / 255.0f;
        float m = src_data[src_idx + 1] / 255.0f;
        float y = src_data[src_idx + 2] / 255.0f;
        float k = src_data[src_idx + 3] / 255.0f;

        // CMYK to RGB conversion
        dst_data[i * 3] = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
        dst_data[i * 3 + 1] = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
        dst_data[i * 3 + 2] = static_cast<uint8_t>(255 * (1.0f - y) * (1.0f - k));
      }
    }
  }
}

Blend2DBackend::Blend2DBackend() {}

Blend2DBackend::~Blend2DBackend() {}

bool Blend2DBackend::initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

  // Create image with PRGB32 format (premultiplied RGBA)
  BLResult result = image_.create(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_PRGB32);
  if (result != BL_SUCCESS) {
    return false;
  }

  // Attach context to image
  result = ctx_.begin(image_);
  if (result != BL_SUCCESS) {
    return false;
  }

  // Clear to white
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(0xFFFFFFFF));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  initialized_ = true;
  return true;
}

bool Blend2DBackend::begin_scene() {
  if (!initialized_) return false;

  // Clear to white
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(0xFFFFFFFF));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  return true;
}

bool Blend2DBackend::end_scene() {
  if (!initialized_) return false;
  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);
  return true;
}

bool Blend2DBackend::draw_rectangle(float x, float y, float w, float h,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_rect(x, y, w, h);
  return true;
}

bool Blend2DBackend::draw_circle(float cx, float cy, float radius,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_circle(cx, cy, radius);
  return true;
}

bool Blend2DBackend::draw_path(const BLPath& path,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_path(path);
  return true;
}

bool Blend2DBackend::draw_text(float x, float y, const std::string& text, float size,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  // Draw each character as a glyph
  float cursor_x = x;
  for (char c : text) {
    draw_glyph(static_cast<int>(c), cursor_x, y, size, r, g, b, a);
    // Approximate character width
    cursor_x += size * 0.6f;
  }
  return true;
}

bool Blend2DBackend::draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_stroke_style(BLRgba32(r, g, b, a));
  ctx_.set_stroke_width(stroke_width);
  ctx_.stroke_line(x1, y1, x2, y2);
  return true;
}

Blend2DRenderResult Blend2DBackend::get_buffer() {
  Blend2DRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);

  BLImageData data;
  if (image_.get_data(&data) != BL_SUCCESS) {
    result.error = "Failed to get image data";
    return result;
  }

  result.width = width_;
  result.height = height_;

  // Convert from PRGB32 to RGBA8888
  result.pixels.resize(width_ * height_ * 4);
  const uint8_t* src = static_cast<const uint8_t*>(data.pixel_data);

  for (uint32_t y = 0; y < height_; y++) {
    for (uint32_t x = 0; x < width_; x++) {
      size_t src_idx = y * data.stride + x * 4;
      size_t dst_idx = (y * width_ + x) * 4;

      // PRGB32 is BGRA in memory (little-endian)
      uint8_t b_val = src[src_idx + 0];
      uint8_t g_val = src[src_idx + 1];
      uint8_t r_val = src[src_idx + 2];
      uint8_t a_val = src[src_idx + 3];

      // Convert from premultiplied to straight alpha
      if (a_val > 0) {
        r_val = static_cast<uint8_t>(std::min(255, (r_val * 255) / a_val));
        g_val = static_cast<uint8_t>(std::min(255, (g_val * 255) / a_val));
        b_val = static_cast<uint8_t>(std::min(255, (b_val * 255) / a_val));
      }

      result.pixels[dst_idx + 0] = r_val;
      result.pixels[dst_idx + 1] = g_val;
      result.pixels[dst_idx + 2] = b_val;
      result.pixels[dst_idx + 3] = a_val;
    }
  }

  result.success = true;
  return result;
}

bool Blend2DBackend::save_to_png(const std::string& filename) {
  RenderOptions options;
  options.format = RenderOptions::Format::PNG;
  return save_to_file(filename, options);
}

bool Blend2DBackend::save_to_file(const std::string& filename, const RenderOptions& options) {
  auto result = get_buffer();
  if (!result.success) {
    return false;
  }

  int ret = 0;
  int width = static_cast<int>(result.width);
  int height = static_cast<int>(result.height);
  int stride = width * 4;

  switch (options.format) {
    case RenderOptions::Format::PNG: {
      // Set PNG compression level (stb_image_write uses this global)
      stbi_write_png_compression_level = options.png_compression;
      ret = stbi_write_png(filename.c_str(), width, height, 4,
                           result.pixels.data(), stride);
      break;
    }
    case RenderOptions::Format::JPEG: {
      // JPEG doesn't support alpha, convert RGBA to RGB
      std::vector<uint8_t> rgb_pixels(width * height * 3);
      for (int i = 0; i < width * height; i++) {
        rgb_pixels[i * 3 + 0] = result.pixels[i * 4 + 0];
        rgb_pixels[i * 3 + 1] = result.pixels[i * 4 + 1];
        rgb_pixels[i * 3 + 2] = result.pixels[i * 4 + 2];
      }
      ret = stbi_write_jpg(filename.c_str(), width, height, 3,
                           rgb_pixels.data(), options.jpeg_quality);
      break;
    }
    case RenderOptions::Format::BMP:
      ret = stbi_write_bmp(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
    case RenderOptions::Format::TGA:
      ret = stbi_write_tga(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
  }

  return ret != 0;
}

Blend2DRenderResult Blend2DBackend::render_page(const Pdf& pdf, const Page& page,
                                                 const RenderOptions& options) {
  Blend2DRenderResult result;

  // Get page dimensions
  float page_width = 612.0f;
  float page_height = 792.0f;

  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Calculate scale based on DPI (72 DPI is standard PDF resolution)
  float dpi_scale = options.dpi / 72.0f;

  // Resize canvas if needed for DPI scaling
  uint32_t target_width = static_cast<uint32_t>(page_width * dpi_scale);
  uint32_t target_height = static_cast<uint32_t>(page_height * dpi_scale);

  if (target_width != width_ || target_height != height_) {
    if (!initialize(target_width, target_height)) {
      result.error = "Failed to resize canvas for DPI scaling";
      return result;
    }
  }

  // Calculate scale to fit in canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Initialize graphics state
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;

  current_pdf_ = &pdf;
  current_page_ = &page;

  // Clear canvas with background color
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(options.bg_r, options.bg_g, options.bg_b, options.bg_a));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Process each content stream
  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;

    if (content_obj.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, content_obj.ref_object_number,
                                        content_obj.ref_generation_number);
      if (resolved.success) {
        resolved_obj = resolved.value;
      } else {
        continue;
      }
    }

    if (resolved_obj.type == Value::STREAM) {
      auto decoded_result = decode_stream(pdf, resolved_obj);
      if (decoded_result.success) {
        state_ = GraphicsState();
        state_.page_width = page_width;
        state_.page_height = page_height;
        state_.scale = scale;
        parse_pdf_content(decoded_result.data);
      }
    }
  }

  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

// Helper to determine font category from name
static int get_font_category(const std::string& font_name) {
  // Convert to lowercase for comparison
  std::string lower_name;
  for (char c : font_name) {
    lower_name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  // Check for monospace/typewriter fonts
  if (lower_name.find("courier") != std::string::npos ||
      lower_name.find("mono") != std::string::npos ||
      lower_name.find("typewriter") != std::string::npos ||
      lower_name.find("consol") != std::string::npos ||
      lower_name.find("fixed") != std::string::npos) {
    return 1;  // Monospace
  }

  // Check for serif fonts
  if (lower_name.find("times") != std::string::npos ||
      lower_name.find("serif") != std::string::npos ||
      lower_name.find("roman") != std::string::npos ||
      lower_name.find("garamond") != std::string::npos ||
      lower_name.find("palatino") != std::string::npos ||
      lower_name.find("georgia") != std::string::npos ||
      lower_name.find("cambria") != std::string::npos) {
    return 2;  // Serif
  }

  // Check for symbol/dingbat fonts
  if (lower_name.find("symbol") != std::string::npos ||
      lower_name.find("dingbat") != std::string::npos ||
      lower_name.find("wingding") != std::string::npos ||
      lower_name.find("zapf") != std::string::npos) {
    return 3;  // Symbol
  }

  // Default to sans-serif (most common)
  return 0;  // Sans-serif
}

bool Blend2DBackend::load_fallback_font(const std::string& font_name) {
  // Determine font category for better substitution
  int category = get_font_category(font_name);

  // Font paths organized by category
  // Category 0: Sans-serif, 1: Monospace, 2: Serif, 3: Symbol
  std::vector<std::vector<std::string>> font_paths_by_category = {
    // Sans-serif fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/System/Library/Fonts/SFNSText.ttf",
      "/Library/Fonts/Arial.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
      "C:\\Windows\\Fonts\\segoeui.ttf"
    },
    // Monospace fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
      "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
      "/System/Library/Fonts/Courier.ttc",
      "/System/Library/Fonts/Monaco.ttf",
      "/Library/Fonts/Courier New.ttf",
      "C:\\Windows\\Fonts\\cour.ttf",
      "C:\\Windows\\Fonts\\consola.ttf"
    },
    // Serif fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
      "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
      "/System/Library/Fonts/Times.ttc",
      "/Library/Fonts/Times New Roman.ttf",
      "C:\\Windows\\Fonts\\times.ttf",
      "C:\\Windows\\Fonts\\georgia.ttf"
    },
    // Symbol fonts (fallback to sans if not found)
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/System/Library/Fonts/Symbol.ttf",
      "C:\\Windows\\Fonts\\symbol.ttf"
    }
  };

  // Try fonts in the matching category first
  const auto& preferred_paths = font_paths_by_category[category];
  for (const auto& path : preferred_paths) {
    std::ifstream file(path, std::ios::binary);
    if (file) {
      file.seekg(0, std::ios::end);
      size_t size = file.tellg();
      file.seekg(0, std::ios::beg);

      FontCache& cache = font_cache_[font_name];
      cache.font_data.resize(size);
      file.read(reinterpret_cast<char*>(cache.font_data.data()), size);

      if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), 0)) {
        cache.initialized = true;
#if NANOPDF_DEBUG_PRINT
        printf("[Blend2D] Using fallback font: %s for '%s'\n", path.c_str(), font_name.c_str());
#endif
        return true;
      }
    }
  }

  // If category-specific fonts not found, try sans-serif as ultimate fallback
  if (category != 0) {
    for (const auto& path : font_paths_by_category[0]) {
      std::ifstream file(path, std::ios::binary);
      if (file) {
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        FontCache& cache = font_cache_[font_name];
        cache.font_data.resize(size);
        file.read(reinterpret_cast<char*>(cache.font_data.data()), size);

        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), 0)) {
          cache.initialized = true;
#if NANOPDF_DEBUG_PRINT
          printf("[Blend2D] Using fallback font: %s for '%s'\n", path.c_str(), font_name.c_str());
#endif
          return true;
        }
      }
    }
  }

  return false;
}

bool Blend2DBackend::load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font) {
  // Check if already loaded
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    return true;
  }

  // Try to load embedded font
  if (font && font->descriptor) {
    const FontDescriptor* desc = font->descriptor;

    // Check if font file is available
    if (desc->font_file.type == Value::UNDEFINED ||
        desc->font_file.type == Value::NULL_OBJ) {
      // No embedded font, try fallback
      return load_fallback_font(font_name);
    }

    // Resolve and decode font file stream
    Value font_file_val = desc->font_file;
    if (font_file_val.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, font_file_val.ref_object_number,
                                        font_file_val.ref_generation_number);
      if (!resolved.success) {
        return load_fallback_font(font_name);
      }
      font_file_val = resolved.value;
    }

    if (font_file_val.type == Value::STREAM) {
      auto decoded = decode_stream(pdf, font_file_val);
      if (decoded.success && !decoded.data.empty()) {
        FontCache& cache = font_cache_[font_name];
        cache.font_data = decoded.data;
        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), 0)) {
          cache.initialized = true;
          return true;
        }
      }
    }
  }

  // Load fallback font
  return load_fallback_font(font_name);
}

Blend2DBackend::FontCache* Blend2DBackend::get_font(const std::string& font_name) {
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    return &it->second;
  }
  return nullptr;
}

bool Blend2DBackend::draw_glyph(int codepoint, float x, float y, float size,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to placeholder
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetCodepointShape(&font->font_info, codepoint, &vertices);

  if (num_verts == 0 || !vertices) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  BLPath path;
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;

    switch (v->type) {
      case STBTT_vmove:
        path.move_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        path.line_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        path.quad_to(cx, cy, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        path.cubic_to(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  path.close();

  // Apply text rendering mode
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

  if (invisible) {
    return true;
  }

  if (do_fill) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_path(path);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;
    ctx_.set_stroke_width(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));
    ctx_.stroke_path(path);
  }

  return true;
}

bool Blend2DBackend::draw_glyph_by_index(int glyph_index, float x, float y, float size,
                                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  FontCache* font = get_font(current_font_name_);
  if (!font) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetGlyphShape(&font->font_info, glyph_index, &vertices);

  if (num_verts == 0 || !vertices) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  BLPath path;
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;

    switch (v->type) {
      case STBTT_vmove:
        path.move_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        path.line_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        path.quad_to(cx, cy, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        path.cubic_to(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  path.close();

  // Apply text rendering mode
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

  if (invisible) {
    return true;
  }

  if (do_fill) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_path(path);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;
    ctx_.set_stroke_width(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));
    ctx_.stroke_path(path);
  }

  return true;
}

float Blend2DBackend::calculate_text_width(const std::string& text, float font_size) {
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    return text.length() * font_size * 0.5f;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);
  float width = 0.0f;

  for (size_t i = 0; i < text.length(); i++) {
    int codepoint = static_cast<unsigned char>(text[i]);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->font_info, codepoint, &advance, &lsb);
    width += advance * scale;
  }

  return width;
}

bool Blend2DBackend::draw_image(const ImageXObject& image, float x, float y, float w, float h) {
  if (!initialized_ || image.data.empty()) {
    return false;
  }

  int img_width = image.width;
  int img_height = image.height;

  // Create Blend2D image from decoded data
  BLImage img;
  img.create(img_width, img_height, BL_FORMAT_PRGB32);

  BLImageData img_data;
  img.get_data(&img_data);

  uint8_t* dst = static_cast<uint8_t*>(img_data.pixel_data);
  const uint8_t* src = image.data.data();

  // Handle ICCBased color space with profile conversion
  if (image.color_space.type == ColorSpaceType::ICCBased) {
    std::vector<uint8_t> rgb_data;
    convert_icc_to_srgb(image.data, rgb_data, img_width, img_height, image.color_space);

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        size_t src_idx = (row * img_width + col) * 3;
        size_t dst_idx = row * img_data.stride + col * 4;

        uint8_t r_val = 0, g_val = 0, b_val = 0;
        if (src_idx + 2 < rgb_data.size()) {
          r_val = rgb_data[src_idx];
          g_val = rgb_data[src_idx + 1];
          b_val = rgb_data[src_idx + 2];
        }

        // PRGB32 is BGRA
        dst[dst_idx + 0] = b_val;
        dst[dst_idx + 1] = g_val;
        dst[dst_idx + 2] = r_val;
        dst[dst_idx + 3] = 255;
      }
    }
  } else {
    // Convert image data to PRGB32
    int components = 1;
    if (image.color_space.type == ColorSpaceType::DeviceRGB) components = 3;
    else if (image.color_space.type == ColorSpaceType::DeviceCMYK) components = 4;
    else if (image.color_space.type == ColorSpaceType::DeviceGray) components = 1;

    size_t src_size = image.data.size();
    size_t expected_size = static_cast<size_t>(img_width) * img_height * components;

    // Bounds check - if data is too small, fill with gray
    if (src_size < expected_size) {
#ifdef NANOPDF_DEBUG_PRINT
      printf("DEBUG: Image data size mismatch: got %zu, expected %zu\n", src_size, expected_size);
#endif
      // Fill with gray placeholder
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          size_t dst_idx = row * img_data.stride + col * 4;
          dst[dst_idx + 0] = 200;  // B
          dst[dst_idx + 1] = 200;  // G
          dst[dst_idx + 2] = 200;  // R
          dst[dst_idx + 3] = 255;  // A
        }
      }
    } else {
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          size_t src_idx = (row * img_width + col) * components;
          size_t dst_idx = row * img_data.stride + col * 4;

          uint8_t r_val, g_val, b_val, a_val = 255;

          if (components == 1) {
            r_val = g_val = b_val = src[src_idx];
          } else if (components == 3) {
            r_val = src[src_idx];
            g_val = src[src_idx + 1];
            b_val = src[src_idx + 2];
          } else if (components == 4) {
            // CMYK to RGB
            float c = src[src_idx] / 255.0f;
            float m = src[src_idx + 1] / 255.0f;
            float y_val = src[src_idx + 2] / 255.0f;
            float k = src[src_idx + 3] / 255.0f;
            r_val = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
            g_val = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
            b_val = static_cast<uint8_t>((1.0f - y_val) * (1.0f - k) * 255);
          } else {
            r_val = g_val = b_val = 128;
          }

          // PRGB32 is BGRA
          dst[dst_idx + 0] = b_val;
          dst[dst_idx + 1] = g_val;
          dst[dst_idx + 2] = r_val;
          dst[dst_idx + 3] = a_val;
        }
      }
    }
  }

  // Draw image scaled to fit
  ctx_.save();
  ctx_.translate(x, y);
  ctx_.scale(w / img_width, h / img_height);
  ctx_.blit_image(BLPointI(0, 0), img);
  ctx_.restore();

  return true;
}

// Helper to lookup a resource, checking Form XObject resources first, then page resources
const Value* Blend2DBackend::lookup_resource(const std::string& resource_type, const std::string& name) const {
  // Check Form XObject resources stack (from top to bottom)
  for (auto it = form_resources_stack_.rbegin(); it != form_resources_stack_.rend(); ++it) {
    auto type_it = it->find(resource_type);
    if (type_it != it->end()) {
      Value resolved_dict = type_it->second;

      // Resolve reference if needed
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success) {
          resolved_dict = resolved.value;
        }
      }

      if (resolved_dict.type == Value::DICTIONARY) {
        auto name_it = resolved_dict.dict.find(name);
        if (name_it != resolved_dict.dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  // Fall back to page resources
  if (current_page_) {
    auto type_it = current_page_->resources.find(resource_type);
    if (type_it != current_page_->resources.end()) {
      Value resolved_dict = type_it->second;

      // Resolve reference if needed
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success) {
          resolved_dict = resolved.value;
        }
      }

      if (resolved_dict.type == Value::DICTIONARY) {
        auto name_it = resolved_dict.dict.find(name);
        if (name_it != resolved_dict.dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  return nullptr;
}

bool Blend2DBackend::draw_shading(const std::string& shading_name) {
  if (!current_pdf_ || !current_page_) {
    return false;
  }

  // Look up shading from page resources
  auto shading_dict_it = current_page_->resources.find("Shading");
  if (shading_dict_it == current_page_->resources.end()) {
    return false;
  }

  Dictionary shading_resources;
  if (shading_dict_it->second.type == Value::DICTIONARY) {
    shading_resources = shading_dict_it->second.dict;
  } else if (shading_dict_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      shading_dict_it->second.ref_object_number,
                                      shading_dict_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    shading_resources = resolved.value.dict;
  } else {
    return false;
  }

  auto shading_it = shading_resources.find(shading_name);
  if (shading_it == shading_resources.end()) {
    return false;
  }

  // Resolve shading reference
  Dictionary shading_dict;
  if (shading_it->second.type == Value::DICTIONARY) {
    shading_dict = shading_it->second.dict;
  } else if (shading_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      shading_it->second.ref_object_number,
                                      shading_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    shading_dict = resolved.value.dict;
  } else {
    return false;
  }

  // Parse the shading
  auto shading = parse_shading(*current_pdf_, shading_dict);
  if (!shading) {
    return false;
  }

  // Create gradient based on shading type
  if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
    // Linear gradient
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float x1 = static_cast<float>(shading->coords[2]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[3])) * state_.scale;

    BLGradient gradient(BLLinearGradientValues(x0, y0, x1, y1));

    // Extract colors from function
    if (shading->function.type == Value::DICTIONARY) {
      auto func_type_it = shading->function.dict.find("FunctionType");
      if (func_type_it != shading->function.dict.end() &&
          func_type_it->second.type == Value::NUMBER) {
        int func_type = static_cast<int>(func_type_it->second.number);

        if (func_type == 2) {
          // Exponential interpolation
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          float r0 = 0, g0 = 0, b0 = 0;
          float r1 = 1, g1 = 1, b1 = 1;

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            if (c0_it->second.array.size() >= 3) {
              r0 = static_cast<float>(c0_it->second.array[0].number);
              g0 = static_cast<float>(c0_it->second.array[1].number);
              b0 = static_cast<float>(c0_it->second.array[2].number);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            if (c1_it->second.array.size() >= 3) {
              r1 = static_cast<float>(c1_it->second.array[0].number);
              g1 = static_cast<float>(c1_it->second.array[1].number);
              b1 = static_cast<float>(c1_it->second.array[2].number);
            }
          }

          gradient.add_stop(0.0, BLRgba32(
            static_cast<uint8_t>(r0 * 255),
            static_cast<uint8_t>(g0 * 255),
            static_cast<uint8_t>(b0 * 255),
            255));
          gradient.add_stop(1.0, BLRgba32(
            static_cast<uint8_t>(r1 * 255),
            static_cast<uint8_t>(g1 * 255),
            static_cast<uint8_t>(b1 * 255),
            255));
        }
      }
    }

    // Fill page with gradient
    ctx_.set_fill_style(gradient);
    ctx_.fill_rect(0, 0, width_, height_);
    return true;
  }
  else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
    // Radial gradient
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float r0 = static_cast<float>(shading->coords[2]) * state_.scale;
    float x1 = static_cast<float>(shading->coords[3]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[4])) * state_.scale;
    float r1 = static_cast<float>(shading->coords[5]) * state_.scale;

    BLGradient gradient(BLRadialGradientValues(x1, y1, x0, y0, r1, r0));

    // Extract colors (same as axial)
    if (shading->function.type == Value::DICTIONARY) {
      auto func_type_it = shading->function.dict.find("FunctionType");
      if (func_type_it != shading->function.dict.end() &&
          func_type_it->second.type == Value::NUMBER) {
        int func_type = static_cast<int>(func_type_it->second.number);

        if (func_type == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          float r0_c = 0, g0 = 0, b0 = 0;
          float r1_c = 1, g1 = 1, b1 = 1;

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            if (c0_it->second.array.size() >= 3) {
              r0_c = static_cast<float>(c0_it->second.array[0].number);
              g0 = static_cast<float>(c0_it->second.array[1].number);
              b0 = static_cast<float>(c0_it->second.array[2].number);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            if (c1_it->second.array.size() >= 3) {
              r1_c = static_cast<float>(c1_it->second.array[0].number);
              g1 = static_cast<float>(c1_it->second.array[1].number);
              b1 = static_cast<float>(c1_it->second.array[2].number);
            }
          }

          gradient.add_stop(0.0, BLRgba32(
            static_cast<uint8_t>(r0_c * 255),
            static_cast<uint8_t>(g0 * 255),
            static_cast<uint8_t>(b0 * 255),
            255));
          gradient.add_stop(1.0, BLRgba32(
            static_cast<uint8_t>(r1_c * 255),
            static_cast<uint8_t>(g1 * 255),
            static_cast<uint8_t>(b1 * 255),
            255));
        }
      }
    }

    ctx_.set_fill_style(gradient);
    ctx_.fill_rect(0, 0, width_, height_);
    return true;
  }

  return false;
}

// Helper to check if a path is a simple rectangle
static bool is_rectangular_path(const BLPath& path, BLBox& out_rect) {
  // Get path info
  const BLPathView view = path.view();
  if (view.size < 4) return false;

  // Check for rectangle pattern: moveTo, lineTo, lineTo, lineTo, close (or 4 lines)
  // or moveTo followed by rect primitive
  const uint8_t* cmd = view.command_data;
  const BLPoint* pts = view.vertex_data;

  size_t pt_idx = 0;
  std::vector<BLPoint> corners;

  for (size_t i = 0; i < view.size && corners.size() < 5; i++) {
    switch (cmd[i]) {
      case BL_PATH_CMD_MOVE:
        corners.push_back(pts[pt_idx++]);
        break;
      case BL_PATH_CMD_ON:  // Line to
        corners.push_back(pts[pt_idx++]);
        break;
      case BL_PATH_CMD_CLOSE:
        break;
      default:
        // Has curves or other commands - not a simple rectangle
        return false;
    }
  }

  // Need exactly 4 corners for a rectangle
  if (corners.size() < 4 || corners.size() > 5) return false;

  // Check if it forms an axis-aligned rectangle
  // All x or y coords should match in pairs
  float x_vals[4] = {static_cast<float>(corners[0].x), static_cast<float>(corners[1].x),
                     static_cast<float>(corners[2].x), static_cast<float>(corners[3].x)};
  float y_vals[4] = {static_cast<float>(corners[0].y), static_cast<float>(corners[1].y),
                     static_cast<float>(corners[2].y), static_cast<float>(corners[3].y)};

  // Find min/max
  float min_x = x_vals[0], max_x = x_vals[0];
  float min_y = y_vals[0], max_y = y_vals[0];
  for (int i = 1; i < 4; i++) {
    if (x_vals[i] < min_x) min_x = x_vals[i];
    if (x_vals[i] > max_x) max_x = x_vals[i];
    if (y_vals[i] < min_y) min_y = y_vals[i];
    if (y_vals[i] > max_y) max_y = y_vals[i];
  }

  // Check that each corner is at a min/max combination (axis-aligned)
  int corners_at_edges = 0;
  const float eps = 0.01f;
  for (int i = 0; i < 4; i++) {
    bool at_x_edge = (std::abs(x_vals[i] - min_x) < eps) || (std::abs(x_vals[i] - max_x) < eps);
    bool at_y_edge = (std::abs(y_vals[i] - min_y) < eps) || (std::abs(y_vals[i] - max_y) < eps);
    if (at_x_edge && at_y_edge) corners_at_edges++;
  }

  if (corners_at_edges == 4) {
    out_rect.x0 = min_x;
    out_rect.y0 = min_y;
    out_rect.x1 = max_x;
    out_rect.y1 = max_y;
    return true;
  }

  return false;
}

bool Blend2DBackend::push_with_clip(BLPath& path, bool fill, bool stroke) {
  if (!initialized_) return false;

  // Apply clipping path if set
  if (state_.has_clip && !state_.clip_path.is_empty()) {
    ctx_.save();

    // Check if the clip path is a simple rectangle (common case in PDFs)
    BLBox clip_rect;
    if (is_rectangular_path(state_.clip_path, clip_rect)) {
      // Use precise rectangular clipping
      ctx_.clip_to_rect(clip_rect.x0, clip_rect.y0,
                        clip_rect.x1 - clip_rect.x0, clip_rect.y1 - clip_rect.y0);
    } else {
      // For non-rectangular paths, use bounding box approximation
      // Note: Blend2D doesn't support arbitrary path clipping, so this is
      // an approximation. For precise clipping, consider using ThorVG backend.
      BLBox clip_box;
      state_.clip_path.get_bounding_box(&clip_box);
      ctx_.clip_to_rect(clip_box.x0, clip_box.y0,
                        clip_box.x1 - clip_box.x0, clip_box.y1 - clip_box.y0);
    }
  }

  // Apply blend mode
  ctx_.set_comp_op(static_cast<BLCompOp>(state_.blend_mode));

  if (fill) {
    uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
    ctx_.set_fill_style(BLRgba32(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha));
    ctx_.fill_path(path);
  }

  if (stroke) {
    // Set stroke style
    ctx_.set_stroke_width(state_.stroke_width * state_.scale);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));

    // Line cap
    BLStrokeCap cap = BL_STROKE_CAP_BUTT;
    if (state_.line_cap == 1) cap = BL_STROKE_CAP_ROUND;
    else if (state_.line_cap == 2) cap = BL_STROKE_CAP_SQUARE;
    ctx_.set_stroke_caps(cap);

    // Line join
    BLStrokeJoin join = BL_STROKE_JOIN_MITER_CLIP;
    if (state_.line_join == 1) join = BL_STROKE_JOIN_ROUND;
    else if (state_.line_join == 2) join = BL_STROKE_JOIN_BEVEL;
    ctx_.set_stroke_join(join);
    ctx_.set_stroke_miter_limit(state_.miter_limit);

    // Dash pattern
    if (!state_.dash_pattern.empty()) {
      BLArray<double> dashes;
      for (float d : state_.dash_pattern) {
        dashes.append(static_cast<double>(d * state_.scale));
      }
      ctx_.set_stroke_dash_array(dashes);
      ctx_.set_stroke_dash_offset(static_cast<double>(state_.dash_phase * state_.scale));
    }

    ctx_.stroke_path(path);
  }

  if (state_.has_clip) {
    ctx_.restore();
  }

  return true;
}

Blend2DRenderResult Blend2DBackend::render_page(const Pdf& pdf, const Page& page) {
  Blend2DRenderResult result;

  // Get page dimensions
  float page_width = 612.0f;
  float page_height = 792.0f;

  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Calculate scale to fit in canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Initialize graphics state
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;

  current_pdf_ = &pdf;
  current_page_ = &page;

  // Clear canvas
  begin_scene();

  // Process each content stream
  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;

    // Resolve reference if needed
    if (content_obj.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, content_obj.ref_object_number,
                                        content_obj.ref_generation_number);
      if (resolved.success) {
        resolved_obj = resolved.value;
      } else {
        continue;
      }
    }

    if (resolved_obj.type == Value::STREAM) {
      auto decoded_result = decode_stream(pdf, resolved_obj);
      if (decoded_result.success) {
        state_ = GraphicsState();  // Reset state
        // Set page coordinate info
        state_.page_width = page_width;
        state_.page_height = page_height;
        state_.scale = scale;
        parse_pdf_content(decoded_result.data);
      }
    }
  }

  end_scene();

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

bool Blend2DBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  if (content_data.empty()) return true;

  std::string content(content_data.begin(), content_data.end());
  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;

  size_t pos = 0;
  while (pos < content.length()) {
    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.length()) break;

    // Check for comment
    if (content[pos] == '%') {
      while (pos < content.length() && content[pos] != '\n' && content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    // Parse token
    std::string token;
    bool is_string = false;

    if (content[pos] == '(') {
      // String literal
      is_string = true;
      int paren_depth = 1;
      pos++;
      while (pos < content.length() && paren_depth > 0) {
        if (content[pos] == '\\' && pos + 1 < content.length()) {
          token += content[pos];
          pos++;
          token += content[pos];
          pos++;
        } else if (content[pos] == '(') {
          paren_depth++;
          token += content[pos];
          pos++;
        } else if (content[pos] == ')') {
          paren_depth--;
          if (paren_depth > 0) {
            token += content[pos];
          }
          pos++;
        } else {
          token += content[pos];
          pos++;
        }
      }
    } else if (content[pos] == '<') {
      if (pos + 1 < content.length() && content[pos + 1] == '<') {
        // Dictionary start
        token = "<<";
        pos += 2;
      } else {
        // Hex string
        pos++;
        while (pos < content.length() && content[pos] != '>') {
          if (!std::isspace(static_cast<unsigned char>(content[pos]))) {
            token += content[pos];
          }
          pos++;
        }
        if (pos < content.length()) pos++;
        is_string = true;
      }
    } else if (content[pos] == '>') {
      if (pos + 1 < content.length() && content[pos + 1] == '>') {
        token = ">>";
        pos += 2;
      } else {
        pos++;
      }
    } else if (content[pos] == '[') {
      token = "[";
      pos++;
    } else if (content[pos] == ']') {
      token = "]";
      pos++;
    } else if (content[pos] == '/') {
      // Name
      token = "/";
      pos++;
      while (pos < content.length() &&
             !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/' && content[pos] != '[' &&
             content[pos] != ']' && content[pos] != '(' &&
             content[pos] != ')' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '{' &&
             content[pos] != '}') {
        token += content[pos];
        pos++;
      }
    } else {
      // Number or operator
      while (pos < content.length() &&
             !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/' && content[pos] != '[' &&
             content[pos] != ']' && content[pos] != '(' &&
             content[pos] != ')' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '{' &&
             content[pos] != '}') {
        token += content[pos];
        pos++;
      }
    }

    if (token.empty()) continue;

    // Check if token is an operator or operand
    bool is_operator = false;
    if (!is_string && !token.empty()) {
      char first = token[0];
      if (token == "<<" || token == ">>" || token == "[" || token == "]") {
        // These are structural, add as operands
      } else if ((first >= 'a' && first <= 'z') ||
                 (first >= 'A' && first <= 'Z') ||
                 first == '\'' || first == '"') {
        is_operator = true;
      }
    }

    if (is_operator) {
      // Process operator
      // Graphics state operators
      if (token == "q") {
        state_stack.push_back(state_);
        ctx_.save();
      } else if (token == "Q") {
        if (!state_stack.empty()) {
          state_ = state_stack.back();
          state_stack.pop_back();
          ctx_.restore();
        }
      }
      // Path construction
      else if (token == "m") {  // moveto
        if (operands.size() >= 2) {
          float x = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.move_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
          state_.in_path = true;
        }
      } else if (token == "l") {  // lineto
        if (operands.size() >= 2) {
          float x = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.line_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
        }
      } else if (token == "c") {  // curveto
        if (operands.size() >= 6) {
          float x1 = std::stof(operands[operands.size() - 6]) * state_.scale;
          float y1 = (state_.page_height - std::stof(operands[operands.size() - 5])) * state_.scale;
          float x2 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "v") {  // curveto (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(state_.current_x, state_.current_y, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "y") {  // curveto (last control point = end point)
        if (operands.size() >= 4) {
          float x1 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y1 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x3, y3, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "h") {  // closepath
        state_.current_path.close();
      } else if (token == "re") {  // rectangle
        if (operands.size() >= 4) {
          float x = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float w = std::stof(operands[operands.size() - 2]) * state_.scale;
          float h = std::stof(operands[operands.size() - 1]) * state_.scale;
          // PDF y is bottom-up, adjust for top-down
          y -= h;
          state_.current_path.add_rect(x, y, w, h);
          state_.in_path = true;
        }
      }
      // Path painting
      else if (token == "f" || token == "F" || token == "f*") {
        if (state_.in_path) {
          BLFillRule rule = (token == "f*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, false);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "S") {  // stroke
        if (state_.in_path) {
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "s") {  // close and stroke
        if (state_.in_path) {
          state_.current_path.close();
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "B" || token == "B*") {  // fill and stroke
        if (state_.in_path) {
          BLFillRule rule = (token == "B*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "b" || token == "b*") {  // close, fill and stroke
        if (state_.in_path) {
          state_.current_path.close();
          BLFillRule rule = (token == "b*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "n") {  // end path
        state_.current_path.reset();
        state_.in_path = false;
      }
      // Clipping
      else if (token == "W" || token == "W*") {
        if (state_.in_path) {
          state_.has_clip = true;
          state_.clip_even_odd = (token == "W*");
          state_.clip_path = state_.current_path;
        }
      }
      // Color operators
      else if (token == "g") {  // gray fill
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (token == "G") {  // gray stroke
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (token == "rg") {  // RGB fill
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "RG") {  // RGB stroke
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "k") {  // CMYK fill
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          state_.fill_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.fill_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.fill_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      } else if (token == "K") {  // CMYK stroke
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          state_.stroke_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.stroke_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.stroke_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      }
      // Color space
      else if (token == "cs") {
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.fill_color_space = cs_name;
          state_.fill_pattern.clear();
        }
      } else if (token == "CS") {
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.stroke_color_space = cs_name;
          state_.stroke_pattern.clear();
        }
      } else if (token == "sc" || token == "scn") {
        // Check for pattern
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            state_.fill_pattern = last.substr(1);
            is_pattern = true;
          }
        }
        if (!is_pattern) {
          state_.fill_pattern.clear();
          if (operands.size() >= 3) {
            state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.fill_r = state_.fill_g = state_.fill_b = gray;
          }
        }
      } else if (token == "SC" || token == "SCN") {
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            state_.stroke_pattern = last.substr(1);
            is_pattern = true;
          }
        }
        if (!is_pattern) {
          state_.stroke_pattern.clear();
          if (operands.size() >= 3) {
            state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
          }
        }
      }
      // Line style
      else if (token == "w") {
        if (operands.size() >= 1) {
          state_.stroke_width = std::stof(operands[0]);
        }
      } else if (token == "J") {
        if (operands.size() >= 1) {
          state_.line_cap = std::stoi(operands[0]);
        }
      } else if (token == "j") {
        if (operands.size() >= 1) {
          state_.line_join = std::stoi(operands[0]);
        }
      } else if (token == "M") {
        if (operands.size() >= 1) {
          state_.miter_limit = std::stof(operands[0]);
        }
      } else if (token == "d") {
        state_.dash_pattern.clear();
        state_.dash_phase = 0.0f;
        bool in_array = false;
        for (size_t i = 0; i < operands.size(); i++) {
          if (operands[i] == "[") {
            in_array = true;
          } else if (operands[i] == "]") {
            in_array = false;
          } else if (in_array) {
            state_.dash_pattern.push_back(std::stof(operands[i]));
          } else if (!in_array && i == operands.size() - 1) {
            state_.dash_phase = std::stof(operands[i]);
          }
        }
      }
      // Graphics state dictionary (gs operator)
      else if (token == "gs") {
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string gs_name = operands[0];
          if (!gs_name.empty() && gs_name[0] == '/') {
            gs_name = gs_name.substr(1);
          }
          // Look up ExtGState from page resources
          auto extgstate_it = current_page_->resources.find("ExtGState");
          if (extgstate_it != current_page_->resources.end()) {
            Dictionary extgstate_dict;
            if (extgstate_it->second.type == Value::DICTIONARY) {
              extgstate_dict = extgstate_it->second.dict;
            } else if (extgstate_it->second.type == Value::REFERENCE) {
              auto resolved = resolve_reference(*current_pdf_,
                                                extgstate_it->second.ref_object_number,
                                                extgstate_it->second.ref_generation_number);
              if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                extgstate_dict = resolved.value.dict;
              }
            }
            // Find the specific graphics state
            auto gs_it = extgstate_dict.find(gs_name);
            if (gs_it != extgstate_dict.end()) {
              Dictionary gs_dict;
              if (gs_it->second.type == Value::DICTIONARY) {
                gs_dict = gs_it->second.dict;
              } else if (gs_it->second.type == Value::REFERENCE) {
                auto resolved = resolve_reference(*current_pdf_,
                                                  gs_it->second.ref_object_number,
                                                  gs_it->second.ref_generation_number);
                if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                  gs_dict = resolved.value.dict;
                }
              }
              // Apply graphics state parameters
              // ca - non-stroking alpha (fill)
              auto ca_it = gs_dict.find("ca");
              if (ca_it != gs_dict.end() && ca_it->second.type == Value::NUMBER) {
                state_.fill_opacity = static_cast<float>(ca_it->second.number);
              }
              // CA - stroking alpha
              auto CA_it = gs_dict.find("CA");
              if (CA_it != gs_dict.end() && CA_it->second.type == Value::NUMBER) {
                state_.stroke_opacity = static_cast<float>(CA_it->second.number);
              }
              // BM - blend mode
              auto bm_it = gs_dict.find("BM");
              if (bm_it != gs_dict.end()) {
                std::string bm_name;
                if (bm_it->second.type == Value::NAME) {
                  bm_name = bm_it->second.name;
                } else if (bm_it->second.type == Value::ARRAY && !bm_it->second.array.empty()) {
                  if (bm_it->second.array[0].type == Value::NAME) {
                    bm_name = bm_it->second.array[0].name;
                  }
                }
                // Map PDF blend mode to Blend2D comp op
                if (bm_name == "Normal" || bm_name == "Compatible") {
                  state_.blend_mode = BL_COMP_OP_SRC_OVER;
                } else if (bm_name == "Multiply") {
                  state_.blend_mode = BL_COMP_OP_MULTIPLY;
                } else if (bm_name == "Screen") {
                  state_.blend_mode = BL_COMP_OP_SCREEN;
                } else if (bm_name == "Overlay") {
                  state_.blend_mode = BL_COMP_OP_OVERLAY;
                } else if (bm_name == "Darken") {
                  state_.blend_mode = BL_COMP_OP_DARKEN;
                } else if (bm_name == "Lighten") {
                  state_.blend_mode = BL_COMP_OP_LIGHTEN;
                } else if (bm_name == "ColorDodge") {
                  state_.blend_mode = BL_COMP_OP_COLOR_DODGE;
                } else if (bm_name == "ColorBurn") {
                  state_.blend_mode = BL_COMP_OP_COLOR_BURN;
                } else if (bm_name == "HardLight") {
                  state_.blend_mode = BL_COMP_OP_HARD_LIGHT;
                } else if (bm_name == "SoftLight") {
                  state_.blend_mode = BL_COMP_OP_SOFT_LIGHT;
                } else if (bm_name == "Difference") {
                  state_.blend_mode = BL_COMP_OP_DIFFERENCE;
                } else if (bm_name == "Exclusion") {
                  state_.blend_mode = BL_COMP_OP_EXCLUSION;
                } else {
                  state_.blend_mode = BL_COMP_OP_SRC_OVER;  // Default
                }
                ctx_.set_comp_op(static_cast<BLCompOp>(state_.blend_mode));
              }
              // LW - line width
              auto lw_it = gs_dict.find("LW");
              if (lw_it != gs_dict.end() && lw_it->second.type == Value::NUMBER) {
                state_.stroke_width = static_cast<float>(lw_it->second.number);
              }
              // LC - line cap
              auto lc_it = gs_dict.find("LC");
              if (lc_it != gs_dict.end() && lc_it->second.type == Value::NUMBER) {
                state_.line_cap = static_cast<int>(lc_it->second.number);
              }
              // LJ - line join
              auto lj_it = gs_dict.find("LJ");
              if (lj_it != gs_dict.end() && lj_it->second.type == Value::NUMBER) {
                state_.line_join = static_cast<int>(lj_it->second.number);
              }
              // ML - miter limit
              auto ml_it = gs_dict.find("ML");
              if (ml_it != gs_dict.end() && ml_it->second.type == Value::NUMBER) {
                state_.miter_limit = static_cast<float>(ml_it->second.number);
              }
              // AIS - alpha is shape
              auto ais_it = gs_dict.find("AIS");
              if (ais_it != gs_dict.end() && ais_it->second.type == Value::BOOLEAN) {
                state_.alpha_is_shape = ais_it->second.boolean;
              }
              // TK - text knockout
              auto tk_it = gs_dict.find("TK");
              if (tk_it != gs_dict.end() && tk_it->second.type == Value::BOOLEAN) {
                state_.text_knockout = tk_it->second.boolean;
              }
              // OP - overprint for stroking
              auto op_stroke_it = gs_dict.find("OP");
              if (op_stroke_it != gs_dict.end() && op_stroke_it->second.type == Value::BOOLEAN) {
                state_.overprint_stroke = op_stroke_it->second.boolean;
              }
              // op - overprint for non-stroking (fill)
              auto op_fill_it = gs_dict.find("op");
              if (op_fill_it != gs_dict.end() && op_fill_it->second.type == Value::BOOLEAN) {
                state_.overprint_fill = op_fill_it->second.boolean;
              }
              // OPM - overprint mode
              auto opm_it = gs_dict.find("OPM");
              if (opm_it != gs_dict.end() && opm_it->second.type == Value::NUMBER) {
                state_.overprint_mode = static_cast<int>(opm_it->second.number);
              }
              // FL - flatness tolerance
              auto fl_it = gs_dict.find("FL");
              if (fl_it != gs_dict.end() && fl_it->second.type == Value::NUMBER) {
                state_.flatness = static_cast<float>(fl_it->second.number);
              }
              // SA - stroke adjustment
              auto sa_it = gs_dict.find("SA");
              if (sa_it != gs_dict.end() && sa_it->second.type == Value::BOOLEAN) {
                state_.stroke_adjustment = sa_it->second.boolean;
              }
              // SMask - soft mask
              auto smask_it = gs_dict.find("SMask");
              if (smask_it != gs_dict.end()) {
                if (smask_it->second.type == Value::NAME && smask_it->second.name == "None") {
                  // Clear soft mask
                  state_.has_soft_mask = false;
                  state_.soft_mask_type = 0;
                  state_.soft_mask_data.clear();
                } else if (smask_it->second.type == Value::DICTIONARY ||
                           smask_it->second.type == Value::REFERENCE) {
                  // Parse soft mask dictionary
                  Dictionary smask_dict;
                  if (smask_it->second.type == Value::DICTIONARY) {
                    smask_dict = smask_it->second.dict;
                  } else {
                    auto resolved = resolve_reference(*current_pdf_,
                                                      smask_it->second.ref_object_number,
                                                      smask_it->second.ref_generation_number);
                    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                      smask_dict = resolved.value.dict;
                    }
                  }
                  if (!smask_dict.empty()) {
                    state_.has_soft_mask = true;
                    // Get soft mask type (S entry)
                    auto s_it = smask_dict.find("S");
                    if (s_it != smask_dict.end() && s_it->second.type == Value::NAME) {
                      if (s_it->second.name == "Alpha") {
                        state_.soft_mask_type = 1;
                      } else if (s_it->second.name == "Luminosity") {
                        state_.soft_mask_type = 2;
                      }
                    }
                    // Get and render the G (transparency group) XObject
                    auto g_it = smask_dict.find("G");
                    if (g_it != smask_dict.end()) {
                      render_soft_mask_group(g_it->second, state_.soft_mask_type);
                    }
                  }
                }
              }
            }
          }
        }
      }
      // Text operators
      else if (token == "BT") {
        state_.in_text_block = true;
        state_.text_matrix.reset();
        state_.text_line_matrix.reset();
      } else if (token == "ET") {
        state_.in_text_block = false;
      } else if (token == "Tf") {
        if (operands.size() >= 2) {
          std::string font_name = operands[0];
          if (!font_name.empty() && font_name[0] == '/') {
            font_name = font_name.substr(1);
          }
          state_.font_size = std::stof(operands[operands.size() - 1]);
          current_font_name_ = font_name;
          current_font_ = nullptr;
          if (current_pdf_ && current_page_) {
            auto font_it = current_page_->fonts.find(font_name);
            if (font_it != current_page_->fonts.end()) {
              current_font_ = font_it->second.get();
              load_font(*current_pdf_, font_name, current_font_);
            }
          }
        }
      } else if (token == "Td") {
        if (operands.size() >= 2) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          state_.text_matrix.e += tx;
          state_.text_matrix.f += ty;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "TD") {
        if (operands.size() >= 2) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          state_.text_leading = -ty;
          state_.text_matrix.e += tx;
          state_.text_matrix.f += ty;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "Tm") {
        if (operands.size() >= 6) {
          state_.text_matrix.a = std::stof(operands[0]);
          state_.text_matrix.b = std::stof(operands[1]);
          state_.text_matrix.c = std::stof(operands[2]);
          state_.text_matrix.d = std::stof(operands[3]);
          state_.text_matrix.e = std::stof(operands[4]);
          state_.text_matrix.f = std::stof(operands[5]);
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "T*") {
        state_.text_matrix.e = state_.text_line_matrix.e;
        state_.text_matrix.f = state_.text_line_matrix.f - state_.text_leading;
        state_.text_line_matrix = state_.text_matrix;
      } else if (token == "TL") {
        if (operands.size() >= 1) {
          state_.text_leading = std::stof(operands[0]);
        }
      } else if (token == "Tc") {
        if (operands.size() >= 1) {
          state_.char_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tw") {
        if (operands.size() >= 1) {
          state_.word_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tz") {
        if (operands.size() >= 1) {
          state_.horiz_scaling = std::stof(operands[0]);
        }
      } else if (token == "Ts") {
        if (operands.size() >= 1) {
          state_.text_rise = static_cast<int>(std::stof(operands[0]));
        }
      } else if (token == "Tr") {
        if (operands.size() >= 1) {
          state_.text_render_mode = std::stoi(operands[0]);
        }
      } else if (token == "Tj") {
        // Show text
        if (!operands.empty() && state_.in_text_block) {
          std::string text = operands[0];
          float x = state_.text_matrix.e * state_.scale;
          float y = (state_.page_height - state_.text_matrix.f) * state_.scale;
          float font_size = state_.font_size * state_.scale;

          uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);

          for (size_t i = 0; i < text.length(); i++) {
            int codepoint = static_cast<unsigned char>(text[i]);
            draw_glyph(codepoint, x, y, font_size,
                      state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
            x += font_size * 0.6f;
          }

          state_.text_matrix.e += text.length() * state_.font_size * 0.6f;
        }
      } else if (token == "TJ") {
        // Show text with positioning
        if (state_.in_text_block) {
          float x = state_.text_matrix.e * state_.scale;
          float y = (state_.page_height - state_.text_matrix.f) * state_.scale;
          float font_size = state_.font_size * state_.scale;
          uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);

          for (const auto& op : operands) {
            if (op == "[" || op == "]") continue;

            try {
              float adjust = std::stof(op);
              x -= (adjust / 1000.0f) * font_size;
            } catch (...) {
              for (size_t i = 0; i < op.length(); i++) {
                int codepoint = static_cast<unsigned char>(op[i]);
                draw_glyph(codepoint, x, y, font_size,
                          state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
                x += font_size * 0.6f;
              }
            }
          }
        }
      }
      // Shading
      else if (token == "sh") {
        if (operands.size() >= 1) {
          std::string shading_name = operands[0];
          if (!shading_name.empty() && shading_name[0] == '/') {
            shading_name = shading_name.substr(1);
          }
          draw_shading(shading_name);
        }
      }
      // Inline image (BI ... ID data EI)
      else if (token == "BI") {
        parse_inline_image(content, pos);
        operands.clear();
        continue;  // Skip normal operand clearing
      }
      // XObject (Do operator for images/forms)
      else if (token == "Do") {
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string xobj_name = operands[0];
          if (!xobj_name.empty() && xobj_name[0] == '/') {
            xobj_name = xobj_name.substr(1);
          }

          // Look up XObject in page resources
          auto xobj_it = current_page_->resources.find("XObject");
          if (xobj_it != current_page_->resources.end()) {
            Dictionary xobj_dict;
            if (xobj_it->second.type == Value::REFERENCE) {
              auto resolved = resolve_reference(*current_pdf_,
                  xobj_it->second.ref_object_number,
                  xobj_it->second.ref_generation_number);
              if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                xobj_dict = resolved.value.dict;
              }
            } else if (xobj_it->second.type == Value::DICTIONARY) {
              xobj_dict = xobj_it->second.dict;
            }

            auto entry_it = xobj_dict.find(xobj_name);
            if (entry_it != xobj_dict.end()) {
              Value xobj_value;
              if (entry_it->second.type == Value::REFERENCE) {
                auto resolved = resolve_reference(*current_pdf_,
                    entry_it->second.ref_object_number,
                    entry_it->second.ref_generation_number);
                if (resolved.success) {
                  xobj_value = std::move(resolved.value);
                }
              } else {
                xobj_value = entry_it->second;
              }

              if (xobj_value.type == Value::STREAM) {
                auto subtype_it = xobj_value.stream.dict.find("Subtype");
                if (subtype_it != xobj_value.stream.dict.end() &&
                    subtype_it->second.type == Value::NAME) {
                  if (subtype_it->second.name == "Image") {
                    ImageXObject image = parse_image_xobject(*current_pdf_, xobj_value);
                    float img_x = state_.transform.e * state_.scale;
                    float img_y = state_.transform.f;
                    float img_width = state_.transform.a * state_.scale;
                    float img_height = state_.transform.d * state_.scale;
                    if (img_height < 0) {
                      img_y += img_height;
                      img_height = -img_height;
                    }
                    img_y = (state_.page_height - img_y) * state_.scale - img_height;
                    draw_image(image, img_x, img_y, img_width, img_height);
                  } else if (subtype_it->second.name == "Form") {
                    // Form XObject - decode and parse its content stream
                    auto decoded = decode_stream(*current_pdf_, xobj_value);
                    if (decoded.success && !decoded.data.empty()) {
                      GraphicsState saved_state = state_;

                      // Check for Form's Resources and push onto stack
                      auto resources_it = xobj_value.stream.dict.find("Resources");
                      bool has_form_resources = false;
                      if (resources_it != xobj_value.stream.dict.end()) {
                        Value form_resources = resources_it->second;
                        // Resolve reference if needed
                        if (form_resources.type == Value::REFERENCE) {
                          auto resolved = resolve_reference(*current_pdf_,
                              form_resources.ref_object_number,
                              form_resources.ref_generation_number);
                          if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                            form_resources_stack_.push_back(resolved.value.dict);
                            has_form_resources = true;
                          }
                        } else if (form_resources.type == Value::DICTIONARY) {
                          form_resources_stack_.push_back(form_resources.dict);
                          has_form_resources = true;
                        }
                      }

                      auto matrix_it = xobj_value.stream.dict.find("Matrix");
                      if (matrix_it != xobj_value.stream.dict.end() &&
                          matrix_it->second.type == Value::ARRAY &&
                          matrix_it->second.array.size() >= 6) {
                        GraphicsState::Matrix form_matrix;
                        form_matrix.a = static_cast<float>(matrix_it->second.array[0].number);
                        form_matrix.b = static_cast<float>(matrix_it->second.array[1].number);
                        form_matrix.c = static_cast<float>(matrix_it->second.array[2].number);
                        form_matrix.d = static_cast<float>(matrix_it->second.array[3].number);
                        form_matrix.e = static_cast<float>(matrix_it->second.array[4].number);
                        form_matrix.f = static_cast<float>(matrix_it->second.array[5].number);
                        state_.transform = state_.transform * form_matrix;
                      }
                      parse_pdf_content(decoded.data);

                      // Pop Form resources from stack
                      if (has_form_resources) {
                        form_resources_stack_.pop_back();
                      }

                      state_ = saved_state;
                    }
                  }
                }
              }
            }
          }
        }
      }
      // Transformation
      else if (token == "cm") {
        if (operands.size() >= 6) {
          GraphicsState::Matrix m;
          m.a = std::stof(operands[0]);
          m.b = std::stof(operands[1]);
          m.c = std::stof(operands[2]);
          m.d = std::stof(operands[3]);
          m.e = std::stof(operands[4]);
          m.f = std::stof(operands[5]);
          state_.transform = state_.transform * m;
        }
      }

      operands.clear();
    } else {
      // Add as operand
      if (is_string) {
        operands.push_back(token);
      } else {
        operands.push_back(token);
      }
    }
  }

  return true;
}

bool Blend2DBackend::parse_inline_image(const std::string& content, size_t& pos) {
  // Parse inline image dictionary (BI ... ID data EI)
  // pos should be right after "BI"

  std::map<std::string, std::string> dict;

  // Skip whitespace after BI
  while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
    pos++;
  }

  // Parse dictionary entries until we hit "ID"
  while (pos < content.length()) {
    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.length()) break;

    // Check for ID (marks start of image data)
    if (content[pos] == 'I' && pos + 1 < content.length() && content[pos + 1] == 'D') {
      pos += 2;  // Skip "ID"
      // Skip single whitespace after ID
      if (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n' || content[pos] == '\r')) {
        pos++;
      }
      break;
    }

    // Parse key (should start with /)
    std::string key;
    if (content[pos] == '/') {
      pos++;  // Skip /
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/') {
        key += content[pos++];
      }
    } else {
      // Skip unknown token
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos]))) {
        pos++;
      }
      continue;
    }

    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }

    // Parse value
    std::string value;
    if (pos < content.length()) {
      if (content[pos] == '/') {
        pos++;  // Skip /
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/') {
          value += content[pos++];
        }
      } else if (content[pos] == '[') {
        // Array value
        value += content[pos++];
        while (pos < content.length() && content[pos] != ']') {
          value += content[pos++];
        }
        if (pos < content.length()) value += content[pos++];
      } else {
        // Numeric or other value
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/') {
          value += content[pos++];
        }
      }
    }

    if (!key.empty()) {
      dict[key] = value;
    }
  }

  // Get image properties using PDF abbreviations
  int width = 0, height = 0, bpc = 8;
  std::string cs = "G";  // Default grayscale
  std::string filter;

  // W or Width
  auto it = dict.find("W");
  if (it == dict.end()) it = dict.find("Width");
  if (it != dict.end()) width = std::stoi(it->second);

  // H or Height
  it = dict.find("H");
  if (it == dict.end()) it = dict.find("Height");
  if (it != dict.end()) height = std::stoi(it->second);

  // BPC or BitsPerComponent
  it = dict.find("BPC");
  if (it == dict.end()) it = dict.find("BitsPerComponent");
  if (it != dict.end()) bpc = std::stoi(it->second);

  // CS or ColorSpace
  it = dict.find("CS");
  if (it == dict.end()) it = dict.find("ColorSpace");
  if (it != dict.end()) cs = it->second;

  // F or Filter
  it = dict.find("F");
  if (it == dict.end()) it = dict.find("Filter");
  if (it != dict.end()) filter = it->second;

  if (width <= 0 || height <= 0) {
    // Skip to EI and return
    while (pos < content.length()) {
      if (content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
        pos += 2;
        return false;
      }
      pos++;
    }
    return false;
  }

  // Determine components based on color space
  int components = 1;
  if (cs == "RGB" || cs == "DeviceRGB") components = 3;
  else if (cs == "CMYK" || cs == "DeviceCMYK") components = 4;
  else if (cs == "G" || cs == "DeviceGray") components = 1;

  // Calculate expected data size
  int row_bytes = (width * components * bpc + 7) / 8;
  int expected_size = row_bytes * height;

  // Read raw image data until EI
  std::vector<uint8_t> raw_data;
  raw_data.reserve(expected_size);

  size_t data_start = pos;
  while (pos < content.length()) {
    // Look for EI preceded by whitespace
    if (pos > data_start &&
        (content[pos - 1] == ' ' || content[pos - 1] == '\n' || content[pos - 1] == '\r') &&
        content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
      break;
    }
    raw_data.push_back(static_cast<uint8_t>(content[pos]));
    pos++;
  }

  // Skip EI
  if (pos < content.length() && content[pos] == 'E') {
    pos += 2;  // Skip "EI"
  }

  // Decode if filtered
  std::vector<uint8_t> decoded_data;
  if (filter == "AHx" || filter == "ASCIIHexDecode") {
    // ASCII Hex decode
    decoded_data.reserve(raw_data.size() / 2);
    for (size_t i = 0; i + 1 < raw_data.size(); i += 2) {
      char hex[3] = {static_cast<char>(raw_data[i]), static_cast<char>(raw_data[i + 1]), 0};
      decoded_data.push_back(static_cast<uint8_t>(strtol(hex, nullptr, 16)));
    }
  } else if (filter == "A85" || filter == "ASCII85Decode") {
    // ASCII85 decode - simplified, just use raw data for now
    decoded_data = raw_data;
  } else if (filter == "Fl" || filter == "FlateDecode" || filter == "LZW" || filter == "LZWDecode") {
    // Need to decompress - use nanopdf's decode functions
    if (current_pdf_) {
      // Create a temporary stream value for decoding
      Value stream_val;
      stream_val.type = Value::STREAM;
      stream_val.stream.data = raw_data;
      if (filter == "Fl" || filter == "FlateDecode") {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "FlateDecode";
      } else {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "LZWDecode";
      }
      auto result = decode_stream(*current_pdf_, stream_val);
      if (result.success) {
        decoded_data = result.data;
      } else {
        decoded_data = raw_data;
      }
    } else {
      decoded_data = raw_data;
    }
  } else {
    // No filter or unknown, use raw data
    decoded_data = raw_data;
  }

  // Build ImageXObject
  ImageXObject image;
  image.width = width;
  image.height = height;
  image.bits_per_component = bpc;
  image.data = decoded_data;

  if (cs == "RGB" || cs == "DeviceRGB") {
    image.color_space.type = ColorSpaceType::DeviceRGB;
  } else if (cs == "CMYK" || cs == "DeviceCMYK") {
    image.color_space.type = ColorSpaceType::DeviceCMYK;
  } else {
    image.color_space.type = ColorSpaceType::DeviceGray;
  }

  // Draw the image using CTM
  float img_x = state_.transform.e * state_.scale;
  float img_y = state_.transform.f;
  float img_width = state_.transform.a * state_.scale;
  float img_height = state_.transform.d * state_.scale;

  if (img_height < 0) {
    img_y += img_height;
    img_height = -img_height;
  }
  img_y = (state_.page_height - img_y) * state_.scale - img_height;

  draw_image(image, img_x, img_y, img_width, img_height);

  return true;
}

bool Blend2DBackend::apply_pattern_fill(BLPath& path, const std::string& pattern_name, bool is_stroke) {
  if (!current_pdf_ || !current_page_) {
    return false;
  }

  // Look up pattern resources
  auto pattern_dict_it = current_page_->resources.find("Pattern");
  if (pattern_dict_it == current_page_->resources.end()) {
    return false;
  }

  Dictionary pattern_resources;
  if (pattern_dict_it->second.type == Value::DICTIONARY) {
    pattern_resources = pattern_dict_it->second.dict;
  } else if (pattern_dict_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      pattern_dict_it->second.ref_object_number,
                                      pattern_dict_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    pattern_resources = resolved.value.dict;
  } else {
    return false;
  }

  // Look up the specific pattern
  auto pattern_it = pattern_resources.find(pattern_name);
  if (pattern_it == pattern_resources.end()) {
    return false;
  }

  // Resolve pattern reference if needed
  Dictionary pattern_dict;
  if (pattern_it->second.type == Value::DICTIONARY) {
    pattern_dict = pattern_it->second.dict;
  } else if (pattern_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      pattern_it->second.ref_object_number,
                                      pattern_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    pattern_dict = resolved.value.dict;
  } else {
    return false;
  }

  // Parse the pattern
  auto pattern = parse_pattern(*current_pdf_, pattern_dict);
  if (!pattern) {
    return false;
  }

  if (pattern->type == PatternType::Shading && pattern->shading) {
    // Shading pattern - apply as gradient fill
    auto shading = pattern->shading.get();

    if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
      // Linear gradient
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float x1 = static_cast<float>(shading->coords[2]);
      float y1 = static_cast<float>(shading->coords[3]);

      // Transform through pattern matrix
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;

      BLGradient gradient(BLLinearGradientValues(x0, y0, x1, y1));

      // Get colors from function
      uint8_t start_r = 0, start_g = 0, start_b = 0;
      uint8_t end_r = 255, end_g = 255, end_b = 255;

      if (shading->function.type == Value::DICTIONARY) {
        auto func_type_it = shading->function.dict.find("FunctionType");
        if (func_type_it != shading->function.dict.end() &&
            func_type_it->second.type == Value::NUMBER &&
            static_cast<int>(func_type_it->second.number) == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            const auto& c0 = c0_it->second.array;
            if (c0.size() >= 3) {
              start_r = static_cast<uint8_t>(c0[0].number * 255);
              start_g = static_cast<uint8_t>(c0[1].number * 255);
              start_b = static_cast<uint8_t>(c0[2].number * 255);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            const auto& c1 = c1_it->second.array;
            if (c1.size() >= 3) {
              end_r = static_cast<uint8_t>(c1[0].number * 255);
              end_g = static_cast<uint8_t>(c1[1].number * 255);
              end_b = static_cast<uint8_t>(c1[2].number * 255);
            }
          }
        }
      }

      gradient.add_stop(0.0, BLRgba32(start_r, start_g, start_b, 255));
      gradient.add_stop(1.0, BLRgba32(end_r, end_g, end_b, 255));

      if (is_stroke) {
        ctx_.set_stroke_style(gradient);
      } else {
        ctx_.set_fill_style(gradient);
      }
      return true;
    }
    else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
      // Radial gradient
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float r0 = static_cast<float>(shading->coords[2]);
      float x1 = static_cast<float>(shading->coords[3]);
      float y1 = static_cast<float>(shading->coords[4]);
      float r1 = static_cast<float>(shading->coords[5]);

      // Transform through pattern matrix
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        float scale_factor = std::sqrt(std::abs(a * d - b * c));
        r0 *= scale_factor;
        r1 *= scale_factor;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      r0 *= state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;
      r1 *= state_.scale;

      BLGradient gradient(BLRadialGradientValues(x1, y1, x0, y0, r1, r0));

      // Get colors from function (similar to axial)
      uint8_t start_r = 0, start_g = 0, start_b = 0;
      uint8_t end_r = 255, end_g = 255, end_b = 255;

      if (shading->function.type == Value::DICTIONARY) {
        auto func_type_it = shading->function.dict.find("FunctionType");
        if (func_type_it != shading->function.dict.end() &&
            func_type_it->second.type == Value::NUMBER &&
            static_cast<int>(func_type_it->second.number) == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            const auto& c0 = c0_it->second.array;
            if (c0.size() >= 3) {
              start_r = static_cast<uint8_t>(c0[0].number * 255);
              start_g = static_cast<uint8_t>(c0[1].number * 255);
              start_b = static_cast<uint8_t>(c0[2].number * 255);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            const auto& c1 = c1_it->second.array;
            if (c1.size() >= 3) {
              end_r = static_cast<uint8_t>(c1[0].number * 255);
              end_g = static_cast<uint8_t>(c1[1].number * 255);
              end_b = static_cast<uint8_t>(c1[2].number * 255);
            }
          }
        }
      }

      gradient.add_stop(0.0, BLRgba32(start_r, start_g, start_b, 255));
      gradient.add_stop(1.0, BLRgba32(end_r, end_g, end_b, 255));

      if (is_stroke) {
        ctx_.set_stroke_style(gradient);
      } else {
        ctx_.set_fill_style(gradient);
      }
      return true;
    }
  }
  else if (pattern->type == PatternType::Tiling && pattern->tiling) {
    // Tiling pattern - render tile to an image and create BLPattern
    auto& tiling = *pattern->tiling;

    // Calculate tile dimensions from bbox
    float tile_width = 1.0f, tile_height = 1.0f;
    if (tiling.bbox.size() >= 4) {
      tile_width = static_cast<float>(tiling.bbox[2] - tiling.bbox[0]);
      tile_height = static_cast<float>(tiling.bbox[3] - tiling.bbox[1]);
    }

    // Apply step if specified
    float x_step = tiling.x_step > 0 ? static_cast<float>(tiling.x_step) : tile_width;
    float y_step = tiling.y_step > 0 ? static_cast<float>(tiling.y_step) : tile_height;

    // Scale tile dimensions
    uint32_t img_width = static_cast<uint32_t>(x_step * state_.scale);
    uint32_t img_height = static_cast<uint32_t>(y_step * state_.scale);

    if (img_width == 0) img_width = 32;
    if (img_height == 0) img_height = 32;
    if (img_width > 512) img_width = 512;  // Limit size for performance
    if (img_height > 512) img_height = 512;

    // Create tile image
    BLImage tile_image(img_width, img_height, BL_FORMAT_PRGB32);
    BLContext tile_ctx(tile_image);

    // Clear with transparent
    tile_ctx.set_comp_op(BL_COMP_OP_CLEAR);
    tile_ctx.fill_all();
    tile_ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

    // For colored tiles, we would render the content stream
    // For now, use a simple placeholder pattern
    if (tiling.paint_type == TilingPaintType::ColoredTiles) {
      // Use a checkered pattern as placeholder
      tile_ctx.set_fill_style(BLRgba32(200, 200, 200, 255));
      tile_ctx.fill_rect(0, 0, img_width / 2, img_height / 2);
      tile_ctx.fill_rect(img_width / 2, img_height / 2, img_width / 2, img_height / 2);
      tile_ctx.set_fill_style(BLRgba32(230, 230, 230, 255));
      tile_ctx.fill_rect(img_width / 2, 0, img_width / 2, img_height / 2);
      tile_ctx.fill_rect(0, img_height / 2, img_width / 2, img_height / 2);
    } else {
      // Uncolored tiles - use current fill color
      tile_ctx.set_fill_style(BLRgba32(state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a));
      tile_ctx.fill_all();
    }

    tile_ctx.end();

    // Create repeating pattern
    BLPattern bl_pattern(tile_image, BL_EXTEND_MODE_REPEAT);

    // Apply pattern matrix
    if (pattern->matrix.size() >= 6) {
      BLMatrix2D mat(
        pattern->matrix[0], pattern->matrix[1],
        pattern->matrix[2], pattern->matrix[3],
        pattern->matrix[4] * state_.scale,
        (state_.page_height - pattern->matrix[5]) * state_.scale
      );
      bl_pattern.set_transform(mat);
    }

    if (is_stroke) {
      ctx_.set_stroke_style(bl_pattern);
    } else {
      ctx_.set_fill_style(bl_pattern);
    }
    return true;
  }

  // Fallback to placeholder color
  if (is_stroke) {
    ctx_.set_stroke_style(BLRgba32(128, 128, 128, 255));
  } else {
    ctx_.set_fill_style(BLRgba32(200, 200, 200, 255));
  }
  return true;
}

bool Blend2DBackend::render_soft_mask_group(const Value& group_xobject, int mask_type) {
  if (!current_pdf_) return false;

  // Get the XObject stream
  Value xobject_value = group_xobject;
  if (xobject_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      xobject_value.ref_object_number,
                                      xobject_value.ref_generation_number);
    if (!resolved.success) return false;
    xobject_value = resolved.value;
  }

  if (xobject_value.type != Value::STREAM) return false;

  // Check that it's a Form XObject
  auto subtype_it = xobject_value.stream.dict.find("Subtype");
  if (subtype_it == xobject_value.stream.dict.end() ||
      subtype_it->second.type != Value::NAME ||
      subtype_it->second.name != "Form") {
    return false;
  }

  // Get dimensions from BBox
  float bbox[4] = {0, 0, 100, 100};  // Default
  auto bbox_it = xobject_value.stream.dict.find("BBox");
  if (bbox_it != xobject_value.stream.dict.end() && bbox_it->second.type == Value::ARRAY) {
    const auto& arr = bbox_it->second.array;
    for (size_t i = 0; i < 4 && i < arr.size(); ++i) {
      if (arr[i].type == Value::NUMBER) {
        bbox[i] = static_cast<float>(arr[i].number);
      }
    }
  }

  uint32_t mask_width = static_cast<uint32_t>(std::abs(bbox[2] - bbox[0]) * state_.scale);
  uint32_t mask_height = static_cast<uint32_t>(std::abs(bbox[3] - bbox[1]) * state_.scale);

  if (mask_width == 0) mask_width = width_;
  if (mask_height == 0) mask_height = height_;

  // Create a temporary image for rendering the soft mask
  BLImage mask_image(mask_width, mask_height, BL_FORMAT_PRGB32);
  BLContext mask_ctx(mask_image);

  // Clear with white (for luminosity) or transparent (for alpha)
  if (mask_type == 2) {  // Luminosity
    mask_ctx.set_fill_style(BLRgba32(255, 255, 255, 255));
  } else {  // Alpha
    mask_ctx.set_fill_style(BLRgba32(0, 0, 0, 0));
  }
  mask_ctx.fill_all();

  // Decode the XObject stream content
  auto decoded = decode_stream(*current_pdf_, xobject_value);
  if (!decoded.success) {
    return false;
  }

  // TODO: Full implementation would parse and render the XObject content
  // to the mask_ctx. For now, we store basic mask info.

  // Store mask data
  state_.soft_mask_width = mask_width;
  state_.soft_mask_height = mask_height;

  // Get the rendered pixels
  BLImageData data;
  mask_image.get_data(&data);

  // Convert to grayscale mask values
  state_.soft_mask_data.resize(mask_width * mask_height);
  const uint8_t* pixels = static_cast<const uint8_t*>(data.pixel_data);

  for (uint32_t y = 0; y < mask_height; ++y) {
    for (uint32_t x = 0; x < mask_width; ++x) {
      size_t src_idx = (y * data.stride) + (x * 4);
      size_t dst_idx = y * mask_width + x;

      if (mask_type == 2) {  // Luminosity mask
        // Convert RGB to luminance using standard coefficients
        float r = pixels[src_idx + 2] / 255.0f;
        float g = pixels[src_idx + 1] / 255.0f;
        float b = pixels[src_idx + 0] / 255.0f;
        float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        state_.soft_mask_data[dst_idx] = static_cast<uint8_t>(luminance * 255.0f);
      } else {  // Alpha mask
        state_.soft_mask_data[dst_idx] = pixels[src_idx + 3];  // Alpha channel
      }
    }
  }

  mask_ctx.end();
  return true;
}

void Blend2DBackend::apply_soft_mask_to_context() {
  if (!state_.has_soft_mask || state_.soft_mask_data.empty()) {
    return;
  }

  // Blend2D doesn't have direct soft mask support like PDF
  // We simulate by adjusting the global alpha based on mask values
  // This is a simplified approximation

  // For proper implementation, we would need to:
  // 1. Render to an offscreen buffer
  // 2. Apply the soft mask as a per-pixel alpha multiplier
  // 3. Composite the result to the main image

  // For now, calculate average mask value as a global alpha approximation
  if (state_.soft_mask_width > 0 && state_.soft_mask_height > 0) {
    uint64_t sum = 0;
    for (size_t i = 0; i < state_.soft_mask_data.size(); ++i) {
      sum += state_.soft_mask_data[i];
    }
    float avg_alpha = static_cast<float>(sum) / (state_.soft_mask_data.size() * 255.0f);

    // Multiply current opacity by mask average
    state_.fill_opacity *= avg_alpha;
    state_.stroke_opacity *= avg_alpha;
  }
}

}  // namespace nanopdf

#endif // NANOPDF_USE_BLEND2D
