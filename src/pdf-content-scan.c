// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pdf-content-scan.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static int npdf_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
         c == '\f' || c == '\0';
}

void npdf_content_scanner_init(npdf_content_scanner* scanner,
                               const void* data,
                               size_t size) {
  if (!scanner) return;
  scanner->data = (const char*)data;
  scanner->size = size;
  scanner->pos = 0;
}

static void npdf_skip_ws_and_comments(npdf_content_scanner* scanner) {
  const char* data = scanner->data;
  const size_t size = scanner->size;
  size_t pos = scanner->pos;

  for (;;) {
    while (pos < size && npdf_is_space((unsigned char)data[pos])) pos++;
    if (pos >= size || data[pos] != '%') break;
    while (pos < size && data[pos] != '\r' && data[pos] != '\n') pos++;
  }

  scanner->pos = pos;
}

static int npdf_regular_token_delimiter(unsigned char c) {
  return npdf_is_space(c) || c == '<' || c == '>' || c == '(' ||
         c == '[' || c == ']';
}

int npdf_content_next_token(npdf_content_scanner* scanner,
                            npdf_content_token* token) {
  if (!scanner || !token || !scanner->data) return 0;

  const char* data = scanner->data;
  const size_t size = scanner->size;

  for (;;) {
    npdf_skip_ws_and_comments(scanner);
    if (scanner->pos >= size) {
      token->ptr = NULL;
      token->len = 0;
      token->kind = NPDF_CONTENT_TOKEN_EOF;
      return 0;
    }
    if (data[scanner->pos] != '>') break;
    scanner->pos++;
  }

  size_t pos = scanner->pos;
  const size_t start = pos;
  token->ptr = data + start;
  token->len = 0;
  token->kind = NPDF_CONTENT_TOKEN_ATOM;

  if (data[pos] == '<') {
    if (pos + 1 < size && data[pos + 1] == '<') {
      pos += 2;
      int depth = 1;
      while (pos < size && depth > 0) {
        if (data[pos] == '<' && pos + 1 < size && data[pos + 1] == '<') {
          depth++;
          pos += 2;
        } else if (data[pos] == '>' && pos + 1 < size && data[pos + 1] == '>') {
          depth--;
          pos += 2;
        } else if (data[pos] == '(') {
          int pdepth = 1;
          pos++;
          while (pos < size && pdepth > 0) {
            if (data[pos] == '\\' && pos + 1 < size) {
              pos += 2;
              continue;
            }
            if (data[pos] == '(') pdepth++;
            else if (data[pos] == ')') pdepth--;
            pos++;
          }
        } else {
          pos++;
        }
      }
      token->kind = NPDF_CONTENT_TOKEN_DICTIONARY;
      token->len = pos - start;
      scanner->pos = pos;
      return 1;
    }

    pos++;
    while (pos < size && data[pos] != '>') pos++;
    if (pos < size) pos++;
    token->kind = NPDF_CONTENT_TOKEN_HEX_STRING;
    token->len = pos - start;
    scanner->pos = pos;
    return 1;
  }

  if (data[pos] == '(') {
    int depth = 1;
    pos++;
    while (pos < size && depth > 0) {
      if (data[pos] == '\\') {
        pos += (pos + 1 < size) ? 2 : 1;
        continue;
      }
      if (data[pos] == '(') depth++;
      else if (data[pos] == ')') depth--;
      pos++;
    }
    token->kind = NPDF_CONTENT_TOKEN_LITERAL_STRING;
    token->len = pos - start;
    scanner->pos = pos;
    return 1;
  }

  if (data[pos] == '[') {
    scanner->pos = pos + 1;
    token->kind = NPDF_CONTENT_TOKEN_ARRAY_START;
    token->len = 1;
    return 1;
  }

  if (data[pos] == ']') {
    scanner->pos = pos + 1;
    token->kind = NPDF_CONTENT_TOKEN_ARRAY_END;
    token->len = 1;
    return 1;
  }

  while (pos < size &&
         !npdf_regular_token_delimiter((unsigned char)data[pos])) {
    pos++;
  }
  if (pos == start) {
    scanner->pos = pos + 1;
    return npdf_content_next_token(scanner, token);
  }

  token->kind = NPDF_CONTENT_TOKEN_ATOM;
  token->len = pos - start;
  scanner->pos = pos;
  return 1;
}

static int npdf_parse_unsigned_pow10(const char* ptr,
                                     size_t len,
                                     size_t* pos,
                                     int* out) {
  int value = 0;
  size_t p = *pos;
  size_t digits = 0;
  while (p < len && ptr[p] >= '0' && ptr[p] <= '9') {
    if (value < 10000) value = value * 10 + (ptr[p] - '0');
    p++;
    digits++;
  }
  if (digits == 0) return 0;
  *pos = p;
  *out = value;
  return 1;
}

int npdf_parse_double_span(const char* ptr, size_t len, double* out) {
  if (!ptr || !out || len == 0) return 0;

  size_t pos = 0;
  while (pos < len && isspace((unsigned char)ptr[pos])) pos++;
  if (pos >= len) return 0;

  int sign = 1;
  if (ptr[pos] == '-') {
    sign = -1;
    pos++;
  } else if (ptr[pos] == '+') {
    pos++;
  }

  double value = 0.0;
  size_t digits = 0;
  while (pos < len && ptr[pos] >= '0' && ptr[pos] <= '9') {
    value = value * 10.0 + (double)(ptr[pos] - '0');
    pos++;
    digits++;
  }

  if (pos < len && ptr[pos] == '.') {
    pos++;
    double scale = 0.1;
    while (pos < len && ptr[pos] >= '0' && ptr[pos] <= '9') {
      value += (double)(ptr[pos] - '0') * scale;
      scale *= 0.1;
      pos++;
      digits++;
    }
  }

  if (digits == 0) return 0;

  if (pos < len && (ptr[pos] == 'e' || ptr[pos] == 'E')) {
    size_t exp_pos = pos + 1;
    int exp_sign = 1;
    if (exp_pos < len && ptr[exp_pos] == '-') {
      exp_sign = -1;
      exp_pos++;
    } else if (exp_pos < len && ptr[exp_pos] == '+') {
      exp_pos++;
    }
    int exp_value = 0;
    if (!npdf_parse_unsigned_pow10(ptr, len, &exp_pos, &exp_value)) {
      return 0;
    }
    value *= pow(10.0, (double)(exp_sign * exp_value));
    pos = exp_pos;
  }

  while (pos < len && isspace((unsigned char)ptr[pos])) pos++;
  if (pos != len) return 0;

  value *= (double)sign;
  if (!isfinite(value)) return 0;
  *out = value;
  return 1;
}

int npdf_parse_float_span(const char* ptr, size_t len, float* out) {
  if (!out) return 0;
  double value = 0.0;
  if (!npdf_parse_double_span(ptr, len, &value)) return 0;
  if (value > (double)FLT_MAX || value < -(double)FLT_MAX) return 0;
  *out = (float)value;
  return 1;
}

static int npdf_span_eq(const char* ptr,
                        size_t len,
                        const char* literal) {
  size_t literal_len = strlen(literal);
  return len == literal_len && memcmp(ptr, literal, literal_len) == 0;
}

static int npdf_parse_int_span(const char* ptr, size_t len, int* out) {
  if (!ptr || !out || len == 0) return 0;
  size_t pos = 0;
  int sign = 1;
  if (ptr[pos] == '-') {
    sign = -1;
    pos++;
  } else if (ptr[pos] == '+') {
    pos++;
  }
  if (pos >= len) return 0;
  int value = 0;
  for (; pos < len; pos++) {
    if (ptr[pos] < '0' || ptr[pos] > '9') return 0;
    if (value <= 214748364) {
      value = value * 10 + (ptr[pos] - '0');
    }
  }
  *out = value * sign;
  return 1;
}

static void npdf_read_inline_token(const char* data,
                                   size_t size,
                                   size_t* pos,
                                   const char** out_ptr,
                                   size_t* out_len,
                                   int strip_name_slash) {
  *out_ptr = NULL;
  *out_len = 0;
  while (*pos < size && npdf_is_space((unsigned char)data[*pos])) (*pos)++;
  if (*pos >= size) return;

  size_t start = *pos;
  if (data[*pos] == '/') {
    (*pos)++;
    start = strip_name_slash ? *pos : *pos - 1;
    while (*pos < size &&
           !npdf_is_space((unsigned char)data[*pos]) &&
           data[*pos] != '/') {
      (*pos)++;
    }
  } else if (data[*pos] == '[') {
    (*pos)++;
    while (*pos < size && data[*pos] != ']') (*pos)++;
    if (*pos < size) (*pos)++;
  } else {
    while (*pos < size &&
           !npdf_is_space((unsigned char)data[*pos]) &&
           data[*pos] != '/') {
      (*pos)++;
    }
  }

  *out_ptr = data + start;
  *out_len = *pos - start;
}

static size_t npdf_find_inline_image_end(const char* data,
                                         size_t size,
                                         size_t data_start) {
  size_t pos = data_start;
  while (pos < size) {
    if (pos > data_start &&
        (data[pos - 1] == ' ' || data[pos - 1] == '\n' ||
         data[pos - 1] == '\r') &&
        data[pos] == 'E' && pos + 1 < size && data[pos + 1] == 'I') {
      return pos;
    }
    pos++;
  }
  return size;
}

static size_t npdf_skip_inline_image_ei(const char* data,
                                        size_t size,
                                        size_t pos) {
  if (pos < size && data[pos] == 'E' && pos + 1 < size && data[pos + 1] == 'I') {
    return pos + 2;
  }
  while (pos < size) {
    if (data[pos] == 'E' && pos + 1 < size && data[pos + 1] == 'I') {
      return pos + 2;
    }
    pos++;
  }
  return pos;
}

int npdf_parse_inline_image_info(const char* data,
                                 size_t size,
                                 size_t start_pos,
                                 npdf_inline_image_info* out) {
  if (!data || !out || start_pos > size) return 0;

  memset(out, 0, sizeof(*out));
  out->bits_per_component = 8;
  out->color_space = NPDF_INLINE_IMAGE_GRAY;
  out->filter = NPDF_INLINE_IMAGE_FILTER_NONE;
  out->components = 1;
  out->end_pos = start_pos;

  size_t pos = start_pos;
  while (pos < size && npdf_is_space((unsigned char)data[pos])) pos++;

  while (pos < size) {
    while (pos < size && npdf_is_space((unsigned char)data[pos])) pos++;
    if (pos >= size) break;

    if (data[pos] == 'I' && pos + 1 < size && data[pos + 1] == 'D') {
      pos += 2;
      if (pos < size &&
          (data[pos] == ' ' || data[pos] == '\n' || data[pos] == '\r')) {
        pos++;
      }
      break;
    }

    const char* key = NULL;
    size_t key_len = 0;
    if (data[pos] == '/') {
      npdf_read_inline_token(data, size, &pos, &key, &key_len, 1);
    } else {
      while (pos < size && !npdf_is_space((unsigned char)data[pos])) pos++;
      continue;
    }

    const char* value = NULL;
    size_t value_len = 0;
    npdf_read_inline_token(data, size, &pos, &value, &value_len, 1);
    if (!key || !value) continue;

    if (npdf_span_eq(key, key_len, "W") ||
        npdf_span_eq(key, key_len, "Width")) {
      npdf_parse_int_span(value, value_len, &out->width);
    } else if (npdf_span_eq(key, key_len, "H") ||
               npdf_span_eq(key, key_len, "Height")) {
      npdf_parse_int_span(value, value_len, &out->height);
    } else if (npdf_span_eq(key, key_len, "BPC") ||
               npdf_span_eq(key, key_len, "BitsPerComponent")) {
      npdf_parse_int_span(value, value_len, &out->bits_per_component);
    } else if (npdf_span_eq(key, key_len, "CS") ||
               npdf_span_eq(key, key_len, "ColorSpace")) {
      if (npdf_span_eq(value, value_len, "RGB") ||
          npdf_span_eq(value, value_len, "DeviceRGB")) {
        out->color_space = NPDF_INLINE_IMAGE_RGB;
      } else if (npdf_span_eq(value, value_len, "CMYK") ||
                 npdf_span_eq(value, value_len, "DeviceCMYK")) {
        out->color_space = NPDF_INLINE_IMAGE_CMYK;
      } else {
        out->color_space = NPDF_INLINE_IMAGE_GRAY;
      }
    } else if (npdf_span_eq(key, key_len, "F") ||
               npdf_span_eq(key, key_len, "Filter")) {
      if (npdf_span_eq(value, value_len, "AHx") ||
          npdf_span_eq(value, value_len, "ASCIIHexDecode")) {
        out->filter = NPDF_INLINE_IMAGE_FILTER_ASCII_HEX;
      } else if (npdf_span_eq(value, value_len, "A85") ||
                 npdf_span_eq(value, value_len, "ASCII85Decode")) {
        out->filter = NPDF_INLINE_IMAGE_FILTER_ASCII85;
      } else if (npdf_span_eq(value, value_len, "Fl") ||
                 npdf_span_eq(value, value_len, "FlateDecode")) {
        out->filter = NPDF_INLINE_IMAGE_FILTER_FLATE;
      } else if (npdf_span_eq(value, value_len, "LZW") ||
                 npdf_span_eq(value, value_len, "LZWDecode")) {
        out->filter = NPDF_INLINE_IMAGE_FILTER_LZW;
      } else {
        out->filter = NPDF_INLINE_IMAGE_FILTER_UNKNOWN;
      }
    } else if (npdf_span_eq(key, key_len, "I") ||
               npdf_span_eq(key, key_len, "Interpolate")) {
      out->interpolate =
          npdf_span_eq(value, value_len, "true") ||
          npdf_span_eq(value, value_len, "True") ||
          npdf_span_eq(value, value_len, "1");
    }
  }

  out->components = 1;
  if (out->color_space == NPDF_INLINE_IMAGE_RGB) {
    out->components = 3;
  } else if (out->color_space == NPDF_INLINE_IMAGE_CMYK) {
    out->components = 4;
  }

  out->data_start = pos;
  out->data_end = npdf_find_inline_image_end(data, size, pos);
  out->end_pos = npdf_skip_inline_image_ei(data, size, out->data_end);

  if (out->width <= 0 || out->height <= 0 || out->bits_per_component <= 0) {
    return 0;
  }

  const size_t row_bits = (size_t)out->width *
                          (size_t)out->components *
                          (size_t)out->bits_per_component;
  out->expected_data_size = ((row_bits + 7u) / 8u) * (size_t)out->height;
  return 1;
}
