// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Light Transport Entertainment Inc.

// nanopdf core for PDF parsing (needed for merge/split)
// IMPORTANT: Must be included BEFORE pdf-writer.hh so that NANOPDF_HH_INCLUDED
// is defined, preventing duplicate enum class definitions (EncryptionAlgorithm,
// BlendMode) which are conditionally defined in pdf-writer.hh.
#include "nanopdf.hh"

#include "pdf-writer.hh"

#ifdef NANOPDF_USE_MINIZ
#include "miniz.h"
// miniz defines `#define compress mz_compress` which clashes with struct field names.
// We only use compress2/compressBound, so undef the bare `compress` macro.
#undef compress
#else
#include <zlib.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>

// stb_image for loading images (implementation in stb_image_impl.cc)
#include "stb_image.h"

// stb_truetype for font metrics extraction (implementation in stb_truetype_impl.cc)
#include "stb_truetype.h"

// CCITT Group 4 encoder for monochrome images
#include "ccitt-encoder.hh"

// Cryptographic functions for encryption
#include "crypto.hh"

namespace nanopdf {

// ============================================================================
// UserPermissions implementation
// ============================================================================

int32_t UserPermissions::to_flags() const {
  // PDF permission flags are defined in ISO 32000-1:2008 Table 22
  // Bits 1-2 must be 0, bits 7-8 must be 1
  int32_t flags = 0xFFFFF0C0;  // Set reserved bits

  if (allow_print) flags |= (1 << 2);              // Bit 3
  if (allow_modify) flags |= (1 << 3);             // Bit 4
  if (allow_copy) flags |= (1 << 4);               // Bit 5
  if (allow_annotate) flags |= (1 << 5);           // Bit 6
  if (allow_fill_forms) flags |= (1 << 8);         // Bit 9
  if (allow_accessibility) flags |= (1 << 9);      // Bit 10
  if (allow_assemble) flags |= (1 << 10);          // Bit 11
  if (allow_print_high_quality) flags |= (1 << 11); // Bit 12

  return flags;
}

UserPermissions UserPermissions::from_flags(int32_t flags) {
  UserPermissions perms;
  perms.allow_print = (flags & (1 << 2)) != 0;
  perms.allow_modify = (flags & (1 << 3)) != 0;
  perms.allow_copy = (flags & (1 << 4)) != 0;
  perms.allow_annotate = (flags & (1 << 5)) != 0;
  perms.allow_fill_forms = (flags & (1 << 8)) != 0;
  perms.allow_accessibility = (flags & (1 << 9)) != 0;
  perms.allow_assemble = (flags & (1 << 10)) != 0;
  perms.allow_print_high_quality = (flags & (1 << 11)) != 0;
  return perms;
}

UserPermissions UserPermissions::all_allowed() {
  UserPermissions perms;
  perms.allow_print = true;
  perms.allow_modify = true;
  perms.allow_copy = true;
  perms.allow_annotate = true;
  perms.allow_fill_forms = true;
  perms.allow_accessibility = true;
  perms.allow_assemble = true;
  perms.allow_print_high_quality = true;
  return perms;
}

UserPermissions UserPermissions::view_only() {
  UserPermissions perms;
  perms.allow_print = false;
  perms.allow_modify = false;
  perms.allow_copy = false;
  perms.allow_annotate = false;
  perms.allow_fill_forms = false;
  perms.allow_accessibility = true;  // Required for accessibility
  perms.allow_assemble = false;
  perms.allow_print_high_quality = false;
  return perms;
}

// ============================================================================
// FontFlags implementation
// ============================================================================

int FontFlags::to_int() const {
  int flags = 0;
  if (fixed_pitch) flags |= (1 << 0);   // Bit 1
  if (serif) flags |= (1 << 1);         // Bit 2
  if (symbolic) flags |= (1 << 2);      // Bit 3
  if (script) flags |= (1 << 3);        // Bit 4
  if (nonsymbolic) flags |= (1 << 5);   // Bit 6
  if (italic) flags |= (1 << 6);        // Bit 7
  if (all_cap) flags |= (1 << 16);      // Bit 17
  if (small_cap) flags |= (1 << 17);    // Bit 18
  if (force_bold) flags |= (1 << 18);   // Bit 19
  return flags;
}

FontFlags FontFlags::from_int(int flags) {
  FontFlags f;
  f.fixed_pitch = (flags & (1 << 0)) != 0;
  f.serif = (flags & (1 << 1)) != 0;
  f.symbolic = (flags & (1 << 2)) != 0;
  f.script = (flags & (1 << 3)) != 0;
  f.nonsymbolic = (flags & (1 << 5)) != 0;
  f.italic = (flags & (1 << 6)) != 0;
  f.all_cap = (flags & (1 << 16)) != 0;
  f.small_cap = (flags & (1 << 17)) != 0;
  f.force_bold = (flags & (1 << 18)) != 0;
  return f;
}

// ============================================================================
// FontMetrics implementation
// ============================================================================

int FontMetrics::get_width(uint32_t codepoint) const {
  auto it = char_to_glyph.find(codepoint);
  if (it == char_to_glyph.end()) {
    return 0;  // Unknown character
  }
  uint16_t glyph_id = it->second;
  if (glyph_id < glyph_widths.size()) {
    return glyph_widths[glyph_id];
  }
  return 0;
}

// ============================================================================
// FontData implementation
// ============================================================================

namespace {

// Helper to extract font metrics using stb_truetype
bool extract_font_metrics(const uint8_t* data, size_t size, FontMetrics& metrics) {
  stbtt_fontinfo font;
  if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) {
    return false;
  }

  // Get basic metrics
  int ascent, descent, line_gap;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
  metrics.ascender = ascent;
  metrics.descender = descent;
  metrics.line_gap = line_gap;

  // Get units per em from head table
  // stbtt_ScaleForMappingEmToPixels returns scale = pixels / unitsPerEm
  // So calling with pixels=1 gives us scale = 1/unitsPerEm
  float scale = stbtt_ScaleForMappingEmToPixels(&font, 1.0f);
  if (scale > 0) {
    metrics.units_per_em = static_cast<int>(1.0f / scale + 0.5f);
    if (metrics.units_per_em <= 0) metrics.units_per_em = 1000;
  } else {
    metrics.units_per_em = 1000;
  }

  // Get bounding box
  int x0, y0, x1, y1;
  stbtt_GetFontBoundingBox(&font, &x0, &y0, &x1, &y1);
  metrics.bbox = {x0, y0, x1, y1};

  // Try to get font name
  int name_len;
  const char* name = stbtt_GetFontNameString(&font, &name_len,
      STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
      STBTT_MS_LANG_ENGLISH, 6);  // 6 = PostScript name
  if (name && name_len > 0) {
    // Microsoft names are UTF-16BE encoded
    std::string ps_name;
    for (int i = 0; i + 1 < name_len; i += 2) {
      uint16_t c = (static_cast<uint8_t>(name[i]) << 8) |
                   static_cast<uint8_t>(name[i + 1]);
      if (c > 0 && c < 128) {
        ps_name += static_cast<char>(c);
      }
    }
    if (!ps_name.empty()) {
      metrics.font_name = ps_name;
    }
  }

  // Try family name independently so callers can inspect it even when the
  // PostScript name is also present.
  name = stbtt_GetFontNameString(&font, &name_len,
      STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
      STBTT_MS_LANG_ENGLISH, 1);  // 1 = Family name
  if (name && name_len > 0) {
    std::string family_name;
    for (int i = 0; i + 1 < name_len; i += 2) {
      uint16_t c = (static_cast<uint8_t>(name[i]) << 8) |
                   static_cast<uint8_t>(name[i + 1]);
      if (c > 0 && c < 128) {
        family_name += static_cast<char>(c);
      }
    }
    if (!family_name.empty()) {
      metrics.family_name = family_name;
      if (metrics.font_name.empty()) {
        metrics.font_name = family_name;
      }
    }
  }

  // Fallback name
  if (metrics.font_name.empty()) {
    metrics.font_name = "EmbeddedFont";
  }

  // Clean up font name (replace spaces with hyphens for PostScript)
  for (char& c : metrics.font_name) {
    if (c == ' ') c = '-';
  }

  // Extract glyph widths
  int num_glyphs = font.numGlyphs;
  metrics.glyph_widths.resize(num_glyphs, 0);

  for (int glyph_id = 0; glyph_id < num_glyphs; glyph_id++) {
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&font, glyph_id, &advance, &lsb);
    // Convert to 1/1000 em units
    metrics.glyph_widths[glyph_id] =
        static_cast<int>(advance * 1000.0f / metrics.units_per_em);
  }

  // Build character to glyph mapping for common characters
  // We'll map codepoints 0-65535 (BMP)
  for (uint32_t cp = 0; cp < 65536; cp++) {
    int glyph_id = stbtt_FindGlyphIndex(&font, cp);
    if (glyph_id > 0) {
      metrics.char_to_glyph[cp] = static_cast<uint16_t>(glyph_id);
    }
  }

  // Estimate cap height and x-height from glyph metrics
  int cap_h_glyph = stbtt_FindGlyphIndex(&font, 'H');
  if (cap_h_glyph > 0) {
    int x0g, y0g, x1g, y1g;
    stbtt_GetGlyphBox(&font, cap_h_glyph, &x0g, &y0g, &x1g, &y1g);
    metrics.cap_height = y1g;
  } else {
    metrics.cap_height = static_cast<int>(metrics.ascender * 0.7);
  }

  int x_h_glyph = stbtt_FindGlyphIndex(&font, 'x');
  if (x_h_glyph > 0) {
    int x0g, y0g, x1g, y1g;
    stbtt_GetGlyphBox(&font, x_h_glyph, &x0g, &y0g, &x1g, &y1g);
    metrics.x_height = y1g;
  } else {
    metrics.x_height = static_cast<int>(metrics.cap_height * 0.7);
  }

  // Estimate stem width from 'I' glyph
  int stem_glyph = stbtt_FindGlyphIndex(&font, 'I');
  if (stem_glyph > 0) {
    int x0g, y0g, x1g, y1g;
    stbtt_GetGlyphBox(&font, stem_glyph, &x0g, &y0g, &x1g, &y1g);
    metrics.stem_v = x1g - x0g;
    if (metrics.stem_v < 50) metrics.stem_v = 80;  // Reasonable default
  } else {
    metrics.stem_v = 80;
  }

  // Set default flags
  metrics.flags.nonsymbolic = true;

  return true;
}

std::vector<std::pair<uint32_t, uint16_t>> collect_page_object_refs(const Pdf& pdf) {
  std::vector<std::pair<uint32_t, uint16_t>> page_obj_nums;
  auto root_it = pdf.trailer.find("Root");
  if (root_it == pdf.trailer.end() || root_it->second.type != Value::REFERENCE) {
    return page_obj_nums;
  }

  ResolvedObject root = resolve_reference(
      pdf, root_it->second.ref_object_number, root_it->second.ref_generation_number);
  if (!root.success || root.value.type != Value::DICTIONARY) {
    return page_obj_nums;
  }

  auto pages_it = root.value.dict.find("Pages");
  if (pages_it == root.value.dict.end() || pages_it->second.type != Value::REFERENCE) {
    return page_obj_nums;
  }

  std::function<void(uint32_t, uint16_t)> collect_pages;
  collect_pages = [&](uint32_t obj_n, uint16_t gen_n) {
    ResolvedObject node = resolve_reference(pdf, obj_n, gen_n);
    if (!node.success || node.value.type != Value::DICTIONARY) return;

    auto type_it = node.value.dict.find("Type");
    if (type_it != node.value.dict.end() &&
        type_it->second.type == Value::NAME &&
        type_it->second.name == "Page") {
      page_obj_nums.push_back({obj_n, gen_n});
      return;
    }

    auto kids_it = node.value.dict.find("Kids");
    if (kids_it == node.value.dict.end() || kids_it->second.type != Value::ARRAY) {
      return;
    }

    for (const auto& kid : kids_it->second.array) {
      if (kid.type == Value::REFERENCE) {
        collect_pages(kid.ref_object_number, kid.ref_generation_number);
      }
    }
  };

  collect_pages(
      pages_it->second.ref_object_number, pages_it->second.ref_generation_number);
  return page_obj_nums;
}

}  // namespace

FontData FontData::FromFile(const std::string& path) {
  FontData result;

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return result;
  }

  size_t size = file.tellg();
  file.seekg(0);

  result.data.resize(size);
  file.read(reinterpret_cast<char*>(result.data.data()), size);

  if (!file) {
    result.data.clear();
    return result;
  }

  if (extract_font_metrics(result.data.data(), result.data.size(),
                           result.metrics)) {
    result.valid = true;
  }

  return result;
}

FontData FontData::FromMemory(const uint8_t* data, size_t size) {
  FontData result;

  if (!data || size == 0) {
    return result;
  }

  result.data.assign(data, data + size);

  if (extract_font_metrics(result.data.data(), result.data.size(),
                           result.metrics)) {
    result.valid = true;
  }

  return result;
}

namespace {

// Helper to format doubles without trailing zeros
std::string format_number(double n) {
  // Handle integers
  if (n == std::floor(n) && std::abs(n) < 1e9) {
    return std::to_string(static_cast<long long>(n));
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(4) << n;
  std::string s = oss.str();

  // Remove trailing zeros
  size_t dot = s.find('.');
  if (dot != std::string::npos) {
    size_t last_nonzero = s.find_last_not_of('0');
    if (last_nonzero == dot) {
      s = s.substr(0, dot);
    } else if (last_nonzero != std::string::npos) {
      s = s.substr(0, last_nonzero + 1);
    }
  }
  return s;
}

// Escape special characters in PDF string
std::string escape_pdf_string(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 10);
  for (char c : s) {
    switch (c) {
      case '(':
        result += "\\(";
        break;
      case ')':
        result += "\\)";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 32 ||
            static_cast<unsigned char>(c) > 126) {
          // Octal escape for non-printable
          char buf[8];
          snprintf(buf, sizeof(buf), "\\%03o",
                   static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

std::string serialize_pdf_value(
    const Value& value,
    const std::function<std::string(const std::string&)>& string_formatter = {}) {
  auto format_string = [&](const std::string& s) {
    if (string_formatter) {
      return string_formatter(s);
    }
    return "(" + escape_pdf_string(s) + ")";
  };

  switch (value.type) {
    case Value::BOOLEAN:
      return value.boolean ? "true" : "false";

    case Value::NUMBER:
      return format_number(value.number);

    case Value::STRING:
      return format_string(value.str);

    case Value::NAME:
      return "/" + value.name;

    case Value::ARRAY: {
      std::string serialized = "[";
      for (size_t i = 0; i < value.array.size(); ++i) {
        if (i > 0) serialized += " ";
        serialized += serialize_pdf_value(value.array[i], string_formatter);
      }
      serialized += "]";
      return serialized;
    }

    case Value::DICTIONARY: {
      std::string serialized = "<<";
      for (const auto& item : value.dict) {
        serialized += "\n/";
        serialized += item.first;
        serialized += " ";
        serialized += serialize_pdf_value(item.second, string_formatter);
      }
      serialized += "\n>>";
      return serialized;
    }

    case Value::REFERENCE:
      return std::to_string(value.ref_object_number) + " " +
             std::to_string(value.ref_generation_number) + " R";

    case Value::NULL_OBJ:
    case Value::UNDEFINED:
      return "null";

    case Value::STREAM:
      return "null";
  }

  return "null";
}

bool pdf_name_needs_escape(unsigned char ch) {
  switch (ch) {
    case 0:
    case '(':
    case ')':
    case '<':
    case '>':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '%':
    case '#':
      return true;
    default:
      break;
  }
  return ch <= 0x20 || ch >= 0x7f;
}

std::string escape_pdf_name(const std::string& input) {
  static const char hex[] = "0123456789ABCDEF";
  std::string escaped;
  escaped.reserve(input.size());
  for (unsigned char ch : input) {
    if (pdf_name_needs_escape(ch)) {
      escaped += '#';
      escaped += hex[(ch >> 4) & 0x0f];
      escaped += hex[ch & 0x0f];
    } else {
      escaped += static_cast<char>(ch);
    }
  }
  return escaped;
}

// Compress data using zlib
std::vector<uint8_t> compress_data(const uint8_t* data, size_t size) {
  std::vector<uint8_t> compressed;
  uLongf compressed_size = compressBound(static_cast<uLong>(size));
  if (compressed_size == 0) {
    compressed_size = 16;
  }
  compressed.resize(compressed_size);

  static const uint8_t kEmptyInput = 0;
  const uint8_t* input = size > 0 ? data : &kEmptyInput;

  int result = compress2(compressed.data(), &compressed_size, input,
                         static_cast<uLong>(size), Z_DEFAULT_COMPRESSION);
  if (result != Z_OK) {
    compressed.clear();
    return compressed;
  }

  compressed.resize(compressed_size);
  return compressed;
}

// Get current timestamp in PDF format
std::string get_pdf_timestamp() {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  char buf[64];
  strftime(buf, sizeof(buf), "D:%Y%m%d%H%M%S", tm_info);
  return std::string(buf);
}

// Get PDF version string
const char* get_version_string(PdfVersion version) {
  switch (version) {
    case PdfVersion::v1_4: return "1.4";
    case PdfVersion::v1_5: return "1.5";
    case PdfVersion::v1_6: return "1.6";
    case PdfVersion::v1_7: return "1.7";
    case PdfVersion::v2_0: return "2.0";
    default: return "1.4";
  }
}

// Get signature filter name
const char* get_filter_name(SignatureFilter filter) {
  switch (filter) {
    case SignatureFilter::AdobePPKLite: return "Adobe.PPKLite";
    case SignatureFilter::EntrustPPKEF: return "Entrust.PPKEF";
    case SignatureFilter::CICISignIt: return "CICI.SignIt";
    case SignatureFilter::VeriSignPPKVS: return "VeriSign.PPKVS";
    default: return "Adobe.PPKLite";
  }
}

// Get signature subfilter name
const char* get_subfilter_name(SignatureSubFilter subfilter) {
  switch (subfilter) {
    case SignatureSubFilter::Pkcs7Detached: return "adbe.pkcs7.detached";
    case SignatureSubFilter::Pkcs7Sha1: return "adbe.pkcs7.sha1";
    case SignatureSubFilter::EtsiCadesDetached: return "ETSI.CAdES.detached";
    case SignatureSubFilter::EtsiRfc3161: return "ETSI.RFC3161";
    default: return "adbe.pkcs7.detached";
  }
}

// Generate random bytes for document ID
std::vector<uint8_t> generate_random_id() {
  std::vector<uint8_t> id(16);
  // Simple pseudo-random generation based on time and address
  uint64_t seed = static_cast<uint64_t>(time(nullptr));
  seed ^= reinterpret_cast<uint64_t>(&id);
  for (int i = 0; i < 16; ++i) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    id[i] = static_cast<uint8_t>(seed >> 56);
    if (id[i] == 0) {
      id[i] = 1;
    }
  }
  return id;
}

// Convert bytes to hex string
std::string bytes_to_hex(const std::vector<uint8_t>& data) {
  std::string hex;
  hex.reserve(data.size() * 2);
  static const char hex_chars[] = "0123456789ABCDEF";
  for (uint8_t b : data) {
    hex += hex_chars[(b >> 4) & 0xF];
    hex += hex_chars[b & 0xF];
  }
  return hex;
}

// ============================================================
// PDF Parsing Helpers (for incremental update support)
// ============================================================

// Find "startxref" and return the xref offset
bool find_startxref(const std::vector<uint8_t>& data, size_t& xref_offset) {
  // Search from end of file for "startxref"
  const char* pattern = "startxref";
  size_t pattern_len = 9;

  if (data.size() < pattern_len + 10) return false;

  // Search in last 1024 bytes (typical EOF location)
  size_t search_start = (data.size() > 1024) ? data.size() - 1024 : 0;

  for (size_t i = search_start; i < data.size() - pattern_len; ++i) {
    if (memcmp(&data[i], pattern, pattern_len) == 0) {
      // Found "startxref", now read the offset
      size_t pos = i + pattern_len;
      // Skip whitespace
      while (pos < data.size() && (data[pos] == ' ' || data[pos] == '\n' || data[pos] == '\r')) {
        ++pos;
      }
      // Read number
      xref_offset = 0;
      while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
        xref_offset = xref_offset * 10 + (data[pos] - '0');
        ++pos;
      }
      return xref_offset > 0;
    }
  }
  return false;
}

// Skip whitespace in PDF data
size_t skip_whitespace(const std::vector<uint8_t>& data, size_t pos) {
  while (pos < data.size() &&
         (data[pos] == ' ' || data[pos] == '\n' || data[pos] == '\r' || data[pos] == '\t')) {
    ++pos;
  }
  return pos;
}

// Read an integer from PDF data
int read_int(const std::vector<uint8_t>& data, size_t& pos) {
  pos = skip_whitespace(data, pos);
  int sign = 1;
  if (pos < data.size() && data[pos] == '-') {
    sign = -1;
    ++pos;
  }
  int value = 0;
  while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
    value = value * 10 + (data[pos] - '0');
    ++pos;
  }
  return sign * value;
}

// Find a keyword in PDF data starting from pos
size_t find_keyword(const std::vector<uint8_t>& data, size_t pos, const char* keyword) {
  size_t keyword_len = strlen(keyword);
  while (pos + keyword_len < data.size()) {
    if (memcmp(&data[pos], keyword, keyword_len) == 0) {
      return pos;
    }
    ++pos;
  }
  return std::string::npos;
}

// Parse trailer dictionary and extract Root, Info, Size, ID
bool parse_trailer(const std::vector<uint8_t>& data, size_t trailer_pos,
                   int& root_obj, int& root_gen,
                   int& info_obj, int& info_gen,
                   int& size_value,
                   std::vector<uint8_t>& id1, std::vector<uint8_t>& id2) {
  // Find "<<" after "trailer"
  size_t pos = find_keyword(data, trailer_pos, "<<");
  if (pos == std::string::npos) return false;
  pos += 2;  // Skip "<<"

  // Parse dictionary entries until ">>"
  while (pos < data.size()) {
    pos = skip_whitespace(data, pos);
    if (pos >= data.size()) break;

    // Check for end of dictionary
    if (data[pos] == '>' && pos + 1 < data.size() && data[pos + 1] == '>') {
      break;
    }

    // Expect name starting with '/'
    if (data[pos] != '/') {
      ++pos;
      continue;
    }
    ++pos;  // Skip '/'

    // Read name
    std::string name;
    while (pos < data.size() && data[pos] != ' ' && data[pos] != '\n' &&
           data[pos] != '\r' && data[pos] != '/' && data[pos] != '<' &&
           data[pos] != '[' && data[pos] != '(') {
      name += static_cast<char>(data[pos]);
      ++pos;
    }

    pos = skip_whitespace(data, pos);

    if (name == "Root" || name == "Info") {
      // Parse indirect reference: num gen R
      int obj_num = read_int(data, pos);
      int gen_num = read_int(data, pos);
      pos = skip_whitespace(data, pos);
      if (pos < data.size() && data[pos] == 'R') {
        ++pos;
        if (name == "Root") {
          root_obj = obj_num;
          root_gen = gen_num;
        } else {
          info_obj = obj_num;
          info_gen = gen_num;
        }
      }
    } else if (name == "Size") {
      size_value = read_int(data, pos);
    } else if (name == "ID") {
      // Parse ID array: [<hex> <hex>]
      pos = skip_whitespace(data, pos);
      if (pos < data.size() && data[pos] == '[') {
        ++pos;
        pos = skip_whitespace(data, pos);

        // Read first hex string
        if (pos < data.size() && data[pos] == '<') {
          ++pos;
          id1.clear();
          while (pos < data.size() && data[pos] != '>') {
            // Parse hex pairs
            if (data[pos] >= '0' && data[pos] <= '9') {
              uint8_t high = data[pos] - '0';
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id1.push_back((high << 4) | low);
                ++pos;
              }
            } else if (data[pos] >= 'A' && data[pos] <= 'F') {
              uint8_t high = data[pos] - 'A' + 10;
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id1.push_back((high << 4) | low);
                ++pos;
              }
            } else if (data[pos] >= 'a' && data[pos] <= 'f') {
              uint8_t high = data[pos] - 'a' + 10;
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id1.push_back((high << 4) | low);
                ++pos;
              }
            } else {
              ++pos;
            }
          }
          if (pos < data.size() && data[pos] == '>') ++pos;
        }

        pos = skip_whitespace(data, pos);

        // Read second hex string
        if (pos < data.size() && data[pos] == '<') {
          ++pos;
          id2.clear();
          while (pos < data.size() && data[pos] != '>') {
            if (data[pos] >= '0' && data[pos] <= '9') {
              uint8_t high = data[pos] - '0';
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id2.push_back((high << 4) | low);
                ++pos;
              }
            } else if (data[pos] >= 'A' && data[pos] <= 'F') {
              uint8_t high = data[pos] - 'A' + 10;
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id2.push_back((high << 4) | low);
                ++pos;
              }
            } else if (data[pos] >= 'a' && data[pos] <= 'f') {
              uint8_t high = data[pos] - 'a' + 10;
              ++pos;
              if (pos < data.size()) {
                uint8_t low = 0;
                if (data[pos] >= '0' && data[pos] <= '9') low = data[pos] - '0';
                else if (data[pos] >= 'A' && data[pos] <= 'F') low = data[pos] - 'A' + 10;
                else if (data[pos] >= 'a' && data[pos] <= 'f') low = data[pos] - 'a' + 10;
                id2.push_back((high << 4) | low);
                ++pos;
              }
            } else {
              ++pos;
            }
          }
          if (pos < data.size() && data[pos] == '>') ++pos;
        }
      }
    } else {
      // Skip other values
      while (pos < data.size() && data[pos] != '/' && data[pos] != '>') {
        if (data[pos] == '<' && pos + 1 < data.size() && data[pos + 1] == '<') {
          // Skip nested dictionary
          int depth = 1;
          pos += 2;
          while (pos < data.size() && depth > 0) {
            if (data[pos] == '<' && pos + 1 < data.size() && data[pos + 1] == '<') {
              ++depth;
              ++pos;
            } else if (data[pos] == '>' && pos + 1 < data.size() && data[pos + 1] == '>') {
              --depth;
              ++pos;
            }
            ++pos;
          }
        } else if (data[pos] == '[') {
          // Skip array
          int depth = 1;
          ++pos;
          while (pos < data.size() && depth > 0) {
            if (data[pos] == '[') ++depth;
            else if (data[pos] == ']') --depth;
            ++pos;
          }
        } else if (data[pos] == '(') {
          // Skip string
          ++pos;
          while (pos < data.size() && data[pos] != ')') {
            if (data[pos] == '\\') ++pos;  // Skip escaped char
            ++pos;
          }
          if (pos < data.size()) ++pos;
        } else {
          ++pos;
        }
      }
    }
  }

  return root_obj > 0;
}

// Count revisions by following /Prev chain
int count_revisions(const std::vector<uint8_t>& data, size_t xref_offset) {
  int count = 1;  // Current revision
  size_t pos = xref_offset;

  while (pos < data.size()) {
    // Find trailer
    size_t trailer_pos = find_keyword(data, pos, "trailer");
    if (trailer_pos == std::string::npos) break;

    // Find /Prev in trailer
    size_t prev_pos = find_keyword(data, trailer_pos, "/Prev");
    if (prev_pos == std::string::npos) break;

    prev_pos += 5;  // Skip "/Prev"
    int prev_offset = read_int(data, prev_pos);
    if (prev_offset <= 0 || static_cast<size_t>(prev_offset) >= pos) break;

    ++count;
    pos = prev_offset;
  }

  return count;
}

// ============================================================
// PDF Encryption Helpers
// ============================================================

// Password padding string (32 bytes, from PDF spec)
static const uint8_t kPasswordPadding[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
    0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
    0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
};

// Pad password to 32 bytes using PDF padding
std::vector<uint8_t> pad_password(const std::string& password) {
  std::vector<uint8_t> padded(32);
  size_t len = std::min(password.size(), size_t(32));
  memcpy(padded.data(), password.data(), len);
  if (len < 32) {
    memcpy(padded.data() + len, kPasswordPadding, 32 - len);
  }
  return padded;
}

// Generate random bytes
std::vector<uint8_t> generate_random_bytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<uint8_t>(dis(gen));
  }
  return bytes;
}

// Compute encryption key for PDF standard security handler (Algorithm 2)
// For RC4 and AES-128
std::vector<uint8_t> compute_encryption_key_r4(
    const std::string& password,
    const std::vector<uint8_t>& o_value,
    int32_t permissions,
    const std::vector<uint8_t>& doc_id,
    int key_length,
    int revision,
    bool encrypt_metadata) {
  // Step 1: Pad password
  std::vector<uint8_t> padded = pad_password(password);

  // Step 2-7: MD5 hash of concatenated data
  crypto::MD5 md5;
  md5.update(padded.data(), padded.size());
  md5.update(o_value.data(), o_value.size());

  uint8_t perm_bytes[4] = {
      static_cast<uint8_t>(permissions & 0xFF),
      static_cast<uint8_t>((permissions >> 8) & 0xFF),
      static_cast<uint8_t>((permissions >> 16) & 0xFF),
      static_cast<uint8_t>((permissions >> 24) & 0xFF)
  };
  md5.update(perm_bytes, 4);
  md5.update(doc_id.data(), doc_id.size());

  // For revision 4+, if metadata is not encrypted
  if (!encrypt_metadata) {
    uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    md5.update(ff, 4);
  }

  md5.finalize();
  uint8_t digest[16];
  md5.get_digest(digest);

  int n = key_length / 8;  // Key length in bytes
  if (revision >= 3) {
    for (int i = 0; i < 50; ++i) {
      crypto::MD5 md5_iter;
      md5_iter.update(digest, n);
      md5_iter.finalize();
      md5_iter.get_digest(digest);
    }
  }

  return std::vector<uint8_t>(digest, digest + n);
}

// Compute O value (owner password hash) - Algorithm 3
std::vector<uint8_t> compute_o_value(
    const std::string& owner_password,
    const std::string& user_password,
    int key_length,
    int revision) {
  // Step 1-4: MD5 of padded owner password (or user if owner is empty)
  std::string owner = owner_password.empty() ? user_password : owner_password;
  std::vector<uint8_t> padded = pad_password(owner);

  crypto::MD5 md5;
  md5.update(padded.data(), padded.size());
  md5.finalize();
  uint8_t digest[16];
  md5.get_digest(digest);

  int n = key_length / 8;
  if (revision >= 3) {
    for (int i = 0; i < 50; ++i) {
      crypto::MD5 md5_iter;
      md5_iter.update(digest, n);
      md5_iter.finalize();
      md5_iter.get_digest(digest);
    }
  }

  // Step 6: Use first n bytes as RC4 key
  std::vector<uint8_t> rc4_key(digest, digest + n);

  // Step 7: Pad user password and encrypt with RC4
  std::vector<uint8_t> o_value = pad_password(user_password);

  if (revision == 2) {
    crypto::RC4 rc4;
    rc4.init(rc4_key.data(), rc4_key.size());
    rc4.crypt(o_value.data(), o_value.size());
  } else {
    // For revision 3+, do 20 iterations with modified keys
    for (int i = 0; i < 20; ++i) {
      std::vector<uint8_t> iter_key(n);
      for (int j = 0; j < n; ++j) {
        iter_key[j] = rc4_key[j] ^ static_cast<uint8_t>(i);
      }
      crypto::RC4 rc4;
      rc4.init(iter_key.data(), iter_key.size());
      rc4.crypt(o_value.data(), o_value.size());
    }
  }

  return o_value;
}

// Compute U value (user password hash) - Algorithm 5 (revision 3+)
std::vector<uint8_t> compute_u_value(
    const std::vector<uint8_t>& encryption_key,
    const std::vector<uint8_t>& doc_id,
    int revision) {
  if (revision == 2) {
    std::vector<uint8_t> u_value(kPasswordPadding, kPasswordPadding + 32);
    crypto::RC4 rc4;
    rc4.init(encryption_key.data(), encryption_key.size());
    rc4.crypt(u_value.data(), u_value.size());
    return u_value;
  }

  // Step 1: MD5 of password padding + document ID
  crypto::MD5 md5;
  md5.update(kPasswordPadding, 32);
  md5.update(doc_id.data(), doc_id.size());
  md5.finalize();
  uint8_t digest[16];
  md5.get_digest(digest);

  // Step 2-3: RC4 encrypt with 20 iterations
  std::vector<uint8_t> u_value(digest, digest + 16);

  for (int i = 0; i < 20; ++i) {
    std::vector<uint8_t> iter_key(encryption_key.size());
    for (size_t j = 0; j < encryption_key.size(); ++j) {
      iter_key[j] = encryption_key[j] ^ static_cast<uint8_t>(i);
    }
    crypto::RC4 rc4;
    rc4.init(iter_key.data(), iter_key.size());
    rc4.crypt(u_value.data(), u_value.size());
  }

  // Pad to 32 bytes with arbitrary data
  u_value.resize(32);
  return u_value;
}

// Compute encryption key for AES-256 (PDF 2.0 / Extension Level 3)
// Uses SHA-256 based algorithm
std::vector<uint8_t> compute_encryption_key_aes256(
    const std::string& password,
    const std::vector<uint8_t>& user_key_salt,
    const std::vector<uint8_t>& owner_key_salt,
    bool is_owner) {
  // For AES-256, the file encryption key is random (32 bytes)
  // We compute user/owner keys to encrypt/verify it
  crypto::SHA256 sha;
  std::vector<uint8_t> padded = pad_password(password);
  sha.update(padded.data(), std::min(padded.size(), size_t(127)));

  if (is_owner) {
    sha.update(owner_key_salt.data(), 8);
  } else {
    sha.update(user_key_salt.data(), 8);
  }

  sha.finalize();
  std::vector<uint8_t> key(32);
  sha.get_digest(key.data());
  return key;
}

// Encrypt data with object-specific key
std::vector<uint8_t> encrypt_data(
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& base_key,
    int obj_num,
    int gen_num,
    EncryptionAlgorithm algorithm) {
  if (algorithm == EncryptionAlgorithm::None) {
    return data;
  }

  // Compute object-specific key
  std::vector<uint8_t> obj_key;

  if (algorithm == EncryptionAlgorithm::AES_256) {
    // AES-256 uses the base key directly
    obj_key = base_key;
  } else {
    // For RC4 and AES-128: concatenate base key + obj num + gen num
    std::vector<uint8_t> key_data = base_key;
    key_data.push_back(obj_num & 0xFF);
    key_data.push_back((obj_num >> 8) & 0xFF);
    key_data.push_back((obj_num >> 16) & 0xFF);
    key_data.push_back(gen_num & 0xFF);
    key_data.push_back((gen_num >> 8) & 0xFF);

    if (algorithm == EncryptionAlgorithm::AES_128) {
      // For AES, append "sAlT"
      key_data.push_back('s');
      key_data.push_back('A');
      key_data.push_back('l');
      key_data.push_back('T');
    }

    // MD5 hash
    crypto::MD5 md5;
    md5.update(key_data.data(), key_data.size());
    md5.finalize();
    uint8_t digest[16];
    md5.get_digest(digest);

    // Key length is min(base_key.size() + 5, 16)
    size_t key_len = std::min(base_key.size() + 5, size_t(16));
    obj_key.assign(digest, digest + key_len);
  }

  std::vector<uint8_t> result;

  if (algorithm == EncryptionAlgorithm::RC4_40 ||
      algorithm == EncryptionAlgorithm::RC4_128) {
    // RC4 encryption
    result = data;
    crypto::RC4 rc4;
    rc4.init(obj_key.data(), obj_key.size());
    rc4.crypt(result.data(), result.size());
  } else if (algorithm == EncryptionAlgorithm::AES_128) {
    // AES-128 CBC encryption
    // Generate random IV
    std::vector<uint8_t> iv = generate_random_bytes(16);

    // Pad data to block size
    std::vector<uint8_t> padded = data;
    crypto::pad_pkcs7(padded, 16);

    // Encrypt
    result.resize(16 + padded.size());  // IV + ciphertext
    memcpy(result.data(), iv.data(), 16);

    crypto::AES128 aes;
    aes.set_key(obj_key.data());
    aes.encrypt_cbc(padded.data(), result.data() + 16, padded.size(), iv.data());
  } else if (algorithm == EncryptionAlgorithm::AES_256) {
    // AES-256 CBC encryption
    std::vector<uint8_t> iv = generate_random_bytes(16);

    std::vector<uint8_t> padded = data;
    crypto::pad_pkcs7(padded, 16);

    result.resize(16 + padded.size());
    memcpy(result.data(), iv.data(), 16);

    crypto::AES256 aes;
    aes.set_key(obj_key.data());
    aes.encrypt_cbc(padded.data(), result.data() + 16, padded.size(), iv.data());
  }

  return result;
}

// Encrypt a string for PDF output
std::string encrypt_string(
    const std::string& str,
    const std::vector<uint8_t>& base_key,
    int obj_num,
    int gen_num,
    EncryptionAlgorithm algorithm) {
  std::vector<uint8_t> data(str.begin(), str.end());
  std::vector<uint8_t> encrypted = encrypt_data(data, base_key, obj_num, gen_num, algorithm);
  return std::string(encrypted.begin(), encrypted.end());
}

// Standard font names
const char* get_standard_font_name(StandardFont font) {
  switch (font) {
    case StandardFont::Helvetica:
      return "Helvetica";
    case StandardFont::HelveticaBold:
      return "Helvetica-Bold";
    case StandardFont::HelveticaOblique:
      return "Helvetica-Oblique";
    case StandardFont::HelveticaBoldOblique:
      return "Helvetica-BoldOblique";
    case StandardFont::TimesRoman:
      return "Times-Roman";
    case StandardFont::TimesBold:
      return "Times-Bold";
    case StandardFont::TimesItalic:
      return "Times-Italic";
    case StandardFont::TimesBoldItalic:
      return "Times-BoldItalic";
    case StandardFont::Courier:
      return "Courier";
    case StandardFont::CourierBold:
      return "Courier-Bold";
    case StandardFont::CourierOblique:
      return "Courier-Oblique";
    case StandardFont::CourierBoldOblique:
      return "Courier-BoldOblique";
    case StandardFont::Symbol:
      return "Symbol";
    case StandardFont::ZapfDingbats:
      return "ZapfDingbats";
    default:
      return "Helvetica";
  }
}

}  // namespace

// ============================================================================
// ImageData implementation
// ============================================================================

ImageData ImageData::FromFile(const std::string& path) {
  ImageData img;

  // Read file into memory
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return img;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return img;
  }

  return FromMemory(buffer.data(), buffer.size());
}

ImageData ImageData::FromMemory(const uint8_t* data, size_t size) {
  ImageData img;
  if (!data || size < 4) return img;

  // Detect format from magic bytes
  if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
    img.format = ImageFormat::JPEG;
    img.is_jpeg = true;
  } else if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
             data[3] == 'G') {
    img.format = ImageFormat::PNG;
  } else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    img.format = ImageFormat::BMP;
  } else if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
                           memcmp(data, "GIF89a", 6) == 0)) {
    img.format = ImageFormat::GIF;
  }

  // Store encoded data for JPEG passthrough
  if (img.is_jpeg) {
    img.encoded_data.assign(data, data + size);
  }

  // Decode with stb_image
  int w, h, channels;
  unsigned char* decoded =
      stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 0);
  if (!decoded) {
    return img;
  }

  img.width = w;
  img.height = h;
  img.channels = channels;
  img.raw_data.assign(decoded, decoded + w * h * channels);
  stbi_image_free(decoded);

  return img;
}

std::vector<uint8_t> ImageData::extract_alpha() const {
  std::vector<uint8_t> alpha;
  if (channels != 4 || raw_data.empty()) {
    return alpha;
  }
  alpha.reserve(static_cast<size_t>(width) * height);
  for (int i = 0; i < width * height; ++i) {
    alpha.push_back(raw_data[i * 4 + 3]);
  }
  return alpha;
}

std::vector<uint8_t> ImageData::extract_rgb() const {
  std::vector<uint8_t> rgb;
  if (channels != 4 || raw_data.empty()) {
    return rgb;
  }
  rgb.reserve(static_cast<size_t>(width) * height * 3);
  for (int i = 0; i < width * height; ++i) {
    rgb.push_back(raw_data[i * 4]);
    rgb.push_back(raw_data[i * 4 + 1]);
    rgb.push_back(raw_data[i * 4 + 2]);
  }
  return rgb;
}

// ============================================================================
// PageBuilder implementation
// ============================================================================

PageBuilder::PageBuilder(PdfWriter* writer) : writer_(writer) {}

void PageBuilder::emit(const std::string& op) {
  if (!content_.empty() && content_.back() != '\n') {
    content_ += ' ';
  }
  content_ += op;
}

void PageBuilder::emit_number(double n) { emit(format_number(n)); }

std::string PageBuilder::escape_string(const std::string& s) {
  return escape_pdf_string(s);
}

void PageBuilder::save_state() { emit("q\n"); }

void PageBuilder::restore_state() { emit("Q\n"); }

void PageBuilder::translate(double tx, double ty) {
  concat_matrix(1, 0, 0, 1, tx, ty);
}

void PageBuilder::scale(double sx, double sy) {
  concat_matrix(sx, 0, 0, sy, 0, 0);
}

void PageBuilder::rotate(double angle_degrees) {
  double rad = angle_degrees * 3.14159265358979323846 / 180.0;
  double c = cos(rad);
  double s = sin(rad);
  concat_matrix(c, s, -s, c, 0, 0);
}

void PageBuilder::concat_matrix(double a, double b, double c, double d,
                                double e, double f) {
  emit_number(a);
  emit_number(b);
  emit_number(c);
  emit_number(d);
  emit_number(e);
  emit_number(f);
  emit("cm\n");
}

void PageBuilder::set_stroke_color(double r, double g, double b) {
  emit_number(r);
  emit_number(g);
  emit_number(b);
  emit("RG\n");
}

void PageBuilder::set_fill_color(double r, double g, double b) {
  emit_number(r);
  emit_number(g);
  emit_number(b);
  emit("rg\n");
}

void PageBuilder::set_stroke_gray(double g) {
  emit_number(g);
  emit("G\n");
}

void PageBuilder::set_fill_gray(double g) {
  emit_number(g);
  emit("g\n");
}

void PageBuilder::set_line_width(double w) {
  emit_number(w);
  emit("w\n");
}

void PageBuilder::set_line_cap(int cap) {
  emit(std::to_string(cap) + " J\n");
}

void PageBuilder::set_line_join(int join) {
  emit(std::to_string(join) + " j\n");
}

void PageBuilder::set_dash_pattern(const std::vector<double>& pattern,
                                   double phase) {
  content_ += "[";
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (i > 0) content_ += " ";
    content_ += format_number(pattern[i]);
  }
  content_ += "] ";
  content_ += format_number(phase);
  content_ += " d\n";
}

void PageBuilder::move_to(double x, double y) {
  emit_number(x);
  emit_number(y);
  emit("m\n");
}

void PageBuilder::line_to(double x, double y) {
  emit_number(x);
  emit_number(y);
  emit("l\n");
}

void PageBuilder::curve_to(double x1, double y1, double x2, double y2,
                           double x3, double y3) {
  emit_number(x1);
  emit_number(y1);
  emit_number(x2);
  emit_number(y2);
  emit_number(x3);
  emit_number(y3);
  emit("c\n");
}

void PageBuilder::close_path() { emit("h\n"); }

void PageBuilder::stroke() { emit("S\n"); }

void PageBuilder::fill() { emit("f\n"); }

void PageBuilder::fill_even_odd() { emit("f*\n"); }

void PageBuilder::fill_stroke() { emit("B\n"); }

void PageBuilder::clip() { emit("W n\n"); }

void PageBuilder::clip_even_odd() { emit("W* n\n"); }

void PageBuilder::reset_transparency() {
  current_fill_alpha_ = 1.0;
  current_stroke_alpha_ = 1.0;
  current_blend_mode_ = BlendMode::Normal;
  std::string gs_name = get_or_create_extgstate(
      current_fill_alpha_, current_stroke_alpha_, current_blend_mode_);
  emit("/" + gs_name + " gs\n");
}

void PageBuilder::set_fill_gradient(const std::string& name) {
  emit("/Pattern cs\n");
  emit("/" + name + " scn\n");
}

void PageBuilder::set_stroke_gradient(const std::string& name) {
  emit("/Pattern CS\n");
  emit("/" + name + " SCN\n");
}

void PageBuilder::add_link(double x, double y, double w, double h, const std::string& uri) {
  LinkConfig config;
  config.x = x;
  config.y = y;
  config.width = w;
  config.height = h;
  config.action = LinkAction::URI;
  config.uri = uri;
  links_.push_back(config);
}

void PageBuilder::add_link(double x, double y, double w, double h, int dest_page, double dest_y) {
  LinkConfig config;
  config.x = x;
  config.y = y;
  config.width = w;
  config.height = h;
  config.action = LinkAction::GoTo;
  config.dest_page = dest_page;
  config.dest_y = dest_y;
  links_.push_back(config);
}

void PageBuilder::add_link(const LinkConfig& config) {
  links_.push_back(config);
}

// Helper function to get RGB color from HighlightColor preset
static void get_highlight_color(HighlightColor preset, double& r, double& g, double& b) {
  switch (preset) {
    case HighlightColor::Yellow:  r = 1.0; g = 1.0; b = 0.0; break;
    case HighlightColor::Green:   r = 0.0; g = 1.0; b = 0.0; break;
    case HighlightColor::Cyan:    r = 0.0; g = 1.0; b = 1.0; break;
    case HighlightColor::Magenta: r = 1.0; g = 0.0; b = 1.0; break;
    case HighlightColor::Red:     r = 1.0; g = 0.0; b = 0.0; break;
    case HighlightColor::Custom:
    default:
      // Leave r, g, b unchanged (caller should provide custom values)
      break;
  }
}

void PageBuilder::add_highlight(double x, double y, double width, double height,
                                HighlightColor color) {
  TextMarkupConfig config;
  config.type = MarkupType::Highlight;
  config.quads.push_back(quad_from_rect(x, y, width, height));
  get_highlight_color(color, config.r, config.g, config.b);
  highlights_.push_back(std::move(config));
}

void PageBuilder::add_highlight(const HighlightConfig& config) {
  TextMarkupConfig markup;
  markup.type = MarkupType::Highlight;
  markup.quads = config.quads;
  markup.alpha = config.alpha;
  markup.author = config.author;
  markup.contents = config.contents;
  markup.print = config.print;

  if (config.color_preset == HighlightColor::Custom) {
    markup.r = config.r;
    markup.g = config.g;
    markup.b = config.b;
  } else {
    get_highlight_color(config.color_preset, markup.r, markup.g, markup.b);
  }

  highlights_.push_back(std::move(markup));
}

void PageBuilder::add_highlight(const std::vector<QuadPoints>& quads,
                                HighlightColor color) {
  TextMarkupConfig config;
  config.type = MarkupType::Highlight;
  config.quads = quads;
  get_highlight_color(color, config.r, config.g, config.b);
  highlights_.push_back(std::move(config));
}

void PageBuilder::add_text_markup(const TextMarkupConfig& config) {
  highlights_.push_back(config);
}

void PageBuilder::end_layer() {
  emit("EMC\n");
}

// ============================================================================
// TableBuilder Implementation
// ============================================================================

TableBuilder::TableBuilder() {
  // Set default header style
  config_.header_style.has_background = true;
  config_.header_style.bg_r = 0.9;
  config_.header_style.bg_g = 0.9;
  config_.header_style.bg_b = 0.9;
  config_.header_style.align = CellAlign::Center;
}

void TableBuilder::set_position(double x, double y) {
  config_.x = x;
  config_.y = y;
}

void TableBuilder::set_width(double width) {
  config_.width = width;
}

void TableBuilder::set_column_widths(const std::vector<double>& widths) {
  config_.column_widths = widths;
}

void TableBuilder::set_font(const std::string& font_name, double font_size) {
  config_.font_name = font_name;
  config_.font_size = font_size;
}

void TableBuilder::set_text_color(double r, double g, double b) {
  config_.text_r = r;
  config_.text_g = g;
  config_.text_b = b;
}

void TableBuilder::set_header_style(const CellStyle& style) {
  config_.header_style = style;
}

void TableBuilder::set_border(double width, double r, double g, double b) {
  config_.border_width = width;
  config_.border_r = r;
  config_.border_g = g;
  config_.border_b = b;
}

void TableBuilder::set_outer_border(bool enabled) {
  config_.draw_outer_border = enabled;
}

void TableBuilder::set_inner_borders(bool enabled) {
  config_.draw_inner_borders = enabled;
}

void TableBuilder::set_alternating_rows(bool enabled, double r, double g, double b) {
  config_.alternate_row_colors = enabled;
  config_.alt_bg_r = r;
  config_.alt_bg_g = g;
  config_.alt_bg_b = b;
}

void TableBuilder::add_header_row(const std::vector<std::string>& cells) {
  TableRow row;
  for (const auto& text : cells) {
    WriterTableCell cell;
    cell.text = text;
    row.cells.push_back(cell);
  }
  add_header_row(row);
}

void TableBuilder::add_header_row(const std::vector<WriterTableCell>& cells) {
  TableRow row;
  row.cells = cells;
  add_header_row(row);
}

void TableBuilder::add_header_row(const TableRow& row) {
  config_.has_header = true;
  if (rows_.empty()) {
    rows_.push_back(row);
  } else {
    rows_.insert(rows_.begin(), row);
  }
}

void TableBuilder::add_row(const std::vector<std::string>& cells) {
  TableRow row;
  for (const auto& text : cells) {
    WriterTableCell cell;
    cell.text = text;
    row.cells.push_back(cell);
  }
  rows_.push_back(row);
}

void TableBuilder::add_row(const std::vector<WriterTableCell>& cells) {
  TableRow row;
  row.cells = cells;
  rows_.push_back(row);
}

void TableBuilder::add_row(const TableRow& row) {
  rows_.push_back(row);
}

double TableBuilder::calculate_height() const {
  double total = 0;
  double default_row_height = config_.font_size * 1.5 + 4;  // font + padding
  for (const auto& row : rows_) {
    total += (row.height > 0) ? row.height : default_row_height;
  }
  return total;
}

// ============================================================================
// PageBuilder::draw_table Implementation
// ============================================================================

void PageBuilder::draw_table(const TableBuilder& table) {
  const auto& cfg = table.config();
  const auto& rows = table.rows();

  if (rows.empty()) return;

  // Determine number of columns
  size_t num_cols = 0;
  for (const auto& row : rows) {
    size_t row_cols = 0;
    for (const auto& cell : row.cells) {
      row_cols += cell.colspan;
    }
    if (row_cols > num_cols) num_cols = row_cols;
  }
  if (num_cols == 0) return;

  // Calculate column widths
  std::vector<double> col_widths = cfg.column_widths;
  if (col_widths.size() < num_cols) {
    // Fill with default widths
    double default_width = (cfg.width > 0) ? cfg.width / num_cols : 80;
    while (col_widths.size() < num_cols) {
      col_widths.push_back(default_width);
    }
  }

  // Calculate total width
  double total_width = 0;
  for (double w : col_widths) total_width += w;

  // Calculate row heights
  double default_row_height = cfg.font_size * 1.5 + 4;
  std::vector<double> row_heights;
  for (const auto& row : rows) {
    row_heights.push_back((row.height > 0) ? row.height : default_row_height);
  }

  // Calculate total height
  double total_height = 0;
  for (double h : row_heights) total_height += h;

  // Start drawing from top-left (y is at top of table)
  double current_y = cfg.y + total_height;

  // Ensure the table has a usable font resource even when the caller relies on
  // the default table font.
  std::string default_font_name = cfg.font_name;
  if (default_font_name.empty() ||
      (writer_ && !writer_->has_font_resource(default_font_name))) {
    if (writer_) {
      default_font_name = writer_->add_standard_font(StandardFont::Helvetica);
    } else if (default_font_name.empty()) {
      default_font_name = "F1";
    }
  }

  // Draw each row
  for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
    const auto& row = rows[row_idx];
    double row_height = row_heights[row_idx];
    current_y -= row_height;

    // Determine if this is header row
    bool is_header = cfg.has_header && row_idx == 0;

    // Determine background color for this row
    bool draw_bg = false;
    double bg_r = 1, bg_g = 1, bg_b = 1;

    if (is_header && cfg.header_style.has_background) {
      draw_bg = true;
      bg_r = cfg.header_style.bg_r;
      bg_g = cfg.header_style.bg_g;
      bg_b = cfg.header_style.bg_b;
    } else if (cfg.alternate_row_colors && !is_header) {
      size_t data_row = cfg.has_header ? row_idx - 1 : row_idx;
      if (data_row % 2 == 1) {
        draw_bg = true;
        bg_r = cfg.alt_bg_r;
        bg_g = cfg.alt_bg_g;
        bg_b = cfg.alt_bg_b;
      }
    }

    // Draw cells
    double current_x = cfg.x;
    size_t col_idx = 0;

    for (const auto& cell : row.cells) {
      if (col_idx >= num_cols) break;

      // Calculate cell width (accounting for colspan)
      double cell_width = 0;
      for (int c = 0; c < cell.colspan && (col_idx + c) < col_widths.size(); ++c) {
        cell_width += col_widths[col_idx + c];
      }

      // Check for cell-specific background
      bool cell_has_bg = draw_bg || cell.style.has_background;
      double cell_bg_r = cell.style.has_background ? cell.style.bg_r : bg_r;
      double cell_bg_g = cell.style.has_background ? cell.style.bg_g : bg_g;
      double cell_bg_b = cell.style.has_background ? cell.style.bg_b : bg_b;

      // Draw cell background
      if (cell_has_bg) {
        save_state();
        set_fill_color(cell_bg_r, cell_bg_g, cell_bg_b);
        rectangle(current_x, current_y, cell_width, row_height);
        fill();
        restore_state();
      }

      // Draw cell text
      if (!cell.text.empty()) {
        const CellStyle default_cell_style;

        // Determine text properties
        std::string text_font_name = default_font_name;
        double text_size = cfg.font_size;
        double text_r = cfg.text_r;
        double text_g = cfg.text_g;
        double text_b = cfg.text_b;

        if (is_header) {
          if (!cfg.header_style.font_name.empty()) {
            text_font_name = cfg.header_style.font_name;
          }
          if (cfg.header_style.font_size > 0) {
            text_size = cfg.header_style.font_size;
            text_r = cfg.header_style.text_r;
            text_g = cfg.header_style.text_g;
            text_b = cfg.header_style.text_b;
          }
        }

        if (!row.row_style.font_name.empty()) {
          text_font_name = row.row_style.font_name;
        }
        if (row.row_style.font_size > 0) {
          text_size = row.row_style.font_size;
          text_r = row.row_style.text_r;
          text_g = row.row_style.text_g;
          text_b = row.row_style.text_b;
        }

        if (!cell.style.font_name.empty()) {
          text_font_name = cell.style.font_name;
        }
        if (cell.style.font_size > 0) {
          text_size = cell.style.font_size;
          text_r = cell.style.text_r;
          text_g = cell.style.text_g;
          text_b = cell.style.text_b;
        }

        if (writer_ && !writer_->has_font_resource(text_font_name)) {
          text_font_name = default_font_name;
        }

        CellAlign align = is_header ? cfg.header_style.align : default_cell_style.align;
        if (row.row_style.align != default_cell_style.align) {
          align = row.row_style.align;
        }
        if (cell.style.align != default_cell_style.align) {
          align = cell.style.align;
        }

        CellVAlign valign = is_header ? cfg.header_style.valign : default_cell_style.valign;
        if (row.row_style.valign != default_cell_style.valign) {
          valign = row.row_style.valign;
        }
        if (cell.style.valign != default_cell_style.valign) {
          valign = cell.style.valign;
        }

        double padding_left = is_header
                                  ? cfg.header_style.padding_left
                                  : default_cell_style.padding_left;
        double padding_right = is_header
                                   ? cfg.header_style.padding_right
                                   : default_cell_style.padding_right;
        double padding_top = is_header
                                 ? cfg.header_style.padding_top
                                 : default_cell_style.padding_top;
        double padding_bottom = is_header
                                    ? cfg.header_style.padding_bottom
                                    : default_cell_style.padding_bottom;

        if (row.row_style.padding_left != default_cell_style.padding_left) {
          padding_left = row.row_style.padding_left;
        }
        if (row.row_style.padding_right != default_cell_style.padding_right) {
          padding_right = row.row_style.padding_right;
        }
        if (row.row_style.padding_top != default_cell_style.padding_top) {
          padding_top = row.row_style.padding_top;
        }
        if (row.row_style.padding_bottom != default_cell_style.padding_bottom) {
          padding_bottom = row.row_style.padding_bottom;
        }
        if (cell.style.padding_left != default_cell_style.padding_left) {
          padding_left = cell.style.padding_left;
        }
        if (cell.style.padding_right != default_cell_style.padding_right) {
          padding_right = cell.style.padding_right;
        }
        if (cell.style.padding_top != default_cell_style.padding_top) {
          padding_top = cell.style.padding_top;
        }
        if (cell.style.padding_bottom != default_cell_style.padding_bottom) {
          padding_bottom = cell.style.padding_bottom;
        }

        // Calculate text position
        double text_x = current_x + padding_left;
        double text_width = cell.text.length() * text_size * 0.5;

        switch (align) {
          case CellAlign::Center:
            text_x = current_x + (cell_width - text_width) / 2;
            break;
          case CellAlign::Right:
            text_x = current_x + cell_width - text_width - padding_right;
            break;
          default:
            break;
        }

        double text_y = current_y + padding_bottom +
                        ((row_height - padding_top - padding_bottom - text_size) / 2);
        switch (valign) {
          case CellVAlign::Top:
            text_y = current_y + row_height - text_size - padding_top;
            break;
          case CellVAlign::Bottom:
            text_y = current_y + padding_bottom;
            break;
          case CellVAlign::Middle:
            break;
        }

        begin_text();
        set_font(text_font_name, text_size);
        emit(format_number(text_r) + " " + format_number(text_g) + " " +
             format_number(text_b) + " rg\n");
        text_position(text_x, text_y);
        show_text(cell.text);
        end_text();
      }

      current_x += cell_width;
      col_idx += cell.colspan;
    }
  }

  // Draw borders
  if (cfg.draw_outer_border || cfg.draw_inner_borders) {
    save_state();
    set_stroke_color(cfg.border_r, cfg.border_g, cfg.border_b);
    emit(format_number(cfg.border_width) + " w\n");

    // Draw outer border
    if (cfg.draw_outer_border) {
      rectangle(cfg.x, cfg.y, total_width, total_height);
      stroke();
    }

    // Draw inner borders
    if (cfg.draw_inner_borders) {
      // Vertical lines
      double x = cfg.x;
      for (size_t i = 0; i < col_widths.size() - 1; ++i) {
        x += col_widths[i];
        line(x, cfg.y, x, cfg.y + total_height);
        stroke();
      }

      // Horizontal lines
      double y = cfg.y + total_height;
      for (size_t i = 0; i < row_heights.size() - 1; ++i) {
        y -= row_heights[i];
        line(cfg.x, y, cfg.x + total_width, y);
        stroke();
      }
    }

    restore_state();
  }
}

void PageBuilder::draw_table(double x, double y, const std::vector<std::vector<std::string>>& data,
                             const std::vector<double>& column_widths) {
  if (data.empty()) return;

  TableBuilder table;
  table.set_position(x, y);

  if (!column_widths.empty()) {
    table.set_column_widths(column_widths);
  }

  for (const auto& row : data) {
    table.add_row(row);
  }

  draw_table(table);
}

// Note: The following methods are implemented after PdfWriter::Impl definition:
// - PageBuilder::get_or_create_extgstate
// - PageBuilder::set_fill_alpha
// - PageBuilder::set_stroke_alpha
// - PageBuilder::set_blend_mode
// - PageBuilder::begin_layer

void PageBuilder::rectangle(double x, double y, double w, double h) {
  emit_number(x);
  emit_number(y);
  emit_number(w);
  emit_number(h);
  emit("re\n");
}

void PageBuilder::line(double x1, double y1, double x2, double y2) {
  move_to(x1, y1);
  line_to(x2, y2);
  stroke();
}

void PageBuilder::circle(double cx, double cy, double r) {
  // Approximate circle with 4 Bezier curves
  const double k = 0.5522847498;  // Magic number for circle approximation
  double kx = k * r;
  double ky = k * r;

  move_to(cx + r, cy);
  curve_to(cx + r, cy + ky, cx + kx, cy + r, cx, cy + r);
  curve_to(cx - kx, cy + r, cx - r, cy + ky, cx - r, cy);
  curve_to(cx - r, cy - ky, cx - kx, cy - r, cx, cy - r);
  curve_to(cx + kx, cy - r, cx + r, cy - ky, cx + r, cy);
  close_path();
}

void PageBuilder::ellipse(double cx, double cy, double rx, double ry) {
  const double k = 0.5522847498;
  double kx = k * rx, ky = k * ry;
  move_to(cx + rx, cy);
  curve_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
  curve_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
  curve_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
  curve_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
  close_path();
}

void PageBuilder::arc(double cx, double cy, double rx, double ry,
                      double start_angle, double end_angle) {
  const double k = 0.5522847498;
  const double d2r = M_PI / 180.0;
  double start = start_angle * d2r;
  double end = end_angle * d2r;
  if (start == end) return;
  if (end < start) end += 2.0 * M_PI;
  double span = end - start;
  if (span >= 2.0 * M_PI) {
    ellipse(cx, cy, rx, ry);
    return;
  }
  int segments = static_cast<int>(std::ceil(span / (M_PI * 0.5)));
  double step = span / segments;
  double a = start;
  double x0 = cx + rx * std::cos(a);
  double y0 = cy + ry * std::sin(a);
  move_to(x0, y0);
  for (int i = 0; i < segments; ++i) {
    double a1 = a + step;
    double cosa = std::cos(a);
    double sina = std::sin(a);
    double cosa1 = std::cos(a1);
    double sina1 = std::sin(a1);
    double dx = rx * (cosa1 - cosa);
    double dy = ry * (sina1 - sina);
    double x1 = cx + rx * cosa;
    double y1 = cy + ry * sina;
    double x2 = cx + rx * cosa1;
    double y2 = cy + ry * sina1;
    double len = std::sqrt(dx * dx + dy * dy);
    double alpha = k * len / std::sqrt(rx * rx + ry * ry);
    alpha = std::min(alpha, k);
    double tx = -ry * cosa;
    double ty = rx * sina;
    double nlen = std::sqrt(tx * tx + ty * ty);
    if (nlen > 0) {
      tx /= nlen;
      ty /= nlen;
    } else {
      double ang = a + step * 0.5;
      tx = -ry * std::cos(ang);
      ty = rx * std::sin(ang);
      nlen = std::sqrt(tx * tx + ty * ty);
      if (nlen > 0) { tx /= nlen; ty /= nlen; }
    }
    double cp1x = x1 + alpha * rx * tx;
    double cp1y = y1 + alpha * ry * ty;
    double cp2x = x2 + alpha * rx * tx;
    double cp2y = y2 + alpha * ry * ty;
    curve_to(cp1x, cp1y, cp2x, cp2y, x2, y2);
    a = a1;
  }
}

void PageBuilder::rounded_rect(double x, double y, double w, double h, double r) {
  r = std::min(r, std::min(w, h) * 0.5);
  const double k = 0.5522847498;
  double k_r = k * r;
  move_to(x + r, y);
  line_to(x + w - r, y);
  curve_to(x + w - r + k_r, y, x + w, y + r - k_r, x + w, y + r);
  line_to(x + w, y + h - r);
  curve_to(x + w, y + h - r + k_r, x + w - r + k_r, y + h, x + w - r, y + h);
  line_to(x + r, y + h);
  curve_to(x + r - k_r, y + h, x, y + h - r + k_r, x, y + h - r);
  line_to(x, y + r);
  curve_to(x, y + r - k_r, x + r - k_r, y, x + r, y);
  close_path();
}

void PageBuilder::draw_image(const std::string& name, double x, double y,
                             double w, double h) {
  save_state();
  concat_matrix(w, 0, 0, h, x, y);
  content_ += "/" + name + " Do\n";
  restore_state();
}

void PageBuilder::append_raw(const std::string& raw_content) {
  content_ += raw_content;
}

void PageBuilder::add_resource_ref(const std::string& category,
                                   const std::string& name,
                                   int obj_id) {
  if (category.empty() || name.empty() || obj_id <= 0) {
    return;
  }

  CustomResourceRef resource;
  resource.category = category;
  resource.name = name;
  resource.obj_id = obj_id;
  custom_resources_.push_back(std::move(resource));
}

void PageBuilder::begin_text() { emit("BT\n"); }

void PageBuilder::end_text() { emit("ET\n"); }

void PageBuilder::set_font(const std::string& name, double size) {
  current_font_ = name;
  content_ += "/" + name + " ";
  emit_number(size);
  emit("Tf\n");
}

void PageBuilder::text_position(double x, double y) {
  emit_number(x);
  emit_number(y);
  emit("Td\n");
}

// encode_text_for_font delegates to PdfWriter's internal method
std::string PageBuilder::encode_text_for_font(const std::string& text) {
  if (!current_font_.empty()) {
    std::string encoded = writer_->encode_text_internal(current_font_, text);
    if (!encoded.empty()) {
      return encoded;
    }
  }
  // Standard font - use escaped string
  return "(" + escape_string(text) + ")";
}

void PageBuilder::show_text(const std::string& text) {
  content_ += encode_text_for_font(text) + " Tj\n";
}

void PageBuilder::show_text_at(double x, double y, const std::string& text) {
  emit_number(1);
  emit_number(0);
  emit_number(0);
  emit_number(1);
  emit_number(x);
  emit_number(y);
  emit("Tm\n");
  show_text(text);
}

// ============================================================================
// PdfWriter implementation
// ============================================================================

struct PdfWriter::Impl {
  // PDF version
  PdfVersion version = PdfVersion::v1_4;

  // Document ID (two 16-byte values)
  std::vector<uint8_t> doc_id1;
  std::vector<uint8_t> doc_id2;

  // Document metadata
  std::string title;
  std::string author;
  std::string subject;
  std::string keywords;
  std::string creator = "nanopdf";
  std::string producer = "nanopdf";
  std::map<std::string, std::string> custom_info;
  std::string creation_date;
  std::string modification_date;
  PageLabels page_labels;
  std::map<std::string, NamedDestination> named_destinations;
  std::string open_action_destination_name;
  PageLayout page_layout = PageLayout::Unset;
  PageMode page_mode = PageMode::Unset;
  bool has_viewer_preferences = false;
  ViewerPreferences viewer_preferences;
  std::string language;
  std::string xmp_metadata_xml;
  std::vector<OutputIntentConfig> output_intents;
  bool has_mark_info = false;
  MarkInfoConfig mark_info;
  TrappedState trapped = TrappedState::Unset;

  // Permissions (for certification signatures)
  bool has_permissions = false;
  MdpPermissions permissions = MdpPermissions::AnnotateFormFillSign;

  // ============================================================
  // Incremental Update (Revision) Support
  // ============================================================
  std::vector<uint8_t> existing_pdf;      // Original PDF data
  bool has_existing = false;               // True if loaded existing PDF
  size_t prev_xref_offset = 0;            // Previous xref offset (startxref)
  int existing_root_obj = 0;              // Root object from existing PDF
  int existing_root_gen = 0;              // Root generation number
  int existing_info_obj = 0;              // Info object from existing PDF
  int existing_info_gen = 0;              // Info generation number
  int revision_count = 0;                 // Number of revisions (0 for new)
  int existing_page_count = 0;            // Number of pages in existing PDF
  int existing_pages_obj = 0;             // Pages object from existing PDF
  std::vector<int> existing_page_objs;    // Page objects from existing PDF
  bool existing_encrypted = false;        // Existing PDF has an /Encrypt dict
  std::string existing_encrypt_ref;       // "N G R" of the /Encrypt dictionary

  // Object tracking
  int next_obj_id = 1;
  std::vector<size_t> obj_offsets;

  // Resources
  struct ImageResource {
    std::string name;
    int obj_id;
    ImageData data;
    ImageCompression compression = ImageCompression::Auto;
    // For CCITT images:
    std::vector<uint8_t> ccitt_data;  // Pre-encoded CCITT data
    int ccitt_width = 0;
    int ccitt_height = 0;
    bool is_ccitt_only = false;  // True if this is a CCITT-only image (no ImageData)
    // For soft mask (alpha channel):
    int smask_obj_id = 0;          // SMask XObject ID (0 = no mask)
    std::vector<uint8_t> alpha_data;  // Alpha channel data (grayscale)
    bool has_alpha = false;        // True if this image has an alpha channel
  };
  std::vector<ImageResource> images;
  int image_counter = 0;

  struct FontResource {
    std::string name;
    int obj_id;
    StandardFont font;
  };
  std::vector<FontResource> fonts;
  int font_counter = 0;

  // Embedded TrueType fonts
  struct EmbeddedFontResource {
    std::string name;                      // Resource name (e.g., "F2")
    FontData font_data;                    // Font file and metrics
    FontEmbedding embedding;               // Embedding mode
    int font_obj_id = 0;                   // Font dictionary object
    int descriptor_obj_id = 0;             // FontDescriptor object
    int file_obj_id = 0;                   // FontFile2 stream object
    int tounicode_obj_id = 0;              // ToUnicode CMap object
    int cidset_obj_id = 0;                 // CIDSet stream object (for subsetting)
    int descendant_obj_id = 0;             // CIDFont descendant object (for Type0)
    std::set<uint32_t> used_chars;         // Characters used (for subsetting)
    std::string subset_tag;                // 6-letter subset tag (e.g., "ABCDEF+")
  };
  std::vector<EmbeddedFontResource> embedded_fonts;

  // Link Annotations (defined here so PageData can reference it)
  struct LinkAnnotationData {
    double x, y, width, height;
    LinkAction action;
    std::string uri;
    int dest_page = 0;
    double dest_y = 0;
    bool show_border = false;
    int obj_id = 0;
  };

  // Highlight/Markup Annotations
  struct HighlightAnnotationData {
    MarkupType type = MarkupType::Highlight;
    std::vector<QuadPoints> quads;
    double r = 1.0, g = 1.0, b = 0.0;  // Color
    double alpha = 0.5;
    std::string author;
    std::string contents;
    bool print = true;
    int obj_id = 0;
  };

  // Pages
  struct CustomResourceRef {
    std::string category;
    std::string name;
    int obj_id = 0;
  };

  struct PageData {
    PageSize size;
    std::string content;
    std::vector<std::string> used_images;
    std::vector<std::string> used_fonts;
    std::vector<std::string> used_extgstates;   // ExtGState resources
    std::vector<std::string> used_layers;       // Layer (OCG) resources
    std::vector<std::string> used_patterns;     // Pattern (gradient) resources
    std::vector<std::string> used_templates;    // Template (Form XObject) resources
    std::vector<CustomResourceRef> custom_resources;
    std::vector<int> annotation_refs;  // Annotation object references for this page
    std::vector<LinkAnnotationData> links;  // Link annotations for this page
    std::vector<HighlightAnnotationData> highlights;  // Highlight annotations for this page
  };
  std::vector<PageData> pages;

  // Signature fields
  struct SignatureFieldData {
    SignatureFieldConfig config;
    int field_obj_id = 0;
    int sig_obj_id = 0;
    int widget_obj_id = 0;
    int appearance_obj_id = 0;
  };
  std::vector<SignatureFieldData> signature_fields;
  bool has_acroform = false;
  int acroform_obj_id = 0;

  // Form field updates (for fill & save on existing PDFs)
  struct FieldUpdate {
    std::string field_name;
    std::string value;           // PDF value string (e.g. "(text)" or "/Yes")
    uint32_t object_number{0};   // Original object number in existing PDF
    uint16_t generation{0};
  };
  std::vector<FieldUpdate> field_updates;

  // Annotation updates for existing PDFs
  enum class AnnotationUpdateType {
    AddText,
    AddHighlight,
    AddTextMarkup,
    AddLink,
    Delete
  };

  struct AnnotationUpdate {
    AnnotationUpdateType update_type;
    int page_index{0};

    // For AddText:
    double x{0}, y{0}, w{0}, h{0};
    std::string contents;

    // For AddHighlight:
    HighlightConfig highlight_config;

    // For AddTextMarkup:
    TextMarkupConfig text_markup_config;

    // For AddLink:
    LinkConfig link_config;

    // For Delete:
    int annot_index{0};
  };
  std::vector<AnnotationUpdate> annotation_updates;

  // Signature placeholders (filled after writing)
  std::vector<SignaturePlaceholder> sig_placeholders;
  size_t sig_content_length = 8192;  // Reserved space for signature

  // ============================================================
  // Encryption Support
  // ============================================================
  bool encryption_enabled = false;
  EncryptionConfig encryption_config;
  std::vector<uint8_t> encryption_key;     // File encryption key
  std::vector<uint8_t> o_value;            // Owner password hash (32 bytes)
  std::vector<uint8_t> u_value;            // User password hash (32 bytes)
  std::vector<uint8_t> oe_value;           // Owner encrypted key (AES-256)
  std::vector<uint8_t> ue_value;           // User encrypted key (AES-256)
  std::vector<uint8_t> perms_value;        // Encrypted permissions (AES-256)
  int encrypt_obj_id = 0;                  // Encryption dictionary object ID

  // ============================================================
  // Timestamp Support
  // ============================================================
  bool timestamp_enabled = false;
  TimestampConfig timestamp_config;

  // ============================================================
  // Bookmarks/Outlines
  // ============================================================
  struct OutlineItem {
    std::string title;
    int dest_page = 0;
    double dest_y = 0;
    int parent_id = -1;      // -1 = root
    int first_child_id = -1;
    int last_child_id = -1;
    int prev_id = -1;
    int next_id = -1;
    int child_count = 0;     // Negative if closed
    bool open = true;
    bool bold = false;
    bool italic = false;
    int obj_id = 0;
  };
  std::vector<OutlineItem> outlines;
  int outlines_obj_id = 0;

  // ============================================================
  // Attachments
  // ============================================================
  struct AttachmentData {
    std::string filename;
    std::string description;
    std::string mime_type;
    std::vector<uint8_t> data;
    bool compress = true;
    int filespec_obj_id = 0;
    int embedded_file_obj_id = 0;
  };
  std::vector<AttachmentData> attachments;

  // ============================================================
  // Layers (OCG)
  // ============================================================
  struct LayerData {
    std::string name;
    std::string internal_name;  // MC0, MC1, etc.
    bool visible = true;
    bool printable = true;
    bool locked = false;
    int obj_id = 0;
  };
  std::vector<LayerData> layers;
  int layer_counter = 0;

  // ============================================================
  // Extended Graphics States (for transparency)
  // ============================================================
  struct ExtGStateData {
    double fill_alpha = 1.0;
    double stroke_alpha = 1.0;
    BlendMode blend_mode = BlendMode::Normal;
    std::string name;  // GS1, GS2, etc.
    int obj_id = 0;
  };
  std::vector<ExtGStateData> extgstates;
  int extgstate_counter = 0;

  // ============================================================
  // Form Fields (additional to signature fields)
  // ============================================================
  struct FormFieldData {
    enum class Type { Text, Checkbox, Radio, Dropdown, Listbox, Button };
    Type type;
    std::string name;
    int page = 0;
    double x = 0, y = 0, width = 0, height = 0;
    std::string value;
    std::vector<std::string> options;
    bool checked = false;
    int selected = 0;
    bool multiline = false;
    bool password = false;
    bool read_only = false;
    bool required = false;
    bool editable = false;  // For combo box
    int max_length = 0;
    double font_size = 12;
    std::string font_name;
    std::string export_value;
    std::string caption;  // For buttons
    int field_obj_id = 0;
    int appearance_obj_id = 0;
    std::vector<int> radio_widget_ids;  // For radio groups
  };
  std::vector<FormFieldData> form_fields;

  // ============================================================
  // Gradients
  // ============================================================
  struct GradientData {
    std::string name;  // P1, P2, etc.
    GradientType type;
    double x1, y1, x2, y2;  // Linear
    double cx, cy, r;       // Radial
    std::vector<ColorStop> stops;
    bool extend_start = false;
    bool extend_end = false;
    int shading_obj_id = 0;
    int pattern_obj_id = 0;
    int function_obj_id = 0;
  };
  std::vector<GradientData> gradients;
  int gradient_counter = 0;

  // ============================================================
  // Page Templates (Form XObjects)
  // ============================================================
  struct TemplateData {
    std::string name;  // Fm1, Fm2, etc.
    double width, height;
    std::string content;
    std::vector<std::string> used_images;
    std::vector<std::string> used_fonts;
    std::vector<std::string> used_extgstates;
    std::vector<std::string> used_layers;
    std::vector<std::string> used_patterns;
    std::vector<std::string> used_templates;
    std::vector<CustomResourceRef> custom_resources;
    int obj_id = 0;

    // For imported pages: pre-built resource dictionary string and
    // associated imported object IDs. When is_imported is true,
    // the resources are written from imported_resources_str instead of
    // looking up fonts/images in the writer's registry.
    bool is_imported = false;
    std::string imported_resources_str;  // Serialized /Resources dict content
  };
  std::vector<TemplateData> templates;
  int template_counter = 0;

  // ============================================================
  // Imported PDF Objects (for merge/split)
  // ============================================================
  // Each imported object holds raw PDF object data to be written verbatim.
  struct ImportedObject {
    int obj_id = 0;               // Object ID in the new PDF
    std::string serialized_data;  // Complete serialized PDF object body
                                  // (everything between "N 0 obj\n" and "endobj\n")
    // For stream objects:
    bool is_stream = false;
    std::string stream_dict;          // Dictionary portion (including << >>)
    std::vector<uint8_t> stream_data; // Raw stream bytes
  };
  std::vector<ImportedObject> imported_objects;

  // ============================================================
  // Watermarks
  // ============================================================
  struct WatermarkData {
    bool is_text = true;  // true = text, false = image
    // Text watermark
    TextWatermarkConfig text_config;
    // Image watermark
    ImageWatermarkConfig image_config;
    // Computed
    std::string template_name;  // Form XObject name
    bool active = false;
  };
  WatermarkData global_watermark;
  std::map<int, WatermarkData> page_watermarks;  // Per-page watermarks

  // ============================================================
  // Headers/Footers
  // ============================================================
  struct HeaderFooterData {
    HeaderConfig header;
    FooterConfig footer;
    bool has_header = false;
    bool has_footer = false;
  };
  HeaderFooterData global_header_footer;
  std::map<int, HeaderFooterData> page_header_footers;  // Per-page overrides
  std::set<int> skip_header_footer_pages;  // Pages to skip header/footer

  // ============================================================
  // Bates Stamping
  // ============================================================
  BatesConfig bates_config;
  bool has_bates = false;
  std::set<int> skip_bates_pages;  // Pages to skip Bates number

  // Output buffer
  std::ostringstream output;
  int current_obj_id = 0;

  int allocate_obj() { return next_obj_id++; }

  void write_obj_start(int obj_id) {
    while (obj_offsets.size() <= static_cast<size_t>(obj_id)) {
      obj_offsets.push_back(0);
    }
    obj_offsets[obj_id] = output.tellp();
    current_obj_id = obj_id;
    output << obj_id << " 0 obj\n";
  }

  void write_obj_end() {
    output << "endobj\n";
    current_obj_id = 0;
  }

  std::string format_pdf_string_for_object(
      int obj_id, const std::string& plain_text) const {
    if (!encryption_enabled || obj_id <= 0 || obj_id == encrypt_obj_id) {
      return "(" + escape_pdf_string(plain_text) + ")";
    }

    std::vector<uint8_t> plain_bytes(plain_text.begin(), plain_text.end());
    std::vector<uint8_t> encrypted_bytes = encrypt_data(
        plain_bytes, encryption_key, obj_id, 0, encryption_config.algorithm);
    return "<" + bytes_to_hex(encrypted_bytes) + ">";
  }

  std::string format_pdf_string(const std::string& plain_text) const {
    return format_pdf_string_for_object(current_obj_id, plain_text);
  }

  std::vector<uint8_t> prepare_stream_data(
      int obj_id, const std::vector<uint8_t>& plain_data) const {
    if (!encryption_enabled || obj_id == encrypt_obj_id) {
      return plain_data;
    }
    return encrypt_data(
        plain_data, encryption_key, obj_id, 0, encryption_config.algorithm);
  }

  std::string get_image_resource(const std::string& name) {
    for (const auto& img : images) {
      if (img.name == name) {
        return std::to_string(img.obj_id) + " 0 R";
      }
    }
    return "";
  }

  std::string get_font_resource(const std::string& name) {
    for (const auto& font : fonts) {
      if (font.name == name) {
        return std::to_string(font.obj_id) + " 0 R";
      }
    }
    // Check embedded fonts
    for (const auto& ef : embedded_fonts) {
      if (ef.name == name) {
        return std::to_string(ef.font_obj_id) + " 0 R";
      }
    }
    return "";
  }

  // Check if a font name is an embedded font
  bool is_embedded_font(const std::string& name) {
    for (const auto& ef : embedded_fonts) {
      if (ef.name == name) {
        return true;
      }
    }
    return false;
  }

  // Get embedded font by name
  EmbeddedFontResource* get_embedded_font(const std::string& name) {
    for (auto& ef : embedded_fonts) {
      if (ef.name == name) {
        return &ef;
      }
    }
    return nullptr;
  }

  // ============================================================
  // Watermark rendering helper
  // ============================================================
  std::string generate_watermark_content(const WatermarkData& wm, double page_width, double page_height,
                                         std::set<std::string>& used_fonts,
                                         std::set<std::string>& used_extgstates,
                                         std::set<std::string>& used_images) {
    if (!wm.active) return "";

    std::ostringstream content;

    if (wm.is_text) {
      const auto& cfg = wm.text_config;

      // Get or create ExtGState for transparency
      std::string gs_name;
      for (const auto& gs : extgstates) {
        if (std::abs(gs.fill_alpha - cfg.alpha) < 0.001 &&
            std::abs(gs.stroke_alpha - 1.0) < 0.001 &&
            gs.blend_mode == BlendMode::Normal) {
          gs_name = gs.name;
          break;
        }
      }
      if (gs_name.empty()) {
        extgstate_counter++;
        gs_name = "GS" + std::to_string(extgstate_counter);
        ExtGStateData gs;
        gs.name = gs_name;
        gs.obj_id = allocate_obj();
        gs.fill_alpha = cfg.alpha;
        gs.stroke_alpha = 1.0;
        gs.blend_mode = BlendMode::Normal;
        extgstates.push_back(gs);
      }
      used_extgstates.insert(gs_name);

      // Get font name - use Helvetica if not specified
      std::string font_name = cfg.font_name.empty() ? "F1" : cfg.font_name;
      // Check if F1 exists, if not add Helvetica
      bool font_exists = false;
      for (const auto& f : fonts) {
        if (f.name == font_name) {
          font_exists = true;
          break;
        }
      }
      if (!font_exists && font_name == "F1") {
        // Add Helvetica as F1
        FontResource fr;
        fr.name = "F1";
        fr.font = StandardFont::Helvetica;
        fr.obj_id = allocate_obj();
        fonts.push_back(fr);
      }
      used_fonts.insert(font_name);

      // Calculate position based on WatermarkPosition
      double cx = page_width / 2;
      double cy = page_height / 2;
      switch (cfg.position) {
        case WatermarkPosition::TopLeft:
          cx = 100;
          cy = page_height - 100;
          break;
        case WatermarkPosition::TopRight:
          cx = page_width - 100;
          cy = page_height - 100;
          break;
        case WatermarkPosition::BottomLeft:
          cx = 100;
          cy = 100;
          break;
        case WatermarkPosition::BottomRight:
          cx = page_width - 100;
          cy = 100;
          break;
        case WatermarkPosition::Center:
        default:
          cx = page_width / 2;
          cy = page_height / 2;
          break;
      }

      // Convert rotation to radians
      double rad = cfg.rotation * 3.14159265358979 / 180.0;
      double cos_r = std::cos(rad);
      double sin_r = std::sin(rad);

      // Estimate text width for centering
      double text_width = cfg.text.length() * cfg.font_size * 0.5;

      // Build content stream
      content << "q\n";  // Save state
      content << "/" << gs_name << " gs\n";  // Set transparency

      // Transform: translate to center, rotate, translate back for text centering
      content << "1 0 0 1 " << format_number(cx) << " " << format_number(cy) << " cm\n";
      content << format_number(cos_r) << " " << format_number(sin_r) << " "
              << format_number(-sin_r) << " " << format_number(cos_r) << " 0 0 cm\n";

      content << "BT\n";
      content << "/" << font_name << " " << format_number(cfg.font_size) << " Tf\n";
      content << format_number(cfg.r) << " " << format_number(cfg.g) << " "
              << format_number(cfg.b) << " rg\n";
      content << format_number(-text_width / 2) << " 0 Td\n";
      content << "(" << cfg.text << ") Tj\n";
      content << "ET\n";
      content << "Q\n";  // Restore state
    } else {
      // Image watermark
      const auto& cfg = wm.image_config;

      // Find the image resource
      ImageResource* img_res = nullptr;
      for (auto& img : images) {
        if (img.name == cfg.image_name) {
          img_res = &img;
          break;
        }
      }
      if (!img_res) return "";  // Image not found

      used_images.insert(cfg.image_name);

      // Get or create ExtGState for transparency
      std::string gs_name;
      for (const auto& gs : extgstates) {
        if (std::abs(gs.fill_alpha - cfg.alpha) < 0.001 &&
            std::abs(gs.stroke_alpha - cfg.alpha) < 0.001 &&
            gs.blend_mode == BlendMode::Normal) {
          gs_name = gs.name;
          break;
        }
      }
      if (gs_name.empty()) {
        extgstate_counter++;
        gs_name = "GS" + std::to_string(extgstate_counter);
        ExtGStateData gs;
        gs.name = gs_name;
        gs.obj_id = allocate_obj();
        gs.fill_alpha = cfg.alpha;
        gs.stroke_alpha = cfg.alpha;
        gs.blend_mode = BlendMode::Normal;
        extgstates.push_back(gs);
      }
      used_extgstates.insert(gs_name);

      // Get image dimensions
      int img_width = img_res->is_ccitt_only ? img_res->ccitt_width : img_res->data.width;
      int img_height = img_res->is_ccitt_only ? img_res->ccitt_height : img_res->data.height;

      // Calculate scaled dimensions
      double scaled_width = img_width * cfg.scale;
      double scaled_height = img_height * cfg.scale;

      // Calculate position based on WatermarkPosition
      double x = 0, y = 0;
      switch (cfg.position) {
        case WatermarkPosition::TopLeft:
          x = cfg.offset_x;
          y = page_height - scaled_height - cfg.offset_y;
          break;
        case WatermarkPosition::TopRight:
          x = page_width - scaled_width - cfg.offset_x;
          y = page_height - scaled_height - cfg.offset_y;
          break;
        case WatermarkPosition::BottomLeft:
          x = cfg.offset_x;
          y = cfg.offset_y;
          break;
        case WatermarkPosition::BottomRight:
          x = page_width - scaled_width - cfg.offset_x;
          y = cfg.offset_y;
          break;
        case WatermarkPosition::Tiled:
          // Tiled watermark - draw multiple copies
          {
            content << "q\n";
            content << "/" << gs_name << " gs\n";
            double tile_spacing_x = scaled_width + 50;
            double tile_spacing_y = scaled_height + 50;
            for (double ty = cfg.offset_y; ty < page_height; ty += tile_spacing_y) {
              for (double tx = cfg.offset_x; tx < page_width; tx += tile_spacing_x) {
                content << "q\n";
                content << format_number(scaled_width) << " 0 0 "
                        << format_number(scaled_height) << " "
                        << format_number(tx) << " " << format_number(ty) << " cm\n";
                content << "/" << cfg.image_name << " Do\n";
                content << "Q\n";
              }
            }
            content << "Q\n";
            return content.str();
          }
        case WatermarkPosition::Center:
        default:
          x = (page_width - scaled_width) / 2 + cfg.offset_x;
          y = (page_height - scaled_height) / 2 + cfg.offset_y;
          break;
      }

      // Build content stream for single image
      content << "q\n";
      content << "/" << gs_name << " gs\n";
      content << format_number(scaled_width) << " 0 0 "
              << format_number(scaled_height) << " "
              << format_number(x) << " " << format_number(y) << " cm\n";
      content << "/" << cfg.image_name << " Do\n";
      content << "Q\n";
    }

    return content.str();
  }

  // ============================================================
  // Header/Footer rendering helper
  // ============================================================
  std::string generate_header_footer_content(int page_index, int total_pages,
                                             double page_width, double page_height,
                                             std::set<std::string>& used_fonts) {
    // Check if this page should skip header/footer
    if (skip_header_footer_pages.count(page_index) > 0) {
      return "";
    }

    // Get header/footer config - page-specific overrides global
    HeaderFooterData hf_data = global_header_footer;
    auto it = page_header_footers.find(page_index);
    if (it != page_header_footers.end()) {
      if (it->second.has_header) {
        hf_data.header = it->second.header;
        hf_data.has_header = true;
      }
      if (it->second.has_footer) {
        hf_data.footer = it->second.footer;
        hf_data.has_footer = true;
      }
    }

    if (!hf_data.has_header && !hf_data.has_footer) {
      return "";
    }

    std::ostringstream content;

    auto expand_text = [&](const HFSection& section) -> std::string {
      if (section.type == HeaderFooterContent::None) {
        return "";
      }
      if (section.type == HeaderFooterContent::PageNumber) {
        return std::to_string(page_index + 1);
      }
      if (section.type == HeaderFooterContent::TotalPages) {
        return std::to_string(total_pages);
      }
      if (section.type == HeaderFooterContent::Date) {
        // Get current date
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char buffer[64];
        strftime(buffer, 64, section.date_format.empty() ? "%Y-%m-%d" : section.date_format.c_str(), tm_info);
        return buffer;
      }
      // Text type - expand %d and %D placeholders
      std::string result = section.text;
      size_t pos;
      while ((pos = result.find("%d")) != std::string::npos) {
        result.replace(pos, 2, std::to_string(page_index + 1));
      }
      while ((pos = result.find("%D")) != std::string::npos) {
        result.replace(pos, 2, std::to_string(total_pages));
      }
      return result;
    };

    // Ensure font exists
    std::string font_name = "F1";
    bool font_exists = false;
    for (const auto& f : fonts) {
      if (f.name == font_name) {
        font_exists = true;
        break;
      }
    }
    if (!font_exists) {
      FontResource fr;
      fr.name = "F1";
      fr.font = StandardFont::Helvetica;
      fr.obj_id = allocate_obj();
      fonts.push_back(fr);
    }
    used_fonts.insert(font_name);

    // Draw header
    if (hf_data.has_header) {
      const auto& hdr = hf_data.header;
      double y = page_height - hdr.margin_top;
      double left_x = hdr.margin_left;
      double right_x = page_width - hdr.margin_right;
      double center_x = page_width / 2;

      content << "q\n";
      content << "BT\n";
      content << "/" << font_name << " " << format_number(hdr.font_size) << " Tf\n";
      content << format_number(hdr.r) << " " << format_number(hdr.g) << " "
              << format_number(hdr.b) << " rg\n";

      // Left text
      std::string left_text = expand_text(hdr.left);
      if (!left_text.empty()) {
        content << format_number(left_x) << " " << format_number(y) << " Td\n";
        content << "(" << left_text << ") Tj\n";
        content << format_number(-left_x) << " " << format_number(-y) << " Td\n";  // Reset
      }

      // Center text
      std::string center_text = expand_text(hdr.center);
      if (!center_text.empty()) {
        double text_width = center_text.length() * hdr.font_size * 0.5;
        content << format_number(center_x - text_width / 2) << " " << format_number(y) << " Td\n";
        content << "(" << center_text << ") Tj\n";
        content << format_number(-(center_x - text_width / 2)) << " " << format_number(-y) << " Td\n";
      }

      // Right text
      std::string right_text = expand_text(hdr.right);
      if (!right_text.empty()) {
        double text_width = right_text.length() * hdr.font_size * 0.5;
        content << format_number(right_x - text_width) << " " << format_number(y) << " Td\n";
        content << "(" << right_text << ") Tj\n";
      }

      content << "ET\n";

      // Draw separator line
      if (hdr.draw_line) {
        content << format_number(hdr.line_width) << " w\n";
        content << format_number(hdr.r) << " " << format_number(hdr.g) << " "
                << format_number(hdr.b) << " RG\n";
        content << format_number(left_x) << " " << format_number(y - 5) << " m\n";
        content << format_number(right_x) << " " << format_number(y - 5) << " l\n";
        content << "S\n";
      }
      content << "Q\n";
    }

    // Draw footer
    if (hf_data.has_footer) {
      const auto& ftr = hf_data.footer;
      double y = ftr.margin_bottom;
      double left_x = ftr.margin_left;
      double right_x = page_width - ftr.margin_right;
      double center_x = page_width / 2;

      content << "q\n";
      content << "BT\n";
      content << "/" << font_name << " " << format_number(ftr.font_size) << " Tf\n";
      content << format_number(ftr.r) << " " << format_number(ftr.g) << " "
              << format_number(ftr.b) << " rg\n";

      // Left text
      std::string left_text = expand_text(ftr.left);
      if (!left_text.empty()) {
        content << format_number(left_x) << " " << format_number(y) << " Td\n";
        content << "(" << left_text << ") Tj\n";
        content << format_number(-left_x) << " " << format_number(-y) << " Td\n";
      }

      // Center text
      std::string center_text = expand_text(ftr.center);
      if (!center_text.empty()) {
        double text_width = center_text.length() * ftr.font_size * 0.5;
        content << format_number(center_x - text_width / 2) << " " << format_number(y) << " Td\n";
        content << "(" << center_text << ") Tj\n";
        content << format_number(-(center_x - text_width / 2)) << " " << format_number(-y) << " Td\n";
      }

      // Right text
      std::string right_text = expand_text(ftr.right);
      if (!right_text.empty()) {
        double text_width = right_text.length() * ftr.font_size * 0.5;
        content << format_number(right_x - text_width) << " " << format_number(y) << " Td\n";
        content << "(" << right_text << ") Tj\n";
      }

      content << "ET\n";

      // Draw separator line
      if (ftr.draw_line) {
        content << format_number(ftr.line_width) << " w\n";
        content << format_number(ftr.r) << " " << format_number(ftr.g) << " "
                << format_number(ftr.b) << " RG\n";
        content << format_number(left_x) << " " << format_number(y + ftr.font_size + 3) << " m\n";
        content << format_number(right_x) << " " << format_number(y + ftr.font_size + 3) << " l\n";
        content << "S\n";
      }
      content << "Q\n";
    }

    return content.str();
  }

  // ============================================================
  // Bates Stamping rendering helper
  // ============================================================
  std::string generate_bates_content(int page_index, double page_width, double page_height,
                                     std::set<std::string>& used_fonts) {
    if (!has_bates) return "";
    if (skip_bates_pages.count(page_index) > 0) return "";

    const auto& cfg = bates_config;

    // Calculate Bates number for this page
    int bates_number = cfg.start_number + (page_index * cfg.increment);

    // Format the number with zero-padding
    std::ostringstream num_stream;
    num_stream << std::setfill('0') << std::setw(cfg.digits) << bates_number;
    std::string bates_text = cfg.prefix + num_stream.str() + cfg.suffix;

    // Ensure font exists
    std::string font_name = cfg.font_name.empty() ? "F1" : cfg.font_name;
    bool font_exists = false;
    for (const auto& f : fonts) {
      if (f.name == font_name) {
        font_exists = true;
        break;
      }
    }
    if (!font_exists && font_name == "F1") {
      FontResource fr;
      fr.name = "F1";
      fr.font = StandardFont::Helvetica;
      fr.obj_id = allocate_obj();
      fonts.push_back(fr);
    }
    used_fonts.insert(font_name);

    // Calculate position based on BatesPosition
    double x = 0, y = 0;
    double text_width = bates_text.length() * cfg.font_size * 0.5;

    switch (cfg.position) {
      case BatesPosition::TopLeft:
        x = cfg.margin_x;
        y = page_height - cfg.margin_y;
        break;
      case BatesPosition::TopCenter:
        x = (page_width - text_width) / 2;
        y = page_height - cfg.margin_y;
        break;
      case BatesPosition::TopRight:
        x = page_width - cfg.margin_x - text_width;
        y = page_height - cfg.margin_y;
        break;
      case BatesPosition::BottomLeft:
        x = cfg.margin_x;
        y = cfg.margin_y;
        break;
      case BatesPosition::BottomCenter:
        x = (page_width - text_width) / 2;
        y = cfg.margin_y;
        break;
      case BatesPosition::BottomRight:
      default:
        x = page_width - cfg.margin_x - text_width;
        y = cfg.margin_y;
        break;
    }

    // Build content stream
    std::ostringstream content;
    content << "q\n";
    content << "BT\n";
    content << "/" << font_name << " " << format_number(cfg.font_size) << " Tf\n";
    content << format_number(cfg.r) << " " << format_number(cfg.g) << " "
            << format_number(cfg.b) << " rg\n";
    content << format_number(x) << " " << format_number(y) << " Td\n";
    content << "(" << bates_text << ") Tj\n";
    content << "ET\n";
    content << "Q\n";

    return content.str();
  }
};

PdfWriter::PdfWriter() : impl_(new Impl()) {}

PdfWriter::~PdfWriter() { delete impl_; }

// ============================================================
// PageBuilder methods that depend on PdfWriter::Impl
// ============================================================

namespace {
const char* get_blend_mode_name(BlendMode mode) {
  switch (mode) {
    case BlendMode::Normal: return "Normal";
    case BlendMode::Multiply: return "Multiply";
    case BlendMode::Screen: return "Screen";
    case BlendMode::Overlay: return "Overlay";
    case BlendMode::Darken: return "Darken";
    case BlendMode::Lighten: return "Lighten";
    case BlendMode::ColorDodge: return "ColorDodge";
    case BlendMode::ColorBurn: return "ColorBurn";
    case BlendMode::HardLight: return "HardLight";
    case BlendMode::SoftLight: return "SoftLight";
    case BlendMode::Difference: return "Difference";
    case BlendMode::Exclusion: return "Exclusion";
    default: return "Normal";
  }
}
}  // namespace

std::string PageBuilder::get_or_create_extgstate(double fill_alpha, double stroke_alpha, BlendMode mode) {
  // Check if we already have this ExtGState
  for (const auto& gs : writer_->impl_->extgstates) {
    if (std::abs(gs.fill_alpha - fill_alpha) < 0.001 &&
        std::abs(gs.stroke_alpha - stroke_alpha) < 0.001 &&
        gs.blend_mode == mode) {
      return gs.name;
    }
  }

  // Create new ExtGState
  writer_->impl_->extgstate_counter++;
  std::string name = "GS" + std::to_string(writer_->impl_->extgstate_counter);

  PdfWriter::Impl::ExtGStateData gs;
  gs.name = name;
  gs.fill_alpha = fill_alpha;
  gs.stroke_alpha = stroke_alpha;
  gs.blend_mode = mode;
  gs.obj_id = writer_->impl_->allocate_obj();

  writer_->impl_->extgstates.push_back(gs);
  return name;
}

void PageBuilder::set_fill_alpha(double alpha) {
  current_fill_alpha_ = std::max(0.0, std::min(1.0, alpha));
  std::string gs_name = get_or_create_extgstate(current_fill_alpha_, current_stroke_alpha_, current_blend_mode_);
  emit("/" + gs_name + " gs\n");
}

void PageBuilder::set_stroke_alpha(double alpha) {
  current_stroke_alpha_ = std::max(0.0, std::min(1.0, alpha));
  std::string gs_name = get_or_create_extgstate(current_fill_alpha_, current_stroke_alpha_, current_blend_mode_);
  emit("/" + gs_name + " gs\n");
}

void PageBuilder::set_blend_mode(BlendMode mode) {
  current_blend_mode_ = mode;
  std::string gs_name = get_or_create_extgstate(current_fill_alpha_, current_stroke_alpha_, current_blend_mode_);
  emit("/" + gs_name + " gs\n");
}

void PageBuilder::begin_layer(const std::string& layer_name) {
  // Find the layer by name
  for (const auto& layer : writer_->impl_->layers) {
    if (layer.name == layer_name) {
      emit("/OC /" + layer.internal_name + " BDC\n");
      return;
    }
  }
  // Layer not found - use generic name
  emit("/OC /MC0 BDC\n");
}

std::string PdfWriter::encode_text_internal(const std::string& font_name,
                                             const std::string& text) {
  // Check if this is an embedded font
  if (!impl_->is_embedded_font(font_name)) {
    return "";  // Not an embedded font, caller will use standard encoding
  }

  auto* ef = impl_->get_embedded_font(font_name);
  if (!ef) {
    return "";
  }

  const auto& metrics = ef->font_data.metrics;

  // Convert UTF-8 text to glyph IDs (big-endian 16-bit values)
  std::string hex_str = "<";
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
  const uint8_t* end = p + text.size();

  while (p < end) {
    uint32_t cp;
    // Decode UTF-8
    if ((*p & 0x80) == 0) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = (*p++ & 0x1F) << 6;
      if (p < end) cp |= (*p++ & 0x3F);
    } else if ((*p & 0xF0) == 0xE0) {
      cp = (*p++ & 0x0F) << 12;
      if (p < end) cp |= (*p++ & 0x3F) << 6;
      if (p < end) cp |= (*p++ & 0x3F);
    } else if ((*p & 0xF8) == 0xF0) {
      cp = (*p++ & 0x07) << 18;
      if (p < end) cp |= (*p++ & 0x3F) << 12;
      if (p < end) cp |= (*p++ & 0x3F) << 6;
      if (p < end) cp |= (*p++ & 0x3F);
    } else {
      p++;
      continue;
    }

    // Track used character for subsetting
    ef->used_chars.insert(cp);

    // Look up glyph ID
    uint16_t glyph_id = 0;
    auto it = metrics.char_to_glyph.find(cp);
    if (it != metrics.char_to_glyph.end()) {
      glyph_id = it->second;
    }

    // Output as 4-digit hex (big-endian)
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", glyph_id);
    hex_str += buf;
  }

  hex_str += ">";
  return hex_str;
}

void PdfWriter::set_title(const std::string& title) { impl_->title = title; }

void PdfWriter::set_author(const std::string& author) {
  impl_->author = author;
}

void PdfWriter::set_subject(const std::string& subject) {
  impl_->subject = subject;
}

void PdfWriter::set_keywords(const std::string& keywords) {
  impl_->keywords = keywords;
}

void PdfWriter::set_creator(const std::string& creator) {
  impl_->creator = creator;
}

void PdfWriter::set_producer(const std::string& producer) {
  impl_->producer = producer;
}

void PdfWriter::set_custom_info(
    const std::string& key,
    const std::string& value) {
  impl_->custom_info[key] = value;
}

void PdfWriter::clear_custom_info(const std::string& key) {
  impl_->custom_info.erase(key);
}

void PdfWriter::set_creation_date(const std::string& date) {
  impl_->creation_date = date;
}

void PdfWriter::set_modification_date(const std::string& date) {
  impl_->modification_date = date;
}

void PdfWriter::set_page_label(uint32_t page_index, const PageLabel& label) {
  impl_->page_labels.labels[page_index] = label;
}

void PdfWriter::clear_page_labels() {
  impl_->page_labels.labels.clear();
}

void PdfWriter::add_named_destination(const NamedDestination& destination) {
  impl_->named_destinations[destination.name] = destination;
}

void PdfWriter::clear_named_destinations() {
  impl_->named_destinations.clear();
}

void PdfWriter::set_open_action_named_destination(
    const std::string& destination_name) {
  impl_->open_action_destination_name = destination_name;
}

void PdfWriter::clear_open_action() {
  impl_->open_action_destination_name.clear();
}

void PdfWriter::set_page_layout(PageLayout layout) {
  impl_->page_layout = layout;
}

void PdfWriter::set_page_mode(PageMode mode) {
  impl_->page_mode = mode;
}

void PdfWriter::set_viewer_preferences(
    const ViewerPreferences& preferences) {
  impl_->viewer_preferences = preferences;
  impl_->has_viewer_preferences = true;
}

void PdfWriter::clear_viewer_preferences() {
  impl_->viewer_preferences = ViewerPreferences{};
  impl_->has_viewer_preferences = false;
}

void PdfWriter::set_language(const std::string& language) {
  impl_->language = language;
}

void PdfWriter::clear_language() {
  impl_->language.clear();
}

void PdfWriter::set_xmp_metadata(const std::string& xml) {
  impl_->xmp_metadata_xml = xml;
}

void PdfWriter::clear_xmp_metadata() {
  impl_->xmp_metadata_xml.clear();
}

void PdfWriter::add_output_intent(const OutputIntentConfig& config) {
  impl_->output_intents.push_back(config);
}

void PdfWriter::clear_output_intents() {
  impl_->output_intents.clear();
}

void PdfWriter::set_mark_info(const MarkInfoConfig& config) {
  impl_->mark_info = config;
  impl_->has_mark_info = true;
}

void PdfWriter::clear_mark_info() {
  impl_->mark_info = MarkInfoConfig{};
  impl_->has_mark_info = false;
}

void PdfWriter::set_trapped(TrappedState trapped) {
  impl_->trapped = trapped;
}

void PdfWriter::clear_trapped() {
  impl_->trapped = TrappedState::Unset;
}

// ============================================================
// Incremental Update (Revision) Implementation
// ============================================================

bool PdfWriter::load_existing(const std::string& path, std::string* error) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    if (error) *error = "Failed to open file: " + path;
    return false;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> data(size);
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
    if (error) *error = "Failed to read file: " + path;
    return false;
  }

  return load_existing(data, error);
}

bool PdfWriter::load_existing(const std::vector<uint8_t>& data, std::string* error) {
  if (data.size() < 20) {
    if (error) *error = "File too small to be a valid PDF";
    return false;
  }

  // Check PDF header
  if (memcmp(data.data(), "%PDF-", 5) != 0) {
    if (error) *error = "Not a valid PDF (missing header)";
    return false;
  }
  if (data.size() >= 8) {
    if (data[5] == '1' && data[6] == '.' && data[7] == '4') {
      impl_->version = PdfVersion::v1_4;
    } else if (data[5] == '1' && data[6] == '.' && data[7] == '5') {
      impl_->version = PdfVersion::v1_5;
    } else if (data[5] == '1' && data[6] == '.' && data[7] == '6') {
      impl_->version = PdfVersion::v1_6;
    } else if (data[5] == '1' && data[6] == '.' && data[7] == '7') {
      impl_->version = PdfVersion::v1_7;
    } else if (data[5] == '2' && data[6] == '.' && data[7] == '0') {
      impl_->version = PdfVersion::v2_0;
    }
  }

  // Find startxref
  size_t xref_offset;
  if (!find_startxref(data, xref_offset)) {
    if (error) *error = "Could not find startxref";
    return false;
  }

  // Find and parse trailer. Classic xref tables are followed by a "trailer"
  // keyword; PDF 1.5+ cross-reference STREAMS have no trailer keyword — their
  // trailer dictionary (Root/Info/Size/ID/Prev) is the xref-stream object's own
  // dictionary at xref_offset. parse_trailer() locates the next "<<", so for an
  // xref stream we point it at the object start.
  size_t trailer_pos = find_keyword(data, xref_offset, "trailer");
  bool is_xref_stream = false;
  if (trailer_pos == std::string::npos) {
    trailer_pos = xref_offset;  // parse the xref-stream object's dictionary
    is_xref_stream = true;
  }

  int root_obj = 0, root_gen = 0;
  int info_obj = 0, info_gen = 0;
  int size_value = 0;
  std::vector<uint8_t> id1, id2;

  if (!parse_trailer(data, trailer_pos, root_obj, root_gen, info_obj, info_gen,
                     size_value, id1, id2) ||
      root_obj == 0) {
    if (error)
      *error = is_xref_stream ? "Could not parse xref-stream trailer dictionary"
                              : "Failed to parse trailer";
    return false;
  }

  // Detect encryption: the trailer (classic or xref-stream) dictionary holds
  // an /Encrypt reference. Incremental updates of encrypted PDFs require
  // re-encrypting new objects and carrying /Encrypt forward, which is not yet
  // implemented — write_incremental_* will reject such documents to avoid
  // producing a file whose existing content can no longer be decrypted.
  {
    size_t d0 = find_keyword(data, trailer_pos, "<<");
    if (d0 != std::string::npos) {
      // Find the matching ">>" honoring nested dictionaries (/DecodeParms etc.).
      int depth = 0;
      size_t p = d0, end = std::string::npos;
      while (p + 1 < data.size()) {
        if (data[p] == '<' && data[p + 1] == '<') { depth++; p += 2; }
        else if (data[p] == '>' && data[p + 1] == '>') {
          depth--; p += 2;
          if (depth == 0) { end = p; break; }
        } else { ++p; }
      }
      if (end != std::string::npos) {
        std::string dict(reinterpret_cast<const char*>(data.data()) + d0,
                         end - d0);
        size_t ep = dict.find("/Encrypt");
        if (ep != std::string::npos) {
          impl_->existing_encrypted = true;
          // Capture the "N G R" indirect reference to carry into our trailer.
          int en = 0, eg = 0;
          char r = 0;
          if (std::sscanf(dict.c_str() + ep + 8, " %d %d %c", &en, &eg, &r) ==
                  3 && r == 'R') {
            impl_->existing_encrypt_ref =
                std::to_string(en) + " " + std::to_string(eg) + " R";
          }
        }
      }
    }
  }

  // For encrypted PDFs, derive the document key (empty user password) by
  // parsing the document, so new objects' strings/streams are encrypted with
  // the same key (the signature /Contents stays exempt). The /Encrypt dict is
  // carried forward in the trailer.
  if (impl_->existing_encrypted && !impl_->existing_encrypt_ref.empty()) {
    Pdf enc_pdf;
    if (parse_from_memory(data.data(), data.size(), &enc_pdf) &&
        !enc_pdf.security.encryption_key.empty()) {
      impl_->encryption_enabled = true;
      impl_->encryption_key = enc_pdf.security.encryption_key;
      impl_->encryption_config.algorithm = enc_pdf.security.algorithm;
      impl_->encrypt_obj_id = std::atoi(impl_->existing_encrypt_ref.c_str());
    } else {
      // Could not derive the key (e.g. owner-only password): refuse rather
      // than emit a file whose existing content can't be decrypted.
      if (error) *error = "encrypted PDF: could not derive the document key";
      return false;
    }
  }

  // Store existing PDF data and metadata
  impl_->existing_pdf = data;
  impl_->has_existing = true;
  impl_->prev_xref_offset = xref_offset;
  impl_->existing_root_obj = root_obj;
  impl_->existing_root_gen = root_gen;
  impl_->existing_info_obj = info_obj;
  impl_->existing_info_gen = info_gen;
  impl_->next_obj_id = size_value;  // Start numbering from existing Size
  impl_->revision_count = count_revisions(data, xref_offset);

  // Copy document IDs (keep ID1, update ID2 for modifications)
  if (!id1.empty()) {
    impl_->doc_id1 = id1;
    impl_->doc_id2 = generate_random_id();  // New ID2 for this revision
  }

  return true;
}

bool PdfWriter::has_existing_pdf() const {
  return impl_->has_existing;
}

int PdfWriter::get_revision_count() const {
  return impl_->revision_count;
}

WriteResult PdfWriter::write_incremental(std::vector<uint8_t>& output) {
  return write_incremental_for_signing(output, 0);
}

WriteResult PdfWriter::write_incremental_to_file(const std::string& path) {
  std::vector<uint8_t> data;
  WriteResult result = write_incremental(data);
  if (!result.success) {
    return result;
  }

  std::ofstream file(path, std::ios::binary);
  if (!file) {
    result.success = false;
    result.error = "Failed to open file for writing: " + path;
    return result;
  }

  file.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (!file) {
    result.success = false;
    result.error = "Failed to write to file: " + path;
    return result;
  }

  return result;
}

WriteResult PdfWriter::write_incremental_for_signing(std::vector<uint8_t>& output,
                                                     size_t content_length) {
  WriteResult result;

  if (!impl_->has_existing) {
    result.success = false;
    result.error = "No existing PDF loaded - use load_existing() first";
    return result;
  }

  if (impl_->existing_encrypted && impl_->existing_encrypt_ref.empty()) {
    result.success = false;
    result.error = "encrypted PDF: could not locate the /Encrypt reference";
    return result;
  }

  // Start with existing PDF data
  output = impl_->existing_pdf;

  // Clear tracking for new objects
  impl_->output.str("");
  impl_->output.clear();
  impl_->obj_offsets.clear();
  impl_->sig_placeholders.clear();
  impl_->sig_content_length = content_length;

  // Track new object offsets relative to start of incremental update
  size_t base_offset = output.size();

  // Generate new document ID2 if needed
  if (impl_->doc_id2.empty()) {
    impl_->doc_id2 = generate_random_id();
  }

  // Track objects we're adding in this update
  std::vector<std::pair<int, size_t>> new_obj_offsets;  // obj_id -> offset in output

  int certification_sig_obj_id = 0;
  MdpPermissions certification_permissions = impl_->permissions;
  for (const auto& sig_field : impl_->signature_fields) {
    if (sig_field.config.is_certification) {
      certification_sig_obj_id = sig_field.sig_obj_id;
      certification_permissions = sig_field.config.mdp_permissions;
      break;
    }
  }
  if (certification_sig_obj_id == 0 &&
      impl_->has_permissions &&
      !impl_->signature_fields.empty()) {
    certification_sig_obj_id = impl_->signature_fields.front().sig_obj_id;
  }

  // Write new image objects
  for (auto& img_res : impl_->images) {
    size_t obj_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({img_res.obj_id, obj_offset});

    impl_->output << img_res.obj_id << " 0 obj\n";

    std::vector<uint8_t> stream_data;
    std::string filter;
    std::string color_space;
    int bits_per_component = 8;
    int img_width, img_height;
    std::string decode_parms;

    if (img_res.is_ccitt_only) {
      stream_data = img_res.ccitt_data;
      filter = "/CCITTFaxDecode";
      color_space = "/DeviceGray";
      bits_per_component = 1;
      img_width = img_res.ccitt_width;
      img_height = img_res.ccitt_height;
      decode_parms = "/DecodeParms << /K -1 /Columns " +
                     std::to_string(img_width) + " >>";
    } else {
      const ImageData& img = img_res.data;
      img_width = img.width;
      img_height = img.height;
      ImageCompression comp = img_res.compression;

      if (comp == ImageCompression::CCITTFax && !img_res.ccitt_data.empty()) {
        stream_data = img_res.ccitt_data;
        filter = "/CCITTFaxDecode";
        color_space = "/DeviceGray";
        bits_per_component = 1;
        decode_parms = "/DecodeParms << /K -1 /Columns " +
                       std::to_string(img_width) + " >>";
      } else if ((comp == ImageCompression::Auto || comp == ImageCompression::DCT) &&
                 img.is_jpeg && !img.encoded_data.empty()) {
        stream_data = img.encoded_data;
        filter = "/DCTDecode";
        color_space = (img.channels == 1) ? "/DeviceGray" : "/DeviceRGB";
      } else {
        std::vector<uint8_t> rgb_data;
        if (img.channels == 4) {
          rgb_data.reserve(img.width * img.height * 3);
          for (int i = 0; i < img.width * img.height; ++i) {
            rgb_data.push_back(img.raw_data[i * 4]);
            rgb_data.push_back(img.raw_data[i * 4 + 1]);
            rgb_data.push_back(img.raw_data[i * 4 + 2]);
          }
          color_space = "/DeviceRGB";
        } else if (img.channels == 3) {
          rgb_data = img.raw_data;
          color_space = "/DeviceRGB";
        } else if (img.channels == 1) {
          rgb_data = img.raw_data;
          color_space = "/DeviceGray";
        } else {
          result.success = false;
          result.error = "Unsupported image channel count";
          return result;
        }

        stream_data = compress_data(rgb_data.data(), rgb_data.size());
        if (stream_data.empty()) {
          result.success = false;
          result.error = "Failed to compress image data";
          return result;
        }
        filter = "/FlateDecode";
      }
    }

    impl_->output << "<<\n";
    impl_->output << "/Type /XObject\n";
    impl_->output << "/Subtype /Image\n";
    impl_->output << "/Width " << img_width << "\n";
    impl_->output << "/Height " << img_height << "\n";
    impl_->output << "/ColorSpace " << color_space << "\n";
    impl_->output << "/BitsPerComponent " << bits_per_component << "\n";
    impl_->output << "/Filter " << filter << "\n";
    if (!decode_parms.empty()) {
      impl_->output << decode_parms << "\n";
    }
    std::vector<uint8_t> encrypted_stream_data =
        impl_->prepare_stream_data(img_res.obj_id, stream_data);
    impl_->output << "/Length " << encrypted_stream_data.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(
        reinterpret_cast<const char*>(encrypted_stream_data.data()),
        encrypted_stream_data.size());
    impl_->output << "\nendstream\n";
    impl_->output << "endobj\n";
  }

  // Write new font objects
  for (const auto& font_res : impl_->fonts) {
    size_t obj_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({font_res.obj_id, obj_offset});

    impl_->output << font_res.obj_id << " 0 obj\n";
    impl_->output << "<<\n";
    impl_->output << "/Type /Font\n";
    impl_->output << "/Subtype /Type1\n";
    impl_->output << "/BaseFont /" << get_standard_font_name(font_res.font) << "\n";
    impl_->output << "/Encoding /WinAnsiEncoding\n";
    impl_->output << ">>\n";
    impl_->output << "endobj\n";
  }

  // Write signature field objects if any
  for (auto& sig_field : impl_->signature_fields) {
    const auto& config = sig_field.config;

    // Write signature value dictionary (placeholder)
    size_t sig_obj_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({sig_field.sig_obj_id, sig_obj_offset});

    impl_->output << sig_field.sig_obj_id << " 0 obj\n";
    impl_->output << "<<\n";
    impl_->output << "/Type /Sig\n";
    impl_->output << "/Filter /" << get_filter_name(config.filter) << "\n";
    impl_->output << "/SubFilter /" << get_subfilter_name(config.subfilter) << "\n";

    if (!config.reason.empty()) {
      impl_->output << "/Reason "
                    << impl_->format_pdf_string_for_object(sig_field.sig_obj_id, config.reason)
                    << "\n";
    }
    if (!config.location.empty()) {
      impl_->output << "/Location "
                    << impl_->format_pdf_string_for_object(sig_field.sig_obj_id, config.location)
                    << "\n";
    }
    if (!config.contact_info.empty()) {
      impl_->output << "/ContactInfo "
                    << impl_->format_pdf_string_for_object(
                           sig_field.sig_obj_id, config.contact_info)
                    << "\n";
    }
    impl_->output << "/M "
                  << impl_->format_pdf_string_for_object(sig_field.sig_obj_id,
                                                         get_pdf_timestamp())
                  << "\n";

    // ByteRange placeholder
    size_t byte_range_offset = base_offset + impl_->output.tellp();
    impl_->output << "/ByteRange [0 0000000000 0000000000 0000000000]\n";

    // Contents placeholder
    impl_->output << "/Contents <";
    size_t contents_offset = base_offset + impl_->output.tellp();
    size_t sig_size = (impl_->sig_content_length > 0) ? impl_->sig_content_length : 8192;
    for (size_t j = 0; j < sig_size; ++j) {
      impl_->output << "00";
    }
    impl_->output << ">\n";

    if (sig_field.sig_obj_id == certification_sig_obj_id) {
      impl_->output << "/Reference [<< /Type /SigRef /TransformMethod /DocMDP "
                    << "/TransformParams << /Type /TransformParams /P "
                    << static_cast<int>(certification_permissions)
                    << " /V /1.2 >> >>]\n";
    }

    impl_->output << ">>\n";
    impl_->output << "endobj\n";

    // Record placeholder info
    SignaturePlaceholder placeholder;
    placeholder.field_name = config.name;
    placeholder.contents_offset = contents_offset;
    placeholder.contents_length = sig_size;
    placeholder.byte_range_offset = byte_range_offset;
    impl_->sig_placeholders.push_back(placeholder);

    // Write signature field / widget annotation
    size_t field_obj_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({sig_field.field_obj_id, field_obj_offset});

    impl_->output << sig_field.field_obj_id << " 0 obj\n";
    impl_->output << "<<\n";
    impl_->output << "/Type /Annot\n";
    impl_->output << "/Subtype /Widget\n";
    impl_->output << "/FT /Sig\n";
    impl_->output << "/T "
                  << impl_->format_pdf_string_for_object(sig_field.field_obj_id, config.name)
                  << "\n";
    impl_->output << "/V " << sig_field.sig_obj_id << " 0 R\n";
    impl_->output << "/F " << (config.visible ? 4 : 6) << "\n";
    // Note: /P would reference existing page object - needs page tracking
    impl_->output << "/Rect [" << format_number(config.x) << " "
                  << format_number(config.y) << " "
                  << format_number(config.x + config.width) << " "
                  << format_number(config.y + config.height) << "]\n";
    impl_->output << ">>\n";
    impl_->output << "endobj\n";
  }

  // Write AcroForm if we have signature fields
  if (impl_->has_acroform && !impl_->signature_fields.empty()) {
    Pdf existing_pdf;
    ResolvedObject root_obj;
    std::map<std::string, Value> existing_acroform_dict;
    std::vector<Value> existing_acroform_fields;
    bool parsed_existing_pdf = parse_from_memory(
        impl_->existing_pdf.data(), impl_->existing_pdf.size(), &existing_pdf);

    if (!parsed_existing_pdf) {
      result.success = false;
      result.error = "Failed to parse existing PDF for incremental signing";
      return result;
    }

    root_obj = resolve_reference(
        existing_pdf, impl_->existing_root_obj, impl_->existing_root_gen);
    if (!root_obj.success || root_obj.value.type != Value::DICTIONARY) {
      result.success = false;
      result.error = "Failed to resolve existing catalog for incremental signing";
      return result;
    }

    auto acroform_it = root_obj.value.dict.find("AcroForm");
    if (acroform_it != root_obj.value.dict.end()) {
      Value acroform_value = acroform_it->second;
      if (acroform_value.type == Value::REFERENCE) {
        ResolvedObject resolved_acroform = resolve_reference(
            existing_pdf,
            acroform_value.ref_object_number,
            acroform_value.ref_generation_number);
        if (resolved_acroform.success &&
            resolved_acroform.value.type == Value::DICTIONARY) {
          acroform_value = resolved_acroform.value;
        }
      }
      if (acroform_value.type == Value::DICTIONARY) {
        existing_acroform_dict = acroform_value.dict;
        auto fields_it = existing_acroform_dict.find("Fields");
        if (fields_it != existing_acroform_dict.end()) {
          Value fields_value = fields_it->second;
          if (fields_value.type == Value::REFERENCE) {
            ResolvedObject resolved_fields = resolve_reference(
                existing_pdf,
                fields_value.ref_object_number,
                fields_value.ref_generation_number);
            if (resolved_fields.success &&
                resolved_fields.value.type == Value::ARRAY) {
              existing_acroform_fields = resolved_fields.value.array;
            }
          } else if (fields_value.type == Value::ARRAY) {
            existing_acroform_fields = fields_value.array;
          }
        }
      }
    }

    size_t acroform_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({impl_->acroform_obj_id, acroform_offset});

    impl_->output << impl_->acroform_obj_id << " 0 obj\n";
    impl_->output << "<<\n";
    for (const auto& entry : existing_acroform_dict) {
      if (entry.first == "Fields" || entry.first == "SigFlags") continue;
      impl_->output << "/" << entry.first << " "
                    << serialize_pdf_value(entry.second) << "\n";
    }
    impl_->output << "/Fields [";
    bool first_field = true;
    for (const auto& existing_field : existing_acroform_fields) {
      if (existing_field.type != Value::REFERENCE) continue;
      if (!first_field) impl_->output << " ";
      impl_->output << existing_field.ref_object_number << " "
                    << existing_field.ref_generation_number << " R";
      first_field = false;
    }
    for (const auto& sig_field : impl_->signature_fields) {
      if (!first_field) impl_->output << " ";
      impl_->output << sig_field.field_obj_id << " 0 R";
      first_field = false;
    }
    impl_->output << "]\n";
    impl_->output << "/SigFlags 3\n";
    impl_->output << ">>\n";
    impl_->output << "endobj\n";

    size_t root_offset = base_offset + impl_->output.tellp();
    new_obj_offsets.push_back({impl_->existing_root_obj, root_offset});

    impl_->output << impl_->existing_root_obj << " "
                  << impl_->existing_root_gen << " obj\n";
    impl_->output << "<<\n";
    for (const auto& entry : root_obj.value.dict) {
      if (entry.first == "AcroForm") continue;
      if (entry.first == "Perms" && certification_sig_obj_id != 0) continue;
      impl_->output << "/" << entry.first << " "
                    << serialize_pdf_value(entry.second) << "\n";
    }
    impl_->output << "/AcroForm " << impl_->acroform_obj_id << " 0 R\n";
    if (certification_sig_obj_id != 0) {
      impl_->output << "/Perms << /DocMDP " << certification_sig_obj_id
                    << " 0 R >>\n";
    }
    impl_->output << ">>\n";
    impl_->output << "endobj\n";
  }

  // Write updated form field objects (for form fill & save)
  if (!impl_->field_updates.empty()) {
    // Parse existing PDF to get original field dictionaries
    Pdf existing_pdf;
    bool parsed = parse_from_memory(impl_->existing_pdf.data(),
                                    impl_->existing_pdf.size(), &existing_pdf);

    for (const auto& update : impl_->field_updates) {
      if (update.object_number == 0) continue;

      size_t field_offset = base_offset + impl_->output.tellp();
      new_obj_offsets.push_back({static_cast<int>(update.object_number), field_offset});

      impl_->output << update.object_number << " " << update.generation << " obj\n";

      // Try to preserve original field dictionary, just update /V
      if (parsed) {
        ResolvedObject resolved = resolve_reference(
            existing_pdf, update.object_number, update.generation);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          impl_->output << "<<\n";
          for (const auto& kv : resolved.value.dict) {
            if (kv.first == "V" || kv.first == "AS") continue;  // Skip old value
            impl_->output << "/" << kv.first << " "
                          << serialize_pdf_value(kv.second) << "\n";
          }
          // Write updated value
          impl_->output << "/V " << update.value << "\n";
          // For checkboxes/radio buttons, also update /AS
          if (update.value[0] == '/') {
            impl_->output << "/AS " << update.value << "\n";
          }
          impl_->output << ">>\n";
        } else {
          // Fallback: minimal field dict with just the value
          impl_->output << "<<\n";
          impl_->output << "/V " << update.value << "\n";
          impl_->output << ">>\n";
        }
      } else {
        impl_->output << "<<\n";
        impl_->output << "/V " << update.value << "\n";
        impl_->output << ">>\n";
      }

      impl_->output << "endobj\n";
    }
  }

  // Write annotation updates and signature widgets for existing pages
  if (!impl_->annotation_updates.empty() || !impl_->signature_fields.empty()) {
    // Parse existing PDF to get page /Annots arrays
    Pdf existing_pdf;
    bool parsed = parse_from_memory(impl_->existing_pdf.data(),
                                    impl_->existing_pdf.size(), &existing_pdf);
    if (parsed) {
      parsed = existing_pdf.load_document_structure();
    }

    if (parsed) {
      const auto existing_page_refs = collect_page_object_refs(existing_pdf);

      // Group updates by page
      std::map<int, std::vector<const Impl::AnnotationUpdate*>> page_updates;
      for (const auto& update : impl_->annotation_updates) {
        page_updates[update.page_index].push_back(&update);
      }
      std::map<int, std::vector<int>> signature_widgets_by_page;
      for (const auto& sig_field : impl_->signature_fields) {
        signature_widgets_by_page[sig_field.config.page].push_back(
            sig_field.field_obj_id);
      }
      std::set<int> pages_to_update;
      for (const auto& entry : page_updates) {
        pages_to_update.insert(entry.first);
      }
      for (const auto& entry : signature_widgets_by_page) {
        pages_to_update.insert(entry.first);
      }

      for (int page_idx : pages_to_update) {
        static const std::vector<const Impl::AnnotationUpdate*> kEmptyUpdates;
        static const std::vector<int> kEmptySignatureWidgets;
        auto updates_it = page_updates.find(page_idx);
        auto sig_widgets_it = signature_widgets_by_page.find(page_idx);
        const auto& updates =
            (updates_it != page_updates.end()) ? updates_it->second : kEmptyUpdates;
        const auto& signature_widgets =
            (sig_widgets_it != signature_widgets_by_page.end())
                ? sig_widgets_it->second
                : kEmptySignatureWidgets;

        if (page_idx < 0 || page_idx >= static_cast<int>(existing_pdf.catalog.pages.size())) {
          continue;
        }

        uint32_t page_obj_num = 0;
        uint16_t page_gen = 0;

        if (page_idx >= 0 && page_idx < static_cast<int>(existing_page_refs.size())) {
          page_obj_num = existing_page_refs[page_idx].first;
          page_gen = existing_page_refs[page_idx].second;
        }

        if (page_obj_num == 0) continue;

        // Get existing /Annots array references
        std::vector<Value> existing_annots;
        {
          ResolvedObject page_obj = resolve_reference(existing_pdf, page_obj_num, page_gen);
          if (page_obj.success && page_obj.value.type == Value::DICTIONARY) {
            auto annots_it = page_obj.value.dict.find("Annots");
            if (annots_it != page_obj.value.dict.end()) {
              const Value* annots_val = &annots_it->second;
              if (annots_val->type == Value::REFERENCE) {
                ResolvedObject resolved = resolve_reference(existing_pdf,
                    annots_val->ref_object_number, annots_val->ref_generation_number);
                if (resolved.success && resolved.value.type == Value::ARRAY) {
                  existing_annots = resolved.value.array;
                }
              } else if (annots_val->type == Value::ARRAY) {
                existing_annots = annots_val->array;
              }
            }
          }
        }

        // Collect indices to delete (process deletes)
        std::set<int> delete_indices;
        for (const auto* upd : updates) {
          if (upd->update_type == Impl::AnnotationUpdateType::Delete) {
            delete_indices.insert(upd->annot_index);
          }
        }

        // Remove deleted annotations from existing list
        std::vector<Value> kept_annots;
        for (int i = 0; i < static_cast<int>(existing_annots.size()); ++i) {
          if (delete_indices.find(i) == delete_indices.end()) {
            kept_annots.push_back(existing_annots[i]);
          }
        }

        // Write new annotation objects and collect their IDs
        std::vector<int> new_annot_obj_ids;
        for (const auto* upd : updates) {
          if (upd->update_type == Impl::AnnotationUpdateType::Delete) continue;

          int annot_obj_id = impl_->next_obj_id++;
          new_annot_obj_ids.push_back(annot_obj_id);

          size_t annot_offset = base_offset + impl_->output.tellp();
          new_obj_offsets.push_back({annot_obj_id, annot_offset});

          impl_->output << annot_obj_id << " 0 obj\n";
          impl_->output << "<<\n";
          impl_->output << "/Type /Annot\n";

          auto write_text_markup_annotation =
              [&](const TextMarkupConfig& markup) {
                const char* subtype = "/Highlight";
                switch (markup.type) {
                  case MarkupType::Highlight: subtype = "/Highlight"; break;
                  case MarkupType::Underline: subtype = "/Underline"; break;
                  case MarkupType::Squiggly: subtype = "/Squiggly"; break;
                  case MarkupType::StrikeOut: subtype = "/StrikeOut"; break;
                }
                impl_->output << "/Subtype " << subtype << "\n";

                double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
                for (const auto& q : markup.quads) {
                  min_x = std::min({min_x, q.x1, q.x2, q.x3, q.x4});
                  min_y = std::min({min_y, q.y1, q.y2, q.y3, q.y4});
                  max_x = std::max({max_x, q.x1, q.x2, q.x3, q.x4});
                  max_y = std::max({max_y, q.y1, q.y2, q.y3, q.y4});
                }
                impl_->output << "/Rect [" << format_number(min_x) << " "
                              << format_number(min_y) << " "
                              << format_number(max_x) << " "
                              << format_number(max_y) << "]\n";

                impl_->output << "/QuadPoints [";
                bool first_quad = true;
                for (const auto& q : markup.quads) {
                  if (!first_quad) impl_->output << " ";
                  impl_->output << format_number(q.x1) << " " << format_number(q.y1) << " "
                                << format_number(q.x2) << " " << format_number(q.y2) << " "
                                << format_number(q.x3) << " " << format_number(q.y3) << " "
                                << format_number(q.x4) << " " << format_number(q.y4);
                  first_quad = false;
                }
                impl_->output << "]\n";
                impl_->output << "/C [" << format_number(markup.r) << " "
                              << format_number(markup.g) << " "
                              << format_number(markup.b) << "]\n";
                if (markup.alpha < 1.0) {
                  impl_->output << "/CA " << format_number(markup.alpha) << "\n";
                }
                if (!markup.contents.empty()) {
                  impl_->output << "/Contents "
                                << impl_->format_pdf_string_for_object(
                                       annot_obj_id, markup.contents)
                                << "\n";
                }
                if (!markup.author.empty()) {
                  impl_->output << "/T "
                                << impl_->format_pdf_string_for_object(
                                       annot_obj_id, markup.author)
                                << "\n";
                }
                impl_->output << "/F " << (markup.print ? 4 : 0) << "\n";
                impl_->output << "/P " << page_obj_num << " " << page_gen << " R\n";
              };

          if (upd->update_type == Impl::AnnotationUpdateType::AddText) {
            impl_->output << "/Subtype /Text\n";
            impl_->output << "/Rect [" << format_number(upd->x) << " "
                          << format_number(upd->y) << " "
                          << format_number(upd->x + upd->w) << " "
                          << format_number(upd->y + upd->h) << "]\n";
            impl_->output << "/Contents "
                          << impl_->format_pdf_string_for_object(
                                 annot_obj_id, upd->contents)
                          << "\n";
            impl_->output << "/F 4\n";  // Print flag
            impl_->output << "/P " << page_obj_num << " " << page_gen << " R\n";
          } else if (upd->update_type == Impl::AnnotationUpdateType::AddHighlight) {
            TextMarkupConfig markup;
            markup.type = MarkupType::Highlight;
            markup.quads = upd->highlight_config.quads;
            markup.r = upd->highlight_config.r;
            markup.g = upd->highlight_config.g;
            markup.b = upd->highlight_config.b;
            markup.alpha = upd->highlight_config.alpha;
            markup.author = upd->highlight_config.author;
            markup.contents = upd->highlight_config.contents;
            markup.print = upd->highlight_config.print;
            write_text_markup_annotation(markup);
          } else if (upd->update_type == Impl::AnnotationUpdateType::AddTextMarkup) {
            write_text_markup_annotation(upd->text_markup_config);
          } else if (upd->update_type == Impl::AnnotationUpdateType::AddLink) {
            const auto& lc = upd->link_config;
            impl_->output << "/Subtype /Link\n";
            impl_->output << "/Rect [" << format_number(lc.x) << " "
                          << format_number(lc.y) << " "
                          << format_number(lc.x + lc.width) << " "
                          << format_number(lc.y + lc.height) << "]\n";

            if (!lc.show_border) {
              impl_->output << "/Border [0 0 0]\n";
            }

            if (lc.action == LinkAction::URI && !lc.uri.empty()) {
              impl_->output << "/A << /Type /Action /S /URI /URI "
                            << impl_->format_pdf_string_for_object(annot_obj_id, lc.uri)
                            << " >>\n";
            } else if (lc.action == LinkAction::GoTo) {
              if (lc.dest_page >= 0 &&
                  lc.dest_page < static_cast<int>(existing_page_refs.size())) {
                impl_->output << "/Dest ["
                              << existing_page_refs[lc.dest_page].first << " "
                              << existing_page_refs[lc.dest_page].second
                              << " R /XYZ null "
                              << format_number(lc.dest_y) << " null]\n";
              }
            }

            impl_->output << "/F 4\n";
            impl_->output << "/P " << page_obj_num << " " << page_gen << " R\n";
          }

          impl_->output << ">>\n";
          impl_->output << "endobj\n";
        }
        for (int widget_obj_id : signature_widgets) {
          new_annot_obj_ids.push_back(widget_obj_id);
        }

        // Now write the updated page object with modified /Annots array
        size_t page_offset = base_offset + impl_->output.tellp();
        new_obj_offsets.push_back({static_cast<int>(page_obj_num), page_offset});

        // Get original page dict to preserve all other keys
        ResolvedObject orig_page = resolve_reference(existing_pdf, page_obj_num, page_gen);
        impl_->output << page_obj_num << " " << page_gen << " obj\n";
        impl_->output << "<<\n";

        if (orig_page.success && orig_page.value.type == Value::DICTIONARY) {
          for (const auto& entry : orig_page.value.dict) {
            if (entry.first == "Annots") continue;  // We'll write our own
            impl_->output << "/" << entry.first << " "
                          << serialize_pdf_value(entry.second) << "\n";
          }
        }

        // Write updated /Annots array
        impl_->output << "/Annots [";
        for (size_t ai = 0; ai < kept_annots.size(); ++ai) {
          if (ai > 0) impl_->output << " ";
          const Value& av = kept_annots[ai];
          if (av.type == Value::REFERENCE) {
            impl_->output << av.ref_object_number << " "
                          << av.ref_generation_number << " R";
          }
        }
        for (int new_id : new_annot_obj_ids) {
          if (!kept_annots.empty() || &new_id != &new_annot_obj_ids.front()) {
            impl_->output << " ";
          }
          impl_->output << new_id << " 0 R";
        }
        impl_->output << "]\n";
        impl_->output << ">>\n";
        impl_->output << "endobj\n";
      }
    }
  }

  // Write incremental xref table
  size_t xref_offset = base_offset + impl_->output.tellp();
  impl_->output << "xref\n";

  // Write xref entries for new objects (sorted by object number)
  std::sort(new_obj_offsets.begin(), new_obj_offsets.end());

  if (!new_obj_offsets.empty()) {
    // Group consecutive object numbers
    int start_obj = new_obj_offsets[0].first;
    int count = 1;
    size_t i = 1;

    while (i <= new_obj_offsets.size()) {
      bool consecutive = (i < new_obj_offsets.size() &&
                          new_obj_offsets[i].first == start_obj + count);

      if (consecutive) {
        ++count;
        ++i;
      } else {
        // Write this range
        impl_->output << start_obj << " " << count << "\n";
        for (int j = 0; j < count; ++j) {
          size_t idx = i - count + j;
          char buf[32];
          snprintf(buf, sizeof(buf), "%010zu 00000 n \n", new_obj_offsets[idx].second);
          impl_->output << buf;
        }

        if (i < new_obj_offsets.size()) {
          start_obj = new_obj_offsets[i].first;
          count = 1;
        }
        ++i;
      }
    }
  }

  // Write trailer
  impl_->output << "trailer\n";
  impl_->output << "<<\n";
  impl_->output << "/Size " << impl_->next_obj_id << "\n";
  impl_->output << "/Root " << impl_->existing_root_obj << " "
                << impl_->existing_root_gen << " R\n";
  if (impl_->existing_info_obj > 0) {
    impl_->output << "/Info " << impl_->existing_info_obj << " "
                  << impl_->existing_info_gen << " R\n";
  }
  // Carry the encryption dictionary forward so readers using this (newest)
  // trailer still decrypt the existing content.
  if (!impl_->existing_encrypt_ref.empty()) {
    impl_->output << "/Encrypt " << impl_->existing_encrypt_ref << "\n";
  }
  impl_->output << "/Prev " << impl_->prev_xref_offset << "\n";
  // Document ID
  impl_->output << "/ID [<" << bytes_to_hex(impl_->doc_id1) << "> <"
                << bytes_to_hex(impl_->doc_id2) << ">]\n";
  impl_->output << ">>\n";
  impl_->output << "startxref\n";
  impl_->output << xref_offset << "\n";
  impl_->output << "%%EOF\n";

  // Append incremental update to output
  std::string update_str = impl_->output.str();
  output.insert(output.end(), update_str.begin(), update_str.end());

  // Update signature ByteRange values
  for (auto& placeholder : impl_->sig_placeholders) {
    size_t part1_start = 0;
    size_t part1_len = placeholder.contents_offset - 1;
    size_t part2_start = placeholder.contents_offset + placeholder.contents_length * 2 + 1;
    size_t part2_len = output.size() - part2_start;

    placeholder.byte_range = {part1_start, part1_len, part2_start, part2_len};
  }

  result.success = true;
  result.bytes_written = output.size();
  return result;
}

void PdfWriter::set_version(PdfVersion version) {
  impl_->version = version;
}

PdfVersion PdfWriter::get_version() const {
  return impl_->version;
}

void PdfWriter::set_document_id(const std::vector<uint8_t>& id1,
                                const std::vector<uint8_t>& id2) {
  impl_->doc_id1 = id1;
  impl_->doc_id2 = id2;
}

void PdfWriter::generate_document_id() {
  impl_->doc_id1 = generate_random_id();
  impl_->doc_id2 = generate_random_id();
}

void PdfWriter::ensure_acroform() {
  if (!impl_->has_acroform) {
    impl_->acroform_obj_id = impl_->allocate_obj();
    impl_->has_acroform = true;
  }
}

std::string PdfWriter::add_signature_field(const SignatureFieldConfig& config) {
  // Validate page index (skip for incremental updates where pages already exist)
  if (!impl_->has_existing) {
    if (config.page < 0 || static_cast<size_t>(config.page) >= impl_->pages.size()) {
      return "";
    }
  }

  // Ensure AcroForm exists
  ensure_acroform();

  // Allocate object IDs
  Impl::SignatureFieldData sig_data;
  sig_data.config = config;
  sig_data.field_obj_id = impl_->allocate_obj();
  sig_data.sig_obj_id = impl_->allocate_obj();
  sig_data.widget_obj_id = sig_data.field_obj_id;  // Combined field/widget
  if (config.visible) {
    sig_data.appearance_obj_id = impl_->allocate_obj();
  }

  // Add annotation reference to page (only for new PDFs, not incremental updates)
  if (!impl_->has_existing && static_cast<size_t>(config.page) < impl_->pages.size()) {
    impl_->pages[config.page].annotation_refs.push_back(sig_data.field_obj_id);
  }

  impl_->signature_fields.push_back(sig_data);

  return config.name;
}

const std::vector<SignaturePlaceholder>& PdfWriter::get_signature_placeholders() const {
  return impl_->sig_placeholders;
}

void PdfWriter::set_permissions(MdpPermissions permissions) {
  impl_->has_permissions = true;
  impl_->permissions = permissions;
}

// ============================================================
// Digital Signature Application (free function)
// ============================================================

WriteResult apply_signature(std::vector<uint8_t>& pdf_data,
                            const SignaturePlaceholder& placeholder,
                            const SigningCallback& sign_fn) {
  WriteResult result;

  // Validate placeholder
  if (placeholder.byte_range.size() != 4) {
    result.error = "invalid placeholder: ByteRange must have 4 elements";
    return result;
  }
  if (placeholder.contents_length == 0) {
    result.error = "invalid placeholder: contents_length is 0";
    return result;
  }
  if (!sign_fn) {
    result.error = "signing callback is null";
    return result;
  }

  size_t part1_offset = placeholder.byte_range[0];
  size_t part1_length = placeholder.byte_range[1];
  size_t part2_offset = placeholder.byte_range[2];
  size_t part2_length = placeholder.byte_range[3];

  // Bounds check
  if (part1_offset + part1_length > pdf_data.size() ||
      part2_offset + part2_length > pdf_data.size()) {
    result.error = "ByteRange exceeds PDF data size";
    return result;
  }

  if (part1_length > 9999999999ULL || part2_offset > 9999999999ULL ||
      part2_length > 9999999999ULL) {
    result.error = "ByteRange value exceeds placeholder width";
    return result;
  }

  {
    char byte_range_text[64];
    int written = std::snprintf(
        byte_range_text,
        sizeof(byte_range_text),
        "/ByteRange [0 %010zu %010zu %010zu]",
        part1_length,
        part2_offset,
        part2_length);
    if (written <= 0 || static_cast<size_t>(written) != 47) {
      result.error = "failed to format ByteRange";
      return result;
    }
    if (placeholder.byte_range_offset + static_cast<size_t>(written) > pdf_data.size()) {
      result.error = "ByteRange placeholder offset exceeds PDF data size";
      return result;
    }
    std::memcpy(pdf_data.data() + placeholder.byte_range_offset,
                byte_range_text,
                static_cast<size_t>(written));
  }

  // Collect the data covered by ByteRange (everything except the signature hex)
  std::vector<uint8_t> data_to_sign;
  data_to_sign.reserve(part1_length + part2_length);
  data_to_sign.insert(data_to_sign.end(),
                       pdf_data.begin() + part1_offset,
                       pdf_data.begin() + part1_offset + part1_length);
  data_to_sign.insert(data_to_sign.end(),
                       pdf_data.begin() + part2_offset,
                       pdf_data.begin() + part2_offset + part2_length);

  // Call the user's signing callback
  std::vector<uint8_t> signature = sign_fn(data_to_sign);
  if (signature.empty()) {
    result.error = "signing callback returned empty signature";
    return result;
  }
  if (signature.size() > placeholder.contents_length) {
    result.error = "signature (" + std::to_string(signature.size()) +
                   " bytes) exceeds reserved space (" +
                   std::to_string(placeholder.contents_length) + " bytes)";
    return result;
  }

  // Encode signature as hex string and write into the /Contents placeholder.
  // The placeholder in the PDF is: <0000...0000> where the hex digits are at
  // contents_offset. Each byte becomes 2 hex chars.
  static const char hex_chars[] = "0123456789abcdef";
  size_t hex_offset = placeholder.contents_offset;

  for (size_t i = 0; i < signature.size(); ++i) {
    pdf_data[hex_offset + i * 2] = hex_chars[(signature[i] >> 4) & 0xF];
    pdf_data[hex_offset + i * 2 + 1] = hex_chars[signature[i] & 0xF];
  }
  // Remaining hex positions stay as '0' (already filled by write_for_signing)

  result.success = true;
  result.bytes_written = signature.size();
  return result;
}

// ============================================================
// Encryption Implementation
// ============================================================

void PdfWriter::set_encryption(const EncryptionConfig& config) {
  if (config.algorithm == EncryptionAlgorithm::None) {
    impl_->encryption_enabled = false;
    return;
  }

  if (config.owner_password.empty()) {
    // Owner password is required
    return;
  }

  impl_->encryption_enabled = true;
  impl_->encryption_config = config;

  // Upgrade PDF version if needed
  if (config.algorithm == EncryptionAlgorithm::AES_128 &&
      impl_->version < PdfVersion::v1_6) {
    impl_->version = PdfVersion::v1_6;
  } else if (config.algorithm == EncryptionAlgorithm::AES_256 &&
             impl_->version < PdfVersion::v1_7) {
    impl_->version = PdfVersion::v1_7;
  }

  // Allocate encryption dictionary object
  impl_->encrypt_obj_id = impl_->allocate_obj();

  // Ensure document ID is generated (required for encryption)
  if (impl_->doc_id1.empty()) {
    impl_->doc_id1 = generate_random_id();
  }
  if (impl_->doc_id2.empty()) {
    impl_->doc_id2 = impl_->doc_id1;
  }

  // Compute encryption parameters
  int32_t perm_flags = config.permissions.to_flags();

  if (config.algorithm == EncryptionAlgorithm::AES_256) {
    // AES-256 uses the revision-5 SHA-256 based key derivation.
    // Generate random file encryption key
    impl_->encryption_key = generate_random_bytes(32);

    // Generate random salts
    std::vector<uint8_t> user_val_salt = generate_random_bytes(8);
    std::vector<uint8_t> user_key_salt = generate_random_bytes(8);
    std::vector<uint8_t> owner_val_salt = generate_random_bytes(8);
    std::vector<uint8_t> owner_key_salt = generate_random_bytes(8);

    // Compute U value (48 bytes): hash(32) + validation_salt(8) + key_salt(8)
    crypto::SHA256 sha_u;
    std::string user_pwd = config.user_password;
    if (user_pwd.size() > 127) user_pwd.resize(127);
    sha_u.update(reinterpret_cast<const uint8_t*>(user_pwd.data()), user_pwd.size());
    sha_u.update(user_val_salt.data(), 8);
    sha_u.finalize();
    impl_->u_value.resize(48);
    sha_u.get_digest(impl_->u_value.data());
    memcpy(impl_->u_value.data() + 32, user_val_salt.data(), 8);
    memcpy(impl_->u_value.data() + 40, user_key_salt.data(), 8);

    // Compute UE value: AES-256 encrypt file key with user key
    crypto::SHA256 sha_ue;
    sha_ue.update(reinterpret_cast<const uint8_t*>(user_pwd.data()), user_pwd.size());
    sha_ue.update(user_key_salt.data(), 8);
    sha_ue.finalize();
    uint8_t ue_key[32];
    sha_ue.get_digest(ue_key);

    impl_->ue_value.resize(32);
    crypto::AES256 aes_ue;
    aes_ue.set_key(ue_key);
    uint8_t zero_iv[16] = {0};
    aes_ue.encrypt_cbc(impl_->encryption_key.data(), impl_->ue_value.data(), 32, zero_iv);

    // Compute O value (48 bytes)
    crypto::SHA256 sha_o;
    std::string owner_pwd = config.owner_password;
    if (owner_pwd.size() > 127) owner_pwd.resize(127);
    sha_o.update(reinterpret_cast<const uint8_t*>(owner_pwd.data()), owner_pwd.size());
    sha_o.update(owner_val_salt.data(), 8);
    sha_o.update(impl_->u_value.data(), 48);
    sha_o.finalize();
    impl_->o_value.resize(48);
    sha_o.get_digest(impl_->o_value.data());
    memcpy(impl_->o_value.data() + 32, owner_val_salt.data(), 8);
    memcpy(impl_->o_value.data() + 40, owner_key_salt.data(), 8);

    // Compute OE value
    crypto::SHA256 sha_oe;
    sha_oe.update(reinterpret_cast<const uint8_t*>(owner_pwd.data()), owner_pwd.size());
    sha_oe.update(owner_key_salt.data(), 8);
    sha_oe.update(impl_->u_value.data(), 48);
    sha_oe.finalize();
    uint8_t oe_key[32];
    sha_oe.get_digest(oe_key);

    impl_->oe_value.resize(32);
    crypto::AES256 aes_oe;
    aes_oe.set_key(oe_key);
    aes_oe.encrypt_cbc(impl_->encryption_key.data(), impl_->oe_value.data(), 32, zero_iv);

    // Compute Perms value (16 bytes)
    uint8_t perms_plaintext[16];
    perms_plaintext[0] = perm_flags & 0xFF;
    perms_plaintext[1] = (perm_flags >> 8) & 0xFF;
    perms_plaintext[2] = (perm_flags >> 16) & 0xFF;
    perms_plaintext[3] = (perm_flags >> 24) & 0xFF;
    perms_plaintext[4] = 0xFF;
    perms_plaintext[5] = 0xFF;
    perms_plaintext[6] = 0xFF;
    perms_plaintext[7] = 0xFF;
    perms_plaintext[8] = config.encrypt_metadata ? 'T' : 'F';
    perms_plaintext[9] = 'a';
    perms_plaintext[10] = 'd';
    perms_plaintext[11] = 'b';
    // Random bytes for 12-15
    auto rand_bytes = generate_random_bytes(4);
    memcpy(perms_plaintext + 12, rand_bytes.data(), 4);

    impl_->perms_value.resize(16);
    crypto::AES256 aes_perms;
    aes_perms.set_key(impl_->encryption_key.data());
    aes_perms.encrypt_block(perms_plaintext, impl_->perms_value.data());

  } else {
    // RC4 or AES-128 (revision 4)
    int key_length = 128;  // bits
    int revision = 4;
    if (config.algorithm == EncryptionAlgorithm::RC4_40) {
      key_length = 40;
      revision = 2;
    }

    // Compute O value
    impl_->o_value = compute_o_value(
        config.owner_password, config.user_password, key_length, revision);

    // Compute encryption key
    impl_->encryption_key = compute_encryption_key_r4(
        config.user_password.empty() ? "" : config.user_password,
        impl_->o_value,
        perm_flags,
        impl_->doc_id1,
        key_length,
        revision,
        config.encrypt_metadata);

    // Compute U value
    impl_->u_value = compute_u_value(
        impl_->encryption_key, impl_->doc_id1, revision);
  }
}

bool PdfWriter::is_encrypted() const {
  return impl_->encryption_enabled;
}

EncryptionAlgorithm PdfWriter::get_encryption_algorithm() const {
  if (!impl_->encryption_enabled) {
    return EncryptionAlgorithm::None;
  }
  return impl_->encryption_config.algorithm;
}

// ============================================================
// Timestamp Implementation
// ============================================================

void PdfWriter::set_timestamp_config(const TimestampConfig& config) {
  impl_->timestamp_enabled = !config.server_url.empty();
  impl_->timestamp_config = config;
}

bool PdfWriter::has_timestamp_config() const {
  return impl_->timestamp_enabled;
}

// ============================================================
// Bookmarks/Outlines Implementation
// ============================================================

int PdfWriter::add_bookmark(const std::string& title, int page_index, double y) {
  BookmarkConfig config;
  config.title = title;
  config.page_index = page_index;
  config.y_position = y;
  return add_bookmark(config);
}

int PdfWriter::add_bookmark(const BookmarkConfig& config) {
  int id = static_cast<int>(impl_->outlines.size());

  Impl::OutlineItem item;
  item.title = config.title;
  item.dest_page = config.page_index;
  item.dest_y = config.y_position;
  item.parent_id = -1;  // Top level
  item.open = config.open;
  item.bold = config.bold;
  item.italic = config.italic;
  item.obj_id = impl_->allocate_obj();

  // Link to previous sibling if exists
  for (int i = static_cast<int>(impl_->outlines.size()) - 1; i >= 0; --i) {
    if (impl_->outlines[i].parent_id == -1) {
      impl_->outlines[i].next_id = id;
      item.prev_id = i;
      break;
    }
  }

  impl_->outlines.push_back(item);
  return id;
}

int PdfWriter::add_child_bookmark(int parent_id, const std::string& title,
                                  int page_index, double y) {
  BookmarkConfig config;
  config.title = title;
  config.page_index = page_index;
  config.y_position = y;
  return add_child_bookmark(parent_id, config);
}

int PdfWriter::add_child_bookmark(int parent_id, const BookmarkConfig& config) {
  if (parent_id < 0 || parent_id >= static_cast<int>(impl_->outlines.size())) {
    return -1;
  }

  int id = static_cast<int>(impl_->outlines.size());

  Impl::OutlineItem item;
  item.title = config.title;
  item.dest_page = config.page_index;
  item.dest_y = config.y_position;
  item.parent_id = parent_id;
  item.open = config.open;
  item.bold = config.bold;
  item.italic = config.italic;
  item.obj_id = impl_->allocate_obj();

  // Link to parent
  auto& parent = impl_->outlines[parent_id];
  if (parent.first_child_id == -1) {
    parent.first_child_id = id;
  }

  // Link to previous sibling
  if (parent.last_child_id != -1) {
    impl_->outlines[parent.last_child_id].next_id = id;
    item.prev_id = parent.last_child_id;
  }
  parent.last_child_id = id;
  parent.child_count++;

  impl_->outlines.push_back(item);
  return id;
}

// ============================================================
// Attachments Implementation
// ============================================================

void PdfWriter::add_attachment(const std::string& filename,
                               const std::vector<uint8_t>& data,
                               const std::string& description) {
  AttachmentConfig config;
  config.filename = filename;
  config.data = data;
  config.description = description;
  add_attachment(config);
}

void PdfWriter::add_attachment(const AttachmentConfig& config) {
  Impl::AttachmentData att;
  att.filename = config.filename;
  att.description = config.description;
  att.mime_type = config.mime_type;
  att.data = config.data;
  att.compress = config.compress;
  att.embedded_file_obj_id = impl_->allocate_obj();
  att.filespec_obj_id = impl_->allocate_obj();

  impl_->attachments.push_back(att);

  // Upgrade version if needed (PDF 1.4+ for embedded files)
  if (impl_->version < PdfVersion::v1_4) {
    impl_->version = PdfVersion::v1_4;
  }
}

void PdfWriter::add_attachment_from_file(const std::string& path,
                                         const std::string& description) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return;

  size_t size = file.tellg();
  file.seekg(0);

  std::vector<uint8_t> data(size);
  file.read(reinterpret_cast<char*>(data.data()), size);

  // Extract filename from path
  std::string filename = path;
  size_t pos = path.find_last_of("/\\");
  if (pos != std::string::npos) {
    filename = path.substr(pos + 1);
  }

  add_attachment(filename, data, description);
}

// ============================================================
// Layers (OCG) Implementation
// ============================================================

std::string PdfWriter::add_layer(const std::string& name, bool visible) {
  LayerConfig config;
  config.name = name;
  config.visible = visible;
  return add_layer(config);
}

std::string PdfWriter::add_layer(const LayerConfig& config) {
  impl_->layer_counter++;
  std::string internal_name = "MC" + std::to_string(impl_->layer_counter - 1);

  Impl::LayerData layer;
  layer.name = config.name;
  layer.internal_name = internal_name;
  layer.visible = config.visible;
  layer.printable = config.printable;
  layer.locked = config.locked;
  layer.obj_id = impl_->allocate_obj();

  impl_->layers.push_back(layer);

  // Upgrade version if needed (PDF 1.5+ for OCG)
  if (impl_->version < PdfVersion::v1_5) {
    impl_->version = PdfVersion::v1_5;
  }

  return config.name;
}

// ============================================================
// Form Fields Implementation
// ============================================================

std::string PdfWriter::add_text_field(const TextFieldConfig& config) {
  if (!impl_->has_existing) {
    if (config.page < 0 || static_cast<size_t>(config.page) >= impl_->pages.size()) {
      return "";
    }
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Text;
  field.name = config.name;
  field.page = config.page;
  field.x = config.x;
  field.y = config.y;
  field.width = config.width;
  field.height = config.height;
  field.value = config.default_value;
  field.multiline = config.multiline;
  field.password = config.password;
  field.read_only = config.read_only;
  field.required = config.required;
  field.max_length = config.max_length;
  field.font_size = config.font_size;
  field.font_name = config.font_name.empty() ? "Helvetica" : config.font_name;
  field.field_obj_id = impl_->allocate_obj();
  field.appearance_obj_id = impl_->allocate_obj();

  if (!impl_->has_existing && static_cast<size_t>(config.page) < impl_->pages.size()) {
    impl_->pages[config.page].annotation_refs.push_back(field.field_obj_id);
  }

  impl_->form_fields.push_back(field);
  return config.name;
}

std::string PdfWriter::add_checkbox(const CheckboxConfig& config) {
  if (!impl_->has_existing) {
    if (config.page < 0 || static_cast<size_t>(config.page) >= impl_->pages.size()) {
      return "";
    }
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Checkbox;
  field.name = config.name;
  field.page = config.page;
  field.x = config.x;
  field.y = config.y;
  field.width = config.size;
  field.height = config.size;
  field.checked = config.checked;
  field.export_value = config.export_value;
  field.field_obj_id = impl_->allocate_obj();
  field.appearance_obj_id = impl_->allocate_obj();

  if (!impl_->has_existing && static_cast<size_t>(config.page) < impl_->pages.size()) {
    impl_->pages[config.page].annotation_refs.push_back(field.field_obj_id);
  }

  impl_->form_fields.push_back(field);
  return config.name;
}

std::string PdfWriter::add_radio_group(const RadioGroupConfig& config) {
  if (!impl_->has_existing) {
    if (config.page < 0 || static_cast<size_t>(config.page) >= impl_->pages.size()) {
      return "";
    }
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Radio;
  field.name = config.name;
  field.page = config.page;
  field.selected = config.selected;
  field.field_obj_id = impl_->allocate_obj();

  // Create widget for each option
  for (const auto& opt : config.options) {
    field.options.push_back(opt.value);
    field.radio_widget_ids.push_back(impl_->allocate_obj());
  }

  // Store option positions (we'll use them during writing)
  // For simplicity, store x, y, size in the options vector as "x,y,size"
  for (size_t i = 0; i < config.options.size(); ++i) {
    std::ostringstream oss;
    oss << config.options[i].x << "," << config.options[i].y << ","
        << config.options[i].size;
    if (i < field.options.size()) {
      field.options[i] = oss.str() + ":" + field.options[i];
    }
  }

  if (!impl_->has_existing && static_cast<size_t>(config.page) < impl_->pages.size()) {
    for (int widget_obj_id : field.radio_widget_ids) {
      impl_->pages[config.page].annotation_refs.push_back(widget_obj_id);
    }
  }

  impl_->form_fields.push_back(field);
  return config.name;
}

std::string PdfWriter::add_dropdown(const DropdownConfig& config) {
  if (!impl_->has_existing) {
    if (config.page < 0 || static_cast<size_t>(config.page) >= impl_->pages.size()) {
      return "";
    }
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Dropdown;
  field.name = config.name;
  field.page = config.page;
  field.x = config.x;
  field.y = config.y;
  field.width = config.width;
  field.height = config.height;
  field.options = config.options;
  field.selected = config.selected;
  field.editable = config.editable;
  field.field_obj_id = impl_->allocate_obj();
  field.appearance_obj_id = impl_->allocate_obj();

  if (!impl_->has_existing && static_cast<size_t>(config.page) < impl_->pages.size()) {
    impl_->pages[config.page].annotation_refs.push_back(field.field_obj_id);
  }

  impl_->form_fields.push_back(field);
  return config.name;
}

std::string PdfWriter::add_listbox(const std::string& name, int page, double x, double y,
                                   double w, double h,
                                   const std::vector<std::string>& options,
                                   int selected) {
  if (!impl_->has_existing) {
    if (page < 0 || static_cast<size_t>(page) >= impl_->pages.size()) {
      return "";
    }
  }
  if (selected < -1) {
    return "";
  }
  if (options.empty()) {
    if (selected != -1) {
      return "";
    }
  } else if (selected >= static_cast<int>(options.size())) {
    return "";
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Listbox;
  field.name = name;
  field.page = page;
  field.x = x;
  field.y = y;
  field.width = w;
  field.height = h;
  field.options = options;
  field.selected = options.empty() ? -1 : selected;
  field.field_obj_id = impl_->allocate_obj();
  field.appearance_obj_id = impl_->allocate_obj();

  if (!impl_->has_existing && static_cast<size_t>(page) < impl_->pages.size()) {
    impl_->pages[page].annotation_refs.push_back(field.field_obj_id);
  }

  impl_->form_fields.push_back(field);
  return name;
}

std::string PdfWriter::add_button(const std::string& name, int page, double x, double y,
                                  double w, double h, const std::string& caption) {
  if (!impl_->has_existing) {
    if (page < 0 || static_cast<size_t>(page) >= impl_->pages.size()) {
      return "";
    }
  }

  ensure_acroform();

  Impl::FormFieldData field;
  field.type = Impl::FormFieldData::Type::Button;
  field.name = name;
  field.page = page;
  field.x = x;
  field.y = y;
  field.width = w;
  field.height = h;
  field.caption = caption;
  field.field_obj_id = impl_->allocate_obj();
  field.appearance_obj_id = impl_->allocate_obj();

  if (!impl_->has_existing && static_cast<size_t>(page) < impl_->pages.size()) {
    impl_->pages[page].annotation_refs.push_back(field.field_obj_id);
  }

  impl_->form_fields.push_back(field);
  return name;
}

// ============================================================
// Gradients Implementation
// ============================================================

std::string PdfWriter::create_gradient(const GradientConfig& config) {
  if (config.stops.size() < 2) {
    return "";  // Need at least 2 color stops
  }

  impl_->gradient_counter++;
  std::string name = "P" + std::to_string(impl_->gradient_counter);

  Impl::GradientData grad;
  grad.name = name;
  grad.type = config.type;
  grad.x1 = config.x1;
  grad.y1 = config.y1;
  grad.x2 = config.x2;
  grad.y2 = config.y2;
  grad.cx = config.cx;
  grad.cy = config.cy;
  grad.r = config.r;
  grad.stops = config.stops;
  grad.extend_start = config.extend_start;
  grad.extend_end = config.extend_end;
  grad.function_obj_id = impl_->allocate_obj();
  grad.shading_obj_id = impl_->allocate_obj();
  grad.pattern_obj_id = impl_->allocate_obj();

  impl_->gradients.push_back(grad);
  return name;
}

// ============================================================
// Page Templates Implementation
// ============================================================

Template::Template(PdfWriter* writer, double width, double height)
    : writer_(writer),
      width_(width),
      height_(height),
      builder_(std::make_unique<PageBuilder>(writer)) {}

Template::~Template() = default;

Template::Template(Template&& other) noexcept
    : writer_(other.writer_),
      width_(other.width_),
      height_(other.height_),
      builder_(std::move(other.builder_)) {}

Template& Template::operator=(Template&& other) noexcept {
  if (this != &other) {
    writer_ = other.writer_;
    width_ = other.width_;
    height_ = other.height_;
    builder_ = std::move(other.builder_);
  }
  return *this;
}

void Template::set_size(double width, double height) {
  width_ = width;
  height_ = height;
}

PageBuilder& Template::builder() {
  return *builder_;
}

Template PdfWriter::create_template(double width, double height) {
  return Template(this, width, height);
}

std::string PdfWriter::add_template(const Template& tmpl) {
  if (!tmpl.builder_) {
    return "";
  }
  return add_template(tmpl.width_, tmpl.height_, *tmpl.builder_);
}

std::string PdfWriter::add_template(double width, double height,
                                    const PageBuilder& builder) {
  impl_->template_counter++;
  std::string name = "Fm" + std::to_string(impl_->template_counter);

  Impl::TemplateData tdata;
  tdata.name = name;
  tdata.width = width;
  tdata.height = height;
  tdata.content = builder.content_;

  for (const auto& img : impl_->images) {
    if (tdata.content.find("/" + img.name + " Do") != std::string::npos) {
      tdata.used_images.push_back(img.name);
    }
  }
  for (const auto& font : impl_->fonts) {
    if (tdata.content.find("/" + font.name + " ") != std::string::npos) {
      tdata.used_fonts.push_back(font.name);
    }
  }
  for (const auto& ef : impl_->embedded_fonts) {
    if (tdata.content.find("/" + ef.name + " ") != std::string::npos) {
      tdata.used_fonts.push_back(ef.name);
    }
  }
  for (const auto& gs : impl_->extgstates) {
    if (tdata.content.find("/" + gs.name + " gs") != std::string::npos) {
      tdata.used_extgstates.push_back(gs.name);
    }
  }
  for (const auto& layer : impl_->layers) {
    if (tdata.content.find("/" + layer.internal_name + " BDC") != std::string::npos) {
      tdata.used_layers.push_back(layer.internal_name);
    }
  }
  for (const auto& grad : impl_->gradients) {
    if (tdata.content.find("/" + grad.name + " ") != std::string::npos) {
      tdata.used_patterns.push_back(grad.name);
    }
  }
  for (const auto& tmpl_ref : impl_->templates) {
    if (tdata.content.find("/" + tmpl_ref.name + " Do") != std::string::npos) {
      tdata.used_templates.push_back(tmpl_ref.name);
    }
  }
  for (const auto& resource : builder.custom_resources_) {
    Impl::CustomResourceRef custom_resource;
    custom_resource.category = resource.category;
    custom_resource.name = resource.name;
    custom_resource.obj_id = resource.obj_id;
    tdata.custom_resources.push_back(std::move(custom_resource));
  }
  tdata.obj_id = impl_->allocate_obj();

  impl_->templates.push_back(tdata);
  return name;
}

void PdfWriter::use_template(PageBuilder& builder, const std::string& name,
                             double x, double y, double scale_x, double scale_y) {
  builder.save_state();
  builder.concat_matrix(scale_x, 0, 0, scale_y, x, y);
  builder.emit("/" + name + " Do\n");
  builder.restore_state();
}

// ============================================================
// Text Layout Implementation
// ============================================================

struct TextLayout::Impl {
  double width = 0;
  double max_height = 0;
  TextAlign alignment = TextAlign::Left;
  TextStyle style;

  struct Line {
    std::string text;
    double width;
  };
  std::vector<Line> lines;
  std::string pending_text;

  double calculated_height = 0;
  bool overflow = false;
};

TextLayout::TextLayout() : impl_(new Impl()) {}
TextLayout::~TextLayout() { delete impl_; }

void TextLayout::set_width(double width) { impl_->width = width; }
void TextLayout::set_max_height(double height) { impl_->max_height = height; }
void TextLayout::set_alignment(TextAlign align) { impl_->alignment = align; }
void TextLayout::set_style(const TextStyle& style) { impl_->style = style; }

void TextLayout::add_text(const std::string& text) {
  impl_->pending_text += text;
}

void TextLayout::add_line_break() {
  impl_->lines.push_back({impl_->pending_text, 0});
  impl_->pending_text.clear();
}

void TextLayout::add_paragraph_break() {
  add_line_break();
  impl_->lines.push_back({"", 0});  // Empty line for paragraph spacing
}

double TextLayout::get_height() const {
  return impl_->calculated_height;
}

int TextLayout::get_line_count() const {
  return static_cast<int>(impl_->lines.size());
}

bool TextLayout::has_overflow() const {
  return impl_->overflow;
}

TextLayout PdfWriter::create_text_layout() {
  return TextLayout();
}

void PdfWriter::draw_text_layout(PageBuilder& builder, const TextLayout& layout,
                                 double x, double y) {
  TextLayout& mutable_layout = const_cast<TextLayout&>(layout);
  auto* impl = mutable_layout.impl_;
  std::string font_name = impl->style.font_name;
  if (font_name.empty() || !has_font_resource(font_name)) {
    font_name = add_standard_font(StandardFont::Helvetica);
  }

  auto measure_text_width = [&](const std::string& text,
                                const FontMetrics* metrics) -> double {
    double width = 0.0;
    for (size_t i = 0; i < text.size(); ++i) {
      unsigned char ch = static_cast<unsigned char>(text[i]);
      if (metrics && metrics->units_per_em > 0) {
        width += metrics->get_width(static_cast<uint32_t>(ch)) *
                 impl->style.font_size / metrics->units_per_em;
      } else {
        width += impl->style.font_size * (ch == ' ' ? 0.28 : 0.5);
      }
      if (i + 1 < text.size()) {
        width += impl->style.letter_spacing;
        if (text[i] == ' ') {
          width += impl->style.word_spacing;
        }
      }
    }
    return width;
  };

  auto wrap_line = [&](const std::string& text,
                       const FontMetrics* metrics,
                       std::vector<TextLayout::Impl::Line>* out_lines) {
    std::vector<std::string> words;
    std::string word;

    if (text.empty()) {
      out_lines->push_back({"", 0.0});
      return;
    }
    if (impl->width <= 0.0) {
      out_lines->push_back({text, measure_text_width(text, metrics)});
      return;
    }

    for (char ch : text) {
      if (ch == ' ' || ch == '\t') {
        if (!word.empty()) {
          words.push_back(word);
          word.clear();
        }
      } else {
        word.push_back(ch);
      }
    }
    if (!word.empty()) {
      words.push_back(word);
    }
    if (words.empty()) {
      out_lines->push_back({"", 0.0});
      return;
    }

    auto append_word_segments = [&](const std::string& source_word,
                                    std::string* current_line) {
      std::string segment;
      for (char ch : source_word) {
        std::string candidate = segment + ch;
        if (!segment.empty() &&
            measure_text_width(candidate, metrics) > impl->width) {
          if (!current_line->empty()) {
            out_lines->push_back(
                {*current_line, measure_text_width(*current_line, metrics)});
            current_line->clear();
          }
          out_lines->push_back({segment, measure_text_width(segment, metrics)});
          segment.assign(1, ch);
        } else {
          segment = candidate;
        }
      }
      if (!segment.empty()) {
        *current_line = segment;
      }
    };

    std::string current_line;
    for (const auto& current_word : words) {
      std::string candidate =
          current_line.empty() ? current_word : current_line + " " + current_word;
      if (current_line.empty() &&
          measure_text_width(current_word, metrics) > impl->width) {
        append_word_segments(current_word, &current_line);
      } else if (measure_text_width(candidate, metrics) <= impl->width) {
        current_line = candidate;
      } else {
        out_lines->push_back(
            {current_line, measure_text_width(current_line, metrics)});
        if (measure_text_width(current_word, metrics) > impl->width) {
          current_line.clear();
          append_word_segments(current_word, &current_line);
        } else {
          current_line = current_word;
        }
      }
    }
    if (!current_line.empty()) {
      out_lines->push_back(
          {current_line, measure_text_width(current_line, metrics)});
    }
  };

  if (!impl->pending_text.empty()) {
    impl->lines.push_back({impl->pending_text, 0});
    impl->pending_text.clear();
  }

  const FontMetrics* metrics = get_font_metrics(font_name);
  std::vector<TextLayout::Impl::Line> wrapped_lines;
  wrapped_lines.reserve(std::max<size_t>(1, impl->lines.size()));
  for (const auto& line : impl->lines) {
    wrap_line(line.text, metrics, &wrapped_lines);
  }
  impl->lines = wrapped_lines;

  double line_height = impl->style.font_size * impl->style.line_height;
  int drawable_lines = static_cast<int>(impl->lines.size());
  impl->overflow = false;
  if (impl->max_height > 0.0 && line_height > 0.0) {
    drawable_lines = static_cast<int>(std::floor(impl->max_height / line_height + 1e-9));
    if (drawable_lines < 0) {
      drawable_lines = 0;
    }
    if (drawable_lines < static_cast<int>(impl->lines.size())) {
      impl->overflow = true;
    }
  }

  builder.begin_text();
  builder.set_font(font_name, impl->style.font_size);
  builder.set_fill_color(impl->style.r, impl->style.g, impl->style.b);
  if (impl->style.letter_spacing != 0.0) {
    builder.emit_number(impl->style.letter_spacing);
    builder.emit("Tc\n");
  }
  if (impl->style.word_spacing != 0.0) {
    builder.emit_number(impl->style.word_spacing);
    builder.emit("Tw\n");
  }

  double current_y = y;
  int drawn_lines = 0;
  for (const auto& line : impl->lines) {
    if (drawn_lines >= drawable_lines) {
      break;
    }

    double line_x = x;
    if ((impl->alignment == TextAlign::Center || impl->alignment == TextAlign::Right) &&
        impl->width > 0.0) {
      double text_width = line.width;
      if (impl->alignment == TextAlign::Center) {
        line_x = x + (impl->width - text_width) / 2.0;
      } else {
        line_x = x + impl->width - text_width;
      }
    }

    if (!line.text.empty()) {
      builder.emit_number(1);
      builder.emit_number(0);
      builder.emit_number(0);
      builder.emit_number(1);
      builder.emit_number(line_x);
      builder.emit_number(current_y);
      builder.emit("Tm\n");
      builder.show_text(line.text);
    }
    current_y -= line_height;
    drawn_lines++;
  }

  builder.end_text();
  impl->calculated_height = line_height * drawn_lines;
}

// ============================================================================
// Watermarks
// ============================================================================

void PdfWriter::set_watermark(const TextWatermarkConfig& config) {
  impl_->global_watermark.is_text = true;
  impl_->global_watermark.text_config = config;
  impl_->global_watermark.active = true;
}

void PdfWriter::set_watermark(const ImageWatermarkConfig& config) {
  impl_->global_watermark.is_text = false;
  impl_->global_watermark.image_config = config;
  impl_->global_watermark.active = true;
}

void PdfWriter::add_page_watermark(int page_index, const TextWatermarkConfig& config) {
  Impl::WatermarkData wm;
  wm.is_text = true;
  wm.text_config = config;
  wm.active = true;
  impl_->page_watermarks[page_index] = wm;
}

void PdfWriter::add_page_watermark(int page_index, const ImageWatermarkConfig& config) {
  Impl::WatermarkData wm;
  wm.is_text = false;
  wm.image_config = config;
  wm.active = true;
  impl_->page_watermarks[page_index] = wm;
}

void PdfWriter::clear_watermark() {
  impl_->global_watermark.active = false;
}

// ============================================================================
// Headers/Footers
// ============================================================================

void PdfWriter::set_header(const HeaderConfig& config) {
  impl_->global_header_footer.header = config;
  impl_->global_header_footer.has_header = true;
}

void PdfWriter::set_footer(const FooterConfig& config) {
  impl_->global_header_footer.footer = config;
  impl_->global_header_footer.has_footer = true;
}

void PdfWriter::set_page_header(int page_index, const HeaderConfig& config) {
  impl_->page_header_footers[page_index].header = config;
  impl_->page_header_footers[page_index].has_header = true;
}

void PdfWriter::set_page_footer(int page_index, const FooterConfig& config) {
  impl_->page_header_footers[page_index].footer = config;
  impl_->page_header_footers[page_index].has_footer = true;
}

void PdfWriter::skip_header_footer(int page_index) {
  impl_->skip_header_footer_pages.insert(page_index);
}

void PdfWriter::clear_header() {
  impl_->global_header_footer.has_header = false;
}

void PdfWriter::clear_footer() {
  impl_->global_header_footer.has_footer = false;
}

int PdfWriter::get_page_count() const {
  if (impl_->has_existing && impl_->pages.empty()) {
    Pdf existing_pdf;
    if (parse_from_memory(
            impl_->existing_pdf.data(), impl_->existing_pdf.size(), &existing_pdf) &&
        existing_pdf.load_document_structure()) {
      return static_cast<int>(existing_pdf.catalog.pages.size());
    }
  }
  return static_cast<int>(impl_->pages.size());
}

// ============================================================================
// Bates Stamping
// ============================================================================

void PdfWriter::set_bates_numbering(const BatesConfig& config) {
  impl_->bates_config = config;
  impl_->has_bates = true;
}

void PdfWriter::clear_bates_numbering() {
  impl_->has_bates = false;
}

void PdfWriter::skip_bates_number(int page_index) {
  impl_->skip_bates_pages.insert(page_index);
}

// ============================================================================
// Highlight Annotations
// ============================================================================

void PdfWriter::add_highlight(int page_index, const HighlightConfig& config) {
  if (page_index < 0 || page_index >= static_cast<int>(impl_->pages.size())) {
    return;  // Invalid page index
  }

  Impl::HighlightAnnotationData ha;
  ha.type = MarkupType::Highlight;
  ha.quads = config.quads;
  ha.alpha = config.alpha;
  ha.author = config.author;
  ha.contents = config.contents;
  ha.print = config.print;

  if (config.color_preset == HighlightColor::Custom) {
    ha.r = config.r;
    ha.g = config.g;
    ha.b = config.b;
  } else {
    get_highlight_color(config.color_preset, ha.r, ha.g, ha.b);
  }

  ha.obj_id = impl_->allocate_obj();
  impl_->pages[page_index].highlights.push_back(ha);
}

void PdfWriter::add_text_markup(int page_index, const TextMarkupConfig& config) {
  if (page_index < 0 || page_index >= static_cast<int>(impl_->pages.size())) {
    return;  // Invalid page index
  }

  Impl::HighlightAnnotationData ha;
  ha.type = config.type;
  ha.quads = config.quads;
  ha.r = config.r;
  ha.g = config.g;
  ha.b = config.b;
  ha.alpha = config.alpha;
  ha.author = config.author;
  ha.contents = config.contents;
  ha.print = config.print;

  ha.obj_id = impl_->allocate_obj();
  impl_->pages[page_index].highlights.push_back(ha);
}

std::string PdfWriter::add_image(const ImageData& image,
                                  ImageCompression compression) {
  if (!image.valid()) {
    return "";
  }

  impl_->image_counter++;
  std::string name = "Im" + std::to_string(impl_->image_counter);

  Impl::ImageResource res;
  res.name = name;
  res.obj_id = impl_->allocate_obj();
  res.data = image;
  res.compression = compression;

  // If CCITT compression requested, convert image to monochrome and encode
  if (compression == ImageCompression::CCITTFax) {
    std::vector<uint8_t> mono_data = ccitt::convert_to_monochrome(
        image.raw_data.data(), image.width, image.height, image.channels);
    std::string error;
    if (ccitt::encode_ccitt_g4(mono_data.data(), image.width, image.height,
                               res.ccitt_data, &error)) {
      res.ccitt_width = image.width;
      res.ccitt_height = image.height;
    }
  }

  impl_->images.push_back(std::move(res));

  return name;
}

std::string PdfWriter::add_ccitt_image(const uint8_t* mono_data, int width,
                                       int height) {
  if (!mono_data || width <= 0 || height <= 0) {
    return "";
  }

  impl_->image_counter++;
  std::string name = "Im" + std::to_string(impl_->image_counter);

  Impl::ImageResource res;
  res.name = name;
  res.obj_id = impl_->allocate_obj();
  res.compression = ImageCompression::CCITTFax;
  res.is_ccitt_only = true;
  res.ccitt_width = width;
  res.ccitt_height = height;

  std::string error;
  if (!ccitt::encode_ccitt_g4(mono_data, width, height, res.ccitt_data,
                              &error)) {
    return "";
  }

  impl_->images.push_back(std::move(res));

  return name;
}

std::string PdfWriter::add_image_with_alpha(const ImageData& image,
                                            ImageCompression compression) {
  if (!image.valid()) {
    return "";
  }

  impl_->image_counter++;
  std::string name = "Im" + std::to_string(impl_->image_counter);

  Impl::ImageResource res;
  res.name = name;
  res.obj_id = impl_->allocate_obj();
  res.data = image;
  res.compression = compression;

  // If image has alpha channel, extract it for soft mask
  if (image.channels == 4) {
    res.has_alpha = true;
    res.alpha_data = image.extract_alpha();
    res.smask_obj_id = impl_->allocate_obj();  // Allocate object for SMask
  }

  impl_->images.push_back(std::move(res));

  return name;
}

std::string PdfWriter::add_image_with_mask(const ImageData& image,
                                           const SoftMaskConfig& mask,
                                           ImageCompression compression) {
  if (!image.valid()) {
    return "";
  }

  // Validate mask dimensions match image
  if (mask.width != image.width || mask.height != image.height) {
    return "";  // Mask dimensions must match image
  }

  impl_->image_counter++;
  std::string name = "Im" + std::to_string(impl_->image_counter);

  Impl::ImageResource res;
  res.name = name;
  res.obj_id = impl_->allocate_obj();
  res.data = image;
  res.compression = compression;

  // Use the provided mask
  res.has_alpha = true;
  if (mask.invert) {
    // Invert mask values
    res.alpha_data.reserve(mask.mask_data.size());
    for (uint8_t v : mask.mask_data) {
      res.alpha_data.push_back(255 - v);
    }
  } else {
    res.alpha_data = mask.mask_data;
  }
  res.smask_obj_id = impl_->allocate_obj();

  impl_->images.push_back(std::move(res));

  return name;
}

std::string PdfWriter::add_standard_font(StandardFont font) {
  // Check if already added
  for (const auto& f : impl_->fonts) {
    if (f.font == font) {
      return f.name;
    }
  }

  impl_->font_counter++;
  std::string name = "F" + std::to_string(impl_->font_counter);

  Impl::FontResource res;
  res.name = name;
  res.obj_id = impl_->allocate_obj();
  res.font = font;
  impl_->fonts.push_back(std::move(res));

  return name;
}

bool PdfWriter::has_font_resource(const std::string& name) const {
  for (const auto& font : impl_->fonts) {
    if (font.name == name) {
      return true;
    }
  }
  for (const auto& font : impl_->embedded_fonts) {
    if (font.name == name) {
      return true;
    }
  }
  return false;
}

bool PdfWriter::has_image_resource(const std::string& name) const {
  for (const auto& image : impl_->images) {
    if (image.name == name) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// Font Embedding
// ============================================================================

namespace {

// Generate a random 6-letter subset tag
std::string generate_subset_tag() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 25);

  std::string tag;
  for (int i = 0; i < 6; i++) {
    tag += static_cast<char>('A' + dis(gen));
  }
  return tag + "+";
}

// Generate ToUnicode CMap for embedded font
std::string generate_tounicode_cmap(const std::string& font_name,
                                     const std::set<uint32_t>& used_chars,
                                     const FontMetrics& metrics) {
  std::ostringstream cmap;

  cmap << "/CIDInit /ProcSet findresource begin\n";
  cmap << "12 dict begin\n";
  cmap << "begincmap\n";
  cmap << "/CIDSystemInfo <<\n";
  cmap << "  /Registry (Adobe)\n";
  cmap << "  /Ordering (UCS)\n";
  cmap << "  /Supplement 0\n";
  cmap << ">> def\n";
  cmap << "/CMapName /Adobe-Identity-UCS def\n";
  cmap << "/CMapType 2 def\n";
  cmap << "1 begincodespacerange\n";
  cmap << "<0000> <FFFF>\n";
  cmap << "endcodespacerange\n";

  // Build mapping entries - map glyph IDs to Unicode codepoints
  std::vector<std::pair<uint16_t, uint32_t>> mappings;
  for (uint32_t cp : used_chars) {
    auto it = metrics.char_to_glyph.find(cp);
    if (it != metrics.char_to_glyph.end()) {
      mappings.push_back({it->second, cp});
    }
  }

  // Sort by glyph ID
  std::sort(mappings.begin(), mappings.end());

  // Write in chunks of 100
  size_t i = 0;
  while (i < mappings.size()) {
    size_t chunk_size = std::min(static_cast<size_t>(100), mappings.size() - i);
    cmap << chunk_size << " beginbfchar\n";
    for (size_t j = 0; j < chunk_size; j++) {
      auto& m = mappings[i + j];
      char buf[32];
      snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", m.first, m.second);
      cmap << buf;
    }
    cmap << "endbfchar\n";
    i += chunk_size;
  }

  cmap << "endcmap\n";
  cmap << "CMapName currentdict /CMap defineresource pop\n";
  cmap << "end\n";
  cmap << "end\n";

  return cmap.str();
}

}  // namespace

std::string PdfWriter::add_truetype_font(const std::string& path,
                                          FontEmbedding embedding) {
  FontData font = FontData::FromFile(path);
  if (!font.is_valid()) {
    return "";
  }
  return add_truetype_font(font, embedding);
}

std::string PdfWriter::add_truetype_font(const uint8_t* data, size_t size,
                                          FontEmbedding embedding) {
  FontData font = FontData::FromMemory(data, size);
  if (!font.is_valid()) {
    return "";
  }
  return add_truetype_font(font, embedding);
}

std::string PdfWriter::add_truetype_font(const FontData& font,
                                          FontEmbedding embedding) {
  if (!font.is_valid()) {
    return "";
  }

  // Check if this font is already added (by font name)
  for (const auto& ef : impl_->embedded_fonts) {
    if (ef.font_data.metrics.font_name == font.metrics.font_name) {
      return ef.name;
    }
  }

  impl_->font_counter++;
  std::string name = "F" + std::to_string(impl_->font_counter);

  Impl::EmbeddedFontResource res;
  res.name = name;
  res.font_data = font;
  res.embedding = embedding;
  res.subset_tag = generate_subset_tag();

  // Allocate object IDs
  // For TrueType embedding as Type0 (CID) font:
  // - Type0 font dictionary
  // - CIDFont (descendant)
  // - FontDescriptor
  // - FontFile2 stream
  // - ToUnicode CMap stream
  res.font_obj_id = impl_->allocate_obj();
  res.descendant_obj_id = impl_->allocate_obj();
  res.descriptor_obj_id = impl_->allocate_obj();
  res.file_obj_id = impl_->allocate_obj();
  res.tounicode_obj_id = impl_->allocate_obj();

  impl_->embedded_fonts.push_back(std::move(res));

  return name;
}

const FontMetrics* PdfWriter::get_font_metrics(const std::string& name) const {
  for (const auto& ef : impl_->embedded_fonts) {
    if (ef.name == name) {
      return &ef.font_data.metrics;
    }
  }
  return nullptr;
}

void PdfWriter::mark_chars_used(const std::string& font_name,
                                 const std::string& text) {
  for (auto& ef : impl_->embedded_fonts) {
    if (ef.name == font_name) {
      // Decode UTF-8 and add codepoints
      const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
      const uint8_t* end = p + text.size();
      while (p < end) {
        uint32_t cp;
        if ((*p & 0x80) == 0) {
          cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
          cp = (*p++ & 0x1F) << 6;
          if (p < end) cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
          cp = (*p++ & 0x0F) << 12;
          if (p < end) cp |= (*p++ & 0x3F) << 6;
          if (p < end) cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF8) == 0xF0) {
          cp = (*p++ & 0x07) << 18;
          if (p < end) cp |= (*p++ & 0x3F) << 12;
          if (p < end) cp |= (*p++ & 0x3F) << 6;
          if (p < end) cp |= (*p++ & 0x3F);
        } else {
          p++;
          continue;
        }
        ef.used_chars.insert(cp);
      }
      return;
    }
  }
}

void PdfWriter::add_page(PageSize size, const PageBuilder& builder) {
  Impl::PageData page;
  page.size = size;
  page.content = builder.content();

  // Track used resources from content
  for (const auto& img : impl_->images) {
    if (page.content.find("/" + img.name + " Do") != std::string::npos) {
      page.used_images.push_back(img.name);
    }
  }
  for (const auto& font : impl_->fonts) {
    if (page.content.find("/" + font.name + " ") != std::string::npos) {
      page.used_fonts.push_back(font.name);
    }
  }
  // Also track embedded fonts
  for (const auto& ef : impl_->embedded_fonts) {
    if (page.content.find("/" + ef.name + " ") != std::string::npos) {
      page.used_fonts.push_back(ef.name);
    }
  }
  // Track ExtGStates
  for (const auto& gs : impl_->extgstates) {
    if (page.content.find("/" + gs.name + " gs") != std::string::npos) {
      page.used_extgstates.push_back(gs.name);
    }
  }
  // Track layers
  for (const auto& layer : impl_->layers) {
    if (page.content.find("/" + layer.internal_name + " BDC") != std::string::npos) {
      page.used_layers.push_back(layer.internal_name);
    }
  }
  // Track patterns (gradients)
  for (const auto& grad : impl_->gradients) {
    if (page.content.find("/" + grad.name + " ") != std::string::npos) {
      page.used_patterns.push_back(grad.name);
    }
  }
  // Track templates
  for (const auto& tmpl : impl_->templates) {
    if (page.content.find("/" + tmpl.name + " Do") != std::string::npos) {
      page.used_templates.push_back(tmpl.name);
    }
  }

  // Copy link annotations from builder
  for (const auto& link : builder.links_) {
    Impl::LinkAnnotationData la;
    la.x = link.x;
    la.y = link.y;
    la.width = link.width;
    la.height = link.height;
    la.action = link.action;
    la.uri = link.uri;
    la.dest_page = link.dest_page;
    la.dest_y = link.dest_y;
    la.show_border = link.show_border;
    la.obj_id = impl_->allocate_obj();
    page.links.push_back(la);
  }

  // Copy highlight annotations from builder
  for (const auto& hl : builder.highlights_) {
    Impl::HighlightAnnotationData ha;
    ha.type = hl.type;
    ha.quads = hl.quads;
    ha.r = hl.r;
    ha.g = hl.g;
    ha.b = hl.b;
    ha.alpha = hl.alpha;
    ha.author = hl.author;
    ha.contents = hl.contents;
    ha.print = hl.print;
    ha.obj_id = impl_->allocate_obj();
    page.highlights.push_back(ha);
  }

  for (const auto& resource : builder.custom_resources_) {
    Impl::CustomResourceRef custom_resource;
    custom_resource.category = resource.category;
    custom_resource.name = resource.name;
    custom_resource.obj_id = resource.obj_id;
    page.custom_resources.push_back(std::move(custom_resource));
  }

  impl_->pages.push_back(std::move(page));
}

void PdfWriter::add_page(PageSize size,
                         std::function<void(PageBuilder&)> build_fn) {
  PageBuilder builder(this);
  build_fn(builder);
  add_page(size, builder);
}

void PdfWriter::add_image_page(const ImageData& img, PageSize size,
                               double margin, bool fit_to_page) {
  if (!img.valid()) return;

  std::string img_name = add_image(img);

  add_page(size, [&](PageBuilder& builder) {
    double page_w = size.width - 2 * margin;
    double page_h = size.height - 2 * margin;

    double img_w, img_h;
    if (fit_to_page) {
      // Scale to fit within page
      double scale_x = page_w / img.width;
      double scale_y = page_h / img.height;
      double scale = std::min(scale_x, scale_y);
      img_w = img.width * scale;
      img_h = img.height * scale;
    } else {
      // Use image dimensions at 72 DPI
      img_w = static_cast<double>(img.width);
      img_h = static_cast<double>(img.height);
    }

    // Center on page
    double x = margin + (page_w - img_w) / 2;
    double y = margin + (page_h - img_h) / 2;

    builder.draw_image(img_name, x, y, img_w, img_h);
  });
}

void PdfWriter::add_image_page_fit(const ImageData& img, double margin) {
  if (!img.valid()) return;

  PageSize size =
      PageSize::FromPixels(img.width + static_cast<int>(2 * margin),
                           img.height + static_cast<int>(2 * margin));
  add_image_page(img, size, margin, false);
}

// ============================================================================
// PDF Merge and Split Implementation
// ============================================================================

// Helper class to recursively copy PDF objects from a source PDF into
// new objects in the target PdfWriter, remapping all object references.
class ObjectCopier {
 public:
  ObjectCopier(const Pdf& src, PdfWriter::Impl* dst_impl)
      : source_(src), impl_(dst_impl) {}

  // Copy a Value from the source PDF, creating new objects as needed.
  // Returns the serialized PDF representation with remapped references.
  std::string serialize_value(const Value& val) {
    switch (val.type) {
      case Value::BOOLEAN:
        return val.boolean ? "true" : "false";

      case Value::NUMBER: {
        // Use integer representation when possible
        if (val.number == static_cast<int64_t>(val.number) &&
            std::abs(val.number) < 1e15) {
          return std::to_string(static_cast<int64_t>(val.number));
        }
        return format_number(val.number);
      }

      case Value::STRING: {
        // Check if string contains non-printable characters
        bool has_binary = false;
        for (unsigned char c : val.str) {
          if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            has_binary = true;
            break;
          }
        }
        if (has_binary) {
          // Use hex string encoding for binary content
          std::string hex = "<";
          for (unsigned char c : val.str) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", c);
            hex += buf;
          }
          hex += ">";
          return hex;
        }
        // Use literal string with escaping
        std::string escaped;
        escaped += '(';
        for (char c : val.str) {
          switch (c) {
            case '(': escaped += "\\("; break;
            case ')': escaped += "\\)"; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
          }
        }
        escaped += ')';
        return escaped;
      }

      case Value::NAME:
        return "/" + val.name;

      case Value::NULL_OBJ:
        return "null";

      case Value::ARRAY: {
        std::string s = "[";
        for (size_t i = 0; i < val.array.size(); ++i) {
          if (i > 0) s += " ";
          s += serialize_value(val.array[i]);
        }
        s += "]";
        return s;
      }

      case Value::DICTIONARY: {
        std::string s = "<<\n";
        for (const auto& kv : val.dict) {
          s += "/" + kv.first + " " + serialize_value(kv.second) + "\n";
        }
        s += ">>";
        return s;
      }

      case Value::REFERENCE: {
        // This is the key part: we need to copy the referenced object
        // from the source PDF and create a new object in the target PDF.
        int new_id = copy_object(val.ref_object_number,
                                 val.ref_generation_number);
        return std::to_string(new_id) + " 0 R";
      }

      case Value::STREAM: {
        // Streams are indirect objects; they need to be written as
        // separate objects. Create a new imported object for the stream.
        int new_id = impl_->allocate_obj();

        PdfWriter::Impl::ImportedObject imp;
        imp.obj_id = new_id;
        imp.is_stream = true;

        // Serialize stream dictionary, skipping Length (we override it)
        std::string dict_str = "<<\n";
        for (const auto& kv : val.stream.dict) {
          if (kv.first == "Length") continue;  // Override below
          dict_str += "/" + kv.first + " " + serialize_value(kv.second) + "\n";
        }
        dict_str += "/Length " + std::to_string(val.stream.data.size()) + "\n";
        dict_str += ">>";

        imp.stream_dict = dict_str;
        imp.stream_data = val.stream.data;

        impl_->imported_objects.push_back(std::move(imp));

        return std::to_string(new_id) + " 0 R";
      }

      default:
        return "null";
    }
  }

  // Copy an indirect object from the source PDF. Returns the new object ID
  // in the target PDF. Caches results to avoid duplicating objects.
  int copy_object(uint32_t obj_num, uint16_t gen_num) {
    uint64_t key = (static_cast<uint64_t>(obj_num) << 16) |
                   static_cast<uint64_t>(gen_num);

    auto it = obj_map_.find(key);
    if (it != obj_map_.end()) {
      return it->second;
    }

    // Allocate a new object ID first (before resolving, to handle cycles)
    int new_id = impl_->allocate_obj();
    obj_map_[key] = new_id;

    // Resolve the object in the source PDF
    ResolvedObject resolved = resolve_reference(source_, obj_num, gen_num);
    if (!resolved.success) {
      // Write a null placeholder for unresolvable objects
      PdfWriter::Impl::ImportedObject imp;
      imp.obj_id = new_id;
      imp.is_stream = false;
      imp.serialized_data = "null";
      impl_->imported_objects.push_back(std::move(imp));
      return new_id;
    }

    const Value& val = resolved.value;

    if (val.type == Value::STREAM) {
      // Stream object - serialize dict and keep raw stream data
      PdfWriter::Impl::ImportedObject imp;
      imp.obj_id = new_id;
      imp.is_stream = true;

      std::string dict_str = "<<\n";
      for (const auto& kv : val.stream.dict) {
        if (kv.first == "Length") continue;  // Override below
        dict_str += "/" + kv.first + " " + serialize_value(kv.second) + "\n";
      }
      // Always write Length with actual stream data size
      dict_str += "/Length " + std::to_string(val.stream.data.size()) + "\n";
      dict_str += ">>";

      imp.stream_dict = dict_str;
      imp.stream_data = val.stream.data;

      impl_->imported_objects.push_back(std::move(imp));
    } else {
      // Non-stream object
      PdfWriter::Impl::ImportedObject imp;
      imp.obj_id = new_id;
      imp.is_stream = false;
      imp.serialized_data = serialize_value(val);

      impl_->imported_objects.push_back(std::move(imp));
    }

    return new_id;
  }

 private:
  const Pdf& source_;
  PdfWriter::Impl* impl_;
  // Maps (source_obj_num << 16 | gen_num) -> new_obj_id
  std::map<uint64_t, int> obj_map_;
};

int PdfWriter::import_pages_from(const Pdf& source_pdf,
                                 const std::vector<int>& page_indices) {
  const auto& pages = source_pdf.catalog.pages;
  if (pages.empty()) {
    return 0;
  }

  // Determine which pages to import
  std::vector<int> indices;
  if (page_indices.empty()) {
    // Import all pages
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
      indices.push_back(i);
    }
  } else {
    indices = page_indices;
  }

  // Create a single ObjectCopier for the entire import operation to share
  // the object deduplication cache across all pages from the same source PDF.
  ObjectCopier copier(source_pdf, impl_);

  int imported_count = 0;

  for (int idx : indices) {
    if (idx < 0 || idx >= static_cast<int>(pages.size())) {
      continue;  // Skip invalid indices
    }

    const Page& src_page = pages[idx];

    // Get page dimensions from MediaBox
    double page_width = 612.0;   // Default Letter width
    double page_height = 792.0;  // Default Letter height
    if (src_page.media_box.size() >= 4) {
      page_width = src_page.media_box[2] - src_page.media_box[0];
      page_height = src_page.media_box[3] - src_page.media_box[1];
    }

    // Load the content stream data from the source page
    PageContent content = src_page.load_contents(source_pdf);
    if (!content.success) {
      continue;  // Skip pages we can't decode
    }

    // Build the content stream for the Form XObject
    std::string form_content(content.data.begin(), content.data.end());

    // Serialize the source page's resource dictionary with remapped references.
    // This recursively copies all referenced objects (fonts, images, color
    // spaces, graphics states, etc.) from the source PDF.
    std::string resources_str = "<<>>";
    if (!src_page.resources.empty()) {
      // Build a Value to serialize
      Value res_val;
      res_val.type = Value::DICTIONARY;
      res_val.dict = src_page.resources;
      resources_str = copier.serialize_value(res_val);
    }

    // Create a template (Form XObject) for this page
    impl_->template_counter++;
    std::string tmpl_name = "Fm" + std::to_string(impl_->template_counter);

    Impl::TemplateData tdata;
    tdata.name = tmpl_name;
    tdata.width = page_width;
    tdata.height = page_height;
    tdata.content = form_content;
    tdata.obj_id = impl_->allocate_obj();
    tdata.is_imported = true;
    tdata.imported_resources_str = resources_str;

    impl_->templates.push_back(tdata);

    // Create a page that draws this Form XObject
    PageSize ps;
    ps.width = page_width;
    ps.height = page_height;

    Impl::PageData page_data;
    page_data.size = ps;

    // The page content simply invokes the Form XObject.
    // Handle MediaBox origin offset if it's not (0,0).
    std::ostringstream page_content;
    if (src_page.media_box.size() >= 4 &&
        (src_page.media_box[0] != 0.0 || src_page.media_box[1] != 0.0)) {
      page_content << "q\n";
      page_content << "1 0 0 1 "
                   << format_number(-src_page.media_box[0]) << " "
                   << format_number(-src_page.media_box[1]) << " cm\n";
      page_content << "/" << tmpl_name << " Do\n";
      page_content << "Q\n";
    } else {
      page_content << "/" << tmpl_name << " Do\n";
    }

    page_data.content = page_content.str();
    page_data.used_templates.push_back(tmpl_name);

    impl_->pages.push_back(page_data);
    ++imported_count;
  }

  return imported_count;
}

int PdfWriter::add_raw_object(const std::string& serialized_data) {
  Impl::ImportedObject object;
  object.obj_id = impl_->allocate_obj();
  object.is_stream = false;
  object.serialized_data = serialized_data.empty() ? "null" : serialized_data;
  impl_->imported_objects.push_back(std::move(object));
  return impl_->imported_objects.back().obj_id;
}

int PdfWriter::add_raw_stream_object(const std::string& stream_dict,
                                     const std::vector<uint8_t>& stream_data) {
  Impl::ImportedObject object;
  object.obj_id = impl_->allocate_obj();
  object.is_stream = true;
  object.stream_dict = stream_dict;
  object.stream_data = stream_data;
  impl_->imported_objects.push_back(std::move(object));
  return impl_->imported_objects.back().obj_id;
}

WriteResult PdfWriter::split_pages(const Pdf& source_pdf,
                                   const std::vector<int>& page_indices,
                                   std::vector<uint8_t>& output) {
  WriteResult result;

  if (page_indices.empty()) {
    result.success = false;
    result.error = "No page indices specified for split";
    return result;
  }

  // Validate indices
  int total_pages = static_cast<int>(source_pdf.catalog.pages.size());
  for (int idx : page_indices) {
    if (idx < 0 || idx >= total_pages) {
      result.success = false;
      result.error = "Page index " + std::to_string(idx) + " out of range [0, " +
                     std::to_string(total_pages - 1) + "]";
      return result;
    }
  }

  PdfWriter writer;
  int imported = writer.import_pages_from(source_pdf, page_indices);
  if (imported <= 0) {
    result.success = false;
    result.error = "Failed to import any pages from source PDF";
    return result;
  }

  return writer.write_to_memory(output);
}

WriteResult PdfWriter::merge_pdfs(const std::vector<std::vector<uint8_t>>& pdf_data,
                                  std::vector<uint8_t>& output) {
  WriteResult result;

  if (pdf_data.empty()) {
    result.success = false;
    result.error = "No PDF data provided for merge";
    return result;
  }

  PdfWriter writer;
  int total_imported = 0;

  for (size_t i = 0; i < pdf_data.size(); ++i) {
    const auto& data = pdf_data[i];
    if (data.empty()) {
      continue;
    }

    // Parse the source PDF
    Pdf source_pdf;
    if (!parse_from_memory(data.data(), data.size(), &source_pdf)) {
      result.success = false;
      result.error = "Failed to parse PDF #" + std::to_string(i);
      return result;
    }

    // Import all pages from this PDF
    int imported = writer.import_pages_from(source_pdf);
    if (imported < 0) {
      result.success = false;
      result.error = "Failed to import pages from PDF #" + std::to_string(i);
      return result;
    }
    total_imported += imported;
  }

  if (total_imported == 0) {
    result.success = false;
    result.error = "No pages were imported from any source PDF";
    return result;
  }

  return writer.write_to_memory(output);
}

// ============================================================================
// Form Fill & Save Implementation
// ============================================================================

namespace {

// Find a form field's object number by name in a parsed PDF.
// Searches recursively through the AcroForm /Fields array.
bool find_field_obj_by_name(const Pdf& pdf, const std::string& target_name,
                            uint32_t* out_obj_num, uint16_t* out_gen) {
  auto acro_it = pdf.catalog.acro_form.find("Fields");
  if (acro_it == pdf.catalog.acro_form.end() ||
      acro_it->second.type != Value::ARRAY) {
    return false;
  }

  // Recursive search through field tree
  std::function<bool(const Value&, const std::string&)> search;
  search = [&](const Value& field_ref, const std::string& parent_name) -> bool {
    const Value* field_val = &field_ref;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    if (field_ref.type == Value::REFERENCE) {
      obj_num = field_ref.ref_object_number;
      gen_num = field_ref.ref_generation_number;
      ResolvedObject resolved = resolve_reference(pdf, obj_num, gen_num);
      if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
        return false;
      }
      field_val = &resolved.value;
    } else if (field_ref.type == Value::DICTIONARY) {
      // Inline - no object number
    } else {
      return false;
    }

    // Get partial name
    std::string partial_name;
    auto t_it = field_val->dict.find("T");
    if (t_it != field_val->dict.end() && t_it->second.type == Value::STRING) {
      partial_name = t_it->second.str;
    }

    // Build full name
    std::string full_name = parent_name.empty() ? partial_name
                          : (partial_name.empty() ? parent_name
                          : parent_name + "." + partial_name);

    // Check if this is our target
    if (full_name == target_name || partial_name == target_name) {
      *out_obj_num = obj_num;
      *out_gen = gen_num;
      return true;
    }

    // Search children (/Kids)
    auto kids_it = field_val->dict.find("Kids");
    if (kids_it != field_val->dict.end() && kids_it->second.type == Value::ARRAY) {
      for (const auto& kid : kids_it->second.array) {
        if (search(kid, full_name)) {
          return true;
        }
      }
    }

    return false;
  };

  for (const auto& field : acro_it->second.array) {
    if (search(field, "")) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool PdfWriter::set_field_value(const std::string& field_name,
                                const std::string& value) {
  if (!impl_->has_existing) return false;

  // Parse existing PDF to find the field
  Pdf pdf;
  if (!parse_from_memory(impl_->existing_pdf.data(),
                         impl_->existing_pdf.size(), &pdf)) {
    return false;
  }

  uint32_t obj_num = 0;
  uint16_t gen = 0;
  if (!find_field_obj_by_name(pdf, field_name, &obj_num, &gen)) {
    return false;
  }
  if (obj_num == 0) return false;

  // Record the update - PDF string value
  Impl::FieldUpdate update;
  update.field_name = field_name;
  update.value = "(" + escape_pdf_string(value) + ")";
  update.object_number = obj_num;
  update.generation = gen;
  impl_->field_updates.push_back(update);
  return true;
}

bool PdfWriter::set_field_checked(const std::string& field_name, bool checked) {
  if (!impl_->has_existing) return false;

  Pdf pdf;
  if (!parse_from_memory(impl_->existing_pdf.data(),
                         impl_->existing_pdf.size(), &pdf)) {
    return false;
  }

  uint32_t obj_num = 0;
  uint16_t gen = 0;
  if (!find_field_obj_by_name(pdf, field_name, &obj_num, &gen)) {
    return false;
  }
  if (obj_num == 0) return false;

  Impl::FieldUpdate update;
  update.field_name = field_name;
  update.value = checked ? "/Yes" : "/Off";
  update.object_number = obj_num;
  update.generation = gen;
  impl_->field_updates.push_back(update);
  return true;
}

bool PdfWriter::set_field_choice(const std::string& field_name,
                                 const std::string& value) {
  // Same as set_field_value - choice fields also use /V with a string
  return set_field_value(field_name, value);
}

WriteResult PdfWriter::write_to_file(const std::string& path) {
  std::vector<uint8_t> data;
  WriteResult result = write_to_memory(data);
  if (!result.success) {
    return result;
  }

  std::ofstream file(path, std::ios::binary);
  if (!file) {
    result.success = false;
    result.error = "Failed to open file for writing: " + path;
    return result;
  }

  file.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (!file) {
    result.success = false;
    result.error = "Failed to write to file: " + path;
    return result;
  }

  return result;
}

WriteResult PdfWriter::write_to_memory(std::vector<uint8_t>& output) {
  return write_for_signing(output, 0);  // 0 means no signature placeholder
}

WriteResult PdfWriter::write_for_signing(std::vector<uint8_t>& output,
                                         size_t content_length) {
  WriteResult result;
  impl_->output.str("");
  impl_->output.clear();
  impl_->obj_offsets.clear();
  impl_->obj_offsets.push_back(0);  // Object 0 is not used
  impl_->sig_placeholders.clear();
  impl_->sig_content_length = content_length;

  // Generate document IDs if not set
  if (impl_->doc_id1.empty()) {
    impl_->doc_id1 = generate_random_id();
  }
  if (impl_->doc_id2.empty()) {
    impl_->doc_id2 = impl_->doc_id1;  // Same as ID1 for new documents
  }

  // PDF Header with version
  impl_->output << "%PDF-" << get_version_string(impl_->version) << "\n";
  impl_->output << "%\xE2\xE3\xCF\xD3\n";  // Binary indicator

  // Allocate object IDs
  int catalog_id = impl_->allocate_obj();
  int pages_id = impl_->allocate_obj();
  int info_id = impl_->allocate_obj();

  // Allocate page object IDs and content stream IDs
  std::vector<int> page_obj_ids;
  std::vector<int> content_obj_ids;
  for (size_t i = 0; i < impl_->pages.size(); ++i) {
    page_obj_ids.push_back(impl_->allocate_obj());
    content_obj_ids.push_back(impl_->allocate_obj());
  }

  // Write SMask objects first (for images with alpha)
  for (auto& img_res : impl_->images) {
    if (img_res.has_alpha && img_res.smask_obj_id > 0 && !img_res.alpha_data.empty()) {
      impl_->write_obj_start(img_res.smask_obj_id);

      // Compress alpha channel data
      std::vector<uint8_t> alpha_compressed = compress_data(
          img_res.alpha_data.data(), img_res.alpha_data.size());

      int smask_width = img_res.is_ccitt_only ? img_res.ccitt_width : img_res.data.width;
      int smask_height = img_res.is_ccitt_only ? img_res.ccitt_height : img_res.data.height;

      impl_->output << "<<\n";
      impl_->output << "/Type /XObject\n";
      impl_->output << "/Subtype /Image\n";
      impl_->output << "/Width " << smask_width << "\n";
      impl_->output << "/Height " << smask_height << "\n";
      impl_->output << "/ColorSpace /DeviceGray\n";
      impl_->output << "/BitsPerComponent 8\n";
      impl_->output << "/Filter /FlateDecode\n";
      std::vector<uint8_t> encrypted_alpha_compressed =
          impl_->prepare_stream_data(img_res.smask_obj_id, alpha_compressed);
      impl_->output << "/Length " << encrypted_alpha_compressed.size() << "\n";
      impl_->output << ">>\n";
      impl_->output << "stream\n";
      impl_->output.write(
          reinterpret_cast<const char*>(encrypted_alpha_compressed.data()),
          encrypted_alpha_compressed.size());
      impl_->output << "\nendstream\n";
      impl_->write_obj_end();
    }
  }

  // Write image objects
  for (auto& img_res : impl_->images) {
    impl_->write_obj_start(img_res.obj_id);

    std::vector<uint8_t> stream_data;
    std::string filter;
    std::string color_space;
    int bits_per_component = 8;
    int img_width, img_height;
    std::string decode_parms;

    // Handle CCITT-only images (no ImageData)
    if (img_res.is_ccitt_only) {
      stream_data = img_res.ccitt_data;
      filter = "/CCITTFaxDecode";
      color_space = "/DeviceGray";
      bits_per_component = 1;
      img_width = img_res.ccitt_width;
      img_height = img_res.ccitt_height;
      // DecodeParms for Group 4: K=-1 means Group 4
      decode_parms = "/DecodeParms << /K -1 /Columns " +
                     std::to_string(img_width) + " >>";
    } else {
      const ImageData& img = img_res.data;
      img_width = img.width;
      img_height = img.height;

      // Determine compression based on setting
      ImageCompression comp = img_res.compression;

      // Handle CCITT compression
      if (comp == ImageCompression::CCITTFax && !img_res.ccitt_data.empty()) {
        stream_data = img_res.ccitt_data;
        filter = "/CCITTFaxDecode";
        color_space = "/DeviceGray";
        bits_per_component = 1;
        decode_parms = "/DecodeParms << /K -1 /Columns " +
                       std::to_string(img_width) + " >>";
      }
      // Handle Auto or DCT compression (JPEG passthrough)
      else if ((comp == ImageCompression::Auto || comp == ImageCompression::DCT) &&
               img.is_jpeg && !img.encoded_data.empty()) {
        stream_data = img.encoded_data;
        filter = "/DCTDecode";
        color_space = (img.channels == 1) ? "/DeviceGray" : "/DeviceRGB";
      }
      // Handle Flate compression (or fallback)
      else {
        std::vector<uint8_t> rgb_data;
        if (img.channels == 4) {
          // For images with alpha, extract RGB only (SMask handles alpha)
          if (img_res.has_alpha) {
            rgb_data = img.extract_rgb();
          } else {
            // RGBA -> RGB (discard alpha)
            rgb_data.reserve(img.width * img.height * 3);
            for (int i = 0; i < img.width * img.height; ++i) {
              rgb_data.push_back(img.raw_data[i * 4]);
              rgb_data.push_back(img.raw_data[i * 4 + 1]);
              rgb_data.push_back(img.raw_data[i * 4 + 2]);
            }
          }
          color_space = "/DeviceRGB";
        } else if (img.channels == 3) {
          rgb_data = img.raw_data;
          color_space = "/DeviceRGB";
        } else if (img.channels == 1) {
          rgb_data = img.raw_data;
          color_space = "/DeviceGray";
        } else {
          result.success = false;
          result.error = "Unsupported image channel count";
          return result;
        }

        stream_data = compress_data(rgb_data.data(), rgb_data.size());
        if (stream_data.empty()) {
          result.success = false;
          result.error = "Failed to compress image data";
          return result;
        }
        filter = "/FlateDecode";
      }
    }

    impl_->output << "<<\n";
    impl_->output << "/Type /XObject\n";
    impl_->output << "/Subtype /Image\n";
    impl_->output << "/Width " << img_width << "\n";
    impl_->output << "/Height " << img_height << "\n";
    impl_->output << "/ColorSpace " << color_space << "\n";
    impl_->output << "/BitsPerComponent " << bits_per_component << "\n";
    impl_->output << "/Filter " << filter << "\n";
    if (!decode_parms.empty()) {
      impl_->output << decode_parms << "\n";
    }
    // Add SMask reference if present
    if (img_res.has_alpha && img_res.smask_obj_id > 0) {
      impl_->output << "/SMask " << img_res.smask_obj_id << " 0 R\n";
    }
    std::vector<uint8_t> encrypted_image_stream =
        impl_->prepare_stream_data(img_res.obj_id, stream_data);
    impl_->output << "/Length " << encrypted_image_stream.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_image_stream.data()),
                        encrypted_image_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();
  }

  // Write font objects
  for (const auto& font_res : impl_->fonts) {
    impl_->write_obj_start(font_res.obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Font\n";
    impl_->output << "/Subtype /Type1\n";
    impl_->output << "/BaseFont /" << get_standard_font_name(font_res.font)
                  << "\n";
    impl_->output << "/Encoding /WinAnsiEncoding\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write embedded font objects
  for (auto& ef : impl_->embedded_fonts) {
    const auto& metrics = ef.font_data.metrics;

    // If subsetting mode but no chars marked, skip (font unused)
    // For full embedding, always include
    if (ef.embedding == FontEmbedding::Subset && ef.used_chars.empty()) {
      // Add basic ASCII chars as fallback
      for (uint32_t c = 32; c < 127; c++) {
        ef.used_chars.insert(c);
      }
    }

    // Build subset name
    std::string base_font_name = (ef.embedding == FontEmbedding::Subset)
                                     ? ef.subset_tag + metrics.font_name
                                     : metrics.font_name;

    // ----------------------------------------
    // 1. Write FontFile2 stream (TrueType data)
    // ----------------------------------------
    std::vector<uint8_t> font_stream_data = ef.font_data.data;
    std::vector<uint8_t> compressed_font =
        compress_data(font_stream_data.data(), font_stream_data.size());

    impl_->write_obj_start(ef.file_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Filter /FlateDecode\n";
    std::vector<uint8_t> encrypted_font_stream =
        impl_->prepare_stream_data(ef.file_obj_id, compressed_font);
    impl_->output << "/Length " << encrypted_font_stream.size() << "\n";
    impl_->output << "/Length1 " << font_stream_data.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_font_stream.data()),
                        encrypted_font_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();

    // ----------------------------------------
    // 2. Write ToUnicode CMap
    // ----------------------------------------
    std::string tounicode_cmap =
        generate_tounicode_cmap(metrics.font_name, ef.used_chars, metrics);
    std::vector<uint8_t> compressed_cmap =
        compress_data(reinterpret_cast<const uint8_t*>(tounicode_cmap.data()),
                      tounicode_cmap.size());

    impl_->write_obj_start(ef.tounicode_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Filter /FlateDecode\n";
    std::vector<uint8_t> encrypted_cmap_stream =
        impl_->prepare_stream_data(ef.tounicode_obj_id, compressed_cmap);
    impl_->output << "/Length " << encrypted_cmap_stream.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_cmap_stream.data()),
                        encrypted_cmap_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();

    // ----------------------------------------
    // 3. Write FontDescriptor
    // ----------------------------------------
    // Scale metrics to 1000 units
    double scale = 1000.0 / metrics.units_per_em;

    impl_->write_obj_start(ef.descriptor_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /FontDescriptor\n";
    impl_->output << "/FontName /" << base_font_name << "\n";
    if (!metrics.family_name.empty()) {
      impl_->output << "/FontFamily (" << metrics.family_name << ")\n";
    }
    impl_->output << "/Flags " << metrics.flags.to_int() << "\n";
    impl_->output << "/FontBBox [";
    if (metrics.bbox.size() >= 4) {
      impl_->output << static_cast<int>(metrics.bbox[0] * scale) << " "
                    << static_cast<int>(metrics.bbox[1] * scale) << " "
                    << static_cast<int>(metrics.bbox[2] * scale) << " "
                    << static_cast<int>(metrics.bbox[3] * scale);
    } else {
      impl_->output << "0 " << static_cast<int>(metrics.descender * scale) << " "
                    << "1000 " << static_cast<int>(metrics.ascender * scale);
    }
    impl_->output << "]\n";
    impl_->output << "/ItalicAngle " << metrics.italic_angle << "\n";
    impl_->output << "/Ascent " << static_cast<int>(metrics.ascender * scale) << "\n";
    impl_->output << "/Descent " << static_cast<int>(metrics.descender * scale) << "\n";
    impl_->output << "/CapHeight " << static_cast<int>(metrics.cap_height * scale) << "\n";
    impl_->output << "/XHeight " << static_cast<int>(metrics.x_height * scale) << "\n";
    impl_->output << "/StemV " << static_cast<int>(metrics.stem_v * scale) << "\n";
    impl_->output << "/FontFile2 " << ef.file_obj_id << " 0 R\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();

    // ----------------------------------------
    // 4. Write CIDFont (descendant)
    // ----------------------------------------
    // Build widths array - for CID fonts this is /W array
    // Format: [cid [width] cid [width] ...] or [start_cid width1 width2 ...]
    std::ostringstream widths_array;
    widths_array << "[";

    // Build list of used glyph IDs with their widths
    std::vector<std::pair<uint16_t, int>> glyph_widths;
    for (uint32_t cp : ef.used_chars) {
      auto it = metrics.char_to_glyph.find(cp);
      if (it != metrics.char_to_glyph.end()) {
        uint16_t gid = it->second;
        int width = (gid < metrics.glyph_widths.size())
                        ? metrics.glyph_widths[gid]
                        : 0;
        glyph_widths.push_back({gid, width});
      }
    }

    // Sort by glyph ID and output
    std::sort(glyph_widths.begin(), glyph_widths.end());
    for (const auto& gw : glyph_widths) {
      widths_array << gw.first << "[" << gw.second << "]";
    }
    widths_array << "]";

    impl_->write_obj_start(ef.descendant_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Font\n";
    impl_->output << "/Subtype /CIDFontType2\n";
    impl_->output << "/BaseFont /" << base_font_name << "\n";
    impl_->output << "/CIDSystemInfo <<\n";
    impl_->output << "  /Registry (Adobe)\n";
    impl_->output << "  /Ordering (Identity)\n";
    impl_->output << "  /Supplement 0\n";
    impl_->output << ">>\n";
    impl_->output << "/FontDescriptor " << ef.descriptor_obj_id << " 0 R\n";
    impl_->output << "/DW 1000\n";  // Default width
    impl_->output << "/W " << widths_array.str() << "\n";
    impl_->output << "/CIDToGIDMap /Identity\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();

    // ----------------------------------------
    // 5. Write Type0 font dictionary
    // ----------------------------------------
    impl_->write_obj_start(ef.font_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Font\n";
    impl_->output << "/Subtype /Type0\n";
    impl_->output << "/BaseFont /" << base_font_name << "\n";
    impl_->output << "/Encoding /Identity-H\n";
    impl_->output << "/DescendantFonts [" << ef.descendant_obj_id << " 0 R]\n";
    impl_->output << "/ToUnicode " << ef.tounicode_obj_id << " 0 R\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write content streams (with watermarks and headers/footers)
  int total_pages = static_cast<int>(impl_->pages.size());
  for (size_t i = 0; i < impl_->pages.size(); ++i) {
    auto& page = impl_->pages[i];  // Non-const to allow updating used resources

    // Build the final content stream with watermarks and headers/footers
    std::string final_content;
    std::set<std::string> additional_fonts;
    std::set<std::string> additional_extgstates;
    std::set<std::string> additional_images;

    // Get watermark for this page (page-specific overrides global)
    Impl::WatermarkData wm;
    auto wm_it = impl_->page_watermarks.find(static_cast<int>(i));
    if (wm_it != impl_->page_watermarks.end() && wm_it->second.active) {
      wm = wm_it->second;
    } else if (impl_->global_watermark.active) {
      wm = impl_->global_watermark;
    }

    // Determine if this is a background or foreground watermark
    bool is_background = wm.is_text ?
        wm.text_config.layer == WatermarkLayer::Background :
        wm.image_config.layer == WatermarkLayer::Background;

    // Add background watermark (before page content)
    if (wm.active && is_background) {
      std::string wm_content = impl_->generate_watermark_content(
          wm, page.size.width, page.size.height, additional_fonts, additional_extgstates, additional_images);
      final_content += wm_content;
    }

    // Add original page content
    final_content += page.content;

    // Add foreground watermark (after page content)
    if (wm.active && !is_background) {
      std::string wm_content = impl_->generate_watermark_content(
          wm, page.size.width, page.size.height, additional_fonts, additional_extgstates, additional_images);
      final_content += wm_content;
    }

    // Add headers/footers (always after page content)
    std::string hf_content = impl_->generate_header_footer_content(
        static_cast<int>(i), total_pages, page.size.width, page.size.height, additional_fonts);
    final_content += hf_content;

    // Add Bates stamping (always after page content)
    std::string bates_content = impl_->generate_bates_content(
        static_cast<int>(i), page.size.width, page.size.height, additional_fonts);
    final_content += bates_content;

    // Update page's used resources with additional fonts, extgstates, and images
    for (const auto& font : additional_fonts) {
      if (std::find(page.used_fonts.begin(), page.used_fonts.end(), font) == page.used_fonts.end()) {
        page.used_fonts.push_back(font);
      }
    }
    for (const auto& gs : additional_extgstates) {
      if (std::find(page.used_extgstates.begin(), page.used_extgstates.end(), gs) == page.used_extgstates.end()) {
        page.used_extgstates.push_back(gs);
      }
    }
    for (const auto& img : additional_images) {
      if (std::find(page.used_images.begin(), page.used_images.end(), img) == page.used_images.end()) {
        page.used_images.push_back(img);
      }
    }

    std::vector<uint8_t> compressed =
        compress_data(reinterpret_cast<const uint8_t*>(final_content.data()),
                      final_content.size());

    impl_->write_obj_start(content_obj_ids[i]);
    impl_->output << "<<\n";
    impl_->output << "/Filter /FlateDecode\n";
    std::vector<uint8_t> encrypted_content_stream =
        impl_->prepare_stream_data(content_obj_ids[i], compressed);
    impl_->output << "/Length " << encrypted_content_stream.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_content_stream.data()),
                        encrypted_content_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();
  }

  // Write page objects
  for (size_t i = 0; i < impl_->pages.size(); ++i) {
    const auto& page = impl_->pages[i];

    impl_->write_obj_start(page_obj_ids[i]);
    impl_->output << "<<\n";
    impl_->output << "/Type /Page\n";
    impl_->output << "/Parent " << pages_id << " 0 R\n";
    impl_->output << "/MediaBox [0 0 " << format_number(page.size.width) << " "
                  << format_number(page.size.height) << "]\n";

    // Resources
    impl_->output << "/Resources <<\n";

    auto has_custom_category = [&](const std::string& category) {
      return std::any_of(page.custom_resources.begin(),
                         page.custom_resources.end(),
                         [&](const Impl::CustomResourceRef& resource) {
                           return resource.category == category;
                         });
    };

    auto write_custom_category_entries = [&](const std::string& category) {
      for (const auto& resource : page.custom_resources) {
        if (resource.category != category) continue;
        impl_->output << "    /" << resource.name << " "
                      << resource.obj_id << " 0 R\n";
      }
    };

    // Image XObjects and Templates
    if (!page.used_images.empty() || !page.used_templates.empty() ||
        has_custom_category("XObject")) {
      impl_->output << "  /XObject <<\n";
      for (const auto& img_name : page.used_images) {
        std::string ref = impl_->get_image_resource(img_name);
        if (!ref.empty()) {
          impl_->output << "    /" << img_name << " " << ref << "\n";
        }
      }
      for (const auto& tmpl_name : page.used_templates) {
        for (const auto& tmpl : impl_->templates) {
          if (tmpl.name == tmpl_name) {
            impl_->output << "    /" << tmpl_name << " " << tmpl.obj_id << " 0 R\n";
            break;
          }
        }
      }
      write_custom_category_entries("XObject");
      impl_->output << "  >>\n";
    }

    // Fonts
    if (!page.used_fonts.empty() || has_custom_category("Font")) {
      impl_->output << "  /Font <<\n";
      for (const auto& font_name : page.used_fonts) {
        std::string ref = impl_->get_font_resource(font_name);
        if (!ref.empty()) {
          impl_->output << "    /" << font_name << " " << ref << "\n";
        }
      }
      write_custom_category_entries("Font");
      impl_->output << "  >>\n";
    }

    // ExtGState (for transparency)
    if (!page.used_extgstates.empty() || has_custom_category("ExtGState")) {
      impl_->output << "  /ExtGState <<\n";
      for (const auto& gs_name : page.used_extgstates) {
        for (const auto& gs : impl_->extgstates) {
          if (gs.name == gs_name) {
            impl_->output << "    /" << gs_name << " " << gs.obj_id << " 0 R\n";
            break;
          }
        }
      }
      write_custom_category_entries("ExtGState");
      impl_->output << "  >>\n";
    }

    // Patterns (for gradients)
    if (!page.used_patterns.empty() || has_custom_category("Pattern")) {
      impl_->output << "  /Pattern <<\n";
      for (const auto& pat_name : page.used_patterns) {
        for (const auto& grad : impl_->gradients) {
          if (grad.name == pat_name) {
            impl_->output << "    /" << pat_name << " " << grad.pattern_obj_id << " 0 R\n";
            break;
          }
        }
      }
      write_custom_category_entries("Pattern");
      impl_->output << "  >>\n";
    }

    // Properties (for layers/OCG)
    if (!page.used_layers.empty() || has_custom_category("Properties")) {
      impl_->output << "  /Properties <<\n";
      for (const auto& layer_name : page.used_layers) {
        for (const auto& layer : impl_->layers) {
          if (layer.internal_name == layer_name) {
            impl_->output << "    /" << layer_name << " " << layer.obj_id << " 0 R\n";
            break;
          }
        }
      }
      write_custom_category_entries("Properties");
      impl_->output << "  >>\n";
    }

    std::set<std::string> standard_categories = {
        "XObject", "Font", "ExtGState", "Pattern", "Properties"};
    std::set<std::string> extra_categories;
    for (const auto& resource : page.custom_resources) {
      if (!standard_categories.count(resource.category)) {
        extra_categories.insert(resource.category);
      }
    }
    for (const auto& category : extra_categories) {
      impl_->output << "  /" << category << " <<\n";
      write_custom_category_entries(category);
      impl_->output << "  >>\n";
    }

    impl_->output << ">>\n";

    impl_->output << "/Contents " << content_obj_ids[i] << " 0 R\n";

    // Add annotations (signature fields, links, highlights, etc.)
    bool has_annotations = !page.annotation_refs.empty() || !page.links.empty() ||
                           !page.highlights.empty();
    if (has_annotations) {
      impl_->output << "/Annots [";
      bool first = true;
      for (int ref : page.annotation_refs) {
        if (!first) impl_->output << " ";
        impl_->output << ref << " 0 R";
        first = false;
      }
      for (const auto& link : page.links) {
        if (!first) impl_->output << " ";
        impl_->output << link.obj_id << " 0 R";
        first = false;
      }
      for (const auto& hl : page.highlights) {
        if (!first) impl_->output << " ";
        impl_->output << hl.obj_id << " 0 R";
        first = false;
      }
      impl_->output << "]\n";
    }

    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write link annotation objects
  for (size_t i = 0; i < impl_->pages.size(); ++i) {
    const auto& page = impl_->pages[i];
    for (const auto& link : page.links) {
      impl_->write_obj_start(link.obj_id);
      impl_->output << "<<\n";
      impl_->output << "/Type /Annot\n";
      impl_->output << "/Subtype /Link\n";
      impl_->output << "/Rect [" << format_number(link.x) << " "
                    << format_number(link.y) << " "
                    << format_number(link.x + link.width) << " "
                    << format_number(link.y + link.height) << "]\n";

      if (!link.show_border) {
        impl_->output << "/Border [0 0 0]\n";
      }

      if (link.action == LinkAction::URI) {
        impl_->output << "/A << /Type /Action /S /URI /URI "
                      << impl_->format_pdf_string(link.uri) << " >>\n";
      } else if (link.action == LinkAction::GoTo) {
        // Reference to page object
        if (link.dest_page >= 0 && link.dest_page < static_cast<int>(page_obj_ids.size())) {
          impl_->output << "/Dest [" << page_obj_ids[link.dest_page] << " 0 R /XYZ 0 "
                        << format_number(link.dest_y) << " null]\n";
        }
      }

      impl_->output << ">>\n";
      impl_->write_obj_end();
    }
  }

  // Write highlight annotation objects
  for (size_t i = 0; i < impl_->pages.size(); ++i) {
    const auto& page = impl_->pages[i];
    for (const auto& hl : page.highlights) {
      impl_->write_obj_start(hl.obj_id);
      impl_->output << "<<\n";
      impl_->output << "/Type /Annot\n";

      // Subtype based on markup type
      const char* subtype = "/Highlight";
      switch (hl.type) {
        case MarkupType::Highlight:  subtype = "/Highlight"; break;
        case MarkupType::Underline:  subtype = "/Underline"; break;
        case MarkupType::Squiggly:   subtype = "/Squiggly"; break;
        case MarkupType::StrikeOut:  subtype = "/StrikeOut"; break;
      }
      impl_->output << "/Subtype " << subtype << "\n";

      // Calculate bounding rectangle from quads
      double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
      for (const auto& q : hl.quads) {
        min_x = std::min({min_x, q.x1, q.x2, q.x3, q.x4});
        min_y = std::min({min_y, q.y1, q.y2, q.y3, q.y4});
        max_x = std::max({max_x, q.x1, q.x2, q.x3, q.x4});
        max_y = std::max({max_y, q.y1, q.y2, q.y3, q.y4});
      }
      impl_->output << "/Rect [" << format_number(min_x) << " "
                    << format_number(min_y) << " "
                    << format_number(max_x) << " "
                    << format_number(max_y) << "]\n";

      // QuadPoints array (PDF spec: 4 points per quad)
      impl_->output << "/QuadPoints [";
      bool first_quad = true;
      for (const auto& q : hl.quads) {
        if (!first_quad) impl_->output << " ";
        // PDF QuadPoints order: x1,y1,x2,y2,x3,y3,x4,y4 (counterclockwise)
        impl_->output << format_number(q.x1) << " " << format_number(q.y1) << " "
                      << format_number(q.x2) << " " << format_number(q.y2) << " "
                      << format_number(q.x3) << " " << format_number(q.y3) << " "
                      << format_number(q.x4) << " " << format_number(q.y4);
        first_quad = false;
      }
      impl_->output << "]\n";

      // Color
      impl_->output << "/C [" << format_number(hl.r) << " "
                    << format_number(hl.g) << " "
                    << format_number(hl.b) << "]\n";

      // Transparency (CA for stroke alpha)
      if (hl.alpha < 1.0) {
        impl_->output << "/CA " << format_number(hl.alpha) << "\n";
      }

      // Author (T in PDF)
      if (!hl.author.empty()) {
        impl_->output << "/T " << impl_->format_pdf_string(hl.author) << "\n";
      }

      // Contents (comment text)
      if (!hl.contents.empty()) {
        impl_->output << "/Contents " << impl_->format_pdf_string(hl.contents) << "\n";
      }

      // Flags: bit 3 = NoZoom, bit 4 = NoRotate, bit 2 = Print
      int flags = 0;
      if (hl.print) flags |= 4;  // Print flag
      impl_->output << "/F " << flags << "\n";

      impl_->output << ">>\n";
      impl_->write_obj_end();
    }
  }

  // Write ExtGState objects (for transparency)
  for (const auto& gs : impl_->extgstates) {
    impl_->write_obj_start(gs.obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /ExtGState\n";
    if (gs.fill_alpha < 1.0) {
      impl_->output << "/ca " << format_number(gs.fill_alpha) << "\n";
    }
    if (gs.stroke_alpha < 1.0) {
      impl_->output << "/CA " << format_number(gs.stroke_alpha) << "\n";
    }
    if (gs.blend_mode != BlendMode::Normal) {
      impl_->output << "/BM /" << get_blend_mode_name(gs.blend_mode) << "\n";
    }
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write layer (OCG) objects
  for (const auto& layer : impl_->layers) {
    impl_->write_obj_start(layer.obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /OCG\n";
    impl_->output << "/Name " << impl_->format_pdf_string(layer.name) << "\n";
    impl_->output << "/Intent /View\n";
    impl_->output << "/Usage << /Print << /PrintState /"
                  << (layer.printable ? "ON" : "OFF") << " >> >>\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write bookmark/outline objects
  if (!impl_->outlines.empty()) {
    // Allocate outlines root object if not already done
    if (impl_->outlines_obj_id == 0) {
      impl_->outlines_obj_id = impl_->allocate_obj();
    }

    // Count top-level items
    int top_level_count = 0;
    int first_top = -1, last_top = -1;
    for (size_t i = 0; i < impl_->outlines.size(); ++i) {
      if (impl_->outlines[i].parent_id == -1) {
        if (first_top == -1) first_top = static_cast<int>(i);
        last_top = static_cast<int>(i);
        top_level_count++;
      }
    }

    // Write outlines root
    impl_->write_obj_start(impl_->outlines_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Outlines\n";
    if (first_top >= 0) {
      impl_->output << "/First " << impl_->outlines[first_top].obj_id << " 0 R\n";
      impl_->output << "/Last " << impl_->outlines[last_top].obj_id << " 0 R\n";
    }
    impl_->output << "/Count " << top_level_count << "\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();

    // Write each outline item
    for (size_t i = 0; i < impl_->outlines.size(); ++i) {
      const auto& item = impl_->outlines[i];
      impl_->write_obj_start(item.obj_id);
      impl_->output << "<<\n";
      impl_->output << "/Title " << impl_->format_pdf_string(item.title) << "\n";

      // Parent reference
      if (item.parent_id >= 0) {
        impl_->output << "/Parent " << impl_->outlines[item.parent_id].obj_id << " 0 R\n";
      } else {
        impl_->output << "/Parent " << impl_->outlines_obj_id << " 0 R\n";
      }

      // Sibling references
      if (item.prev_id >= 0) {
        impl_->output << "/Prev " << impl_->outlines[item.prev_id].obj_id << " 0 R\n";
      }
      if (item.next_id >= 0) {
        impl_->output << "/Next " << impl_->outlines[item.next_id].obj_id << " 0 R\n";
      }

      // Child references
      if (item.first_child_id >= 0) {
        impl_->output << "/First " << impl_->outlines[item.first_child_id].obj_id << " 0 R\n";
        impl_->output << "/Last " << impl_->outlines[item.last_child_id].obj_id << " 0 R\n";
        int count = item.child_count;
        if (!item.open) count = -count;
        impl_->output << "/Count " << count << "\n";
      }

      // Destination
      if (item.dest_page >= 0 && item.dest_page < static_cast<int>(page_obj_ids.size())) {
        impl_->output << "/Dest [" << page_obj_ids[item.dest_page] << " 0 R /XYZ 0 "
                      << format_number(item.dest_y) << " null]\n";
      }

      // Style flags
      int flags = 0;
      if (item.italic) flags |= 1;
      if (item.bold) flags |= 2;
      if (flags > 0) {
        impl_->output << "/F " << flags << "\n";
      }

      impl_->output << ">>\n";
      impl_->write_obj_end();
    }
  }

  // Write attachment objects
  for (auto& att : impl_->attachments) {
    // Write embedded file stream
    std::vector<uint8_t> file_data = att.data;
    if (att.compress && !file_data.empty()) {
      file_data = compress_data(att.data.data(), att.data.size());
    }

    impl_->write_obj_start(att.embedded_file_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /EmbeddedFile\n";
    if (!att.mime_type.empty()) {
      // The Subtype is a PDF name; MIME types contain '/' (e.g. text/plain)
      // which must be escaped as #2F or the name token breaks dict parsing.
      impl_->output << "/Subtype /" << escape_pdf_name(att.mime_type) << "\n";
    }
    impl_->output << "/Params << /Size " << att.data.size() << " >>\n";
    if (att.compress && !att.data.empty()) {
      impl_->output << "/Filter /FlateDecode\n";
    }
    std::vector<uint8_t> encrypted_attachment_stream =
        impl_->prepare_stream_data(att.embedded_file_obj_id, file_data);
    impl_->output << "/Length " << encrypted_attachment_stream.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_attachment_stream.data()),
                        encrypted_attachment_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();

    // Write filespec dictionary
    impl_->write_obj_start(att.filespec_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Filespec\n";
    impl_->output << "/F " << impl_->format_pdf_string(att.filename) << "\n";
    impl_->output << "/UF " << impl_->format_pdf_string(att.filename) << "\n";
    if (!att.description.empty()) {
      impl_->output << "/Desc " << impl_->format_pdf_string(att.description) << "\n";
    }
    impl_->output << "/EF << /F " << att.embedded_file_obj_id << " 0 R >>\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write gradient objects
  for (const auto& grad : impl_->gradients) {
    // Write function object (color interpolation)
    impl_->write_obj_start(grad.function_obj_id);
    if (grad.stops.size() == 2) {
      // Simple two-stop gradient - Type 2 exponential
      impl_->output << "<<\n";
      impl_->output << "/FunctionType 2\n";
      impl_->output << "/Domain [0 1]\n";
      impl_->output << "/C0 [" << format_number(grad.stops[0].r) << " "
                    << format_number(grad.stops[0].g) << " "
                    << format_number(grad.stops[0].b) << "]\n";
      impl_->output << "/C1 [" << format_number(grad.stops[1].r) << " "
                    << format_number(grad.stops[1].g) << " "
                    << format_number(grad.stops[1].b) << "]\n";
      impl_->output << "/N 1\n";
      impl_->output << ">>\n";
    } else {
      // Multi-stop gradient - Type 3 stitching function
      impl_->output << "<<\n";
      impl_->output << "/FunctionType 3\n";
      impl_->output << "/Domain [0 1]\n";
      impl_->output << "/Bounds [";
      for (size_t i = 1; i < grad.stops.size() - 1; ++i) {
        if (i > 1) impl_->output << " ";
        impl_->output << format_number(grad.stops[i].position);
      }
      impl_->output << "]\n";
      impl_->output << "/Encode [";
      for (size_t i = 0; i < grad.stops.size() - 1; ++i) {
        if (i > 0) impl_->output << " ";
        impl_->output << "0 1";
      }
      impl_->output << "]\n";
      impl_->output << "/Functions [\n";
      for (size_t i = 0; i < grad.stops.size() - 1; ++i) {
        impl_->output << "  << /FunctionType 2 /Domain [0 1] /C0 ["
                      << format_number(grad.stops[i].r) << " "
                      << format_number(grad.stops[i].g) << " "
                      << format_number(grad.stops[i].b) << "] /C1 ["
                      << format_number(grad.stops[i + 1].r) << " "
                      << format_number(grad.stops[i + 1].g) << " "
                      << format_number(grad.stops[i + 1].b) << "] /N 1 >>\n";
      }
      impl_->output << "]\n";
      impl_->output << ">>\n";
    }
    impl_->write_obj_end();

    // Write shading dictionary
    impl_->write_obj_start(grad.shading_obj_id);
    impl_->output << "<<\n";
    if (grad.type == GradientType::Linear) {
      impl_->output << "/ShadingType 2\n";  // Axial
      impl_->output << "/Coords [" << format_number(grad.x1) << " "
                    << format_number(grad.y1) << " "
                    << format_number(grad.x2) << " "
                    << format_number(grad.y2) << "]\n";
    } else {
      impl_->output << "/ShadingType 3\n";  // Radial
      impl_->output << "/Coords [" << format_number(grad.cx) << " "
                    << format_number(grad.cy) << " 0 "
                    << format_number(grad.cx) << " "
                    << format_number(grad.cy) << " "
                    << format_number(grad.r) << "]\n";
    }
    impl_->output << "/ColorSpace /DeviceRGB\n";
    impl_->output << "/Function " << grad.function_obj_id << " 0 R\n";
    impl_->output << "/Extend [" << (grad.extend_start ? "true" : "false") << " "
                  << (grad.extend_end ? "true" : "false") << "]\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();

    // Write pattern dictionary
    impl_->write_obj_start(grad.pattern_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Pattern\n";
    impl_->output << "/PatternType 2\n";  // Shading pattern
    impl_->output << "/Shading " << grad.shading_obj_id << " 0 R\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write imported PDF objects (for merge/split)
  for (const auto& imp_obj : impl_->imported_objects) {
    impl_->write_obj_start(imp_obj.obj_id);
    if (imp_obj.is_stream) {
      std::string stream_dict = imp_obj.stream_dict;
      std::vector<uint8_t> encrypted_imported_stream =
          impl_->prepare_stream_data(imp_obj.obj_id, imp_obj.stream_data);
      size_t length_pos = stream_dict.find("/Length ");
      if (length_pos != std::string::npos) {
        size_t length_value_start = length_pos + 8;
        size_t length_value_end = length_value_start;
        while (length_value_end < stream_dict.size() &&
               std::isdigit(static_cast<unsigned char>(stream_dict[length_value_end]))) {
          ++length_value_end;
        }
        stream_dict.replace(
            length_value_start,
            length_value_end - length_value_start,
            std::to_string(encrypted_imported_stream.size()));
      }
      impl_->output << stream_dict << "\n";
      impl_->output << "stream\n";
      impl_->output.write(
          reinterpret_cast<const char*>(encrypted_imported_stream.data()),
          encrypted_imported_stream.size());
      impl_->output << "\nendstream\n";
    } else {
      impl_->output << imp_obj.serialized_data << "\n";
    }
    impl_->write_obj_end();
  }

  // Write template (Form XObject) objects
  for (const auto& tmpl : impl_->templates) {
    std::vector<uint8_t> content_data(tmpl.content.begin(), tmpl.content.end());
    std::vector<uint8_t> compressed = compress_data(content_data.data(), content_data.size());

    impl_->write_obj_start(tmpl.obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /XObject\n";
    impl_->output << "/Subtype /Form\n";
    impl_->output << "/BBox [0 0 " << format_number(tmpl.width) << " "
                  << format_number(tmpl.height) << "]\n";
    impl_->output << "/Matrix [1 0 0 1 0 0]\n";

    if (tmpl.is_imported && !tmpl.imported_resources_str.empty()) {
      // For imported pages: use the pre-built resource dictionary
      impl_->output << "/Resources " << tmpl.imported_resources_str << "\n";
    } else {
      auto has_custom_category = [&](const std::string& category) {
        for (const auto& resource : tmpl.custom_resources) {
          if (resource.category == category) {
            return true;
          }
        }
        return false;
      };
      auto write_custom_category_entries = [&](const std::string& category) {
        for (const auto& resource : tmpl.custom_resources) {
          if (resource.category == category) {
            impl_->output << "    /" << resource.name << " "
                          << resource.obj_id << " 0 R\n";
          }
        }
      };

      impl_->output << "/Resources <<\n";
      if (!tmpl.used_fonts.empty() || has_custom_category("Font")) {
        impl_->output << "  /Font <<\n";
        for (const auto& font_name : tmpl.used_fonts) {
          std::string ref = impl_->get_font_resource(font_name);
          if (!ref.empty()) {
            impl_->output << "    /" << font_name << " " << ref << "\n";
          }
        }
        write_custom_category_entries("Font");
        impl_->output << "  >>\n";
      }
      if (!tmpl.used_images.empty() || !tmpl.used_templates.empty() ||
          has_custom_category("XObject")) {
        impl_->output << "  /XObject <<\n";
        for (const auto& img_name : tmpl.used_images) {
          std::string ref = impl_->get_image_resource(img_name);
          if (!ref.empty()) {
            impl_->output << "    /" << img_name << " " << ref << "\n";
          }
        }
        for (const auto& tmpl_name : tmpl.used_templates) {
          for (const auto& nested : impl_->templates) {
            if (nested.name == tmpl_name) {
              impl_->output << "    /" << tmpl_name << " "
                            << nested.obj_id << " 0 R\n";
              break;
            }
          }
        }
        write_custom_category_entries("XObject");
        impl_->output << "  >>\n";
      }
      if (!tmpl.used_extgstates.empty() || has_custom_category("ExtGState")) {
        impl_->output << "  /ExtGState <<\n";
        for (const auto& gs_name : tmpl.used_extgstates) {
          for (const auto& gs : impl_->extgstates) {
            if (gs.name == gs_name) {
              impl_->output << "    /" << gs_name << " " << gs.obj_id << " 0 R\n";
              break;
            }
          }
        }
        write_custom_category_entries("ExtGState");
        impl_->output << "  >>\n";
      }
      if (!tmpl.used_patterns.empty() || has_custom_category("Pattern")) {
        impl_->output << "  /Pattern <<\n";
        for (const auto& pat_name : tmpl.used_patterns) {
          for (const auto& grad : impl_->gradients) {
            if (grad.name == pat_name) {
              impl_->output << "    /" << pat_name << " "
                            << grad.pattern_obj_id << " 0 R\n";
              break;
            }
          }
        }
        write_custom_category_entries("Pattern");
        impl_->output << "  >>\n";
      }
      if (!tmpl.used_layers.empty() || has_custom_category("Properties")) {
        impl_->output << "  /Properties <<\n";
        for (const auto& layer_name : tmpl.used_layers) {
          for (const auto& layer : impl_->layers) {
            if (layer.internal_name == layer_name) {
              impl_->output << "    /" << layer_name << " "
                            << layer.obj_id << " 0 R\n";
              break;
            }
          }
        }
        write_custom_category_entries("Properties");
        impl_->output << "  >>\n";
      }

      std::set<std::string> standard_categories = {
          "XObject", "Font", "ExtGState", "Pattern", "Properties"};
      std::set<std::string> extra_categories;
      for (const auto& resource : tmpl.custom_resources) {
        if (!standard_categories.count(resource.category)) {
          extra_categories.insert(resource.category);
        }
      }
      for (const auto& category : extra_categories) {
        impl_->output << "  /" << category << " <<\n";
        write_custom_category_entries(category);
        impl_->output << "  >>\n";
      }

      impl_->output << ">>\n";
    }

    impl_->output << "/Filter /FlateDecode\n";
    std::vector<uint8_t> encrypted_template_stream =
        impl_->prepare_stream_data(tmpl.obj_id, compressed);
    impl_->output << "/Length " << encrypted_template_stream.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(reinterpret_cast<const char*>(encrypted_template_stream.data()),
                        encrypted_template_stream.size());
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();
  }

  // Write regular form field objects
  for (const auto& field : impl_->form_fields) {
    if (field.page < 0 || field.page >= static_cast<int>(page_obj_ids.size())) {
      continue;
    }

    auto write_widget_rect = [&](double x, double y, double width, double height) {
      impl_->output << "/Rect [" << format_number(x) << " "
                    << format_number(y) << " "
                    << format_number(x + width) << " "
                    << format_number(y + height) << "]\n";
    };

    auto write_string_array = [&](const std::vector<std::string>& values) {
      impl_->output << "[";
      for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) impl_->output << " ";
        impl_->output << impl_->format_pdf_string(values[i]);
      }
      impl_->output << "]";
    };

    auto write_default_appearance = [&](double font_size) {
      double effective_font_size = font_size > 0.0 ? font_size : 12.0;
      impl_->output << "/DA (/Helv " << format_number(effective_font_size)
                    << " Tf 0 g)\n";
    };

    switch (field.type) {
      case Impl::FormFieldData::Type::Text: {
        int flags = 0;
        if (field.read_only) flags |= 1;
        if (field.required) flags |= 2;
        if (field.multiline) flags |= 4096;
        if (field.password) flags |= 8192;

        impl_->write_obj_start(field.field_obj_id);
        impl_->output << "<<\n";
        impl_->output << "/Type /Annot\n";
        impl_->output << "/Subtype /Widget\n";
        impl_->output << "/FT /Tx\n";
        impl_->output << "/T " << impl_->format_pdf_string(field.name) << "\n";
        impl_->output << "/P " << page_obj_ids[field.page] << " 0 R\n";
        impl_->output << "/F 4\n";
        write_widget_rect(field.x, field.y, field.width, field.height);
        if (!field.value.empty()) {
          impl_->output << "/V " << impl_->format_pdf_string(field.value) << "\n";
          impl_->output << "/DV " << impl_->format_pdf_string(field.value) << "\n";
        }
        if (flags != 0) {
          impl_->output << "/Ff " << flags << "\n";
        }
        if (field.max_length > 0) {
          impl_->output << "/MaxLen " << field.max_length << "\n";
        }
        write_default_appearance(field.font_size);
        impl_->output << ">>\n";
        impl_->write_obj_end();
        break;
      }
      case Impl::FormFieldData::Type::Checkbox: {
        std::string export_name = field.export_value.empty() ? "Yes" : field.export_value;
        std::string state_name = field.checked ? export_name : "Off";

        impl_->write_obj_start(field.field_obj_id);
        impl_->output << "<<\n";
        impl_->output << "/Type /Annot\n";
        impl_->output << "/Subtype /Widget\n";
        impl_->output << "/FT /Btn\n";
        impl_->output << "/T " << impl_->format_pdf_string(field.name) << "\n";
        impl_->output << "/P " << page_obj_ids[field.page] << " 0 R\n";
        impl_->output << "/F 4\n";
        write_widget_rect(field.x, field.y, field.width, field.height);
        impl_->output << "/V /" << escape_pdf_name(state_name) << "\n";
        impl_->output << "/AS /" << escape_pdf_name(state_name) << "\n";
        impl_->output << ">>\n";
        impl_->write_obj_end();
        break;
      }
      case Impl::FormFieldData::Type::Radio: {
        std::string selected_value;

        if (field.selected >= 0 &&
            field.selected < static_cast<int>(field.options.size())) {
          size_t colon = field.options[field.selected].find(':');
          if (colon != std::string::npos) {
            selected_value = field.options[field.selected].substr(colon + 1);
          }
        }

        impl_->write_obj_start(field.field_obj_id);
        impl_->output << "<<\n";
        impl_->output << "/FT /Btn\n";
        impl_->output << "/Ff 32768\n";
        impl_->output << "/T " << impl_->format_pdf_string(field.name) << "\n";
        if (!selected_value.empty()) {
          impl_->output << "/V /" << escape_pdf_name(selected_value) << "\n";
        }
        impl_->output << "/Kids [";
        for (size_t i = 0; i < field.radio_widget_ids.size(); ++i) {
          if (i > 0) impl_->output << " ";
          impl_->output << field.radio_widget_ids[i] << " 0 R";
        }
        impl_->output << "]\n";
        impl_->output << ">>\n";
        impl_->write_obj_end();

        for (size_t i = 0; i < field.radio_widget_ids.size() && i < field.options.size(); ++i) {
          const std::string& encoded_option = field.options[i];
          size_t colon = encoded_option.find(':');
          std::string geometry = colon == std::string::npos
                                     ? encoded_option
                                     : encoded_option.substr(0, colon);
          std::string value = colon == std::string::npos
                                  ? encoded_option
                                  : encoded_option.substr(colon + 1);
          double x = 0.0;
          double y = 0.0;
          double size = 12.0;
          char separator1 = 0;
          char separator2 = 0;
          std::istringstream geometry_stream(geometry);
          geometry_stream >> x >> separator1 >> y >> separator2 >> size;

          impl_->write_obj_start(field.radio_widget_ids[i]);
          impl_->output << "<<\n";
          impl_->output << "/Type /Annot\n";
          impl_->output << "/Subtype /Widget\n";
          impl_->output << "/Parent " << field.field_obj_id << " 0 R\n";
          impl_->output << "/P " << page_obj_ids[field.page] << " 0 R\n";
          impl_->output << "/F 4\n";
          write_widget_rect(x, y, size, size);
          impl_->output << "/AS /"
                        << escape_pdf_name(
                               (static_cast<int>(i) == field.selected) ? value : "Off")
                        << "\n";
          impl_->output << ">>\n";
          impl_->write_obj_end();
        }
        break;
      }
      case Impl::FormFieldData::Type::Dropdown:
      case Impl::FormFieldData::Type::Listbox: {
        int flags = field.read_only ? 1 : 0;
        std::string selected_value;

        if (field.type == Impl::FormFieldData::Type::Dropdown) {
          flags |= 131072;
          if (field.editable) {
            flags |= 262144;
          }
        }
        if (field.selected >= 0 &&
            field.selected < static_cast<int>(field.options.size())) {
          selected_value = field.options[field.selected];
        }

        impl_->write_obj_start(field.field_obj_id);
        impl_->output << "<<\n";
        impl_->output << "/Type /Annot\n";
        impl_->output << "/Subtype /Widget\n";
        impl_->output << "/FT /Ch\n";
        impl_->output << "/T " << impl_->format_pdf_string(field.name) << "\n";
        impl_->output << "/P " << page_obj_ids[field.page] << " 0 R\n";
        impl_->output << "/F 4\n";
        write_widget_rect(field.x, field.y, field.width, field.height);
        impl_->output << "/Opt ";
        write_string_array(field.options);
        impl_->output << "\n";
        if (!selected_value.empty()) {
          impl_->output << "/V " << impl_->format_pdf_string(selected_value) << "\n";
          impl_->output << "/DV " << impl_->format_pdf_string(selected_value) << "\n";
        }
        if (flags != 0) {
          impl_->output << "/Ff " << flags << "\n";
        }
        write_default_appearance(field.font_size);
        impl_->output << ">>\n";
        impl_->write_obj_end();
        break;
      }
      case Impl::FormFieldData::Type::Button: {
        impl_->write_obj_start(field.field_obj_id);
        impl_->output << "<<\n";
        impl_->output << "/Type /Annot\n";
        impl_->output << "/Subtype /Widget\n";
        impl_->output << "/FT /Btn\n";
        impl_->output << "/Ff 65536\n";
        impl_->output << "/T " << impl_->format_pdf_string(field.name) << "\n";
        impl_->output << "/P " << page_obj_ids[field.page] << " 0 R\n";
        impl_->output << "/F 4\n";
        write_widget_rect(field.x, field.y, field.width, field.height);
        if (!field.caption.empty()) {
          impl_->output << "/MK << /CA " << impl_->format_pdf_string(field.caption)
                        << " >>\n";
        }
        write_default_appearance(field.font_size);
        impl_->output << ">>\n";
        impl_->write_obj_end();
        break;
      }
    }
  }

  int certification_sig_obj_id = 0;
  MdpPermissions certification_permissions = impl_->permissions;
  for (const auto& sig_field : impl_->signature_fields) {
    if (sig_field.config.is_certification) {
      certification_sig_obj_id = sig_field.sig_obj_id;
      certification_permissions = sig_field.config.mdp_permissions;
      break;
    }
  }
  if (certification_sig_obj_id == 0 &&
      impl_->has_permissions &&
      !impl_->signature_fields.empty()) {
    certification_sig_obj_id = impl_->signature_fields.front().sig_obj_id;
  }

  // Write signature field objects
  for (auto& sig_field : impl_->signature_fields) {
    const auto& config = sig_field.config;

    // Write signature value dictionary (placeholder)
    impl_->write_obj_start(sig_field.sig_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Sig\n";
    impl_->output << "/Filter /" << get_filter_name(config.filter) << "\n";
    impl_->output << "/SubFilter /" << get_subfilter_name(config.subfilter) << "\n";

    if (!config.reason.empty()) {
      impl_->output << "/Reason " << impl_->format_pdf_string(config.reason) << "\n";
    }
    if (!config.location.empty()) {
      impl_->output << "/Location " << impl_->format_pdf_string(config.location) << "\n";
    }
    if (!config.contact_info.empty()) {
      impl_->output << "/ContactInfo " << impl_->format_pdf_string(config.contact_info) << "\n";
    }
    impl_->output << "/M (" << get_pdf_timestamp() << ")\n";

    // ByteRange placeholder - will be filled later
    size_t byte_range_offset = impl_->output.tellp();
    impl_->output << "/ByteRange [0 0000000000 0000000000 0000000000]\n";

    // Contents placeholder - hex string of zeros
    impl_->output << "/Contents <";
    size_t contents_offset = impl_->output.tellp();
    size_t sig_size = (impl_->sig_content_length > 0) ? impl_->sig_content_length : 8192;
    for (size_t j = 0; j < sig_size; ++j) {
      impl_->output << "00";
    }
    impl_->output << ">\n";

    if (sig_field.sig_obj_id == certification_sig_obj_id) {
      impl_->output << "/Reference [<< /Type /SigRef /TransformMethod /DocMDP "
                    << "/TransformParams << /Type /TransformParams /P "
                    << static_cast<int>(certification_permissions)
                    << " /V /1.2 >> >>]\n";
    }

    impl_->output << ">>\n";
    impl_->write_obj_end();

    // Record placeholder info
    SignaturePlaceholder placeholder;
    placeholder.field_name = config.name;
    placeholder.contents_offset = contents_offset;
    placeholder.contents_length = sig_size;
    placeholder.byte_range_offset = byte_range_offset;
    impl_->sig_placeholders.push_back(placeholder);

    // Write signature field / widget annotation
    impl_->write_obj_start(sig_field.field_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Annot\n";
    impl_->output << "/Subtype /Widget\n";
    impl_->output << "/FT /Sig\n";
    impl_->output << "/T " << impl_->format_pdf_string(config.name) << "\n";
    impl_->output << "/V " << sig_field.sig_obj_id << " 0 R\n";
    impl_->output << "/F " << (config.visible ? 4 : 6) << "\n";  // Print flag (4) or Hidden+Print (6)
    impl_->output << "/P " << page_obj_ids[config.page] << " 0 R\n";
    impl_->output << "/Rect [" << format_number(config.x) << " "
                  << format_number(config.y) << " "
                  << format_number(config.x + config.width) << " "
                  << format_number(config.y + config.height) << "]\n";

    if (config.visible && sig_field.appearance_obj_id > 0) {
      impl_->output << "/AP << /N " << sig_field.appearance_obj_id << " 0 R >>\n";
    }

    impl_->output << ">>\n";
    impl_->write_obj_end();

    // Write appearance stream for visible signatures
    if (config.visible && sig_field.appearance_obj_id > 0) {
      std::string ap_content = "q 1 0 0 1 0 0 cm Q";  // Empty appearance
      std::vector<uint8_t> ap_compressed =
          compress_data(reinterpret_cast<const uint8_t*>(ap_content.data()),
                        ap_content.size());

      impl_->write_obj_start(sig_field.appearance_obj_id);
      impl_->output << "<<\n";
      impl_->output << "/Type /XObject\n";
      impl_->output << "/Subtype /Form\n";
      impl_->output << "/BBox [0 0 " << format_number(config.width) << " "
                    << format_number(config.height) << "]\n";
      impl_->output << "/Filter /FlateDecode\n";
      std::vector<uint8_t> encrypted_appearance_stream =
          impl_->prepare_stream_data(sig_field.appearance_obj_id, ap_compressed);
      impl_->output << "/Length " << encrypted_appearance_stream.size() << "\n";
      impl_->output << ">>\n";
      impl_->output << "stream\n";
      impl_->output.write(
          reinterpret_cast<const char*>(encrypted_appearance_stream.data()),
          encrypted_appearance_stream.size());
      impl_->output << "\nendstream\n";
      impl_->write_obj_end();
    }
  }

  // Write AcroForm object if present
  if (impl_->has_acroform &&
      (!impl_->signature_fields.empty() || !impl_->form_fields.empty())) {
    impl_->write_obj_start(impl_->acroform_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Fields [";
    bool first_field = true;
    for (const auto& field : impl_->form_fields) {
      if (field.page < 0 || field.page >= static_cast<int>(page_obj_ids.size())) {
        continue;
      }
      if (!first_field) impl_->output << " ";
      impl_->output << field.field_obj_id << " 0 R";
      first_field = false;
    }
    for (size_t i = 0; i < impl_->signature_fields.size(); ++i) {
      if (!first_field) impl_->output << " ";
      impl_->output << impl_->signature_fields[i].field_obj_id << " 0 R";
      first_field = false;
    }
    impl_->output << "]\n";
    impl_->output << "/NeedAppearances true\n";
    impl_->output << "/DA (/Helv 12 Tf 0 g)\n";
    impl_->output << "/DR << /Font << /Helv << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>\n";
    if (!impl_->signature_fields.empty()) {
      impl_->output << "/SigFlags 3\n";  // SignaturesExist + AppendOnly
    }
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write Pages object
  impl_->write_obj_start(pages_id);
  impl_->output << "<<\n";
  impl_->output << "/Type /Pages\n";
  impl_->output << "/Kids [";
  for (size_t i = 0; i < page_obj_ids.size(); ++i) {
    if (i > 0) impl_->output << " ";
    impl_->output << page_obj_ids[i] << " 0 R";
  }
  impl_->output << "]\n";
  impl_->output << "/Count " << impl_->pages.size() << "\n";
  impl_->output << ">>\n";
  impl_->write_obj_end();

  // Write PageLabels number tree
  int page_labels_obj_id = 0;
  if (!impl_->page_labels.labels.empty()) {
    auto label_style_name = [](PageLabelStyle style) -> const char* {
      switch (style) {
        case PageLabelStyle::DecimalArabic:
          return "D";
        case PageLabelStyle::UppercaseRoman:
          return "R";
        case PageLabelStyle::LowercaseRoman:
          return "r";
        case PageLabelStyle::UppercaseLetters:
          return "A";
        case PageLabelStyle::LowercaseLetters:
          return "a";
        case PageLabelStyle::None:
          return nullptr;
      }
      return nullptr;
    };

    page_labels_obj_id = impl_->allocate_obj();
    impl_->write_obj_start(page_labels_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Nums [\n";
    for (const auto& entry : impl_->page_labels.labels) {
      const PageLabel& label = entry.second;
      const char* style_name = label_style_name(label.style);
      impl_->output << "  " << entry.first << " <<\n";
      if (style_name) {
        impl_->output << "    /S /" << style_name << "\n";
      }
      if (!label.prefix.empty()) {
        impl_->output << "    /P "
                      << impl_->format_pdf_string(label.prefix) << "\n";
      }
      if (label.start_value > 1) {
        impl_->output << "    /St " << label.start_value << "\n";
      }
      impl_->output << "  >>\n";
    }
    impl_->output << "]\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write XMP metadata stream
  int metadata_obj_id = 0;
  if (!impl_->xmp_metadata_xml.empty()) {
    metadata_obj_id = impl_->allocate_obj();
    impl_->write_obj_start(metadata_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /Metadata\n";
    impl_->output << "/Subtype /XML\n";
    impl_->output << "/Length " << impl_->xmp_metadata_xml.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    impl_->output.write(
        impl_->xmp_metadata_xml.data(),
        static_cast<std::streamsize>(impl_->xmp_metadata_xml.size()));
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();
  }

  // Write OutputIntents and ICC profile streams
  std::vector<int> output_intent_obj_ids;
  for (const auto& output_intent : impl_->output_intents) {
    int profile_obj_id = impl_->allocate_obj();
    int output_intent_obj_id = impl_->allocate_obj();

    impl_->write_obj_start(profile_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/N " << output_intent.color_components << "\n";
    impl_->output << "/Length " << output_intent.dest_output_profile.size() << "\n";
    impl_->output << ">>\n";
    impl_->output << "stream\n";
    if (!output_intent.dest_output_profile.empty()) {
      impl_->output.write(
          reinterpret_cast<const char*>(output_intent.dest_output_profile.data()),
          static_cast<std::streamsize>(output_intent.dest_output_profile.size()));
    }
    impl_->output << "\nendstream\n";
    impl_->write_obj_end();

    impl_->write_obj_start(output_intent_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Type /OutputIntent\n";
    impl_->output << "/S /" << output_intent.subtype << "\n";
    if (!output_intent.output_condition.empty()) {
      impl_->output << "/OutputCondition "
                    << impl_->format_pdf_string(output_intent.output_condition)
                    << "\n";
    }
    if (!output_intent.output_condition_id.empty()) {
      impl_->output << "/OutputConditionIdentifier "
                    << impl_->format_pdf_string(output_intent.output_condition_id)
                    << "\n";
    }
    if (!output_intent.registry_name.empty()) {
      impl_->output << "/RegistryName "
                    << impl_->format_pdf_string(output_intent.registry_name)
                    << "\n";
    }
    if (!output_intent.info.empty()) {
      impl_->output << "/Info "
                    << impl_->format_pdf_string(output_intent.info)
                    << "\n";
    }
    impl_->output << "/DestOutputProfile " << profile_obj_id << " 0 R\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
    output_intent_obj_ids.push_back(output_intent_obj_id);
  }

  // Write Names dictionary (for attachments and named destinations)
  int names_obj_id = 0;
  if (!impl_->attachments.empty() || !impl_->named_destinations.empty()) {
    names_obj_id = impl_->allocate_obj();
    impl_->write_obj_start(names_obj_id);
    impl_->output << "<<\n";
    if (!impl_->attachments.empty()) {
      impl_->output << "/EmbeddedFiles <<\n";
      impl_->output << "  /Names [\n";
      for (const auto& att : impl_->attachments) {
        impl_->output << "    " << impl_->format_pdf_string(att.filename) << " "
                      << att.filespec_obj_id << " 0 R\n";
      }
      impl_->output << "  ]\n";
      impl_->output << ">>\n";
    }
    if (!impl_->named_destinations.empty()) {
      impl_->output << "/Dests <<\n";
      impl_->output << "  /Names [\n";
      for (const auto& entry : impl_->named_destinations) {
        const NamedDestination& dest = entry.second;
        std::string fit_type = dest.fit_type.empty() ? "Fit" : dest.fit_type;

        if (dest.page_number >= page_obj_ids.size()) {
          result.error = "Named destination page index out of range";
          return result;
        }

        impl_->output << "    " << impl_->format_pdf_string(entry.first) << " ["
                      << page_obj_ids[dest.page_number] << " 0 R /" << fit_type;
        for (double value : dest.position) {
          impl_->output << " " << format_number(value);
        }
        impl_->output << "]\n";
      }
      impl_->output << "  ]\n";
      impl_->output << ">>\n";
    }
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write OCProperties (for layers)
  int ocproperties_obj_id = 0;
  if (!impl_->layers.empty()) {
    ocproperties_obj_id = impl_->allocate_obj();
    impl_->write_obj_start(ocproperties_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/OCGs [";
    for (size_t i = 0; i < impl_->layers.size(); ++i) {
      if (i > 0) impl_->output << " ";
      impl_->output << impl_->layers[i].obj_id << " 0 R";
    }
    impl_->output << "]\n";

    // Default configuration
    impl_->output << "/D <<\n";
    impl_->output << "  /Name (Default)\n";
    impl_->output << "  /BaseState /ON\n";

    // List layers that are off by default
    bool has_off = false;
    bool has_locked = false;
    for (const auto& layer : impl_->layers) {
      if (!layer.visible) {
        if (!has_off) {
          impl_->output << "  /OFF [";
          has_off = true;
        } else {
          impl_->output << " ";
        }
        impl_->output << layer.obj_id << " 0 R";
      }
      if (layer.locked) {
        if (!has_locked) {
          impl_->output << "  /Locked [";
          has_locked = true;
        } else {
          impl_->output << " ";
        }
        impl_->output << layer.obj_id << " 0 R";
      }
    }
    if (has_off) impl_->output << "]\n";
    if (has_locked) impl_->output << "]\n";

    impl_->output << ">>\n";
    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write Catalog object
  impl_->write_obj_start(catalog_id);
  impl_->output << "<<\n";
  impl_->output << "/Type /Catalog\n";
  impl_->output << "/Pages " << pages_id << " 0 R\n";
  if (impl_->page_layout != PageLayout::Unset) {
    const char* layout_name = nullptr;
    switch (impl_->page_layout) {
      case PageLayout::SinglePage:
        layout_name = "SinglePage";
        break;
      case PageLayout::OneColumn:
        layout_name = "OneColumn";
        break;
      case PageLayout::TwoColumnLeft:
        layout_name = "TwoColumnLeft";
        break;
      case PageLayout::TwoColumnRight:
        layout_name = "TwoColumnRight";
        break;
      case PageLayout::TwoPageLeft:
        layout_name = "TwoPageLeft";
        break;
      case PageLayout::TwoPageRight:
        layout_name = "TwoPageRight";
        break;
      case PageLayout::Unset:
        break;
    }
    if (layout_name) {
      impl_->output << "/PageLayout /" << layout_name << "\n";
    }
  }
  if (impl_->page_mode != PageMode::Unset) {
    const char* mode_name = nullptr;
    switch (impl_->page_mode) {
      case PageMode::UseNone:
        mode_name = "UseNone";
        break;
      case PageMode::UseOutlines:
        mode_name = "UseOutlines";
        break;
      case PageMode::UseThumbs:
        mode_name = "UseThumbs";
        break;
      case PageMode::FullScreen:
        mode_name = "FullScreen";
        break;
      case PageMode::UseOC:
        mode_name = "UseOC";
        break;
      case PageMode::UseAttachments:
        mode_name = "UseAttachments";
        break;
      case PageMode::Unset:
        break;
    }
    if (mode_name) {
      impl_->output << "/PageMode /" << mode_name << "\n";
    }
  }
  if (!impl_->language.empty()) {
    impl_->output << "/Lang " << impl_->format_pdf_string(impl_->language)
                  << "\n";
  }
  if (impl_->has_mark_info) {
    impl_->output << "/MarkInfo <<\n";
    impl_->output << "  /Marked "
                  << (impl_->mark_info.marked ? "true" : "false") << "\n";
    impl_->output << "  /Suspects "
                  << (impl_->mark_info.suspects ? "true" : "false") << "\n";
    impl_->output << ">>\n";
  }
  if (impl_->has_viewer_preferences) {
    const ViewerPreferences& prefs = impl_->viewer_preferences;
    impl_->output << "/ViewerPreferences <<\n";
    if (prefs.hide_toolbar) {
      impl_->output << "  /HideToolbar true\n";
    }
    if (prefs.hide_menubar) {
      impl_->output << "  /HideMenubar true\n";
    }
    if (prefs.hide_window_ui) {
      impl_->output << "  /HideWindowUI true\n";
    }
    if (prefs.fit_window) {
      impl_->output << "  /FitWindow true\n";
    }
    if (prefs.center_window) {
      impl_->output << "  /CenterWindow true\n";
    }
    if (prefs.display_doc_title) {
      impl_->output << "  /DisplayDocTitle true\n";
    }
    impl_->output << ">>\n";
  }

  // AcroForm (for signatures and form fields)
  bool has_form_content = impl_->has_acroform &&
      (!impl_->signature_fields.empty() || !impl_->form_fields.empty());
  if (has_form_content) {
    impl_->output << "/AcroForm " << impl_->acroform_obj_id << " 0 R\n";
  }
  if (certification_sig_obj_id > 0) {
    impl_->output << "/Perms << /DocMDP " << certification_sig_obj_id << " 0 R >>\n";
  }

  // Outlines/Bookmarks
  if (!impl_->outlines.empty() && impl_->outlines_obj_id > 0) {
    impl_->output << "/Outlines " << impl_->outlines_obj_id << " 0 R\n";
  }

  // Names dictionary (for attachments)
  if (names_obj_id > 0) {
    impl_->output << "/Names " << names_obj_id << " 0 R\n";
  }

  // OCProperties (for layers)
  if (ocproperties_obj_id > 0) {
    impl_->output << "/OCProperties " << ocproperties_obj_id << " 0 R\n";
  }
  if (page_labels_obj_id > 0) {
    impl_->output << "/PageLabels " << page_labels_obj_id << " 0 R\n";
  }
  if (metadata_obj_id > 0) {
    impl_->output << "/Metadata " << metadata_obj_id << " 0 R\n";
  }
  if (!output_intent_obj_ids.empty()) {
    impl_->output << "/OutputIntents [";
    for (size_t i = 0; i < output_intent_obj_ids.size(); ++i) {
      if (i > 0) {
        impl_->output << " ";
      }
      impl_->output << output_intent_obj_ids[i] << " 0 R";
    }
    impl_->output << "]\n";
  }
  if (!impl_->open_action_destination_name.empty()) {
    if (impl_->named_destinations.find(impl_->open_action_destination_name) ==
        impl_->named_destinations.end()) {
      result.error = "OpenAction named destination was not found";
      return result;
    }
    impl_->output << "/OpenAction "
                  << impl_->format_pdf_string(
                         impl_->open_action_destination_name)
                  << "\n";
  }

  impl_->output << ">>\n";
  impl_->write_obj_end();

  // Write Info object
  impl_->write_obj_start(info_id);
  impl_->output << "<<\n";
  if (!impl_->title.empty()) {
    impl_->output << "/Title " << impl_->format_pdf_string(impl_->title) << "\n";
  }
  if (!impl_->author.empty()) {
    impl_->output << "/Author " << impl_->format_pdf_string(impl_->author) << "\n";
  }
  if (!impl_->subject.empty()) {
    impl_->output << "/Subject " << impl_->format_pdf_string(impl_->subject) << "\n";
  }
  if (!impl_->keywords.empty()) {
    impl_->output << "/Keywords " << impl_->format_pdf_string(impl_->keywords)
                  << "\n";
  }
  impl_->output << "/Creator " << impl_->format_pdf_string(impl_->creator) << "\n";
  impl_->output << "/Producer " << impl_->format_pdf_string(impl_->producer) << "\n";
  for (const auto& entry : impl_->custom_info) {
    if (entry.first.empty()) {
      continue;
    }
    if (entry.first == "Title" || entry.first == "Author" ||
        entry.first == "Subject" || entry.first == "Keywords" ||
        entry.first == "Creator" || entry.first == "Producer" ||
        entry.first == "CreationDate" || entry.first == "ModDate" ||
        entry.first == "Trapped") {
      continue;
    }
    impl_->output << "/" << entry.first << " "
                  << impl_->format_pdf_string(entry.second) << "\n";
  }
  if (impl_->trapped != TrappedState::Unset) {
    const char* trapped_name = "Unknown";
    switch (impl_->trapped) {
      case TrappedState::False:
        trapped_name = "False";
        break;
      case TrappedState::True:
        trapped_name = "True";
        break;
      case TrappedState::Unknown:
        trapped_name = "Unknown";
        break;
      case TrappedState::Unset:
        break;
    }
    impl_->output << "/Trapped /" << trapped_name << "\n";
  }
  std::string creation_date = impl_->creation_date.empty() ?
      get_pdf_timestamp() : impl_->creation_date;
  std::string mod_date = impl_->modification_date.empty() ?
      get_pdf_timestamp() : impl_->modification_date;
  impl_->output << "/CreationDate " << impl_->format_pdf_string(creation_date) << "\n";
  impl_->output << "/ModDate " << impl_->format_pdf_string(mod_date) << "\n";
  impl_->output << ">>\n";
  impl_->write_obj_end();

  // Write encryption dictionary if encryption is enabled
  if (impl_->encryption_enabled) {
    impl_->write_obj_start(impl_->encrypt_obj_id);
    impl_->output << "<<\n";
    impl_->output << "/Filter /Standard\n";

    EncryptionAlgorithm algo = impl_->encryption_config.algorithm;
    if (algo == EncryptionAlgorithm::AES_256) {
      // AES-256 (revision 5, AESV3 crypt filters)
      impl_->output << "/V 5\n";
      impl_->output << "/R 5\n";
      impl_->output << "/Length 256\n";
      impl_->output << "/CF << /StdCF << /CFM /AESV3 /AuthEvent /DocOpen /Length 32 >> >>\n";
      impl_->output << "/StmF /StdCF\n";
      impl_->output << "/StrF /StdCF\n";

      // O value (48 bytes)
      impl_->output << "/O <" << bytes_to_hex(impl_->o_value) << ">\n";
      // U value (48 bytes)
      impl_->output << "/U <" << bytes_to_hex(impl_->u_value) << ">\n";
      // OE value (32 bytes)
      impl_->output << "/OE <" << bytes_to_hex(impl_->oe_value) << ">\n";
      // UE value (32 bytes)
      impl_->output << "/UE <" << bytes_to_hex(impl_->ue_value) << ">\n";
      // Perms value (16 bytes)
      impl_->output << "/Perms <" << bytes_to_hex(impl_->perms_value) << ">\n";

    } else if (algo == EncryptionAlgorithm::AES_128) {
      // AES-128 (PDF 1.6+)
      impl_->output << "/V 4\n";
      impl_->output << "/R 4\n";
      impl_->output << "/Length 128\n";
      impl_->output << "/CF << /StdCF << /CFM /AESV2 /AuthEvent /DocOpen /Length 16 >> >>\n";
      impl_->output << "/StmF /StdCF\n";
      impl_->output << "/StrF /StdCF\n";

      impl_->output << "/O <" << bytes_to_hex(impl_->o_value) << ">\n";
      impl_->output << "/U <" << bytes_to_hex(impl_->u_value) << ">\n";

    } else if (algo == EncryptionAlgorithm::RC4_128) {
      // RC4 128-bit (PDF 1.4+)
      impl_->output << "/V 2\n";
      impl_->output << "/R 3\n";
      impl_->output << "/Length 128\n";

      impl_->output << "/O <" << bytes_to_hex(impl_->o_value) << ">\n";
      impl_->output << "/U <" << bytes_to_hex(impl_->u_value) << ">\n";

    } else {
      // RC4 40-bit (PDF 1.1+)
      impl_->output << "/V 1\n";
      impl_->output << "/R 2\n";

      impl_->output << "/O <" << bytes_to_hex(impl_->o_value) << ">\n";
      impl_->output << "/U <" << bytes_to_hex(impl_->u_value) << ">\n";
    }

    // Permission flags
    int32_t perm_flags = impl_->encryption_config.permissions.to_flags();
    impl_->output << "/P " << perm_flags << "\n";

    // Encrypt metadata flag (for revision 4+)
    if (algo == EncryptionAlgorithm::AES_128 || algo == EncryptionAlgorithm::AES_256) {
      impl_->output << "/EncryptMetadata "
                    << (impl_->encryption_config.encrypt_metadata ? "true" : "false") << "\n";
    }

    impl_->output << ">>\n";
    impl_->write_obj_end();
  }

  // Write xref table
  size_t xref_offset = impl_->output.tellp();
  impl_->output << "xref\n";
  impl_->output << "0 " << impl_->obj_offsets.size() << "\n";

  // Object 0 (free list head)
  impl_->output << "0000000000 65535 f \n";

  // All other objects
  for (size_t i = 1; i < impl_->obj_offsets.size(); ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%010zu 00000 n \n", impl_->obj_offsets[i]);
    impl_->output << buf;
  }

  // Write trailer
  impl_->output << "trailer\n";
  impl_->output << "<<\n";
  impl_->output << "/Size " << impl_->obj_offsets.size() << "\n";
  impl_->output << "/Root " << catalog_id << " 0 R\n";
  impl_->output << "/Info " << info_id << " 0 R\n";
  // Encryption dictionary reference
  if (impl_->encryption_enabled) {
    impl_->output << "/Encrypt " << impl_->encrypt_obj_id << " 0 R\n";
  }
  // Document ID (required for signatures and encryption)
  impl_->output << "/ID [<" << bytes_to_hex(impl_->doc_id1) << "> <"
                << bytes_to_hex(impl_->doc_id2) << ">]\n";
  impl_->output << ">>\n";
  impl_->output << "startxref\n";
  impl_->output << xref_offset << "\n";
  impl_->output << "%%EOF\n";

  // Copy to output buffer
  std::string str = impl_->output.str();
  output.assign(str.begin(), str.end());

  // Update signature ByteRange values
  for (auto& placeholder : impl_->sig_placeholders) {
    // ByteRange: [0, contents_offset-1, contents_offset+contents_length*2+1, remaining]
    size_t part1_start = 0;
    size_t part1_len = placeholder.contents_offset - 1;  // Before '<'
    size_t part2_start = placeholder.contents_offset + placeholder.contents_length * 2 + 1;  // After '>'
    size_t part2_len = output.size() - part2_start;

    placeholder.byte_range = {part1_start, part1_len, part2_start, part2_len};
  }

  result.success = true;
  result.bytes_written = output.size();
  return result;
}

bool PdfWriter::add_text_annotation_to_existing_page(int page_index,
                                                       double x, double y,
                                                       double w, double h,
                                                       const std::string& contents) {
  if (!impl_->has_existing) return false;
  Impl::AnnotationUpdate update;
  update.update_type = Impl::AnnotationUpdateType::AddText;
  update.page_index = page_index;
  update.x = x;
  update.y = y;
  update.w = w;
  update.h = h;
  update.contents = contents;
  impl_->annotation_updates.push_back(update);
  return true;
}

bool PdfWriter::add_highlight_to_existing_page(int page_index,
                                                 const HighlightConfig& config) {
  if (!impl_->has_existing) return false;
  Impl::AnnotationUpdate update;
  update.update_type = Impl::AnnotationUpdateType::AddHighlight;
  update.page_index = page_index;
  update.highlight_config = config;
  impl_->annotation_updates.push_back(update);
  return true;
}

bool PdfWriter::add_text_markup_to_existing_page(int page_index,
                                                  const TextMarkupConfig& config) {
  if (!impl_->has_existing) return false;
  Impl::AnnotationUpdate update;
  update.update_type = Impl::AnnotationUpdateType::AddTextMarkup;
  update.page_index = page_index;
  update.text_markup_config = config;
  impl_->annotation_updates.push_back(update);
  return true;
}

bool PdfWriter::add_link_to_existing_page(int page_index,
                                            const LinkConfig& config) {
  if (!impl_->has_existing) return false;
  Impl::AnnotationUpdate update;
  update.update_type = Impl::AnnotationUpdateType::AddLink;
  update.page_index = page_index;
  update.link_config = config;
  impl_->annotation_updates.push_back(update);
  return true;
}

bool PdfWriter::delete_annotation_from_existing_page(int page_index,
                                                       int annot_index) {
  if (!impl_->has_existing) return false;
  Impl::AnnotationUpdate update;
  update.update_type = Impl::AnnotationUpdateType::Delete;
  update.page_index = page_index;
  update.annot_index = annot_index;
  impl_->annotation_updates.push_back(update);
  return true;
}

WriteResult PdfWriter::apply_redactions(const Pdf& source_pdf,
                                         std::vector<uint8_t>& output) {
  WriteResult result;

  // Check for redactions
  bool has_redactions = false;
  for (const auto& page : source_pdf.catalog.pages) {
    if (!page.redaction_annotations.empty()) {
      has_redactions = true;
      break;
    }
  }

  if (!has_redactions) {
    result.success = false;
    result.error = "No redaction annotations found in the PDF";
    return result;
  }

  // Build page indices for import - import all pages
  // For pages WITHOUT redactions, import them directly.
  // For pages WITH redactions, import them as Form XObjects and
  // draw redaction overlays on top.

  PdfWriter writer;
  writer.set_creator("nanopdf Redaction");
  std::string std_font = writer.add_standard_font(StandardFont::Helvetica);
  (void)std_font;

  for (size_t pi = 0; pi < source_pdf.catalog.pages.size(); ++pi) {
    const auto& page = source_pdf.catalog.pages[pi];

    if (page.redaction_annotations.empty()) {
      // No redactions - just import directly
      writer.import_pages_from(source_pdf, {static_cast<int>(pi)});
    } else {
      // Has redactions - import page then add overlay highlights

      // Import the page into writer
      writer.import_pages_from(source_pdf, {static_cast<int>(pi)});

      // Add redaction overlays using highlight annotations with full opacity
      for (const auto& redact : page.redaction_annotations) {
        HighlightConfig hc;
        hc.page = static_cast<int>(writer.get_page_count() - 1);
        hc.r = redact.overlay_color_r;
        hc.g = redact.overlay_color_g;
        hc.b = redact.overlay_color_b;
        hc.alpha = 1.0;  // Fully opaque to hide content
        hc.print = true;

        if (!redact.quad_points.empty()) {
          for (const auto& quad : redact.quad_points) {
            if (quad.size() >= 8) {
              QuadPoints qp;
              qp.x1 = quad[0]; qp.y1 = quad[1];
              qp.x2 = quad[2]; qp.y2 = quad[3];
              qp.x3 = quad[4]; qp.y3 = quad[5];
              qp.x4 = quad[6]; qp.y4 = quad[7];
              hc.quads.push_back(qp);
            }
          }
        } else if (redact.rect.size() >= 4) {
          QuadPoints qp = quad_from_rect(
              redact.rect[0], redact.rect[1],
              redact.rect[2] - redact.rect[0],
              redact.rect[3] - redact.rect[1]);
          hc.quads.push_back(qp);
        }

        if (!hc.quads.empty()) {
          writer.add_highlight(hc.page, hc);
        }
      }
    }
  }

  return writer.write_to_memory(output);
}

}  // namespace nanopdf
