#include "nanopdf_object.h"
#include "nanopdf_crypto.h"
#include "nanopdf_image_decoder.h"

#include "../third_party/miniz.h"

#include <ctype.h>
#include <stdlib.h>
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

static void skip_ws(const char* data, size_t size, size_t* pos) {
  while (*pos < size) {
    unsigned char ch = (unsigned char)data[*pos];
    if (ch == '%') {
      while (*pos < size && data[*pos] != '\n' && data[*pos] != '\r') {
        (*pos)++;
      }
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' ||
        ch == '\0') {
      (*pos)++;
      continue;
    }
    break;
  }
}

static int is_delim(char ch) {
  return ch == '/' || ch == '<' || ch == '>' || ch == '[' || ch == ']' ||
         ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == '%';
}

static int match_literal(
    const char* data,
    size_t size,
    size_t pos,
    const char* literal) {
  size_t length = strlen(literal);
  if (pos + length > size) {
    return 0;
  }
  return memcmp(data + pos, literal, length) == 0;
}

static int parse_unsigned_value(
    const char* data,
    size_t size,
    size_t* pos,
    uint32_t* out_value) {
  uint64_t value = 0;
  size_t cursor = *pos;
  int saw_digit = 0;

  while (cursor < size && isdigit((unsigned char)data[cursor])) {
    saw_digit = 1;
    value = value * 10u + (uint64_t)(data[cursor] - '0');
    if (value > 0xffffffffu) {
      return 0;
    }
    cursor++;
  }

  if (!saw_digit) {
    return 0;
  }

  *pos = cursor;
  *out_value = (uint32_t)value;
  return 1;
}

static int parse_number_token(
    const char* data,
    size_t size,
    size_t* pos,
    double* out_value) {
  char buffer[64];
  size_t cursor = *pos;
  size_t length = 0;

  if (cursor < size && (data[cursor] == '+' || data[cursor] == '-')) {
    buffer[length++] = data[cursor++];
  }

  while (cursor < size && length + 1 < sizeof(buffer)) {
    char ch = data[cursor];
    if (!(isdigit((unsigned char)ch) || ch == '.')) {
      break;
    }
    buffer[length++] = ch;
    cursor++;
  }

  if (length == 0 || (length == 1 && (buffer[0] == '+' || buffer[0] == '-'))) {
    return 0;
  }

  buffer[length] = '\0';
  *out_value = strtod(buffer, NULL);
  *pos = cursor;
  return 1;
}

static int append_byte(
    nanopdf_context* context,
    uint8_t** buffer,
    size_t* length,
    size_t* capacity,
    uint8_t value) {
  uint8_t* resized = NULL;
  size_t new_capacity = 0;

  if (*length + 1 <= *capacity) {
    (*buffer)[(*length)++] = value;
    return 1;
  }

  new_capacity = *capacity == 0 ? 32 : *capacity * 2;
  while (new_capacity < *length + 1) {
    new_capacity *= 2;
  }

  resized = (uint8_t*)nanopdf__allocator_realloc(
      &context->allocator, *buffer, new_capacity);
  if (!resized) {
    return 0;
  }
  *buffer = resized;
  *capacity = new_capacity;
  (*buffer)[(*length)++] = value;
  return 1;
}

static const nanopdf_basic_object* dict_get_raw(
    const nanopdf_basic_dict* dict,
    const char* key);

static int security_is_xref_stream(const nanopdf_basic_object* object) {
  const nanopdf_basic_dict* dict = nanopdf_basic_object_as_dict(object);
  const nanopdf_basic_object* type_obj = NULL;
  if (!dict) {
    return 0;
  }
  type_obj = dict_get_raw(dict, "Type");
  return type_obj && type_obj->type == NANOPDF_BASIC_OBJECT_NAME &&
         strcmp(type_obj->as.text, "XRef") == 0;
}

static void compute_object_key(
    const nanopdf_basic_document* document,
    uint32_t object_number,
    uint16_t generation,
    uint8_t algorithm,
    uint8_t out_key[32],
    uint8_t* out_key_length) {
  uint8_t digest[16];
  uint8_t material[32 + 5 + 4];
  size_t material_length = 0;

  if (!document || !document->security.authenticated ||
      document->security.key_length == 0) {
    *out_key_length = 0;
    return;
  }

  if (algorithm == NANOPDF_BASIC_SECURITY_AES_256) {
    memcpy(out_key, document->security.key, document->security.key_length);
    *out_key_length = document->security.key_length;
    return;
  }

  memcpy(material, document->security.key, document->security.key_length);
  material_length = document->security.key_length;
  material[material_length++] = (uint8_t)(object_number & 0xff);
  material[material_length++] = (uint8_t)((object_number >> 8) & 0xff);
  material[material_length++] = (uint8_t)((object_number >> 16) & 0xff);
  material[material_length++] = (uint8_t)(generation & 0xff);
  material[material_length++] = (uint8_t)((generation >> 8) & 0xff);
  if (algorithm == NANOPDF_BASIC_SECURITY_AES_128) {
    material[material_length++] = (uint8_t)'s';
    material[material_length++] = (uint8_t)'A';
    material[material_length++] = (uint8_t)'l';
    material[material_length++] = (uint8_t)'T';
  }
  nanopdf_md5_hash(material, material_length, digest);
  *out_key_length = (uint8_t)(document->security.key_length + 5);
  if (*out_key_length > 16) {
    *out_key_length = 16;
  }
  memcpy(out_key, digest, *out_key_length);
}

static uint8_t get_security_algorithm_for_kind(
    const nanopdf_basic_document* document,
    int is_stream) {
  if (!document) {
    return NANOPDF_BASIC_SECURITY_NONE;
  }
  return is_stream ? document->security.stream_algorithm
                   : document->security.string_algorithm;
}

static nanopdf_status decrypt_bytes_in_place(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    uint32_t object_number,
    uint16_t generation,
    int is_stream,
    uint8_t* data,
    size_t* in_out_size) {
  size_t size = in_out_size ? *in_out_size : 0;
  nanopdf_rc4 rc4;
  nanopdf_aes128 aes128;
  nanopdf_aes256 aes256;
  uint8_t key[32];
  uint8_t key_length = 0;
  uint8_t algorithm = get_security_algorithm_for_kind(document, is_stream);

  if (!document || !document->security.authenticated ||
      algorithm == NANOPDF_BASIC_SECURITY_NONE ||
      !data || !in_out_size || size == 0) {
    return clear_status(context);
  }

  compute_object_key(
      document, object_number, generation, algorithm, key, &key_length);
  if (key_length == 0) {
    return clear_status(context);
  }

  if (algorithm == NANOPDF_BASIC_SECURITY_RC4_40 ||
      algorithm == NANOPDF_BASIC_SECURITY_RC4_128) {
    nanopdf_rc4_init(&rc4, key, key_length);
    nanopdf_rc4_crypt(&rc4, data, size);
    return clear_status(context);
  }

  if (algorithm == NANOPDF_BASIC_SECURITY_AES_128) {
    uint8_t iv[16];
    uint8_t* decrypted = NULL;
    size_t encrypted_size = 0;
    size_t plain_size = 0;

    if (size < 32) {
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-CBC payload");
    }
    encrypted_size = size - 16;
    if ((encrypted_size % 16) != 0) {
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-CBC payload");
    }

    decrypted = (uint8_t*)nanopdf__allocator_alloc(
        &context->allocator, encrypted_size == 0 ? 1 : encrypted_size);
    if (!decrypted) {
      return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate AES buffer");
    }

    memcpy(iv, data, 16);
    nanopdf_aes128_set_key(&aes128, key);
    nanopdf_aes128_decrypt_cbc(&aes128, data + 16, decrypted, encrypted_size, iv);
    if (!nanopdf_pkcs7_unpad(decrypted, encrypted_size, 16, &plain_size)) {
      nanopdf__allocator_free(&context->allocator, decrypted);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-CBC padding");
    }
    if (plain_size > 0) {
      memcpy(data, decrypted, plain_size);
    }
    nanopdf__allocator_free(&context->allocator, decrypted);
    *in_out_size = plain_size;
    return clear_status(context);
  }

  if (algorithm == NANOPDF_BASIC_SECURITY_AES_256) {
    uint8_t iv[16];
    uint8_t* decrypted = NULL;
    size_t encrypted_size = 0;
    size_t plain_size = 0;

    if (size < 32) {
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-256 payload");
    }
    encrypted_size = size - 16;
    if ((encrypted_size % 16) != 0) {
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-256 payload");
    }
    decrypted = (uint8_t*)nanopdf__allocator_alloc(
        &context->allocator, encrypted_size == 0 ? 1 : encrypted_size);
    if (!decrypted) {
      return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate AES-256 buffer");
    }
    memcpy(iv, data, 16);
    nanopdf_aes256_set_key(&aes256, key);
    nanopdf_aes256_decrypt_cbc(&aes256, data + 16, decrypted, encrypted_size, iv);
    if (!nanopdf_pkcs7_unpad(decrypted, encrypted_size, 16, &plain_size)) {
      nanopdf__allocator_free(&context->allocator, decrypted);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid AES-256 padding");
    }
    if (plain_size > 0) {
      memcpy(data, decrypted, plain_size);
    }
    nanopdf__allocator_free(&context->allocator, decrypted);
    *in_out_size = plain_size;
    return clear_status(context);
  }

  return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "unsupported security handler");
}

static nanopdf_status decrypt_indirect_strings(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    uint32_t object_number,
    uint16_t generation,
    nanopdf_basic_object* object) {
  size_t i = 0;

  if (!object || !document || !document->security.authenticated) {
    return clear_status(context);
  }

  switch (object->type) {
    case NANOPDF_BASIC_OBJECT_STRING:
      if (object->as.text) {
        size_t length = object->length;
        nanopdf_status status = decrypt_bytes_in_place(
            context,
            document,
            object_number,
            generation,
            0,
            (uint8_t*)object->as.text,
            &length);
        if (status == NANOPDF_STATUS_OK) {
          object->length = length;
          object->as.text[length] = '\0';
        }
        return status;
      }
      return clear_status(context);
    case NANOPDF_BASIC_OBJECT_ARRAY:
      for (i = 0; i < object->as.array.count; ++i) {
        nanopdf_status status = decrypt_indirect_strings(
            context,
            document,
            object_number,
            generation,
            &object->as.array.items[i]);
        if (status != NANOPDF_STATUS_OK) {
          return status;
        }
      }
      return clear_status(context);
    case NANOPDF_BASIC_OBJECT_DICT:
      for (i = 0; i < object->as.dict.count; ++i) {
        nanopdf_status status = decrypt_indirect_strings(
            context,
            document,
            object_number,
            generation,
            object->as.dict.entries[i].value);
        if (status != NANOPDF_STATUS_OK) {
          return status;
        }
      }
      return clear_status(context);
    case NANOPDF_BASIC_OBJECT_STREAM:
      for (i = 0; i < object->as.stream.dict.count; ++i) {
        nanopdf_status status = decrypt_indirect_strings(
            context,
            document,
            object_number,
            generation,
            object->as.stream.dict.entries[i].value);
        if (status != NANOPDF_STATUS_OK) {
          return status;
        }
      }
      return clear_status(context);
    default:
      return clear_status(context);
  }
}

static int parse_backslash_escape(
    const char* data,
    size_t size,
    size_t* cursor,
    char* out_char) {
  size_t pos = *cursor;
  char escaped = '\0';

  if (pos >= size) {
    return 0;
  }

  escaped = data[pos++];
  if (escaped >= '0' && escaped <= '7') {
    int value = escaped - '0';
    int count = 1;
    while (pos < size && count < 3 && data[pos] >= '0' && data[pos] <= '7') {
      value = value * 8 + (data[pos] - '0');
      pos++;
      count++;
    }
    *cursor = pos;
    *out_char = (char)(value & 0xff);
    return 1;
  }

  if (escaped == '\r') {
    if (pos < size && data[pos] == '\n') {
      pos++;
    }
    *cursor = pos;
    *out_char = '\0';
    return 2;
  }
  if (escaped == '\n') {
    *cursor = pos;
    *out_char = '\0';
    return 2;
  }

  if (escaped == 'n') *out_char = '\n';
  else if (escaped == 'r') *out_char = '\r';
  else if (escaped == 't') *out_char = '\t';
  else if (escaped == 'b') *out_char = '\b';
  else if (escaped == 'f') *out_char = '\f';
  else *out_char = escaped;

  *cursor = pos;
  return 1;
}

static int parse_literal_string(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    char** out_text,
    size_t* out_length) {
  size_t cursor = *pos;
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int depth = 0;

  if (cursor >= size || data[cursor] != '(') {
    return 0;
  }

  cursor++;
  depth = 1;
  while (cursor < size && depth > 0) {
    char ch = data[cursor++];
    if (ch == '\\' && cursor < size) {
      int escape_result = parse_backslash_escape(data, size, &cursor, &ch);
      if (escape_result == 0) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
      if (escape_result == 2) {
        continue;
      }
      if (depth == 1 &&
          !append_byte(context, &buffer, &length, &capacity, (uint8_t)ch)) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return -1;
      }
      continue;
    }
    if (ch == '(') {
      depth++;
      if (depth == 2 &&
          !append_byte(context, &buffer, &length, &capacity, (uint8_t)'(')) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return -1;
      }
      continue;
    }
    if (ch == ')') {
      depth--;
      if (depth == 0) {
        break;
      }
    }
    if (depth == 1 &&
        !append_byte(context, &buffer, &length, &capacity, (uint8_t)ch)) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return -1;
    }
  }

  if (depth != 0) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return 0;
  }

  if (!buffer) {
    buffer = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return -1;
    }
  } else {
    uint8_t* resized = (uint8_t*)nanopdf__allocator_realloc(
        &context->allocator, buffer, length + 1);
    if (resized) {
      buffer = resized;
    }
  }
  buffer[length] = '\0';
  *out_text = (char*)buffer;
  if (out_length) {
    *out_length = length;
  }
  *pos = cursor;
  return 1;
}

static int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static int parse_hex_string(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    char** out_text,
    size_t* out_length) {
  size_t cursor = *pos;
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int hi = -1;

  if (cursor + 1 >= size || data[cursor] != '<' || data[cursor + 1] == '<') {
    return 0;
  }
  cursor++;
  while (cursor < size && data[cursor] != '>') {
    int v = 0;
    if (isspace((unsigned char)data[cursor])) {
      cursor++;
      continue;
    }
    v = hex_value(data[cursor]);
    if (v < 0) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return 0;
    }
    if (hi < 0) {
      hi = v;
    } else {
      if (!append_byte(
              context,
              &buffer,
              &length,
              &capacity,
              (uint8_t)((hi << 4) | v))) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return -1;
      }
      hi = -1;
    }
    cursor++;
  }

  if (cursor >= size || data[cursor] != '>') {
    nanopdf__allocator_free(&context->allocator, buffer);
    return 0;
  }
  cursor++;
  if (hi >= 0 &&
      !append_byte(context, &buffer, &length, &capacity, (uint8_t)(hi << 4))) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return -1;
  }

  if (!buffer) {
    buffer = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return -1;
    }
  } else {
    uint8_t* resized = (uint8_t*)nanopdf__allocator_realloc(
        &context->allocator, buffer, length + 1);
    if (resized) {
      buffer = resized;
    }
  }
  buffer[length] = '\0';
  *out_text = (char*)buffer;
  if (out_length) {
    *out_length = length;
  }
  *pos = cursor;
  return 1;
}

static int parse_name(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    char** out_name,
    size_t* out_length) {
  size_t cursor = *pos;
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  char* name = NULL;

  if (cursor >= size || data[cursor] != '/') {
    return 0;
  }

  cursor++;
  while (cursor < size && !isspace((unsigned char)data[cursor]) &&
         !is_delim(data[cursor])) {
    if (data[cursor] == '#' && cursor + 2 < size) {
      int hi = hex_value(data[cursor + 1]);
      int lo = hex_value(data[cursor + 2]);
      if (hi >= 0 && lo >= 0) {
        if (!append_byte(
                context,
                &buffer,
                &length,
                &capacity,
                (uint8_t)((hi << 4) | lo))) {
          nanopdf__allocator_free(&context->allocator, buffer);
          return -1;
        }
        cursor += 3;
        continue;
      }
    }
    if (!append_byte(
            context,
            &buffer,
            &length,
            &capacity,
            (uint8_t)data[cursor])) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return -1;
    }
    cursor++;
  }
  if (!buffer) {
    buffer = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return -1;
    }
  } else {
    uint8_t* resized = (uint8_t*)nanopdf__allocator_realloc(
        &context->allocator, buffer, length + 1);
    if (resized) {
      buffer = resized;
    }
  }
  buffer[length] = '\0';
  name = (char*)buffer;
  *out_name = name;
  if (out_length) {
    *out_length = length;
  }
  *pos = cursor;
  return 1;
}

void nanopdf_basic_object_init(nanopdf_basic_object* object) {
  if (!object) {
    return;
  }
  memset(object, 0, sizeof(*object));
}

void nanopdf_basic_object_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_object* object) {
  size_t i = 0;

  if (!allocator || !object) {
    return;
  }

  switch (object->type) {
    case NANOPDF_BASIC_OBJECT_STRING:
    case NANOPDF_BASIC_OBJECT_NAME:
      nanopdf__allocator_free(allocator, object->as.text);
      break;
    case NANOPDF_BASIC_OBJECT_ARRAY:
      for (i = 0; i < object->as.array.count; ++i) {
        nanopdf_basic_object_destroy(allocator, &object->as.array.items[i]);
      }
      nanopdf__allocator_free(allocator, object->as.array.items);
      break;
    case NANOPDF_BASIC_OBJECT_DICT:
      for (i = 0; i < object->as.dict.count; ++i) {
        nanopdf__allocator_free(allocator, object->as.dict.entries[i].key);
        if (object->as.dict.entries[i].value) {
          nanopdf_basic_object_destroy(allocator, object->as.dict.entries[i].value);
          nanopdf__allocator_free(allocator, object->as.dict.entries[i].value);
        }
      }
      nanopdf__allocator_free(allocator, object->as.dict.entries);
      break;
    case NANOPDF_BASIC_OBJECT_STREAM:
      {
        nanopdf_basic_object dict_object;
        nanopdf_basic_object_init(&dict_object);
        dict_object.type = NANOPDF_BASIC_OBJECT_DICT;
        dict_object.as.dict = object->as.stream.dict;
        nanopdf_basic_object_destroy(allocator, &dict_object);
      }
      nanopdf__allocator_free(allocator, object->as.stream.data);
      break;
    default:
      break;
  }

  memset(object, 0, sizeof(*object));
}

static nanopdf_status append_array_item(
    nanopdf_context* context,
    nanopdf_basic_array* array,
    const nanopdf_basic_object* value) {
  nanopdf_basic_object* resized = NULL;
  size_t new_capacity = 0;

  if (array->count == array->capacity) {
    new_capacity = array->capacity == 0 ? 4 : array->capacity * 2;
    resized = (nanopdf_basic_object*)nanopdf__allocator_realloc(
        &context->allocator,
        array->items,
        new_capacity * sizeof(*array->items));
    if (!resized) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow object array");
    }
    memset(
        resized + array->capacity,
        0,
        (new_capacity - array->capacity) * sizeof(*array->items));
    array->items = resized;
    array->capacity = new_capacity;
  }

  array->items[array->count++] = *value;
  return NANOPDF_STATUS_OK;
}

static nanopdf_status append_dict_entry(
    nanopdf_context* context,
    nanopdf_basic_dict* dict,
    char* key,
    nanopdf_basic_object* value) {
  nanopdf_basic_dict_entry* resized = NULL;
  size_t new_capacity = 0;

  if (dict->count == dict->capacity) {
    new_capacity = dict->capacity == 0 ? 4 : dict->capacity * 2;
    resized = (nanopdf_basic_dict_entry*)nanopdf__allocator_realloc(
        &context->allocator,
        dict->entries,
        new_capacity * sizeof(*dict->entries));
    if (!resized) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow object dictionary");
    }
    memset(
        resized + dict->capacity,
        0,
        (new_capacity - dict->capacity) * sizeof(*dict->entries));
    dict->entries = resized;
    dict->capacity = new_capacity;
  }

  dict->entries[dict->count].key = key;
  dict->entries[dict->count].value = value;
  dict->count++;
  return NANOPDF_STATUS_OK;
}

static nanopdf_status parse_direct_object(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    int depth,
    nanopdf_basic_object* out_object);

static nanopdf_status parse_array(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    int depth,
    nanopdf_basic_object* out_object) {
  nanopdf_basic_object array_object;
  nanopdf_basic_object_init(&array_object);
  array_object.type = NANOPDF_BASIC_OBJECT_ARRAY;

  (*pos)++;
  while (*pos < size) {
    nanopdf_basic_object item;
    nanopdf_status status = NANOPDF_STATUS_OK;
    skip_ws(data, size, pos);
    if (*pos < size && data[*pos] == ']') {
      (*pos)++;
      *out_object = array_object;
      return clear_status(context);
    }
    nanopdf_basic_object_init(&item);
    status = parse_direct_object(context, data, size, pos, depth + 1, &item);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &item);
      nanopdf_basic_object_destroy(&context->allocator, &array_object);
      return status;
    }
    status = append_array_item(context, &array_object.as.array, &item);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &item);
      nanopdf_basic_object_destroy(&context->allocator, &array_object);
      return status;
    }
  }

  nanopdf_basic_object_destroy(&context->allocator, &array_object);
  return set_status(context, NANOPDF_STATUS_MALFORMED, "unterminated array");
}

static nanopdf_status parse_dict(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    int depth,
    nanopdf_basic_object* out_object) {
  nanopdf_basic_object dict_object;
  nanopdf_basic_object_init(&dict_object);
  dict_object.type = NANOPDF_BASIC_OBJECT_DICT;

  *pos += 2;
  while (*pos < size) {
    char* key = NULL;
    nanopdf_basic_object* value = NULL;
    nanopdf_status status = NANOPDF_STATUS_OK;
    int name_result = 0;

    skip_ws(data, size, pos);
    if (*pos + 1 < size && data[*pos] == '>' && data[*pos + 1] == '>') {
      *pos += 2;
      *out_object = dict_object;
      return clear_status(context);
    }

    name_result = parse_name(context, data, size, pos, &key, NULL);
    if (name_result <= 0) {
      nanopdf_basic_object_destroy(&context->allocator, &dict_object);
      return name_result < 0
                 ? set_status(
                       context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate dict key")
                 : set_status(context, NANOPDF_STATUS_MALFORMED, "invalid dictionary key");
    }

    value = (nanopdf_basic_object*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(*value));
    if (!value) {
      nanopdf__allocator_free(&context->allocator, key);
      nanopdf_basic_object_destroy(&context->allocator, &dict_object);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate dict value");
    }
    nanopdf_basic_object_init(value);

    status = parse_direct_object(context, data, size, pos, depth + 1, value);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, key);
      nanopdf_basic_object_destroy(&context->allocator, value);
      nanopdf__allocator_free(&context->allocator, value);
      nanopdf_basic_object_destroy(&context->allocator, &dict_object);
      return status;
    }

    status = append_dict_entry(context, &dict_object.as.dict, key, value);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, key);
      nanopdf_basic_object_destroy(&context->allocator, value);
      nanopdf__allocator_free(&context->allocator, value);
      nanopdf_basic_object_destroy(&context->allocator, &dict_object);
      return status;
    }
  }

  nanopdf_basic_object_destroy(&context->allocator, &dict_object);
  return set_status(context, NANOPDF_STATUS_MALFORMED, "unterminated dictionary");
}

static nanopdf_status parse_direct_object(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t* pos,
    int depth,
    nanopdf_basic_object* out_object) {
  size_t saved = *pos;
  double number = 0.0;

  if (depth > 64) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "object nesting too deep");
  }

  skip_ws(data, size, pos);
  if (*pos >= size) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "unexpected end of object");
  }

  if (*pos + 1 < size && data[*pos] == '<' && data[*pos + 1] == '<') {
    return parse_dict(context, data, size, pos, depth, out_object);
  }
  if (data[*pos] == '[') {
    return parse_array(context, data, size, pos, depth, out_object);
  }
  if (data[*pos] == '(') {
    char* text = NULL;
    size_t text_length = 0;
    int result = parse_literal_string(context, data, size, pos, &text, &text_length);
    if (result <= 0) {
      return result < 0
                 ? set_status(
                       context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate string")
                 : set_status(context, NANOPDF_STATUS_MALFORMED, "invalid string");
    }
    out_object->type = NANOPDF_BASIC_OBJECT_STRING;
    out_object->as.text = text;
    out_object->length = text_length;
    return clear_status(context);
  }
  if (data[*pos] == '<') {
    char* text = NULL;
    size_t text_length = 0;
    int result = parse_hex_string(context, data, size, pos, &text, &text_length);
    if (result <= 0) {
      return result < 0
                 ? set_status(
                       context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate hex string")
                 : set_status(context, NANOPDF_STATUS_MALFORMED, "invalid hex string");
    }
    out_object->type = NANOPDF_BASIC_OBJECT_STRING;
    out_object->as.text = text;
    out_object->length = text_length;
    return clear_status(context);
  }
  if (data[*pos] == '/') {
    char* name = NULL;
    size_t name_length = 0;
    int result = parse_name(context, data, size, pos, &name, &name_length);
    if (result <= 0) {
      return result < 0
                 ? set_status(
                       context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate name")
                 : set_status(context, NANOPDF_STATUS_MALFORMED, "invalid name");
    }
    out_object->type = NANOPDF_BASIC_OBJECT_NAME;
    out_object->as.text = name;
    out_object->length = name_length;
    return clear_status(context);
  }
  if (match_literal(data, size, *pos, "true")) {
    *pos += 4;
    out_object->type = NANOPDF_BASIC_OBJECT_BOOL;
    out_object->as.boolean = 1;
    return clear_status(context);
  }
  if (match_literal(data, size, *pos, "false")) {
    *pos += 5;
    out_object->type = NANOPDF_BASIC_OBJECT_BOOL;
    out_object->as.boolean = 0;
    return clear_status(context);
  }
  if (match_literal(data, size, *pos, "null")) {
    *pos += 4;
    out_object->type = NANOPDF_BASIC_OBJECT_NULL;
    return clear_status(context);
  }

  saved = *pos;
  if (parse_number_token(data, size, pos, &number)) {
    size_t ref_pos = *pos;
    uint32_t obj_number = 0;
    uint32_t generation = 0;

    skip_ws(data, size, &ref_pos);
    if (number >= 0.0 && number == (double)((uint32_t)number) &&
        parse_unsigned_value(data, size, &saved, &obj_number)) {
      size_t gen_pos = *pos;
      skip_ws(data, size, &gen_pos);
      if (parse_unsigned_value(data, size, &gen_pos, &generation)) {
        skip_ws(data, size, &gen_pos);
        if (gen_pos < size && data[gen_pos] == 'R') {
          out_object->type = NANOPDF_BASIC_OBJECT_REF;
          out_object->as.ref.object_number = (uint32_t)number;
          out_object->as.ref.generation = (uint16_t)generation;
          out_object->as.ref.valid = 1;
          *pos = gen_pos + 1;
          return clear_status(context);
        }
      }
    }

    out_object->type = NANOPDF_BASIC_OBJECT_NUMBER;
    out_object->as.number = number;
    return clear_status(context);
  }

  return set_status(context, NANOPDF_STATUS_MALFORMED, "unsupported object token");
}

static const nanopdf_basic_object* dict_get_raw(
    const nanopdf_basic_dict* dict,
    const char* key) {
  size_t i = 0;
  if (!dict || !key) {
    return NULL;
  }
  for (i = 0; i < dict->count; ++i) {
    if (dict->entries[i].key && strcmp(dict->entries[i].key, key) == 0) {
      return dict->entries[i].value;
    }
  }
  return NULL;
}

const nanopdf_basic_object* nanopdf_basic_dict_get(
    const nanopdf_basic_dict* dict,
    const char* key) {
  return dict_get_raw(dict, key);
}

const nanopdf_basic_dict* nanopdf_basic_object_as_dict(
    const nanopdf_basic_object* object) {
  if (!object) {
    return NULL;
  }
  if (object->type == NANOPDF_BASIC_OBJECT_DICT) {
    return &object->as.dict;
  }
  if (object->type == NANOPDF_BASIC_OBJECT_STREAM) {
    return &object->as.stream.dict;
  }
  return NULL;
}

typedef struct nanopdf_basic_decode_params {
  int predictor;
  int colors;
  int bits_per_component;
  int columns;
  int early_change;
  int rows;
  int ccitt_k;
  int end_of_line;
  int encoded_byte_align;
  int black_is_1;
  uint8_t* jbig2_globals;
  size_t jbig2_globals_size;
} nanopdf_basic_decode_params;

static nanopdf_status decode_stream_impl(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    uint32_t depth,
    uint8_t** out_data,
    size_t* out_size);

typedef struct nanopdf_basic_lzw_decoder {
  const uint8_t* data;
  size_t size;
  size_t pos;
  int bit_pos;
  uint32_t bit_buffer;
  int early_change;
} nanopdf_basic_lzw_decoder;

static void init_decode_params(nanopdf_basic_decode_params* params) {
  if (!params) {
    return;
  }
  params->predictor = 1;
  params->colors = 1;
  params->bits_per_component = 8;
  params->columns = 1;
  params->early_change = 1;
  params->rows = 0;
  params->ccitt_k = 0;
  params->end_of_line = 0;
  params->encoded_byte_align = 0;
  params->black_is_1 = 0;
  params->jbig2_globals = NULL;
  params->jbig2_globals_size = 0;
}

static void destroy_decode_params(
    nanopdf_context* context,
    nanopdf_basic_decode_params* params) {
  if (!context || !params) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, params->jbig2_globals);
  params->jbig2_globals = NULL;
  params->jbig2_globals_size = 0;
}

static nanopdf_status parse_decode_params(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    uint32_t depth,
    nanopdf_basic_decode_params* params) {
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* value = NULL;
  nanopdf_basic_object globals_object;
  nanopdf_status status = NANOPDF_STATUS_OK;

  init_decode_params(params);
  dict = nanopdf_basic_object_as_dict(object);
  if (!dict) {
    return clear_status(context);
  }

  value = dict_get_raw(dict, "Predictor");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->predictor = (int)value->as.number;
  }
  value = dict_get_raw(dict, "Colors");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->colors = (int)value->as.number;
  }
  value = dict_get_raw(dict, "BitsPerComponent");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->bits_per_component = (int)value->as.number;
  }
  value = dict_get_raw(dict, "Columns");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->columns = (int)value->as.number;
  }
  value = dict_get_raw(dict, "Rows");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->rows = (int)value->as.number;
  }
  value = dict_get_raw(dict, "K");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->ccitt_k = (int)value->as.number;
  }
  value = dict_get_raw(dict, "EndOfLine");
  if (value && value->type == NANOPDF_BASIC_OBJECT_BOOL) {
    params->end_of_line = value->as.boolean ? 1 : 0;
  }
  value = dict_get_raw(dict, "EncodedByteAlign");
  if (value && value->type == NANOPDF_BASIC_OBJECT_BOOL) {
    params->encoded_byte_align = value->as.boolean ? 1 : 0;
  }
  value = dict_get_raw(dict, "BlackIs1");
  if (value && value->type == NANOPDF_BASIC_OBJECT_BOOL) {
    params->black_is_1 = value->as.boolean ? 1 : 0;
  }
  value = dict_get_raw(dict, "EarlyChange");
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    params->early_change = value->as.number != 0.0 ? 1 : 0;
  }
  value = dict_get_raw(dict, "JBIG2Globals");
  if (document && value && value->type == NANOPDF_BASIC_OBJECT_REF && value->as.ref.valid) {
    nanopdf_basic_object_init(&globals_object);
    status = nanopdf_basic_load_object(context, document, value->as.ref, &globals_object);
    if (status == NANOPDF_STATUS_OK &&
        globals_object.type == NANOPDF_BASIC_OBJECT_STREAM) {
      status = decode_stream_impl(
          context,
          document,
          &globals_object,
          depth + 1u,
          &params->jbig2_globals,
          &params->jbig2_globals_size);
    }
    nanopdf_basic_object_destroy(&context->allocator, &globals_object);
    if (status != NANOPDF_STATUS_OK) {
      return status;
    }
  }

  return clear_status(context);
}

static nanopdf_status copy_bytes(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  size_t alloc_size = input_size == 0 ? 1 : input_size;

  *out_data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, alloc_size);
  if (!*out_data) {
    return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate stream copy");
  }
  if (input_size > 0) {
    memcpy(*out_data, input, input_size);
  }
  *out_size = input_size;
  return clear_status(context);
}

static nanopdf_status decode_asciihex(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int hi = -1;
  size_t i = 0;

  for (i = 0; i < input_size; ++i) {
    int value = 0;
    unsigned char ch = input[i];
    if (ch == '>') {
      break;
    }
    if (isspace(ch) || ch == '\0') {
      continue;
    }
    value = hex_value((char)ch);
    if (value < 0) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "ASCIIHexDecode: invalid digit");
    }
    if (hi < 0) {
      hi = value;
    } else if (!append_byte(
                   context,
                   &buffer,
                   &length,
                   &capacity,
                   (uint8_t)((hi << 4) | value))) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCIIHexDecode output");
    } else {
      hi = -1;
    }
  }

  if (hi >= 0 &&
      !append_byte(context, &buffer, &length, &capacity, (uint8_t)(hi << 4))) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCIIHexDecode output");
  }

  if (!buffer) {
    return copy_bytes(context, NULL, 0, out_data, out_size);
  }
  *out_data = buffer;
  *out_size = length;
  return clear_status(context);
}

static nanopdf_status decode_ascii85(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  uint32_t value = 0;
  int count = 0;
  size_t i = 0;

  for (i = 0; i < input_size; ++i) {
    unsigned char ch = input[i];
    if (isspace(ch) || ch == '\0') {
      continue;
    }
    if (ch == '~') {
      if (i + 1 < input_size && input[i + 1] == '>') {
        break;
      }
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "ASCII85Decode: invalid terminator");
    }
    if (ch == 'z') {
      if (count != 0) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "ASCII85Decode: invalid z position");
      }
      if (!append_byte(context, &buffer, &length, &capacity, 0) ||
          !append_byte(context, &buffer, &length, &capacity, 0) ||
          !append_byte(context, &buffer, &length, &capacity, 0) ||
          !append_byte(context, &buffer, &length, &capacity, 0)) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCII85Decode output");
      }
      continue;
    }
    if (ch < '!' || ch > 'u') {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "ASCII85Decode: invalid character");
    }

    value = value * 85u + (uint32_t)(ch - '!');
    count++;
    if (count == 5) {
      if (!append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 24) & 0xff)) ||
          !append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 16) & 0xff)) ||
          !append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 8) & 0xff)) ||
          !append_byte(context, &buffer, &length, &capacity, (uint8_t)(value & 0xff))) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCII85Decode output");
      }
      value = 0;
      count = 0;
    }
  }

  if (count == 1) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "ASCII85Decode: truncated group");
  }
  if (count > 1) {
    int j = 0;
    for (j = count; j < 5; ++j) {
      value = value * 85u + 84u;
    }
    if (!append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 24) & 0xff))) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCII85Decode output");
    }
    if (count > 2 &&
        !append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 16) & 0xff))) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCII85Decode output");
    }
    if (count > 3 &&
        !append_byte(context, &buffer, &length, &capacity, (uint8_t)((value >> 8) & 0xff))) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow ASCII85Decode output");
    }
  }

  if (!buffer) {
    return copy_bytes(context, NULL, 0, out_data, out_size);
  }
  *out_data = buffer;
  *out_size = length;
  return clear_status(context);
}

static nanopdf_status decode_runlength(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  uint8_t* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t pos = 0;

  while (pos < input_size) {
    uint8_t run = input[pos++];
    if (run == 128) {
      break;
    }
    if (run < 128) {
      size_t count = (size_t)run + 1u;
      size_t i = 0;
      if (pos + count > input_size) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "RunLengthDecode: truncated literal");
      }
      for (i = 0; i < count; ++i) {
        if (!append_byte(context, &buffer, &length, &capacity, input[pos++])) {
          nanopdf__allocator_free(&context->allocator, buffer);
          return set_status(
              context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow RunLengthDecode output");
        }
      }
    } else {
      size_t count = 257u - (size_t)run;
      uint8_t value = 0;
      size_t i = 0;
      if (pos >= input_size) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "RunLengthDecode: truncated repeat");
      }
      value = input[pos++];
      for (i = 0; i < count; ++i) {
        if (!append_byte(context, &buffer, &length, &capacity, value)) {
          nanopdf__allocator_free(&context->allocator, buffer);
          return set_status(
              context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow RunLengthDecode output");
        }
      }
    }
  }

  if (!buffer) {
    return copy_bytes(context, NULL, 0, out_data, out_size);
  }
  *out_data = buffer;
  *out_size = length;
  return clear_status(context);
}

static void lzw_decoder_init(
    nanopdf_basic_lzw_decoder* decoder,
    const uint8_t* input,
    size_t input_size,
    int early_change) {
  decoder->data = input;
  decoder->size = input_size;
  decoder->pos = 0;
  decoder->bit_pos = 0;
  decoder->bit_buffer = 0;
  decoder->early_change = early_change;
}

static int lzw_fill_buffer(nanopdf_basic_lzw_decoder* decoder) {
  if (decoder->pos >= decoder->size) {
    return 0;
  }
  decoder->bit_buffer = (decoder->bit_buffer << 8) | decoder->data[decoder->pos++];
  decoder->bit_pos += 8;
  return 1;
}

static int lzw_get_code(nanopdf_basic_lzw_decoder* decoder, int code_size) {
  while (decoder->bit_pos < code_size) {
    if (!lzw_fill_buffer(decoder)) {
      return -1;
    }
  }
  {
    int code = (int)(decoder->bit_buffer >> (decoder->bit_pos - code_size));
    decoder->bit_pos -= code_size;
    decoder->bit_buffer &= decoder->bit_pos == 32 ? 0xffffffffu : ((1u << decoder->bit_pos) - 1u);
    return code;
  }
}

static nanopdf_status decode_lzw(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_basic_decode_params* params,
    uint8_t** out_data,
    size_t* out_size) {
  enum { LZW_CLEAR = 256, LZW_EOD = 257, LZW_FIRST = 258, LZW_MAX = 4096 };
  uint16_t prefixes[LZW_MAX];
  uint8_t suffixes[LZW_MAX];
  uint8_t stack[LZW_MAX];
  nanopdf_basic_lzw_decoder decoder;
  uint8_t* output = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int next_code = LZW_FIRST;
  int code_size = 9;
  int old_code = -1;
  int first_char = 0;

  size_t i = 0;
  for (i = 0; i < 256; ++i) {
    prefixes[i] = 0xffffu;
    suffixes[i] = (uint8_t)i;
  }

  lzw_decoder_init(&decoder, input, input_size, params ? params->early_change : 1);
  while (1) {
    int code = lzw_get_code(&decoder, code_size);
    if (code < 0) {
      nanopdf__allocator_free(&context->allocator, output);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "LZWDecode: truncated data");
    }
    if (code == LZW_EOD) {
      break;
    }
    if (code == LZW_CLEAR) {
      next_code = LZW_FIRST;
      code_size = 9;
      old_code = -1;
      continue;
    }

    {
      int current = code;
      size_t stack_len = 0;

      if (current > next_code || current >= LZW_MAX) {
        nanopdf__allocator_free(&context->allocator, output);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "LZWDecode: invalid code");
      }
      if (current == next_code) {
        if (old_code < 0) {
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(context, NANOPDF_STATUS_MALFORMED, "LZWDecode: invalid early code");
        }
        current = old_code;
        stack[stack_len++] = (uint8_t)first_char;
      }

      while (current >= 256) {
        if ((size_t)current >= LZW_MAX || stack_len >= LZW_MAX) {
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(context, NANOPDF_STATUS_MALFORMED, "LZWDecode: dictionary overflow");
        }
        stack[stack_len++] = suffixes[current];
        current = prefixes[current];
      }

      first_char = suffixes[current];
      stack[stack_len++] = (uint8_t)first_char;

      while (stack_len > 0) {
        if (!append_byte(context, &output, &length, &capacity, stack[--stack_len])) {
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(
              context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow LZWDecode output");
        }
      }
    }

    if (old_code >= 0 && next_code < LZW_MAX) {
      prefixes[next_code] = (uint16_t)old_code;
      suffixes[next_code] = (uint8_t)first_char;
      next_code++;
      if (next_code + (decoder.early_change ? 0 : 1) == (1 << code_size) && code_size < 12) {
        code_size++;
      }
    }
    old_code = code;
  }

  if (!output) {
    return copy_bytes(context, NULL, 0, out_data, out_size);
  }
  *out_data = output;
  *out_size = length;
  return clear_status(context);
}

static nanopdf_status apply_tiff_predictor(
    nanopdf_context* context,
    const nanopdf_basic_decode_params* params,
    uint8_t** io_data,
    size_t* io_size) {
  size_t bytes_per_pixel = 0;
  size_t bytes_per_row = 0;
  size_t row_count = 0;
  size_t row = 0;

  if (!params || !io_data || !*io_data || !io_size) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid TIFF predictor input");
  }
  if (params->colors <= 0 || params->columns <= 0 || params->bits_per_component <= 0) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "Predictor: invalid TIFF parameters");
  }

  bytes_per_pixel =
      (size_t)((params->bits_per_component * params->colors + 7) / 8);
  bytes_per_row =
      (size_t)((params->bits_per_component * params->colors * params->columns + 7) / 8);
  if (bytes_per_row == 0 || *io_size < bytes_per_row) {
    return clear_status(context);
  }

  row_count = *io_size / bytes_per_row;
  if (params->bits_per_component == 8) {
    for (row = 0; row < row_count; ++row) {
      size_t row_start = row * bytes_per_row;
      size_t col = 0;
      for (col = bytes_per_pixel; col < bytes_per_row; ++col) {
        (*io_data)[row_start + col] =
            (uint8_t)((*io_data)[row_start + col] + (*io_data)[row_start + col - bytes_per_pixel]);
      }
    }
  } else if (params->bits_per_component == 16) {
    for (row = 0; row < row_count; ++row) {
      size_t row_start = row * bytes_per_row;
      size_t sample = 0;
      size_t samples_per_row = (size_t)params->columns * (size_t)params->colors;
      for (sample = (size_t)params->colors; sample < samples_per_row; ++sample) {
        size_t offset = row_start + sample * 2u;
        size_t prev_offset = row_start + (sample - (size_t)params->colors) * 2u;
        uint16_t prev = (uint16_t)(((*io_data)[prev_offset] << 8) | (*io_data)[prev_offset + 1]);
        uint16_t curr = (uint16_t)(((*io_data)[offset] << 8) | (*io_data)[offset + 1]);
        uint16_t result = (uint16_t)(curr + prev);
        (*io_data)[offset] = (uint8_t)((result >> 8) & 0xff);
        (*io_data)[offset + 1] = (uint8_t)(result & 0xff);
      }
    }
  }

  return clear_status(context);
}

static nanopdf_status apply_predictor(
    nanopdf_context* context,
    const nanopdf_basic_decode_params* params,
    uint8_t** io_data,
    size_t* io_size) {
  size_t bytes_per_pixel = 0;
  size_t bytes_per_row = 0;
  size_t row_stride = 0;
  size_t row_count = 0;
  uint8_t* output = NULL;
  size_t out_length = 0;
  size_t out_capacity = 0;
  size_t row = 0;

  if (!params || !io_data || !io_size) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid predictor input");
  }
  if (params->predictor == 1) {
    return clear_status(context);
  }
  if (params->predictor == 2) {
    return apply_tiff_predictor(context, params, io_data, io_size);
  }
  if (params->predictor < 10 || params->predictor > 15) {
    return clear_status(context);
  }
  if (params->colors <= 0 || params->columns <= 0 || params->bits_per_component <= 0) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "Predictor: invalid PNG parameters");
  }

  bytes_per_pixel =
      (size_t)((params->bits_per_component * params->colors + 7) / 8);
  bytes_per_row =
      (size_t)((params->bits_per_component * params->colors * params->columns + 7) / 8);
  row_stride = bytes_per_row + 1u;
  if (bytes_per_row == 0 || *io_size < row_stride) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "Predictor: insufficient row data");
  }

  row_count = *io_size / row_stride;
  for (row = 0; row < row_count; ++row) {
    const uint8_t* row_data = *io_data + row * row_stride + 1u;
    const uint8_t* prev_row = row == 0 ? NULL : output + (row - 1u) * bytes_per_row;
    uint8_t filter = (*io_data)[row * row_stride];
    size_t col = 0;
    for (col = 0; col < bytes_per_row; ++col) {
      uint8_t left = col >= bytes_per_pixel ? output[row * bytes_per_row + col - bytes_per_pixel] : 0u;
      uint8_t up = prev_row ? prev_row[col] : 0u;
      uint8_t up_left = (prev_row && col >= bytes_per_pixel) ? prev_row[col - bytes_per_pixel] : 0u;
      uint8_t value = 0;

      switch (filter) {
        case 0:
          value = row_data[col];
          break;
        case 1:
          value = (uint8_t)(row_data[col] + left);
          break;
        case 2:
          value = (uint8_t)(row_data[col] + up);
          break;
        case 3:
          value = (uint8_t)(row_data[col] + ((left + up) >> 1));
          break;
        case 4:
          {
            int p = (int)left + (int)up - (int)up_left;
            int pa = abs(p - (int)left);
            int pb = abs(p - (int)up);
            int pc = abs(p - (int)up_left);
            if (pa <= pb && pa <= pc) {
              value = (uint8_t)(row_data[col] + left);
            } else if (pb <= pc) {
              value = (uint8_t)(row_data[col] + up);
            } else {
              value = (uint8_t)(row_data[col] + up_left);
            }
          }
          break;
        default:
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(context, NANOPDF_STATUS_MALFORMED, "Predictor: unknown PNG row filter");
      }

      if (!append_byte(context, &output, &out_length, &out_capacity, value)) {
        nanopdf__allocator_free(&context->allocator, output);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow predictor output");
      }
    }
  }

  nanopdf__allocator_free(&context->allocator, *io_data);
  *io_data = output;
  *io_size = out_length;
  return clear_status(context);
}

static nanopdf_status inflate_flate(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  mz_stream stream;
  uint8_t* output = NULL;
  size_t capacity = input_size > 0 ? input_size * 4 : 256;
  int ret = MZ_OK;

  if (!out_data || !out_size) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid flate output arguments");
  }

  if (capacity < 256) {
    capacity = 256;
  }

  memset(&stream, 0, sizeof(stream));
  stream.next_in = (const unsigned char*)input;
  stream.avail_in = (mz_uint)input_size;
  if (mz_inflateInit(&stream) != MZ_OK) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "failed to initialize flate decoder");
  }

  output = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, capacity);
  if (!output) {
    mz_inflateEnd(&stream);
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate flate output");
  }

  for (;;) {
    uint8_t* resized = NULL;
    if (stream.total_out >= capacity) {
      size_t new_capacity = capacity * 2;
      resized = (uint8_t*)nanopdf__allocator_realloc(
          &context->allocator, output, new_capacity);
      if (!resized) {
        nanopdf__allocator_free(&context->allocator, output);
        mz_inflateEnd(&stream);
        return set_status(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow flate output");
      }
      output = resized;
      capacity = new_capacity;
    }

    stream.next_out = output + stream.total_out;
    stream.avail_out = (mz_uint)(capacity - stream.total_out);
    ret = mz_inflate(&stream, MZ_NO_FLUSH);
    if (ret == MZ_STREAM_END) {
      break;
    }
    if (ret != MZ_OK) {
      nanopdf__allocator_free(&context->allocator, output);
      mz_inflateEnd(&stream);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "flate decode failed");
    }
  }

  mz_inflateEnd(&stream);
  *out_size = (size_t)stream.total_out;
  if (*out_size == 0) {
    uint8_t* resized = (uint8_t*)nanopdf__allocator_realloc(
        &context->allocator, output, 1);
    if (resized) {
      output = resized;
    }
  } else {
    uint8_t* resized = (uint8_t*)nanopdf__allocator_realloc(
        &context->allocator, output, *out_size);
    if (resized) {
      output = resized;
    }
  }
  *out_data = output;
  return clear_status(context);
}

static nanopdf_status decode_filter_step(
    nanopdf_context* context,
    const char* filter_name,
    const nanopdf_basic_decode_params* params,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (strcmp(filter_name, "FlateDecode") == 0 || strcmp(filter_name, "Fl") == 0) {
    status = inflate_flate(context, input, input_size, out_data, out_size);
    if (status == NANOPDF_STATUS_OK) {
      status = apply_predictor(context, params, out_data, out_size);
    }
    return status;
  }
  if (strcmp(filter_name, "ASCIIHexDecode") == 0 || strcmp(filter_name, "AHx") == 0) {
    return decode_asciihex(context, input, input_size, out_data, out_size);
  }
  if (strcmp(filter_name, "ASCII85Decode") == 0 || strcmp(filter_name, "A85") == 0) {
    return decode_ascii85(context, input, input_size, out_data, out_size);
  }
  if (strcmp(filter_name, "RunLengthDecode") == 0 || strcmp(filter_name, "RL") == 0) {
    return decode_runlength(context, input, input_size, out_data, out_size);
  }
  if (strcmp(filter_name, "LZWDecode") == 0 || strcmp(filter_name, "LZW") == 0) {
    status = decode_lzw(context, input, input_size, params, out_data, out_size);
    if (status == NANOPDF_STATUS_OK) {
      status = apply_predictor(context, params, out_data, out_size);
    }
    return status;
  }
  if (strcmp(filter_name, "DCTDecode") == 0 || strcmp(filter_name, "DCT") == 0) {
    return nanopdf__decode_dct(context, input, input_size, out_data, out_size);
  }
  if (strcmp(filter_name, "CCITTFaxDecode") == 0 || strcmp(filter_name, "CCF") == 0) {
    nanopdf_ccitt_params ccitt_params;
    ccitt_params.columns = params ? params->columns : 0;
    ccitt_params.rows = params ? params->rows : 0;
    ccitt_params.k = params ? params->ccitt_k : 0;
    ccitt_params.end_of_line = params ? params->end_of_line : 0;
    ccitt_params.encoded_byte_align = params ? params->encoded_byte_align : 0;
    ccitt_params.black_is_1 = params ? params->black_is_1 : 0;
    return nanopdf__decode_ccitt(
        context, input, input_size, &ccitt_params, out_data, out_size);
  }
  if (strcmp(filter_name, "JPXDecode") == 0) {
    return nanopdf__decode_jpx(context, input, input_size, out_data, out_size);
  }
  if (strcmp(filter_name, "JBIG2Decode") == 0) {
    nanopdf_jbig2_params jbig2_params;
    jbig2_params.globals = params ? params->jbig2_globals : NULL;
    jbig2_params.globals_size = params ? params->jbig2_globals_size : 0;
    return nanopdf__decode_jbig2(
        context, input, input_size, &jbig2_params, out_data, out_size);
  }
  if (strcmp(filter_name, "Crypt") == 0) {
    return copy_bytes(context, input, input_size, out_data, out_size);
  }

  return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "unsupported stream filter");
}

static nanopdf_status decode_stream_impl(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    uint32_t depth,
    uint8_t** out_data,
    size_t* out_size) {
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* filter = NULL;
  const nanopdf_basic_object* decode_parms = NULL;
  uint8_t* current = NULL;
  size_t current_size = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;
  size_t i = 0;

  if (!context || !object || object->type != NANOPDF_BASIC_OBJECT_STREAM ||
      !out_data || !out_size) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid stream decode arguments");
  }
  if (depth > 8u) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "stream DecodeParms recursion limit exceeded");
  }

  *out_data = NULL;
  *out_size = 0;
  dict = &object->as.stream.dict;
  filter = dict_get_raw(dict, "Filter");
  decode_parms = dict_get_raw(dict, "DecodeParms");
  if (!decode_parms) {
    decode_parms = dict_get_raw(dict, "DP");
  }
  if (!filter) {
    return copy_bytes(context, object->as.stream.data, object->as.stream.size, out_data, out_size);
  }

  status = copy_bytes(context, object->as.stream.data, object->as.stream.size, &current, &current_size);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (filter->type == NANOPDF_BASIC_OBJECT_NAME) {
    nanopdf_basic_decode_params params;
    init_decode_params(&params);
    if (decode_parms && decode_parms->type != NANOPDF_BASIC_OBJECT_NULL) {
      status = parse_decode_params(context, document, decode_parms, depth, &params);
      if (status != NANOPDF_STATUS_OK) {
        nanopdf__allocator_free(&context->allocator, current);
        destroy_decode_params(context, &params);
        return status;
      }
    }
    status = decode_filter_step(
        context,
        filter->as.text,
        &params,
        current,
        current_size,
        out_data,
        out_size);
    destroy_decode_params(context, &params);
    nanopdf__allocator_free(&context->allocator, current);
    return status;
  }

  if (filter->type == NANOPDF_BASIC_OBJECT_ARRAY) {
    for (i = 0; i < filter->as.array.count; ++i) {
      nanopdf_basic_decode_params params;
      uint8_t* next = NULL;
      size_t next_size = 0;
      const nanopdf_basic_object* item = &filter->as.array.items[i];
      const nanopdf_basic_object* param_object = NULL;

      if (item->type != NANOPDF_BASIC_OBJECT_NAME) {
        nanopdf__allocator_free(&context->allocator, current);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "Filter array contains non-name");
      }

      init_decode_params(&params);
      if (decode_parms) {
        if (decode_parms->type == NANOPDF_BASIC_OBJECT_ARRAY) {
          if (i < decode_parms->as.array.count) {
            param_object = &decode_parms->as.array.items[i];
          }
        } else if (decode_parms->type != NANOPDF_BASIC_OBJECT_NULL && i == 0) {
          param_object = decode_parms;
        }
      }
      if (param_object && param_object->type != NANOPDF_BASIC_OBJECT_NULL) {
        status = parse_decode_params(context, document, param_object, depth, &params);
        if (status != NANOPDF_STATUS_OK) {
          nanopdf__allocator_free(&context->allocator, current);
          destroy_decode_params(context, &params);
          return status;
        }
      }

      status = decode_filter_step(
          context,
          item->as.text,
          &params,
          current,
          current_size,
          &next,
          &next_size);
      destroy_decode_params(context, &params);
      nanopdf__allocator_free(&context->allocator, current);
      current = NULL;
      current_size = 0;
      if (status != NANOPDF_STATUS_OK) {
        nanopdf__allocator_free(&context->allocator, next);
        return status;
      }
      current = next;
      current_size = next_size;
    }

    *out_data = current;
    *out_size = current_size;
    return clear_status(context);
  }

  nanopdf__allocator_free(&context->allocator, current);
  return set_status(context, NANOPDF_STATUS_MALFORMED, "Filter must be name or array");
}

nanopdf_status nanopdf_basic_decode_stream(
    nanopdf_context* context,
    const nanopdf_basic_object* object,
    uint8_t** out_data,
    size_t* out_size) {
  return decode_stream_impl(context, NULL, object, 0u, out_data, out_size);
}

nanopdf_status nanopdf_basic_decode_stream_with_document(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    uint8_t** out_data,
    size_t* out_size) {
  return decode_stream_impl(context, document, object, 0u, out_data, out_size);
}

static int find_endstream(
    const char* data,
    size_t size,
    size_t pos,
    size_t* out_end) {
  size_t cursor = pos;
  while (cursor + 9 <= size) {
    if (memcmp(data + cursor, "endstream", 9) == 0) {
      *out_end = cursor;
      return 1;
    }
    cursor++;
  }
  return 0;
}

static nanopdf_status parse_indirect_object_internal(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const uint8_t* data,
    size_t size,
    size_t offset,
    nanopdf_basic_object* out_object);

nanopdf_status nanopdf_basic_parse_object_from_buffer(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t start,
    nanopdf_basic_object* out_object) {
  size_t pos = start;

  if (!context || !data || !out_object || start >= size) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid object buffer arguments");
  }

  nanopdf_basic_object_init(out_object);
  return parse_direct_object(context, (const char*)data, size, &pos, 0, out_object);
}

nanopdf_status nanopdf_basic_parse_indirect_object_at(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t offset,
    nanopdf_basic_object* out_object) {
  return parse_indirect_object_internal(context, NULL, data, size, offset, out_object);
}

static nanopdf_status parse_indirect_object_internal(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const uint8_t* data,
    size_t size,
    size_t offset,
    nanopdf_basic_object* out_object) {
  size_t pos = offset;
  uint32_t object_number = 0;
  uint32_t generation_number = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!context || !data || !out_object || offset >= size) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid indirect object arguments");
  }

  nanopdf_basic_object_init(out_object);
  skip_ws((const char*)data, size, &pos);
  if (!parse_unsigned_value((const char*)data, size, &pos, &object_number)) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid object number");
  }
  skip_ws((const char*)data, size, &pos);
  if (!parse_unsigned_value((const char*)data, size, &pos, &generation_number)) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid object generation");
  }
  skip_ws((const char*)data, size, &pos);
  if (!match_literal((const char*)data, size, pos, "obj")) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "missing obj marker");
  }
  pos += 3;

  status = parse_direct_object(context, (const char*)data, size, &pos, 0, out_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (document && document->security.authenticated) {
    status = decrypt_indirect_strings(
        context,
        document,
        object_number,
        (uint16_t)generation_number,
        out_object);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, out_object);
      return status;
    }
  }

  skip_ws((const char*)data, size, &pos);
  if (out_object->type == NANOPDF_BASIC_OBJECT_DICT &&
      match_literal((const char*)data, size, pos, "stream")) {
    const nanopdf_basic_object* length_obj = dict_get_raw(&out_object->as.dict, "Length");
    nanopdf_basic_object length_value;
    size_t stream_start = pos + 6;
    size_t stream_end = 0;
    size_t length = 0;
    nanopdf_basic_object stream_object;
    uint8_t* stream_data = NULL;
    int have_length = 0;

    nanopdf_basic_object_init(&length_value);

    if (stream_start < size && data[stream_start] == '\r') {
      stream_start++;
    }
    if (stream_start < size && data[stream_start] == '\n') {
      stream_start++;
    }

    if (length_obj && length_obj->type == NANOPDF_BASIC_OBJECT_NUMBER &&
        length_obj->as.number >= 0.0) {
      length = (size_t)length_obj->as.number;
      have_length = 1;
    } else if (document && length_obj && length_obj->type == NANOPDF_BASIC_OBJECT_REF &&
               length_obj->as.ref.valid) {
      status = nanopdf_basic_load_object(context, document, length_obj->as.ref, &length_value);
      if (status == NANOPDF_STATUS_OK &&
          length_value.type == NANOPDF_BASIC_OBJECT_NUMBER &&
          length_value.as.number >= 0.0) {
        length = (size_t)length_value.as.number;
        have_length = 1;
      }
      nanopdf_basic_object_destroy(&context->allocator, &length_value);
      if (status != NANOPDF_STATUS_OK &&
          status != NANOPDF_STATUS_NOT_FOUND &&
          status != NANOPDF_STATUS_UNSUPPORTED) {
        nanopdf_basic_object_destroy(&context->allocator, out_object);
        return status;
      }
      nanopdf__clear_error(context);
    }

    if (have_length) {
      if (stream_start + length > size) {
        if (document && document->recover_stream_length_enabled &&
            find_endstream((const char*)data, size, stream_start, &stream_end)) {
          while (stream_end > stream_start &&
                 (((const char*)data)[stream_end - 1] == '\n' ||
                  ((const char*)data)[stream_end - 1] == '\r')) {
            stream_end--;
          }
          length = stream_end - stream_start;
        } else {
          nanopdf_basic_object_destroy(&context->allocator, out_object);
          return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid stream length");
        }
      } else {
        stream_end = stream_start + length;
      }
    } else if (find_endstream((const char*)data, size, stream_start, &stream_end)) {
      while (stream_end > stream_start &&
             (((const char*)data)[stream_end - 1] == '\n' ||
              ((const char*)data)[stream_end - 1] == '\r')) {
        stream_end--;
      }
      length = stream_end - stream_start;
    } else {
      nanopdf_basic_object_destroy(&context->allocator, out_object);
      return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "unsupported stream length");
    }

    stream_data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, length == 0 ? 1 : length);
    if (!stream_data) {
      nanopdf_basic_object_destroy(&context->allocator, out_object);
      return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate stream data");
    }
    if (length > 0) {
      memcpy(stream_data, data + stream_start, length);
    }
    if (document && document->security.authenticated &&
        !security_is_xref_stream(out_object)) {
      status = decrypt_bytes_in_place(
          context,
          document,
          object_number,
          (uint16_t)generation_number,
          1,
          stream_data,
          &length);
      if (status != NANOPDF_STATUS_OK) {
        nanopdf__allocator_free(&context->allocator, stream_data);
        nanopdf_basic_object_destroy(&context->allocator, out_object);
        return status;
      }
    }

    nanopdf_basic_object_init(&stream_object);
    stream_object.type = NANOPDF_BASIC_OBJECT_STREAM;
    stream_object.as.stream.dict = out_object->as.dict;
    stream_object.as.stream.data = stream_data;
    stream_object.as.stream.size = length;
    memset(out_object, 0, sizeof(*out_object));
    *out_object = stream_object;
  }

  return clear_status(context);
}

static nanopdf_status load_from_object_stream(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_basic_ref ref,
    nanopdf_basic_object* out_object) {
  const nanopdf_basic_xref_entry* entry = NULL;
  nanopdf_basic_object stream_object;
  const nanopdf_basic_object* first_obj = NULL;
  const nanopdf_basic_object* count_obj = NULL;
  uint8_t* decoded = NULL;
  size_t decoded_size = 0;
  size_t pos = 0;
  uint32_t found_offset = 0;
  size_t first_offset = 0;
  size_t object_count = 0;
  size_t i = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;

  entry = &document->xrefs[ref.object_number];
  nanopdf_basic_object_init(&stream_object);

  status = nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){
          entry->object_stream_number,
          0,
          1},
      &stream_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (stream_object.type != NANOPDF_BASIC_OBJECT_STREAM) {
    nanopdf_basic_object_destroy(&context->allocator, &stream_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "object stream is not a stream");
  }

  first_obj = dict_get_raw(&stream_object.as.stream.dict, "First");
  count_obj = dict_get_raw(&stream_object.as.stream.dict, "N");
  if (!first_obj || first_obj->type != NANOPDF_BASIC_OBJECT_NUMBER ||
      !count_obj || count_obj->type != NANOPDF_BASIC_OBJECT_NUMBER) {
    nanopdf_basic_object_destroy(&context->allocator, &stream_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid object stream header");
  }
  first_offset = (size_t)first_obj->as.number;
  object_count = (size_t)count_obj->as.number;

  status = nanopdf_basic_decode_stream_with_document(
      context, document, &stream_object, &decoded, &decoded_size);
  nanopdf_basic_object_destroy(&context->allocator, &stream_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  for (i = 0; i < object_count; ++i) {
    uint32_t object_number = 0;
    uint32_t object_offset = 0;
    skip_ws((const char*)decoded, decoded_size, &pos);
    if (!parse_unsigned_value((const char*)decoded, decoded_size, &pos, &object_number)) {
      nanopdf__allocator_free(&context->allocator, decoded);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid object stream object number");
    }
    skip_ws((const char*)decoded, decoded_size, &pos);
    if (!parse_unsigned_value((const char*)decoded, decoded_size, &pos, &object_offset)) {
      nanopdf__allocator_free(&context->allocator, decoded);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid object stream object offset");
    }
    if (i == entry->object_stream_index) {
      found_offset = object_offset;
      break;
    }
  }

  if (first_offset + found_offset >= decoded_size) {
    nanopdf__allocator_free(&context->allocator, decoded);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "object stream offset out of range");
  }

  status = nanopdf_basic_parse_object_from_buffer(
      context,
      decoded,
      decoded_size,
      first_offset + found_offset,
      out_object);
  nanopdf__allocator_free(&context->allocator, decoded);
  return status;
}

nanopdf_status nanopdf_basic_load_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_basic_ref ref,
    nanopdf_basic_object* out_object) {
  if (!context || !document || !out_object || !ref.valid ||
      ref.object_number >= document->xref_count) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid object reference");
  }

  if (!document->xrefs[ref.object_number].present ||
      !document->xrefs[ref.object_number].in_use) {
    return set_status(context, NANOPDF_STATUS_NOT_FOUND, "xref entry is not in use");
  }
  if (document->xrefs[ref.object_number].compressed) {
    return load_from_object_stream(context, document, ref, out_object);
  }

  return parse_indirect_object_internal(
      context,
      document,
      document->data,
      document->data_size,
      document->xrefs[ref.object_number].offset,
      out_object);
}
