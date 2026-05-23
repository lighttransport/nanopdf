#include "nanopdf_write.h"

#include "nanopdf_c_internal.h"
#include "nanopdf.hh"
#include "pdf-writer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
  std::unique_ptr<nanopdf::PageBuilder> builder;
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

nanopdf_status nanopdf_page_builder_close(nanopdf_page_builder* page) {
  nanopdf_status status = validate_page(
      page, "invalid page builder for close");
  nanopdf_context* context = page ? page->context : NULL;

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  page->owner->writer.add_page(page->size, *page->builder);
  delete page;
  return clear_success(context);
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
  void* output = NULL;
  nanopdf_status status = validate_writer(
      writer, "invalid writer for write_memory");

  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (!out_data || !out_size) {
    return set_error(
        writer->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid output buffer arguments");
  }

  *out_data = NULL;
  *out_size = 0;
  result = writer->writer.write_to_memory(pdf_data);
  if (!result.success) {
    return set_write_error(writer->context, result, NANOPDF_STATUS_INTERNAL_ERROR);
  }

  if (!pdf_data.empty()) {
    output = nanopdf__allocator_alloc(&writer->context->allocator, pdf_data.size());
    if (!output) {
      return set_error(
          writer->context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate PDF output buffer");
    }
    std::memcpy(output, pdf_data.data(), pdf_data.size());
  }

  *out_data = output;
  *out_size = pdf_data.size();
  return clear_success(writer->context);
}

}  // extern "C"
