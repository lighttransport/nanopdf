#include "nanopdf_write.h"

#include "nanopdf_c_internal.h"
#include "nanopdf.hh"
#include "pdf-writer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct nanopdf_writer {
  nanopdf_context* context;
  nanopdf::PdfWriter writer;
};

struct nanopdf_page_builder {
  nanopdf_context* context;
  nanopdf_writer* owner;
  nanopdf::PageSize size;
  bool is_template = false;
  std::unique_ptr<nanopdf::PageBuilder> builder;
};

struct nanopdf_writer_text_layout {
  nanopdf_context* context;
  std::unique_ptr<nanopdf::TextLayout> layout;
};

struct nanopdf_writer_table {
  nanopdf_context* context;
  std::unique_ptr<nanopdf::TableBuilder> table;
};

struct nanopdf_object {
  nanopdf_context* context;
  nanopdf::Value value;
};

namespace {

nanopdf_status set_error(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message) {
  nanopdf__set_error(context, status, message);
  return status;
}

nanopdf_status clear_success(nanopdf_context* context) {
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

nanopdf_status validate_writer(
    nanopdf_writer* writer,
    const char* message) {
  if (!writer || !writer->context) {
    return set_error(
        writer ? writer->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

bool is_reserved_info_key(const char* key) {
  return key &&
         (std::strcmp(key, "Title") == 0 || std::strcmp(key, "Author") == 0 ||
          std::strcmp(key, "Subject") == 0 || std::strcmp(key, "Keywords") == 0 ||
          std::strcmp(key, "Creator") == 0 || std::strcmp(key, "Producer") == 0 ||
          std::strcmp(key, "CreationDate") == 0 || std::strcmp(key, "ModDate") == 0 ||
          std::strcmp(key, "Trapped") == 0);
}

nanopdf_status validate_page(
    nanopdf_page_builder* page,
    const char* message) {
  if (!page || !page->context || !page->owner || !page->builder) {
    return set_error(
        page ? page->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status require_page_target(
    nanopdf_page_builder* page,
    const char* message) {
  nanopdf_status status = validate_page(page, message);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (page->is_template) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "operation is only valid for page builders");
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status require_template_target(
    nanopdf_page_builder* page,
    const char* message) {
  nanopdf_status status = validate_page(page, message);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!page->is_template) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "operation is only valid for template builders");
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status validate_object(
    const nanopdf_object* object,
    const char* message) {
  if (!object || !object->context) {
    return set_error(
        object ? object->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status validate_writer_text_layout(
    const nanopdf_writer_text_layout* layout,
    const char* message) {
  if (!layout || !layout->context || !layout->layout) {
    return set_error(
        layout ? layout->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status validate_writer_table(
    const nanopdf_writer_table* table,
    const char* message) {
  if (!table || !table->context || !table->table) {
    return set_error(
        table ? table->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

bool map_standard_font(
    nanopdf_standard_font font,
    nanopdf::StandardFont* out_font) {
  if (!out_font) {
    return false;
  }

  switch (font) {
    case NANOPDF_STANDARD_FONT_HELVETICA:
      *out_font = nanopdf::StandardFont::Helvetica;
      return true;
    case NANOPDF_STANDARD_FONT_HELVETICA_BOLD:
      *out_font = nanopdf::StandardFont::HelveticaBold;
      return true;
    case NANOPDF_STANDARD_FONT_HELVETICA_OBLIQUE:
      *out_font = nanopdf::StandardFont::HelveticaOblique;
      return true;
    case NANOPDF_STANDARD_FONT_HELVETICA_BOLD_OBLIQUE:
      *out_font = nanopdf::StandardFont::HelveticaBoldOblique;
      return true;
    case NANOPDF_STANDARD_FONT_TIMES_ROMAN:
      *out_font = nanopdf::StandardFont::TimesRoman;
      return true;
    case NANOPDF_STANDARD_FONT_TIMES_BOLD:
      *out_font = nanopdf::StandardFont::TimesBold;
      return true;
    case NANOPDF_STANDARD_FONT_TIMES_ITALIC:
      *out_font = nanopdf::StandardFont::TimesItalic;
      return true;
    case NANOPDF_STANDARD_FONT_TIMES_BOLD_ITALIC:
      *out_font = nanopdf::StandardFont::TimesBoldItalic;
      return true;
    case NANOPDF_STANDARD_FONT_COURIER:
      *out_font = nanopdf::StandardFont::Courier;
      return true;
    case NANOPDF_STANDARD_FONT_COURIER_BOLD:
      *out_font = nanopdf::StandardFont::CourierBold;
      return true;
    case NANOPDF_STANDARD_FONT_COURIER_OBLIQUE:
      *out_font = nanopdf::StandardFont::CourierOblique;
      return true;
    case NANOPDF_STANDARD_FONT_COURIER_BOLD_OBLIQUE:
      *out_font = nanopdf::StandardFont::CourierBoldOblique;
      return true;
    case NANOPDF_STANDARD_FONT_SYMBOL:
      *out_font = nanopdf::StandardFont::Symbol;
      return true;
    case NANOPDF_STANDARD_FONT_ZAPF_DINGBATS:
      *out_font = nanopdf::StandardFont::ZapfDingbats;
      return true;
  }

  return false;
}

bool map_image_compression(
    nanopdf_image_compression compression,
    nanopdf::ImageCompression* out_compression) {
  if (!out_compression) {
    return false;
  }

  switch (compression) {
    case NANOPDF_IMAGE_COMPRESSION_AUTO:
      *out_compression = nanopdf::ImageCompression::Auto;
      return true;
    case NANOPDF_IMAGE_COMPRESSION_FLATE:
      *out_compression = nanopdf::ImageCompression::Flate;
      return true;
    case NANOPDF_IMAGE_COMPRESSION_DCT:
      *out_compression = nanopdf::ImageCompression::DCT;
      return true;
    case NANOPDF_IMAGE_COMPRESSION_CCITT_FAX:
      *out_compression = nanopdf::ImageCompression::CCITTFax;
      return true;
  }

  return false;
}

bool map_font_embedding(
    nanopdf_font_embedding embedding,
    nanopdf::FontEmbedding* out_embedding) {
  if (!out_embedding) {
    return false;
  }

  switch (embedding) {
    case NANOPDF_FONT_EMBEDDING_FULL:
      *out_embedding = nanopdf::FontEmbedding::Full;
      return true;
    case NANOPDF_FONT_EMBEDDING_SUBSET:
      *out_embedding = nanopdf::FontEmbedding::Subset;
      return true;
    case NANOPDF_FONT_EMBEDDING_NONE:
      *out_embedding = nanopdf::FontEmbedding::None;
      return true;
  }

  return false;
}

bool map_pdf_version(
    nanopdf_pdf_version version,
    nanopdf::PdfVersion* out_version) {
  if (!out_version) {
    return false;
  }

  switch (version) {
    case NANOPDF_PDF_VERSION_1_4:
      *out_version = nanopdf::PdfVersion::v1_4;
      return true;
    case NANOPDF_PDF_VERSION_1_5:
      *out_version = nanopdf::PdfVersion::v1_5;
      return true;
    case NANOPDF_PDF_VERSION_1_6:
      *out_version = nanopdf::PdfVersion::v1_6;
      return true;
    case NANOPDF_PDF_VERSION_1_7:
      *out_version = nanopdf::PdfVersion::v1_7;
      return true;
    case NANOPDF_PDF_VERSION_2_0:
      *out_version = nanopdf::PdfVersion::v2_0;
      return true;
  }

  return false;
}

bool map_page_label_style(
    nanopdf_page_label_style style,
    nanopdf::PageLabelStyle* out_style) {
  if (!out_style) {
    return false;
  }

  switch (style) {
    case NANOPDF_PAGE_LABEL_STYLE_DECIMAL_ARABIC:
      *out_style = nanopdf::PageLabelStyle::DecimalArabic;
      return true;
    case NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_ROMAN:
      *out_style = nanopdf::PageLabelStyle::UppercaseRoman;
      return true;
    case NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_ROMAN:
      *out_style = nanopdf::PageLabelStyle::LowercaseRoman;
      return true;
    case NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_LETTERS:
      *out_style = nanopdf::PageLabelStyle::UppercaseLetters;
      return true;
    case NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_LETTERS:
      *out_style = nanopdf::PageLabelStyle::LowercaseLetters;
      return true;
    case NANOPDF_PAGE_LABEL_STYLE_NONE:
      *out_style = nanopdf::PageLabelStyle::None;
      return true;
  }

  return false;
}

bool map_page_layout(
    nanopdf_page_layout layout,
    nanopdf::PageLayout* out_layout) {
  if (!out_layout) {
    return false;
  }

  switch (layout) {
    case NANOPDF_PAGE_LAYOUT_DEFAULT:
      *out_layout = nanopdf::PageLayout::Unset;
      return true;
    case NANOPDF_PAGE_LAYOUT_SINGLE_PAGE:
      *out_layout = nanopdf::PageLayout::SinglePage;
      return true;
    case NANOPDF_PAGE_LAYOUT_ONE_COLUMN:
      *out_layout = nanopdf::PageLayout::OneColumn;
      return true;
    case NANOPDF_PAGE_LAYOUT_TWO_COLUMN_LEFT:
      *out_layout = nanopdf::PageLayout::TwoColumnLeft;
      return true;
    case NANOPDF_PAGE_LAYOUT_TWO_COLUMN_RIGHT:
      *out_layout = nanopdf::PageLayout::TwoColumnRight;
      return true;
    case NANOPDF_PAGE_LAYOUT_TWO_PAGE_LEFT:
      *out_layout = nanopdf::PageLayout::TwoPageLeft;
      return true;
    case NANOPDF_PAGE_LAYOUT_TWO_PAGE_RIGHT:
      *out_layout = nanopdf::PageLayout::TwoPageRight;
      return true;
  }

  return false;
}

bool map_page_mode(
    nanopdf_page_mode mode,
    nanopdf::PageMode* out_mode) {
  if (!out_mode) {
    return false;
  }

  switch (mode) {
    case NANOPDF_PAGE_MODE_DEFAULT:
      *out_mode = nanopdf::PageMode::Unset;
      return true;
    case NANOPDF_PAGE_MODE_USE_NONE:
      *out_mode = nanopdf::PageMode::UseNone;
      return true;
    case NANOPDF_PAGE_MODE_USE_OUTLINES:
      *out_mode = nanopdf::PageMode::UseOutlines;
      return true;
    case NANOPDF_PAGE_MODE_USE_THUMBS:
      *out_mode = nanopdf::PageMode::UseThumbs;
      return true;
    case NANOPDF_PAGE_MODE_FULL_SCREEN:
      *out_mode = nanopdf::PageMode::FullScreen;
      return true;
    case NANOPDF_PAGE_MODE_USE_OC:
      *out_mode = nanopdf::PageMode::UseOC;
      return true;
    case NANOPDF_PAGE_MODE_USE_ATTACHMENTS:
      *out_mode = nanopdf::PageMode::UseAttachments;
      return true;
  }

  return false;
}

bool map_trapped_state(
    nanopdf_trapped_state trapped,
    nanopdf::TrappedState* out_trapped) {
  if (!out_trapped) {
    return false;
  }

  switch (trapped) {
    case NANOPDF_TRAPPED_STATE_DEFAULT:
      *out_trapped = nanopdf::TrappedState::Unset;
      return true;
    case NANOPDF_TRAPPED_STATE_FALSE:
      *out_trapped = nanopdf::TrappedState::False;
      return true;
    case NANOPDF_TRAPPED_STATE_TRUE:
      *out_trapped = nanopdf::TrappedState::True;
      return true;
    case NANOPDF_TRAPPED_STATE_UNKNOWN:
      *out_trapped = nanopdf::TrappedState::Unknown;
      return true;
  }

  return false;
}

bool map_cpp_pdf_version(
    nanopdf::PdfVersion version,
    nanopdf_pdf_version* out_version) {
  if (!out_version) {
    return false;
  }

  switch (version) {
    case nanopdf::PdfVersion::v1_4:
      *out_version = NANOPDF_PDF_VERSION_1_4;
      return true;
    case nanopdf::PdfVersion::v1_5:
      *out_version = NANOPDF_PDF_VERSION_1_5;
      return true;
    case nanopdf::PdfVersion::v1_6:
      *out_version = NANOPDF_PDF_VERSION_1_6;
      return true;
    case nanopdf::PdfVersion::v1_7:
      *out_version = NANOPDF_PDF_VERSION_1_7;
      return true;
    case nanopdf::PdfVersion::v2_0:
      *out_version = NANOPDF_PDF_VERSION_2_0;
      return true;
  }

  return false;
}

bool map_blend_mode(
    nanopdf_blend_mode mode,
    nanopdf::BlendMode* out_mode) {
  if (!out_mode) {
    return false;
  }

  switch (mode) {
    case NANOPDF_BLEND_MODE_NORMAL:
      *out_mode = nanopdf::BlendMode::Normal;
      return true;
    case NANOPDF_BLEND_MODE_MULTIPLY:
      *out_mode = nanopdf::BlendMode::Multiply;
      return true;
    case NANOPDF_BLEND_MODE_SCREEN:
      *out_mode = nanopdf::BlendMode::Screen;
      return true;
    case NANOPDF_BLEND_MODE_OVERLAY:
      *out_mode = nanopdf::BlendMode::Overlay;
      return true;
    case NANOPDF_BLEND_MODE_DARKEN:
      *out_mode = nanopdf::BlendMode::Darken;
      return true;
    case NANOPDF_BLEND_MODE_LIGHTEN:
      *out_mode = nanopdf::BlendMode::Lighten;
      return true;
    case NANOPDF_BLEND_MODE_COLOR_DODGE:
      *out_mode = nanopdf::BlendMode::ColorDodge;
      return true;
    case NANOPDF_BLEND_MODE_COLOR_BURN:
      *out_mode = nanopdf::BlendMode::ColorBurn;
      return true;
    case NANOPDF_BLEND_MODE_HARD_LIGHT:
      *out_mode = nanopdf::BlendMode::HardLight;
      return true;
    case NANOPDF_BLEND_MODE_SOFT_LIGHT:
      *out_mode = nanopdf::BlendMode::SoftLight;
      return true;
    case NANOPDF_BLEND_MODE_DIFFERENCE:
      *out_mode = nanopdf::BlendMode::Difference;
      return true;
    case NANOPDF_BLEND_MODE_EXCLUSION:
      *out_mode = nanopdf::BlendMode::Exclusion;
      return true;
  }

  return false;
}

bool map_markup_type(
    nanopdf_markup_type type,
    nanopdf::MarkupType* out_type) {
  if (!out_type) {
    return false;
  }

  switch (type) {
    case NANOPDF_MARKUP_TYPE_HIGHLIGHT:
      *out_type = nanopdf::MarkupType::Highlight;
      return true;
    case NANOPDF_MARKUP_TYPE_UNDERLINE:
      *out_type = nanopdf::MarkupType::Underline;
      return true;
    case NANOPDF_MARKUP_TYPE_SQUIGGLY:
      *out_type = nanopdf::MarkupType::Squiggly;
      return true;
    case NANOPDF_MARKUP_TYPE_STRIKE_OUT:
      *out_type = nanopdf::MarkupType::StrikeOut;
      return true;
  }

  return false;
}

bool map_link_action(
    nanopdf_link_action action,
    nanopdf::LinkAction* out_action) {
  if (!out_action) {
    return false;
  }

  switch (action) {
    case NANOPDF_LINK_ACTION_URI:
      *out_action = nanopdf::LinkAction::URI;
      return true;
    case NANOPDF_LINK_ACTION_GOTO:
      *out_action = nanopdf::LinkAction::GoTo;
      return true;
  }

  return false;
}

bool map_signature_filter(
    nanopdf_signature_filter filter,
    nanopdf::SignatureFilter* out_filter) {
  if (!out_filter) {
    return false;
  }

  switch (filter) {
    case NANOPDF_SIGNATURE_FILTER_ADOBE_PPK_LITE:
      *out_filter = nanopdf::SignatureFilter::AdobePPKLite;
      return true;
    case NANOPDF_SIGNATURE_FILTER_ENTRUST_PPKEF:
      *out_filter = nanopdf::SignatureFilter::EntrustPPKEF;
      return true;
    case NANOPDF_SIGNATURE_FILTER_CICI_SIGN_IT:
      *out_filter = nanopdf::SignatureFilter::CICISignIt;
      return true;
    case NANOPDF_SIGNATURE_FILTER_VERISIGN_PPKVS:
      *out_filter = nanopdf::SignatureFilter::VeriSignPPKVS;
      return true;
  }

  return false;
}

bool map_signature_subfilter(
    nanopdf_signature_subfilter subfilter,
    nanopdf::SignatureSubFilter* out_subfilter) {
  if (!out_subfilter) {
    return false;
  }

  switch (subfilter) {
    case NANOPDF_SIGNATURE_SUBFILTER_PKCS7_DETACHED:
      *out_subfilter = nanopdf::SignatureSubFilter::Pkcs7Detached;
      return true;
    case NANOPDF_SIGNATURE_SUBFILTER_PKCS7_SHA1:
      *out_subfilter = nanopdf::SignatureSubFilter::Pkcs7Sha1;
      return true;
    case NANOPDF_SIGNATURE_SUBFILTER_ETSI_CADES_DETACHED:
      *out_subfilter = nanopdf::SignatureSubFilter::EtsiCadesDetached;
      return true;
    case NANOPDF_SIGNATURE_SUBFILTER_ETSI_RFC3161:
      *out_subfilter = nanopdf::SignatureSubFilter::EtsiRfc3161;
      return true;
  }

  return false;
}

bool map_mdp_permissions(
    nanopdf_mdp_permissions permissions,
    nanopdf::MdpPermissions* out_permissions) {
  if (!out_permissions) {
    return false;
  }

  if (permissions == 0) {
    *out_permissions = nanopdf::MdpPermissions::AnnotateFormFillSign;
    return true;
  }

  switch (permissions) {
    case NANOPDF_MDP_PERMISSIONS_NO_CHANGES:
      *out_permissions = nanopdf::MdpPermissions::NoChanges;
      return true;
    case NANOPDF_MDP_PERMISSIONS_FORM_FILL_AND_SIGN:
      *out_permissions = nanopdf::MdpPermissions::FormFillAndSign;
      return true;
    case NANOPDF_MDP_PERMISSIONS_ANNOTATE_FORM_FILL_SIGN:
      *out_permissions = nanopdf::MdpPermissions::AnnotateFormFillSign;
      return true;
  }

  return false;
}

bool map_encryption_algorithm(
    nanopdf_encryption_algorithm algorithm,
    nanopdf::EncryptionAlgorithm* out_algorithm) {
  if (!out_algorithm) {
    return false;
  }

  switch (algorithm) {
    case NANOPDF_ENCRYPTION_NONE:
      *out_algorithm = nanopdf::EncryptionAlgorithm::None;
      return true;
    case NANOPDF_ENCRYPTION_RC4_40:
      *out_algorithm = nanopdf::EncryptionAlgorithm::RC4_40;
      return true;
    case NANOPDF_ENCRYPTION_RC4_128:
      *out_algorithm = nanopdf::EncryptionAlgorithm::RC4_128;
      return true;
    case NANOPDF_ENCRYPTION_AES_128:
      *out_algorithm = nanopdf::EncryptionAlgorithm::AES_128;
      return true;
    case NANOPDF_ENCRYPTION_AES_256:
      *out_algorithm = nanopdf::EncryptionAlgorithm::AES_256;
      return true;
  }

  return false;
}

bool map_encryption_algorithm_to_c(
    nanopdf::EncryptionAlgorithm algorithm,
    nanopdf_encryption_algorithm* out_algorithm) {
  if (!out_algorithm) {
    return false;
  }

  switch (algorithm) {
    case nanopdf::EncryptionAlgorithm::None:
      *out_algorithm = NANOPDF_ENCRYPTION_NONE;
      return true;
    case nanopdf::EncryptionAlgorithm::RC4_40:
      *out_algorithm = NANOPDF_ENCRYPTION_RC4_40;
      return true;
    case nanopdf::EncryptionAlgorithm::RC4_128:
      *out_algorithm = NANOPDF_ENCRYPTION_RC4_128;
      return true;
    case nanopdf::EncryptionAlgorithm::AES_128:
      *out_algorithm = NANOPDF_ENCRYPTION_AES_128;
      return true;
    case nanopdf::EncryptionAlgorithm::AES_256:
      *out_algorithm = NANOPDF_ENCRYPTION_AES_256;
      return true;
  }

  return false;
}

bool map_watermark_position(
    nanopdf_watermark_position position,
    nanopdf::WatermarkPosition* out_position) {
  if (!out_position) {
    return false;
  }

  switch (position) {
    case NANOPDF_WATERMARK_POSITION_CENTER:
      *out_position = nanopdf::WatermarkPosition::Center;
      return true;
    case NANOPDF_WATERMARK_POSITION_TOP_LEFT:
      *out_position = nanopdf::WatermarkPosition::TopLeft;
      return true;
    case NANOPDF_WATERMARK_POSITION_TOP_RIGHT:
      *out_position = nanopdf::WatermarkPosition::TopRight;
      return true;
    case NANOPDF_WATERMARK_POSITION_BOTTOM_LEFT:
      *out_position = nanopdf::WatermarkPosition::BottomLeft;
      return true;
    case NANOPDF_WATERMARK_POSITION_BOTTOM_RIGHT:
      *out_position = nanopdf::WatermarkPosition::BottomRight;
      return true;
    case NANOPDF_WATERMARK_POSITION_TILED:
      *out_position = nanopdf::WatermarkPosition::Tiled;
      return true;
  }

  return false;
}

bool map_watermark_layer(
    nanopdf_watermark_layer layer,
    nanopdf::WatermarkLayer* out_layer) {
  if (!out_layer) {
    return false;
  }

  switch (layer) {
    case NANOPDF_WATERMARK_LAYER_BACKGROUND:
      *out_layer = nanopdf::WatermarkLayer::Background;
      return true;
    case NANOPDF_WATERMARK_LAYER_FOREGROUND:
      *out_layer = nanopdf::WatermarkLayer::Foreground;
      return true;
  }

  return false;
}

bool map_header_footer_content(
    nanopdf_header_footer_content content,
    nanopdf::HeaderFooterContent* out_content) {
  if (!out_content) {
    return false;
  }

  switch (content) {
    case NANOPDF_HEADER_FOOTER_CONTENT_NONE:
      *out_content = nanopdf::HeaderFooterContent::None;
      return true;
    case NANOPDF_HEADER_FOOTER_CONTENT_TEXT:
      *out_content = nanopdf::HeaderFooterContent::Text;
      return true;
    case NANOPDF_HEADER_FOOTER_CONTENT_PAGE_NUMBER:
      *out_content = nanopdf::HeaderFooterContent::PageNumber;
      return true;
    case NANOPDF_HEADER_FOOTER_CONTENT_TOTAL_PAGES:
      *out_content = nanopdf::HeaderFooterContent::TotalPages;
      return true;
    case NANOPDF_HEADER_FOOTER_CONTENT_DATE:
      *out_content = nanopdf::HeaderFooterContent::Date;
      return true;
  }

  return false;
}

bool map_bates_position(
    nanopdf_bates_position position,
    nanopdf::BatesPosition* out_position) {
  if (!out_position) {
    return false;
  }

  switch (position) {
    case NANOPDF_BATES_POSITION_TOP_LEFT:
      *out_position = nanopdf::BatesPosition::TopLeft;
      return true;
    case NANOPDF_BATES_POSITION_TOP_CENTER:
      *out_position = nanopdf::BatesPosition::TopCenter;
      return true;
    case NANOPDF_BATES_POSITION_TOP_RIGHT:
      *out_position = nanopdf::BatesPosition::TopRight;
      return true;
    case NANOPDF_BATES_POSITION_BOTTOM_LEFT:
      *out_position = nanopdf::BatesPosition::BottomLeft;
      return true;
    case NANOPDF_BATES_POSITION_BOTTOM_CENTER:
      *out_position = nanopdf::BatesPosition::BottomCenter;
      return true;
    case NANOPDF_BATES_POSITION_BOTTOM_RIGHT:
      *out_position = nanopdf::BatesPosition::BottomRight;
      return true;
  }

  return false;
}

bool map_writer_text_align(
    nanopdf_writer_text_align alignment,
    nanopdf::TextAlign* out_alignment) {
  if (!out_alignment) {
    return false;
  }

  switch (alignment) {
    case NANOPDF_WRITER_TEXT_ALIGN_LEFT:
      *out_alignment = nanopdf::TextAlign::Left;
      return true;
    case NANOPDF_WRITER_TEXT_ALIGN_CENTER:
      *out_alignment = nanopdf::TextAlign::Center;
      return true;
    case NANOPDF_WRITER_TEXT_ALIGN_RIGHT:
      *out_alignment = nanopdf::TextAlign::Right;
      return true;
    case NANOPDF_WRITER_TEXT_ALIGN_JUSTIFIED:
      *out_alignment = nanopdf::TextAlign::Justified;
      return true;
  }

  return false;
}

bool map_table_cell_align(
    nanopdf_table_cell_align alignment,
    nanopdf::CellAlign* out_alignment) {
  if (!out_alignment) {
    return false;
  }

  switch (alignment) {
    case NANOPDF_TABLE_CELL_ALIGN_LEFT:
      *out_alignment = nanopdf::CellAlign::Left;
      return true;
    case NANOPDF_TABLE_CELL_ALIGN_CENTER:
      *out_alignment = nanopdf::CellAlign::Center;
      return true;
    case NANOPDF_TABLE_CELL_ALIGN_RIGHT:
      *out_alignment = nanopdf::CellAlign::Right;
      return true;
  }

  return false;
}

bool map_table_cell_valign(
    nanopdf_table_cell_valign valign,
    nanopdf::CellVAlign* out_valign) {
  if (!out_valign) {
    return false;
  }

  switch (valign) {
    case NANOPDF_TABLE_CELL_VALIGN_MIDDLE:
      *out_valign = nanopdf::CellVAlign::Middle;
      return true;
    case NANOPDF_TABLE_CELL_VALIGN_TOP:
      *out_valign = nanopdf::CellVAlign::Top;
      return true;
    case NANOPDF_TABLE_CELL_VALIGN_BOTTOM:
      *out_valign = nanopdf::CellVAlign::Bottom;
      return true;
  }

  return false;
}

nanopdf_status validate_page_index(
    nanopdf_writer* writer,
    uint32_t page_index,
    const char* message) {
  nanopdf_status status = validate_writer(writer, message);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (static_cast<int>(page_index) >= writer->writer.get_page_count()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index is out of range");
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_color_stops(
    nanopdf_context* context,
    const nanopdf_color_stop* stops,
    size_t stop_count,
    std::vector<nanopdf::ColorStop>* out_stops) {
  double previous_position = -1.0;

  if (!out_stops) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "gradient stop output is null");
  }
  out_stops->clear();

  if (stop_count < 2 || !stops) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "gradient requires at least two color stops");
  }

  out_stops->reserve(stop_count);
  for (size_t i = 0; i < stop_count; ++i) {
    const nanopdf_color_stop& stop = stops[i];
    nanopdf::ColorStop cpp_stop;
    if (stop.position < 0.0 || stop.position > 1.0 ||
        stop.position < previous_position) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "gradient stop positions must be sorted in the range [0,1]");
    }
    cpp_stop.position = stop.position;
    cpp_stop.r = stop.r;
    cpp_stop.g = stop.g;
    cpp_stop.b = stop.b;
    out_stops->push_back(cpp_stop);
    previous_position = stop.position;
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_string_vector(
    nanopdf_context* context,
    const char* const* values,
    size_t value_count,
    const char* null_value_message,
    std::vector<std::string>* out_values) {
  if (!out_values) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "string vector output is null");
  }

  out_values->clear();
  if (value_count == 0) {
    return NANOPDF_STATUS_OK;
  }
  if (!values) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "string vector input is null");
  }

  out_values->reserve(value_count);
  for (size_t i = 0; i < value_count; ++i) {
    if (!values[i]) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          null_value_message);
    }
    out_values->emplace_back(values[i]);
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_quad_points(
    nanopdf_context* context,
    const nanopdf_quad_points* quads,
    size_t quad_count,
    std::vector<nanopdf::QuadPoints>* out_quads) {
  if (!out_quads) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "quad points output is null");
  }

  out_quads->clear();
  if (quad_count == 0 || !quads) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text markup requires at least one quad");
  }

  out_quads->reserve(quad_count);
  for (size_t i = 0; i < quad_count; ++i) {
    nanopdf::QuadPoints cpp_quad;
    cpp_quad.x1 = quads[i].x1;
    cpp_quad.y1 = quads[i].y1;
    cpp_quad.x2 = quads[i].x2;
    cpp_quad.y2 = quads[i].y2;
    cpp_quad.x3 = quads[i].x3;
    cpp_quad.y3 = quads[i].y3;
    cpp_quad.x4 = quads[i].x4;
    cpp_quad.y4 = quads[i].y4;
    out_quads->push_back(cpp_quad);
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_writer_table_cell_style(
    nanopdf_context* context,
    const nanopdf_writer_table_cell_style* style,
    nanopdf::CellStyle* out_style) {
  if (!out_style) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell style output is null");
  }

  *out_style = nanopdf::CellStyle();
  if (!style) {
    return NANOPDF_STATUS_OK;
  }
  if (style->font_size < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell font size must be non-negative");
  }
  if (style->border_width < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell border width must be non-negative");
  }
  if (!map_table_cell_align(style->align, &out_style->align)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table cell alignment");
  }
  if (!map_table_cell_valign(style->valign, &out_style->valign)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table cell vertical alignment");
  }

  out_style->font_name = style->font_name ? style->font_name : "";
  out_style->font_size = style->font_size;
  out_style->text_r = style->text_r;
  out_style->text_g = style->text_g;
  out_style->text_b = style->text_b;
  out_style->padding_left = style->padding_left;
  out_style->padding_right = style->padding_right;
  out_style->padding_top = style->padding_top;
  out_style->padding_bottom = style->padding_bottom;
  out_style->has_background = style->has_background != 0;
  out_style->bg_r = style->bg_r;
  out_style->bg_g = style->bg_g;
  out_style->bg_b = style->bg_b;
  out_style->override_border = style->override_border != 0;
  out_style->border_width = style->border_width;
  out_style->border_r = style->border_r;
  out_style->border_g = style->border_g;
  out_style->border_b = style->border_b;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_writer_table_cells(
    nanopdf_context* context,
    const nanopdf_writer_table_cell* cells,
    size_t cell_count,
    std::vector<nanopdf::WriterTableCell>* out_cells) {
  nanopdf_status status;

  if (!out_cells) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell output is null");
  }
  if (cell_count == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table row must contain at least one cell");
  }
  if (!cells) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell pointer is null");
  }

  out_cells->clear();
  out_cells->reserve(cell_count);
  for (size_t i = 0; i < cell_count; ++i) {
    nanopdf::WriterTableCell cpp_cell;
    if (cells[i].colspan < 0 || cells[i].rowspan < 0) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "table cell spans must be non-negative");
    }

    cpp_cell.text = cells[i].text ? cells[i].text : "";
    cpp_cell.colspan = (cells[i].colspan > 0) ? cells[i].colspan : 1;
    cpp_cell.rowspan = (cells[i].rowspan > 0) ? cells[i].rowspan : 1;
    status = build_writer_table_cell_style(
        context, &cells[i].style, &cpp_cell.style);
    if (status != NANOPDF_STATUS_OK) {
      return status;
    }
    out_cells->push_back(cpp_cell);
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_writer_table_row(
    nanopdf_context* context,
    const nanopdf_writer_table_row* row,
    nanopdf::TableRow* out_row) {
  nanopdf_status status;

  if (!out_row) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table row output is null");
  }
  if (!row) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table row config is null");
  }
  if (row->height < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table row height must be non-negative");
  }

  out_row->cells.clear();
  out_row->height = row->height;
  out_row->row_style = nanopdf::CellStyle();

  status = build_writer_table_cells(
      context, row->cells, row->cell_count, &out_row->cells);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_cell_style(
      context, &row->row_style, &out_row->row_style);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_text_markup_config(
    nanopdf_context* context,
    const nanopdf_text_markup_config* config,
    nanopdf::TextMarkupConfig* out_config) {
  nanopdf_status status;

  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text markup output is null");
  }
  if (!config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text markup config is null");
  }
  if (config->alpha < 0.0 || config->alpha > 1.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text markup alpha must be in the range [0,1]");
  }
  if (!map_markup_type(config->type, &out_config->type)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid text markup type");
  }

  status = build_quad_points(
      context, config->quads, config->quad_count, &out_config->quads);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  out_config->r = config->r;
  out_config->g = config->g;
  out_config->b = config->b;
  out_config->alpha = config->alpha;
  out_config->author = config->author ? config->author : "";
  out_config->contents = config->contents ? config->contents : "";
  out_config->print = config->print != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_link_config(
    nanopdf_context* context,
    const nanopdf_link_config* config,
    bool require_positive_size,
    nanopdf::LinkConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "link output is null");
  }
  if (!config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "link config is null");
  }
  if (require_positive_size) {
    if (config->width <= 0.0 || config->height <= 0.0) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "link rectangle dimensions must be positive");
    }
  } else if (config->width < 0.0 || config->height < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "link rectangle dimensions must be non-negative");
  }
  if (!map_link_action(config->action, &out_config->action)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid link action");
  }
  if (config->action == NANOPDF_LINK_ACTION_URI &&
      (!config->uri || !config->uri[0])) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "URI link config requires a non-empty uri");
  }

  out_config->x = config->x;
  out_config->y = config->y;
  out_config->width = config->width;
  out_config->height = config->height;
  out_config->uri = config->uri ? config->uri : "";
  out_config->dest_page = static_cast<int>(config->dest_page_index);
  out_config->dest_y = config->dest_y;
  out_config->show_border = config->show_border != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_signature_field_config(
    nanopdf_context* context,
    const nanopdf_signature_field_config* config,
    nanopdf::SignatureFieldConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature field output is null");
  }
  if (!config || !config->name || !config->name[0]) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature field config is invalid");
  }
  if (config->width < 0.0 || config->height < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature field size must be non-negative");
  }
  if (!map_signature_filter(config->filter, &out_config->filter)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid signature filter");
  }
  if (!map_signature_subfilter(config->subfilter, &out_config->subfilter)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid signature subfilter");
  }
  if (!map_mdp_permissions(config->mdp_permissions, &out_config->mdp_permissions)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid MDP permissions");
  }

  out_config->name = config->name;
  out_config->page = static_cast<int>(config->page_index);
  out_config->x = config->x;
  out_config->y = config->y;
  out_config->width = config->width;
  out_config->height = config->height;
  out_config->visible = config->visible != 0;
  out_config->reason = config->reason ? config->reason : "";
  out_config->location = config->location ? config->location : "";
  out_config->contact_info = config->contact_info ? config->contact_info : "";
  out_config->is_certification = config->is_certification != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_page_label(
    nanopdf_context* context,
    const nanopdf_page_label* label,
    nanopdf::PageLabel* out_label) {
  if (!label || !out_label) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid page label configuration");
  }
  if (!map_page_label_style(label->style, &out_label->style)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid page label style");
  }

  out_label->prefix = label->prefix ? label->prefix : "";
  out_label->start_value = label->start_value == 0 ? 1 : label->start_value;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_named_destination(
    nanopdf_context* context,
    const nanopdf_named_destination* destination,
    nanopdf::NamedDestination* out_destination) {
  if (!destination || !out_destination || !destination->name ||
      destination->name[0] == '\0') {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid named destination configuration");
  }
  if (destination->position_count > 4) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "named destination position count must be between 0 and 4");
  }
  if (destination->position_count > 0 && !destination->position) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "named destination position array is null");
  }

  out_destination->name = destination->name;
  out_destination->page_number = destination->page_index;
  out_destination->fit_type =
      (destination->fit_type && destination->fit_type[0])
          ? destination->fit_type
          : "Fit";
  out_destination->position.clear();
  for (size_t i = 0; i < destination->position_count; ++i) {
    if (!std::isfinite(destination->position[i])) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "named destination positions must be finite");
    }
    out_destination->position.push_back(destination->position[i]);
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status build_viewer_preferences(
    nanopdf_context* context,
    const nanopdf_viewer_preferences* preferences,
    nanopdf::ViewerPreferences* out_preferences) {
  if (!preferences || !out_preferences) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid viewer preferences configuration");
  }

  out_preferences->hide_toolbar = preferences->hide_toolbar != 0;
  out_preferences->hide_menubar = preferences->hide_menubar != 0;
  out_preferences->hide_window_ui = preferences->hide_window_ui != 0;
  out_preferences->fit_window = preferences->fit_window != 0;
  out_preferences->center_window = preferences->center_window != 0;
  out_preferences->display_doc_title = preferences->display_doc_title != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_output_intent(
    nanopdf_context* context,
    const nanopdf_output_intent* output_intent,
    nanopdf::OutputIntentConfig* out_output_intent) {
  if (!output_intent || !out_output_intent ||
      !output_intent->dest_output_profile.data ||
      output_intent->dest_output_profile.size == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid output intent configuration");
  }

  out_output_intent->subtype =
      (output_intent->subtype && output_intent->subtype[0])
          ? output_intent->subtype
          : "GTS_PDFA1";
  out_output_intent->output_condition =
      output_intent->output_condition ? output_intent->output_condition : "";
  out_output_intent->output_condition_id =
      output_intent->output_condition_identifier
          ? output_intent->output_condition_identifier
          : "";
  out_output_intent->registry_name =
      output_intent->registry_name ? output_intent->registry_name : "";
  out_output_intent->info =
      output_intent->info ? output_intent->info : "";
  out_output_intent->dest_output_profile.assign(
      static_cast<const uint8_t*>(output_intent->dest_output_profile.data),
      static_cast<const uint8_t*>(output_intent->dest_output_profile.data) +
          output_intent->dest_output_profile.size);
  out_output_intent->color_components =
      output_intent->color_components <= 0
          ? 3
          : output_intent->color_components;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_mark_info(
    nanopdf_context* context,
    const nanopdf_mark_info* mark_info,
    nanopdf::MarkInfoConfig* out_mark_info) {
  if (!mark_info || !out_mark_info) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid mark info configuration");
  }

  out_mark_info->marked = mark_info->marked != 0;
  out_mark_info->suspects = mark_info->suspects != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_bookmark_config(
    nanopdf_context* context,
    const nanopdf_bookmark_config* config,
    nanopdf::BookmarkConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "bookmark output is null");
  }
  if (!config || !config->title || !config->title[0]) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "bookmark config is invalid");
  }

  out_config->title = config->title;
  out_config->page_index = static_cast<int>(config->page_index);
  out_config->y_position = config->dest_y;
  out_config->open = config->open != 0;
  out_config->bold = config->bold != 0;
  out_config->italic = config->italic != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_attachment_config(
    nanopdf_context* context,
    const nanopdf_attachment_config* config,
    nanopdf::AttachmentConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment output is null");
  }
  if (!config || !config->filename || !config->filename[0] ||
      (config->size > 0 && !config->data)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment config is invalid");
  }

  out_config->filename = config->filename;
  out_config->description = config->description ? config->description : "";
  out_config->mime_type = config->mime_type ? config->mime_type : "";
  out_config->compress = config->use_compression != 0;
  out_config->data.clear();
  if (config->size > 0) {
    const uint8_t* bytes = static_cast<const uint8_t*>(config->data);
    out_config->data.assign(bytes, bytes + config->size);
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_layer_config(
    nanopdf_context* context,
    const nanopdf_layer_config* config,
    nanopdf::LayerConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "layer output is null");
  }
  if (!config || !config->name || !config->name[0]) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "layer config is invalid");
  }

  out_config->name = config->name;
  out_config->visible = config->visible != 0;
  out_config->printable = config->printable != 0;
  out_config->locked = config->locked != 0;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_writer_encryption_config(
    nanopdf_context* context,
    const nanopdf_writer_encryption_config* config,
    nanopdf::EncryptionConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer encryption output is null");
  }
  if (!config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer encryption config is null");
  }
  if (!map_encryption_algorithm(config->algorithm, &out_config->algorithm)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid encryption algorithm");
  }
  if (out_config->algorithm != nanopdf::EncryptionAlgorithm::None &&
      (!config->owner_password || !config->owner_password[0])) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "owner password is required for encryption");
  }

  out_config->user_password = config->user_password ? config->user_password : "";
  out_config->owner_password = config->owner_password ? config->owner_password : "";
  out_config->permissions.allow_print = config->allow_print != 0;
  out_config->permissions.allow_modify = config->allow_modify != 0;
  out_config->permissions.allow_copy = config->allow_copy != 0;
  out_config->permissions.allow_annotate = config->allow_annotate != 0;
  out_config->permissions.allow_fill_forms = config->allow_fill_forms != 0;
  out_config->permissions.allow_accessibility = config->allow_accessibility != 0;
  out_config->permissions.allow_assemble = config->allow_assemble != 0;
  out_config->permissions.allow_print_high_quality =
      config->allow_print_high_quality != 0;
  out_config->encrypt_metadata = config->encrypt_metadata != 0;
  return NANOPDF_STATUS_OK;
}

void fill_signature_placeholder(
    const nanopdf::SignaturePlaceholder& source,
    nanopdf_signature_placeholder* destination) {
  destination->field_name = source.field_name.c_str();
  destination->contents_offset = source.contents_offset;
  destination->contents_length = source.contents_length;
  destination->byte_range_offset = source.byte_range_offset;
  destination->byte_range_0 = source.byte_range.size() > 0 ? source.byte_range[0] : 0;
  destination->byte_range_1 = source.byte_range.size() > 1 ? source.byte_range[1] : 0;
  destination->byte_range_2 = source.byte_range.size() > 2 ? source.byte_range[2] : 0;
  destination->byte_range_3 = source.byte_range.size() > 3 ? source.byte_range[3] : 0;
}

nanopdf_status build_text_watermark_config(
    nanopdf_context* context,
    const char* text,
    const char* font_name,
    double font_size,
    double r,
    double g,
    double b,
    double alpha,
    double rotation_degrees,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    nanopdf::TextWatermarkConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text watermark config output is null");
  }
  if (!text || !text[0] || font_size <= 0.0 || alpha < 0.0 || alpha > 1.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid text watermark arguments");
  }
  if (!map_watermark_position(position, &out_config->position)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid watermark position");
  }
  if (!map_watermark_layer(layer, &out_config->layer)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid watermark layer");
  }

  out_config->text = text;
  out_config->font_name = font_name ? font_name : "";
  out_config->font_size = font_size;
  out_config->r = r;
  out_config->g = g;
  out_config->b = b;
  out_config->alpha = alpha;
  out_config->rotation = rotation_degrees;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_image_watermark_config(
    nanopdf_context* context,
    const char* image_name,
    double alpha,
    double scale,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    double offset_x,
    double offset_y,
    nanopdf::ImageWatermarkConfig* out_config) {
  if (!out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "image watermark config output is null");
  }
  if (!image_name || !image_name[0] || scale <= 0.0 || alpha < 0.0 || alpha > 1.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image watermark arguments");
  }
  if (!map_watermark_position(position, &out_config->position)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid watermark position");
  }
  if (!map_watermark_layer(layer, &out_config->layer)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid watermark layer");
  }

  out_config->image_name = image_name;
  out_config->alpha = alpha;
  out_config->scale = scale;
  out_config->offset_x = offset_x;
  out_config->offset_y = offset_y;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_hf_section(
    nanopdf_context* context,
    const nanopdf_header_footer_section* section,
    nanopdf::HFSection* out_section) {
  if (!section || !out_section) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "header/footer section is null");
  }
  if (!map_header_footer_content(section->type, &out_section->type)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid header/footer content type");
  }

  out_section->text = section->text ? section->text : "";
  out_section->date_format = section->date_format ? section->date_format : "";
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_header_config(
    nanopdf_context* context,
    const nanopdf_header_config* config,
    nanopdf::HeaderConfig* out_config) {
  nanopdf_status status;

  if (!config || !out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "header config is null");
  }
  if (config->font_size <= 0.0 || config->margin_top < 0.0 ||
      config->margin_left < 0.0 || config->margin_right < 0.0 ||
      (config->draw_line != 0 && config->line_width <= 0.0)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid header config");
  }

  status = build_hf_section(context, &config->left, &out_config->left);
  if (status != NANOPDF_STATUS_OK) return status;
  status = build_hf_section(context, &config->center, &out_config->center);
  if (status != NANOPDF_STATUS_OK) return status;
  status = build_hf_section(context, &config->right, &out_config->right);
  if (status != NANOPDF_STATUS_OK) return status;

  out_config->font_name = config->font_name ? config->font_name : "";
  out_config->font_size = config->font_size;
  out_config->r = config->r;
  out_config->g = config->g;
  out_config->b = config->b;
  out_config->margin_top = config->margin_top;
  out_config->margin_left = config->margin_left;
  out_config->margin_right = config->margin_right;
  out_config->draw_line = config->draw_line != 0;
  out_config->line_width = config->line_width;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_footer_config(
    nanopdf_context* context,
    const nanopdf_footer_config* config,
    nanopdf::FooterConfig* out_config) {
  nanopdf_status status;

  if (!config || !out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "footer config is null");
  }
  if (config->font_size <= 0.0 || config->margin_bottom < 0.0 ||
      config->margin_left < 0.0 || config->margin_right < 0.0 ||
      (config->draw_line != 0 && config->line_width <= 0.0)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid footer config");
  }

  status = build_hf_section(context, &config->left, &out_config->left);
  if (status != NANOPDF_STATUS_OK) return status;
  status = build_hf_section(context, &config->center, &out_config->center);
  if (status != NANOPDF_STATUS_OK) return status;
  status = build_hf_section(context, &config->right, &out_config->right);
  if (status != NANOPDF_STATUS_OK) return status;

  out_config->font_name = config->font_name ? config->font_name : "";
  out_config->font_size = config->font_size;
  out_config->r = config->r;
  out_config->g = config->g;
  out_config->b = config->b;
  out_config->margin_bottom = config->margin_bottom;
  out_config->margin_left = config->margin_left;
  out_config->margin_right = config->margin_right;
  out_config->draw_line = config->draw_line != 0;
  out_config->line_width = config->line_width;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_bates_config(
    nanopdf_context* context,
    const nanopdf_bates_config* config,
    nanopdf::BatesConfig* out_config) {
  if (!config || !out_config) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "bates config is null");
  }
  if (config->digits <= 0 || config->increment == 0 ||
      config->font_size <= 0.0 || config->margin_x < 0.0 ||
      config->margin_y < 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid bates config");
  }
  if (!map_bates_position(config->position, &out_config->position)) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid bates position");
  }

  out_config->prefix = config->prefix ? config->prefix : "";
  out_config->suffix = config->suffix ? config->suffix : "";
  out_config->start_number = config->start_number;
  out_config->digits = config->digits;
  out_config->increment = config->increment;
  out_config->font_name = config->font_name ? config->font_name : "";
  out_config->font_size = config->font_size;
  out_config->r = config->r;
  out_config->g = config->g;
  out_config->b = config->b;
  out_config->margin_x = config->margin_x;
  out_config->margin_y = config->margin_y;
  return NANOPDF_STATUS_OK;
}

nanopdf_status build_writer_text_style(
    nanopdf_context* context,
    const nanopdf_writer_text_style* style,
    nanopdf::TextStyle* out_style) {
  if (!style || !out_style) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text style is null");
  }
  if (style->font_size <= 0.0 || style->line_height <= 0.0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid writer text style");
  }

  out_style->font_name = style->font_name ? style->font_name : "";
  out_style->font_size = style->font_size;
  out_style->r = style->r;
  out_style->g = style->g;
  out_style->b = style->b;
  out_style->line_height = style->line_height;
  out_style->letter_spacing = style->letter_spacing;
  out_style->word_spacing = style->word_spacing;
  return NANOPDF_STATUS_OK;
}

template <typename Fn>
nanopdf_status with_writer(
    nanopdf_writer* writer,
    const char* message,
    Fn&& fn) {
  nanopdf_status status = validate_writer(writer, message);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  fn(writer->writer);
  return clear_success(writer->context);
}

template <typename Fn>
nanopdf_status with_page(
    nanopdf_page_builder* page,
    const char* message,
    Fn&& fn) {
  nanopdf_status status = validate_page(page, message);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  fn(*page->builder);
  return clear_success(page->context);
}

template <typename Fn>
nanopdf_status create_object(
    nanopdf_context* context,
    nanopdf_object** out_object,
    Fn&& fn) {
  nanopdf_object* object = NULL;

  if (!context || !out_object) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf object creation");
  }

  *out_object = NULL;
  object = new (std::nothrow) nanopdf_object();
  if (!object) {
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate nanopdf object");
  }

  object->context = context;
  fn(object->value);
  *out_object = object;
  return clear_success(context);
}

std::string format_number(double number) {
  char buffer[64];
  if (number == static_cast<int64_t>(number) &&
      std::abs(number) < 1e15) {
    std::snprintf(buffer, sizeof(buffer), "%lld",
                  static_cast<long long>(number));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.6f", number);
    char* end = buffer + std::strlen(buffer) - 1;
    while (end > buffer && *end == '0') {
      *end-- = '\0';
    }
    if (end > buffer && *end == '.') {
      *end = '\0';
    }
  }
  return std::string(buffer);
}

std::string escape_pdf_string(const std::string& input) {
  std::string escaped;
  escaped.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
      case '(':
        escaped += "\\(";
        break;
      case ')':
        escaped += "\\)";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
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

std::string filename_from_path(const char* path) {
  std::string filename = path ? path : "";
  size_t pos = filename.find_last_of("/\\");
  if (pos != std::string::npos) {
    filename = filename.substr(pos + 1);
  }
  return filename;
}

bool serialize_value(
    const nanopdf::Value& value,
    std::string* out,
    std::string* error);

bool serialize_dictionary(
    const nanopdf::Dictionary& dict,
    size_t stream_length,
    bool include_length,
    std::string* out,
    std::string* error) {
  std::string serialized = "<<\n";

  for (const auto& item : dict) {
    std::string value_text;
    if (include_length && item.first == "Length") {
      continue;
    }
    if (!serialize_value(item.second, &value_text, error)) {
      return false;
    }
    serialized += "/";
    serialized += escape_pdf_name(item.first);
    serialized += " ";
    serialized += value_text;
    serialized += "\n";
  }

  if (include_length) {
    serialized += "/Length ";
    serialized += std::to_string(stream_length);
    serialized += "\n";
  }

  serialized += ">>";
  *out = std::move(serialized);
  return true;
}

bool serialize_value(
    const nanopdf::Value& value,
    std::string* out,
    std::string* error) {
  switch (value.type) {
    case nanopdf::Value::BOOLEAN:
      *out = value.boolean ? "true" : "false";
      return true;

    case nanopdf::Value::NUMBER:
      *out = format_number(value.number);
      return true;

    case nanopdf::Value::STRING: {
      bool has_binary = false;
      for (unsigned char ch : value.str) {
        if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') {
          has_binary = true;
          break;
        }
      }
      if (has_binary) {
        static const char hex[] = "0123456789ABCDEF";
        std::string hex_text;
        hex_text.reserve(value.str.size() * 2 + 2);
        hex_text += '<';
        for (unsigned char ch : value.str) {
          hex_text += hex[(ch >> 4) & 0x0f];
          hex_text += hex[ch & 0x0f];
        }
        hex_text += '>';
        *out = std::move(hex_text);
      } else {
        *out = "(" + escape_pdf_string(value.str) + ")";
      }
      return true;
    }

    case nanopdf::Value::NAME:
      *out = "/" + escape_pdf_name(value.name);
      return true;

    case nanopdf::Value::ARRAY: {
      std::string serialized = "[";
      for (size_t i = 0; i < value.array.size(); ++i) {
        std::string item_text;
        if (i > 0) {
          serialized += " ";
        }
        if (!serialize_value(value.array[i], &item_text, error)) {
          return false;
        }
        serialized += item_text;
      }
      serialized += "]";
      *out = std::move(serialized);
      return true;
    }

    case nanopdf::Value::DICTIONARY:
      return serialize_dictionary(value.dict, 0, false, out, error);

    case nanopdf::Value::REFERENCE:
      if (value.ref_object_number == 0) {
        if (error) {
          *error = "reference object number must be non-zero";
        }
        return false;
      }
      *out = std::to_string(value.ref_object_number) + " " +
             std::to_string(value.ref_generation_number) + " R";
      return true;

    case nanopdf::Value::NULL_OBJ:
    case nanopdf::Value::UNDEFINED:
      *out = "null";
      return true;

    case nanopdf::Value::STREAM:
      if (error) {
        *error = "stream objects must be added as indirect objects";
      }
      return false;
  }

  if (error) {
    *error = "unsupported PDF value type";
  }
  return false;
}

nanopdf_status set_write_error(
    nanopdf_context* context,
    const nanopdf::WriteResult& result,
    nanopdf_status fallback_status) {
  nanopdf_status status = fallback_status;

  switch (result.kind) {
    case nanopdf::ErrorKind::Malformed:
      status = NANOPDF_STATUS_MALFORMED;
      break;
    case nanopdf::ErrorKind::Unsupported:
      status = NANOPDF_STATUS_UNSUPPORTED;
      break;
    case nanopdf::ErrorKind::Encrypted:
      status = NANOPDF_STATUS_ENCRYPTED;
      break;
    case nanopdf::ErrorKind::IOError:
      status = NANOPDF_STATUS_IO_ERROR;
      break;
    case nanopdf::ErrorKind::Internal:
      status = NANOPDF_STATUS_INTERNAL_ERROR;
      break;
    case nanopdf::ErrorKind::None:
      break;
  }

  return set_error(
      context,
      status,
      result.error.empty() ? "PDF write failed" : result.error.c_str());
}

nanopdf_status set_load_existing_error(
    nanopdf_context* context,
    const std::string& error) {
  nanopdf_status status = NANOPDF_STATUS_MALFORMED;

  if (error.find("Failed to open file:") == 0 ||
      error.find("Failed to read file:") == 0) {
    status = NANOPDF_STATUS_IO_ERROR;
  }

  return set_error(
      context,
      status,
      error.empty() ? "failed to load existing PDF" : error.c_str());
}

nanopdf_status set_parse_error(
    nanopdf_context* context,
    const nanopdf::ParseResult& result,
    nanopdf_status fallback_status) {
  nanopdf_status status = fallback_status;

  switch (result.kind) {
    case nanopdf::ErrorKind::Malformed:
      status = NANOPDF_STATUS_MALFORMED;
      break;
    case nanopdf::ErrorKind::Unsupported:
      status = NANOPDF_STATUS_UNSUPPORTED;
      break;
    case nanopdf::ErrorKind::Encrypted:
      status = NANOPDF_STATUS_ENCRYPTED;
      break;
    case nanopdf::ErrorKind::IOError:
      status = NANOPDF_STATUS_IO_ERROR;
      break;
    case nanopdf::ErrorKind::Internal:
      status = NANOPDF_STATUS_INTERNAL_ERROR;
      break;
    case nanopdf::ErrorKind::None:
      break;
  }

  return set_error(
      context,
      status,
      result.error.empty() ? "failed to parse PDF" : result.error.c_str());
}

nanopdf_status build_page_indices(
    nanopdf_context* context,
    const int32_t* page_indices,
    size_t page_index_count,
    bool allow_empty,
    std::vector<int>* out_indices) {
  if (!out_indices) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index output is null");
  }

  out_indices->clear();
  if (page_index_count == 0) {
    if (allow_empty) {
      return NANOPDF_STATUS_OK;
    }
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index list is empty");
  }
  if (!page_indices) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index array is null");
  }

  out_indices->reserve(page_index_count);
  for (size_t i = 0; i < page_index_count; ++i) {
    if (page_indices[i] < 0) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "page indices must be non-negative");
    }
    out_indices->push_back(static_cast<int>(page_indices[i]));
  }

  return NANOPDF_STATUS_OK;
}

nanopdf_status parse_pdf_from_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    nanopdf::Pdf* out_pdf,
    const char* empty_message,
    const char* parse_message) {
  nanopdf::ParseResult result;

  if (!context || !out_pdf) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid parse_pdf_from_memory arguments");
  }
  if (!data || size == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        empty_message);
  }

  result = nanopdf::parse_pdf(
      static_cast<const uint8_t*>(data), size, out_pdf);
  if (!result.success) {
    if (!result.error.empty()) {
      return set_parse_error(context, result, NANOPDF_STATUS_MALFORMED);
    }
    return set_error(context, NANOPDF_STATUS_MALFORMED, parse_message);
  }
  return NANOPDF_STATUS_OK;
}

nanopdf_status copy_output_buffer(
    nanopdf_context* context,
    const std::vector<uint8_t>& pdf_data,
    void** out_data,
    size_t* out_size) {
  void* output = NULL;

  if (!context || !out_data || !out_size) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid output buffer arguments");
  }

  *out_data = NULL;
  *out_size = 0;
  if (!pdf_data.empty()) {
    output = nanopdf__allocator_alloc(&context->allocator, pdf_data.size());
    if (!output) {
      return set_error(
          context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate PDF output buffer");
    }
    std::memcpy(output, pdf_data.data(), pdf_data.size());
  }

  *out_data = output;
  *out_size = pdf_data.size();
  return NANOPDF_STATUS_OK;
}

}  // namespace

extern "C" {

nanopdf_status nanopdf_writer_create(
    nanopdf_context* context,
    nanopdf_writer** out_writer) {
  nanopdf_writer* writer = NULL;

  if (!context || !out_writer) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_create");
  }

  *out_writer = NULL;
  writer = new (std::nothrow) nanopdf_writer();
  if (!writer) {
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate nanopdf writer");
  }

  writer->context = context;
  *out_writer = writer;
  return clear_success(context);
}

void nanopdf_writer_destroy(nanopdf_writer* writer) {
  delete writer;
}

nanopdf_status nanopdf_writer_load_existing_file(
    nanopdf_writer* writer,
    const char* path) {
  std::string error;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for load_existing_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0]) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "existing PDF path is empty");
  }

  if (!writer->writer.load_existing(path, &error)) {
    return set_load_existing_error(writer->context, error);
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_load_existing_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size) {
  std::string error;
  std::vector<uint8_t> bytes;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for load_existing_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "existing PDF data is null or empty");
  }

  bytes.assign(
      static_cast<const uint8_t*>(data),
      static_cast<const uint8_t*>(data) + size);
  if (!writer->writer.load_existing(bytes, &error)) {
    return set_load_existing_error(writer->context, error);
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_import_pages_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    const int32_t* page_indices,
    size_t page_index_count,
    int32_t* out_imported_page_count) {
  nanopdf::Pdf source_pdf;
  std::vector<int> cpp_page_indices;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for import_pages_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (out_imported_page_count) {
    *out_imported_page_count = 0;
  }

  status = parse_pdf_from_memory(
      writer->context,
      data,
      size,
      &source_pdf,
      "source PDF data is null or empty",
      "failed to parse source PDF for page import");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_page_indices(
      writer->context, page_indices, page_index_count, true, &cpp_page_indices);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (!cpp_page_indices.empty()) {
    const int total_pages = static_cast<int>(source_pdf.catalog.pages.size());
    for (int page_index : cpp_page_indices) {
      if (page_index >= total_pages) {
        return set_error(
            writer->context,
            NANOPDF_STATUS_INVALID_ARGUMENT,
            "page index is out of range for import_pages_from_memory");
      }
    }
  }

  int imported_page_count = writer->writer.import_pages_from(source_pdf, cpp_page_indices);
  if (imported_page_count < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to import pages from source PDF");
  }

  if (out_imported_page_count) {
    *out_imported_page_count = static_cast<int32_t>(imported_page_count);
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_has_existing_pdf(
    nanopdf_writer* writer,
    int* out_has_existing_pdf) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for has_existing_pdf");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_has_existing_pdf) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "existing PDF output is null");
  }

  *out_has_existing_pdf = writer->writer.has_existing_pdf() ? 1 : 0;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_revision_count(
    nanopdf_writer* writer,
    int32_t* out_revision_count) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_revision_count");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_revision_count) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "revision count output is null");
  }

  *out_revision_count = static_cast<int32_t>(writer->writer.get_revision_count());
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_version(
    nanopdf_writer* writer,
    nanopdf_pdf_version* out_version) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_version");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_version) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output version pointer is null");
  }
  if (!map_cpp_pdf_version(writer->writer.get_version(), out_version)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to map PDF version");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_page_count(
    nanopdf_writer* writer,
    int32_t* out_page_count) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_page_count");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_page_count) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output page count pointer is null");
  }

  *out_page_count = static_cast<int32_t>(writer->writer.get_page_count());
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_page_label(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_page_label* label) {
  nanopdf::PageLabel cpp_label;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_page_label");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_page_label(writer->context, label, &cpp_label);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.set_page_label(page_index, cpp_label);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_page_labels(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_page_labels",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_page_labels();
      });
}

nanopdf_status nanopdf_writer_add_named_destination(
    nanopdf_writer* writer,
    const nanopdf_named_destination* destination) {
  nanopdf::NamedDestination cpp_destination;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_named_destination");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_named_destination(
      writer->context, destination, &cpp_destination);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.add_named_destination(cpp_destination);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_named_destinations(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_named_destinations",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_named_destinations();
      });
}

nanopdf_status nanopdf_writer_set_open_action_named_destination(
    nanopdf_writer* writer,
    const char* destination_name) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_open_action_named_destination");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!destination_name || destination_name[0] == '\0') {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "open action destination name is empty");
  }

  writer->writer.set_open_action_named_destination(destination_name);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_open_action(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_open_action",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_open_action();
      });
}

nanopdf_status nanopdf_writer_set_page_layout(
    nanopdf_writer* writer,
    nanopdf_page_layout layout) {
  nanopdf::PageLayout cpp_layout;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_page_layout");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_page_layout(layout, &cpp_layout)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid page layout");
  }

  writer->writer.set_page_layout(cpp_layout);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_page_mode(
    nanopdf_writer* writer,
    nanopdf_page_mode mode) {
  nanopdf::PageMode cpp_mode;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_page_mode");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_page_mode(mode, &cpp_mode)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid page mode");
  }

  writer->writer.set_page_mode(cpp_mode);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_viewer_preferences(
    nanopdf_writer* writer,
    const nanopdf_viewer_preferences* preferences) {
  nanopdf::ViewerPreferences cpp_preferences;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_viewer_preferences");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_viewer_preferences(
      writer->context, preferences, &cpp_preferences);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.set_viewer_preferences(cpp_preferences);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_viewer_preferences(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_viewer_preferences",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_viewer_preferences();
      });
}

nanopdf_status nanopdf_writer_set_language(
    nanopdf_writer* writer,
    const char* language) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_language");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!language || language[0] == '\0') {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "language is empty");
  }

  writer->writer.set_language(language);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_language(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_language",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_language();
      });
}

nanopdf_status nanopdf_writer_set_xmp_metadata(
    nanopdf_writer* writer,
    const char* xml) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_xmp_metadata");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!xml || xml[0] == '\0') {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "XMP metadata XML is empty");
  }

  writer->writer.set_xmp_metadata(xml);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_xmp_metadata(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_xmp_metadata",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_xmp_metadata();
      });
}

nanopdf_status nanopdf_writer_add_output_intent(
    nanopdf_writer* writer,
    const nanopdf_output_intent* output_intent) {
  nanopdf::OutputIntentConfig cpp_output_intent;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_output_intent");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_output_intent(
      writer->context, output_intent, &cpp_output_intent);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.add_output_intent(cpp_output_intent);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_output_intents(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_output_intents",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_output_intents();
      });
}

nanopdf_status nanopdf_writer_set_mark_info(
    nanopdf_writer* writer,
    const nanopdf_mark_info* mark_info) {
  nanopdf::MarkInfoConfig cpp_mark_info;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_mark_info");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_mark_info(writer->context, mark_info, &cpp_mark_info);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.set_mark_info(cpp_mark_info);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_mark_info(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_mark_info",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_mark_info();
      });
}

nanopdf_status nanopdf_writer_set_trapped(
    nanopdf_writer* writer,
    nanopdf_trapped_state trapped) {
  nanopdf::TrappedState cpp_trapped;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_trapped");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_trapped_state(trapped, &cpp_trapped)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid trapped state");
  }

  writer->writer.set_trapped(cpp_trapped);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_trapped(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for clear_trapped",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.clear_trapped();
      });
}

nanopdf_status nanopdf_writer_set_title(
    nanopdf_writer* writer,
    const char* title) {
  return with_writer(
      writer,
      "invalid writer for set_title",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_title(title ? title : "");
      });
}

nanopdf_status nanopdf_writer_set_author(
    nanopdf_writer* writer,
    const char* author) {
  return with_writer(
      writer,
      "invalid writer for set_author",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_author(author ? author : "");
      });
}

nanopdf_status nanopdf_writer_set_subject(
    nanopdf_writer* writer,
    const char* subject) {
  return with_writer(
      writer,
      "invalid writer for set_subject",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_subject(subject ? subject : "");
      });
}

nanopdf_status nanopdf_writer_set_keywords(
    nanopdf_writer* writer,
    const char* keywords) {
  return with_writer(
      writer,
      "invalid writer for set_keywords",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_keywords(keywords ? keywords : "");
      });
}

nanopdf_status nanopdf_writer_set_creator(
    nanopdf_writer* writer,
    const char* creator) {
  return with_writer(
      writer,
      "invalid writer for set_creator",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_creator(creator ? creator : "");
      });
}

nanopdf_status nanopdf_writer_set_producer(
    nanopdf_writer* writer,
    const char* producer) {
  return with_writer(
      writer,
      "invalid writer for set_producer",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_producer(producer ? producer : "");
      });
}

nanopdf_status nanopdf_writer_set_custom_info(
    nanopdf_writer* writer,
    const char* key,
    const char* value) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_custom_info");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!key || key[0] == '\0') {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "custom info key is null or empty");
  }
  if (is_reserved_info_key(key)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "custom info key conflicts with a standard info field");
  }
  writer->writer.set_custom_info(key, value ? value : "");
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_custom_info(
    nanopdf_writer* writer,
    const char* key) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for clear_custom_info");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!key || key[0] == '\0') {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "custom info key is null or empty");
  }
  writer->writer.clear_custom_info(key);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_creation_date(
    nanopdf_writer* writer,
    const char* creation_date) {
  return with_writer(
      writer,
      "invalid writer for set_creation_date",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_creation_date(creation_date ? creation_date : "");
      });
}

nanopdf_status nanopdf_writer_set_modification_date(
    nanopdf_writer* writer,
    const char* modification_date) {
  return with_writer(
      writer,
      "invalid writer for set_modification_date",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.set_modification_date(modification_date ? modification_date : "");
      });
}

nanopdf_status nanopdf_writer_set_version(
    nanopdf_writer* writer,
    nanopdf_pdf_version version) {
  nanopdf::PdfVersion cpp_version;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_version");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_pdf_version(version, &cpp_version)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid PDF version");
  }

  writer->writer.set_version(cpp_version);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_document_id(
    nanopdf_writer* writer,
    const nanopdf_buffer_view* id1,
    const nanopdf_buffer_view* id2) {
  std::vector<uint8_t> cpp_id1;
  std::vector<uint8_t> cpp_id2;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_document_id");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!id1 || !id2 || !id1->data || !id2->data || id1->size == 0 || id2->size == 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_set_document_id");
  }

  cpp_id1.assign(
      static_cast<const uint8_t*>(id1->data),
      static_cast<const uint8_t*>(id1->data) + id1->size);
  cpp_id2.assign(
      static_cast<const uint8_t*>(id2->data),
      static_cast<const uint8_t*>(id2->data) + id2->size);
  writer->writer.set_document_id(cpp_id1, cpp_id2);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_generate_document_id(
    nanopdf_writer* writer) {
  return with_writer(
      writer,
      "invalid writer for generate_document_id",
      [&](nanopdf::PdfWriter& cpp_writer) {
        cpp_writer.generate_document_id();
      });
}

nanopdf_status nanopdf_writer_add_standard_font(
    nanopdf_writer* writer,
    nanopdf_standard_font font,
    char** out_font_name) {
  nanopdf::StandardFont cpp_font;
  std::string font_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_standard_font");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_font_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output font name pointer is null");
  }
  if (!map_standard_font(font, &cpp_font)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid standard font");
  }

  font_name = writer->writer.add_standard_font(cpp_font);
  return nanopdf__copy_owned_string(
      writer->context, font_name.c_str(), out_font_name);
}

nanopdf_status nanopdf_writer_add_truetype_font_from_file(
    nanopdf_writer* writer,
    const char* path,
    nanopdf_font_embedding embedding,
    char** out_font_name) {
  nanopdf::FontEmbedding cpp_embedding;
  std::string font_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_truetype_font_from_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0] || !out_font_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_truetype_font_from_file");
  }
  if (!map_font_embedding(embedding, &cpp_embedding)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid font embedding value");
  }

  font_name = writer->writer.add_truetype_font(path, cpp_embedding);
  if (font_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_IO_ERROR,
        "failed to load TrueType/OpenType font file");
  }

  return nanopdf__copy_owned_string(
      writer->context, font_name.c_str(), out_font_name);
}

nanopdf_status nanopdf_writer_add_truetype_font_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_font_embedding embedding,
    char** out_font_name) {
  nanopdf::FontEmbedding cpp_embedding;
  std::string font_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_truetype_font_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || !out_font_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_truetype_font_from_memory");
  }
  if (!map_font_embedding(embedding, &cpp_embedding)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid font embedding value");
  }

  font_name = writer->writer.add_truetype_font(
      static_cast<const uint8_t*>(data), size, cpp_embedding);
  if (font_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to load TrueType/OpenType font data");
  }

  return nanopdf__copy_owned_string(
      writer->context, font_name.c_str(), out_font_name);
}

nanopdf_status nanopdf_writer_has_font_resource(
    nanopdf_writer* writer,
    const char* font_name,
    int* out_has_font) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for has_font_resource");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !out_has_font) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_has_font_resource");
  }

  *out_has_font = writer->writer.has_font_resource(font_name) ? 1 : 0;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_font_metrics(
    nanopdf_writer* writer,
    const char* font_name,
    nanopdf_font_metrics_summary* out_metrics) {
  const nanopdf::FontMetrics* metrics = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_font_metrics");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !out_metrics) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_get_font_metrics");
  }

  metrics = writer->writer.get_font_metrics(font_name);
  if (!metrics) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "embedded font metrics were not found");
  }

  memset(out_metrics, 0, sizeof(*out_metrics));
  out_metrics->units_per_em = metrics->units_per_em;
  out_metrics->ascender = metrics->ascender;
  out_metrics->descender = metrics->descender;
  out_metrics->line_gap = metrics->line_gap;
  out_metrics->cap_height = metrics->cap_height;
  out_metrics->x_height = metrics->x_height;
  out_metrics->italic_angle = metrics->italic_angle;
  out_metrics->stem_v = metrics->stem_v;
  if (metrics->bbox.size() >= 4) {
    out_metrics->bbox[0] = metrics->bbox[0];
    out_metrics->bbox[1] = metrics->bbox[1];
    out_metrics->bbox[2] = metrics->bbox[2];
    out_metrics->bbox[3] = metrics->bbox[3];
  }
  out_metrics->flags = static_cast<uint32_t>(metrics->flags.to_int());
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_font_codepoint_width(
    nanopdf_writer* writer,
    const char* font_name,
    uint32_t codepoint,
    int32_t* out_width) {
  const nanopdf::FontMetrics* metrics = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_font_codepoint_width");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !out_width) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_get_font_codepoint_width");
  }

  metrics = writer->writer.get_font_metrics(font_name);
  if (!metrics) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "embedded font metrics were not found");
  }

  *out_width = static_cast<int32_t>(metrics->get_width(codepoint));
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_copy_font_postscript_name(
    nanopdf_writer* writer,
    const char* font_name,
    char** out_name) {
  const nanopdf::FontMetrics* metrics = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for copy_font_postscript_name");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !out_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_copy_font_postscript_name");
  }

  metrics = writer->writer.get_font_metrics(font_name);
  if (!metrics) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "embedded font metrics were not found");
  }

  return nanopdf__copy_owned_string(
      writer->context, metrics->font_name.c_str(), out_name);
}

nanopdf_status nanopdf_writer_copy_font_family_name(
    nanopdf_writer* writer,
    const char* font_name,
    char** out_name) {
  const nanopdf::FontMetrics* metrics = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for copy_font_family_name");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !out_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_copy_font_family_name");
  }

  metrics = writer->writer.get_font_metrics(font_name);
  if (!metrics) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "embedded font metrics were not found");
  }

  return nanopdf__copy_owned_string(
      writer->context, metrics->family_name.c_str(), out_name);
}

nanopdf_status nanopdf_writer_mark_font_text_used(
    nanopdf_writer* writer,
    const char* font_name,
    const char* text) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for mark_font_text_used");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || !text) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_mark_font_text_used");
  }
  if (!writer->writer.has_font_resource(font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "font resource was not found");
  }

  writer->writer.mark_chars_used(font_name, text);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_image_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_image_compression compression,
    char** out_image_name) {
  nanopdf::ImageCompression cpp_compression;
  nanopdf::ImageData image;
  std::string image_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || !out_image_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_from_memory");
  }
  if (!map_image_compression(compression, &cpp_compression)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image compression value");
  }

  image = nanopdf::ImageData::FromMemory(
      static_cast<const uint8_t*>(data), size);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to decode image data");
  }

  image_name = writer->writer.add_image(image, cpp_compression);
  return nanopdf__copy_owned_string(
      writer->context, image_name.c_str(), out_image_name);
}

nanopdf_status nanopdf_writer_add_image_with_alpha_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_image_compression compression,
    char** out_image_name) {
  nanopdf::ImageCompression cpp_compression;
  nanopdf::ImageData image;
  std::string image_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_with_alpha_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || !out_image_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_with_alpha_from_memory");
  }
  if (!map_image_compression(compression, &cpp_compression)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image compression value");
  }

  image = nanopdf::ImageData::FromMemory(
      static_cast<const uint8_t*>(data), size);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to decode image data");
  }

  image_name = writer->writer.add_image_with_alpha(image, cpp_compression);
  if (image_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to add image with alpha");
  }

  return nanopdf__copy_owned_string(
      writer->context, image_name.c_str(), out_image_name);
}

nanopdf_status nanopdf_writer_add_image_with_mask_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    const nanopdf_soft_mask_config* mask,
    nanopdf_image_compression compression,
    char** out_image_name) {
  nanopdf::ImageCompression cpp_compression;
  nanopdf::ImageData image;
  nanopdf::SoftMaskConfig cpp_mask;
  std::string image_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_with_mask_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || !mask || !mask->data.data || mask->data.size == 0 ||
      mask->width == 0 || mask->height == 0 || !out_image_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_with_mask_from_memory");
  }
  if (!map_image_compression(compression, &cpp_compression)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image compression value");
  }

  image = nanopdf::ImageData::FromMemory(
      static_cast<const uint8_t*>(data), size);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to decode image data");
  }

  if (mask->data.size != static_cast<size_t>(mask->width) * static_cast<size_t>(mask->height)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "soft mask byte count does not match width * height");
  }

  cpp_mask.mask_data.assign(
      static_cast<const uint8_t*>(mask->data.data),
      static_cast<const uint8_t*>(mask->data.data) + mask->data.size);
  cpp_mask.width = static_cast<int>(mask->width);
  cpp_mask.height = static_cast<int>(mask->height);
  cpp_mask.invert = mask->invert != 0;

  image_name = writer->writer.add_image_with_mask(image, cpp_mask, cpp_compression);
  if (image_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to add image with explicit soft mask");
  }

  return nanopdf__copy_owned_string(
      writer->context, image_name.c_str(), out_image_name);
}

nanopdf_status nanopdf_writer_add_image_from_file(
    nanopdf_writer* writer,
    const char* path,
    nanopdf_image_compression compression,
    char** out_image_name) {
  nanopdf::ImageCompression cpp_compression;
  nanopdf::ImageData image;
  std::string image_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_from_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0] || !out_image_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_from_file");
  }
  if (!map_image_compression(compression, &cpp_compression)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image compression value");
  }

  image = nanopdf::ImageData::FromFile(path);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_IO_ERROR,
        "failed to load image file");
  }

  image_name = writer->writer.add_image(image, cpp_compression);
  return nanopdf__copy_owned_string(
      writer->context, image_name.c_str(), out_image_name);
}

nanopdf_status nanopdf_writer_add_ccitt_image(
    nanopdf_writer* writer,
    const void* mono_data,
    uint32_t width,
    uint32_t height,
    char** out_image_name) {
  std::string image_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_ccitt_image");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!mono_data || width == 0 || height == 0 || !out_image_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_ccitt_image");
  }

  image_name = writer->writer.add_ccitt_image(
      static_cast<const uint8_t*>(mono_data),
      static_cast<int>(width),
      static_cast<int>(height));
  if (image_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to add CCITT image");
  }

  return nanopdf__copy_owned_string(
      writer->context, image_name.c_str(), out_image_name);
}

nanopdf_status nanopdf_writer_has_image_resource(
    nanopdf_writer* writer,
    const char* image_name,
    int* out_has_image) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for has_image_resource");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!image_name || !image_name[0] || !out_has_image) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_has_image_resource");
  }

  *out_has_image = writer->writer.has_image_resource(image_name) ? 1 : 0;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_image_page_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    double page_width,
    double page_height,
    double margin) {
  nanopdf::ImageData image;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_page_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || page_width <= 0.0 || page_height <= 0.0 || margin < 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_page_from_memory");
  }

  image = nanopdf::ImageData::FromMemory(
      static_cast<const uint8_t*>(data), size);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to decode image data");
  }

  writer->writer.add_image_page(
      image,
      nanopdf::PageSize{page_width, page_height},
      margin,
      true);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_image_page_fit_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    double margin) {
  nanopdf::ImageData image;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_image_page_fit_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!data || size == 0 || margin < 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_image_page_fit_from_memory");
  }

  image = nanopdf::ImageData::FromMemory(
      static_cast<const uint8_t*>(data), size);
  if (!image.valid()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to decode image data");
  }

  writer->writer.add_image_page_fit(image, margin);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_text_watermark(
    nanopdf_writer* writer,
    const char* text,
    const char* font_name,
    double font_size,
    double r,
    double g,
    double b,
    double alpha,
    double rotation_degrees,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer) {
  nanopdf::TextWatermarkConfig config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_text_watermark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (font_name && font_name[0] &&
      !writer->writer.has_font_resource(font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "watermark font resource was not found");
  }
  status = build_text_watermark_config(
      writer->context,
      text,
      font_name,
      font_size,
      r,
      g,
      b,
      alpha,
      rotation_degrees,
      position,
      layer,
      &config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.set_watermark(config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_image_watermark(
    nanopdf_writer* writer,
    const char* image_name,
    double alpha,
    double scale,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    double offset_x,
    double offset_y) {
  nanopdf::ImageWatermarkConfig config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_image_watermark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_image_watermark_config(
      writer->context,
      image_name,
      alpha,
      scale,
      position,
      layer,
      offset_x,
      offset_y,
      &config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_image_resource(config.image_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "watermark image resource was not found");
  }

  writer->writer.set_watermark(config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_page_text_watermark(
    nanopdf_writer* writer,
    uint32_t page_index,
    const char* text,
    const char* font_name,
    double font_size,
    double r,
    double g,
    double b,
    double alpha,
    double rotation_degrees,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer) {
  nanopdf::TextWatermarkConfig config;
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for add_page_text_watermark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (font_name && font_name[0] &&
      !writer->writer.has_font_resource(font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "watermark font resource was not found");
  }
  status = build_text_watermark_config(
      writer->context,
      text,
      font_name,
      font_size,
      r,
      g,
      b,
      alpha,
      rotation_degrees,
      position,
      layer,
      &config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.add_page_watermark(static_cast<int>(page_index), config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_page_image_watermark(
    nanopdf_writer* writer,
    uint32_t page_index,
    const char* image_name,
    double alpha,
    double scale,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    double offset_x,
    double offset_y) {
  nanopdf::ImageWatermarkConfig config;
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for add_page_image_watermark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_image_watermark_config(
      writer->context,
      image_name,
      alpha,
      scale,
      position,
      layer,
      offset_x,
      offset_y,
      &config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_image_resource(config.image_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "watermark image resource was not found");
  }

  writer->writer.add_page_watermark(static_cast<int>(page_index), config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_watermark(
    nanopdf_writer* writer) {
  return with_writer(writer, "invalid writer for clear_watermark",
                     [](nanopdf::PdfWriter& pdf_writer) {
                       pdf_writer.clear_watermark();
                     });
}

nanopdf_status nanopdf_writer_set_header(
    nanopdf_writer* writer,
    const nanopdf_header_config* config) {
  nanopdf::HeaderConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_header");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_header_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!cpp_config.font_name.empty() &&
      !writer->writer.has_font_resource(cpp_config.font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "header font resource was not found");
  }

  writer->writer.set_header(cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_footer(
    nanopdf_writer* writer,
    const nanopdf_footer_config* config) {
  nanopdf::FooterConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_footer");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_footer_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!cpp_config.font_name.empty() &&
      !writer->writer.has_font_resource(cpp_config.font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "footer font resource was not found");
  }

  writer->writer.set_footer(cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_page_header(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_header_config* config) {
  nanopdf::HeaderConfig cpp_config;
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for set_page_header");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_header_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!cpp_config.font_name.empty() &&
      !writer->writer.has_font_resource(cpp_config.font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "header font resource was not found");
  }

  writer->writer.set_page_header(static_cast<int>(page_index), cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_page_footer(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_footer_config* config) {
  nanopdf::FooterConfig cpp_config;
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for set_page_footer");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_footer_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!cpp_config.font_name.empty() &&
      !writer->writer.has_font_resource(cpp_config.font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "footer font resource was not found");
  }

  writer->writer.set_page_footer(static_cast<int>(page_index), cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_header(
    nanopdf_writer* writer) {
  return with_writer(writer, "invalid writer for clear_header",
                     [](nanopdf::PdfWriter& pdf_writer) {
                       pdf_writer.clear_header();
                     });
}

nanopdf_status nanopdf_writer_clear_footer(
    nanopdf_writer* writer) {
  return with_writer(writer, "invalid writer for clear_footer",
                     [](nanopdf::PdfWriter& pdf_writer) {
                       pdf_writer.clear_footer();
                     });
}

nanopdf_status nanopdf_writer_skip_header_footer(
    nanopdf_writer* writer,
    uint32_t page_index) {
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for skip_header_footer");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.skip_header_footer(static_cast<int>(page_index));
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_bates_numbering(
    nanopdf_writer* writer,
    const nanopdf_bates_config* config) {
  nanopdf::BatesConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_bates_numbering");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_bates_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!cpp_config.font_name.empty() &&
      !writer->writer.has_font_resource(cpp_config.font_name)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "bates font resource was not found");
  }

  writer->writer.set_bates_numbering(cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_clear_bates_numbering(
    nanopdf_writer* writer) {
  return with_writer(writer, "invalid writer for clear_bates_numbering",
                     [](nanopdf::PdfWriter& pdf_writer) {
                       pdf_writer.clear_bates_numbering();
                     });
}

nanopdf_status nanopdf_writer_skip_bates_number(
    nanopdf_writer* writer,
    uint32_t page_index) {
  nanopdf_status status = validate_page_index(
      writer, page_index, "invalid writer for skip_bates_number");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.skip_bates_number(static_cast<int>(page_index));
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_layer(
    nanopdf_writer* writer,
    const char* name,
    int visible,
    char** out_layer_name) {
  std::string layer_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_layer");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || !out_layer_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_layer");
  }

  layer_name = writer->writer.add_layer(name, visible != 0);
  return nanopdf__copy_owned_string(
      writer->context, layer_name.c_str(), out_layer_name);
}

nanopdf_status nanopdf_writer_add_layer_config(
    nanopdf_writer* writer,
    const nanopdf_layer_config* config,
    char** out_layer_name) {
  nanopdf::LayerConfig cpp_config;
  std::string layer_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_layer_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_layer_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output layer name pointer is null");
  }
  status = build_layer_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  layer_name = writer->writer.add_layer(cpp_config);
  return nanopdf__copy_owned_string(
      writer->context, layer_name.c_str(), out_layer_name);
}

nanopdf_status nanopdf_writer_add_bookmark(
    nanopdf_writer* writer,
    const char* title,
    uint32_t page_index,
    double dest_y,
    int* out_bookmark_id) {
  int bookmark_id = -1;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_bookmark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!title || !title[0] || !out_bookmark_id) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_bookmark");
  }

  bookmark_id = writer->writer.add_bookmark(
      title, static_cast<int>(page_index), dest_y);
  if (bookmark_id < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to create bookmark");
  }

  *out_bookmark_id = bookmark_id;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_bookmark_config(
    nanopdf_writer* writer,
    const nanopdf_bookmark_config* config,
    int* out_bookmark_id) {
  nanopdf::BookmarkConfig cpp_config;
  int bookmark_id = -1;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_bookmark_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_bookmark_id) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output bookmark id pointer is null");
  }
  status = build_bookmark_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  bookmark_id = writer->writer.add_bookmark(cpp_config);
  if (bookmark_id < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to create bookmark");
  }

  *out_bookmark_id = bookmark_id;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_child_bookmark(
    nanopdf_writer* writer,
    int parent_bookmark_id,
    const char* title,
    uint32_t page_index,
    double dest_y,
    int* out_bookmark_id) {
  int bookmark_id = -1;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_child_bookmark");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (parent_bookmark_id < 0 || !title || !title[0] || !out_bookmark_id) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_child_bookmark");
  }

  bookmark_id = writer->writer.add_child_bookmark(
      parent_bookmark_id, title, static_cast<int>(page_index), dest_y);
  if (bookmark_id < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid parent bookmark id");
  }

  *out_bookmark_id = bookmark_id;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_child_bookmark_config(
    nanopdf_writer* writer,
    int parent_bookmark_id,
    const nanopdf_bookmark_config* config,
    int* out_bookmark_id) {
  nanopdf::BookmarkConfig cpp_config;
  int bookmark_id = -1;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_child_bookmark_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (parent_bookmark_id < 0 || !out_bookmark_id) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_child_bookmark_config");
  }
  status = build_bookmark_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  bookmark_id = writer->writer.add_child_bookmark(parent_bookmark_id, cpp_config);
  if (bookmark_id < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid parent bookmark id");
  }

  *out_bookmark_id = bookmark_id;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_attachment(
    nanopdf_writer* writer,
    const nanopdf_attachment_config* config) {
  nanopdf::AttachmentConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_attachment");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_attachment_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.add_attachment(cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_attachment_from_memory(
    nanopdf_writer* writer,
    const char* filename,
    const void* data,
    size_t size,
    const char* description) {
  std::vector<uint8_t> attachment_data;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_attachment_from_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!filename || !filename[0] || (size > 0 && !data)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_attachment_from_memory");
  }

  if (size > 0) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    attachment_data.assign(bytes, bytes + size);
  }

  writer->writer.add_attachment(
      filename, attachment_data, description ? description : "");
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_attachment_from_file(
    nanopdf_writer* writer,
    const char* path,
    const char* description) {
  std::ifstream file;
  std::vector<uint8_t> data;
  std::string filename;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_attachment_from_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0]) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_attachment_from_file");
  }

  file.open(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_IO_ERROR,
        "failed to open attachment file");
  }

  std::streamsize size = file.tellg();
  if (size < 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_IO_ERROR,
        "failed to determine attachment file size");
  }
  file.seekg(0, std::ios::beg);

  data.resize(static_cast<size_t>(size));
  if (size > 0 &&
      !file.read(reinterpret_cast<char*>(data.data()), size)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_IO_ERROR,
        "failed to read attachment file");
  }

  filename = filename_from_path(path);
  if (filename.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment path does not contain a filename");
  }

  writer->writer.add_attachment(
      filename, data, description ? description : "");
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_text_field(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* default_value) {
  nanopdf::TextFieldConfig config;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_text_field");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || width <= 0.0 || height <= 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_text_field");
  }

  config.name = name;
  config.page = static_cast<int>(page_index);
  config.x = x;
  config.y = y;
  config.width = width;
  config.height = height;
  config.default_value = default_value ? default_value : "";

  field_name = writer->writer.add_text_field(config);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create text field");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_checkbox(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double size,
    int checked,
    const char* export_value) {
  nanopdf::CheckboxConfig config;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_checkbox");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || size <= 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_checkbox");
  }

  config.name = name;
  config.page = static_cast<int>(page_index);
  config.x = x;
  config.y = y;
  config.size = size;
  config.checked = checked != 0;
  if (export_value && export_value[0]) {
    config.export_value = export_value;
  }

  field_name = writer->writer.add_checkbox(config);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create checkbox");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_dropdown(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* const* options,
    size_t option_count,
    int32_t selected_index,
    int editable) {
  nanopdf::DropdownConfig config;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_dropdown");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || width <= 0.0 || height <= 0.0 ||
      (option_count > 0 && !options)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_dropdown");
  }
  if (selected_index < -1 ||
      (option_count == 0 && selected_index != -1) ||
      (option_count > 0 && selected_index >= static_cast<int32_t>(option_count))) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid dropdown selected index");
  }

  config.name = name;
  config.page = static_cast<int>(page_index);
  config.x = x;
  config.y = y;
  config.width = width;
  config.height = height;
  config.selected = selected_index;
  config.editable = editable != 0;

  for (size_t i = 0; i < option_count; ++i) {
    if (!options[i]) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "dropdown option is null");
    }
    config.options.push_back(options[i]);
  }

  field_name = writer->writer.add_dropdown(config);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create dropdown");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_radio_group(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    const nanopdf_radio_option* options,
    size_t option_count,
    int32_t selected_index) {
  nanopdf::RadioGroupConfig config;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_radio_group");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || option_count == 0 || !options) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_radio_group");
  }
  if (selected_index < -1 ||
      selected_index >= static_cast<int32_t>(option_count)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid radio selected index");
  }

  config.name = name;
  config.page = static_cast<int>(page_index);
  config.selected = selected_index;

  for (size_t i = 0; i < option_count; ++i) {
    nanopdf::RadioOption option;
    if (!options[i].value || !options[i].value[0] || options[i].size <= 0.0) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "invalid radio option");
    }
    option.x = options[i].x;
    option.y = options[i].y;
    option.size = options[i].size;
    option.value = options[i].value;
    config.options.push_back(option);
  }

  field_name = writer->writer.add_radio_group(config);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create radio group");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_listbox(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* const* options,
    size_t option_count,
    int32_t selected_index) {
  std::vector<std::string> cpp_options;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_listbox");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || width <= 0.0 || height <= 0.0 ||
      (option_count > 0 && !options)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_listbox");
  }
  if (selected_index < -1 ||
      (option_count == 0 && selected_index != -1) ||
      (option_count > 0 && selected_index >= static_cast<int32_t>(option_count))) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid listbox selected index");
  }

  for (size_t i = 0; i < option_count; ++i) {
    if (!options[i]) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "listbox option is null");
    }
    cpp_options.push_back(options[i]);
  }

  field_name = writer->writer.add_listbox(
      name,
      static_cast<int>(page_index),
      x,
      y,
      width,
      height,
      cpp_options,
      selected_index);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create listbox");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_button(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* caption) {
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_button");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!name || !name[0] || width <= 0.0 || height <= 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_button");
  }

  field_name = writer->writer.add_button(
      name,
      static_cast<int>(page_index),
      x,
      y,
      width,
      height,
      caption ? caption : "");
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create button");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_signature_field(
    nanopdf_writer* writer,
    const nanopdf_signature_field_config* config) {
  nanopdf::SignatureFieldConfig cpp_config;
  std::string field_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_signature_field");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_signature_field_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf() &&
      (cpp_config.page < 0 ||
       cpp_config.page >= writer->writer.get_page_count())) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature field page index is out of range");
  }

  field_name = writer->writer.add_signature_field(cpp_config);
  if (field_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create signature field");
  }

  return clear_success(writer->context);
}

void nanopdf_default_signature_field_config(
    nanopdf_signature_field_config* config) {
  if (!config) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->name = "Signature1";
  config->width = 200.0;
  config->height = 50.0;
  config->visible = 1;
  config->filter = NANOPDF_SIGNATURE_FILTER_ADOBE_PPK_LITE;
  config->subfilter = NANOPDF_SIGNATURE_SUBFILTER_PKCS7_DETACHED;
  config->mdp_permissions = NANOPDF_MDP_PERMISSIONS_ANNOTATE_FORM_FILL_SIGN;
}

nanopdf_status nanopdf_writer_set_signature_permissions(
    nanopdf_writer* writer,
    nanopdf_mdp_permissions permissions) {
  nanopdf::MdpPermissions cpp_permissions;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_signature_permissions");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_mdp_permissions(permissions, &cpp_permissions) || permissions == 0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid signature permissions");
  }

  writer->writer.set_permissions(cpp_permissions);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_timestamp_config(
    nanopdf_writer* writer,
    const nanopdf_timestamp_config* config) {
  nanopdf::TimestampConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_timestamp_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!config) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "timestamp config is null");
  }

  cpp_config.server_url = config->server_url ? config->server_url : "";
  cpp_config.username = config->username ? config->username : "";
  cpp_config.password = config->password ? config->password : "";
  cpp_config.timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 30000;
  cpp_config.embed_in_signature = config->embed_in_signature != 0;
  writer->writer.set_timestamp_config(cpp_config);
  return clear_success(writer->context);
}

void nanopdf_default_timestamp_config(
    nanopdf_timestamp_config* config) {
  if (!config) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000;
  config->embed_in_signature = 1;
}

nanopdf_status nanopdf_writer_has_timestamp_config(
    nanopdf_writer* writer,
    int* out_has_timestamp) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for has_timestamp_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_has_timestamp) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "timestamp state output is null");
  }

  *out_has_timestamp = writer->writer.has_timestamp_config() ? 1 : 0;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_signature_placeholder_count(
    nanopdf_writer* writer,
    size_t* out_count) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_signature_placeholder_count");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_count) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature placeholder count output is null");
  }

  *out_count = writer->writer.get_signature_placeholders().size();
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_signature_placeholder(
    nanopdf_writer* writer,
    size_t index,
    nanopdf_signature_placeholder* out_placeholder) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_signature_placeholder");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  const auto& placeholders = writer->writer.get_signature_placeholders();
  if (!out_placeholder) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature placeholder output is null");
  }
  if (index >= placeholders.size()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "signature placeholder index is out of range");
  }

  fill_signature_placeholder(placeholders[index], out_placeholder);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_linear_gradient(
    nanopdf_writer* writer,
    double x1,
    double y1,
    double x2,
    double y2,
    const nanopdf_color_stop* stops,
    size_t stop_count,
    int extend_start,
    int extend_end,
    char** out_gradient_name) {
  nanopdf::GradientConfig config;
  std::vector<nanopdf::ColorStop> cpp_stops;
  std::string gradient_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_linear_gradient");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_gradient_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output gradient name pointer is null");
  }
  status = build_color_stops(writer->context, stops, stop_count, &cpp_stops);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  config.type = nanopdf::GradientType::Linear;
  config.x1 = x1;
  config.y1 = y1;
  config.x2 = x2;
  config.y2 = y2;
  config.stops = cpp_stops;
  config.extend_start = extend_start != 0;
  config.extend_end = extend_end != 0;

  gradient_name = writer->writer.create_gradient(config);
  if (gradient_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create linear gradient");
  }

  return nanopdf__copy_owned_string(
      writer->context, gradient_name.c_str(), out_gradient_name);
}

nanopdf_status nanopdf_writer_add_radial_gradient(
    nanopdf_writer* writer,
    double cx,
    double cy,
    double radius,
    const nanopdf_color_stop* stops,
    size_t stop_count,
    int extend_start,
    int extend_end,
    char** out_gradient_name) {
  nanopdf::GradientConfig config;
  std::vector<nanopdf::ColorStop> cpp_stops;
  std::string gradient_name;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_radial_gradient");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (radius <= 0.0 || !out_gradient_name) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_add_radial_gradient");
  }
  status = build_color_stops(writer->context, stops, stop_count, &cpp_stops);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  config.type = nanopdf::GradientType::Radial;
  config.cx = cx;
  config.cy = cy;
  config.r = radius;
  config.stops = cpp_stops;
  config.extend_start = extend_start != 0;
  config.extend_end = extend_end != 0;

  gradient_name = writer->writer.create_gradient(config);
  if (gradient_name.empty()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "failed to create radial gradient");
  }

  return nanopdf__copy_owned_string(
      writer->context, gradient_name.c_str(), out_gradient_name);
}

nanopdf_status nanopdf_writer_text_layout_create(
    nanopdf_writer* writer,
    nanopdf_writer_text_layout** out_layout) {
  nanopdf_writer_text_layout* layout = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for writer_text_layout_create");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_layout) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout output is null");
  }

  *out_layout = NULL;
  layout = new (std::nothrow) nanopdf_writer_text_layout();
  if (!layout) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate writer text layout");
  }

  layout->context = writer->context;
  layout->layout.reset(new (std::nothrow) nanopdf::TextLayout());
  if (!layout->layout) {
    delete layout;
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate writer text layout state");
  }

  *out_layout = layout;
  return clear_success(writer->context);
}

void nanopdf_writer_text_layout_destroy(
    nanopdf_writer_text_layout* layout) {
  delete layout;
}

nanopdf_status nanopdf_writer_text_layout_set_width(
    nanopdf_writer_text_layout* layout,
    double width) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for set_width");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width <= 0.0) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout width must be positive");
  }

  layout->layout->set_width(width);
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_set_max_height(
    nanopdf_writer_text_layout* layout,
    double height) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for set_max_height");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (height <= 0.0) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout max height must be positive");
  }

  layout->layout->set_max_height(height);
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_set_alignment(
    nanopdf_writer_text_layout* layout,
    nanopdf_writer_text_align alignment) {
  nanopdf::TextAlign cpp_alignment;
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for set_alignment");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_writer_text_align(alignment, &cpp_alignment)) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid writer text alignment");
  }

  layout->layout->set_alignment(cpp_alignment);
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_set_style(
    nanopdf_writer_text_layout* layout,
    const nanopdf_writer_text_style* style) {
  nanopdf::TextStyle cpp_style;
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for set_style");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = build_writer_text_style(layout->context, style, &cpp_style);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  layout->layout->set_style(cpp_style);
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_add_text(
    nanopdf_writer_text_layout* layout,
    const char* text) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for add_text");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!text) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text is null");
  }

  layout->layout->add_text(text);
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_add_line_break(
    nanopdf_writer_text_layout* layout) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for add_line_break");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  layout->layout->add_line_break();
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_add_paragraph_break(
    nanopdf_writer_text_layout* layout) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for add_paragraph_break");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  layout->layout->add_paragraph_break();
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_get_height(
    const nanopdf_writer_text_layout* layout,
    double* out_height) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for get_height");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_height) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout height output is null");
  }

  *out_height = layout->layout->get_height();
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_get_line_count(
    const nanopdf_writer_text_layout* layout,
    int32_t* out_line_count) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for get_line_count");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_line_count) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout line count output is null");
  }

  *out_line_count = static_cast<int32_t>(layout->layout->get_line_count());
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_text_layout_has_overflow(
    const nanopdf_writer_text_layout* layout,
    int* out_has_overflow) {
  nanopdf_status status = validate_writer_text_layout(
      layout, "invalid writer text layout for has_overflow");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_has_overflow) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer text layout overflow output is null");
  }

  *out_has_overflow = layout->layout->has_overflow() ? 1 : 0;
  return clear_success(layout->context);
}

nanopdf_status nanopdf_writer_table_create(
    nanopdf_context* context,
    nanopdf_writer_table** out_table) {
  nanopdf_writer_table* table = NULL;

  if (!context || !out_table) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_table_create");
  }

  *out_table = NULL;
  table = new (std::nothrow) nanopdf_writer_table();
  if (!table) {
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate table builder");
  }

  table->context = context;
  table->table.reset(new (std::nothrow) nanopdf::TableBuilder());
  if (!table->table) {
    delete table;
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate table builder");
  }

  *out_table = table;
  return clear_success(context);
}

void nanopdf_default_writer_table_cell_style(
    nanopdf_writer_table_cell_style* style) {
  if (!style) {
    return;
  }

  memset(style, 0, sizeof(*style));
  style->align = NANOPDF_TABLE_CELL_ALIGN_LEFT;
  style->valign = NANOPDF_TABLE_CELL_VALIGN_MIDDLE;
  style->padding_left = 4.0;
  style->padding_right = 4.0;
  style->padding_top = 2.0;
  style->padding_bottom = 2.0;
  style->bg_r = 1.0;
  style->bg_g = 1.0;
  style->bg_b = 1.0;
  style->border_width = 0.5;
}

void nanopdf_default_writer_table_cell(
    nanopdf_writer_table_cell* cell) {
  if (!cell) {
    return;
  }

  memset(cell, 0, sizeof(*cell));
  cell->colspan = 1;
  cell->rowspan = 1;
  nanopdf_default_writer_table_cell_style(&cell->style);
}

void nanopdf_default_writer_table_row(
    nanopdf_writer_table_row* row) {
  if (!row) {
    return;
  }

  memset(row, 0, sizeof(*row));
  nanopdf_default_writer_table_cell_style(&row->row_style);
}

void nanopdf_writer_table_destroy(nanopdf_writer_table* table) {
  delete table;
}

nanopdf_status nanopdf_writer_table_set_position(
    nanopdf_writer_table* table,
    double x,
    double y) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_position");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_position(x, y);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_width(
    nanopdf_writer_table* table,
    double width) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_width");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width <= 0.0) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table width must be positive");
  }

  table->table->set_width(width);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_column_widths(
    nanopdf_writer_table* table,
    const double* widths,
    size_t width_count) {
  std::vector<double> cpp_widths;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_column_widths");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width_count > 0 && !widths) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "column width pointer is null");
  }

  cpp_widths.reserve(width_count);
  for (size_t i = 0; i < width_count; ++i) {
    if (widths[i] <= 0.0) {
      return set_error(
          table->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "table column widths must be positive");
    }
    cpp_widths.push_back(widths[i]);
  }

  table->table->set_column_widths(cpp_widths);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_font(
    nanopdf_writer_table* table,
    const char* font_name,
    double font_size) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_font");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || font_size <= 0.0) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table font arguments");
  }

  table->table->set_font(font_name, font_size);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_text_color(
    nanopdf_writer_table* table,
    double r,
    double g,
    double b) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_text_color");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_text_color(r, g, b);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_header_style(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell_style* style) {
  nanopdf::CellStyle cpp_style;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_header_style");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_cell_style(
      table->context, style, &cpp_style);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_header_style(cpp_style);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_border(
    nanopdf_writer_table* table,
    double width,
    double r,
    double g,
    double b) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_border");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width < 0.0) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table border width must be non-negative");
  }

  table->table->set_border(width, r, g, b);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_outer_border(
    nanopdf_writer_table* table,
    int enabled) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_outer_border");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_outer_border(enabled != 0);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_inner_borders(
    nanopdf_writer_table* table,
    int enabled) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_inner_borders");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_inner_borders(enabled != 0);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_set_alternating_rows(
    nanopdf_writer_table* table,
    int enabled,
    double r,
    double g,
    double b) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for set_alternating_rows");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->set_alternating_rows(enabled != 0, r, g, b);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_header_row(
    nanopdf_writer_table* table,
    const char* const* cells,
    size_t cell_count) {
  std::vector<std::string> cpp_cells;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_header_row");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (cell_count == 0) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table header row must contain at least one cell");
  }

  status = build_string_vector(
      table->context,
      cells,
      cell_count,
      "table header cell text is null",
      &cpp_cells);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_header_row(cpp_cells);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_header_row_cells(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell* cells,
    size_t cell_count) {
  std::vector<nanopdf::WriterTableCell> cpp_cells;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_header_row_cells");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_cells(
      table->context, cells, cell_count, &cpp_cells);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_header_row(cpp_cells);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_header_row_config(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_row* row) {
  nanopdf::TableRow cpp_row;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_header_row_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_row(table->context, row, &cpp_row);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_header_row(cpp_row);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_row(
    nanopdf_writer_table* table,
    const char* const* cells,
    size_t cell_count) {
  std::vector<std::string> cpp_cells;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_row");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (cell_count == 0) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table row must contain at least one cell");
  }

  status = build_string_vector(
      table->context,
      cells,
      cell_count,
      "table cell text is null",
      &cpp_cells);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_row(cpp_cells);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_row_cells(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell* cells,
    size_t cell_count) {
  std::vector<nanopdf::WriterTableCell> cpp_cells;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_row_cells");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_cells(
      table->context, cells, cell_count, &cpp_cells);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_row(cpp_cells);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_add_row_config(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_row* row) {
  nanopdf::TableRow cpp_row;
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for add_row_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_writer_table_row(table->context, row, &cpp_row);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  table->table->add_row(cpp_row);
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_table_calculate_height(
    const nanopdf_writer_table* table,
    double* out_height) {
  nanopdf_status status = validate_writer_table(
      table, "invalid table builder for calculate_height");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_height) {
    return set_error(
        table->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table height output is null");
  }

  *out_height = table->table->calculate_height();
  return clear_success(table->context);
}

nanopdf_status nanopdf_writer_begin_page(
    nanopdf_writer* writer,
    double width,
    double height,
    nanopdf_page_builder** out_page) {
  nanopdf_page_builder* page = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for begin_page");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_page || width <= 0.0 || height <= 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_begin_page");
  }

  *out_page = NULL;
  page = new (std::nothrow) nanopdf_page_builder();
  if (!page) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate page builder");
  }

  page->context = writer->context;
  page->owner = writer;
  page->size.width = width;
  page->size.height = height;
  page->builder.reset(new (std::nothrow) nanopdf::PageBuilder(&writer->writer));
  if (!page->builder) {
    delete page;
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate page builder state");
  }

  *out_page = page;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_begin_template(
    nanopdf_writer* writer,
    double width,
    double height,
    nanopdf_page_builder** out_template) {
  nanopdf_page_builder* page = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for begin_template");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_template || width <= 0.0 || height <= 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_writer_begin_template");
  }

  *out_template = NULL;
  page = new (std::nothrow) nanopdf_page_builder();
  if (!page) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate template builder");
  }

  page->context = writer->context;
  page->owner = writer;
  page->size.width = width;
  page->size.height = height;
  page->is_template = true;
  page->builder.reset(new (std::nothrow) nanopdf::PageBuilder(&writer->writer));
  if (!page->builder) {
    delete page;
    return set_error(
        writer->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate template builder state");
  }

  *out_template = page;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_page_builder_close(nanopdf_page_builder* page) {
  nanopdf_status status = require_page_target(
      page, "invalid page builder for close");
  nanopdf_context* context = page ? page->context : NULL;

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  page->owner->writer.add_page(page->size, *page->builder);
  delete page;
  return clear_success(context);
}

nanopdf_status nanopdf_page_builder_close_template(
    nanopdf_page_builder* page,
    char** out_template_name) {
  std::string template_name;
  nanopdf_status status = require_template_target(
      page, "invalid template builder for close_template");
  nanopdf_context* context = page ? page->context : NULL;

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_template_name) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "template name output is null");
  }

  *out_template_name = NULL;
  template_name = page->owner->writer.add_template(
      page->size.width, page->size.height, *page->builder);
  if (template_name.empty()) {
    delete page;
    return set_error(
        context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to close template builder");
  }

  delete page;
  return nanopdf__copy_owned_string(context, template_name.c_str(), out_template_name);
}

void nanopdf_page_builder_discard(nanopdf_page_builder* page) {
  delete page;
}

nanopdf_status nanopdf_page_builder_save_state(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for save_state",
                   [](nanopdf::PageBuilder& builder) { builder.save_state(); });
}

nanopdf_status nanopdf_page_builder_restore_state(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for restore_state",
                   [](nanopdf::PageBuilder& builder) { builder.restore_state(); });
}

nanopdf_status nanopdf_page_builder_translate(
    nanopdf_page_builder* page,
    double tx,
    double ty) {
  return with_page(page, "invalid page builder for translate",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.translate(tx, ty);
                   });
}

nanopdf_status nanopdf_page_builder_scale(
    nanopdf_page_builder* page,
    double sx,
    double sy) {
  return with_page(page, "invalid page builder for scale",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.scale(sx, sy);
                   });
}

nanopdf_status nanopdf_page_builder_rotate(
    nanopdf_page_builder* page,
    double angle_degrees) {
  return with_page(page, "invalid page builder for rotate",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.rotate(angle_degrees);
                   });
}

nanopdf_status nanopdf_page_builder_concat_matrix(
    nanopdf_page_builder* page,
    double a,
    double b,
    double c,
    double d,
    double e,
    double f) {
  return with_page(page, "invalid page builder for concat_matrix",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.concat_matrix(a, b, c, d, e, f);
                   });
}

nanopdf_status nanopdf_page_builder_set_stroke_color_rgb(
    nanopdf_page_builder* page,
    double r,
    double g,
    double b) {
  return with_page(page, "invalid page builder for set_stroke_color_rgb",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_stroke_color(r, g, b);
                   });
}

nanopdf_status nanopdf_page_builder_set_fill_color_rgb(
    nanopdf_page_builder* page,
    double r,
    double g,
    double b) {
  return with_page(page, "invalid page builder for set_fill_color_rgb",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_fill_color(r, g, b);
                   });
}

nanopdf_status nanopdf_page_builder_set_stroke_gray(
    nanopdf_page_builder* page,
    double gray) {
  return with_page(page, "invalid page builder for set_stroke_gray",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_stroke_gray(gray);
                   });
}

nanopdf_status nanopdf_page_builder_set_fill_gray(
    nanopdf_page_builder* page,
    double gray) {
  return with_page(page, "invalid page builder for set_fill_gray",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_fill_gray(gray);
                   });
}

nanopdf_status nanopdf_page_builder_set_fill_alpha(
    nanopdf_page_builder* page,
    double alpha) {
  return with_page(page, "invalid page builder for set_fill_alpha",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_fill_alpha(alpha);
                   });
}

nanopdf_status nanopdf_page_builder_set_stroke_alpha(
    nanopdf_page_builder* page,
    double alpha) {
  return with_page(page, "invalid page builder for set_stroke_alpha",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_stroke_alpha(alpha);
                   });
}

nanopdf_status nanopdf_page_builder_set_blend_mode(
    nanopdf_page_builder* page,
    nanopdf_blend_mode mode) {
  nanopdf::BlendMode cpp_mode;
  nanopdf_status status = validate_page(
      page, "invalid page builder for set_blend_mode");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!map_blend_mode(mode, &cpp_mode)) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid blend mode");
  }

  page->builder->set_blend_mode(cpp_mode);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_reset_transparency(
    nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for reset_transparency",
                   [](nanopdf::PageBuilder& builder) {
                     builder.reset_transparency();
                   });
}

nanopdf_status nanopdf_page_builder_set_line_width(
    nanopdf_page_builder* page,
    double width) {
  return with_page(page, "invalid page builder for set_line_width",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_line_width(width);
                   });
}

nanopdf_status nanopdf_page_builder_set_line_cap(
    nanopdf_page_builder* page,
    int cap) {
  return with_page(page, "invalid page builder for set_line_cap",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_line_cap(cap);
                   });
}

nanopdf_status nanopdf_page_builder_set_line_join(
    nanopdf_page_builder* page,
    int join) {
  return with_page(page, "invalid page builder for set_line_join",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.set_line_join(join);
                   });
}

nanopdf_status nanopdf_page_builder_set_dash_pattern(
    nanopdf_page_builder* page,
    const double* pattern,
    size_t pattern_count,
    double phase) {
  std::vector<double> dash_pattern;
  nanopdf_status status = validate_page(
      page, "invalid page builder for set_dash_pattern");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (pattern_count > 0 && !pattern) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "dash pattern pointer is null");
  }

  if (pattern_count > 0) {
    dash_pattern.assign(pattern, pattern + pattern_count);
  }
  page->builder->set_dash_pattern(dash_pattern, phase);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_move_to(
    nanopdf_page_builder* page,
    double x,
    double y) {
  return with_page(page, "invalid page builder for move_to",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.move_to(x, y);
                   });
}

nanopdf_status nanopdf_page_builder_line_to(
    nanopdf_page_builder* page,
    double x,
    double y) {
  return with_page(page, "invalid page builder for line_to",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.line_to(x, y);
                   });
}

nanopdf_status nanopdf_page_builder_curve_to(
    nanopdf_page_builder* page,
    double x1,
    double y1,
    double x2,
    double y2,
    double x3,
    double y3) {
  return with_page(page, "invalid page builder for curve_to",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.curve_to(x1, y1, x2, y2, x3, y3);
                   });
}

nanopdf_status nanopdf_page_builder_close_path(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for close_path",
                   [](nanopdf::PageBuilder& builder) { builder.close_path(); });
}

nanopdf_status nanopdf_page_builder_stroke(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for stroke",
                   [](nanopdf::PageBuilder& builder) { builder.stroke(); });
}

nanopdf_status nanopdf_page_builder_fill(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for fill",
                   [](nanopdf::PageBuilder& builder) { builder.fill(); });
}

nanopdf_status nanopdf_page_builder_fill_even_odd(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for fill_even_odd",
                   [](nanopdf::PageBuilder& builder) { builder.fill_even_odd(); });
}

nanopdf_status nanopdf_page_builder_fill_stroke(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for fill_stroke",
                   [](nanopdf::PageBuilder& builder) { builder.fill_stroke(); });
}

nanopdf_status nanopdf_page_builder_clip(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for clip",
                   [](nanopdf::PageBuilder& builder) { builder.clip(); });
}

nanopdf_status nanopdf_page_builder_clip_even_odd(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for clip_even_odd",
                   [](nanopdf::PageBuilder& builder) { builder.clip_even_odd(); });
}

nanopdf_status nanopdf_page_builder_rectangle(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height) {
  return with_page(page, "invalid page builder for rectangle",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.rectangle(x, y, width, height);
                   });
}

nanopdf_status nanopdf_page_builder_line(
    nanopdf_page_builder* page,
    double x1,
    double y1,
    double x2,
    double y2) {
  return with_page(page, "invalid page builder for line",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.line(x1, y1, x2, y2);
                   });
}

nanopdf_status nanopdf_page_builder_circle(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double radius) {
  return with_page(page, "invalid page builder for circle",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.circle(cx, cy, radius);
                   });
}

nanopdf_status nanopdf_page_builder_ellipse(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double rx,
    double ry) {
  return with_page(page, "invalid page builder for ellipse",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.ellipse(cx, cy, rx, ry);
                   });
}

nanopdf_status nanopdf_page_builder_arc(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double rx,
    double ry,
    double start_angle,
    double end_angle) {
  return with_page(page, "invalid page builder for arc",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.arc(cx, cy, rx, ry, start_angle, end_angle);
                   });
}

nanopdf_status nanopdf_page_builder_rounded_rect(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    double radius) {
  return with_page(page, "invalid page builder for rounded_rect",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.rounded_rect(x, y, width, height, radius);
                   });
}

nanopdf_status nanopdf_page_builder_append_raw_content(
    nanopdf_page_builder* page,
    const char* raw_content) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for append_raw_content");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!raw_content) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "raw content pointer is null");
  }

  page->builder->append_raw(raw_content);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_resource_ref(
    nanopdf_page_builder* page,
    const char* category,
    const char* name,
    nanopdf_object_ref ref) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for add_resource_ref");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!category || !name || !ref.valid || ref.object_number == 0 ||
      ref.generation != 0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid resource reference");
  }

  page->builder->add_resource_ref(
      category, name, static_cast<int>(ref.object_number));
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_begin_text(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for begin_text",
                   [](nanopdf::PageBuilder& builder) { builder.begin_text(); });
}

nanopdf_status nanopdf_page_builder_end_text(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for end_text",
                   [](nanopdf::PageBuilder& builder) { builder.end_text(); });
}

nanopdf_status nanopdf_page_builder_set_font(
    nanopdf_page_builder* page,
    const char* font_name,
    double font_size) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for set_font");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!font_name || !font_name[0] || font_size <= 0.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid font arguments");
  }

  page->builder->set_font(font_name, font_size);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_text_position(
    nanopdf_page_builder* page,
    double x,
    double y) {
  return with_page(page, "invalid page builder for text_position",
                   [&](nanopdf::PageBuilder& builder) {
                     builder.text_position(x, y);
                   });
}

nanopdf_status nanopdf_page_builder_show_text(
    nanopdf_page_builder* page,
    const char* text) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for show_text");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!text) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text pointer is null");
  }

  page->builder->show_text(text);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_show_text_at(
    nanopdf_page_builder* page,
    double x,
    double y,
    const char* text) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for show_text_at");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!text) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text pointer is null");
  }

  page->builder->show_text_at(x, y, text);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_draw_text_layout(
    nanopdf_page_builder* page,
    const nanopdf_writer_text_layout* layout,
    double x,
    double y) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for draw_text_layout");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = validate_writer_text_layout(
      layout, "invalid writer text layout for draw_text_layout");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  page->owner->writer.draw_text_layout(*page->builder, *layout->layout, x, y);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_draw_table(
    nanopdf_page_builder* page,
    const nanopdf_writer_table* table) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for draw_table");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = validate_writer_table(
      table, "invalid table builder for draw_table");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  const std::string& font_name = table->table->config().font_name;
  if (!font_name.empty() &&
      !page->owner->writer.has_font_resource(font_name)) {
    return set_error(
        page->context,
        NANOPDF_STATUS_NOT_FOUND,
        "table font resource was not found");
  }

  page->builder->draw_table(*table->table);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_draw_image(
    nanopdf_page_builder* page,
    const char* image_name,
    double x,
    double y,
    double width,
    double height) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for draw_image");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!image_name || !image_name[0] || width <= 0.0 || height <= 0.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid image draw arguments");
  }

  page->builder->draw_image(image_name, x, y, width, height);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_draw_template(
    nanopdf_page_builder* page,
    const char* template_name,
    double x,
    double y,
    double scale_x,
    double scale_y) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for draw_template");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!template_name || !template_name[0] ||
      scale_x <= 0.0 || scale_y <= 0.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid template draw arguments");
  }

  page->owner->writer.use_template(
      *page->builder, template_name, x, y, scale_x, scale_y);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_link_uri(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    const char* uri) {
  nanopdf_status status = require_page_target(
      page, "invalid page builder for add_link_uri");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!uri || !uri[0] || width <= 0.0 || height <= 0.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid URI link arguments");
  }

  page->builder->add_link(x, y, width, height, uri);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_link_config(
    nanopdf_page_builder* page,
    const nanopdf_link_config* config) {
  nanopdf::LinkConfig cpp_config;
  nanopdf_status status = require_page_target(
      page, "invalid page builder for add_link_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_link_config(page->context, config, true, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  page->builder->add_link(cpp_config);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_link_goto(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    uint32_t dest_page_index,
    double dest_y) {
  nanopdf_status status = require_page_target(
      page, "invalid page builder for add_link_goto");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width <= 0.0 || height <= 0.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid GoTo link arguments");
  }

  page->builder->add_link(
      x, y, width, height, static_cast<int>(dest_page_index), dest_y);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_highlight(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    double r,
    double g,
    double b,
    double alpha) {
  nanopdf::HighlightConfig config;
  nanopdf_status status = require_page_target(
      page, "invalid page builder for add_highlight");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width <= 0.0 || height <= 0.0 ||
      alpha < 0.0 || alpha > 1.0) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid highlight arguments");
  }

  config.quads.push_back(nanopdf::quad_from_rect(x, y, width, height));
  config.color_preset = nanopdf::HighlightColor::Custom;
  config.r = r;
  config.g = g;
  config.b = b;
  config.alpha = alpha;
  page->builder->add_highlight(config);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_add_text_markup(
    nanopdf_page_builder* page,
    const nanopdf_text_markup_config* config) {
  nanopdf::TextMarkupConfig cpp_config;
  nanopdf_status status = require_page_target(
      page, "invalid page builder for add_text_markup");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_text_markup_config(
      page->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  page->builder->add_text_markup(cpp_config);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_begin_layer(
    nanopdf_page_builder* page,
    const char* layer_name) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for begin_layer");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!layer_name || !layer_name[0]) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "layer name is null or empty");
  }

  page->builder->begin_layer(layer_name);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_end_layer(nanopdf_page_builder* page) {
  return with_page(page, "invalid page builder for end_layer",
                   [](nanopdf::PageBuilder& builder) { builder.end_layer(); });
}

nanopdf_status nanopdf_page_builder_set_fill_gradient(
    nanopdf_page_builder* page,
    const char* gradient_name) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for set_fill_gradient");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!gradient_name || !gradient_name[0]) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "gradient name is null or empty");
  }

  page->builder->set_fill_gradient(gradient_name);
  return clear_success(page->context);
}

nanopdf_status nanopdf_page_builder_set_stroke_gradient(
    nanopdf_page_builder* page,
    const char* gradient_name) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for set_stroke_gradient");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!gradient_name || !gradient_name[0]) {
    return set_error(
        page->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "gradient name is null or empty");
  }

  page->builder->set_stroke_gradient(gradient_name);
  return clear_success(page->context);
}

nanopdf_status nanopdf_object_create_null(
    nanopdf_context* context,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [](nanopdf::Value& value) {
    value.SetType(nanopdf::Value::NULL_OBJ);
  });
}

nanopdf_status nanopdf_object_create_bool(
    nanopdf_context* context,
    int value,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [&](nanopdf::Value& object_value) {
    object_value.SetType(nanopdf::Value::BOOLEAN);
    object_value.boolean = value != 0;
  });
}

nanopdf_status nanopdf_object_create_number(
    nanopdf_context* context,
    double value,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [&](nanopdf::Value& object_value) {
    object_value.SetType(nanopdf::Value::NUMBER);
    object_value.number = value;
  });
}

nanopdf_status nanopdf_object_create_string(
    nanopdf_context* context,
    const char* value,
    nanopdf_object** out_object) {
  if (!context || !out_object || !value) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_object_create_string");
  }

  return create_object(context, out_object, [&](nanopdf::Value& object_value) {
    object_value.SetType(nanopdf::Value::STRING);
    object_value.str = value;
  });
}

nanopdf_status nanopdf_object_create_name(
    nanopdf_context* context,
    const char* value,
    nanopdf_object** out_object) {
  if (!context || !out_object || !value) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_object_create_name");
  }

  return create_object(context, out_object, [&](nanopdf::Value& object_value) {
    object_value.SetType(nanopdf::Value::NAME);
    object_value.name = value;
  });
}

nanopdf_status nanopdf_object_create_array(
    nanopdf_context* context,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [](nanopdf::Value& value) {
    value.SetType(nanopdf::Value::ARRAY);
  });
}

nanopdf_status nanopdf_object_create_dict(
    nanopdf_context* context,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [](nanopdf::Value& value) {
    value.SetType(nanopdf::Value::DICTIONARY);
  });
}

nanopdf_status nanopdf_object_create_stream(
    nanopdf_context* context,
    nanopdf_object** out_object) {
  return create_object(context, out_object, [](nanopdf::Value& value) {
    value.SetType(nanopdf::Value::STREAM);
  });
}

nanopdf_status nanopdf_object_create_ref(
    nanopdf_context* context,
    nanopdf_object_ref ref,
    nanopdf_object** out_object) {
  if (!context || !out_object || !ref.valid || ref.object_number == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_object_create_ref");
  }

  return create_object(context, out_object, [&](nanopdf::Value& value) {
    value.SetType(nanopdf::Value::REFERENCE);
    value.ref_object_number = ref.object_number;
    value.ref_generation_number = ref.generation;
  });
}

void nanopdf_object_destroy(nanopdf_object* object) {
  delete object;
}

nanopdf_status nanopdf_object_array_append(
    nanopdf_object* array_object,
    const nanopdf_object* value) {
  nanopdf_status status = validate_object(
      array_object, "invalid array object");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = validate_object(value, "invalid array item");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (array_object->value.type != nanopdf::Value::ARRAY) {
    return set_error(
        array_object->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "target object is not an array");
  }

  array_object->value.array.push_back(value->value);
  return clear_success(array_object->context);
}

nanopdf_status nanopdf_object_dict_set(
    nanopdf_object* dict_or_stream_object,
    const char* key,
    const nanopdf_object* value) {
  nanopdf_status status = validate_object(
      dict_or_stream_object, "invalid dictionary object");
  nanopdf::Dictionary* dict = NULL;

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = validate_object(value, "invalid dictionary value");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!key) {
    return set_error(
        dict_or_stream_object->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "dictionary key is null");
  }

  if (dict_or_stream_object->value.type == nanopdf::Value::DICTIONARY) {
    dict = &dict_or_stream_object->value.dict;
  } else if (dict_or_stream_object->value.type == nanopdf::Value::STREAM) {
    dict = &dict_or_stream_object->value.stream.dict;
  } else {
    return set_error(
        dict_or_stream_object->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "target object is not a dictionary or stream");
  }

  (*dict)[key] = value->value;
  return clear_success(dict_or_stream_object->context);
}

nanopdf_status nanopdf_object_stream_set_data(
    nanopdf_object* stream_object,
    const void* data,
    size_t size) {
  nanopdf_status status = validate_object(
      stream_object, "invalid stream object");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (stream_object->value.type != nanopdf::Value::STREAM) {
    return set_error(
        stream_object->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "target object is not a stream");
  }
  if (size > 0 && !data) {
    return set_error(
        stream_object->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "stream data pointer is null");
  }

  stream_object->value.stream.data.assign(
      static_cast<const uint8_t*>(data),
      static_cast<const uint8_t*>(data) + size);
  return clear_success(stream_object->context);
}

nanopdf_status nanopdf_writer_add_object(
    nanopdf_writer* writer,
    const nanopdf_object* object,
    nanopdf_object_ref* out_ref) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_object");
  std::string serialized;
  std::string error;
  int object_number = 0;

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  status = validate_object(object, "invalid object for add_object");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_ref) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output reference pointer is null");
  }

  out_ref->object_number = 0;
  out_ref->generation = 0;
  out_ref->valid = 0;

  if (object->value.type == nanopdf::Value::STREAM) {
    if (!serialize_dictionary(
            object->value.stream.dict,
            object->value.stream.data.size(),
            true,
            &serialized,
            &error)) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          error.c_str());
    }
    object_number = writer->writer.add_raw_stream_object(
        serialized, object->value.stream.data);
  } else {
    if (!serialize_value(object->value, &serialized, &error)) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          error.c_str());
    }
    object_number = writer->writer.add_raw_object(serialized);
  }

  out_ref->object_number = static_cast<uint32_t>(object_number);
  out_ref->generation = 0;
  out_ref->valid = 1;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_field_value(
    nanopdf_writer* writer,
    const char* field_name,
    const char* value) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_field_value");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!field_name || !field_name[0] || !value) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid field value arguments");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for field updates");
  }
  if (!writer->writer.set_field_value(field_name, value)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "field was not found");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_field_checked(
    nanopdf_writer* writer,
    const char* field_name,
    int checked) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_field_checked");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!field_name || !field_name[0]) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "field name is null or empty");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for field updates");
  }
  if (!writer->writer.set_field_checked(field_name, checked != 0)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "field was not found");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_set_field_choice(
    nanopdf_writer* writer,
    const char* field_name,
    const char* value) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_field_choice");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!field_name || !field_name[0] || !value) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid field choice arguments");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for field updates");
  }
  if (!writer->writer.set_field_choice(field_name, value)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_NOT_FOUND,
        "field was not found");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_existing_text_annotation(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* contents) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_existing_text_annotation");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!contents) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "annotation contents is null");
  }
  if (width < 0.0 || height < 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "annotation rectangle dimensions must be non-negative");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }
  if (!writer->writer.add_text_annotation_to_existing_page(
          static_cast<int>(page_index), x, y, width, height, contents)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue text annotation update");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_existing_text_markup(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_text_markup_config* config) {
  nanopdf::TextMarkupConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_existing_text_markup");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }
  status = build_text_markup_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.add_text_markup_to_existing_page(
          static_cast<int>(page_index), cpp_config)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue text markup update");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_existing_link_uri(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* uri) {
  nanopdf::LinkConfig config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_existing_link_uri");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!uri || !uri[0] || width < 0.0 || height < 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid existing link URI arguments");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }

  config.x = x;
  config.y = y;
  config.width = width;
  config.height = height;
  config.action = nanopdf::LinkAction::URI;
  config.uri = uri;

  if (!writer->writer.add_link_to_existing_page(
          static_cast<int>(page_index), config)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue link annotation update");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_existing_link_config(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_link_config* config) {
  nanopdf::LinkConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_existing_link_config");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }

  status = build_link_config(
      writer->context, config, false, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (!writer->writer.add_link_to_existing_page(
          static_cast<int>(page_index), cpp_config)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue link annotation update");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_add_existing_link_goto(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    uint32_t dest_page_index,
    double dest_y) {
  nanopdf::LinkConfig config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for add_existing_link_goto");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (width < 0.0 || height < 0.0) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "link rectangle dimensions must be non-negative");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }

  config.x = x;
  config.y = y;
  config.width = width;
  config.height = height;
  config.action = nanopdf::LinkAction::GoTo;
  config.dest_page = static_cast<int>(dest_page_index);
  config.dest_y = dest_y;

  if (!writer->writer.add_link_to_existing_page(
          static_cast<int>(page_index), config)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue link annotation update");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_delete_existing_annotation(
    nanopdf_writer* writer,
    uint32_t page_index,
    uint32_t annotation_index) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for delete_existing_annotation");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for annotation updates");
  }
  if (!writer->writer.delete_annotation_from_existing_page(
          static_cast<int>(page_index), static_cast<int>(annotation_index))) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to queue annotation deletion");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_file(
    nanopdf_writer* writer,
    const char* path) {
  nanopdf::WriteResult result;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0]) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output path is empty");
  }

  result = writer->writer.write_to_file(path);
  if (!result.success) {
    return set_write_error(writer->context, result, NANOPDF_STATUS_IO_ERROR);
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_memory(
    nanopdf_writer* writer,
    void** out_data,
    size_t* out_size) {
  nanopdf::WriteResult result;
  std::vector<uint8_t> pdf_data;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  result = writer->writer.write_to_memory(pdf_data);
  if (!result.success) {
    return set_write_error(writer->context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }
  status = copy_output_buffer(writer->context, pdf_data, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_incremental_file(
    nanopdf_writer* writer,
    const char* path) {
  nanopdf::WriteResult result;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_incremental_file");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!path || !path[0]) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output path is empty");
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for incremental write");
  }

  result = writer->writer.write_incremental_to_file(path);
  if (!result.success) {
    return set_write_error(writer->context, result, NANOPDF_STATUS_IO_ERROR);
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_incremental_memory(
    nanopdf_writer* writer,
    void** out_data,
    size_t* out_size) {
  nanopdf::WriteResult result;
  std::vector<uint8_t> pdf_data;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_incremental_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for incremental write");
  }

  result = writer->writer.write_incremental(pdf_data);
  if (!result.success) {
    return set_write_error(writer->context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }
  status = copy_output_buffer(writer->context, pdf_data, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_incremental_for_signing_memory(
    nanopdf_writer* writer,
    size_t reserved_signature_size,
    void** out_data,
    size_t* out_size) {
  nanopdf::WriteResult result;
  std::vector<uint8_t> pdf_data;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_incremental_for_signing_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "writer has no loaded PDF for incremental signing");
  }

  result = writer->writer.write_incremental_for_signing(
      pdf_data, reserved_signature_size);
  if (!result.success) {
    return set_write_error(
        writer->context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }
  status = copy_output_buffer(writer->context, pdf_data, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_write_for_signing_memory(
    nanopdf_writer* writer,
    size_t reserved_signature_size,
    void** out_data,
    size_t* out_size) {
  nanopdf::WriteResult result;
  std::vector<uint8_t> pdf_data;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_for_signing_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "prepared signing for loaded PDFs is not supported by this C API");
  }

  result = writer->writer.write_for_signing(pdf_data, reserved_signature_size);
  if (!result.success) {
    return set_write_error(
        writer->context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }
  status = copy_output_buffer(writer->context, pdf_data, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(writer->context);
}

void nanopdf_default_writer_encryption_config(
    nanopdf_writer_encryption_config* config) {
  if (!config) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->algorithm = NANOPDF_ENCRYPTION_AES_128;
  config->allow_print = 1;
  config->allow_copy = 1;
  config->allow_annotate = 1;
  config->allow_fill_forms = 1;
  config->allow_accessibility = 1;
  config->allow_print_high_quality = 1;
  config->encrypt_metadata = 1;
}

nanopdf_status nanopdf_writer_set_encryption(
    nanopdf_writer* writer,
    const nanopdf_writer_encryption_config* config) {
  nanopdf::EncryptionConfig cpp_config;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for set_encryption");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (writer->writer.has_existing_pdf()) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "encryption is only supported for new PDFs in this C API");
  }
  status = build_writer_encryption_config(writer->context, config, &cpp_config);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  writer->writer.set_encryption(cpp_config);
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_is_encrypted(
    nanopdf_writer* writer,
    int* out_is_encrypted) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for is_encrypted");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_is_encrypted) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "encryption state output is null");
  }

  *out_is_encrypted = writer->writer.is_encrypted() ? 1 : 0;
  return clear_success(writer->context);
}

nanopdf_status nanopdf_writer_get_encryption_algorithm(
    nanopdf_writer* writer,
    nanopdf_encryption_algorithm* out_algorithm) {
  nanopdf_status status = validate_writer(
      writer, "invalid writer for get_encryption_algorithm");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_algorithm) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "encryption algorithm output is null");
  }
  if (!map_encryption_algorithm_to_c(
          writer->writer.get_encryption_algorithm(), out_algorithm)) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to map encryption algorithm");
  }

  return clear_success(writer->context);
}

nanopdf_status nanopdf_split_pdf_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    const int32_t* page_indices,
    size_t page_index_count,
    void** out_data,
    size_t* out_size) {
  nanopdf::Pdf source_pdf;
  nanopdf::WriteResult result;
  std::vector<int> cpp_page_indices;
  std::vector<uint8_t> output;
  nanopdf_status status;

  status = parse_pdf_from_memory(
      context,
      data,
      size,
      &source_pdf,
      "source PDF data is null or empty",
      "failed to parse source PDF for split");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  status = build_page_indices(
      context, page_indices, page_index_count, false, &cpp_page_indices);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  result = nanopdf::PdfWriter::split_pages(source_pdf, cpp_page_indices, output);
  if (!result.success) {
    return set_write_error(context, result, NANOPDF_STATUS_INVALID_ARGUMENT);
  }

  status = copy_output_buffer(context, output, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(context);
}

nanopdf_status nanopdf_merge_pdfs_memory(
    nanopdf_context* context,
    const nanopdf_buffer_view* inputs,
    size_t input_count,
    void** out_data,
    size_t* out_size) {
  nanopdf::WriteResult result;
  std::vector<std::vector<uint8_t>> pdf_data;
  std::vector<uint8_t> output;
  nanopdf_status status;

  if (!context || !inputs || input_count == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to merge_pdfs_memory");
  }

  pdf_data.reserve(input_count);
  for (size_t i = 0; i < input_count; ++i) {
    if (!inputs[i].data || inputs[i].size == 0) {
      return set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "merge input PDF data is null or empty");
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(inputs[i].data);
    pdf_data.emplace_back(bytes, bytes + inputs[i].size);
  }

  result = nanopdf::PdfWriter::merge_pdfs(pdf_data, output);
  if (!result.success) {
    return set_write_error(context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }

  status = copy_output_buffer(context, output, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(context);
}

nanopdf_status nanopdf_apply_redactions_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    void** out_data,
    size_t* out_size) {
  nanopdf::Pdf source_pdf;
  nanopdf::WriteResult result;
  std::vector<uint8_t> output;
  nanopdf_status status;

  status = parse_pdf_from_memory(
      context,
      data,
      size,
      &source_pdf,
      "source PDF data is null or empty",
      "failed to parse source PDF for redaction");
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  result = nanopdf::PdfWriter::apply_redactions(source_pdf, output);
  if (!result.success) {
    return set_write_error(context, result, NANOPDF_STATUS_INVALID_ARGUMENT);
  }

  status = copy_output_buffer(context, output, out_data, out_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(context);
}

nanopdf_status nanopdf_apply_precomputed_signature(
    nanopdf_context* context,
    const void* pdf_data,
    size_t pdf_size,
    const nanopdf_signature_placeholder* placeholder,
    const void* signature,
    size_t signature_size,
    void** out_signed_data,
    size_t* out_signed_size) {
  nanopdf::SignaturePlaceholder cpp_placeholder;
  nanopdf::WriteResult result;
  std::vector<uint8_t> signed_pdf;
  nanopdf_status status;

  if (!context || !pdf_data || pdf_size == 0 || !placeholder ||
      !signature || signature_size == 0) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to apply_precomputed_signature");
  }

  signed_pdf.assign(
      static_cast<const uint8_t*>(pdf_data),
      static_cast<const uint8_t*>(pdf_data) + pdf_size);
  cpp_placeholder.field_name = placeholder->field_name ? placeholder->field_name : "";
  cpp_placeholder.contents_offset = placeholder->contents_offset;
  cpp_placeholder.contents_length = placeholder->contents_length;
  cpp_placeholder.byte_range_offset = placeholder->byte_range_offset;
  cpp_placeholder.byte_range = {
      placeholder->byte_range_0,
      placeholder->byte_range_1,
      placeholder->byte_range_2,
      placeholder->byte_range_3};

  result = nanopdf::apply_signature(
      signed_pdf,
      cpp_placeholder,
      [signature, signature_size](const std::vector<uint8_t>&) {
        return std::vector<uint8_t>(
            static_cast<const uint8_t*>(signature),
            static_cast<const uint8_t*>(signature) + signature_size);
      });
  if (!result.success) {
    return set_write_error(context, result, NANOPDF_STATUS_INVALID_ARGUMENT);
  }
  status = copy_output_buffer(context, signed_pdf, out_signed_data, out_signed_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  return clear_success(context);
}

}  // extern "C"
