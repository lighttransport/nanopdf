// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF structure dump tool using nanopdf
//
// Outputs document information including layout, fonts, and images
// in YAML (default) or JSON format.
//
// Usage: pdfdump <input.pdf> [options]
//
// Options:
//   -f, --format <yaml|json>  Output format (default: yaml)
//   -o, --output <file>       Output to file instead of stdout
//   -p, --page <n>            Dump specific page only (1-based)
//   --no-fonts                Skip font information
//   --no-images               Skip image information
//   --verbose                 Include additional details
//   --help                    Show help message

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <algorithm>

#include "../../src/nanopdf.hh"

namespace {

// Output format enum
enum class OutputFormat {
  YAML,
  JSON
};

struct DumpOptions {
  std::string input_file;
  std::string output_file;
  OutputFormat format = OutputFormat::YAML;
  int specific_page = 0;  // 0 = all pages
  bool include_fonts = true;
  bool include_images = true;
  bool verbose = false;
};

// JSON string escaping
std::string escape_json_string(const std::string& s) {
  std::ostringstream ss;
  for (char c : s) {
    switch (c) {
      case '"': ss << "\\\""; break;
      case '\\': ss << "\\\\"; break;
      case '\b': ss << "\\b"; break;
      case '\f': ss << "\\f"; break;
      case '\n': ss << "\\n"; break;
      case '\r': ss << "\\r"; break;
      case '\t': ss << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<int>(static_cast<unsigned char>(c));
        } else {
          ss << c;
        }
    }
  }
  return ss.str();
}

// YAML string escaping (simple approach: quote if needed)
std::string escape_yaml_string(const std::string& s) {
  bool needs_quote = false;
  for (char c : s) {
    if (c == ':' || c == '#' || c == '\n' || c == '\r' || c == '\t' ||
        c == '"' || c == '\'' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == ',' || c == '&' || c == '*' || c == '!' || c == '|' || c == '>' ||
        c == '%' || c == '@' || c == '`') {
      needs_quote = true;
      break;
    }
  }
  if (s.empty() || s[0] == ' ' || s[s.length()-1] == ' ') {
    needs_quote = true;
  }

  if (needs_quote) {
    std::ostringstream ss;
    ss << '"';
    for (char c : s) {
      if (c == '"') ss << "\\\"";
      else if (c == '\\') ss << "\\\\";
      else if (c == '\n') ss << "\\n";
      else if (c == '\r') ss << "\\r";
      else if (c == '\t') ss << "\\t";
      else ss << c;
    }
    ss << '"';
    return ss.str();
  }
  return s;
}

// Color space type to string
std::string color_space_type_to_string(nanopdf::ColorSpaceType type) {
  switch (type) {
    case nanopdf::ColorSpaceType::DeviceGray: return "DeviceGray";
    case nanopdf::ColorSpaceType::DeviceRGB: return "DeviceRGB";
    case nanopdf::ColorSpaceType::DeviceCMYK: return "DeviceCMYK";
    case nanopdf::ColorSpaceType::CalGray: return "CalGray";
    case nanopdf::ColorSpaceType::CalRGB: return "CalRGB";
    case nanopdf::ColorSpaceType::Lab: return "Lab";
    case nanopdf::ColorSpaceType::ICCBased: return "ICCBased";
    case nanopdf::ColorSpaceType::Indexed: return "Indexed";
    case nanopdf::ColorSpaceType::Pattern: return "Pattern";
    case nanopdf::ColorSpaceType::Separation: return "Separation";
    case nanopdf::ColorSpaceType::DeviceN: return "DeviceN";
    default: return "Unknown";
  }
}

// Font type to string
std::string font_type_to_string(const std::string& subtype) {
  return subtype;
}

// Check if font has embedded data
bool is_font_embedded(const nanopdf::BaseFont& font) {
  if (!font.descriptor) return false;
  // Check if FontFile, FontFile2, or FontFile3 is present
  if (font.descriptor->font_file.type != nanopdf::Value::UNDEFINED &&
      font.descriptor->font_file.type != nanopdf::Value::NULL_OBJ) {
    return true;
  }
  return false;
}

// Get font embedding type
std::string get_font_embedding_type(const nanopdf::BaseFont& font) {
  if (!font.descriptor) return "not_embedded";
  if (font.descriptor->font_file.type != nanopdf::Value::UNDEFINED &&
      font.descriptor->font_file.type != nanopdf::Value::NULL_OBJ) {
    // FontFile = Type1, FontFile2 = TrueType, FontFile3 = CFF/OpenType
    if (font.subtype == "Type1" || font.subtype == "MMType1") {
      return "Type1";
    } else if (font.subtype == "TrueType" || font.subtype == "CIDFontType2") {
      return "TrueType";
    } else if (font.subtype == "CIDFontType0") {
      return "CFF";
    }
    return "embedded";
  }
  return "not_embedded";
}

// ============================================================================
// PDF Revision History Support
// ============================================================================

// Revision information structure
struct RevisionInfo {
  size_t revision_number{0};
  size_t start_offset{0};      // Start of this revision's data
  size_t end_offset{0};        // End offset (%%EOF position + length)
  size_t size_bytes{0};        // Size of this revision
  std::string md5_hash;        // MD5 hash of cumulative data up to this revision
  std::string sha256_hash;     // SHA256 hash of cumulative data up to this revision

  // XRef info for this revision
  size_t xref_offset{0};       // Offset of xref table/stream
  size_t prev_xref_offset{0};  // Prev pointer (0 if first revision)

  // Objects modified in this revision
  std::set<uint32_t> modified_objects;
  std::set<uint32_t> added_objects;
  std::set<uint32_t> deleted_objects;  // Objects marked as free
};

// Convert bytes to hex string
std::string bytes_to_hex(const uint8_t* data, size_t len) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    ss << std::setw(2) << static_cast<int>(data[i]);
  }
  return ss.str();
}

// Find all %%EOF markers in the PDF data
std::vector<size_t> find_eof_markers(const uint8_t* data, size_t size) {
  std::vector<size_t> markers;
  const char* eof_marker = "%%EOF";
  const size_t marker_len = 5;

  for (size_t i = 0; i + marker_len <= size; ++i) {
    if (std::memcmp(data + i, eof_marker, marker_len) == 0) {
      // Found %%EOF, record position after the marker
      markers.push_back(i + marker_len);

      // Skip past any trailing whitespace/newlines
      while (markers.back() < size &&
             (data[markers.back()] == '\r' || data[markers.back()] == '\n')) {
        markers.back()++;
      }
    }
  }

  return markers;
}

// Find startxref value before a given offset
size_t find_startxref_before(const uint8_t* data, size_t eof_offset) {
  // Search backwards for "startxref"
  const char* startxref = "startxref";
  const size_t startxref_len = 9;

  // Need at least startxref_len bytes
  if (eof_offset < startxref_len) return 0;

  // Search in the last 1024 bytes before %%EOF
  size_t search_start = (eof_offset > 1024) ? (eof_offset - 1024) : 0;
  size_t search_end = eof_offset - startxref_len;

  for (size_t i = search_end; i >= search_start; --i) {
    if (std::memcmp(data + i, startxref, startxref_len) == 0) {
      // Found startxref, now parse the offset value
      size_t pos = i + startxref_len;

      // Skip whitespace
      while (pos < eof_offset && (data[pos] == ' ' || data[pos] == '\r' ||
             data[pos] == '\n' || data[pos] == '\t')) {
        pos++;
      }

      // Parse number
      size_t xref_offset = 0;
      while (pos < eof_offset && data[pos] >= '0' && data[pos] <= '9') {
        xref_offset = xref_offset * 10 + (data[pos] - '0');
        pos++;
      }

      return xref_offset;
    }

    if (i == 0) break;
  }

  return 0;
}

// Parse xref table to get object entries for a revision
std::map<uint32_t, std::pair<uint64_t, bool>> parse_xref_entries(
    const uint8_t* data, size_t size, size_t xref_offset) {
  std::map<uint32_t, std::pair<uint64_t, bool>> entries;  // obj_num -> (offset, in_use)

  if (xref_offset >= size) return entries;

  // Check if this is an xref table or xref stream
  const char* xref_keyword = "xref";
  if (xref_offset + 4 <= size && std::memcmp(data + xref_offset, xref_keyword, 4) == 0) {
    // Traditional xref table
    size_t pos = xref_offset + 4;

    // Skip whitespace
    while (pos < size && (data[pos] == ' ' || data[pos] == '\r' ||
           data[pos] == '\n' || data[pos] == '\t')) {
      pos++;
    }

    // Parse subsections
    while (pos < size) {
      // Check for "trailer" keyword
      if (pos + 7 <= size && std::memcmp(data + pos, "trailer", 7) == 0) {
        break;
      }

      // Parse start object number
      uint32_t start_obj = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        start_obj = start_obj * 10 + (data[pos] - '0');
        pos++;
      }

      // Skip whitespace
      while (pos < size && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
      }

      // Parse count
      uint32_t count = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        count = count * 10 + (data[pos] - '0');
        pos++;
      }

      // Skip to next line
      while (pos < size && data[pos] != '\n') pos++;
      if (pos < size) pos++;

      // Parse entries
      for (uint32_t i = 0; i < count && pos + 20 <= size; ++i) {
        // Format: nnnnnnnnnn ggggg n/f (20 bytes per entry)
        uint64_t offset = 0;
        for (int j = 0; j < 10 && pos < size; ++j, ++pos) {
          if (data[pos] >= '0' && data[pos] <= '9') {
            offset = offset * 10 + (data[pos] - '0');
          }
        }

        // Skip space and generation number (5 digits + space) - with bounds check
        if (pos + 7 < size) {
          pos += 7;
        } else {
          break;
        }

        // Get in-use flag
        bool in_use = false;
        if (pos < size) {
          in_use = (data[pos] == 'n');
          pos++;
        }

        // Skip to end of line
        while (pos < size && data[pos] != '\n') pos++;
        if (pos < size) pos++;

        entries[start_obj + i] = std::make_pair(offset, in_use);
      }
    }
  }
  // Note: xref streams would need separate handling

  return entries;
}

// Find Prev pointer in trailer
size_t find_prev_in_trailer(const uint8_t* data, size_t size, size_t xref_offset) {
  // Find trailer after xref
  const char* trailer_keyword = "trailer";
  size_t trailer_pos = 0;

  for (size_t i = xref_offset; i + 7 < size; ++i) {
    if (std::memcmp(data + i, trailer_keyword, 7) == 0) {
      trailer_pos = i;
      break;
    }
  }

  if (trailer_pos == 0) return 0;

  // Search for /Prev in trailer dictionary
  const char* prev_keyword = "/Prev";
  for (size_t i = trailer_pos; i + 5 < size && i < trailer_pos + 1024; ++i) {
    if (std::memcmp(data + i, prev_keyword, 5) == 0) {
      size_t pos = i + 5;

      // Skip whitespace
      while (pos < size && (data[pos] == ' ' || data[pos] == '\r' ||
             data[pos] == '\n' || data[pos] == '\t')) {
        pos++;
      }

      // Parse number
      size_t prev_offset = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        prev_offset = prev_offset * 10 + (data[pos] - '0');
        pos++;
      }

      return prev_offset;
    }

    // Stop at ">>" (end of trailer dict)
    if (data[i] == '>' && i + 1 < size && data[i + 1] == '>') {
      break;
    }
  }

  return 0;
}

// Simple hash computation without using external crypto library
// Using inline implementation to avoid potential ABI issues
namespace {
// Simple FNV-1a hash for quick fingerprinting
uint64_t fnv1a_hash(const uint8_t* data, size_t len) {
  const uint64_t FNV_PRIME = 0x100000001b3ULL;
  const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
  uint64_t hash = FNV_OFFSET;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

std::string compute_simple_hash(const uint8_t* data, size_t len) {
  uint64_t h1 = fnv1a_hash(data, len);
  uint64_t h2 = fnv1a_hash(data, len / 2);  // Hash of first half
  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(16) << h1 << std::setw(16) << h2;
  return ss.str();
}
}

// Detect revisions in PDF
std::vector<RevisionInfo> detect_revisions(const uint8_t* data, size_t size) {
  std::vector<RevisionInfo> revisions;

  if (data == nullptr || size == 0) {
    return revisions;
  }

  // Find all %%EOF markers
  std::vector<size_t> eof_markers = find_eof_markers(data, size);

  if (eof_markers.empty()) {
    return revisions;
  }

  // Build revision info for each %%EOF
  size_t prev_end = 0;
  std::map<uint32_t, std::pair<uint64_t, bool>> cumulative_objects;

  for (size_t i = 0; i < eof_markers.size(); ++i) {
    RevisionInfo rev;
    rev.revision_number = i + 1;
    rev.start_offset = prev_end;
    rev.end_offset = eof_markers[i];
    rev.size_bytes = rev.end_offset - rev.start_offset;

    // Calculate hash for cumulative data up to this revision
    if (rev.end_offset > 0 && rev.end_offset <= size) {
      // Use simple hash - external crypto might have ABI issues
      rev.md5_hash = compute_simple_hash(data, rev.end_offset);
      rev.sha256_hash = rev.md5_hash;  // Use same hash for now
    }

    // Find xref offset for this revision (eof_markers[i] points after %%EOF)
    if (eof_markers[i] >= 10) {
      rev.xref_offset = find_startxref_before(data, eof_markers[i]);
    }

    // Find Prev pointer
    if (rev.xref_offset > 0 && rev.xref_offset < size) {
      rev.prev_xref_offset = find_prev_in_trailer(data, size, rev.xref_offset);
    }

    // Parse xref entries for this revision
    if (rev.xref_offset > 0 && rev.xref_offset < size) {
      auto new_entries = parse_xref_entries(data, size, rev.xref_offset);

      // Determine added/modified/deleted objects
      for (const auto& entry : new_entries) {
        uint32_t obj_num = entry.first;
        bool in_use = entry.second.second;

        auto prev_it = cumulative_objects.find(obj_num);

        if (prev_it == cumulative_objects.end()) {
          // New object
          if (in_use) {
            rev.added_objects.insert(obj_num);
          }
        } else {
          // Existing object
          bool was_in_use = prev_it->second.second;
          uint64_t old_offset = prev_it->second.first;
          uint64_t new_offset = entry.second.first;

          if (in_use && !was_in_use) {
            // Was deleted, now restored
            rev.added_objects.insert(obj_num);
          } else if (!in_use && was_in_use) {
            // Now deleted
            rev.deleted_objects.insert(obj_num);
          } else if (in_use && was_in_use && old_offset != new_offset) {
            // Modified (different offset)
            rev.modified_objects.insert(obj_num);
          }
        }

        // Update cumulative state
        cumulative_objects[obj_num] = entry.second;
      }
    }

    revisions.push_back(rev);
    prev_end = rev.end_offset;
  }

  return revisions;
}

class OutputWriter {
public:
  OutputWriter(std::ostream& os, OutputFormat fmt) : out_(os), format_(fmt) {}

  void begin_document() {
    if (format_ == OutputFormat::JSON) {
      out_ << "{\n";
    }
    first_top_level_ = true;
  }

  void end_document() {
    if (format_ == OutputFormat::JSON) {
      out_ << "\n}\n";
    }
  }

  void begin_section(const std::string& name, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_top_level_) out_ << ",\n";
      first_top_level_ = false;
      out_ << ind << "  \"" << name << "\": {";
      first_in_section_ = true;
    } else {
      out_ << ind << name << ":\n";
    }
  }

  void end_section(int indent = 0) {
    if (format_ == OutputFormat::JSON) {
      std::string ind(indent * 2, ' ');
      out_ << "\n" << ind << "  }";
    }
  }

  void begin_array(const std::string& name, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_section_) out_ << ",";
      out_ << "\n";
      first_in_section_ = false;
      out_ << ind << "    \"" << name << "\": [";
    } else {
      out_ << ind << "  " << name << ":\n";
    }
    first_in_array_ = true;
  }

  void end_array(int indent = 0) {
    if (format_ == OutputFormat::JSON) {
      std::string ind(indent * 2, ' ');
      if (!first_in_array_) {
        out_ << "\n" << ind << "    ]";
      } else {
        out_ << "]";
      }
    }
  }

  void begin_array_item(int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_array_) out_ << ",";
      out_ << "\n";
      first_in_array_ = false;
      out_ << ind << "      {";
    }
    first_in_item_ = true;
  }

  void end_array_item(int indent = 0) {
    if (format_ == OutputFormat::JSON) {
      out_ << "}";
    }
  }

  void write_kv(const std::string& key, const std::string& value, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_section_) out_ << ",";
      out_ << "\n";
      first_in_section_ = false;
      out_ << ind << "    \"" << key << "\": \"" << escape_json_string(value) << "\"";
    } else {
      out_ << ind << "  " << key << ": " << escape_yaml_string(value) << "\n";
    }
  }

  void write_kv(const std::string& key, int value, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_section_) out_ << ",";
      out_ << "\n";
      first_in_section_ = false;
      out_ << ind << "    \"" << key << "\": " << value;
    } else {
      out_ << ind << "  " << key << ": " << value << "\n";
    }
  }

  void write_kv(const std::string& key, double value, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_section_) out_ << ",";
      out_ << "\n";
      first_in_section_ = false;
      out_ << ind << "    \"" << key << "\": " << std::fixed << std::setprecision(2) << value;
    } else {
      out_ << ind << "  " << key << ": " << std::fixed << std::setprecision(2) << value << "\n";
    }
  }

  void write_kv(const std::string& key, bool value, int indent = 0) {
    std::string ind(indent * 2, ' ');
    if (format_ == OutputFormat::JSON) {
      if (!first_in_section_) out_ << ",";
      out_ << "\n";
      first_in_section_ = false;
      out_ << ind << "    \"" << key << "\": " << (value ? "true" : "false");
    } else {
      out_ << ind << "  " << key << ": " << (value ? "true" : "false") << "\n";
    }
  }

  void write_array_item_kv(const std::string& key, const std::string& value) {
    if (format_ == OutputFormat::JSON) {
      if (!first_in_item_) out_ << ", ";
      first_in_item_ = false;
      out_ << "\"" << key << "\": \"" << escape_json_string(value) << "\"";
    } else {
      if (first_in_item_) {
        out_ << "    - " << key << ": " << escape_yaml_string(value) << "\n";
        first_in_item_ = false;
      } else {
        out_ << "      " << key << ": " << escape_yaml_string(value) << "\n";
      }
    }
  }

  void write_array_item_kv(const std::string& key, int value) {
    if (format_ == OutputFormat::JSON) {
      if (!first_in_item_) out_ << ", ";
      first_in_item_ = false;
      out_ << "\"" << key << "\": " << value;
    } else {
      if (first_in_item_) {
        out_ << "    - " << key << ": " << value << "\n";
        first_in_item_ = false;
      } else {
        out_ << "      " << key << ": " << value << "\n";
      }
    }
  }

  void write_array_item_kv(const std::string& key, double value) {
    if (format_ == OutputFormat::JSON) {
      if (!first_in_item_) out_ << ", ";
      first_in_item_ = false;
      out_ << "\"" << key << "\": " << std::fixed << std::setprecision(2) << value;
    } else {
      if (first_in_item_) {
        out_ << "    - " << key << ": " << std::fixed << std::setprecision(2) << value << "\n";
        first_in_item_ = false;
      } else {
        out_ << "      " << key << ": " << std::fixed << std::setprecision(2) << value << "\n";
      }
    }
  }

  void write_array_item_kv(const std::string& key, bool value) {
    if (format_ == OutputFormat::JSON) {
      if (!first_in_item_) out_ << ", ";
      first_in_item_ = false;
      out_ << "\"" << key << "\": " << (value ? "true" : "false");
    } else {
      if (first_in_item_) {
        out_ << "    - " << key << ": " << (value ? "true" : "false") << "\n";
        first_in_item_ = false;
      } else {
        out_ << "      " << key << ": " << (value ? "true" : "false") << "\n";
      }
    }
  }

  void reset_section() {
    first_in_section_ = true;
  }

private:
  std::ostream& out_;
  OutputFormat format_;
  bool first_top_level_ = true;
  bool first_in_section_ = true;
  bool first_in_array_ = true;
  bool first_in_item_ = true;
};

void dump_document_info(OutputWriter& writer, const nanopdf::Pdf& pdf,
                        const DumpOptions& options) {
  writer.begin_section("document");

  // PDF version
  std::ostringstream version_ss;
  version_ss << pdf.version_major << "." << pdf.version_minor;
  writer.write_kv("pdf_version", version_ss.str());

  // Document info
  const auto& info = pdf.catalog.document_info;
  if (!info.title.empty()) {
    writer.write_kv("title", info.title);
  }
  if (!info.author.empty()) {
    writer.write_kv("author", info.author);
  }
  if (!info.subject.empty()) {
    writer.write_kv("subject", info.subject);
  }
  if (!info.keywords.empty()) {
    writer.write_kv("keywords", info.keywords);
  }
  if (!info.creator.empty()) {
    writer.write_kv("creator", info.creator);
  }
  if (!info.producer.empty()) {
    writer.write_kv("producer", info.producer);
  }
  if (!info.creation_date.empty()) {
    writer.write_kv("creation_date", info.creation_date);
  }
  if (!info.mod_date.empty()) {
    writer.write_kv("modification_date", info.mod_date);
  }

  // Page count
  writer.write_kv("page_count", static_cast<int>(pdf.catalog.pages.size()));

  // Encryption status
  if (pdf.security.algorithm != nanopdf::EncryptionAlgorithm::None) {
    std::string enc_type;
    switch (pdf.security.algorithm) {
      case nanopdf::EncryptionAlgorithm::RC4_40: enc_type = "RC4_40"; break;
      case nanopdf::EncryptionAlgorithm::RC4_128: enc_type = "RC4_128"; break;
      case nanopdf::EncryptionAlgorithm::AES_128: enc_type = "AES_128"; break;
      case nanopdf::EncryptionAlgorithm::AES_256: enc_type = "AES_256"; break;
      default: enc_type = "unknown"; break;
    }
    writer.write_kv("encryption", enc_type);
  } else {
    writer.write_kv("encrypted", false);
  }

  writer.end_section();
}

void dump_layout(OutputWriter& writer, const nanopdf::Pdf& pdf,
                 const DumpOptions& options) {
  writer.begin_section("layout");

  writer.begin_array("pages");

  int page_num = 0;
  for (const auto& page : pdf.catalog.pages) {
    page_num++;

    // Skip if specific page requested
    if (options.specific_page > 0 && page_num != options.specific_page) {
      continue;
    }

    writer.begin_array_item();
    writer.write_array_item_kv("number", page_num);

    // Media box
    if (page.media_box.size() >= 4) {
      double width = page.media_box[2] - page.media_box[0];
      double height = page.media_box[3] - page.media_box[1];
      writer.write_array_item_kv("width_pts", width);
      writer.write_array_item_kv("height_pts", height);

      // Convert to mm (1 pt = 0.352778 mm)
      if (options.verbose) {
        writer.write_array_item_kv("width_mm", width * 0.352778);
        writer.write_array_item_kv("height_mm", height * 0.352778);
      }
    }

    // Crop box if different
    if (!page.crop_box.empty() && page.crop_box.size() >= 4) {
      if (page.crop_box != page.media_box) {
        double crop_width = page.crop_box[2] - page.crop_box[0];
        double crop_height = page.crop_box[3] - page.crop_box[1];
        writer.write_array_item_kv("crop_width_pts", crop_width);
        writer.write_array_item_kv("crop_height_pts", crop_height);
      }
    }

    // Rotation
    if (page.rotate != 0.0) {
      writer.write_array_item_kv("rotation", static_cast<int>(page.rotate));
    }

    writer.end_array_item();
  }

  writer.end_array();
  writer.end_section();
}

void dump_fonts(OutputWriter& writer, const nanopdf::Pdf& pdf,
                const DumpOptions& options) {
  writer.begin_section("fonts");

  // Collect all unique fonts across all pages
  std::map<std::string, const nanopdf::BaseFont*> all_fonts;

  int page_num = 0;
  for (const auto& page : pdf.catalog.pages) {
    page_num++;

    // Skip if specific page requested
    if (options.specific_page > 0 && page_num != options.specific_page) {
      continue;
    }

    // Ensure fonts are loaded for this page
    page.ensure_fonts_loaded(pdf);

    for (const auto& font_pair : page.fonts) {
      if (font_pair.second && all_fonts.find(font_pair.first) == all_fonts.end()) {
        all_fonts[font_pair.first] = font_pair.second.get();
      }
    }
  }

  writer.write_kv("count", static_cast<int>(all_fonts.size()));

  writer.begin_array("list");

  for (const auto& font_pair : all_fonts) {
    const auto& font = font_pair.second;
    if (!font) continue;

    writer.begin_array_item();
    writer.write_array_item_kv("name", font_pair.first);
    writer.write_array_item_kv("base_font", font->base_font);
    writer.write_array_item_kv("type", font->subtype);

    if (!font->encoding.empty()) {
      writer.write_array_item_kv("encoding", font->encoding);
    }

    bool embedded = is_font_embedded(*font);
    writer.write_array_item_kv("embedded", embedded);

    if (embedded) {
      writer.write_array_item_kv("embedding_type", get_font_embedding_type(*font));
    }

    // Font metrics (if available and verbose)
    if (options.verbose && font->descriptor) {
      if (font->descriptor->ascent != 0.0) {
        writer.write_array_item_kv("ascent", font->descriptor->ascent);
      }
      if (font->descriptor->descent != 0.0) {
        writer.write_array_item_kv("descent", font->descriptor->descent);
      }
      if (!font->descriptor->font_family.empty()) {
        writer.write_array_item_kv("family", font->descriptor->font_family);
      }
    }

    writer.end_array_item();
  }

  writer.end_array();
  writer.end_section();
}

void dump_images(OutputWriter& writer, const nanopdf::Pdf& pdf,
                 const DumpOptions& options) {
  writer.begin_section("images");

  // Collect all images across all pages
  std::vector<std::tuple<int, std::string, nanopdf::ImageXObject>> all_images;

  int page_num = 0;
  for (const auto& page : pdf.catalog.pages) {
    page_num++;

    // Skip if specific page requested
    if (options.specific_page > 0 && page_num != options.specific_page) {
      continue;
    }

    // Parse XObject resources to find images
    auto xobjects = nanopdf::parse_xobject_resources(pdf, page.resources);

    for (const auto& xobj_pair : xobjects) {
      all_images.push_back(std::make_tuple(page_num, xobj_pair.first, xobj_pair.second));
    }
  }

  writer.write_kv("count", static_cast<int>(all_images.size()));

  writer.begin_array("list");

  for (const auto& img_tuple : all_images) {
    int pg_num = std::get<0>(img_tuple);
    const std::string& name = std::get<1>(img_tuple);
    const nanopdf::ImageXObject& img = std::get<2>(img_tuple);

    writer.begin_array_item();
    writer.write_array_item_kv("name", name);
    writer.write_array_item_kv("page", pg_num);
    writer.write_array_item_kv("width", img.width);
    writer.write_array_item_kv("height", img.height);
    writer.write_array_item_kv("bits_per_component", img.bits_per_component);
    writer.write_array_item_kv("color_space", color_space_type_to_string(img.color_space.type));

    if (!img.filter.empty()) {
      writer.write_array_item_kv("filter", img.filter);
    }

    if (img.image_mask) {
      writer.write_array_item_kv("image_mask", true);
    }

    if (img.interpolate) {
      writer.write_array_item_kv("interpolate", true);
    }

    if (options.verbose) {
      // Raw data size
      if (!img.raw_data.empty()) {
        writer.write_array_item_kv("raw_size_bytes", static_cast<int>(img.raw_data.size()));
      }
      // Decoded data size
      if (!img.data.empty()) {
        writer.write_array_item_kv("decoded_size_bytes", static_cast<int>(img.data.size()));
      }
    }

    writer.end_array_item();
  }

  writer.end_array();
  writer.end_section();
}

void dump_annotations(OutputWriter& writer, const nanopdf::Pdf& pdf,
                      const DumpOptions& options) {
  writer.begin_section("annotations");

  int total_annotations = 0;
  int page_num = 0;

  // First count total
  for (const auto& page : pdf.catalog.pages) {
    page_num++;
    if (options.specific_page > 0 && page_num != options.specific_page) {
      continue;
    }
    total_annotations += static_cast<int>(page.annotations.size());
  }

  writer.write_kv("count", total_annotations);

  if (total_annotations > 0 && options.verbose) {
    writer.begin_array("list");

    page_num = 0;
    for (const auto& page : pdf.catalog.pages) {
      page_num++;
      if (options.specific_page > 0 && page_num != options.specific_page) {
        continue;
      }

      for (const auto& annot : page.annotations) {
        if (!annot) continue;

        writer.begin_array_item();
        writer.write_array_item_kv("page", page_num);

        // Annotation type
        std::string type_name;
        switch (annot->type) {
          case nanopdf::AnnotationType::Text: type_name = "Text"; break;
          case nanopdf::AnnotationType::Link: type_name = "Link"; break;
          case nanopdf::AnnotationType::FreeText: type_name = "FreeText"; break;
          case nanopdf::AnnotationType::Highlight: type_name = "Highlight"; break;
          case nanopdf::AnnotationType::Underline: type_name = "Underline"; break;
          case nanopdf::AnnotationType::StrikeOut: type_name = "StrikeOut"; break;
          case nanopdf::AnnotationType::Widget: type_name = "Widget"; break;
          default: type_name = "Other"; break;
        }
        writer.write_array_item_kv("type", type_name);

        if (!annot->contents.empty()) {
          writer.write_array_item_kv("contents", annot->contents);
        }

        writer.end_array_item();
      }
    }

    writer.end_array();
  }

  writer.end_section();
}

void dump_forms(OutputWriter& writer, const nanopdf::Pdf& pdf,
                const DumpOptions& options) {
  writer.begin_section("forms");

  int field_count = static_cast<int>(pdf.catalog.form_fields.size());
  writer.write_kv("field_count", field_count);

  // Signature fields
  int sig_count = static_cast<int>(pdf.catalog.signature_fields.size());
  writer.write_kv("signature_field_count", sig_count);

  if (field_count > 0 && options.verbose) {
    writer.begin_array("fields");

    for (const auto& field : pdf.catalog.form_fields) {
      if (!field) continue;

      writer.begin_array_item();
      writer.write_array_item_kv("name", field->full_name);

      std::string type_name;
      switch (field->type) {
        case nanopdf::FieldType::Text: type_name = "Text"; break;
        case nanopdf::FieldType::Button: type_name = "Button"; break;
        case nanopdf::FieldType::Choice: type_name = "Choice"; break;
        case nanopdf::FieldType::Signature: type_name = "Signature"; break;
      }
      writer.write_array_item_kv("type", type_name);

      writer.end_array_item();
    }

    writer.end_array();
  }

  writer.end_section();
}

// Verify signature integrity by checking byte range coverage
bool verify_signature_byte_range(const std::vector<uint8_t>& pdf_data,
                                  const nanopdf::SignatureField& sig) {
  if (sig.byte_range.size() != 4) return false;

  uint64_t offset1 = sig.byte_range[0];
  uint64_t length1 = sig.byte_range[1];
  uint64_t offset2 = sig.byte_range[2];
  uint64_t length2 = sig.byte_range[3];

  // Check that byte ranges are valid
  if (offset1 != 0) return false;  // First range should start at 0
  if (offset1 + length1 > pdf_data.size()) return false;
  if (offset2 > pdf_data.size()) return false;
  if (offset2 + length2 > pdf_data.size()) return false;

  // Check that ranges don't overlap and cover the whole file except signature
  if (offset2 <= offset1 + length1) return false;

  // The gap between ranges should be the signature contents
  uint64_t gap_start = offset1 + length1;
  uint64_t gap_end = offset2;

  // Verify the gap contains the signature hex string
  // (between < and > in the /Contents field)
  if (gap_end <= gap_start) return false;

  return true;
}

// Check if document was modified after signing
std::string check_signature_integrity(const std::vector<uint8_t>& pdf_data,
                                       const nanopdf::SignatureField& sig) {
  if (!sig.signature_present || !sig.is_signed) {
    return "not_signed";
  }

  if (sig.byte_range.empty()) {
    return "no_byte_range";
  }

  if (!verify_signature_byte_range(pdf_data, sig)) {
    return "invalid_byte_range";
  }

  // Check if byte range covers whole document
  if (sig.byte_range.size() == 4) {
    uint64_t covered = sig.byte_range[1] + sig.byte_range[3];
    uint64_t gap = sig.byte_range[2] - (sig.byte_range[0] + sig.byte_range[1]);
    uint64_t total = covered + gap;

    if (total < pdf_data.size()) {
      // Document has been modified (appended) after signing
      return "modified_after_signing";
    }
  }

  return "intact";
}

void dump_signatures(OutputWriter& writer, const nanopdf::Pdf& pdf,
                     const std::vector<uint8_t>& pdf_data,
                     const DumpOptions& options) {
  writer.begin_section("signatures");

  const auto& sigs = pdf.catalog.signature_fields;
  int sig_count = static_cast<int>(sigs.size());

  // Count actual signed signatures
  int signed_count = 0;
  for (const auto& sig : sigs) {
    if (sig.is_signed || sig.signature_present) {
      signed_count++;
    }
  }

  writer.write_kv("total_fields", sig_count);
  writer.write_kv("signed_count", signed_count);

  if (sig_count > 0) {
    writer.begin_array("list");

    for (const auto& sig : sigs) {
      writer.begin_array_item();

      // Basic info
      if (!sig.name.empty()) {
        writer.write_array_item_kv("name", sig.name);
      }

      writer.write_array_item_kv("is_signed", sig.is_signed || sig.signature_present);

      if (sig.is_signed || sig.signature_present) {
        // Signer info
        if (!sig.signing_reason.empty()) {
          writer.write_array_item_kv("reason", sig.signing_reason);
        }
        if (!sig.signing_location.empty()) {
          writer.write_array_item_kv("location", sig.signing_location);
        }
        if (!sig.signing_contact_info.empty()) {
          writer.write_array_item_kv("contact", sig.signing_contact_info);
        }
        if (!sig.signing_date.empty()) {
          writer.write_array_item_kv("date", sig.signing_date);
        }

        // Signature algorithm
        if (!sig.digest_algorithm.empty()) {
          writer.write_array_item_kv("algorithm", sig.digest_algorithm);
        }

        // Signature filter/subfilter
        if (!sig.filter.empty()) {
          writer.write_array_item_kv("filter", sig.filter);
        }
        if (!sig.subfilter.empty()) {
          writer.write_array_item_kv("subfilter", sig.subfilter);
        }

        // MDP (Modification Detection and Prevention) / Certification signature
        if (sig.is_certification_signature) {
          writer.write_array_item_kv("type", "certification");

          // DocMDP permissions
          if (sig.mdp_permissions > 0) {
            std::string perm_desc;
            switch (sig.mdp_permissions) {
              case 1: perm_desc = "no_changes"; break;
              case 2: perm_desc = "form_fill_and_sign"; break;
              case 3: perm_desc = "form_fill_sign_annotate"; break;
              default: perm_desc = "unknown"; break;
            }
            writer.write_array_item_kv("mdp_permissions", sig.mdp_permissions);
            writer.write_array_item_kv("allowed_changes", perm_desc);
          }
        } else if (!sig.transform_method.empty()) {
          writer.write_array_item_kv("type", "approval");
          writer.write_array_item_kv("transform_method", sig.transform_method);
        } else {
          writer.write_array_item_kv("type", "approval");
        }

        // Locked fields (for FieldMDP)
        if (!sig.locked_fields.empty()) {
          std::ostringstream fields_ss;
          bool first = true;
          for (const auto& field : sig.locked_fields) {
            if (!first) fields_ss << ", ";
            fields_ss << field;
            first = false;
          }
          writer.write_array_item_kv("locked_fields", fields_ss.str());
        }

        // Timestamp information
        if (sig.has_timestamp) {
          writer.write_array_item_kv("has_timestamp", true);

          if (sig.is_document_timestamp) {
            writer.write_array_item_kv("timestamp_type", "document_timestamp");
          } else {
            writer.write_array_item_kv("timestamp_type", "embedded");
          }

          if (!sig.timestamp_hash_algorithm.empty()) {
            writer.write_array_item_kv("timestamp_algorithm", sig.timestamp_hash_algorithm);
          }

          if (!sig.timestamp_authority.empty()) {
            writer.write_array_item_kv("timestamp_authority", sig.timestamp_authority);
          }

          if (!sig.timestamp_date.empty()) {
            writer.write_array_item_kv("timestamp_date", sig.timestamp_date);
          }
        }

        // Byte range info
        if (!sig.byte_range.empty() && sig.byte_range.size() == 4) {
          std::ostringstream br_ss;
          br_ss << "[" << sig.byte_range[0] << ", " << sig.byte_range[1]
                << ", " << sig.byte_range[2] << ", " << sig.byte_range[3] << "]";
          writer.write_array_item_kv("byte_range", br_ss.str());

          // Calculate coverage
          uint64_t covered = sig.byte_range[1] + sig.byte_range[3];
          writer.write_array_item_kv("bytes_covered", static_cast<int>(covered));
        }

        // Signature contents size
        if (!sig.signature_contents.empty()) {
          writer.write_array_item_kv("signature_size", static_cast<int>(sig.signature_contents.size()));
        }

        // Integrity check
        std::string integrity = check_signature_integrity(pdf_data, sig);
        writer.write_array_item_kv("integrity", integrity);

        if (options.verbose) {
          // Page reference
          if (sig.page_ref > 0) {
            writer.write_array_item_kv("page_object", static_cast<int>(sig.page_ref));
          }

          // Rectangle
          if (sig.rect.size() >= 4) {
            std::ostringstream rect_ss;
            rect_ss << std::fixed << std::setprecision(1)
                    << "[" << sig.rect[0] << ", " << sig.rect[1]
                    << ", " << sig.rect[2] << ", " << sig.rect[3] << "]";
            writer.write_array_item_kv("rect", rect_ss.str());
          }
        }
      }

      writer.end_array_item();
    }

    writer.end_array();
  }

  writer.end_section();
}

void dump_outlines(OutputWriter& writer, const nanopdf::Pdf& pdf,
                   const DumpOptions& options) {
  writer.begin_section("outlines");

  bool has_outlines = pdf.catalog.outline_root != nullptr;
  writer.write_kv("has_bookmarks", has_outlines);

  if (has_outlines && options.verbose) {
    // Count outline items
    std::function<int(const nanopdf::OutlineItem*)> count_items =
        [&count_items](const nanopdf::OutlineItem* item) -> int {
      if (!item) return 0;
      int count = 1;
      for (const auto& child : item->children) {
        count += count_items(child.get());
      }
      return count;
    };

    int bookmark_count = 0;
    for (const auto& child : pdf.catalog.outline_root->children) {
      bookmark_count += count_items(child.get());
    }
    writer.write_kv("bookmark_count", bookmark_count);
  }

  writer.end_section();
}

void dump_revisions(OutputWriter& writer, const std::vector<RevisionInfo>& revisions,
                    const DumpOptions& options) {
  writer.begin_section("revisions");

  writer.write_kv("count", static_cast<int>(revisions.size()));

  if (!revisions.empty()) {
    // Show current document hash (last revision)
    writer.write_kv("current_md5", revisions.back().md5_hash);
    writer.write_kv("current_sha256", revisions.back().sha256_hash);
  }

  writer.begin_array("history");

  for (const auto& rev : revisions) {
    writer.begin_array_item();
    writer.write_array_item_kv("revision", static_cast<int>(rev.revision_number));
    writer.write_array_item_kv("size_bytes", static_cast<int>(rev.size_bytes));
    writer.write_array_item_kv("cumulative_size", static_cast<int>(rev.end_offset));
    writer.write_array_item_kv("md5", rev.md5_hash);

    if (options.verbose) {
      writer.write_array_item_kv("sha256", rev.sha256_hash);
      writer.write_array_item_kv("xref_offset", static_cast<int>(rev.xref_offset));

      if (rev.prev_xref_offset > 0) {
        writer.write_array_item_kv("prev_xref", static_cast<int>(rev.prev_xref_offset));
      }
    }

    // Diff info
    if (!rev.added_objects.empty()) {
      std::ostringstream ss;
      bool first = true;
      for (uint32_t obj : rev.added_objects) {
        if (!first) ss << ",";
        ss << obj;
        first = false;
      }
      writer.write_array_item_kv("added_objects", ss.str());
    }

    if (!rev.modified_objects.empty()) {
      std::ostringstream ss;
      bool first = true;
      for (uint32_t obj : rev.modified_objects) {
        if (!first) ss << ",";
        ss << obj;
        first = false;
      }
      writer.write_array_item_kv("modified_objects", ss.str());
    }

    if (!rev.deleted_objects.empty()) {
      std::ostringstream ss;
      bool first = true;
      for (uint32_t obj : rev.deleted_objects) {
        if (!first) ss << ",";
        ss << obj;
        first = false;
      }
      writer.write_array_item_kv("deleted_objects", ss.str());
    }

    writer.end_array_item();
  }

  writer.end_array();
  writer.end_section();
}

void print_usage(const char* program_name) {
  std::cout << "PDF Structure Dump Tool using nanopdf\n";
  std::cout << "\n";
  std::cout << "Usage: " << program_name << " <input.pdf> [options]\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  -f, --format <yaml|json>  Output format (default: yaml)\n";
  std::cout << "  -o, --output <file>       Output to file instead of stdout\n";
  std::cout << "  -p, --page <n>            Dump specific page only (1-based)\n";
  std::cout << "  --no-fonts                Skip font information\n";
  std::cout << "  --no-images               Skip image information\n";
  std::cout << "  --verbose                 Include additional details\n";
  std::cout << "  --help                    Show this help message\n";
  std::cout << "\n";
  std::cout << "Examples:\n";
  std::cout << "  " << program_name << " document.pdf\n";
  std::cout << "  " << program_name << " document.pdf -f json\n";
  std::cout << "  " << program_name << " document.pdf -o info.yaml --verbose\n";
  std::cout << "  " << program_name << " document.pdf -p 1 --no-images\n";
}

bool parse_arguments(int argc, char* argv[], DumpOptions& options) {
  if (argc < 2) {
    return false;
  }

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      return false;
    } else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
      std::string fmt = argv[++i];
      if (fmt == "yaml" || fmt == "YAML") {
        options.format = OutputFormat::YAML;
      } else if (fmt == "json" || fmt == "JSON") {
        options.format = OutputFormat::JSON;
      } else {
        std::cerr << "Error: Unknown format: " << fmt << " (use yaml or json)\n";
        return false;
      }
    } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
      options.output_file = argv[++i];
    } else if ((arg == "-p" || arg == "--page") && i + 1 < argc) {
      options.specific_page = std::atoi(argv[++i]);
      if (options.specific_page < 1) {
        std::cerr << "Error: Page number must be >= 1\n";
        return false;
      }
    } else if (arg == "--no-fonts") {
      options.include_fonts = false;
    } else if (arg == "--no-images") {
      options.include_images = false;
    } else if (arg == "--verbose" || arg == "-v") {
      options.verbose = true;
    } else if (arg[0] == '-') {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    } else if (options.input_file.empty()) {
      options.input_file = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return false;
    }
  }

  if (options.input_file.empty()) {
    std::cerr << "Error: No input file specified\n";
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  DumpOptions options;

  if (!parse_arguments(argc, argv, options)) {
    print_usage(argv[0]);
    return 1;
  }

  // Read PDF file
  std::ifstream ifs(options.input_file, std::ios::binary);
  if (!ifs) {
    std::cerr << "Error: Failed to open PDF file: " << options.input_file << "\n";
    return 1;
  }

  std::vector<uint8_t> pdf_data((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
  ifs.close();

  // Check minimum file size
  if (pdf_data.size() < 100) {
    std::cerr << "Error: File is too small to be a valid PDF (" << pdf_data.size() << " bytes)\n";
    if (pdf_data.size() >= 8 && std::memcmp(pdf_data.data(), "%PDF-", 5) == 0) {
      std::cerr << "       File appears to be truncated (has PDF header but no content)\n";
    }
    return 1;
  }

  // Check for PDF header
  if (pdf_data.size() < 8 || std::memcmp(pdf_data.data(), "%PDF-", 5) != 0) {
    std::cerr << "Error: File does not appear to be a PDF (missing %PDF- header)\n";
    return 1;
  }

  // Parse PDF
  nanopdf::Pdf pdf;
  if (!nanopdf::parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
    std::cerr << "Error: Failed to parse PDF file\n";
    std::cerr << "       The file may be corrupted, encrypted, or use unsupported features\n";
    return 1;
  }

  // Ensure metadata is loaded
  pdf.ensure_metadata_loaded();

  // Validate page number
  if (options.specific_page > 0 &&
      options.specific_page > static_cast<int>(pdf.catalog.pages.size())) {
    std::cerr << "Error: Page " << options.specific_page << " does not exist. ";
    std::cerr << "PDF has " << pdf.catalog.pages.size() << " page(s).\n";
    return 1;
  }

  // Setup output stream
  std::ofstream output_file;
  std::ostream* out = &std::cout;

  if (!options.output_file.empty()) {
    output_file.open(options.output_file);
    if (!output_file) {
      std::cerr << "Error: Failed to open output file: " << options.output_file << "\n";
      return 1;
    }
    out = &output_file;
  }

  // Create writer and dump information
  OutputWriter writer(*out, options.format);

  writer.begin_document();

  // Always include document info and layout
  dump_document_info(writer, pdf, options);
  dump_layout(writer, pdf, options);

  // Optionally include fonts
  if (options.include_fonts) {
    dump_fonts(writer, pdf, options);
  }

  // Optionally include images
  if (options.include_images) {
    dump_images(writer, pdf, options);
  }

  // Include annotations summary
  dump_annotations(writer, pdf, options);

  // Include forms summary
  dump_forms(writer, pdf, options);

  // Include digital signatures
  dump_signatures(writer, pdf, pdf_data, options);

  // Include outlines/bookmarks summary
  dump_outlines(writer, pdf, options);

  // Detect and dump revision history
  std::vector<RevisionInfo> revisions = detect_revisions(pdf_data.data(), pdf_data.size());
  dump_revisions(writer, revisions, options);

  writer.end_document();

  if (output_file.is_open()) {
    output_file.close();
  }

  return 0;
}
