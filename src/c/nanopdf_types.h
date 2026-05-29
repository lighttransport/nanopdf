#ifndef NANOPDF_C_TYPES_H_
#define NANOPDF_C_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NANOPDF_C_VERSION_MAJOR 0u
#define NANOPDF_C_VERSION_MINOR 51u
#define NANOPDF_C_VERSION_PATCH 0u

typedef struct nanopdf_context nanopdf_context;
typedef struct nanopdf_document nanopdf_document;
typedef struct nanopdf_text_layout nanopdf_text_layout;
typedef struct nanopdf_writer_text_layout nanopdf_writer_text_layout;
typedef struct nanopdf_writer nanopdf_writer;
typedef struct nanopdf_page_builder nanopdf_page_builder;
typedef struct nanopdf_writer_table nanopdf_writer_table;
typedef struct nanopdf_object nanopdf_object;
typedef struct nanopdf_table_result nanopdf_table_result;
typedef struct nanopdf_pdfa_report nanopdf_pdfa_report;

typedef enum nanopdf_status {
  NANOPDF_STATUS_OK = 0,
  NANOPDF_STATUS_INVALID_ARGUMENT = 1,
  NANOPDF_STATUS_OUT_OF_MEMORY = 2,
  NANOPDF_STATUS_PARSE_ERROR = 3,
  NANOPDF_STATUS_MALFORMED = 4,
  NANOPDF_STATUS_UNSUPPORTED = 5,
  NANOPDF_STATUS_ENCRYPTED = 6,
  NANOPDF_STATUS_IO_ERROR = 7,
  NANOPDF_STATUS_INTERNAL_ERROR = 8,
  NANOPDF_STATUS_NOT_FOUND = 9
} nanopdf_status;

typedef enum nanopdf_info_key {
  NANOPDF_INFO_TITLE = 0,
  NANOPDF_INFO_AUTHOR = 1,
  NANOPDF_INFO_SUBJECT = 2,
  NANOPDF_INFO_KEYWORDS = 3,
  NANOPDF_INFO_CREATOR = 4,
  NANOPDF_INFO_PRODUCER = 5,
  NANOPDF_INFO_CREATION_DATE = 6,
  NANOPDF_INFO_MOD_DATE = 7,
  NANOPDF_INFO_TRAPPED = 8
} nanopdf_info_key;

typedef enum nanopdf_pdf_version {
  NANOPDF_PDF_VERSION_1_4 = 0,
  NANOPDF_PDF_VERSION_1_5 = 1,
  NANOPDF_PDF_VERSION_1_6 = 2,
  NANOPDF_PDF_VERSION_1_7 = 3,
  NANOPDF_PDF_VERSION_2_0 = 4
} nanopdf_pdf_version;

typedef enum nanopdf_page_label_style {
  NANOPDF_PAGE_LABEL_STYLE_DECIMAL_ARABIC = 0,
  NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_ROMAN = 1,
  NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_ROMAN = 2,
  NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_LETTERS = 3,
  NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_LETTERS = 4,
  NANOPDF_PAGE_LABEL_STYLE_NONE = 5
} nanopdf_page_label_style;

typedef enum nanopdf_page_layout {
  NANOPDF_PAGE_LAYOUT_DEFAULT = 0,
  NANOPDF_PAGE_LAYOUT_SINGLE_PAGE = 1,
  NANOPDF_PAGE_LAYOUT_ONE_COLUMN = 2,
  NANOPDF_PAGE_LAYOUT_TWO_COLUMN_LEFT = 3,
  NANOPDF_PAGE_LAYOUT_TWO_COLUMN_RIGHT = 4,
  NANOPDF_PAGE_LAYOUT_TWO_PAGE_LEFT = 5,
  NANOPDF_PAGE_LAYOUT_TWO_PAGE_RIGHT = 6
} nanopdf_page_layout;

typedef enum nanopdf_page_mode {
  NANOPDF_PAGE_MODE_DEFAULT = 0,
  NANOPDF_PAGE_MODE_USE_NONE = 1,
  NANOPDF_PAGE_MODE_USE_OUTLINES = 2,
  NANOPDF_PAGE_MODE_USE_THUMBS = 3,
  NANOPDF_PAGE_MODE_FULL_SCREEN = 4,
  NANOPDF_PAGE_MODE_USE_OC = 5,
  NANOPDF_PAGE_MODE_USE_ATTACHMENTS = 6
} nanopdf_page_mode;

typedef enum nanopdf_trapped_state {
  NANOPDF_TRAPPED_STATE_DEFAULT = 0,
  NANOPDF_TRAPPED_STATE_FALSE = 1,
  NANOPDF_TRAPPED_STATE_TRUE = 2,
  NANOPDF_TRAPPED_STATE_UNKNOWN = 3
} nanopdf_trapped_state;

typedef enum nanopdf_field_type {
  NANOPDF_FIELD_TYPE_BUTTON = 0,
  NANOPDF_FIELD_TYPE_TEXT = 1,
  NANOPDF_FIELD_TYPE_CHOICE = 2,
  NANOPDF_FIELD_TYPE_SIGNATURE = 3
} nanopdf_field_type;

typedef enum nanopdf_annotation_type {
  NANOPDF_ANNOTATION_TYPE_TEXT = 0,
  NANOPDF_ANNOTATION_TYPE_LINK = 1,
  NANOPDF_ANNOTATION_TYPE_FREE_TEXT = 2,
  NANOPDF_ANNOTATION_TYPE_LINE = 3,
  NANOPDF_ANNOTATION_TYPE_SQUARE = 4,
  NANOPDF_ANNOTATION_TYPE_CIRCLE = 5,
  NANOPDF_ANNOTATION_TYPE_POLYGON = 6,
  NANOPDF_ANNOTATION_TYPE_POLY_LINE = 7,
  NANOPDF_ANNOTATION_TYPE_HIGHLIGHT = 8,
  NANOPDF_ANNOTATION_TYPE_UNDERLINE = 9,
  NANOPDF_ANNOTATION_TYPE_SQUIGGLY = 10,
  NANOPDF_ANNOTATION_TYPE_STRIKE_OUT = 11,
  NANOPDF_ANNOTATION_TYPE_STAMP = 12,
  NANOPDF_ANNOTATION_TYPE_CARET = 13,
  NANOPDF_ANNOTATION_TYPE_INK = 14,
  NANOPDF_ANNOTATION_TYPE_POPUP = 15,
  NANOPDF_ANNOTATION_TYPE_FILE_ATTACHMENT = 16,
  NANOPDF_ANNOTATION_TYPE_SOUND = 17,
  NANOPDF_ANNOTATION_TYPE_MOVIE = 18,
  NANOPDF_ANNOTATION_TYPE_WIDGET = 19,
  NANOPDF_ANNOTATION_TYPE_SCREEN = 20,
  NANOPDF_ANNOTATION_TYPE_PRINTER_MARK = 21,
  NANOPDF_ANNOTATION_TYPE_TRAP_NET = 22,
  NANOPDF_ANNOTATION_TYPE_WATERMARK = 23,
  NANOPDF_ANNOTATION_TYPE_THREE_D = 24,
  NANOPDF_ANNOTATION_TYPE_REDACT = 25
} nanopdf_annotation_type;

typedef enum nanopdf_annotation_action_type {
  NANOPDF_ANNOTATION_ACTION_NONE = 0,
  NANOPDF_ANNOTATION_ACTION_GOTO = 1,
  NANOPDF_ANNOTATION_ACTION_GOTO_REMOTE = 2,
  NANOPDF_ANNOTATION_ACTION_LAUNCH = 3,
  NANOPDF_ANNOTATION_ACTION_URI = 4,
  NANOPDF_ANNOTATION_ACTION_NAMED = 5,
  NANOPDF_ANNOTATION_ACTION_JAVASCRIPT = 6
} nanopdf_annotation_action_type;

typedef enum nanopdf_bookmark_action {
  NANOPDF_BOOKMARK_ACTION_GOTO = 0,
  NANOPDF_BOOKMARK_ACTION_GOTO_REMOTE = 1,
  NANOPDF_BOOKMARK_ACTION_URI = 2,
  NANOPDF_BOOKMARK_ACTION_LAUNCH = 3
} nanopdf_bookmark_action;

typedef enum nanopdf_table_output_format {
  NANOPDF_TABLE_OUTPUT_CSV = 0,
  NANOPDF_TABLE_OUTPUT_HTML = 1,
  NANOPDF_TABLE_OUTPUT_JSON = 2,
  NANOPDF_TABLE_OUTPUT_MARKDOWN = 3,
  NANOPDF_TABLE_OUTPUT_TEXT = 4
} nanopdf_table_output_format;

typedef enum nanopdf_pdfa_rule {
  NANOPDF_PDFA_RULE_MISSING_XMP_METADATA = 0,
  NANOPDF_PDFA_RULE_MISSING_OUTPUT_INTENT = 1,
  NANOPDF_PDFA_RULE_FONT_NOT_EMBEDDED = 2,
  NANOPDF_PDFA_RULE_TRANSPARENCY_USED = 3,
  NANOPDF_PDFA_RULE_MISSING_DOCUMENT_INFO = 4,
  NANOPDF_PDFA_RULE_ENCRYPTION_PRESENT = 5,
  NANOPDF_PDFA_RULE_INVALID_COLOR_SPACE = 6
} nanopdf_pdfa_rule;

typedef enum nanopdf_standard_font {
  NANOPDF_STANDARD_FONT_HELVETICA = 0,
  NANOPDF_STANDARD_FONT_HELVETICA_BOLD = 1,
  NANOPDF_STANDARD_FONT_HELVETICA_OBLIQUE = 2,
  NANOPDF_STANDARD_FONT_HELVETICA_BOLD_OBLIQUE = 3,
  NANOPDF_STANDARD_FONT_TIMES_ROMAN = 4,
  NANOPDF_STANDARD_FONT_TIMES_BOLD = 5,
  NANOPDF_STANDARD_FONT_TIMES_ITALIC = 6,
  NANOPDF_STANDARD_FONT_TIMES_BOLD_ITALIC = 7,
  NANOPDF_STANDARD_FONT_COURIER = 8,
  NANOPDF_STANDARD_FONT_COURIER_BOLD = 9,
  NANOPDF_STANDARD_FONT_COURIER_OBLIQUE = 10,
  NANOPDF_STANDARD_FONT_COURIER_BOLD_OBLIQUE = 11,
  NANOPDF_STANDARD_FONT_SYMBOL = 12,
  NANOPDF_STANDARD_FONT_ZAPF_DINGBATS = 13
} nanopdf_standard_font;

typedef enum nanopdf_image_compression {
  NANOPDF_IMAGE_COMPRESSION_AUTO = 0,
  NANOPDF_IMAGE_COMPRESSION_FLATE = 1,
  NANOPDF_IMAGE_COMPRESSION_DCT = 2,
  NANOPDF_IMAGE_COMPRESSION_CCITT_FAX = 3
} nanopdf_image_compression;

typedef enum nanopdf_font_embedding {
  NANOPDF_FONT_EMBEDDING_FULL = 0,
  NANOPDF_FONT_EMBEDDING_SUBSET = 1,
  NANOPDF_FONT_EMBEDDING_NONE = 2
} nanopdf_font_embedding;

typedef struct nanopdf_font_metrics_summary {
  int units_per_em;
  int ascender;
  int descender;
  int line_gap;
  int cap_height;
  int x_height;
  double italic_angle;
  int stem_v;
  int bbox[4];
  uint32_t flags;
} nanopdf_font_metrics_summary;

typedef enum nanopdf_blend_mode {
  NANOPDF_BLEND_MODE_NORMAL = 0,
  NANOPDF_BLEND_MODE_MULTIPLY = 1,
  NANOPDF_BLEND_MODE_SCREEN = 2,
  NANOPDF_BLEND_MODE_OVERLAY = 3,
  NANOPDF_BLEND_MODE_DARKEN = 4,
  NANOPDF_BLEND_MODE_LIGHTEN = 5,
  NANOPDF_BLEND_MODE_COLOR_DODGE = 6,
  NANOPDF_BLEND_MODE_COLOR_BURN = 7,
  NANOPDF_BLEND_MODE_HARD_LIGHT = 8,
  NANOPDF_BLEND_MODE_SOFT_LIGHT = 9,
  NANOPDF_BLEND_MODE_DIFFERENCE = 10,
  NANOPDF_BLEND_MODE_EXCLUSION = 11
} nanopdf_blend_mode;

typedef enum nanopdf_watermark_position {
  NANOPDF_WATERMARK_POSITION_CENTER = 0,
  NANOPDF_WATERMARK_POSITION_TOP_LEFT = 1,
  NANOPDF_WATERMARK_POSITION_TOP_RIGHT = 2,
  NANOPDF_WATERMARK_POSITION_BOTTOM_LEFT = 3,
  NANOPDF_WATERMARK_POSITION_BOTTOM_RIGHT = 4,
  NANOPDF_WATERMARK_POSITION_TILED = 5
} nanopdf_watermark_position;

typedef enum nanopdf_watermark_layer {
  NANOPDF_WATERMARK_LAYER_BACKGROUND = 0,
  NANOPDF_WATERMARK_LAYER_FOREGROUND = 1
} nanopdf_watermark_layer;

typedef enum nanopdf_header_footer_content {
  NANOPDF_HEADER_FOOTER_CONTENT_NONE = 0,
  NANOPDF_HEADER_FOOTER_CONTENT_TEXT = 1,
  NANOPDF_HEADER_FOOTER_CONTENT_PAGE_NUMBER = 2,
  NANOPDF_HEADER_FOOTER_CONTENT_TOTAL_PAGES = 3,
  NANOPDF_HEADER_FOOTER_CONTENT_DATE = 4
} nanopdf_header_footer_content;

typedef enum nanopdf_bates_position {
  NANOPDF_BATES_POSITION_TOP_LEFT = 0,
  NANOPDF_BATES_POSITION_TOP_CENTER = 1,
  NANOPDF_BATES_POSITION_TOP_RIGHT = 2,
  NANOPDF_BATES_POSITION_BOTTOM_LEFT = 3,
  NANOPDF_BATES_POSITION_BOTTOM_CENTER = 4,
  NANOPDF_BATES_POSITION_BOTTOM_RIGHT = 5
} nanopdf_bates_position;

typedef enum nanopdf_writer_text_align {
  NANOPDF_WRITER_TEXT_ALIGN_LEFT = 0,
  NANOPDF_WRITER_TEXT_ALIGN_CENTER = 1,
  NANOPDF_WRITER_TEXT_ALIGN_RIGHT = 2,
  NANOPDF_WRITER_TEXT_ALIGN_JUSTIFIED = 3
} nanopdf_writer_text_align;

typedef enum nanopdf_markup_type {
  NANOPDF_MARKUP_TYPE_HIGHLIGHT = 0,
  NANOPDF_MARKUP_TYPE_UNDERLINE = 1,
  NANOPDF_MARKUP_TYPE_SQUIGGLY = 2,
  NANOPDF_MARKUP_TYPE_STRIKE_OUT = 3
} nanopdf_markup_type;

typedef enum nanopdf_link_action {
  NANOPDF_LINK_ACTION_URI = 0,
  NANOPDF_LINK_ACTION_GOTO = 1
} nanopdf_link_action;

typedef enum nanopdf_table_cell_align {
  NANOPDF_TABLE_CELL_ALIGN_LEFT = 0,
  NANOPDF_TABLE_CELL_ALIGN_CENTER = 1,
  NANOPDF_TABLE_CELL_ALIGN_RIGHT = 2
} nanopdf_table_cell_align;

typedef enum nanopdf_table_cell_valign {
  NANOPDF_TABLE_CELL_VALIGN_MIDDLE = 0,
  NANOPDF_TABLE_CELL_VALIGN_TOP = 1,
  NANOPDF_TABLE_CELL_VALIGN_BOTTOM = 2
} nanopdf_table_cell_valign;

typedef enum nanopdf_signature_filter {
  NANOPDF_SIGNATURE_FILTER_ADOBE_PPK_LITE = 0,
  NANOPDF_SIGNATURE_FILTER_ENTRUST_PPKEF = 1,
  NANOPDF_SIGNATURE_FILTER_CICI_SIGN_IT = 2,
  NANOPDF_SIGNATURE_FILTER_VERISIGN_PPKVS = 3
} nanopdf_signature_filter;

typedef enum nanopdf_signature_subfilter {
  NANOPDF_SIGNATURE_SUBFILTER_PKCS7_DETACHED = 0,
  NANOPDF_SIGNATURE_SUBFILTER_PKCS7_SHA1 = 1,
  NANOPDF_SIGNATURE_SUBFILTER_ETSI_CADES_DETACHED = 2,
  NANOPDF_SIGNATURE_SUBFILTER_ETSI_RFC3161 = 3
} nanopdf_signature_subfilter;

typedef enum nanopdf_mdp_permissions {
  NANOPDF_MDP_PERMISSIONS_NO_CHANGES = 1,
  NANOPDF_MDP_PERMISSIONS_FORM_FILL_AND_SIGN = 2,
  NANOPDF_MDP_PERMISSIONS_ANNOTATE_FORM_FILL_SIGN = 3
} nanopdf_mdp_permissions;

typedef enum nanopdf_encryption_algorithm {
  NANOPDF_ENCRYPTION_NONE = 0,
  NANOPDF_ENCRYPTION_RC4_40 = 1,
  NANOPDF_ENCRYPTION_RC4_128 = 2,
  NANOPDF_ENCRYPTION_AES_128 = 3,
  NANOPDF_ENCRYPTION_AES_256 = 4
} nanopdf_encryption_algorithm;

typedef void* (*nanopdf_alloc_fn)(void* user_data, size_t size);
typedef void* (*nanopdf_realloc_fn)(void* user_data, void* ptr, size_t size);
typedef void (*nanopdf_free_fn)(void* user_data, void* ptr);

typedef struct nanopdf_allocator {
  void* user_data;
  nanopdf_alloc_fn alloc;
  nanopdf_realloc_fn realloc;
  nanopdf_free_fn free;
} nanopdf_allocator;

typedef struct nanopdf_context_options {
  size_t struct_size;
  nanopdf_allocator allocator;
} nanopdf_context_options;

typedef struct nanopdf_parse_options {
  uint8_t auto_repair;
  uint8_t recover_stream_length;
  size_t max_repair_scan;
  const char* password;
} nanopdf_parse_options;

typedef struct nanopdf_buffer_view {
  const void* data;
  size_t size;
} nanopdf_buffer_view;

typedef struct nanopdf_soft_mask_config {
  nanopdf_buffer_view data;
  uint32_t width;
  uint32_t height;
  int invert;
} nanopdf_soft_mask_config;

typedef struct nanopdf_page_info {
  uint32_t page_index;
  double width;
  double height;
  double rotation;
} nanopdf_page_info;

typedef struct nanopdf_text_layout_options {
  double baseline_tolerance;
  double line_spacing_threshold;
  double word_spacing_threshold;
  double column_gap_threshold;
  uint8_t detect_columns;
  uint8_t detect_rtl;
} nanopdf_text_layout_options;

typedef struct nanopdf_text_char {
  uint32_t unicode;
  double x;
  double y;
  double width;
  double height;
  double font_size;
  const char* font_name;
  double char_spacing;
  double word_spacing;
  int32_t line_index;
  int32_t word_index;
  double matrix[6];
  double rotation;
} nanopdf_text_char;

typedef struct nanopdf_form_field_info {
  nanopdf_field_type type;
  const char* partial_name;
  const char* full_name;
  const char* alternate_name;
  const char* mapping_name;
  uint32_t flags;
} nanopdf_form_field_info;

typedef struct nanopdf_radio_option {
  double x;
  double y;
  double size;
  const char* value;
} nanopdf_radio_option;

typedef struct nanopdf_color_stop {
  double position;
  double r;
  double g;
  double b;
} nanopdf_color_stop;

typedef struct nanopdf_quad_points {
  double x1;
  double y1;
  double x2;
  double y2;
  double x3;
  double y3;
  double x4;
  double y4;
} nanopdf_quad_points;

typedef struct nanopdf_text_markup_config {
  nanopdf_markup_type type;
  const nanopdf_quad_points* quads;
  size_t quad_count;
  double r;
  double g;
  double b;
  double alpha;
  const char* author;
  const char* contents;
  uint8_t print;
} nanopdf_text_markup_config;

typedef struct nanopdf_link_config {
  double x;
  double y;
  double width;
  double height;
  nanopdf_link_action action;
  const char* uri;
  uint32_t dest_page_index;
  double dest_y;
  uint8_t show_border;
} nanopdf_link_config;

typedef struct nanopdf_page_label {
  nanopdf_page_label_style style;
  const char* prefix;
  uint32_t start_value;
} nanopdf_page_label;

typedef struct nanopdf_named_destination {
  const char* name;
  uint32_t page_index;
  const char* fit_type;
  const double* position;
  size_t position_count;
} nanopdf_named_destination;

typedef struct nanopdf_viewer_preferences {
  uint8_t hide_toolbar;
  uint8_t hide_menubar;
  uint8_t hide_window_ui;
  uint8_t fit_window;
  uint8_t center_window;
  uint8_t display_doc_title;
} nanopdf_viewer_preferences;

typedef struct nanopdf_output_intent {
  const char* subtype;
  const char* output_condition;
  const char* output_condition_identifier;
  const char* registry_name;
  const char* info;
  nanopdf_buffer_view dest_output_profile;
  int32_t color_components;
} nanopdf_output_intent;

typedef struct nanopdf_mark_info {
  uint8_t marked;
  uint8_t suspects;
} nanopdf_mark_info;

typedef struct nanopdf_annotation_info {
  nanopdf_annotation_type type;
  double rect[4];
  const char* contents;
  const char* name;
  const char* modified_date;
  uint32_t flags;
  nanopdf_annotation_action_type action_type;
  const char* uri;
  nanopdf_field_type field_type;
  const char* field_name;
  const char* field_value;
} nanopdf_annotation_info;

typedef struct nanopdf_bookmark_info {
  const char* title;
  uint32_t depth;
  nanopdf_bookmark_action action;
  uint32_t page_index;
  const char* uri;
  const char* file;
  const double* position;
  size_t position_count;
  const double* color;
  size_t color_count;
  uint8_t open;
  uint8_t bold;
  uint8_t italic;
  int32_t visible_descendant_count;
} nanopdf_bookmark_info;

typedef struct nanopdf_attachment_info {
  const char* name;
  const char* description;
  const char* mime_type;
  const char* checksum;
  const char* creation_date;
  const char* modification_date;
  const char* relationship;
  size_t size;
} nanopdf_attachment_info;

typedef struct nanopdf_table_extraction_options {
  double alignment_tolerance;
  int32_t min_rows;
  int32_t min_cols;
  double max_cell_gap;
  int32_t min_chars_per_cell;
  uint8_t debug;
} nanopdf_table_extraction_options;

typedef struct nanopdf_table_info {
  uint32_t row_count;
  uint32_t column_count;
  double x;
  double y;
  double width;
  double height;
} nanopdf_table_info;

typedef struct nanopdf_table_cell_info {
  const char* text;
  uint32_t row;
  uint32_t column;
  uint32_t row_span;
  uint32_t column_span;
  double x;
  double y;
  double width;
  double height;
} nanopdf_table_cell_info;

typedef struct nanopdf_pdfa_summary {
  uint8_t valid;
  const char* claimed_level;
} nanopdf_pdfa_summary;

typedef struct nanopdf_pdfa_violation {
  nanopdf_pdfa_rule rule;
  const char* message;
  const char* detail;
} nanopdf_pdfa_violation;

typedef struct nanopdf_bookmark_config {
  const char* title;
  uint32_t page_index;
  double dest_y;
  uint8_t open;
  uint8_t bold;
  uint8_t italic;
} nanopdf_bookmark_config;

typedef struct nanopdf_attachment_config {
  const char* filename;
  const char* description;
  const char* mime_type;
  const void* data;
  size_t size;
  uint8_t use_compression;
} nanopdf_attachment_config;

typedef struct nanopdf_layer_config {
  const char* name;
  uint8_t visible;
  uint8_t printable;
  uint8_t locked;
} nanopdf_layer_config;

typedef struct nanopdf_signature_field_config {
  const char* name;
  uint32_t page_index;
  double x;
  double y;
  double width;
  double height;
  uint8_t visible;
  const char* reason;
  const char* location;
  const char* contact_info;
  nanopdf_signature_filter filter;
  nanopdf_signature_subfilter subfilter;
  uint8_t is_certification;
  nanopdf_mdp_permissions mdp_permissions;
} nanopdf_signature_field_config;

typedef struct nanopdf_signature_placeholder {
  const char* field_name;
  size_t contents_offset;
  size_t contents_length;
  size_t byte_range_offset;
  size_t byte_range_0;
  size_t byte_range_1;
  size_t byte_range_2;
  size_t byte_range_3;
} nanopdf_signature_placeholder;

typedef struct nanopdf_writer_encryption_config {
  nanopdf_encryption_algorithm algorithm;
  const char* user_password;
  const char* owner_password;
  uint8_t allow_print;
  uint8_t allow_modify;
  uint8_t allow_copy;
  uint8_t allow_annotate;
  uint8_t allow_fill_forms;
  uint8_t allow_accessibility;
  uint8_t allow_assemble;
  uint8_t allow_print_high_quality;
  uint8_t encrypt_metadata;
} nanopdf_writer_encryption_config;

typedef struct nanopdf_timestamp_config {
  const char* server_url;
  const char* username;
  const char* password;
  int32_t timeout_ms;
  uint8_t embed_in_signature;
} nanopdf_timestamp_config;

typedef struct nanopdf_header_footer_section {
  nanopdf_header_footer_content type;
  const char* text;
  const char* date_format;
} nanopdf_header_footer_section;

typedef struct nanopdf_header_config {
  nanopdf_header_footer_section left;
  nanopdf_header_footer_section center;
  nanopdf_header_footer_section right;
  const char* font_name;
  double font_size;
  double r;
  double g;
  double b;
  double margin_top;
  double margin_left;
  double margin_right;
  uint8_t draw_line;
  double line_width;
} nanopdf_header_config;

typedef struct nanopdf_footer_config {
  nanopdf_header_footer_section left;
  nanopdf_header_footer_section center;
  nanopdf_header_footer_section right;
  const char* font_name;
  double font_size;
  double r;
  double g;
  double b;
  double margin_bottom;
  double margin_left;
  double margin_right;
  uint8_t draw_line;
  double line_width;
} nanopdf_footer_config;

typedef struct nanopdf_bates_config {
  const char* prefix;
  const char* suffix;
  int32_t start_number;
  int32_t digits;
  int32_t increment;
  const char* font_name;
  double font_size;
  double r;
  double g;
  double b;
  nanopdf_bates_position position;
  double margin_x;
  double margin_y;
} nanopdf_bates_config;

typedef struct nanopdf_writer_text_style {
  const char* font_name;
  double font_size;
  double r;
  double g;
  double b;
  double line_height;
  double letter_spacing;
  double word_spacing;
} nanopdf_writer_text_style;

typedef struct nanopdf_writer_table_cell_style {
  const char* font_name;
  double font_size;
  double text_r;
  double text_g;
  double text_b;
  nanopdf_table_cell_align align;
  nanopdf_table_cell_valign valign;
  double padding_left;
  double padding_right;
  double padding_top;
  double padding_bottom;
  uint8_t has_background;
  double bg_r;
  double bg_g;
  double bg_b;
  uint8_t override_border;
  double border_width;
  double border_r;
  double border_g;
  double border_b;
} nanopdf_writer_table_cell_style;

typedef struct nanopdf_writer_table_cell {
  const char* text;
  int32_t colspan;
  int32_t rowspan;
  nanopdf_writer_table_cell_style style;
} nanopdf_writer_table_cell;

typedef struct nanopdf_writer_table_row {
  const nanopdf_writer_table_cell* cells;
  size_t cell_count;
  double height;
  nanopdf_writer_table_cell_style row_style;
} nanopdf_writer_table_row;

typedef struct nanopdf_object_ref {
  uint32_t object_number;
  uint16_t generation;
  uint8_t valid;
} nanopdf_object_ref;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_C_TYPES_H_ */
