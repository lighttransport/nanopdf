// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstddef.h"
#include "nanocstdint.h"
#include "nanocstring.h"
using namespace nanostl;
#else
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#endif

#include "stream-reader.hh"

#if defined(NANOPDF_USE_STB_TRUETYPE)
#include "stb_truetype.h"
#endif

namespace nanopdf {

#if 0
struct Value
{
  std::vector<Value> _arr;
  std::map<std::string, Value> _dict;

  bool _b{false};
  std::vector<uint8_t> _s{false}; // string in bytes
  double _v{0.0}; // int, real
  std::string _n; // Name(Identifier)
};
#endif
struct Value;

using Dictionary = std::map<std::string, Value>;

struct Pdf;

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

bool parse_from_memory(const uint8_t* addr, const size_t size);

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

// Color space types
enum class ColorSpaceType {
  DeviceGray,
  DeviceRGB,
  DeviceCMYK,
  CalGray,
  CalRGB,
  Lab,
  ICCBased,
  Indexed,
  Pattern,
  Separation,
  DeviceN
};

// Color space structure
struct ColorSpace {
  ColorSpaceType type;
  std::string name;

  // For Indexed color spaces
  std::unique_ptr<ColorSpace> base_color_space;
  int hival{255};  // Maximum valid index value
  std::vector<uint8_t> lookup_table;

  // For CalRGB/CalGray
  std::vector<double> white_point;  // [X Y Z]
  std::vector<double> black_point;  // [X Y Z]
  std::vector<double> gamma;        // [R G B] or [Gray]
  std::vector<double> matrix;       // 3x3 transformation matrix

  // For ICCBased
  int num_components{0};
  std::vector<uint8_t> icc_profile_data;

  ColorSpace() : type(ColorSpaceType::DeviceGray) {}
  explicit ColorSpace(ColorSpaceType t) : type(t) {}
};

// Image XObject structure
struct ImageXObject {
  // Required entries
  int width{0};
  int height{0};
  int bits_per_component{8};

  // Color space
  ColorSpace color_space;

  // Optional entries
  bool image_mask{false};
  std::vector<double> decode;  // Decode array
  bool interpolate{false};

  // Alternate image for masked images
  Value alternate;

  // Soft mask
  Value soft_mask;

  // Image data (after applying filters)
  std::vector<uint8_t> data;

  ImageXObject() = default;
};

// Forward declarations
struct BaseFont;
struct Type0Font;
struct Type3Font;

// PDF Form field types
enum class FieldType {
  Button,    // Btn
  Text,      // Tx
  Choice,    // Ch
  Signature  // Sig
};

// Text rendering modes
enum class TextRenderingMode {
  Fill = 0,           // Fill text
  Stroke = 1,         // Stroke text
  FillAndStroke = 2,  // Fill then stroke
  Invisible = 3,      // Neither fill nor stroke (invisible)
  FillAndClip = 4,    // Fill and add to clipping path
  StrokeAndClip = 5,  // Stroke and add to clipping path
  FillStrokeAndClip = 6, // Fill, stroke and add to clipping path
  Clip = 7            // Add to clipping path
};

// Text state for content stream processing
struct TextState {
  // Text positioning
  double text_matrix[6] = {1, 0, 0, 1, 0, 0};  // [a b c d e f]
  double line_matrix[6] = {1, 0, 0, 1, 0, 0};
  double text_x = 0.0;
  double text_y = 0.0;

  // Text formatting
  double char_spacing = 0.0;    // Tc
  double word_spacing = 0.0;    // Tw
  double horizontal_scaling = 100.0; // Tz (percentage)
  double leading = 0.0;          // TL
  double font_size = 0.0;        // Tf size
  double text_rise = 0.0;        // Ts
  TextRenderingMode render_mode = TextRenderingMode::Fill; // Tr

  // Current font
  std::string font_name;
  BaseFont* current_font = nullptr;

  // Text accumulator
  std::string current_text;

  void reset() {
    // Reset to default text state
    std::fill(text_matrix, text_matrix + 6, 0.0);
    text_matrix[0] = text_matrix[3] = 1.0;
    std::copy(text_matrix, text_matrix + 6, line_matrix);
    text_x = text_y = 0.0;
    char_spacing = word_spacing = 0.0;
    horizontal_scaling = 100.0;
    leading = font_size = text_rise = 0.0;
    render_mode = TextRenderingMode::Fill;
    font_name.clear();
    current_font = nullptr;
    current_text.clear();
  }
};

// CMap structure for Type0 fonts
struct CMap {
  std::string name;
  std::string registry;
  std::string ordering;
  int supplement = 0;

  // Code to Unicode mappings
  std::map<uint32_t, uint32_t> code_to_unicode;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> range_mappings;

  uint32_t map_code_to_unicode(uint32_t code) const {
    auto it = code_to_unicode.find(code);
    if (it != code_to_unicode.end()) {
      return it->second;
    }

    // Check range mappings
    for (const auto& range : range_mappings) {
      if (code >= range.first.first && code <= range.first.second) {
        return range.second + (code - range.first.first);
      }
    }

    return code; // Return as-is if no mapping found
  }
};

// Font substitution entry
struct FontSubstitution {
  std::string original_name;
  std::string substitute_name;
  std::string substitute_path;
  bool is_system_font = false;
};

// Annotation types
enum class AnnotationType {
  Text,         // Text annotation (note)
  Link,         // Hyperlink
  FreeText,     // Free text annotation
  Line,         // Line annotation
  Square,       // Square annotation
  Circle,       // Circle annotation
  Polygon,      // Polygon annotation
  PolyLine,     // Polyline annotation
  Highlight,    // Text highlight
  Underline,    // Text underline
  Squiggly,     // Squiggly underline
  StrikeOut,    // Text strikeout
  Stamp,        // Rubber stamp
  Caret,        // Caret annotation
  Ink,          // Freehand drawing
  Popup,        // Popup annotation
  FileAttachment, // File attachment
  Sound,        // Sound annotation
  Movie,        // Movie annotation
  Widget,       // Form field widget
  Screen,       // Screen annotation
  PrinterMark,  // Printer mark
  TrapNet,      // Trap network
  Watermark,    // Watermark
  ThreeD,       // 3D annotation
  Redact        // Redaction annotation
};

// Annotation flags
enum class AnnotationFlags : uint32_t {
  Invisible = 0x0001,      // Annotation is invisible
  Hidden = 0x0002,         // Annotation is hidden
  Print = 0x0004,          // Print annotation
  NoZoom = 0x0008,         // Don't scale annotation
  NoRotate = 0x0010,       // Don't rotate annotation
  NoView = 0x0020,         // Don't display or print
  ReadOnly = 0x0040,       // Don't allow interaction
  Locked = 0x0080,         // Don't allow changes
  ToggleNoView = 0x0100,   // Toggle visibility
  LockedContents = 0x0200  // Don't allow content changes
};

// Annotation border style
struct AnnotationBorder {
  double width = 1.0;
  enum Style {
    Solid,
    Dashed,
    Beveled,
    Inset,
    Underline
  } style = Solid;
  std::vector<double> dash_pattern;

  AnnotationBorder() = default;
};

// Annotation appearance states
enum class AppearanceState {
  Normal,    // N - Normal appearance
  Rollover,  // R - Rollover appearance
  Down       // D - Down (pressed) appearance
};

// Base annotation class
struct Annotation {
  AnnotationType type;
  std::vector<double> rect;  // [x1, y1, x2, y2]
  std::string contents;      // Text content
  std::string name;          // Annotation name (NM)
  std::string modified_date; // Last modified date (M)
  uint32_t flags = 0;        // Annotation flags (F)

  // Appearance
  Dictionary appearance_streams;  // AP dictionary
  AppearanceState current_state = AppearanceState::Normal;

  // Visual properties
  std::vector<double> color;     // C - Color
  AnnotationBorder border;        // Border or BS

  // Page reference
  uint32_t page_ref = 0;

  virtual ~Annotation() = default;

  Annotation() : type(AnnotationType::Text) {}
  explicit Annotation(AnnotationType t) : type(t) {}
};

// Text annotation (sticky note)
struct TextAnnotation : public Annotation {
  std::string state;      // State (e.g., "Marked", "Unmarked")
  std::string state_model; // State model (e.g., "Review")
  std::string icon;       // Icon name (e.g., "Comment", "Note")
  bool open = false;      // Initially open

  TextAnnotation() : Annotation(AnnotationType::Text) {
    icon = "Note";
  }
};

// Link annotation
struct LinkAnnotation : public Annotation {
  enum ActionType {
    GoTo,        // Go to destination in document
    GoToR,       // Go to destination in another document
    Launch,      // Launch application
    URI,         // Open URI
    Named,       // Execute named action
    JavaScript   // Execute JavaScript
  };

  ActionType action_type = GoTo;
  std::string uri;           // For URI actions
  Dictionary destination;    // For GoTo actions
  std::vector<double> border_style;  // Link border

  LinkAnnotation() : Annotation(AnnotationType::Link) {}
};

// Highlight/markup annotation
struct MarkupAnnotation : public Annotation {
  std::vector<std::vector<double>> quad_points;  // QuadPoints for text region
  std::string subject;
  std::string title;     // T - Title (author)
  double opacity = 1.0;  // CA - Opacity

  MarkupAnnotation(AnnotationType t) : Annotation(t) {}
};

// Free text annotation
struct FreeTextAnnotation : public Annotation {
  std::string default_appearance;  // DA - Default appearance string
  int quadding = 0;                // Q - Justification (0=left, 1=center, 2=right)
  std::string default_style;       // DS - Default style string
  std::vector<double> callout_line; // CL - Callout line

  FreeTextAnnotation() : Annotation(AnnotationType::FreeText) {}
};

// Widget annotation for form fields
struct WidgetAnnotation : public Annotation {
  FieldType field_type;
  std::string field_name;    // T - Field name
  std::string field_value;   // V - Field value
  std::string default_value; // DV - Default value
  uint32_t field_flags = 0;  // Ff - Field flags

  // Appearance characteristics
  Dictionary mk_dict;  // MK - Appearance characteristics

  WidgetAnnotation() : Annotation(AnnotationType::Widget), field_type(FieldType::Text) {}
};

// Form field flags
enum class FormFieldFlags : uint32_t {
  ReadOnly = 0x00000001,
  Required = 0x00000002,
  NoExport = 0x00000004,

  // Text field flags
  Multiline = 0x00001000,
  Password = 0x00002000,
  FileSelect = 0x00100000,
  DoNotSpellCheck = 0x00400000,
  DoNotScroll = 0x00800000,
  Comb = 0x01000000,
  RichText = 0x02000000,

  // Button field flags
  NoToggleToOff = 0x00004000,
  Radio = 0x00008000,
  Pushbutton = 0x00010000,
  RadiosInUnison = 0x02000000,

  // Choice field flags
  Combo = 0x00020000,
  Edit = 0x00040000,
  Sort = 0x00080000,
  MultiSelect = 0x00200000,
  CommitOnSelChange = 0x04000000
};

// Base form field
struct FormField {
  FieldType type;
  std::string partial_name;  // T - Partial field name
  std::string full_name;     // Fully qualified field name
  std::string alternate_name; // TU - User-friendly name
  std::string mapping_name;  // TM - Export mapping name
  uint32_t flags = 0;        // Ff - Field flags

  Value field_value;         // V - Field value
  Value default_value;       // DV - Default value

  // Widget annotations associated with this field
  std::vector<std::unique_ptr<WidgetAnnotation>> widgets;

  // Parent/children relationship
  FormField* parent = nullptr;
  std::vector<std::unique_ptr<FormField>> children;

  virtual ~FormField() = default;
  FormField(FieldType t) : type(t) {}
};

// Text field
struct TextField : public FormField {
  int max_length = 0;        // MaxLen - Maximum length
  std::string default_appearance; // DA - Default appearance
  int quadding = 0;          // Q - Justification

  TextField() : FormField(FieldType::Text) {}
};

// Button field (checkbox, radio, pushbutton)
struct ButtonField : public FormField {
  enum ButtonType {
    PushButton,
    CheckBox,
    RadioButton
  };

  ButtonType button_type;
  std::string normal_caption;  // CA - Normal caption
  std::string rollover_caption; // RC - Rollover caption
  std::string down_caption;     // AC - Down caption

  ButtonField() : FormField(FieldType::Button), button_type(CheckBox) {}
};

// Choice field (listbox, combobox)
struct ChoiceField : public FormField {
  std::vector<std::string> options;     // Opt - Options
  std::vector<int> selected_indices;    // I - Selected indices
  std::vector<std::string> top_index;   // TI - Top visible index

  ChoiceField() : FormField(FieldType::Choice) {}
};

// Signature field data structure
struct SignatureField {
  std::string name;                    // Field name (T)
  FieldType field_type{FieldType::Signature}; // Field type (FT)
  std::vector<double> rect;            // Field rectangle [x1 y1 x2 y2] (Rect)
  uint32_t page_ref{0};                // Page reference (P)
  uint32_t flags{0};                   // Field flags (F)
  
  // Signature-specific data
  Dictionary signature_dict;           // Signature dictionary (V)
  Dictionary lock_dict;                // Signature lock dictionary (Lock)
  
  // Signature validation info
  bool is_signed{false};
  std::string signing_reason;
  std::string signing_location;
  std::string signing_contact_info;
  std::string signing_date;
  
  SignatureField() = default;
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
  std::string subtype;                  // Font type (Type1, TrueType, etc.)
  std::string base_font;                // PostScript name of the font
  std::string encoding;                 // Font encoding
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

// Type0 (CID) font structure
struct Type0Font : public BaseFont {
  std::string registry;
  std::string ordering;
  int supplement = 0;

  // CMap for character encoding
  CMap encoding_cmap;

  // ToUnicode CMap for Unicode conversion
  CMap to_unicode_cmap;

  // CIDToGIDMap for glyph mapping
  std::vector<uint16_t> cid_to_gid_map;

  // Descendant font (CIDFontType0 or CIDFontType2)
  std::unique_ptr<BaseFont> descendant_font;

  Type0Font() {
    subtype = "Type0";
  }
};

// Type3 font structure
struct Type3Font : public BaseFont {
  std::vector<double> font_bbox;
  std::vector<double> font_matrix = {0.001, 0, 0, 0.001, 0, 0};

  // CharProcs dictionary containing glyph procedures
  Dictionary char_procs;

  // Resources for glyph procedures
  Dictionary resources;

  Type3Font() {
    subtype = "Type3";
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
      : success(other.success),
        error(std::move(other.error)),
        value(std::move(other.value)) {
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

struct Pdf;   // Forward declare
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
  std::vector<std::unique_ptr<Annotation>> annotations;  // Page annotations

  PageContent load_contents(const Pdf& pdf) const;
};

// Phase 4: Document Structure

// Outline (Bookmarks) structures
enum class OutlineAction {
  GoTo,     // Go to page
  GoToR,    // Go to remote document
  URI,      // Open URI
  Launch    // Launch application
};

struct OutlineItem {
  std::string title;                       // Bookmark title
  std::vector<std::unique_ptr<OutlineItem>> children;  // Child bookmarks

  // Destination
  OutlineAction action_type = OutlineAction::GoTo;
  uint32_t dest_page = 0;                 // For GoTo action
  std::vector<double> dest_position;      // [x, y, zoom] or other parameters
  std::string uri;                        // For URI action
  std::string file;                       // For GoToR/Launch actions

  // Appearance
  std::vector<double> color;              // RGB color (0-1)
  bool italic = false;                    // Text style flags
  bool bold = false;

  // State
  bool open = true;                       // Whether children are visible
  int count = 0;                          // Number of visible descendants
};

// Page labels for custom page numbering
enum class PageLabelStyle {
  None,           // No style
  DecimalArabic,  // 1, 2, 3...
  UppercaseRoman, // I, II, III...
  LowercaseRoman, // i, ii, iii...
  UppercaseLetters, // A, B, C...
  LowercaseLetters  // a, b, c...
};

struct PageLabel {
  PageLabelStyle style = PageLabelStyle::DecimalArabic;
  std::string prefix;    // Label prefix
  uint32_t start_value = 1;  // Starting value for numbering
};

struct PageLabels {
  std::map<uint32_t, PageLabel> labels;  // Page index -> label definition

  std::string get_label(uint32_t page_index) const;
};

// Named destinations
struct NamedDestination {
  std::string name;
  uint32_t page_number = 0;
  std::vector<double> position;  // [x, y, zoom] or other view parameters
  std::string fit_type;  // Fit, FitH, FitV, FitR, etc.
};

// Document information dictionary
struct DocumentInfo {
  std::string title;
  std::string author;
  std::string subject;
  std::string keywords;
  std::string creator;     // Application that created the original document
  std::string producer;    // PDF producer
  std::string creation_date;   // D:YYYYMMDDHHmmSSOHH'mm
  std::string mod_date;        // Modification date
  std::string trapped;         // Trapping state: True, False, Unknown

  // Custom metadata fields
  std::map<std::string, std::string> custom;
};

// XMP metadata
struct XMPMetadata {
  std::string raw_xml;  // Raw XMP XML data

  // Common extracted fields
  std::string dc_title;
  std::string dc_creator;
  std::string dc_description;
  std::vector<std::string> dc_subject;  // Keywords
  std::string xmp_create_date;
  std::string xmp_modify_date;
  std::string xmp_metadata_date;
  std::string pdf_producer;
  std::string pdf_version;

  bool parse_xml(const std::string& xml);
};

// Phase 5: Security and Encryption

enum class EncryptionAlgorithm {
  None,
  RC4_40,      // PDF 1.1 - RC4 with 40-bit key
  RC4_128,     // PDF 1.4 - RC4 with 128-bit key
  AES_128,     // PDF 1.6 - AES with 128-bit key
  AES_256,     // PDF 1.7 ext 3 - AES with 256-bit key
};

enum class PermissionFlags : uint32_t {
  PrintDocument = 0x00000004,        // Bit 3
  ModifyDocument = 0x00000008,       // Bit 4
  ExtractContent = 0x00000010,       // Bit 5
  AddAnnotations = 0x00000020,       // Bit 6
  FillForms = 0x00000100,            // Bit 9
  ExtractForAccessibility = 0x00000200, // Bit 10
  AssembleDocument = 0x00000400,     // Bit 11
  PrintHighQuality = 0x00000800      // Bit 12
};

struct EncryptionDictionary {
  std::string filter;           // Security handler name (usually "Standard")
  int v = 0;                    // Encryption algorithm version (1-5)
  int length = 40;              // Key length in bits
  int r = 2;                    // Revision number
  std::string o;                // Owner password hash (32 bytes)
  std::string u;                // User password hash (32 bytes)
  uint32_t p = 0;              // Permission flags
  bool encrypt_metadata = true; // Whether to encrypt metadata

  // For AES encryption (V=4)
  std::string cf;               // Crypt filter
  std::string stm_f;            // Stream filter
  std::string str_f;            // String filter

  // For AES-256 (V=5)
  std::string oe;               // Owner encryption key
  std::string ue;               // User encryption key
  std::string perms;            // Encrypted permissions
};

struct SecurityHandler {
  EncryptionAlgorithm algorithm = EncryptionAlgorithm::None;
  EncryptionDictionary encrypt_dict;
  std::vector<uint8_t> encryption_key;
  bool authenticated = false;
  bool is_owner = false;

  // Password processing
  bool authenticate_user_password(const std::string& password);
  bool authenticate_owner_password(const std::string& password);

  // Encryption/decryption
  std::vector<uint8_t> decrypt_string(const std::string& str, uint32_t obj_num, uint16_t gen_num);
  std::vector<uint8_t> decrypt_stream(const std::vector<uint8_t>& data, uint32_t obj_num, uint16_t gen_num);
  std::vector<uint8_t> encrypt_string(const std::string& str, uint32_t obj_num, uint16_t gen_num);
  std::vector<uint8_t> encrypt_stream(const std::vector<uint8_t>& data, uint32_t obj_num, uint16_t gen_num);

  // Key generation
  void compute_encryption_key(const std::string& password, bool is_owner_password);
  std::vector<uint8_t> compute_object_key(uint32_t obj_num, uint16_t gen_num);
};

// PDF password padding
extern const uint8_t PDF_PADDING[32];

// Security helper functions
EncryptionDictionary parse_encryption_dictionary(const Pdf& pdf, const Dictionary& encrypt_dict);
SecurityHandler create_security_handler(const Pdf& pdf);
std::vector<uint8_t> pad_password(const std::string& password);
std::vector<uint8_t> compute_owner_key(const std::string& owner_password, const std::string& user_password, int key_length, int revision);
std::vector<uint8_t> compute_user_key(const std::string& user_password, const std::vector<uint8_t>& owner_key, uint32_t permissions, const std::string& file_id, int key_length, int revision);
bool verify_user_password(const std::string& password, const EncryptionDictionary& encrypt_dict, const std::string& file_id);
bool verify_owner_password(const std::string& password, const EncryptionDictionary& encrypt_dict, const std::string& file_id);

struct DocumentCatalog {
  uint32_t object_number{0};
  uint32_t pages_count{0};
  std::vector<Page> pages;
  std::string version;
  Dictionary names;
  Dictionary outlines;
  Dictionary acro_form;
  std::vector<SignatureField> signature_fields;  // Parsed signature fields
  std::vector<std::unique_ptr<FormField>> form_fields;  // All form fields

  // Phase 4 features
  std::unique_ptr<OutlineItem> outline_root;  // Document outline (bookmarks)
  PageLabels page_labels;                      // Custom page numbering
  std::map<std::string, NamedDestination> named_destinations;  // Named destinations
  DocumentInfo document_info;                  // Document information
  XMPMetadata xmp_metadata;                   // XMP metadata
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
  SecurityHandler security;

  bool load_document_structure();
  const Page* get_page(uint32_t page_number) const;
  ResolvedObject load_object(uint32_t obj_num, uint16_t gen_num) const;

  static std::unique_ptr<BaseFont> parse_font(const Pdf& pdf,
                                              const Value& font_val);
  static std::unique_ptr<FontDescriptor> parse_font_descriptor(
      const Pdf& pdf, const Value& font_dict);
  static bool parse_font_resources(const Pdf& pdf, Page& page,
                                   const Dictionary& resources);
  
  // Signature field parsing functions
  bool parse_signature_fields();
  static bool parse_acro_form_fields(const Pdf& pdf, const Dictionary& acro_form, 
                                     std::vector<SignatureField>& signature_fields);
  static SignatureField parse_signature_field(const Pdf& pdf, const Dictionary& field_dict);

 private:
  bool parse_font_resources(Page& page, const Dictionary& resources);
  std::unique_ptr<BaseFont> parse_font(const Value& font_val);
  std::unique_ptr<FontDescriptor> parse_font_descriptor(const Value& font_dict);
};

class Parser;

// Function declarations
bool parse_from_memory(const uint8_t* addr, const size_t size, Pdf* out_pdf);
ResolvedObject resolve_reference(const Pdf& pdf, uint32_t obj_num,
                                 uint16_t gen_num);
bool parse_indirect_object(StreamReader& sr, Parser& parser, Value* out_value);
DecodedStream decode_stream(const Pdf& pdf, const Value& stream_obj);

// Add parse_string function declaration
bool parse_string(StreamReader& sr, Parser& parser, std::string* out_str);

// Color space and image parsing functions
ColorSpace parse_color_space(const Pdf& pdf, const Value& cs_value);
ImageXObject parse_image_xobject(const Pdf& pdf, const Value& stream_value);
std::map<std::string, ImageXObject> parse_xobject_resources(const Pdf& pdf, const Dictionary& resources);

// Text extraction functions
std::string extract_text_from_page(const Pdf& pdf, const Page& page);

// Font parsing functions
std::unique_ptr<BaseFont> parse_type0_font(const Pdf& pdf, const Dictionary& font_dict);
std::unique_ptr<BaseFont> parse_type3_font(const Pdf& pdf, const Dictionary& font_dict);

// Annotation and form parsing functions
std::unique_ptr<Annotation> parse_annotation(const Pdf& pdf, const Value& annot_val);
void parse_page_annotations(const Pdf& pdf, Page& page, const Dictionary& page_dict);
std::unique_ptr<FormField> parse_form_field(const Pdf& pdf, const Dictionary& field_dict);
void parse_acro_form(const Pdf& pdf, DocumentCatalog& catalog);
std::string generate_annotation_appearance(const Annotation& annot);

// Phase 4: Document Structure - Function declarations
std::unique_ptr<OutlineItem> parse_outline_item(const Pdf& pdf, const Dictionary& outline_dict);
void parse_document_outline(const Pdf& pdf, DocumentCatalog& catalog);
void parse_page_labels(const Pdf& pdf, DocumentCatalog& catalog);
void parse_named_destinations(const Pdf& pdf, DocumentCatalog& catalog);
void parse_document_info(const Pdf& pdf, DocumentCatalog& catalog);
void parse_xmp_metadata(const Pdf& pdf, DocumentCatalog& catalog);

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

DecodedStream decode_flate(const uint8_t* data, size_t size,
                           const DecodeParams& params);
DecodedStream decode_ascii85(const uint8_t* data, size_t size,
                             const DecodeParams& params);
DecodedStream decode_lzw(const uint8_t* data, size_t size,
                         const DecodeParams& params);
DecodedStream decode_jbig2(const uint8_t* data, size_t size,
                           const DecodeParams& params);  // JBIG2 stub
DecodedStream decode_runlength(const uint8_t* data, size_t size,
                               const DecodeParams& params);
DecodedStream decode_dct(const uint8_t* data, size_t size,
                        const DecodeParams& params);
DecodeParams parse_decode_params(const Dictionary& params);

}  // namespace filters

}  // namespace nanopdf
