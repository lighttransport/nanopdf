// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstddef.h"
#include "nanocstdint.h"
#include "nanocstring.h"
using namespace nanostl;
#else
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <functional>
#include <memory>
#endif

namespace nanopdf {

class StreamReader {
 public:
  StreamReader(const uint8_t* data, size_t size) 
      : data_(data), size_(size), pos_(0) {}
  
  bool read(uint8_t* buf, size_t count) {
    if (pos_ + count > size_) return false;
    memcpy(buf, data_ + pos_, count);
    pos_ += count;
    return true;
  }
  
  uint8_t peek() const {
    if (pos_ >= size_) return 0;
    return data_[pos_];
  }
  
  uint8_t get() {
    if (pos_ >= size_) return 0;
    return data_[pos_++];
  }
  
  void seek(size_t pos) {
    pos_ = (pos < size_) ? pos : size_;
  }
  
  size_t pos() const { return pos_; }
  size_t size() const { return size_; }
  bool eof() const { return pos_ >= size_; }
  
 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_;
};

namespace detail {
class Parser;
}

// forward decl
class Value;

// Forward declare Dictionary for use in StreamValue
using Dictionary = std::map<std::string, Value>;

// Define StreamValue before Value
struct StreamValue {
    std::vector<uint8_t> data;
    Dictionary dict;

    StreamValue() = default;
    StreamValue(const StreamValue& other) = default;
    StreamValue& operator=(const StreamValue& other) = default;
    StreamValue(StreamValue&& other) = default;
    StreamValue& operator=(StreamValue&& other) = default;
};

// Value type definitions first
class Value {
 public:
  enum Type {
    UNDEFINED,
    BOOLEAN,
    NUMBER,
    STRING,
    NAME,
    ARRAY,
    DICTIONARY,
    STREAM,
    REFERENCE,
    NULL_OBJ
  };

  Type type{UNDEFINED};
  
  union {
    bool boolean;
    double number;
    std::string str;
    std::string name;
    std::vector<Value> array;
    Dictionary dict;
    StreamValue stream;
  };

  // For reference objects
  uint32_t ref_object_number{0};
  uint16_t ref_generation_number{0};

  Value() : type(UNDEFINED) {}
  ~Value() { clear(); }
  
  Value(const Value& other) : type(UNDEFINED) { *this = other; }
  Value(Value&& other) noexcept : type(UNDEFINED) { *this = std::move(other); }
  
  Value& operator=(const Value& other);
  Value& operator=(Value&& other) noexcept;
  
  void clear();

 private:
  void destroy();
};

// Font types supported in PDF
enum class FontType {
  Type1,
  TrueType,
  MMType1,
  Type3,
  Type0,
  CIDFontType0,
  CIDFontType2
};

// Font descriptor containing metrics and font file data
struct FontDescriptor {
  std::string font_name;
  std::string font_family;
  FontType font_type{FontType::Type1};
  
  // Font metrics
  double ascent{0.0};
  double descent{0.0};
  double leading{0.0};
  double cap_height{0.0};
  double x_height{0.0};
  double stem_v{0.0};
  std::vector<double> font_bbox;
  int flags{0};
  double italic_angle{0.0};
  
  // Embedded font file data
  Value font_file;  // Can be FontFile, FontFile2, or FontFile3
  uint32_t font_file_length{0};

  ~FontDescriptor() = default;
};

// Base font class for all font types
struct BaseFont {
  std::string subtype;  // Font type (Type1, TrueType, etc.)
  std::string base_font;  // PostScript name of the font
  std::string encoding;  // Font encoding
  FontDescriptor* descriptor{nullptr};  // Optional font descriptor
  
  // Font widths
  std::vector<int> widths;
  int first_char{0};
  int last_char{0};
  
  virtual ~BaseFont() {
    if (descriptor) {
      delete descriptor;
      descriptor = nullptr;
    }
  }
};

// Result types declared before they are used
struct DecodedStream {
  std::vector<uint8_t> data;
  bool success{false};
  std::string error;
};

struct ResolvedObject {
  bool success{false};
  std::string error;
  Value value;
  
  ResolvedObject() = default;
  
  // Implement move operations
  ResolvedObject(ResolvedObject&& other) noexcept
    : success(other.success)
    , error(std::move(other.error))
    , value(std::move(other.value)) {
    other.success = false;
  }
  
  ResolvedObject& operator=(ResolvedObject&& other) noexcept {
    if (this != &other) {
      success = other.success;
      error = std::move(other.error);
      value = std::move(other.value);
      other.success = false;
    }
    return *this;
  }
  
  // Delete copy operations
  ResolvedObject(const ResolvedObject&) = delete;
  ResolvedObject& operator=(const ResolvedObject&) = delete;
};

struct Pdf;  // Forward declare
struct Page;  // Forward declare

struct PageContent {
  std::vector<uint8_t> data;
  bool success{false};
  std::string error;
};

struct Page {
  uint32_t object_number{0};
  uint32_t page_number{0};
  Dictionary resources;
  std::vector<Value> contents;
  std::vector<double> media_box;  // [left, bottom, right, top]
  std::vector<double> crop_box;
  double rotate{0.0};
  std::map<std::string, std::unique_ptr<BaseFont>> fonts;  // Font resources
  
  PageContent load_contents(const Pdf& pdf) const;
};

struct DocumentCatalog {
  uint32_t object_number{0};
  uint32_t pages_count{0};
  std::vector<Page> pages;
  std::string version;
  Dictionary names;
  Dictionary outlines;
  Dictionary acro_form;
};

struct XRef {
  uint64_t offset{0};
  uint16_t generation{65535};
  bool use{false};
};

struct XRefSection {
  std::vector<XRef> xrefs;
  uint32_t start_object_id{0};
  uint32_t num_objectsid{0};
};

// Now we can define Pdf after its dependencies
struct Pdf {
  int version_major{1};
  int version_minor{5};

  uint32_t size{0};
  uint32_t root{0};
  uint32_t info{0};
  std::string id;
  uint32_t encrypt{0};
  uint32_t prev{0};
  Dictionary trailer;

  std::vector<XRefSection> xref_sections;

  const uint8_t* data{nullptr};
  size_t data_size{0};

  DocumentCatalog catalog;
  
  bool load_document_structure();
  const Page* get_page(uint32_t page_number) const;
  ResolvedObject load_object(uint32_t obj_num, uint16_t gen_num) const;

  static std::unique_ptr<BaseFont> parse_font(const Pdf& pdf, const Value& font_val);
  static std::unique_ptr<FontDescriptor> parse_font_descriptor(const Pdf& pdf, const Value& font_dict);
  static bool parse_font_resources(const Pdf& pdf, Page& page, const Dictionary& resources);

  private:
    bool parse_font_resources(Page& page, const Dictionary& resources);
    std::unique_ptr<BaseFont> parse_font(const Value& font_val);
    std::unique_ptr<FontDescriptor> parse_font_descriptor(const Value& font_dict);
};

// Function declarations
bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf);
ResolvedObject resolve_reference(const Pdf& pdf, uint32_t obj_num, uint16_t gen_num);
bool parse_indirect_object(StreamReader& sr, detail::Parser& parser, Value* out_value);
DecodedStream decode_stream(const Pdf& pdf, const Value& stream_obj);

// Add parse_string function declaration
bool parse_string(StreamReader& sr, detail::Parser& parser, std::string* out_str);

// Stream filter processing namespace declaration remains the same
namespace filters {

struct DecodeParams {
  // Common parameters
  int predictor{1};
  int colors{1};
  int bits_per_component{8};
  int columns{1};
  
  // LZWDecode parameters
  bool early_change{true};

  // ASCII85Decode parameters
  bool ascii85_strip_terminator{true};
};

DecodedStream decode_flate(const uint8_t* data, size_t size, const DecodeParams& params);
DecodedStream decode_ascii85(const uint8_t* data, size_t size, const DecodeParams& params);
DecodedStream decode_lzw(const uint8_t* data, size_t size, const DecodeParams& params);
DecodeParams parse_decode_params(const Dictionary& params);

} // namespace filters

} // namespace nanopdf
