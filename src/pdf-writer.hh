// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Light Transport Entertainment Inc.

#ifndef NANOPDF_PDF_WRITER_HH_
#define NANOPDF_PDF_WRITER_HH_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nanopdf {

// Forward-declare ErrorKind if nanopdf.hh was not included first
#ifndef NANOPDF_HH_INCLUDED
enum class ErrorKind { None, Malformed, Unsupported, Encrypted, IOError, Internal };
#endif

/// Result of a write operation
struct WriteResult {
  bool success = false;
  std::string error;
  ErrorKind kind{ErrorKind::None};
  size_t bytes_written = 0;
};

/// PDF version
enum class PdfVersion {
  v1_4,  // PDF 1.4 (Acrobat 5) - default, widely compatible
  v1_5,  // PDF 1.5 (Acrobat 6) - object streams, cross-reference streams
  v1_6,  // PDF 1.6 (Acrobat 7) - AES encryption, embedded files
  v1_7,  // PDF 1.7 (Acrobat 8) - ISO 32000-1
  v2_0,  // PDF 2.0 (ISO 32000-2)
};

/// Signature filter type
enum class SignatureFilter {
  AdobePPKLite,    // Adobe.PPKLite (most common)
  EntrustPPKEF,    // Entrust.PPKEF
  CICISignIt,      // CICI.SignIt
  VeriSignPPKVS,   // VeriSign.PPKVS
};

/// Signature subfilter (encoding method)
enum class SignatureSubFilter {
  Pkcs7Detached,   // adbe.pkcs7.detached (most common)
  Pkcs7Sha1,       // adbe.pkcs7.sha1
  EtsiCadesDetached, // ETSI.CAdES.detached (PAdES)
  EtsiRfc3161,     // ETSI.RFC3161 (timestamp)
};

/// Document MDP (Modification Detection and Prevention) permissions
enum class MdpPermissions {
  NoChanges = 1,          // No changes allowed
  FormFillAndSign = 2,    // Form fill-in and digital signatures
  AnnotateFormFillSign = 3, // Annotations, form fill-in, and digital signatures
};

// ============================================================
// Timestamp Support
// ============================================================

/// Timestamp configuration for digital signatures
struct TimestampConfig {
  std::string server_url;            // RFC 3161 timestamp server URL
  std::string username;              // Optional: HTTP Basic Auth username
  std::string password;              // Optional: HTTP Basic Auth password
  int timeout_ms = 30000;            // Request timeout in milliseconds
  bool embed_in_signature = true;    // Embed timestamp in signature
};

// ============================================================
// Encryption Support
// ============================================================

/// Encryption algorithm (skip if already defined in nanopdf.hh)
#ifndef NANOPDF_HH_INCLUDED
enum class EncryptionAlgorithm {
  None,           // No encryption
  RC4_40,         // RC4 40-bit (PDF 1.1+, weak, deprecated)
  RC4_128,        // RC4 128-bit (PDF 1.4+, deprecated)
  AES_128,        // AES 128-bit (PDF 1.5+, recommended minimum)
  AES_256,        // AES 256-bit (PDF 1.7 Extension Level 3 / PDF 2.0)
};
#endif

/// User access permissions (bit flags)
/// These control what operations are allowed when opening with user password
struct UserPermissions {
  bool allow_print = true;               // Bit 3: Print document
  bool allow_modify = false;             // Bit 4: Modify contents
  bool allow_copy = true;                // Bit 5: Copy text/graphics
  bool allow_annotate = true;            // Bit 6: Add/modify annotations
  bool allow_fill_forms = true;          // Bit 9: Fill form fields (PDF 1.5+)
  bool allow_accessibility = true;       // Bit 10: Extract for accessibility (PDF 1.5+)
  bool allow_assemble = false;           // Bit 11: Assemble document (PDF 1.5+)
  bool allow_print_high_quality = true;  // Bit 12: High quality print (PDF 1.5+)

  /// Convert to PDF permission flags integer
  int32_t to_flags() const;

  /// Create from PDF permission flags integer
  static UserPermissions from_flags(int32_t flags);

  /// All permissions allowed
  static UserPermissions all_allowed();

  /// Most restrictive (view only)
  static UserPermissions view_only();
};

/// Encryption configuration
struct EncryptionConfig {
  EncryptionAlgorithm algorithm = EncryptionAlgorithm::AES_128;

  /// User password (required to open document if set)
  /// Empty string means no user password required
  std::string user_password;

  /// Owner password (required to change security settings)
  /// Must be set for encryption to work
  std::string owner_password;

  /// User access permissions
  UserPermissions permissions;

  /// Encrypt metadata (PDF 1.5+)
  /// If false, metadata remains in plain text for indexing
  bool encrypt_metadata = true;
};

/// Signature field configuration
struct SignatureFieldConfig {
  std::string name = "Signature1";     // Field name
  int page = 0;                        // Page index (0-based)
  double x = 0, y = 0;                 // Position (bottom-left)
  double width = 200, height = 50;     // Size

  // Appearance settings
  bool visible = true;                 // If false, creates invisible signature
  std::string reason;                  // Reason for signing
  std::string location;                // Signing location
  std::string contact_info;            // Contact information

  // Signature type
  SignatureFilter filter = SignatureFilter::AdobePPKLite;
  SignatureSubFilter subfilter = SignatureSubFilter::Pkcs7Detached;

  // Certification signature options
  bool is_certification = false;       // True for certification (first) signature
  MdpPermissions mdp_permissions = MdpPermissions::AnnotateFormFillSign;
};

/// Signature placeholder info (returned after adding signature field)
struct SignaturePlaceholder {
  std::string field_name;              // Name of the signature field
  size_t contents_offset = 0;          // Offset of /Contents hex string in PDF
  size_t contents_length = 0;          // Length reserved for signature (in bytes)
  std::vector<size_t> byte_range;      // ByteRange array [offset1, len1, offset2, len2]
  size_t byte_range_offset = 0;        // Offset of ByteRange array in PDF (for updating)
};

/// Callback type for digital signing.
/// Receives the data to be signed (the ByteRange portions of the PDF),
/// must return a DER-encoded PKCS#7/CMS signature.
/// Return an empty vector to indicate signing failure.
using SigningCallback = std::function<std::vector<uint8_t>(
    const std::vector<uint8_t>& data_to_sign)>;

/// Apply a digital signature to a PDF prepared with write_for_signing().
///
/// Usage:
///   PdfWriter writer;
///   writer.add_signature_field(config);
///   std::vector<uint8_t> pdf_bytes;
///   writer.write_for_signing(pdf_bytes);
///   auto placeholders = writer.get_signature_placeholders();
///   bool ok = apply_signature(pdf_bytes, placeholders[0], my_signing_callback);
///
/// @param pdf_data      The PDF bytes from write_for_signing() (modified in-place)
/// @param placeholder   The signature placeholder to fill
/// @param sign_fn       Callback that produces a PKCS#7 signature from input data
/// @return WriteResult  success/error
WriteResult apply_signature(std::vector<uint8_t>& pdf_data,
                            const SignaturePlaceholder& placeholder,
                            const SigningCallback& sign_fn);

/// Page size in PDF points (1/72 inch)
struct PageSize {
  double width;
  double height;

  static PageSize A4() { return {595.276, 841.890}; }
  static PageSize A4Landscape() { return {841.890, 595.276}; }
  static PageSize Letter() { return {612.0, 792.0}; }
  static PageSize LetterLandscape() { return {792.0, 612.0}; }
  static PageSize FromPixels(int w, int h, double dpi = 72.0) {
    return {w * 72.0 / dpi, h * 72.0 / dpi};
  }
};

/// Image format detected from file or data
enum class ImageFormat { Unknown, JPEG, PNG, BMP, GIF, TGA };

/// Image compression method for PDF output
enum class ImageCompression {
  Auto,       // Automatic: JPEG passthrough for JPEG, FlateDecode for others
  Flate,      // FlateDecode (zlib) - lossless, good for graphics
  DCT,        // DCTDecode (JPEG) - lossy, good for photos
  CCITTFax,   // CCITTFaxDecode Group 4 - for monochrome/scanned documents
};

/// Image data container
struct ImageData {
  std::vector<uint8_t> raw_data;    // Decoded pixel data (RGB or RGBA)
  std::vector<uint8_t> encoded_data; // Original encoded data (for JPEG passthrough)
  int width = 0;
  int height = 0;
  int channels = 0;  // 1=Gray, 3=RGB, 4=RGBA
  ImageFormat format = ImageFormat::Unknown;
  bool is_jpeg = false;  // If true, use DCTDecode passthrough

  /// Load image from file
  static ImageData FromFile(const std::string& path);

  /// Load image from memory
  static ImageData FromMemory(const uint8_t* data, size_t size);

  /// Check if valid
  bool valid() const { return width > 0 && height > 0 && channels > 0; }

  /// Check if image has alpha channel
  bool has_alpha() const { return channels == 4; }

  /// Extract alpha channel as grayscale data (only valid if has_alpha())
  std::vector<uint8_t> extract_alpha() const;

  /// Extract RGB data without alpha (only valid if has_alpha())
  std::vector<uint8_t> extract_rgb() const;
};

/// Soft mask configuration for explicit image masks
struct SoftMaskConfig {
  std::vector<uint8_t> mask_data;  // Grayscale mask (0-255)
  int width = 0;
  int height = 0;
  bool invert = false;             // Invert mask values
};

/// Standard 14 PDF fonts
enum class StandardFont {
  Helvetica,
  HelveticaBold,
  HelveticaOblique,
  HelveticaBoldOblique,
  TimesRoman,
  TimesBold,
  TimesItalic,
  TimesBoldItalic,
  Courier,
  CourierBold,
  CourierOblique,
  CourierBoldOblique,
  Symbol,
  ZapfDingbats
};

// ============================================================
// Font Embedding Support
// ============================================================

/// Font embedding mode
enum class FontEmbedding {
  Full,       // Embed entire font file (larger but complete)
  Subset,     // Embed only used glyphs (smaller, recommended)
  None,       // Don't embed (font must be available on viewer system)
};

/// Font flags for PDF font descriptor
struct FontFlags {
  bool fixed_pitch = false;    // Bit 1: Monospace font
  bool serif = false;          // Bit 2: Serif font
  bool symbolic = false;       // Bit 3: Symbolic (non-Latin)
  bool script = false;         // Bit 4: Script/handwriting font
  bool nonsymbolic = true;     // Bit 6: Non-symbolic (Latin text)
  bool italic = false;         // Bit 7: Italic/oblique
  bool all_cap = false;        // Bit 17: All capitals
  bool small_cap = false;      // Bit 18: Small capitals
  bool force_bold = false;     // Bit 19: Force bold rendering

  /// Convert to PDF flags integer
  int to_int() const;

  /// Create from PDF flags integer
  static FontFlags from_int(int flags);
};

/// Font metrics extracted from font file
struct FontMetrics {
  std::string font_name;           // PostScript name
  std::string family_name;         // Font family
  int units_per_em = 1000;         // Design units per em
  int ascender = 0;                // Ascender in font units
  int descender = 0;               // Descender in font units (negative)
  int line_gap = 0;                // Line gap
  int cap_height = 0;              // Capital letter height
  int x_height = 0;                // Lowercase x height
  double italic_angle = 0.0;       // Italic angle in degrees
  int stem_v = 0;                  // Vertical stem width
  std::vector<int> bbox;           // Font bounding box [llx, lly, urx, ury]
  FontFlags flags;

  /// Get glyph width for a character (in 1/1000 em units)
  int get_width(uint32_t codepoint) const;

  /// All glyph widths indexed by glyph ID
  std::vector<int> glyph_widths;

  /// Character to glyph ID mapping
  std::map<uint32_t, uint16_t> char_to_glyph;
};

/// Loaded font data container
struct FontData {
  std::vector<uint8_t> data;       // Raw font file data
  FontMetrics metrics;             // Extracted metrics
  bool valid = false;              // True if successfully loaded

  /// Load TrueType/OpenType font from file
  static FontData FromFile(const std::string& path);

  /// Load TrueType/OpenType font from memory
  static FontData FromMemory(const uint8_t* data, size_t size);

  /// Check if valid
  bool is_valid() const { return valid; }
};

// ============================================================
// Transparency Support
// ============================================================

/// Blend modes for transparency (skip if already defined in nanopdf.hh)
#ifndef NANOPDF_HH_INCLUDED
enum class BlendMode {
  Normal,
  Multiply,
  Screen,
  Overlay,
  Darken,
  Lighten,
  ColorDodge,
  ColorBurn,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
};
#endif

// ============================================================
// Link Annotations
// ============================================================

/// Link action types
enum class LinkAction {
  URI,    // External URL
  GoTo,   // Internal page jump
  GoToR,  // Go to remote PDF
  Launch, // Launch application
};

/// Link annotation configuration
struct LinkConfig {
  double x = 0, y = 0, width = 0, height = 0;  // Annotation rectangle
  LinkAction action = LinkAction::URI;

  // For URI action
  std::string uri;

  // For GoTo action
  int dest_page = 0;
  double dest_y = 0;

  // Appearance
  bool show_border = false;
};

// ============================================================
// Bookmarks/Outlines
// ============================================================

/// Bookmark configuration
struct BookmarkConfig {
  std::string title;
  int page_index = 0;
  double y_position = 0;  // Destination Y coordinate
  bool open = true;       // Expanded by default
  bool bold = false;
  bool italic = false;
};

// ============================================================
// Attachments
// ============================================================

/// File attachment configuration
struct AttachmentConfig {
  std::string filename;
  std::string description;
  std::string mime_type;  // Optional, auto-detected if empty
  std::vector<uint8_t> data;
  bool compress = true;
};

// ============================================================
// Layers (Optional Content Groups)
// ============================================================

/// Layer configuration
struct LayerConfig {
  std::string name;
  bool visible = true;     // Default visibility
  bool printable = true;   // Print when visible
  bool locked = false;     // User can't toggle
};

// ============================================================
// Form Fields (AcroForms)
// ============================================================

/// Text field configuration
struct TextFieldConfig {
  std::string name;
  int page = 0;
  double x = 0, y = 0, width = 200, height = 24;
  std::string default_value;
  int max_length = 0;  // 0 = unlimited
  bool multiline = false;
  bool password = false;
  bool read_only = false;
  bool required = false;
  std::string font_name;  // Default appearance font
  double font_size = 12;
};

/// Checkbox configuration
struct CheckboxConfig {
  std::string name;
  int page = 0;
  double x = 0, y = 0, size = 14;
  bool checked = false;
  std::string export_value = "Yes";
};

/// Radio button option
struct RadioOption {
  double x, y, size;
  std::string value;
};

/// Radio group configuration
struct RadioGroupConfig {
  std::string name;
  int page = 0;
  std::vector<RadioOption> options;
  int selected = -1;  // -1 = none selected
};

/// Dropdown configuration
struct DropdownConfig {
  std::string name;
  int page = 0;
  double x = 0, y = 0, width = 200, height = 24;
  std::vector<std::string> options;
  int selected = 0;
  bool editable = false;
};

// ============================================================
// Gradients
// ============================================================

/// Color stop for gradients
struct ColorStop {
  double position;  // 0.0-1.0
  double r, g, b;   // RGB color
};

/// Gradient type
enum class GradientType { Linear, Radial };

/// Gradient configuration
struct GradientConfig {
  GradientType type = GradientType::Linear;

  // For linear: start and end points
  double x1 = 0, y1 = 0, x2 = 100, y2 = 0;

  // For radial: center and radius
  double cx = 50, cy = 50, r = 50;

  std::vector<ColorStop> stops;
  bool extend_start = false;
  bool extend_end = false;
};

// ============================================================
// Text Layout
// ============================================================

/// Text alignment
enum class TextAlign { Left, Center, Right, Justified };

/// Text style for layout
struct TextStyle {
  std::string font_name;
  double font_size = 12;
  double r = 0, g = 0, b = 0;  // Text color
  double line_height = 1.2;    // Multiplier
  double letter_spacing = 0;
  double word_spacing = 0;
};

// ============================================================
// Highlight Annotations
// ============================================================

/// Highlight color presets
enum class HighlightColor {
  Yellow,   // Default highlight color
  Green,
  Cyan,
  Magenta,
  Red,
  Custom,   // Use custom RGB values
};

/// Text markup annotation types
enum class MarkupType {
  Highlight,   // Yellow highlight effect
  Underline,   // Underline text
  Squiggly,    // Squiggly underline (spell check style)
  StrikeOut,   // Strike through text
};

/// Quadrilateral points for text highlight (4 corners)
struct QuadPoints {
  double x1, y1;  // Bottom-left
  double x2, y2;  // Bottom-right
  double x3, y3;  // Top-right
  double x4, y4;  // Top-left
};

/// Create QuadPoints from rectangle
inline QuadPoints quad_from_rect(double x, double y, double width, double height) {
  return {x, y, x + width, y, x + width, y + height, x, y + height};
}

/// Highlight annotation configuration
struct HighlightConfig {
  int page = 0;                   // Page index
  std::vector<QuadPoints> quads;  // Highlighted regions (can span multiple lines)

  // Appearance
  HighlightColor color_preset = HighlightColor::Yellow;
  double r = 1.0, g = 1.0, b = 0.0;  // Custom RGB (when color_preset = Custom)
  double alpha = 0.5;             // Transparency

  // Metadata
  std::string author;             // Annotation author
  std::string contents;           // Annotation text/comment
  std::string subject;            // Subject (e.g., "Comment")

  // Flags
  bool print = true;              // Include when printing
};

/// Generic text markup configuration
struct TextMarkupConfig {
  MarkupType type = MarkupType::Highlight;
  int page = 0;
  std::vector<QuadPoints> quads;
  double r = 1.0, g = 1.0, b = 0.0;
  double alpha = 0.5;
  std::string author;
  std::string contents;
  bool print = true;
};

// ============================================================
// Watermarks
// ============================================================

/// Watermark position on page
enum class WatermarkPosition {
  Center,      // Center of page
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
  Tiled,       // Repeat pattern across page
};

/// Watermark layer (where to draw)
enum class WatermarkLayer {
  Background,  // Draw before page content
  Foreground,  // Draw after page content (overlay)
};

/// Text watermark configuration
struct TextWatermarkConfig {
  std::string text;                 // Watermark text (e.g., "CONFIDENTIAL")
  std::string font_name;            // Font to use (empty = Helvetica)
  double font_size = 72;            // Font size in points
  double r = 0.8, g = 0.8, b = 0.8; // Color (default: light gray)
  double alpha = 0.3;               // Transparency (0.0-1.0)
  double rotation = -45;            // Rotation angle in degrees
  WatermarkPosition position = WatermarkPosition::Center;
  WatermarkLayer layer = WatermarkLayer::Background;
};

/// Image watermark configuration
struct ImageWatermarkConfig {
  std::string image_name;           // Resource name from add_image()
  double alpha = 0.3;               // Transparency (0.0-1.0)
  double scale = 1.0;               // Scale factor
  WatermarkPosition position = WatermarkPosition::Center;
  WatermarkLayer layer = WatermarkLayer::Background;
  double offset_x = 0, offset_y = 0; // Position offset from anchor
};

// ============================================================
// Headers/Footers
// ============================================================

/// Content types for headers/footers
enum class HeaderFooterContent {
  None,        // Empty section
  Text,        // Static text
  PageNumber,  // Current page number
  TotalPages,  // Total page count
  Date,        // Current date
};

/// Header/Footer section content
struct HFSection {
  HeaderFooterContent type = HeaderFooterContent::None;
  std::string text;                     // For Text type, or format string
  std::string date_format = "%Y-%m-%d"; // For Date type
};

/// Header configuration
struct HeaderConfig {
  HFSection left;                // Left-aligned content
  HFSection center;              // Center-aligned content
  HFSection right;               // Right-aligned content

  std::string font_name;         // Font (empty = Helvetica)
  double font_size = 10;         // Font size
  double r = 0, g = 0, b = 0;    // Text color

  double margin_top = 36;        // Distance from page top edge
  double margin_left = 72;       // Left margin
  double margin_right = 72;      // Right margin

  bool draw_line = false;        // Draw separator line below header
  double line_width = 0.5;       // Separator line width
};

/// Footer configuration
struct FooterConfig {
  HFSection left;
  HFSection center;
  HFSection right;

  std::string font_name;
  double font_size = 10;
  double r = 0, g = 0, b = 0;

  double margin_bottom = 36;     // Distance from page bottom edge
  double margin_left = 72;
  double margin_right = 72;

  bool draw_line = false;        // Draw separator line above footer
  double line_width = 0.5;
};

// ============================================================
// Bates Stamping / Page Numbering
// ============================================================

/// Position for Bates stamp
enum class BatesPosition {
  TopLeft,
  TopCenter,
  TopRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};

/// Bates numbering configuration
struct BatesConfig {
  std::string prefix;              // Prefix before number (e.g., "ABC-")
  std::string suffix;              // Suffix after number (e.g., "-2024")
  int start_number = 1;            // Starting number
  int digits = 6;                  // Number of digits (zero-padded)
  int increment = 1;               // Increment per page

  // Appearance
  std::string font_name;           // Font (empty = Helvetica)
  double font_size = 10;           // Font size in points
  double r = 0, g = 0, b = 0;      // Text color (black by default)

  // Position
  BatesPosition position = BatesPosition::BottomRight;
  double margin_x = 36;            // Horizontal margin from edge
  double margin_y = 36;            // Vertical margin from edge
};

// ============================================================
// Table Drawing API
// ============================================================

/// Cell alignment
enum class CellAlign {
  Left,
  Center,
  Right,
};

/// Vertical alignment
enum class CellVAlign {
  Top,
  Middle,
  Bottom,
};

/// Table cell style
struct CellStyle {
  // Text appearance
  std::string font_name;           // Font (empty = use table default)
  double font_size = 0;            // 0 = use table default
  double text_r = 0, text_g = 0, text_b = 0;  // Text color

  // Cell alignment
  CellAlign align = CellAlign::Left;
  CellVAlign valign = CellVAlign::Middle;

  // Cell padding
  double padding_left = 4;
  double padding_right = 4;
  double padding_top = 2;
  double padding_bottom = 2;

  // Background
  bool has_background = false;
  double bg_r = 1, bg_g = 1, bg_b = 1;  // Background color (white)

  // Border (per cell overrides)
  bool override_border = false;
  double border_width = 0.5;
  double border_r = 0, border_g = 0, border_b = 0;
};

/// Table cell for PDF writing
struct WriterTableCell {
  std::string text;                // Cell text content
  int colspan = 1;                 // Number of columns to span
  int rowspan = 1;                 // Number of rows to span
  CellStyle style;                 // Cell-specific style
};

/// Table row
struct TableRow {
  std::vector<WriterTableCell> cells;
  double height = 0;               // 0 = auto height based on content
  CellStyle row_style;             // Row-specific style (applies to all cells)
};

/// Table configuration
struct TableConfig {
  double x = 0, y = 0;             // Position (bottom-left of table)
  double width = 0;                // Total table width (0 = auto based on columns)

  // Column widths (relative or absolute)
  std::vector<double> column_widths;  // If sum > 0 and < width, treated as relative

  // Default appearance
  std::string font_name;           // Default font
  double font_size = 10;           // Default font size
  double text_r = 0, text_g = 0, text_b = 0;  // Default text color

  // Borders
  bool draw_outer_border = true;   // Draw outer border
  bool draw_inner_borders = true;  // Draw inner cell borders
  double border_width = 0.5;       // Border line width
  double border_r = 0, border_g = 0, border_b = 0;  // Border color

  // Header row styling
  bool has_header = false;         // First row is header
  CellStyle header_style;          // Style for header row

  // Alternating row colors
  bool alternate_row_colors = false;
  double alt_bg_r = 0.95, alt_bg_g = 0.95, alt_bg_b = 0.95;  // Alternate row bg
};

/// Table builder for structured table creation
class TableBuilder {
 public:
  TableBuilder();

  /// Set table position
  void set_position(double x, double y);

  /// Set total table width
  void set_width(double width);

  /// Set column widths (absolute values in points)
  void set_column_widths(const std::vector<double>& widths);

  /// Set default font and size
  void set_font(const std::string& font_name, double font_size);

  /// Set default text color
  void set_text_color(double r, double g, double b);

  /// Set border style
  void set_border(double width, double r = 0, double g = 0, double b = 0);

  /// Enable/disable outer border
  void set_outer_border(bool enabled);

  /// Enable/disable inner borders
  void set_inner_borders(bool enabled);

  /// Enable alternating row colors
  void set_alternating_rows(bool enabled, double r = 0.95, double g = 0.95, double b = 0.95);

  /// Add a header row (calls add_row internally with header styling)
  void add_header_row(const std::vector<std::string>& cells);

  /// Add a data row
  void add_row(const std::vector<std::string>& cells);

  /// Add a data row with cell styles
  void add_row(const std::vector<WriterTableCell>& cells);

  /// Add a styled row
  void add_row(const TableRow& row);

  /// Get the table configuration
  const TableConfig& config() const { return config_; }

  /// Get all rows
  const std::vector<TableRow>& rows() const { return rows_; }

  /// Calculate total table height
  double calculate_height() const;

 private:
  TableConfig config_;
  std::vector<TableRow> rows_;
};

// Forward declarations
struct Pdf;
class PdfWriter;
class TextLayout;

/// Page content builder for constructing page content streams
class PageBuilder {
 public:
  explicit PageBuilder(PdfWriter* writer);

  // Graphics state
  void save_state();     // q
  void restore_state();  // Q

  // Transformation
  void translate(double tx, double ty);
  void scale(double sx, double sy);
  void rotate(double angle_degrees);
  void concat_matrix(double a, double b, double c, double d, double e, double f);

  // Colors (RGB, 0.0-1.0)
  void set_stroke_color(double r, double g, double b);
  void set_fill_color(double r, double g, double b);
  void set_stroke_gray(double g);
  void set_fill_gray(double g);

  // Line style
  void set_line_width(double w);
  void set_line_cap(int cap);    // 0=butt, 1=round, 2=square
  void set_line_join(int join);  // 0=miter, 1=round, 2=bevel
  void set_dash_pattern(const std::vector<double>& pattern, double phase);

  // Path construction
  void move_to(double x, double y);
  void line_to(double x, double y);
  void curve_to(double x1, double y1, double x2, double y2, double x3, double y3);
  void close_path();

  // Path painting
  void stroke();
  void fill();
  void fill_even_odd();
  void fill_stroke();
  void clip();
  void clip_even_odd();  // Clip with even-odd rule (W* n)

  // Transparency
  void set_fill_alpha(double alpha);     // 0.0-1.0
  void set_stroke_alpha(double alpha);   // 0.0-1.0
  void set_blend_mode(BlendMode mode);
  void reset_transparency();             // Reset to opaque

  // Link annotations (added to page)
  void add_link(double x, double y, double w, double h, const std::string& uri);
  void add_link(double x, double y, double w, double h, int dest_page, double dest_y = 0);
  void add_link(const LinkConfig& config);

  // Highlight annotations (added to page)
  void add_highlight(double x, double y, double width, double height,
                     HighlightColor color = HighlightColor::Yellow);
  void add_highlight(const HighlightConfig& config);
  void add_highlight(const std::vector<QuadPoints>& quads,
                     HighlightColor color = HighlightColor::Yellow);
  void add_text_markup(const TextMarkupConfig& config);

  // Layer content markers
  void begin_layer(const std::string& layer_name);
  void end_layer();

  // Table drawing
  /// Draw a table using TableBuilder configuration
  void draw_table(const TableBuilder& table);

  /// Draw a simple table from data
  void draw_table(double x, double y, const std::vector<std::vector<std::string>>& data,
                  const std::vector<double>& column_widths = {});

  // Convenience shapes
  void rectangle(double x, double y, double w, double h);
  void line(double x1, double y1, double x2, double y2);
  void circle(double cx, double cy, double r);
  void ellipse(double cx, double cy, double rx, double ry);
  void arc(double cx, double cy, double rx, double ry,
           double start_angle, double end_angle);
  void rounded_rect(double x, double y, double w, double h, double r);

  // Image drawing
  void draw_image(const std::string& name, double x, double y, double w, double h);

  // Text
  void begin_text();
  void end_text();
  void set_font(const std::string& name, double size);
  void text_position(double x, double y);
  void show_text(const std::string& text);
  void show_text_at(double x, double y, const std::string& text);

  // Get the content stream
  const std::string& content() const { return content_; }

 private:
  void emit(const std::string& op);
  void emit_number(double n);
  std::string escape_string(const std::string& s);
  std::string encode_text_for_font(const std::string& text);
  std::string get_or_create_extgstate(double fill_alpha, double stroke_alpha, BlendMode mode);

  PdfWriter* writer_;
  std::string content_;
  std::string current_font_;  // Current font resource name

  // Link annotations to add to this page
  std::vector<LinkConfig> links_;

  // Highlight annotations to add to this page
  std::vector<TextMarkupConfig> highlights_;

  // Current transparency state
  double current_fill_alpha_ = 1.0;
  double current_stroke_alpha_ = 1.0;
  BlendMode current_blend_mode_ = BlendMode::Normal;

  friend class PdfWriter;  // For accessing links_ and highlights_
};

/// Text layout engine for word-wrapped text
class TextLayout {
 public:
  TextLayout();
  ~TextLayout();

  void set_width(double width);
  void set_max_height(double height);  // Optional, for overflow detection
  void set_alignment(TextAlign align);
  void set_style(const TextStyle& style);

  void add_text(const std::string& text);
  void add_line_break();
  void add_paragraph_break();

  // Get layout metrics
  double get_height() const;
  int get_line_count() const;
  bool has_overflow() const;

 private:
  friend class PageBuilder;
  friend class PdfWriter;

  struct Impl;
  Impl* impl_;
};

/// Page template (Form XObject) for reusable content
class Template {
 public:
  Template(double width, double height);
  ~Template();

  void set_size(double width, double height);

  // Get builder for template content
  PageBuilder& builder();

 private:
  friend class PdfWriter;
  double width_, height_;
  std::string content_;
  std::vector<std::string> used_images_;
  std::vector<std::string> used_fonts_;
};

/// PDF document writer
class PdfWriter {
 public:
  PdfWriter();
  ~PdfWriter();

  // ============================================================
  // Incremental Update (Revision) Support
  // ============================================================

  /// Load an existing PDF file for incremental updates
  /// Returns true on success, false on failure (check error in WriteResult)
  bool load_existing(const std::string& path, std::string* error = nullptr);

  /// Load an existing PDF from memory for incremental updates
  bool load_existing(const std::vector<uint8_t>& data, std::string* error = nullptr);

  /// Check if this writer has an existing PDF loaded
  bool has_existing_pdf() const;

  /// Get the current revision number (0 for new PDFs, 1+ for loaded PDFs)
  int get_revision_count() const;

  /// Write incremental update (appends to existing PDF)
  /// Only valid after load_existing() - preserves original PDF and appends changes
  WriteResult write_incremental(std::vector<uint8_t>& output);

  /// Write incremental update to file
  WriteResult write_incremental_to_file(const std::string& path);

  /// Write incremental update prepared for signing
  WriteResult write_incremental_for_signing(std::vector<uint8_t>& output,
                                            size_t content_length = 8192);

  // ============================================================
  // Version management
  // ============================================================

  /// Set the PDF version (default: 1.4)
  void set_version(PdfVersion version);
  PdfVersion get_version() const;

  // Document ID
  /// Set custom document ID (two 16-byte values)
  /// If not set, random IDs are generated
  void set_document_id(const std::vector<uint8_t>& id1,
                       const std::vector<uint8_t>& id2);
  /// Generate random document IDs
  void generate_document_id();

  // Metadata
  void set_title(const std::string& title);
  void set_author(const std::string& author);
  void set_subject(const std::string& subject);
  void set_keywords(const std::string& keywords);
  void set_creator(const std::string& creator);
  void set_creation_date(const std::string& date);  // Format: D:YYYYMMDDHHmmSS
  void set_modification_date(const std::string& date);

  // Resource management
  /// Add an image resource. Returns resource name (e.g., "Im1")
  std::string add_image(const ImageData& image,
                        ImageCompression compression = ImageCompression::Auto);

  /// Add a monochrome image with CCITT Group 4 compression
  /// Input data should be 1-bit packed (MSB first, 0=white, 1=black)
  /// Returns resource name (e.g., "Im1")
  std::string add_ccitt_image(const uint8_t* mono_data, int width, int height);

  /// Add an image with preserved alpha channel (soft mask)
  /// If the image has an alpha channel (RGBA), it will be converted to an SMask
  /// Returns resource name (e.g., "Im1")
  std::string add_image_with_alpha(const ImageData& image,
                                   ImageCompression compression = ImageCompression::Auto);

  /// Add an image with explicit soft mask
  /// The mask must have the same dimensions as the image
  /// Returns resource name (e.g., "Im1")
  std::string add_image_with_mask(const ImageData& image, const SoftMaskConfig& mask,
                                  ImageCompression compression = ImageCompression::Auto);

  /// Add a standard font. Returns resource name (e.g., "F1")
  std::string add_standard_font(StandardFont font);

  // ============================================================
  // Font Embedding
  // ============================================================

  /// Add a TrueType/OpenType font from file. Returns resource name (e.g., "F1")
  /// The font will be embedded in the PDF according to the embedding mode.
  /// Returns empty string on failure.
  std::string add_truetype_font(const std::string& path,
                                FontEmbedding embedding = FontEmbedding::Subset);

  /// Add a TrueType/OpenType font from memory. Returns resource name (e.g., "F1")
  std::string add_truetype_font(const FontData& font,
                                FontEmbedding embedding = FontEmbedding::Subset);

  /// Add a TrueType/OpenType font from raw data. Returns resource name (e.g., "F1")
  std::string add_truetype_font(const uint8_t* data, size_t size,
                                FontEmbedding embedding = FontEmbedding::Subset);

  /// Get font metrics for an embedded font by resource name
  /// Returns nullptr if font not found or is a standard font
  const FontMetrics* get_font_metrics(const std::string& name) const;

  /// Mark characters as used for font subsetting
  /// Call this for each character that will be rendered with the font
  /// If not called, all characters used in show_text() will be tracked automatically
  void mark_chars_used(const std::string& font_name, const std::string& text);

  // ============================================================
  // Bookmarks/Outlines
  // ============================================================

  /// Add a top-level bookmark. Returns bookmark ID for nesting.
  int add_bookmark(const std::string& title, int page_index, double y = 0);

  /// Add a bookmark with full configuration
  int add_bookmark(const BookmarkConfig& config);

  /// Add a child bookmark under a parent. Returns child bookmark ID.
  int add_child_bookmark(int parent_id, const std::string& title,
                         int page_index, double y = 0);

  // ============================================================
  // Attachments
  // ============================================================

  /// Add a file attachment with data
  void add_attachment(const std::string& filename, const std::vector<uint8_t>& data,
                      const std::string& description = "");

  /// Add a file attachment with full configuration
  void add_attachment(const AttachmentConfig& config);

  /// Add a file attachment from disk
  void add_attachment_from_file(const std::string& path,
                                const std::string& description = "");

  // ============================================================
  // Layers (Optional Content Groups)
  // ============================================================

  /// Add a layer (OCG). Returns layer name for use in PageBuilder::begin_layer()
  std::string add_layer(const std::string& name, bool visible = true);

  /// Add a layer with full configuration
  std::string add_layer(const LayerConfig& config);

  // ============================================================
  // Form Fields (AcroForms)
  // ============================================================

  /// Add a text input field. Returns field name.
  std::string add_text_field(const TextFieldConfig& config);

  /// Add a checkbox. Returns field name.
  std::string add_checkbox(const CheckboxConfig& config);

  /// Add a radio button group. Returns group name.
  std::string add_radio_group(const RadioGroupConfig& config);

  /// Add a dropdown/combo box. Returns field name.
  std::string add_dropdown(const DropdownConfig& config);

  /// Add a list box. Returns field name.
  std::string add_listbox(const std::string& name, int page, double x, double y,
                          double w, double h, const std::vector<std::string>& options);

  /// Add a push button. Returns field name.
  std::string add_button(const std::string& name, int page, double x, double y,
                         double w, double h, const std::string& caption);

  // ============================================================
  // Gradients
  // ============================================================

  /// Create a gradient resource. Returns gradient name (e.g., "P1")
  std::string create_gradient(const GradientConfig& config);

  // ============================================================
  // Page Templates (Form XObjects)
  // ============================================================

  /// Create a template for reusable content
  Template create_template(double width, double height);

  /// Add a template to the document. Returns template name (e.g., "Fm1")
  std::string add_template(const Template& tmpl);

  /// Draw a template on the current page (use inside page builder callback)
  void use_template(PageBuilder& builder, const std::string& name, double x, double y,
                    double scale_x = 1.0, double scale_y = 1.0);

  // ============================================================
  // Text Layout
  // ============================================================

  /// Create a text layout for word-wrapped text
  TextLayout create_text_layout();

  /// Draw a text layout on the page at position (x, y)
  void draw_text_layout(PageBuilder& builder, const TextLayout& layout,
                        double x, double y);

  // ============================================================
  // Watermarks
  // ============================================================

  /// Apply text watermark to all pages (including future pages)
  void set_watermark(const TextWatermarkConfig& config);

  /// Apply image watermark to all pages
  void set_watermark(const ImageWatermarkConfig& config);

  /// Apply watermark to specific page only
  void add_page_watermark(int page_index, const TextWatermarkConfig& config);
  void add_page_watermark(int page_index, const ImageWatermarkConfig& config);

  /// Remove global watermark
  void clear_watermark();

  // ============================================================
  // Headers/Footers
  // ============================================================

  /// Set global header (applies to all pages)
  void set_header(const HeaderConfig& config);

  /// Set global footer (applies to all pages)
  void set_footer(const FooterConfig& config);

  /// Set header for specific page
  void set_page_header(int page_index, const HeaderConfig& config);

  /// Set footer for specific page
  void set_page_footer(int page_index, const FooterConfig& config);

  /// Skip header/footer on specific page (e.g., title page)
  void skip_header_footer(int page_index);

  /// Clear global header
  void clear_header();

  /// Clear global footer
  void clear_footer();

  /// Get total page count
  int get_page_count() const;

  // ============================================================
  // Bates Stamping / Page Numbering
  // ============================================================

  /// Set Bates numbering configuration (applies to all pages)
  void set_bates_numbering(const BatesConfig& config);

  /// Clear Bates numbering
  void clear_bates_numbering();

  /// Skip Bates number on specific page (useful for cover pages)
  void skip_bates_number(int page_index);

  // ============================================================
  // Highlight Annotations
  // ============================================================

  /// Add highlight annotation to specific page
  void add_highlight(int page_index, const HighlightConfig& config);

  /// Add text markup annotation to specific page
  void add_text_markup(int page_index, const TextMarkupConfig& config);

  // Page management
  /// Add a page with custom content builder
  void add_page(PageSize size, std::function<void(PageBuilder&)> build_fn);

  /// Convenience: add a page containing a single image
  /// If fit_to_page is true, scales image to fit within page (respecting margin)
  /// If fit_to_page is false, uses image dimensions at 72 DPI
  void add_image_page(const ImageData& img, PageSize size = PageSize::A4(),
                      double margin = 0, bool fit_to_page = true);

  /// Convenience: add a page that fits the image dimensions
  void add_image_page_fit(const ImageData& img, double margin = 0);

  // Signature support
  /// Add an empty signature field (placeholder for later signing)
  /// Returns field name on success, empty string on failure
  std::string add_signature_field(const SignatureFieldConfig& config);

  /// Add an AcroForm if not already present (required for signature fields)
  void ensure_acroform();

  /// Get signature placeholder info after writing
  /// Call after write_to_file/write_to_memory to get ByteRange offsets
  const std::vector<SignaturePlaceholder>& get_signature_placeholders() const;

  /// Enable document permissions dictionary (for certification signatures)
  void set_permissions(MdpPermissions permissions);

  // ============================================================
  // Encryption Support
  // ============================================================

  /// Enable encryption with the specified configuration
  /// Must be called before write_to_file/write_to_memory
  /// Automatically upgrades PDF version if needed (e.g., to 1.6 for AES-128)
  void set_encryption(const EncryptionConfig& config);

  /// Check if encryption is enabled
  bool is_encrypted() const;

  /// Get current encryption algorithm
  EncryptionAlgorithm get_encryption_algorithm() const;

  // ============================================================
  // Timestamp Support
  // ============================================================

  /// Set timestamp server configuration for signatures
  /// The timestamp will be embedded in signatures created after this call
  void set_timestamp_config(const TimestampConfig& config);

  /// Check if timestamp is configured
  bool has_timestamp_config() const;

  // ============================================================
  // Form Fill (for existing PDFs loaded via load_existing)
  // ============================================================

  /// Set a text field value by field name. The field must exist in the loaded PDF.
  /// @param field_name  Fully-qualified field name (e.g., "form1[0].Name[0]" or "Name")
  /// @param value       New text value
  /// @return true if the field was found and updated
  bool set_field_value(const std::string& field_name, const std::string& value);

  /// Set a checkbox/radio button state by field name.
  /// @param field_name  Fully-qualified field name
  /// @param checked     true = checked ("/Yes"), false = unchecked ("/Off")
  /// @return true if the field was found and updated
  bool set_field_checked(const std::string& field_name, bool checked);

  /// Set a choice field (dropdown/listbox) selection by field name.
  /// @param field_name  Fully-qualified field name
  /// @param value       Selected option value
  /// @return true if the field was found and updated
  bool set_field_choice(const std::string& field_name, const std::string& value);

  // ============================================================
  // PDF Merge and Split
  // ============================================================

  /// Import pages from another parsed PDF into this writer.
  /// Each imported page is wrapped as a Form XObject and placed on a new page
  /// with matching dimensions, preserving the visual appearance.
  /// @param source_pdf  A parsed Pdf object (from parse_from_memory)
  /// @param page_indices  0-based page indices to import. Empty = all pages.
  /// @return Number of pages successfully imported, or -1 on error.
  int import_pages_from(const Pdf& source_pdf,
                        const std::vector<int>& page_indices = {});

  /// Extract specific pages from a parsed PDF into a new PDF document.
  /// @param source_pdf   A parsed Pdf object
  /// @param page_indices 0-based page indices to extract
  /// @param output       Output buffer for the new PDF
  /// @return WriteResult indicating success or failure
  static WriteResult split_pages(const Pdf& source_pdf,
                                 const std::vector<int>& page_indices,
                                 std::vector<uint8_t>& output);

  /// Merge multiple PDF documents (given as raw bytes) into a single PDF.
  /// @param pdf_data  Vector of PDF file contents
  /// @param output    Output buffer for the merged PDF
  /// @return WriteResult indicating success or failure
  static WriteResult merge_pdfs(const std::vector<std::vector<uint8_t>>& pdf_data,
                                std::vector<uint8_t>& output);

  /// Apply redaction annotations: remove content under redacted areas,
  /// draw overlay rectangles, and remove the redaction annotations.
  /// @param source_pdf   A parsed Pdf object containing redaction annotations
  /// @param output       Output buffer for the redacted PDF
  /// @return WriteResult indicating success or failure
  static WriteResult apply_redactions(const Pdf& source_pdf,
                                       std::vector<uint8_t>& output);

  // Output
  /// Write PDF to file
  WriteResult write_to_file(const std::string& path);

  /// Write PDF to memory buffer
  WriteResult write_to_memory(std::vector<uint8_t>& output);

  /// Write PDF prepared for signing (with signature placeholder)
  /// The signature placeholder will have space reserved for the signature
  /// content_length specifies how many bytes to reserve for the signature
  WriteResult write_for_signing(std::vector<uint8_t>& output,
                                size_t content_length = 8192);

  // ============================================================
  // Annotation CRUD on Existing PDFs
  // ============================================================

  /// Add a text annotation (sticky note) to an existing PDF page.
  /// Requires load_existing() to have been called first.
  /// @param page_index  0-based page index in the existing PDF
  /// @param x           Annotation rect x position
  /// @param y           Annotation rect y position
  /// @param w           Annotation rect width
  /// @param h           Annotation rect height
  /// @param contents    Annotation text content
  /// @return true if the annotation was queued for writing
  bool add_text_annotation_to_existing_page(int page_index,
                                             double x, double y,
                                             double w, double h,
                                             const std::string& contents);

  /// Add a highlight annotation to an existing PDF page.
  /// @param page_index  0-based page index
  /// @param config      Highlight configuration with quad points and color
  /// @return true if the annotation was queued
  bool add_highlight_to_existing_page(int page_index,
                                       const HighlightConfig& config);

  /// Add a link annotation to an existing PDF page.
  /// @param page_index  0-based page index
  /// @param config      Link configuration with rect, action, and URI/dest
  /// @return true if the annotation was queued
  bool add_link_to_existing_page(int page_index,
                                  const LinkConfig& config);

  /// Delete an annotation from an existing PDF page by index.
  /// The index corresponds to the order of annotations in the page's /Annots array.
  /// @param page_index  0-based page index
  /// @param annot_index 0-based annotation index within the page
  /// @return true if the deletion was queued
  bool delete_annotation_from_existing_page(int page_index,
                                             int annot_index);

 private:
  friend class PageBuilder;
  friend class ObjectCopier;

  // Internal method for PageBuilder to encode text for embedded fonts
  std::string encode_text_internal(const std::string& font_name,
                                   const std::string& text);

  struct Impl;
  Impl* impl_;
};

}  // namespace nanopdf

#endif  // NANOPDF_PDF_WRITER_HH_
