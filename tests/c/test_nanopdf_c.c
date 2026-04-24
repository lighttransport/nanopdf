#include "acutest.h"
#include "nanopdf_c.h"
#include "nanopdf_forms.h"
#include "nanopdf_parse.h"
#include "nanopdf_text.h"
#include "../../src/c/nanopdf_crypto.h"
#include "../../src/c/nanopdf_object.h"
#include "../../src/third_party/miniz.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CountingAllocator {
  size_t alloc_count;
  size_t realloc_count;
  size_t free_count;
} CountingAllocator;

static void* counting_alloc(void* user_data, size_t size) {
  CountingAllocator* allocator = (CountingAllocator*)user_data;
  allocator->alloc_count += 1;
  return malloc(size);
}

static void* counting_realloc(void* user_data, void* ptr, size_t size) {
  CountingAllocator* allocator = (CountingAllocator*)user_data;
  allocator->realloc_count += 1;
  return realloc(ptr, size);
}

static void counting_free(void* user_data, void* ptr) {
  CountingAllocator* allocator = (CountingAllocator*)user_data;
  allocator->free_count += 1;
  free(ptr);
}

static void append_text(char* buffer, size_t* length, const char* text) {
  size_t chunk = strlen(text);
  memcpy(buffer + *length, text, chunk);
  *length += chunk;
}

static void append_bytes(char* buffer, size_t* length, const void* data, size_t size) {
  memcpy(buffer + *length, data, size);
  *length += size;
}

static void append_format(char* buffer, size_t* length, const char* format, ...) {
  va_list args;
  int written;

  va_start(args, format);
  written = vsnprintf(buffer + *length, 4096 - *length, format, args);
  va_end(args);

  TEST_CHECK(written >= 0);
  *length += (size_t)written;
}

static const uint8_t k_jpeg_1x1_red[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x02, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x06,
    0x06, 0x05, 0x06, 0x09, 0x08, 0x0a, 0x0a, 0x09, 0x08, 0x09, 0x09, 0x0a,
    0x0c, 0x0f, 0x0c, 0x0a, 0x0b, 0x0e, 0x0b, 0x09, 0x09, 0x0d, 0x11, 0x0d,
    0x0e, 0x0f, 0x10, 0x10, 0x11, 0x10, 0x0a, 0x0c, 0x12, 0x13, 0x12, 0x10,
    0x13, 0x0f, 0x10, 0x10, 0x10, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x03, 0x03,
    0x03, 0x04, 0x03, 0x04, 0x08, 0x04, 0x04, 0x08, 0x10, 0x0b, 0x09, 0x0b,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x01, 0x03,
    0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
    0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4, 0x00, 0x15, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,
    0x11, 0x00, 0x3f, 0x00, 0x3a, 0x03, 0x15, 0x4d, 0xff, 0xd9};

static char* encode_asciihex(const uint8_t* data, size_t size, size_t* out_size) {
  static const char hex[] = "0123456789ABCDEF";
  char* encoded = (char*)malloc(size * 2 + 2);
  size_t i = 0;

  TEST_CHECK(encoded != NULL);
  for (i = 0; i < size; ++i) {
    encoded[i * 2] = hex[(data[i] >> 4) & 0x0f];
    encoded[i * 2 + 1] = hex[data[i] & 0x0f];
  }
  encoded[size * 2] = '>';
  encoded[size * 2 + 1] = '\0';
  *out_size = size * 2 + 1;
  return encoded;
}

static int has_zero_byte(const uint8_t* data, size_t size) {
  size_t i = 0;
  for (i = 0; i < size; ++i) {
    if (data[i] == 0) {
      return 1;
    }
  }
  return 0;
}

static int bytes_equal(const uint8_t* a, const uint8_t* b, size_t size) {
  return memcmp(a, b, size) == 0;
}

typedef struct TestBitBuilder {
  uint8_t bytes[64];
  size_t bit_count;
} TestBitBuilder;

static void bit_builder_init(TestBitBuilder* builder) {
  memset(builder, 0, sizeof(*builder));
}

static void bit_builder_append(TestBitBuilder* builder, uint16_t code, uint8_t length) {
  int i = 0;
  for (i = (int)length - 1; i >= 0; --i) {
    if ((code >> i) & 1u) {
      size_t bit = builder->bit_count;
      TEST_CHECK(bit / 8 < sizeof(builder->bytes));
      builder->bytes[bit / 8] |= (uint8_t)(1u << (7u - (bit % 8u)));
    }
    ++builder->bit_count;
  }
}

static void bit_builder_append_string(TestBitBuilder* builder, const char* bits) {
  while (*bits) {
    bit_builder_append(builder, (uint16_t)(*bits == '1' ? 1 : 0), 1);
    ++bits;
  }
}

static size_t bit_builder_size(const TestBitBuilder* builder) {
  return (builder->bit_count + 7u) / 8u;
}

static void append_ccitt_white_8(TestBitBuilder* builder) {
  bit_builder_append(builder, 0x13, 5);
}

static void append_ccitt_black_8(TestBitBuilder* builder) {
  bit_builder_append(builder, 0x05, 6);
}

static size_t build_ccitt_1d_sample(uint8_t out[64]) {
  TestBitBuilder builder;
  bit_builder_init(&builder);
  bit_builder_append(&builder, 0x001, 12);
  append_ccitt_white_8(&builder);
  append_ccitt_black_8(&builder);
  bit_builder_append(&builder, 0x001, 12);
  append_ccitt_white_8(&builder);
  append_ccitt_black_8(&builder);
  bit_builder_append(&builder, 0x001, 12);
  memcpy(out, builder.bytes, bit_builder_size(&builder));
  return bit_builder_size(&builder);
}

static size_t build_ccitt_2d_sample(uint8_t out[64]) {
  TestBitBuilder builder;
  bit_builder_init(&builder);
  bit_builder_append(&builder, 0x001, 12);
  bit_builder_append_string(&builder, "1");
  append_ccitt_white_8(&builder);
  append_ccitt_black_8(&builder);
  bit_builder_append(&builder, 0x001, 12);
  bit_builder_append_string(&builder, "0");
  bit_builder_append_string(&builder, "1");
  bit_builder_append_string(&builder, "1");
  bit_builder_append(&builder, 0x001, 12);
  memcpy(out, builder.bytes, bit_builder_size(&builder));
  return bit_builder_size(&builder);
}

static size_t build_ccitt_group4_sample(uint8_t out[64]) {
  TestBitBuilder builder;
  bit_builder_init(&builder);
  bit_builder_append(&builder, 0x001, 12);
  bit_builder_append_string(&builder, "001");
  append_ccitt_white_8(&builder);
  append_ccitt_black_8(&builder);
  bit_builder_append(&builder, 0x001, 12);
  bit_builder_append_string(&builder, "1");
  bit_builder_append_string(&builder, "1");
  bit_builder_append(&builder, 0x001, 12);
  bit_builder_append(&builder, 0x001, 12);
  memcpy(out, builder.bytes, bit_builder_size(&builder));
  return bit_builder_size(&builder);
}

static nanopdf_status decode_test_stream(
    nanopdf_context* context,
    const char* filter_name,
    const uint8_t* payload,
    size_t payload_size,
    uint8_t** out_data,
    size_t* out_size) {
  char prefix[192];
  const char* suffix = "\nendstream\nendobj\n";
  uint8_t* object_data = NULL;
  size_t prefix_size = 0;
  size_t suffix_size = strlen(suffix);
  size_t object_size = 0;
  nanopdf_basic_object object;
  nanopdf_status status = NANOPDF_STATUS_OK;

  snprintf(
      prefix,
      sizeof(prefix),
      "1 0 obj\n<< /Length %zu /Filter /%s >>\nstream\n",
      payload_size,
      filter_name);
  prefix_size = strlen(prefix);
  object_size = prefix_size + payload_size + suffix_size;
  object_data = (uint8_t*)malloc(object_size);
  TEST_CHECK(object_data != NULL);
  if (!object_data) {
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memcpy(object_data, prefix, prefix_size);
  if (payload_size > 0) {
    memcpy(object_data + prefix_size, payload, payload_size);
  }
  memcpy(object_data + prefix_size + payload_size, suffix, suffix_size);

  nanopdf_basic_object_init(&object);
  status = nanopdf_basic_parse_indirect_object_at(
      context, object_data, object_size, 0, &object);
  if (status == NANOPDF_STATUS_OK) {
    status = nanopdf_basic_decode_stream(context, &object, out_data, out_size);
  }

  nanopdf_basic_object_destroy(nanopdf_context_get_allocator(context), &object);
  free(object_data);
  return status;
}

static nanopdf_status decode_test_stream_with_decode_parms(
    nanopdf_context* context,
    const char* filter_name,
    const char* decode_parms,
    const uint8_t* payload,
    size_t payload_size,
    uint8_t** out_data,
    size_t* out_size) {
  char prefix[512];
  const char* suffix = "\nendstream\nendobj\n";
  uint8_t* object_data = NULL;
  size_t prefix_size = 0;
  size_t suffix_size = strlen(suffix);
  size_t object_size = 0;
  nanopdf_basic_object object;
  nanopdf_status status = NANOPDF_STATUS_OK;

  snprintf(
      prefix,
      sizeof(prefix),
      "1 0 obj\n<< /Length %zu /Filter /%s /DecodeParms << %s >> >>\nstream\n",
      payload_size,
      filter_name,
      decode_parms ? decode_parms : "");
  prefix_size = strlen(prefix);
  object_size = prefix_size + payload_size + suffix_size;
  object_data = (uint8_t*)malloc(object_size);
  TEST_CHECK(object_data != NULL);
  if (!object_data) {
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memcpy(object_data, prefix, prefix_size);
  if (payload_size > 0) {
    memcpy(object_data + prefix_size, payload, payload_size);
  }
  memcpy(object_data + prefix_size + payload_size, suffix, suffix_size);

  nanopdf_basic_object_init(&object);
  status = nanopdf_basic_parse_indirect_object_at(
      context, object_data, object_size, 0, &object);
  if (status == NANOPDF_STATUS_OK) {
    status = nanopdf_basic_decode_stream(context, &object, out_data, out_size);
  }

  nanopdf_basic_object_destroy(nanopdf_context_get_allocator(context), &object);
  free(object_data);
  return status;
}

static void pad_password(const char* password, uint8_t out[32]) {
  static const uint8_t padding[32] = {
      0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
      0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
      0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
      0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};
  size_t len = password ? strlen(password) : 0;
  if (len > 32) {
    len = 32;
  }
  memset(out, 0, 32);
  if (len > 0) {
    memcpy(out, password, len);
  }
  if (len < 32) {
    memcpy(out + len, padding, 32 - len);
  }
}

static void compute_owner_key_rc4(
    const char* owner_password,
    const char* user_password,
    int key_bits,
    int revision,
    uint8_t out_owner[32]) {
  uint8_t padded_owner[32];
  uint8_t padded_user[32];
  uint8_t digest[16];
  size_t key_len = 0;
  int i = 0;

  pad_password(
      (owner_password && owner_password[0] != '\0') ? owner_password : user_password,
      padded_owner);
  pad_password(user_password, padded_user);
  nanopdf_md5_hash(padded_owner, 32, digest);
  key_len = (size_t)(key_bits / 8);
  if (key_len == 0) {
    key_len = 5;
  }
  if (key_len > 16) {
    key_len = 16;
  }
  if (revision >= 3) {
    for (i = 0; i < 50; ++i) {
      nanopdf_md5_hash(digest, key_len, digest);
    }
  }
  memcpy(out_owner, padded_user, 32);
  {
    nanopdf_rc4 rc4;
    nanopdf_rc4_init(&rc4, digest, key_len);
    nanopdf_rc4_crypt(&rc4, out_owner, 32);
  }
  if (revision >= 3) {
    for (i = 1; i <= 19; ++i) {
      uint8_t iter_key[16];
      size_t j = 0;
      for (j = 0; j < key_len; ++j) {
        iter_key[j] = (uint8_t)(digest[j] ^ i);
      }
      {
        nanopdf_rc4 rc4;
        nanopdf_rc4_init(&rc4, iter_key, key_len);
        nanopdf_rc4_crypt(&rc4, out_owner, 32);
      }
    }
  }
}

static void compute_file_key_rc4(
    const char* user_password,
    const uint8_t owner_key[32],
    int32_t permissions,
    const uint8_t file_id[16],
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t out_key[16],
    uint8_t* out_key_length) {
  uint8_t padded[32];
  nanopdf_md5 md5;
  uint8_t digest[16];
  int i = 0;
  size_t key_len = 0;

  pad_password(user_password, padded);
  nanopdf_md5_init(&md5);
  nanopdf_md5_update(&md5, padded, 32);
  nanopdf_md5_update(&md5, owner_key, 32);
  digest[0] = (uint8_t)(permissions & 0xff);
  digest[1] = (uint8_t)((permissions >> 8) & 0xff);
  digest[2] = (uint8_t)((permissions >> 16) & 0xff);
  digest[3] = (uint8_t)((permissions >> 24) & 0xff);
  nanopdf_md5_update(&md5, digest, 4);
  nanopdf_md5_update(&md5, file_id, 16);
  if (revision >= 4 && !encrypt_metadata) {
    static const uint8_t ff_bytes[4] = {0xff, 0xff, 0xff, 0xff};
    nanopdf_md5_update(&md5, ff_bytes, 4);
  }
  nanopdf_md5_final(&md5, digest);

  key_len = (size_t)(key_bits / 8);
  if (key_len == 0) {
    key_len = 5;
  }
  if (key_len > 16) {
    key_len = 16;
  }
  if (revision >= 3) {
    for (i = 0; i < 50; ++i) {
      nanopdf_md5_hash(digest, key_len, digest);
    }
  }

  memcpy(out_key, digest, key_len);
  *out_key_length = (uint8_t)key_len;
}

static void compute_user_key_rc4(
    const uint8_t* file_key,
    uint8_t file_key_length,
    const uint8_t file_id[16],
    int revision,
    uint8_t out_user[32]) {
  static const uint8_t padding[32] = {
      0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
      0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
      0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
      0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};
  if (revision == 2) {
    nanopdf_rc4 rc4;
    memcpy(out_user, padding, 32);
    nanopdf_rc4_init(&rc4, file_key, file_key_length);
    nanopdf_rc4_crypt(&rc4, out_user, 32);
  } else {
    uint8_t digest[16];
    int i = 0;
    nanopdf_md5 md5;
    nanopdf_md5_init(&md5);
    nanopdf_md5_update(&md5, padding, 32);
    nanopdf_md5_update(&md5, file_id, 16);
    nanopdf_md5_final(&md5, digest);
    memcpy(out_user, digest, 16);
    for (i = 0; i < 16; ++i) {
      out_user[16 + i] = (uint8_t)('A' + i);
    }
    {
      nanopdf_rc4 rc4;
      nanopdf_rc4_init(&rc4, file_key, file_key_length);
      nanopdf_rc4_crypt(&rc4, out_user, 16);
    }
    for (i = 1; i <= 19; ++i) {
      uint8_t iter_key[16];
      size_t j = 0;
      for (j = 0; j < file_key_length; ++j) {
        iter_key[j] = (uint8_t)(file_key[j] ^ i);
      }
      {
        nanopdf_rc4 rc4;
        nanopdf_rc4_init(&rc4, iter_key, file_key_length);
        nanopdf_rc4_crypt(&rc4, out_user, 16);
      }
    }
  }
}

static void compute_object_key_rc4(
    const uint8_t* file_key,
    uint8_t file_key_length,
    uint32_t object_number,
    uint16_t generation,
    uint8_t out_key[16],
    uint8_t* out_key_length) {
  uint8_t material[21];
  uint8_t digest[16];
  memcpy(material, file_key, file_key_length);
  material[file_key_length + 0] = (uint8_t)(object_number & 0xff);
  material[file_key_length + 1] = (uint8_t)((object_number >> 8) & 0xff);
  material[file_key_length + 2] = (uint8_t)((object_number >> 16) & 0xff);
  material[file_key_length + 3] = (uint8_t)(generation & 0xff);
  material[file_key_length + 4] = (uint8_t)((generation >> 8) & 0xff);
  nanopdf_md5_hash(material, (size_t)file_key_length + 5, digest);
  *out_key_length = (uint8_t)(file_key_length + 5);
  if (*out_key_length > 16) {
    *out_key_length = 16;
  }
  memcpy(out_key, digest, *out_key_length);
}

static void compute_object_key_aesv2(
    const uint8_t* file_key,
    uint8_t file_key_length,
    uint32_t object_number,
    uint16_t generation,
    uint8_t out_key[16],
    uint8_t* out_key_length) {
  uint8_t material[25];
  uint8_t digest[16];
  memcpy(material, file_key, file_key_length);
  material[file_key_length + 0] = (uint8_t)(object_number & 0xff);
  material[file_key_length + 1] = (uint8_t)((object_number >> 8) & 0xff);
  material[file_key_length + 2] = (uint8_t)((object_number >> 16) & 0xff);
  material[file_key_length + 3] = (uint8_t)(generation & 0xff);
  material[file_key_length + 4] = (uint8_t)((generation >> 8) & 0xff);
  material[file_key_length + 5] = (uint8_t)'s';
  material[file_key_length + 6] = (uint8_t)'A';
  material[file_key_length + 7] = (uint8_t)'l';
  material[file_key_length + 8] = (uint8_t)'T';
  nanopdf_md5_hash(material, (size_t)file_key_length + 9, digest);
  *out_key_length = (uint8_t)(file_key_length + 5);
  if (*out_key_length > 16) {
    *out_key_length = 16;
  }
  memcpy(out_key, digest, *out_key_length);
}

static uint8_t* encrypt_object_data_aesv2(
    const uint8_t* plaintext,
    size_t plaintext_size,
    const uint8_t* file_key,
    uint8_t file_key_length,
    uint32_t object_number,
    uint16_t generation,
    const uint8_t iv[16],
    size_t* out_size) {
  nanopdf_aes128 aes128;
  uint8_t object_key[16];
  uint8_t object_key_length = 0;
  uint8_t* padded = NULL;
  uint8_t* encrypted = NULL;
  size_t padded_size = 0;
  uint8_t pad = 0;

  padded_size = ((plaintext_size / 16) + 1) * 16;
  pad = (uint8_t)(padded_size - plaintext_size);
  padded = (uint8_t*)malloc(padded_size);
  TEST_CHECK(padded != NULL);
  memcpy(padded, plaintext, plaintext_size);
  memset(padded + plaintext_size, pad, padded_size - plaintext_size);

  encrypted = (uint8_t*)malloc(16 + padded_size);
  TEST_CHECK(encrypted != NULL);
  memcpy(encrypted, iv, 16);

  compute_object_key_aesv2(
      file_key, file_key_length, object_number, generation, object_key, &object_key_length);
  nanopdf_aes128_set_key(&aes128, object_key);
  nanopdf_aes128_encrypt_cbc(&aes128, padded, encrypted + 16, padded_size, iv);

  free(padded);
  *out_size = 16 + padded_size;
  return encrypted;
}

static uint8_t* encrypt_object_data_aes256(
    const uint8_t* plaintext,
    size_t plaintext_size,
    const uint8_t file_key[32],
    const uint8_t iv[16],
    size_t* out_size) {
  nanopdf_aes256 aes256;
  uint8_t* padded = NULL;
  uint8_t* encrypted = NULL;
  size_t padded_size = ((plaintext_size / 16) + 1) * 16;
  uint8_t pad = (uint8_t)(padded_size - plaintext_size);

  padded = (uint8_t*)malloc(padded_size);
  TEST_CHECK(padded != NULL);
  memcpy(padded, plaintext, plaintext_size);
  memset(padded + plaintext_size, pad, padded_size - plaintext_size);

  encrypted = (uint8_t*)malloc(16 + padded_size);
  TEST_CHECK(encrypted != NULL);
  memcpy(encrypted, iv, 16);
  nanopdf_aes256_set_key(&aes256, file_key);
  nanopdf_aes256_encrypt_cbc(&aes256, padded, encrypted + 16, padded_size, iv);

  free(padded);
  *out_size = 16 + padded_size;
  return encrypted;
}

static char* encode_ascii85(const uint8_t* data, size_t size, size_t* out_size) {
  char* encoded = (char*)malloc(size * 5 / 4 + 16);
  size_t length = 0;
  size_t i = 0;

  TEST_CHECK(encoded != NULL);
  while (i + 4 <= size) {
    uint32_t value =
        ((uint32_t)data[i] << 24) |
        ((uint32_t)data[i + 1] << 16) |
        ((uint32_t)data[i + 2] << 8) |
        (uint32_t)data[i + 3];
    char group[5];
    int j = 0;

    if (value == 0) {
      encoded[length++] = 'z';
      i += 4;
      continue;
    }

    for (j = 4; j >= 0; --j) {
      group[j] = (char)('!' + (value % 85u));
      value /= 85u;
    }
    memcpy(encoded + length, group, 5);
    length += 5;
    i += 4;
  }

  if (i < size) {
    uint32_t value = 0;
    size_t remaining = size - i;
    char group[5];
    size_t j = 0;
    for (j = 0; j < remaining; ++j) {
      value |= (uint32_t)data[i + j] << (24 - (int)(j * 8));
    }
    for (j = 5; j > 0; --j) {
      group[j - 1] = (char)('!' + (value % 85u));
      value /= 85u;
    }
    memcpy(encoded + length, group, remaining + 1);
    length += remaining + 1;
  }

  encoded[length++] = '~';
  encoded[length++] = '>';
  encoded[length] = '\0';
  *out_size = length;
  return encoded;
}

static uint8_t* encode_runlength(const uint8_t* data, size_t size, size_t* out_size) {
  uint8_t* encoded = (uint8_t*)malloc(size * 2 + 8);
  size_t length = 0;
  size_t pos = 0;

  TEST_CHECK(encoded != NULL);
  while (pos < size) {
    size_t chunk = size - pos;
    if (chunk > 128) {
      chunk = 128;
    }
    encoded[length++] = (uint8_t)(chunk - 1);
    memcpy(encoded + length, data + pos, chunk);
    length += chunk;
    pos += chunk;
  }
  encoded[length++] = 128;
  *out_size = length;
  return encoded;
}

static uint8_t* build_minimal_one_page_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 19 >>\n"
      "stream\n"
      "BT (Hello C) Tj ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_multi_stream_text_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents [4 0 R 5 0 R] >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 16 >>\n"
      "stream\n"
      "BT <4869> Tj ET\n"
      "endstream\n"
      "endobj\n";
  const char* obj5 =
      "5 0 obj\n"
      "<< /Length 29 >>\n"
      "stream\n"
      "BT [( there) -120 (C)] TJ ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  off5 = length;
  append_text(pdf, &length, obj5);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_text(pdf, &length, "trailer\n<< /Size 6 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_utf16be_text_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 36 >>\n"
      "stream\n"
      "BT <FEFF00480069002065E5672C> Tj ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_non_text_strings_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content =
      "BT (Visible) Tj ET\n"
      "/Artifact << /ActualText (Hidden) >> BDC EMC\n"
      "[(Nope) 10 (StillNope)] /NotText\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(pdf, &length, "4 0 obj\n<< /Length %zu >>\nstream\n", strlen(content));
  append_text(pdf, &length, content);
  append_text(pdf, &length, "endstream\nendobj\n");
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_utf16be_info_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Title <FEFF004D006500740061002065E5672C> >>\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_pdfdocencoding_info_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Title <4461736820842042756C6C65742080204575726F20A0> >>\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_basic_form_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>\n"
      "endobj\n";
  const char* obj5 =
      "5 0 obj\n"
      "<< /Fields [6 0 R 7 0 R 8 0 R] >>\n"
      "endobj\n";
  const char* obj6 =
      "6 0 obj\n"
      "<< /FT /Tx /T (Name) /TU (Display Name) /TM (name_export) /Ff 1 /V (Alice) >>\n"
      "endobj\n";
  const char* obj7 =
      "7 0 obj\n"
      "<< /FT /Btn /T (Accept) /V /Off >>\n"
      "endobj\n";
  const char* obj8 =
      "8 0 obj\n"
      "<< /FT /Ch /T (Mode) /V /One >>\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off5;
  size_t off6;
  size_t off7;
  size_t off8;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off5 = length;
  append_text(pdf, &length, obj5);
  off6 = length;
  append_text(pdf, &length, obj6);
  off7 = length;
  append_text(pdf, &length, obj7);
  off8 = length;
  append_text(pdf, &length, obj8);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 9\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_text(pdf, &length, "0000000000 00000 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_format(pdf, &length, "%010zu 00000 n \n", off7);
  append_format(pdf, &length, "%010zu 00000 n \n", off8);
  append_text(pdf, &length, "trailer\n<< /Size 9 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_encrypted_text_pdf(
    const char* user_password,
    const char* owner_password,
    const char* plaintext,
    size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R >>\n"
      "endobj\n";
  char* encoded_stream = NULL;
  uint8_t* encrypted_stream = NULL;
  char owner_hex[65];
  char user_hex[65];
  char file_id_hex[33];
  uint8_t owner_key[32];
  uint8_t user_key[32];
  uint8_t file_id[16];
  uint8_t file_key[16];
  uint8_t file_key_length = 0;
  uint8_t object_key[16];
  uint8_t object_key_length = 0;
  size_t encoded_size = 0;
  size_t i = 0;
  int32_t permissions = -4;
  int key_bits = 40;
  int revision = 2;
  uint32_t seed = 1;
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  TEST_CHECK(user_password != NULL);
  TEST_CHECK(owner_password != NULL);
  TEST_CHECK(plaintext != NULL);

  for (;;) {
    for (i = 0; i < sizeof(file_id); ++i) {
      file_id[i] = (uint8_t)('A' + ((seed + (uint32_t)i) % 26u));
    }
    compute_owner_key_rc4(owner_password, user_password, key_bits, revision, owner_key);
    compute_file_key_rc4(
        user_password,
        owner_key,
        permissions,
        file_id,
        key_bits,
        revision,
        1,
        file_key,
        &file_key_length);
    compute_user_key_rc4(file_key, file_key_length, file_id, revision, user_key);
    if (!has_zero_byte(owner_key, sizeof(owner_key)) &&
        !has_zero_byte(user_key, sizeof(user_key)) &&
        !has_zero_byte(file_id, sizeof(file_id))) {
      break;
    }
    seed += 1;
    TEST_CHECK(seed < 10000);
  }

  encoded_stream = encode_asciihex(
      (const uint8_t*)plaintext, strlen(plaintext), &encoded_size);
  TEST_CHECK(encoded_stream != NULL);
  encrypted_stream = (uint8_t*)malloc(encoded_size);
  TEST_CHECK(encrypted_stream != NULL);
  memcpy(encrypted_stream, encoded_stream, encoded_size);
  compute_object_key_rc4(file_key, file_key_length, 4, 0, object_key, &object_key_length);
  {
    nanopdf_rc4 rc4;
    nanopdf_rc4_init(&rc4, object_key, object_key_length);
    nanopdf_rc4_crypt(&rc4, encrypted_stream, encoded_size);
  }

  for (i = 0; i < 32; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    owner_hex[i * 2] = hex[(owner_key[i] >> 4) & 0x0f];
    owner_hex[i * 2 + 1] = hex[owner_key[i] & 0x0f];
    user_hex[i * 2] = hex[(user_key[i] >> 4) & 0x0f];
    user_hex[i * 2 + 1] = hex[user_key[i] & 0x0f];
  }
  owner_hex[64] = '\0';
  user_hex[64] = '\0';
  for (i = 0; i < 16; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    file_id_hex[i * 2] = hex[(file_id[i] >> 4) & 0x0f];
    file_id_hex[i * 2 + 1] = hex[file_id[i] & 0x0f];
  }
  file_id_hex[32] = '\0';

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(
      pdf,
      &length,
      "4 0 obj\n<< /Length %zu /Filter /ASCIIHexDecode >>\nstream\n",
      encoded_size);
  append_bytes(pdf, &length, encrypted_stream, encoded_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Filter /Standard /V 1 /R 2 /O <%s> /U <%s> /P %d >>\n"
      "endobj\n",
      owner_hex,
      user_hex,
      permissions);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(
      pdf,
      &length,
      "trailer\n<< /Size 6 /Root 1 0 R /Encrypt 5 0 R /ID [<%s> <%s>] >>\n",
      file_id_hex,
      file_id_hex);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encrypted_stream);
  free(encoded_stream);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_encrypted_v4_crypt_filter_pdf(size_t* out_size) {
  const char* header = "%PDF-1.6\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* plaintext = "BT (V4 Secret) Tj ET\n";
  const char* title_plaintext = "V4 Title";
  char* encoded_stream = NULL;
  uint8_t* encrypted_stream = NULL;
  char owner_hex[65];
  char user_hex[65];
  char file_id_hex[33];
  char title_hex[64];
  uint8_t owner_key[32];
  uint8_t user_key[32];
  uint8_t file_id[16];
  uint8_t file_key[16];
  uint8_t file_key_length = 0;
  uint8_t stream_key[16];
  uint8_t title_key[16];
  uint8_t stream_key_length = 0;
  uint8_t title_key_length = 0;
  uint8_t title_bytes[32];
  size_t encoded_size = 0;
  size_t title_size = strlen(title_plaintext);
  size_t i = 0;
  int32_t permissions = -1028;
  int key_bits = 128;
  int revision = 4;
  uint32_t seed = 11;
  char* pdf = (char*)malloc(12288);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);

  for (i = 0; i < sizeof(file_id); ++i) {
    file_id[i] = (uint8_t)('K' + ((seed + (uint32_t)i) % 16u));
  }
  compute_owner_key_rc4("owner4", "user4", key_bits, revision, owner_key);
  compute_file_key_rc4(
      "user4",
      owner_key,
      permissions,
      file_id,
      key_bits,
      revision,
      1,
      file_key,
      &file_key_length);
  compute_user_key_rc4(file_key, file_key_length, file_id, revision, user_key);

  encoded_stream = encode_asciihex(
      (const uint8_t*)plaintext, strlen(plaintext), &encoded_size);
  TEST_CHECK(encoded_stream != NULL);
  encrypted_stream = (uint8_t*)malloc(encoded_size);
  TEST_CHECK(encrypted_stream != NULL);
  memcpy(encrypted_stream, encoded_stream, encoded_size);
  compute_object_key_rc4(file_key, file_key_length, 4, 0, stream_key, &stream_key_length);
  {
    nanopdf_rc4 rc4;
    nanopdf_rc4_init(&rc4, stream_key, stream_key_length);
    nanopdf_rc4_crypt(&rc4, encrypted_stream, encoded_size);
  }

  memcpy(title_bytes, title_plaintext, title_size);
  compute_object_key_rc4(file_key, file_key_length, 6, 0, title_key, &title_key_length);
  {
    nanopdf_rc4 rc4;
    nanopdf_rc4_init(&rc4, title_key, title_key_length);
    nanopdf_rc4_crypt(&rc4, title_bytes, title_size);
  }

  for (i = 0; i < 32; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    owner_hex[i * 2] = hex[(owner_key[i] >> 4) & 0x0f];
    owner_hex[i * 2 + 1] = hex[owner_key[i] & 0x0f];
    user_hex[i * 2] = hex[(user_key[i] >> 4) & 0x0f];
    user_hex[i * 2 + 1] = hex[user_key[i] & 0x0f];
  }
  owner_hex[64] = '\0';
  user_hex[64] = '\0';
  for (i = 0; i < 16; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    file_id_hex[i * 2] = hex[(file_id[i] >> 4) & 0x0f];
    file_id_hex[i * 2 + 1] = hex[file_id[i] & 0x0f];
  }
  file_id_hex[32] = '\0';
  for (i = 0; i < title_size; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    title_hex[i * 2] = hex[(title_bytes[i] >> 4) & 0x0f];
    title_hex[i * 2 + 1] = hex[title_bytes[i] & 0x0f];
  }
  title_hex[title_size * 2] = '\0';

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(
      pdf,
      &length,
      "4 0 obj\n<< /Length %zu /Filter /ASCIIHexDecode >>\nstream\n",
      encoded_size);
  append_bytes(pdf, &length, encrypted_stream, encoded_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Filter /Standard /V 4 /R 4 /Length 128 /O <%s> /U <%s> /P %d "
      "/CF << /StdCF << /CFM /V2 >> >> /StmF /StdCF /StrF /StdCF >>\n"
      "endobj\n",
      owner_hex,
      user_hex,
      permissions);
  off6 = length;
  append_format(pdf, &length, "6 0 obj\n<< /Title <%s> >>\nendobj\n", title_hex);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 7\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_format(
      pdf,
      &length,
      "trailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R /Encrypt 5 0 R /ID [<%s> <%s>] >>\n",
      file_id_hex,
      file_id_hex);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encrypted_stream);
  free(encoded_stream);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_encrypted_v4_aesv2_pdf(size_t* out_size) {
  const char* header = "%PDF-1.6\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 240] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* plaintext = "BT (AES Secret) Tj ET\n";
  const char* title_plaintext = "AES Title";
  char owner_hex[65];
  char user_hex[65];
  char file_id_hex[33];
  char title_hex[128];
  uint8_t owner_key[32];
  uint8_t user_key[32];
  uint8_t file_id[16];
  uint8_t file_key[16];
  uint8_t file_key_length = 0;
  uint8_t stream_iv[16];
  uint8_t title_iv[16];
  uint8_t* encrypted_stream = NULL;
  uint8_t* encrypted_title = NULL;
  size_t encrypted_stream_size = 0;
  size_t encrypted_title_size = 0;
  size_t i = 0;
  int32_t permissions = -1032;
  int key_bits = 128;
  int revision = 4;
  uint32_t seed = 19;
  char* pdf = (char*)malloc(16384);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);

  for (i = 0; i < sizeof(file_id); ++i) {
    file_id[i] = (uint8_t)('Q' + ((seed + (uint32_t)i) % 10u));
    stream_iv[i] = (uint8_t)(0x10u + i);
    title_iv[i] = (uint8_t)(0x80u + i);
  }

  compute_owner_key_rc4("aesowner", "aesuser", key_bits, revision, owner_key);
  compute_file_key_rc4(
      "aesuser",
      owner_key,
      permissions,
      file_id,
      key_bits,
      revision,
      1,
      file_key,
      &file_key_length);
  compute_user_key_rc4(file_key, file_key_length, file_id, revision, user_key);

  encrypted_stream = encrypt_object_data_aesv2(
      (const uint8_t*)plaintext,
      strlen(plaintext),
      file_key,
      file_key_length,
      4,
      0,
      stream_iv,
      &encrypted_stream_size);
  encrypted_title = encrypt_object_data_aesv2(
      (const uint8_t*)title_plaintext,
      strlen(title_plaintext),
      file_key,
      file_key_length,
      6,
      0,
      title_iv,
      &encrypted_title_size);

  for (i = 0; i < 32; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    owner_hex[i * 2] = hex[(owner_key[i] >> 4) & 0x0f];
    owner_hex[i * 2 + 1] = hex[owner_key[i] & 0x0f];
    user_hex[i * 2] = hex[(user_key[i] >> 4) & 0x0f];
    user_hex[i * 2 + 1] = hex[user_key[i] & 0x0f];
  }
  owner_hex[64] = '\0';
  user_hex[64] = '\0';
  for (i = 0; i < 16; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    file_id_hex[i * 2] = hex[(file_id[i] >> 4) & 0x0f];
    file_id_hex[i * 2 + 1] = hex[file_id[i] & 0x0f];
  }
  file_id_hex[32] = '\0';
  for (i = 0; i < encrypted_title_size; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    title_hex[i * 2] = hex[(encrypted_title[i] >> 4) & 0x0f];
    title_hex[i * 2 + 1] = hex[encrypted_title[i] & 0x0f];
  }
  title_hex[encrypted_title_size * 2] = '\0';

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(pdf, &length, "4 0 obj\n<< /Length %zu >>\nstream\n", encrypted_stream_size);
  append_bytes(pdf, &length, encrypted_stream, encrypted_stream_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Filter /Standard /V 4 /R 4 /Length 128 /O <%s> /U <%s> /P %d "
      "/CF << /StdCF << /CFM /AESV2 /Length 16 >> >> /StmF /StdCF /StrF /StdCF >>\n"
      "endobj\n",
      owner_hex,
      user_hex,
      permissions);
  off6 = length;
  append_format(pdf, &length, "6 0 obj\n<< /Title <%s> >>\nendobj\n", title_hex);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 7\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_format(
      pdf,
      &length,
      "trailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R /Encrypt 5 0 R /ID [<%s> <%s>] >>\n",
      file_id_hex,
      file_id_hex);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encrypted_title);
  free(encrypted_stream);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_encrypted_v5_aes256_pdf(size_t* out_size) {
  const char* header = "%PDF-2.0\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 260 260] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* plaintext = "BT (AES256 Secret) Tj ET\n";
  const char* title_plaintext = "AES256 Title";
  uint8_t file_key[32];
  uint8_t file_id[16];
  uint8_t user_val_salt[8];
  uint8_t user_key_salt[8];
  uint8_t owner_val_salt[8];
  uint8_t owner_key_salt[8];
  uint8_t u_hash[32];
  uint8_t u_key_hash[32];
  uint8_t o_hash[32];
  uint8_t o_key_hash[32];
  uint8_t u_value[48];
  uint8_t o_value[48];
  uint8_t ue_value[32];
  uint8_t oe_value[32];
  uint8_t perms_plaintext[16];
  uint8_t perms_value[16];
  uint8_t stream_iv[16];
  uint8_t title_iv[16];
  uint8_t zero_iv[16] = {0};
  uint8_t* encrypted_stream = NULL;
  uint8_t* encrypted_title = NULL;
  char o_hex[97];
  char u_hex[97];
  char oe_hex[65];
  char ue_hex[65];
  char perms_hex[33];
  char file_id_hex[33];
  char title_hex[256];
  nanopdf_aes256 aes256;
  size_t encrypted_stream_size = 0;
  size_t encrypted_title_size = 0;
  size_t i = 0;
  int32_t permissions = -1028;
  char* pdf = (char*)malloc(24576);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);

  for (i = 0; i < sizeof(file_key); ++i) {
    file_key[i] = (uint8_t)(0x31 + (i % 40));
  }
  for (i = 0; i < sizeof(file_id); ++i) {
    file_id[i] = (uint8_t)('M' + (i % 10));
    stream_iv[i] = (uint8_t)(0x20 + i);
    title_iv[i] = (uint8_t)(0x40 + i);
  }
  for (i = 0; i < 8; ++i) {
    user_val_salt[i] = (uint8_t)(0x11 + i);
    user_key_salt[i] = (uint8_t)(0x21 + i);
    owner_val_salt[i] = (uint8_t)(0x31 + i);
    owner_key_salt[i] = (uint8_t)(0x41 + i);
  }

  {
    uint8_t buffer[127 + 8];
    memcpy(buffer, "aes256user", 10);
    memcpy(buffer + 10, user_val_salt, 8);
    nanopdf_sha256_hash(buffer, 18, u_hash);
    memcpy(u_value, u_hash, 32);
    memcpy(u_value + 32, user_val_salt, 8);
    memcpy(u_value + 40, user_key_salt, 8);

    memcpy(buffer, "aes256user", 10);
    memcpy(buffer + 10, user_key_salt, 8);
    nanopdf_sha256_hash(buffer, 18, u_key_hash);
  }

  nanopdf_aes256_set_key(&aes256, u_key_hash);
  nanopdf_aes256_encrypt_cbc(&aes256, file_key, ue_value, 32, zero_iv);

  {
    uint8_t buffer[127 + 8 + 48];
    memcpy(buffer, "aes256owner", 11);
    memcpy(buffer + 11, owner_val_salt, 8);
    memcpy(buffer + 19, u_value, 48);
    nanopdf_sha256_hash(buffer, 67, o_hash);
    memcpy(o_value, o_hash, 32);
    memcpy(o_value + 32, owner_val_salt, 8);
    memcpy(o_value + 40, owner_key_salt, 8);

    memcpy(buffer, "aes256owner", 11);
    memcpy(buffer + 11, owner_key_salt, 8);
    memcpy(buffer + 19, u_value, 48);
    nanopdf_sha256_hash(buffer, 67, o_key_hash);
  }

  nanopdf_aes256_set_key(&aes256, o_key_hash);
  nanopdf_aes256_encrypt_cbc(&aes256, file_key, oe_value, 32, zero_iv);

  perms_plaintext[0] = (uint8_t)(permissions & 0xff);
  perms_plaintext[1] = (uint8_t)((permissions >> 8) & 0xff);
  perms_plaintext[2] = (uint8_t)((permissions >> 16) & 0xff);
  perms_plaintext[3] = (uint8_t)((permissions >> 24) & 0xff);
  perms_plaintext[4] = 0xff;
  perms_plaintext[5] = 0xff;
  perms_plaintext[6] = 0xff;
  perms_plaintext[7] = 0xff;
  perms_plaintext[8] = 'T';
  perms_plaintext[9] = 'a';
  perms_plaintext[10] = 'd';
  perms_plaintext[11] = 'b';
  perms_plaintext[12] = 0x55;
  perms_plaintext[13] = 0x56;
  perms_plaintext[14] = 0x57;
  perms_plaintext[15] = 0x58;
  nanopdf_aes256_set_key(&aes256, file_key);
  nanopdf_aes256_encrypt_block(&aes256, perms_plaintext, perms_value);

  encrypted_stream = encrypt_object_data_aes256(
      (const uint8_t*)plaintext, strlen(plaintext), file_key, stream_iv, &encrypted_stream_size);
  encrypted_title = encrypt_object_data_aes256(
      (const uint8_t*)title_plaintext,
      strlen(title_plaintext),
      file_key,
      title_iv,
      &encrypted_title_size);

  for (i = 0; i < 48; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    o_hex[i * 2] = hex[(o_value[i] >> 4) & 0x0f];
    o_hex[i * 2 + 1] = hex[o_value[i] & 0x0f];
    u_hex[i * 2] = hex[(u_value[i] >> 4) & 0x0f];
    u_hex[i * 2 + 1] = hex[u_value[i] & 0x0f];
  }
  o_hex[96] = '\0';
  u_hex[96] = '\0';
  for (i = 0; i < 32; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    oe_hex[i * 2] = hex[(oe_value[i] >> 4) & 0x0f];
    oe_hex[i * 2 + 1] = hex[oe_value[i] & 0x0f];
    ue_hex[i * 2] = hex[(ue_value[i] >> 4) & 0x0f];
    ue_hex[i * 2 + 1] = hex[ue_value[i] & 0x0f];
  }
  oe_hex[64] = '\0';
  ue_hex[64] = '\0';
  for (i = 0; i < 16; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    perms_hex[i * 2] = hex[(perms_value[i] >> 4) & 0x0f];
    perms_hex[i * 2 + 1] = hex[perms_value[i] & 0x0f];
    file_id_hex[i * 2] = hex[(file_id[i] >> 4) & 0x0f];
    file_id_hex[i * 2 + 1] = hex[file_id[i] & 0x0f];
  }
  perms_hex[32] = '\0';
  file_id_hex[32] = '\0';
  for (i = 0; i < encrypted_title_size; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    title_hex[i * 2] = hex[(encrypted_title[i] >> 4) & 0x0f];
    title_hex[i * 2 + 1] = hex[encrypted_title[i] & 0x0f];
  }
  title_hex[encrypted_title_size * 2] = '\0';

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(pdf, &length, "4 0 obj\n<< /Length %zu >>\nstream\n", encrypted_stream_size);
  append_bytes(pdf, &length, encrypted_stream, encrypted_stream_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Filter /Standard /V 5 /R 5 /Length 256 /O <%s> /U <%s> /OE <%s> /UE <%s> /Perms <%s> "
      "/P %d /CF << /StdCF << /CFM /AESV3 /AuthEvent /DocOpen /Length 32 >> >> "
      "/StmF /StdCF /StrF /StdCF /EncryptMetadata true >>\n"
      "endobj\n",
      o_hex,
      u_hex,
      oe_hex,
      ue_hex,
      perms_hex,
      permissions);
  off6 = length;
  append_format(pdf, &length, "6 0 obj\n<< /Title <%s> >>\nendobj\n", title_hex);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 7\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_format(
      pdf,
      &length,
      "trailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R /Encrypt 5 0 R /ID [<%s> <%s>] >>\n",
      file_id_hex,
      file_id_hex);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encrypted_title);
  free(encrypted_stream);
  *out_size = length;
  return (uint8_t*)pdf;
}

static int compute_hash_r6_test(
    const uint8_t* password,
    size_t password_len,
    const uint8_t* salt,
    size_t salt_len,
    const uint8_t* u_data,
    size_t u_data_len,
    uint8_t out_hash[32]) {
  uint8_t k[64];
  uint8_t* input = NULL;
  size_t input_len = password_len + salt_len + u_data_len;
  size_t k_len = 32;
  int round = 0;

  input = (uint8_t*)malloc(input_len ? input_len : 1);
  TEST_CHECK(input != NULL);
  if (password_len > 0) {
    memcpy(input, password, password_len);
  }
  if (salt_len > 0) {
    memcpy(input + password_len, salt, salt_len);
  }
  if (u_data_len > 0) {
    memcpy(input + password_len + salt_len, u_data, u_data_len);
  }
  nanopdf_sha256_hash(input, input_len, k);
  free(input);

  for (;;) {
    size_t single_len = password_len + k_len + u_data_len;
    size_t k1_len = single_len * 64;
    uint8_t* k1 = (uint8_t*)malloc(k1_len);
    uint8_t* e = (uint8_t*)malloc(k1_len);
    unsigned int sum = 0;
    int i = 0;
    nanopdf_aes128 aes128;

    TEST_CHECK(k1 != NULL);
    TEST_CHECK(e != NULL);
    for (i = 0; i < 64; ++i) {
      uint8_t* dst = k1 + (size_t)i * single_len;
      if (password_len > 0) {
        memcpy(dst, password, password_len);
        dst += password_len;
      }
      memcpy(dst, k, k_len);
      dst += k_len;
      if (u_data_len > 0) {
        memcpy(dst, u_data, u_data_len);
      }
    }
    nanopdf_aes128_set_key(&aes128, k);
    nanopdf_aes128_encrypt_cbc(&aes128, k1, e, k1_len, k + 16);
    for (i = 0; i < 16; ++i) {
      sum += e[i];
    }
    if ((sum % 3u) == 0) {
      nanopdf_sha256_hash(e, k1_len, k);
      k_len = 32;
    } else if ((sum % 3u) == 1) {
      nanopdf_sha384_hash(e, k1_len, k);
      k_len = 48;
    } else {
      nanopdf_sha512_hash(e, k1_len, k);
      k_len = 64;
    }
    ++round;
    if (round >= 64 && e[k1_len - 1] <= round - 32) {
      free(e);
      free(k1);
      break;
    }
    free(e);
    free(k1);
  }

  memcpy(out_hash, k, 32);
  return 1;
}

static uint8_t* build_encrypted_v6_aes256_pdf(size_t* out_size) {
  const char* header = "%PDF-2.0\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 260 260] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* user_password = "aes256r6user";
  const char* owner_password = "aes256r6owner";
  const char* plaintext = "BT (AES256 R6 Secret) Tj ET\n";
  const char* title_plaintext = "AES256 R6 Title";
  uint8_t file_key[32];
  uint8_t file_id[16];
  uint8_t user_val_salt[8];
  uint8_t user_key_salt[8];
  uint8_t owner_val_salt[8];
  uint8_t owner_key_salt[8];
  uint8_t u_hash[32];
  uint8_t u_key_hash[32];
  uint8_t o_hash[32];
  uint8_t o_key_hash[32];
  uint8_t u_value[48];
  uint8_t o_value[48];
  uint8_t ue_value[32];
  uint8_t oe_value[32];
  uint8_t perms_plaintext[16];
  uint8_t perms_value[16];
  uint8_t stream_iv[16];
  uint8_t title_iv[16];
  uint8_t zero_iv[16] = {0};
  uint8_t* encrypted_stream = NULL;
  uint8_t* encrypted_title = NULL;
  char o_hex[97];
  char u_hex[97];
  char oe_hex[65];
  char ue_hex[65];
  char perms_hex[33];
  char file_id_hex[33];
  char title_hex[256];
  nanopdf_aes256 aes256;
  size_t encrypted_stream_size = 0;
  size_t encrypted_title_size = 0;
  size_t i = 0;
  int32_t permissions = -1028;
  char* pdf = (char*)malloc(24576);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);

  for (i = 0; i < sizeof(file_key); ++i) {
    file_key[i] = (uint8_t)(0x51 + (i % 40));
  }
  for (i = 0; i < sizeof(file_id); ++i) {
    file_id[i] = (uint8_t)('R' + (i % 8));
    stream_iv[i] = (uint8_t)(0x60 + i);
    title_iv[i] = (uint8_t)(0x70 + i);
  }
  for (i = 0; i < 8; ++i) {
    user_val_salt[i] = (uint8_t)(0x91 + i);
    user_key_salt[i] = (uint8_t)(0xa1 + i);
    owner_val_salt[i] = (uint8_t)(0xb1 + i);
    owner_key_salt[i] = (uint8_t)(0xc1 + i);
  }

  TEST_CHECK(compute_hash_r6_test(
                 (const uint8_t*)user_password,
                 strlen(user_password),
                 user_val_salt,
                 8,
                 NULL,
                 0,
                 u_hash) == 1);
  memcpy(u_value, u_hash, 32);
  memcpy(u_value + 32, user_val_salt, 8);
  memcpy(u_value + 40, user_key_salt, 8);
  TEST_CHECK(compute_hash_r6_test(
                 (const uint8_t*)user_password,
                 strlen(user_password),
                 user_key_salt,
                 8,
                 NULL,
                 0,
                 u_key_hash) == 1);
  nanopdf_aes256_set_key(&aes256, u_key_hash);
  nanopdf_aes256_encrypt_cbc(&aes256, file_key, ue_value, 32, zero_iv);

  TEST_CHECK(compute_hash_r6_test(
                 (const uint8_t*)owner_password,
                 strlen(owner_password),
                 owner_val_salt,
                 8,
                 u_value,
                 48,
                 o_hash) == 1);
  memcpy(o_value, o_hash, 32);
  memcpy(o_value + 32, owner_val_salt, 8);
  memcpy(o_value + 40, owner_key_salt, 8);
  TEST_CHECK(compute_hash_r6_test(
                 (const uint8_t*)owner_password,
                 strlen(owner_password),
                 owner_key_salt,
                 8,
                 u_value,
                 48,
                 o_key_hash) == 1);
  nanopdf_aes256_set_key(&aes256, o_key_hash);
  nanopdf_aes256_encrypt_cbc(&aes256, file_key, oe_value, 32, zero_iv);

  perms_plaintext[0] = (uint8_t)(permissions & 0xff);
  perms_plaintext[1] = (uint8_t)((permissions >> 8) & 0xff);
  perms_plaintext[2] = (uint8_t)((permissions >> 16) & 0xff);
  perms_plaintext[3] = (uint8_t)((permissions >> 24) & 0xff);
  perms_plaintext[4] = 0xff;
  perms_plaintext[5] = 0xff;
  perms_plaintext[6] = 0xff;
  perms_plaintext[7] = 0xff;
  perms_plaintext[8] = 'T';
  perms_plaintext[9] = 'a';
  perms_plaintext[10] = 'd';
  perms_plaintext[11] = 'b';
  perms_plaintext[12] = 0x65;
  perms_plaintext[13] = 0x66;
  perms_plaintext[14] = 0x67;
  perms_plaintext[15] = 0x68;
  nanopdf_aes256_set_key(&aes256, file_key);
  nanopdf_aes256_encrypt_block(&aes256, perms_plaintext, perms_value);

  encrypted_stream = encrypt_object_data_aes256(
      (const uint8_t*)plaintext, strlen(plaintext), file_key, stream_iv, &encrypted_stream_size);
  encrypted_title = encrypt_object_data_aes256(
      (const uint8_t*)title_plaintext,
      strlen(title_plaintext),
      file_key,
      title_iv,
      &encrypted_title_size);

  for (i = 0; i < 48; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    o_hex[i * 2] = hex[(o_value[i] >> 4) & 0x0f];
    o_hex[i * 2 + 1] = hex[o_value[i] & 0x0f];
    u_hex[i * 2] = hex[(u_value[i] >> 4) & 0x0f];
    u_hex[i * 2 + 1] = hex[u_value[i] & 0x0f];
  }
  o_hex[96] = '\0';
  u_hex[96] = '\0';
  for (i = 0; i < 32; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    oe_hex[i * 2] = hex[(oe_value[i] >> 4) & 0x0f];
    oe_hex[i * 2 + 1] = hex[oe_value[i] & 0x0f];
    ue_hex[i * 2] = hex[(ue_value[i] >> 4) & 0x0f];
    ue_hex[i * 2 + 1] = hex[ue_value[i] & 0x0f];
  }
  oe_hex[64] = '\0';
  ue_hex[64] = '\0';
  for (i = 0; i < 16; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    perms_hex[i * 2] = hex[(perms_value[i] >> 4) & 0x0f];
    perms_hex[i * 2 + 1] = hex[perms_value[i] & 0x0f];
    file_id_hex[i * 2] = hex[(file_id[i] >> 4) & 0x0f];
    file_id_hex[i * 2 + 1] = hex[file_id[i] & 0x0f];
  }
  perms_hex[32] = '\0';
  file_id_hex[32] = '\0';
  for (i = 0; i < encrypted_title_size; ++i) {
    static const char hex[] = "0123456789ABCDEF";
    title_hex[i * 2] = hex[(encrypted_title[i] >> 4) & 0x0f];
    title_hex[i * 2 + 1] = hex[encrypted_title[i] & 0x0f];
  }
  title_hex[encrypted_title_size * 2] = '\0';

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(pdf, &length, "4 0 obj\n<< /Length %zu >>\nstream\n", encrypted_stream_size);
  append_bytes(pdf, &length, encrypted_stream, encrypted_stream_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Filter /Standard /V 5 /R 6 /Length 256 /O <%s> /U <%s> /OE <%s> /UE <%s> /Perms <%s> "
      "/P %d /CF << /StdCF << /CFM /AESV3 /AuthEvent /DocOpen /Length 32 >> >> "
      "/StmF /StdCF /StrF /StdCF /EncryptMetadata true >>\n"
      "endobj\n",
      o_hex,
      u_hex,
      oe_hex,
      ue_hex,
      perms_hex,
      permissions);
  off6 = length;
  append_format(pdf, &length, "6 0 obj\n<< /Title <%s> >>\nendobj\n", title_hex);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 7\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_format(
      pdf,
      &length,
      "trailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R /Encrypt 5 0 R /ID [<%s> <%s>] >>\n",
      file_id_hex,
      file_id_hex);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encrypted_title);
  free(encrypted_stream);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_xref_stream_pdf(size_t* out_size) {
  const char* header = "%PDF-1.5\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 320 240] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 17 >>\n"
      "stream\n"
      "BT (XR) Tj ET\n"
      "endstream\n"
      "endobj\n";
  const char* obj5_prefix =
      "5 0 obj\n"
      "<< /Type /XRef /Size 6 /Root 1 0 R /W [1 4 2] /Length 42 >>\n"
      "stream\n";
  const char* obj5_suffix =
      "\nendstream\n"
      "endobj\n";
  unsigned char xref_data[42];
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  memset(xref_data, 0, sizeof(xref_data));

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  off5 = length;
  append_text(pdf, &length, obj5_prefix);

  xref_data[0] = 0;  xref_data[5] = 0xff; xref_data[6] = 0xff;
  xref_data[7] = 1;  xref_data[8] = (unsigned char)((off1 >> 24) & 0xff);
  xref_data[9] = (unsigned char)((off1 >> 16) & 0xff);
  xref_data[10] = (unsigned char)((off1 >> 8) & 0xff);
  xref_data[11] = (unsigned char)(off1 & 0xff);
  xref_data[14] = 1; xref_data[15] = (unsigned char)((off2 >> 24) & 0xff);
  xref_data[16] = (unsigned char)((off2 >> 16) & 0xff);
  xref_data[17] = (unsigned char)((off2 >> 8) & 0xff);
  xref_data[18] = (unsigned char)(off2 & 0xff);
  xref_data[21] = 1; xref_data[22] = (unsigned char)((off3 >> 24) & 0xff);
  xref_data[23] = (unsigned char)((off3 >> 16) & 0xff);
  xref_data[24] = (unsigned char)((off3 >> 8) & 0xff);
  xref_data[25] = (unsigned char)(off3 & 0xff);
  xref_data[28] = 1; xref_data[29] = (unsigned char)((off4 >> 24) & 0xff);
  xref_data[30] = (unsigned char)((off4 >> 16) & 0xff);
  xref_data[31] = (unsigned char)((off4 >> 8) & 0xff);
  xref_data[32] = (unsigned char)(off4 & 0xff);
  xref_data[35] = 1; xref_data[36] = (unsigned char)((off5 >> 24) & 0xff);
  xref_data[37] = (unsigned char)((off5 >> 16) & 0xff);
  xref_data[38] = (unsigned char)((off5 >> 8) & 0xff);
  xref_data[39] = (unsigned char)(off5 & 0xff);

  append_bytes(pdf, &length, xref_data, sizeof(xref_data));
  append_text(pdf, &length, obj5_suffix);
  xref_off = off5;
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_object_stream_pdf(size_t* out_size) {
  const char* header = "%PDF-1.5\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 17 >>\n"
      "stream\n"
      "BT (OS) Tj ET\n"
      "endstream\n"
      "endobj\n";
  const char* obj6_o1 = "<< /Type /Catalog /Pages 2 0 R >> ";
  const char* obj6_o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >> ";
  const char* obj6_o3 =
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R >> ";
  char obj6_header[128];
  char obj6_prefix[256];
  const char* obj6_suffix =
      "\nendstream\n"
      "endobj\n";
  const char* obj5_prefix =
      "5 0 obj\n"
      "<< /Type /XRef /Size 7 /Root 1 0 R /W [1 4 2] /Length 49 >>\n"
      "stream\n";
  const char* obj5_suffix =
      "\nendstream\n"
      "endobj\n";
  unsigned char xref_data[49];
  char* pdf = (char*)malloc(12288);
  size_t length = 0;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t obj6_o1_len = strlen(obj6_o1);
  size_t obj6_o2_len = strlen(obj6_o2);
  size_t obj6_o3_len = strlen(obj6_o3);
  size_t obj6_header_len = 0;
  size_t obj6_stream_len = 0;

  TEST_CHECK(pdf != NULL);
  memset(xref_data, 0, sizeof(xref_data));

  snprintf(
      obj6_header,
      sizeof(obj6_header),
      "1 0 2 %zu 3 %zu ",
      obj6_o1_len,
      obj6_o1_len + obj6_o2_len);
  obj6_header_len = strlen(obj6_header);
  obj6_stream_len = obj6_header_len + obj6_o1_len + obj6_o2_len + obj6_o3_len;
  snprintf(
      obj6_prefix,
      sizeof(obj6_prefix),
      "6 0 obj\n<< /Type /ObjStm /N 3 /First %zu /Length %zu >>\nstream\n",
      obj6_header_len,
      obj6_stream_len);

  append_text(pdf, &length, header);
  off4 = length;
  append_text(pdf, &length, obj4);
  off6 = length;
  append_text(pdf, &length, obj6_prefix);
  append_text(pdf, &length, obj6_header);
  append_text(pdf, &length, obj6_o1);
  append_text(pdf, &length, obj6_o2);
  append_text(pdf, &length, obj6_o3);
  append_text(pdf, &length, obj6_suffix);
  off5 = length;
  append_text(pdf, &length, obj5_prefix);

  xref_data[0] = 0;  xref_data[5] = 0xff; xref_data[6] = 0xff;
  xref_data[7] = 2;  xref_data[11] = 6;  /* obj 1 in object stream 6, index 0 */
  xref_data[14] = 2; xref_data[18] = 6;  xref_data[20] = 1; /* obj 2, index 1 */
  xref_data[21] = 2; xref_data[25] = 6;  xref_data[27] = 2; /* obj 3, index 2 */
  xref_data[28] = 1; xref_data[29] = (unsigned char)((off4 >> 24) & 0xff);
  xref_data[30] = (unsigned char)((off4 >> 16) & 0xff);
  xref_data[31] = (unsigned char)((off4 >> 8) & 0xff);
  xref_data[32] = (unsigned char)(off4 & 0xff);
  xref_data[35] = 1; xref_data[36] = (unsigned char)((off5 >> 24) & 0xff);
  xref_data[37] = (unsigned char)((off5 >> 16) & 0xff);
  xref_data[38] = (unsigned char)((off5 >> 8) & 0xff);
  xref_data[39] = (unsigned char)(off5 & 0xff);
  xref_data[42] = 1; xref_data[43] = (unsigned char)((off6 >> 24) & 0xff);
  xref_data[44] = (unsigned char)((off6 >> 16) & 0xff);
  xref_data[45] = (unsigned char)((off6 >> 8) & 0xff);
  xref_data[46] = (unsigned char)(off6 & 0xff);

  append_bytes(pdf, &length, xref_data, sizeof(xref_data));
  append_text(pdf, &length, obj5_suffix);
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", off5);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_prev_chain_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 250 150] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 19 >>\n"
      "stream\n"
      "BT (Prev OK) Tj ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref1_off;
  size_t xref2_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref1_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");

  xref2_off = length;
  append_text(pdf, &length, "xref\n4 1\n");
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R /Prev ");
  append_format(pdf, &length, "%zu", xref1_off);
  append_text(pdf, &length, " >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref2_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_flate_text_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 180] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (Flate OK) Tj ET\n";
  char obj4_prefix[128];
  const char* obj4_suffix =
      "\nendstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  unsigned char compressed[256];
  mz_ulong compressed_size = sizeof(compressed);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  TEST_CHECK(mz_compress(compressed, &compressed_size, (const unsigned char*)content,
                         (mz_ulong)strlen(content)) == MZ_OK);

  snprintf(
      obj4_prefix,
      sizeof(obj4_prefix),
      "4 0 obj\n<< /Length %lu /Filter /FlateDecode >>\nstream\n",
      (unsigned long)compressed_size);

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4_prefix);
  append_bytes(pdf, &length, compressed, (size_t)compressed_size);
  append_text(pdf, &length, obj4_suffix);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_ascii85_text_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 180] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (A85 OK) Tj ET\n";
  char* encoded = NULL;
  size_t encoded_size = 0;
  char obj4_prefix[128];
  const char* obj4_suffix =
      "\nendstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  encoded = encode_ascii85((const uint8_t*)content, strlen(content), &encoded_size);
  snprintf(
      obj4_prefix,
      sizeof(obj4_prefix),
      "4 0 obj\n<< /Length %zu /Filter /ASCII85Decode >>\nstream\n",
      encoded_size);

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4_prefix);
  append_bytes(pdf, &length, encoded, encoded_size);
  append_text(pdf, &length, obj4_suffix);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encoded);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_runlength_text_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 180] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (RL OK) Tj ET\n";
  uint8_t* encoded = NULL;
  size_t encoded_size = 0;
  char obj4_prefix[128];
  const char* obj4_suffix =
      "\nendstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  encoded = encode_runlength((const uint8_t*)content, strlen(content), &encoded_size);
  snprintf(
      obj4_prefix,
      sizeof(obj4_prefix),
      "4 0 obj\n<< /Length %zu /Filter /RunLengthDecode >>\nstream\n",
      encoded_size);

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4_prefix);
  append_bytes(pdf, &length, encoded, encoded_size);
  append_text(pdf, &length, obj4_suffix);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encoded);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_filter_chain_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 180] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (Chain OK) Tj ET\n";
  unsigned char compressed[256];
  mz_ulong compressed_size = sizeof(compressed);
  char* encoded = NULL;
  size_t encoded_size = 0;
  char obj4_prefix[160];
  const char* obj4_suffix =
      "\nendstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(12288);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  TEST_CHECK(mz_compress(compressed, &compressed_size, (const unsigned char*)content,
                         (mz_ulong)strlen(content)) == MZ_OK);
  encoded = encode_asciihex(compressed, (size_t)compressed_size, &encoded_size);
  snprintf(
      obj4_prefix,
      sizeof(obj4_prefix),
      "4 0 obj\n<< /Length %zu /Filter [/ASCIIHexDecode /FlateDecode] >>\nstream\n",
      encoded_size);

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4_prefix);
  append_bytes(pdf, &length, encoded, encoded_size);
  append_text(pdf, &length, obj4_suffix);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  free(encoded);
  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_indirect_length_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 180] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (Len Ref) Tj ET\n";
  char obj4_prefix[128];
  char obj5[64];
  const char* obj4_suffix =
      "\nendstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  snprintf(obj4_prefix, sizeof(obj4_prefix), "4 0 obj\n<< /Length 5 0 R >>\nstream\n");
  snprintf(obj5, sizeof(obj5), "5 0 obj\n%zu\nendobj\n", strlen(content));

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4_prefix);
  append_text(pdf, &length, content);
  append_text(pdf, &length, obj4_suffix);
  off5 = length;
  append_text(pdf, &length, obj5);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_text(pdf, &length, "trailer\n<< /Size 6 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_escaped_objects_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Ty#70e /Catalog /Pa#67es 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Ty#70e /Pa#67es /Ki#64s [3 0 R] /Cou#6et 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Ty#70e /Pa#67e /Par#65nt 2 0 R /Med#69aBox [0 0 180 180] /Cont#65nts 4 0 R >>\n"
      "endobj\n";
  const char* content = "BT (Octal\\040Text) Tj ET\n";
  char obj4[128];
  const char* obj5 =
      "5 0 obj\n"
      "<< /Ti#74le (Esc\\141ped\\040Title) >>\n"
      "endobj\n";
  char* pdf = (char*)malloc(8192);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  snprintf(
      obj4,
      sizeof(obj4),
      "4 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
      strlen(content),
      content);

  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  off5 = length;
  append_text(pdf, &length, obj5);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_text(pdf, &length, "trailer\n<< /Size 6 /Root 1 0 R /Info 5 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_indirect_values_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R /AcroForm 8 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox 5 0 R /Rotate 6 0 R /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 21 >>\n"
      "stream\n"
      "BT (Indirect OK) Tj ET\n"
      "endstream\n"
      "endobj\n";
  const char* obj5 =
      "5 0 obj\n"
      "[0 0 300 144]\n"
      "endobj\n";
  const char* obj6 =
      "6 0 obj\n"
      "90\n"
      "endobj\n";
  const char* obj7 =
      "7 0 obj\n"
      "<< /Title 9 0 R >>\n"
      "endobj\n";
  const char* obj8 =
      "8 0 obj\n"
      "<< /Fields 10 0 R >>\n"
      "endobj\n";
  const char* obj9 =
      "9 0 obj\n"
      "(Indirect Title)\n"
      "endobj\n";
  const char* obj10 =
      "10 0 obj\n"
      "[11 0 R]\n"
      "endobj\n";
  const char* obj11 =
      "11 0 obj\n"
      "<< /FT /Tx /T 12 0 R /V 13 0 R >>\n"
      "endobj\n";
  const char* obj12 =
      "12 0 obj\n"
      "(IndirectName)\n"
      "endobj\n";
  const char* obj13 =
      "13 0 obj\n"
      "(Indirect Value)\n"
      "endobj\n";
  char* pdf = (char*)malloc(12288);
  size_t length = 0;
  size_t off[14];
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off[1] = length; append_text(pdf, &length, obj1);
  off[2] = length; append_text(pdf, &length, obj2);
  off[3] = length; append_text(pdf, &length, obj3);
  off[4] = length; append_text(pdf, &length, obj4);
  off[5] = length; append_text(pdf, &length, obj5);
  off[6] = length; append_text(pdf, &length, obj6);
  off[7] = length; append_text(pdf, &length, obj7);
  off[8] = length; append_text(pdf, &length, obj8);
  off[9] = length; append_text(pdf, &length, obj9);
  off[10] = length; append_text(pdf, &length, obj10);
  off[11] = length; append_text(pdf, &length, obj11);
  off[12] = length; append_text(pdf, &length, obj12);
  off[13] = length; append_text(pdf, &length, obj13);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 14\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off[1]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[2]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[3]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[4]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[5]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[6]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[7]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[8]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[9]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[10]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[11]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[12]);
  append_format(pdf, &length, "%010zu 00000 n \n", off[13]);
  append_text(pdf, &length, "trailer\n<< /Size 14 /Root 1 0 R /Info 7 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_encrypted_marker_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Filter /Standard /V 1 /R 2 /O () /U () /P -4 >>\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R /Encrypt 4 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_hybrid_xref_pdf(size_t* out_size) {
  const char* header = "%PDF-1.5\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 17 >>\n"
      "stream\n"
      "BT (HY) Tj ET\n"
      "endstream\n"
      "endobj\n";
  const char* obj6_o1 = "<< /Type /Catalog /Pages 2 0 R >> ";
  const char* obj6_o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >> ";
  const char* obj6_o3 =
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 210 110] /Contents 4 0 R >> ";
  char obj6_header[128];
  char obj6_prefix[256];
  const char* obj6_suffix =
      "\nendstream\n"
      "endobj\n";
  char obj5_prefix[192];
  const char* obj5_suffix =
      "\nendstream\n"
      "endobj\n";
  unsigned char xref_data[49];
  char* pdf = (char*)malloc(12288);
  size_t length = 0;
  size_t off4;
  size_t off5;
  size_t off6;
  size_t xref_off;
  size_t obj6_o1_len = strlen(obj6_o1);
  size_t obj6_o2_len = strlen(obj6_o2);
  size_t obj6_o3_len = strlen(obj6_o3);
  size_t obj6_header_len = 0;
  size_t obj6_stream_len = 0;

  TEST_CHECK(pdf != NULL);
  memset(xref_data, 0, sizeof(xref_data));

  snprintf(
      obj6_header,
      sizeof(obj6_header),
      "1 0 2 %zu 3 %zu ",
      obj6_o1_len,
      obj6_o1_len + obj6_o2_len);
  obj6_header_len = strlen(obj6_header);
  obj6_stream_len = obj6_header_len + obj6_o1_len + obj6_o2_len + obj6_o3_len;
  snprintf(
      obj6_prefix,
      sizeof(obj6_prefix),
      "6 0 obj\n<< /Type /ObjStm /N 3 /First %zu /Length %zu >>\nstream\n",
      obj6_header_len,
      obj6_stream_len);

  append_text(pdf, &length, header);
  off4 = length;
  append_text(pdf, &length, obj4);
  off6 = length;
  append_text(pdf, &length, obj6_prefix);
  append_text(pdf, &length, obj6_header);
  append_text(pdf, &length, obj6_o1);
  append_text(pdf, &length, obj6_o2);
  append_text(pdf, &length, obj6_o3);
  append_text(pdf, &length, obj6_suffix);
  off5 = length;
  snprintf(
      obj5_prefix,
      sizeof(obj5_prefix),
      "5 0 obj\n<< /Type /XRef /Size 7 /W [1 4 2] /Length 49 /Root 1 0 R >>\nstream\n");
  append_text(pdf, &length, obj5_prefix);

  xref_data[0] = 0;  xref_data[5] = 0xff; xref_data[6] = 0xff;
  xref_data[7] = 2;  xref_data[11] = 6;
  xref_data[14] = 2; xref_data[18] = 6;  xref_data[20] = 1;
  xref_data[21] = 2; xref_data[25] = 6;  xref_data[27] = 2;
  xref_data[28] = 1; xref_data[29] = (unsigned char)((off4 >> 24) & 0xff);
  xref_data[30] = (unsigned char)((off4 >> 16) & 0xff);
  xref_data[31] = (unsigned char)((off4 >> 8) & 0xff);
  xref_data[32] = (unsigned char)(off4 & 0xff);
  xref_data[35] = 1; xref_data[36] = (unsigned char)((off5 >> 24) & 0xff);
  xref_data[37] = (unsigned char)((off5 >> 16) & 0xff);
  xref_data[38] = (unsigned char)((off5 >> 8) & 0xff);
  xref_data[39] = (unsigned char)(off5 & 0xff);
  xref_data[42] = 1; xref_data[43] = (unsigned char)((off6 >> 24) & 0xff);
  xref_data[44] = (unsigned char)((off6 >> 16) & 0xff);
  xref_data[45] = (unsigned char)((off6 >> 8) & 0xff);
  xref_data[46] = (unsigned char)(off6 & 0xff);

  append_bytes(pdf, &length, xref_data, sizeof(xref_data));
  append_text(pdf, &length, obj5_suffix);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 7\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_text(pdf, &length, "0000000000 00000 f \n");
  append_text(pdf, &length, "0000000000 00000 f \n");
  append_text(pdf, &length, "0000000000 00000 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_format(pdf, &length, "%010zu 00000 n \n", off6);
  append_text(pdf, &length, "trailer\n<< /Size 7 /Root 1 0 R /XRefStm ");
  append_format(pdf, &length, "%zu", off5);
  append_text(pdf, &length, " >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_broken_startxref_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 160 90] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 18 >>\n"
      "stream\n"
      "BT (Repair) Tj ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off + 17);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_bad_stream_length_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 120] /Contents 4 0 R >>\n"
      "endobj\n";
  const char* obj4 =
      "4 0 obj\n"
      "<< /Length 999 >>\n"
      "stream\n"
      "BT (Recovered) Tj ET\n"
      "endstream\n"
      "endobj\n";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_text(pdf, &length, obj4);
  xref_off = length;

  append_text(pdf, &length, "xref\n0 5\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_text(pdf, &length, "trailer\n<< /Size 5 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_jbig2_globals_decode_error_pdf(
    size_t* out_size,
    int use_filter_array) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] >>\n"
      "endobj\n";
  const uint8_t image_payload_raw[] = {0x00};
  const char* image_payload_hex = "00>";
  const uint8_t* image_payload = use_filter_array ? (const uint8_t*)image_payload_hex
                                                  : image_payload_raw;
  size_t image_payload_size = use_filter_array ? strlen(image_payload_hex)
                                               : sizeof(image_payload_raw);
  const char* globals_payload = "ZG";
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  if (use_filter_array) {
    append_format(
        pdf,
        &length,
        "4 0 obj\n"
        "<< /Length %zu /Filter [/ASCIIHexDecode /JBIG2Decode] "
        "/DecodeParms [null << /JBIG2Globals 5 0 R >>] >>\n"
        "stream\n",
        image_payload_size);
  } else {
    append_format(
        pdf,
        &length,
        "4 0 obj\n"
        "<< /Length %zu /Filter /JBIG2Decode /DecodeParms << /JBIG2Globals 5 0 R >> >>\n"
        "stream\n",
        image_payload_size);
  }
  append_bytes(pdf, &length, image_payload, image_payload_size);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n<< /Length %zu /Filter /ASCIIHexDecode >>\nstream\n",
      strlen(globals_payload));
  append_text(pdf, &length, globals_payload);
  append_text(pdf, &length, "\nendstream\nendobj\n");
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_text(pdf, &length, "trailer\n<< /Size 6 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static uint8_t* build_jbig2_globals_cycle_pdf(size_t* out_size) {
  const char* header = "%PDF-1.4\n";
  const char* obj1 =
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n";
  const char* obj2 =
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n";
  const char* obj3 =
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] >>\n"
      "endobj\n";
  const uint8_t image_payload[] = {0x00};
  char* pdf = (char*)malloc(4096);
  size_t length = 0;
  size_t off1;
  size_t off2;
  size_t off3;
  size_t off4;
  size_t off5;
  size_t xref_off;

  TEST_CHECK(pdf != NULL);
  append_text(pdf, &length, header);
  off1 = length;
  append_text(pdf, &length, obj1);
  off2 = length;
  append_text(pdf, &length, obj2);
  off3 = length;
  append_text(pdf, &length, obj3);
  off4 = length;
  append_format(
      pdf,
      &length,
      "4 0 obj\n"
      "<< /Length %zu /Filter /JBIG2Decode /DecodeParms << /JBIG2Globals 5 0 R >> >>\n"
      "stream\n",
      sizeof(image_payload));
  append_bytes(pdf, &length, image_payload, sizeof(image_payload));
  append_text(pdf, &length, "\nendstream\nendobj\n");
  off5 = length;
  append_format(
      pdf,
      &length,
      "5 0 obj\n"
      "<< /Length %zu /Filter /JBIG2Decode /DecodeParms << /JBIG2Globals 5 0 R >> >>\n"
      "stream\n",
      sizeof(image_payload));
  append_bytes(pdf, &length, image_payload, sizeof(image_payload));
  append_text(pdf, &length, "\nendstream\nendobj\n");
  xref_off = length;

  append_text(pdf, &length, "xref\n0 6\n");
  append_text(pdf, &length, "0000000000 65535 f \n");
  append_format(pdf, &length, "%010zu 00000 n \n", off1);
  append_format(pdf, &length, "%010zu 00000 n \n", off2);
  append_format(pdf, &length, "%010zu 00000 n \n", off3);
  append_format(pdf, &length, "%010zu 00000 n \n", off4);
  append_format(pdf, &length, "%010zu 00000 n \n", off5);
  append_text(pdf, &length, "trailer\n<< /Size 6 /Root 1 0 R >>\n");
  append_text(pdf, &length, "startxref\n");
  append_format(pdf, &length, "%zu\n", xref_off);
  append_text(pdf, &length, "%%EOF\n");

  *out_size = length;
  return (uint8_t*)pdf;
}

static void test_nanopdf_c_smoke(void) {
  CountingAllocator allocator = {0, 0, 0};
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  nanopdf_page_info page_info;
  nanopdf_text_layout* layout = NULL;
  nanopdf_text_char text_char;
  nanopdf_form_field_info field_info;
  char* text = NULL;
  char* metadata = NULL;
  char* layout_text = NULL;
  char* rect_text = NULL;
  size_t pdf_size = 0;
  uint8_t* pdf = build_minimal_one_page_pdf(&pdf_size);

  nanopdf_default_context_options(&context_options);
  context_options.allocator.user_data = &allocator;
  context_options.allocator.alloc = counting_alloc;
  context_options.allocator.realloc = counting_realloc;
  context_options.allocator.free = counting_free;
  nanopdf_default_parse_options(&parse_options);

  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(context != NULL);
  TEST_CHECK(nanopdf_document_open_memory(
                 context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_document_page_count(document) == 1u);

  TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(page_info.page_index == 0u);
  TEST_CHECK(fabs(page_info.width - 612.0) < 0.001);
  TEST_CHECK(fabs(page_info.height - 792.0) < 0.001);

  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Hello C") == 0);
  nanopdf_free(context, text);

  TEST_CHECK(nanopdf_page_extract_text_layout(document, 0, NULL, &layout) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(layout != NULL);
  TEST_CHECK(nanopdf_text_layout_char_count(layout) == 6u);
  TEST_CHECK(nanopdf_text_layout_line_count(layout) == 1u);
  TEST_CHECK(nanopdf_text_layout_word_count(layout) == 2u);
  TEST_CHECK(nanopdf_text_layout_get_char(layout, 0, &text_char) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(text_char.unicode == 'H');
  TEST_CHECK(text_char.line_index == 0);
  TEST_CHECK(text_char.word_index == 0);
  TEST_CHECK(fabs(text_char.width - 8.0) < 0.001);
  TEST_CHECK(fabs(text_char.height - 12.0) < 0.001);
  TEST_CHECK(nanopdf_text_layout_copy_text(layout, &layout_text) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(layout_text != NULL);
  TEST_CHECK(strcmp(layout_text, "Hello C") == 0);
  TEST_CHECK(nanopdf_text_layout_copy_text_in_rect(
                 layout, 0.0, 780.0, 39.0, 792.0, &rect_text) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(rect_text != NULL);
  TEST_CHECK(strcmp(rect_text, "Hello") == 0);
  nanopdf_free(context, rect_text);
  nanopdf_free(context, layout_text);
  nanopdf_text_layout_destroy(layout);

  TEST_CHECK(nanopdf_document_form_field_count(document) == 0u);
  TEST_CHECK(nanopdf_document_get_form_field_info(document, 0, &field_info) ==
             NANOPDF_STATUS_INVALID_ARGUMENT);

  TEST_CHECK(nanopdf_document_copy_info_value(
                 document, NANOPDF_INFO_TITLE, &metadata) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(metadata != NULL);
  TEST_CHECK(strcmp(metadata, "") == 0);
  nanopdf_free(context, metadata);

  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);

  TEST_CHECK(allocator.alloc_count > 0u);
  TEST_CHECK(allocator.free_count > 0u);
}

static void test_nanopdf_c_native_content_variants(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_multi_stream_text_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);

  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Hi  there C") == 0);
  nanopdf_free(context, text);

  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_utf16be_text(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_utf16be_text_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);

  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Hi \xe6\x97\xa5\xe6\x9c\xac") == 0);
  nanopdf_free(context, text);

  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_ignores_non_text_strings(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_non_text_strings_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);

  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Visible") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_utf16be_info(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_utf16be_info_pdf(&pdf_size);
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "Meta \xe6\x97\xa5\xe6\x9c\xac") == 0);

  nanopdf_free(context, title);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_pdfdocencoding_info(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_pdfdocencoding_info_pdf(&pdf_size);
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "Dash \xe2\x80\x94 Bullet \xe2\x80\xa2 Euro \xe2\x82\xac") == 0);

  nanopdf_free(context, title);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_native_forms(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_form_field_info field_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_basic_form_pdf(&pdf_size);
  char* value = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);

  TEST_CHECK(nanopdf_document_form_field_count(document) == 3u);
  TEST_CHECK(nanopdf_document_get_form_field_info(document, 0, &field_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(field_info.type == NANOPDF_FIELD_TYPE_TEXT);
  TEST_CHECK(strcmp(field_info.partial_name, "Name") == 0);
  TEST_CHECK(strcmp(field_info.full_name, "Name") == 0);
  TEST_CHECK(strcmp(field_info.alternate_name, "Display Name") == 0);
  TEST_CHECK(strcmp(field_info.mapping_name, "name_export") == 0);
  TEST_CHECK(field_info.flags == 1u);
  TEST_CHECK(nanopdf_document_copy_form_field_value(document, 0, &value) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(value != NULL);
  TEST_CHECK(strcmp(value, "Alice") == 0);
  nanopdf_free(context, value);
  value = NULL;

  TEST_CHECK(nanopdf_document_set_text_field(document, "Name", "Bob") ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_copy_form_field_value(document, 0, &value) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(value, "Bob") == 0);
  nanopdf_free(context, value);
  value = NULL;

  TEST_CHECK(nanopdf_document_get_form_field_info(document, 1, &field_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(field_info.type == NANOPDF_FIELD_TYPE_BUTTON);
  TEST_CHECK(strcmp(field_info.partial_name, "Accept") == 0);
  TEST_CHECK(nanopdf_document_set_button_field(document, "Accept", 1) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_copy_form_field_value(document, 1, &value) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(value, "Yes") == 0);
  nanopdf_free(context, value);
  value = NULL;

  TEST_CHECK(nanopdf_document_get_form_field_info(document, 2, &field_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(field_info.type == NANOPDF_FIELD_TYPE_CHOICE);
  TEST_CHECK(strcmp(field_info.partial_name, "Mode") == 0);
  TEST_CHECK(nanopdf_document_set_choice_field_indices(document, "Mode", (const int32_t[]){2, 4}, 2) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_copy_form_field_value(document, 2, &value) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(value, "2,4") == 0);

  nanopdf_free(context, value);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_xref_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_page_info page_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_xref_stream_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_page_count(document) == 1u);
  TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(fabs(page_info.width - 320.0) < 0.001);
  TEST_CHECK(fabs(page_info.height - 240.0) < 0.001);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "XR") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_object_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_page_info page_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_object_stream_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_page_count(document) == 1u);
  TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(fabs(page_info.width - 200.0) < 0.001);
  TEST_CHECK(fabs(page_info.height - 100.0) < 0.001);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "OS") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_prev_chain(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_page_info page_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_prev_chain_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_page_count(document) == 1u);
  TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(fabs(page_info.width - 250.0) < 0.001);
  TEST_CHECK(fabs(page_info.height - 150.0) < 0.001);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Prev OK") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_flate_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_flate_text_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Flate OK") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_ascii85_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_ascii85_text_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "A85 OK") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_runlength_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_runlength_text_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "RL OK") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_filter_chain(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_filter_chain_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Chain OK") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_dct_stream(void) {
  nanopdf_context* context = NULL;
  nanopdf_context_options context_options;
  uint8_t* decoded = NULL;
  size_t decoded_size = 0;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(decode_test_stream(
                 context,
                 "DCTDecode",
                 k_jpeg_1x1_red,
                 sizeof(k_jpeg_1x1_red),
                 &decoded,
                 &decoded_size) == NANOPDF_STATUS_OK);
  TEST_CHECK(decoded != NULL);
  TEST_CHECK(decoded_size == 3);
  if (decoded && decoded_size == 3) {
    TEST_CHECK(decoded[0] > 200);
    TEST_CHECK(decoded[1] < 80);
    TEST_CHECK(decoded[2] < 80);
  }

  nanopdf_free(context, decoded);
  nanopdf_context_destroy(context);
}

static void test_nanopdf_c_ccitt_stream(void) {
  struct Case {
    int k;
    int black_is_1;
    size_t (*build)(uint8_t out[64]);
    uint8_t expected0;
    uint8_t expected1;
  };
  const struct Case cases[] = {
      {0, 0, build_ccitt_1d_sample, 0xff, 0x00},
      {0, 1, build_ccitt_1d_sample, 0x00, 0xff},
      {1, 0, build_ccitt_2d_sample, 0xff, 0x00},
      {-1, 0, build_ccitt_group4_sample, 0xff, 0x00},
  };
  size_t i = 0;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    nanopdf_context* context = NULL;
    nanopdf_context_options context_options;
    uint8_t encoded[64];
    char decode_parms[128];
    uint8_t* decoded = NULL;
    size_t encoded_size = cases[i].build(encoded);
    size_t decoded_size = 0;

    snprintf(
        decode_parms,
        sizeof(decode_parms),
        "/K %d /Columns 16 /Rows 2 /EndOfLine true /BlackIs1 %s",
        cases[i].k,
        cases[i].black_is_1 ? "true" : "false");

    nanopdf_default_context_options(&context_options);
    TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
    TEST_CHECK(decode_test_stream_with_decode_parms(
                   context,
                   "CCITTFaxDecode",
                   decode_parms,
                   encoded,
                   encoded_size,
                   &decoded,
                   &decoded_size) == NANOPDF_STATUS_OK);
    TEST_CHECK(decoded != NULL);
    TEST_CHECK(decoded_size == 4);
    if (decoded && decoded_size == 4) {
      TEST_CHECK(decoded[0] == cases[i].expected0);
      TEST_CHECK(decoded[1] == cases[i].expected1);
      TEST_CHECK(decoded[2] == cases[i].expected0);
      TEST_CHECK(decoded[3] == cases[i].expected1);
    }

    nanopdf_free(context, decoded);
    nanopdf_context_destroy(context);
  }
}

static void test_nanopdf_c_jpx_jbig2_invalid_payloads(void) {
  static const uint8_t payload[1] = {0};
  const char* filters[] = {"JPXDecode", "JBIG2Decode"};
  size_t i = 0;

  for (i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i) {
    nanopdf_context* context = NULL;
    nanopdf_context_options context_options;
    uint8_t* decoded = NULL;
    size_t decoded_size = 0;
    nanopdf_status status = NANOPDF_STATUS_OK;

    nanopdf_default_context_options(&context_options);
    TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
    status = decode_test_stream(
        context, filters[i], payload, sizeof(payload), &decoded, &decoded_size);
    TEST_CHECK(status != NANOPDF_STATUS_OK);
    TEST_CHECK(decoded == NULL);
    TEST_CHECK(decoded_size == 0);
    TEST_CHECK(nanopdf_context_last_status(context) == status);

    nanopdf_context_destroy(context);
  }
}

static void test_nanopdf_c_jbig2_globals_are_resolved(void) {
  int use_filter_array = 0;

  for (use_filter_array = 0; use_filter_array < 2; ++use_filter_array) {
    nanopdf_context* context = NULL;
    nanopdf_context_options context_options;
    nanopdf_parse_options parse_options;
    nanopdf_basic_document document;
    nanopdf_basic_object image_stream;
    uint8_t* decoded = NULL;
    size_t decoded_size = 0;
    size_t pdf_size = 0;
    uint8_t* pdf = build_jbig2_globals_decode_error_pdf(&pdf_size, use_filter_array);
    nanopdf_status status = NANOPDF_STATUS_OK;
    const char* error = NULL;

    nanopdf_default_context_options(&context_options);
    nanopdf_default_parse_options(&parse_options);
    nanopdf_basic_document_init(&document);
    nanopdf_basic_object_init(&image_stream);

    TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
    TEST_CHECK(nanopdf_basic_document_parse(context, pdf, pdf_size, &parse_options, &document) ==
               NANOPDF_STATUS_OK);
    TEST_CHECK(nanopdf_basic_load_object(
                   context,
                   &document,
                   (nanopdf_basic_ref){4u, 0u, 1u},
                   &image_stream) == NANOPDF_STATUS_OK);

    status = nanopdf_basic_decode_stream_with_document(
        context, &document, &image_stream, &decoded, &decoded_size);
    TEST_CHECK(status == NANOPDF_STATUS_MALFORMED);
    TEST_CHECK(decoded == NULL);
    TEST_CHECK(decoded_size == 0);
    error = nanopdf_context_last_error(context);
    TEST_CHECK(error != NULL);
    TEST_CHECK(strstr(error, "ASCIIHexDecode") != NULL);

    nanopdf_free(context, decoded);
    nanopdf_basic_object_destroy(nanopdf_context_get_allocator(context), &image_stream);
    nanopdf_basic_document_destroy(nanopdf_context_get_allocator(context), &document);
    nanopdf_context_destroy(context);
    free(pdf);
  }
}

static void test_nanopdf_c_jbig2_globals_cycle_is_bounded(void) {
  nanopdf_context* context = NULL;
  nanopdf_context_options context_options;
  nanopdf_parse_options parse_options;
  nanopdf_basic_document document;
  nanopdf_basic_object image_stream;
  uint8_t* decoded = NULL;
  size_t decoded_size = 0;
  size_t pdf_size = 0;
  uint8_t* pdf = build_jbig2_globals_cycle_pdf(&pdf_size);
  nanopdf_status status = NANOPDF_STATUS_OK;
  const char* error = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  nanopdf_basic_document_init(&document);
  nanopdf_basic_object_init(&image_stream);

  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_basic_document_parse(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_basic_load_object(
                 context,
                 &document,
                 (nanopdf_basic_ref){4u, 0u, 1u},
                 &image_stream) == NANOPDF_STATUS_OK);

  status = nanopdf_basic_decode_stream_with_document(
      context, &document, &image_stream, &decoded, &decoded_size);
  TEST_CHECK(status == NANOPDF_STATUS_MALFORMED);
  TEST_CHECK(decoded == NULL);
  TEST_CHECK(decoded_size == 0);
  error = nanopdf_context_last_error(context);
  TEST_CHECK(error != NULL);
  TEST_CHECK(strstr(error, "recursion limit") != NULL);

  nanopdf_free(context, decoded);
  nanopdf_basic_object_destroy(nanopdf_context_get_allocator(context), &image_stream);
  nanopdf_basic_document_destroy(nanopdf_context_get_allocator(context), &document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_crypto_vectors(void) {
  static const uint8_t md5_expected[16] = {
      0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
      0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
  static const uint8_t sha256_expected[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  static const uint8_t sha384_expected[48] = {
      0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b,
      0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07,
      0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63,
      0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed,
      0x80, 0x86, 0x07, 0x2b, 0xa1, 0xe7, 0xcc, 0x23,
      0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7};
  static const uint8_t sha512_expected[64] = {
      0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
      0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
      0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2,
      0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
      0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8,
      0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
      0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e,
      0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f};
  static const uint8_t rc4_key[3] = {'K', 'e', 'y'};
  static const uint8_t rc4_expected[9] = {
      0xbb, 0xf3, 0x16, 0xe8, 0xd9, 0x40, 0xaf, 0x0a, 0xd3};
  static const uint8_t aes128_key[16] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  static const uint8_t aes_plain[16] = {
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  static const uint8_t aes128_expected[16] = {
      0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
      0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  static const uint8_t aes256_key[32] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
  static const uint8_t aes256_expected[16] = {
      0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
      0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
  static const uint8_t zero_iv[16] = {0};
  static const uint8_t padded[16] = {
      'h', 'e', 'l', 'l', 'o', 0x0b, 0x0b, 0x0b,
      0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
  uint8_t digest[64];
  uint8_t data[16];
  uint8_t encrypted[16];
  uint8_t decrypted[16];
  size_t unpadded_size = 0;
  nanopdf_rc4 rc4;
  nanopdf_aes128 aes128;
  nanopdf_aes256 aes256;

  nanopdf_md5_hash((const uint8_t*)"abc", 3, digest);
  TEST_CHECK(bytes_equal(digest, md5_expected, sizeof(md5_expected)));
  nanopdf_sha256_hash((const uint8_t*)"abc", 3, digest);
  TEST_CHECK(bytes_equal(digest, sha256_expected, sizeof(sha256_expected)));
  nanopdf_sha384_hash((const uint8_t*)"abc", 3, digest);
  TEST_CHECK(bytes_equal(digest, sha384_expected, sizeof(sha384_expected)));
  nanopdf_sha512_hash((const uint8_t*)"abc", 3, digest);
  TEST_CHECK(bytes_equal(digest, sha512_expected, sizeof(sha512_expected)));

  memcpy(data, "Plaintext", 9);
  nanopdf_rc4_init(&rc4, rc4_key, sizeof(rc4_key));
  nanopdf_rc4_crypt(&rc4, data, 9);
  TEST_CHECK(bytes_equal(data, rc4_expected, sizeof(rc4_expected)));

  nanopdf_aes128_set_key(&aes128, aes128_key);
  nanopdf_aes128_encrypt_cbc(&aes128, aes_plain, encrypted, 16, zero_iv);
  TEST_CHECK(bytes_equal(encrypted, aes128_expected, sizeof(aes128_expected)));
  nanopdf_aes128_decrypt_cbc(&aes128, encrypted, decrypted, 16, zero_iv);
  TEST_CHECK(bytes_equal(decrypted, aes_plain, sizeof(aes_plain)));

  nanopdf_aes256_set_key(&aes256, aes256_key);
  nanopdf_aes256_encrypt_block(&aes256, aes_plain, encrypted);
  TEST_CHECK(bytes_equal(encrypted, aes256_expected, sizeof(aes256_expected)));

  TEST_CHECK(nanopdf_pkcs7_unpad(padded, sizeof(padded), 16, &unpadded_size) == 1);
  TEST_CHECK(unpadded_size == 5);
}

static void test_nanopdf_c_indirect_length(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_indirect_length_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Len Ref") == 0);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_escaped_objects(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_escaped_objects_pdf(&pdf_size);
  char* text = NULL;
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_page_count(document) == 1u);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strcmp(text, "Octal Text") == 0);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "Escaped Title") == 0);

  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_indirect_values(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_page_info page_info;
  nanopdf_form_field_info field_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_indirect_values_pdf(&pdf_size);
  char* text = NULL;
  char* title = NULL;
  char* value = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) == NANOPDF_STATUS_OK);
  TEST_CHECK(fabs(page_info.width - 300.0) < 0.001);
  TEST_CHECK(fabs(page_info.height - 144.0) < 0.001);
  TEST_CHECK(fabs(page_info.rotation - 90.0) < 0.001);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(text, "Indirect OK") == 0);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(title, "Indirect Title") == 0);
  TEST_CHECK(nanopdf_document_form_field_count(document) == 1u);
  TEST_CHECK(nanopdf_document_get_form_field_info(document, 0, &field_info) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(field_info.partial_name, "IndirectName") == 0);
  TEST_CHECK(nanopdf_document_copy_form_field_value(document, 0, &value) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(strcmp(value, "Indirect Value") == 0);

  nanopdf_free(context, value);
  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_detection(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_marker_pdf(&pdf_size);

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_ENCRYPTED);
  TEST_CHECK(document == NULL);
  TEST_CHECK(nanopdf_context_last_status(context) == NANOPDF_STATUS_ENCRYPTED);

  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_text(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_text_pdf("", "", "BT (Secret C) Tj ET\n", &pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(context != NULL);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "Secret C") != NULL);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_user_password(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_text_pdf("user", "owner", "BT (User Secret) Tj ET\n", &pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "user";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "User Secret") != NULL);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_owner_password(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_text_pdf("user", "owner", "BT (Owner Secret) Tj ET\n", &pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "owner";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "Owner Secret") != NULL);

  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_v4_crypt_filter(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_v4_crypt_filter_pdf(&pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "user4";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "V4 Secret") != NULL);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "V4 Title") == 0);

  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_v4_aesv2(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_v4_aesv2_pdf(&pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "aesuser";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "AES Secret") != NULL);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "AES Title") == 0);

  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_v5_aes256(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_v5_aes256_pdf(&pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "aes256user";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "AES256 Secret") != NULL);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "AES256 Title") == 0);

  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_encrypted_v6_aes256(void) {
  size_t pdf_size = 0;
  uint8_t* pdf = build_encrypted_v6_aes256_pdf(&pdf_size);
  nanopdf_context_options context_options;
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_parse_options parse_options;
  char* text = NULL;
  char* title = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  parse_options.password = "aes256r6user";
  TEST_CHECK(pdf != NULL);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(document != NULL);
  TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
  TEST_CHECK(text != NULL);
  TEST_CHECK(strstr(text, "AES256 R6 Secret") != NULL);
  TEST_CHECK(nanopdf_document_copy_info_value(document, NANOPDF_INFO_TITLE, &title) ==
             NANOPDF_STATUS_OK);
  TEST_CHECK(title != NULL);
  TEST_CHECK(strcmp(title, "AES256 R6 Title") == 0);

  nanopdf_free(context, title);
  nanopdf_free(context, text);
  nanopdf_document_close(document);
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_hybrid_xref(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_page_info page_info;
  size_t pdf_size = 0;
  uint8_t* pdf = build_hybrid_xref_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) ==
             NANOPDF_STATUS_OK);
  if (document) {
    TEST_CHECK(nanopdf_document_page_count(document) == 1u);
    TEST_CHECK(nanopdf_document_get_page_info(document, 0, &page_info) ==
               NANOPDF_STATUS_OK);
    TEST_CHECK(fabs(page_info.width - 210.0) < 0.001);
    TEST_CHECK(fabs(page_info.height - 110.0) < 0.001);
    TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
    TEST_CHECK(strcmp(text, "HY") == 0);
  }

  nanopdf_free(context, text);
  if (document) {
    nanopdf_document_close(document);
  }
  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_auto_repair(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_parse_options parse_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_broken_startxref_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) !=
             NANOPDF_STATUS_OK);
  TEST_CHECK(document == NULL);

  parse_options.auto_repair = 1;
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  if (document) {
    TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
    TEST_CHECK(strcmp(text, "Repair") == 0);
    nanopdf_free(context, text);
    nanopdf_document_close(document);
  }

  nanopdf_context_destroy(context);
  free(pdf);
}

static void test_nanopdf_c_recover_stream_length(void) {
  nanopdf_context* context = NULL;
  nanopdf_document* document = NULL;
  nanopdf_context_options context_options;
  nanopdf_parse_options parse_options;
  size_t pdf_size = 0;
  uint8_t* pdf = build_bad_stream_length_pdf(&pdf_size);
  char* text = NULL;

  nanopdf_default_context_options(&context_options);
  nanopdf_default_parse_options(&parse_options);
  TEST_CHECK(nanopdf_context_create(&context_options, &context) == NANOPDF_STATUS_OK);
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, NULL, &document) !=
             NANOPDF_STATUS_OK);
  TEST_CHECK(document == NULL);

  parse_options.recover_stream_length = 1;
  TEST_CHECK(nanopdf_document_open_memory(context, pdf, pdf_size, &parse_options, &document) ==
             NANOPDF_STATUS_OK);
  if (document) {
    TEST_CHECK(nanopdf_page_extract_text(document, 0, &text) == NANOPDF_STATUS_OK);
    TEST_CHECK(strcmp(text, "Recovered") == 0);
    nanopdf_free(context, text);
    nanopdf_document_close(document);
  }

  nanopdf_context_destroy(context);
  free(pdf);
}

struct TEST_ENTRY TEST_LIST[] = {
    {"nanopdf_c_smoke", test_nanopdf_c_smoke},
    {"nanopdf_c_native_content_variants", test_nanopdf_c_native_content_variants},
    {"nanopdf_c_utf16be_text", test_nanopdf_c_utf16be_text},
    {"nanopdf_c_ignores_non_text_strings", test_nanopdf_c_ignores_non_text_strings},
    {"nanopdf_c_utf16be_info", test_nanopdf_c_utf16be_info},
    {"nanopdf_c_pdfdocencoding_info", test_nanopdf_c_pdfdocencoding_info},
    {"nanopdf_c_native_forms", test_nanopdf_c_native_forms},
    {"nanopdf_c_xref_stream", test_nanopdf_c_xref_stream},
    {"nanopdf_c_object_stream", test_nanopdf_c_object_stream},
    {"nanopdf_c_prev_chain", test_nanopdf_c_prev_chain},
    {"nanopdf_c_flate_stream", test_nanopdf_c_flate_stream},
    {"nanopdf_c_ascii85_stream", test_nanopdf_c_ascii85_stream},
    {"nanopdf_c_runlength_stream", test_nanopdf_c_runlength_stream},
    {"nanopdf_c_filter_chain", test_nanopdf_c_filter_chain},
    {"nanopdf_c_dct_stream", test_nanopdf_c_dct_stream},
    {"nanopdf_c_ccitt_stream", test_nanopdf_c_ccitt_stream},
    {"nanopdf_c_jpx_jbig2_invalid_payloads", test_nanopdf_c_jpx_jbig2_invalid_payloads},
    {"nanopdf_c_jbig2_globals_are_resolved", test_nanopdf_c_jbig2_globals_are_resolved},
    {"nanopdf_c_jbig2_globals_cycle_is_bounded", test_nanopdf_c_jbig2_globals_cycle_is_bounded},
    {"nanopdf_c_crypto_vectors", test_nanopdf_c_crypto_vectors},
    {"nanopdf_c_indirect_length", test_nanopdf_c_indirect_length},
    {"nanopdf_c_escaped_objects", test_nanopdf_c_escaped_objects},
    {"nanopdf_c_indirect_values", test_nanopdf_c_indirect_values},
    {"nanopdf_c_encrypted_detection", test_nanopdf_c_encrypted_detection},
    {"nanopdf_c_encrypted_text", test_nanopdf_c_encrypted_text},
    {"nanopdf_c_encrypted_user_password", test_nanopdf_c_encrypted_user_password},
    {"nanopdf_c_encrypted_owner_password", test_nanopdf_c_encrypted_owner_password},
    {"nanopdf_c_encrypted_v4_crypt_filter", test_nanopdf_c_encrypted_v4_crypt_filter},
    {"nanopdf_c_encrypted_v4_aesv2", test_nanopdf_c_encrypted_v4_aesv2},
    {"nanopdf_c_encrypted_v5_aes256", test_nanopdf_c_encrypted_v5_aes256},
    {"nanopdf_c_encrypted_v6_aes256", test_nanopdf_c_encrypted_v6_aes256},
    {"nanopdf_c_hybrid_xref", test_nanopdf_c_hybrid_xref},
    {"nanopdf_c_auto_repair", test_nanopdf_c_auto_repair},
    {"nanopdf_c_recover_stream_length", test_nanopdf_c_recover_stream_length},
    {NULL, NULL},
};
