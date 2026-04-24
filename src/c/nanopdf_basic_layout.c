#include "nanopdf_basic_layout.h"

#include <ctype.h>
#include <string.h>

static nanopdf_status set_status(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message) {
  nanopdf__set_error(context, status, message);
  return status;
}

static nanopdf_status clear_status(nanopdf_context* context) {
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

static int append_char(
    nanopdf_context* context,
    nanopdf_basic_layout* layout,
    const nanopdf_basic_layout_char* ch) {
  nanopdf_basic_layout_char* resized;
  size_t new_capacity;

  if (layout->char_count < layout->char_capacity) {
    layout->chars[layout->char_count++] = *ch;
    return 1;
  }

  new_capacity = layout->char_capacity == 0 ? 32 : layout->char_capacity * 2;
  resized = (nanopdf_basic_layout_char*)nanopdf__allocator_realloc(
      &context->allocator, layout->chars, new_capacity * sizeof(*layout->chars));
  if (!resized) {
    return 0;
  }

  layout->chars = resized;
  layout->char_capacity = new_capacity;
  layout->chars[layout->char_count++] = *ch;
  return 1;
}

void nanopdf_basic_layout_init(nanopdf_basic_layout* layout) {
  if (!layout) {
    return;
  }
  memset(layout, 0, sizeof(*layout));
}

void nanopdf_basic_layout_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_layout* layout) {
  if (!allocator || !layout) {
    return;
  }

  nanopdf__allocator_free(allocator, layout->text);
  nanopdf__allocator_free(allocator, layout->chars);
  memset(layout, 0, sizeof(*layout));
}

nanopdf_status nanopdf_basic_layout_build(
    nanopdf_context* context,
    const char* text,
    double page_width,
    double page_height,
    nanopdf_basic_layout* out_layout) {
  size_t i;
  int32_t line_index = 0;
  int32_t word_index = -1;
  int in_word = 0;
  double x = 0.0;
  double y = page_height > 0.0 ? page_height : 0.0;
  const double glyph_width = 8.0;
  const double glyph_height = 12.0;
  const double line_step = 14.0;

  if (!context || !text || !out_layout) {
    return set_status(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid basic layout arguments");
  }

  nanopdf_basic_layout_init(out_layout);
  out_layout->text = nanopdf__strdup(&context->allocator, text);
  if (!out_layout->text) {
    return set_status(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate layout text");
  }
  out_layout->page_width = page_width;
  out_layout->page_height = page_height;

  for (i = 0; text[i] != '\0'; ++i) {
    const unsigned char ch = (const unsigned char)text[i];
    if (ch == '\n') {
      line_index++;
      out_layout->line_count = (size_t)line_index + 1;
      x = 0.0;
      y -= line_step;
      in_word = 0;
      continue;
    }
    if (isspace(ch)) {
      x += glyph_width;
      in_word = 0;
      continue;
    }

    if (!in_word) {
      word_index++;
      out_layout->word_count = (size_t)word_index + 1;
      in_word = 1;
    }

    {
      nanopdf_basic_layout_char item;
      item.unicode = (uint32_t)ch;
      item.x = x;
      item.y = y;
      item.width = glyph_width;
      item.height = glyph_height;
      item.line_index = line_index;
      item.word_index = word_index;
      if (!append_char(context, out_layout, &item)) {
        nanopdf_basic_layout_destroy(&context->allocator, out_layout);
        return set_status(
            context,
            NANOPDF_STATUS_OUT_OF_MEMORY,
            "failed to allocate layout characters");
      }
    }

    if (out_layout->line_count == 0) {
      out_layout->line_count = 1;
    }
    x += glyph_width;
  }

  return clear_status(context);
}

nanopdf_status nanopdf_basic_layout_copy_text_in_rect(
    nanopdf_context* context,
    const nanopdf_basic_layout* layout,
    double x1,
    double y1,
    double x2,
    double y2,
    char** out_text) {
  char* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t i;

  if (!context || !layout || !out_text) {
    return set_status(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid rect extraction arguments");
  }

  *out_text = NULL;
  for (i = 0; i < layout->char_count; ++i) {
    const nanopdf_basic_layout_char* ch = &layout->chars[i];
    char* resized;
    size_t new_capacity;
    if (ch->x < x1 || ch->x > x2 || ch->y < y1 || ch->y > y2) {
      continue;
    }

    if (length + 2 > capacity) {
      new_capacity = capacity == 0 ? 32 : capacity * 2;
      while (new_capacity < length + 2) {
        new_capacity *= 2;
      }
      resized = (char*)nanopdf__allocator_realloc(
          &context->allocator, buffer, new_capacity);
      if (!resized) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(
            context,
            NANOPDF_STATUS_OUT_OF_MEMORY,
            "failed to allocate rect text");
      }
      buffer = resized;
      capacity = new_capacity;
    }

    buffer[length++] = (char)ch->unicode;
    buffer[length] = '\0';
  }

  if (!buffer) {
    return nanopdf__copy_owned_string(context, "", out_text);
  }

  *out_text = buffer;
  return clear_status(context);
}
