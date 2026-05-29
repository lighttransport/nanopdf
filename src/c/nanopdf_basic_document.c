#include "nanopdf_basic_document.h"
#include "nanopdf_crypto.h"
#include "nanopdf_object.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define NANOPDF_MAYBE_UNUSED __attribute__((unused))
#else
#define NANOPDF_MAYBE_UNUSED
#endif

typedef struct nanopdf_ref {
  uint32_t object_number;
  uint16_t generation;
  uint8_t valid;
} nanopdf_ref;

typedef struct nanopdf_box {
  double values[4];
  uint8_t valid;
} nanopdf_box;

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
    const unsigned char ch = (const unsigned char)data[*pos];
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
  const size_t length = strlen(literal);
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
    const char ch = data[cursor];
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

static int find_startxref(
    const char* data,
    size_t size,
    size_t* out_offset) {
  size_t pos;

  if (size < 9) {
    return 0;
  }

  pos = size - 9;
  while (1) {
    if (match_literal(data, size, pos, "startxref")) {
      size_t cursor = pos + 9;
      uint32_t value = 0;
      skip_ws(data, size, &cursor);
      if (!parse_unsigned_value(data, size, &cursor, &value)) {
        return 0;
      }
      *out_offset = (size_t)value;
      return 1;
    }
    if (pos == 0) {
      break;
    }
    pos--;
  }

  return 0;
}

static int find_matching_dict_end(
    const char* data,
    size_t size,
    size_t pos,
    size_t* out_end);
static int hex_value(char ch);
static int find_top_level_key_value(
    const char* object,
    size_t object_length,
    const char* key,
    size_t* out_value_start,
    size_t* out_value_end);
static int parse_ref_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_ref* out_ref);
static int basic_object_to_ref(
    const nanopdf_basic_object* object,
    nanopdf_ref* out_ref);
static nanopdf_status ensure_xref_capacity(
    nanopdf_context* context,
    nanopdf_basic_xref_entry** entries,
    size_t* capacity,
    size_t required);

static const uint8_t k_pdf_padding[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
    0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
    0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

static void security_pad_password(const char* password, uint8_t out[32]) {
  size_t len = password ? strlen(password) : 0;
  if (len > 32) {
    len = 32;
  }
  memset(out, 0, 32);
  if (len > 0) {
    memcpy(out, password, len);
  }
  if (len < 32) {
    memcpy(out + len, k_pdf_padding, 32 - len);
  }
}

static void security_compute_file_key(
    const char* password,
    const char* owner_key,
    int32_t permissions,
    const char* file_id,
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t* out_key,
    uint8_t* out_key_length) {
  uint8_t padded[32];
  uint8_t digest[16];
  size_t file_id_len = file_id ? strlen(file_id) : 0;
  nanopdf_md5 md5;
  int i = 0;

  security_pad_password(password, padded);
  nanopdf_md5_init(&md5);
  nanopdf_md5_update(&md5, padded, 32);
  nanopdf_md5_update(&md5, (const uint8_t*)owner_key, 32);
  digest[0] = (uint8_t)(permissions & 0xff);
  digest[1] = (uint8_t)((permissions >> 8) & 0xff);
  digest[2] = (uint8_t)((permissions >> 16) & 0xff);
  digest[3] = (uint8_t)((permissions >> 24) & 0xff);
  nanopdf_md5_update(&md5, digest, 4);
  if (file_id_len > 0) {
    nanopdf_md5_update(&md5, (const uint8_t*)file_id, file_id_len);
  }
  if (revision >= 4 && !encrypt_metadata) {
    static const uint8_t ff_bytes[4] = {0xff, 0xff, 0xff, 0xff};
    nanopdf_md5_update(&md5, ff_bytes, 4);
  }
  nanopdf_md5_final(&md5, digest);

  if (revision >= 3) {
    int key_len_bytes = key_bits / 8;
    if (key_len_bytes <= 0 || key_len_bytes > 16) {
      key_len_bytes = 16;
    }
    for (i = 0; i < 50; ++i) {
      nanopdf_md5_hash(digest, (size_t)key_len_bytes, digest);
    }
  }

  *out_key_length = (uint8_t)(key_bits / 8);
  if (*out_key_length == 0) {
    *out_key_length = 5;
  }
  if (*out_key_length > 16) {
    *out_key_length = 16;
  }
  memcpy(out_key, digest, *out_key_length);
}

static int security_sha256_concat(
    nanopdf_context* context,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    const uint8_t* c,
    size_t c_len,
    uint8_t out_hash[32]) {
  uint8_t* buffer = NULL;
  size_t total = a_len + b_len + c_len;

  if (total == 0) {
    nanopdf_sha256_hash((const uint8_t*)"", 0, out_hash);
    return 1;
  }

  buffer = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, total);
  if (!buffer) {
    return 0;
  }
  if (a_len > 0) {
    memcpy(buffer, a, a_len);
  }
  if (b_len > 0) {
    memcpy(buffer + a_len, b, b_len);
  }
  if (c_len > 0) {
    memcpy(buffer + a_len + b_len, c, c_len);
  }
  nanopdf_sha256_hash(buffer, total, out_hash);
  nanopdf__allocator_free(&context->allocator, buffer);
  return 1;
}

static int security_hash_r6(
    nanopdf_context* context,
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

  if (!context || !out_hash || (password_len > 0 && !password) || (salt_len > 0 && !salt) ||
      (u_data_len > 0 && !u_data)) {
    return 0;
  }

  input = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, input_len ? input_len : 1);
  if (!input) {
    return 0;
  }
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
  nanopdf__allocator_free(&context->allocator, input);

  for (;;) {
    size_t single_len = password_len + k_len + u_data_len;
    size_t k1_len = single_len * 64;
    uint8_t* k1 = NULL;
    uint8_t* e = NULL;
    unsigned int sum = 0;
    int i = 0;
    nanopdf_aes128 aes128;

    if (single_len == 0 || k1_len / 64 != single_len) {
      return 0;
    }
    k1 = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, k1_len);
    e = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, k1_len);
    if (!k1 || !e) {
      nanopdf__allocator_free(&context->allocator, e);
      nanopdf__allocator_free(&context->allocator, k1);
      return 0;
    }
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
      nanopdf__allocator_free(&context->allocator, e);
      nanopdf__allocator_free(&context->allocator, k1);
      break;
    }
    nanopdf__allocator_free(&context->allocator, e);
    nanopdf__allocator_free(&context->allocator, k1);
  }

  memcpy(out_hash, k, 32);
  return 1;
}

static int security_authenticate_aes256_password(
    nanopdf_context* context,
    const char* password,
    const nanopdf_basic_object* o_obj,
    const nanopdf_basic_object* u_obj,
    const nanopdf_basic_object* oe_obj,
    const nanopdf_basic_object* ue_obj,
    int revision,
    uint8_t* out_file_key,
    uint8_t* out_file_key_length) {
  const uint8_t zero_iv[16] = {0};
  const uint8_t* candidate = (const uint8_t*)(password ? password : "");
  size_t candidate_len = password ? strlen(password) : 0;
  uint8_t hash[32];
  nanopdf_aes256 aes256;

  if (!context || !o_obj || !u_obj || !oe_obj || !ue_obj || !out_file_key || !out_file_key_length) {
    return 0;
  }
  if (candidate_len > 127) {
    candidate_len = 127;
  }
  if (o_obj->length < 48 || u_obj->length < 48 || oe_obj->length < 32 || ue_obj->length < 32) {
    return 0;
  }

  if (revision >= 6) {
    if (!security_hash_r6(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)u_obj->as.text + 32,
            8,
            NULL,
            0,
            hash)) {
      return 0;
    }
  } else {
    if (!security_sha256_concat(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)u_obj->as.text + 32,
            8,
            NULL,
            0,
            hash)) {
      return 0;
    }
  }
  if (memcmp(hash, u_obj->as.text, 32) == 0) {
    if (revision >= 6) {
      if (!security_hash_r6(
              context,
              candidate,
              candidate_len,
              (const uint8_t*)u_obj->as.text + 40,
              8,
              NULL,
              0,
              hash)) {
        return 0;
      }
    } else {
      if (!security_sha256_concat(
              context,
              candidate,
              candidate_len,
              (const uint8_t*)u_obj->as.text + 40,
              8,
              NULL,
              0,
              hash)) {
        return 0;
      }
    }
    nanopdf_aes256_set_key(&aes256, hash);
    nanopdf_aes256_decrypt_cbc(
        &aes256, (const uint8_t*)ue_obj->as.text, out_file_key, 32, zero_iv);
    *out_file_key_length = 32;
    return 1;
  }

  if (candidate_len == 0) {
    return 0;
  }

  if (revision >= 6) {
    if (!security_hash_r6(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)o_obj->as.text + 32,
            8,
            (const uint8_t*)u_obj->as.text,
            48,
            hash)) {
      return 0;
    }
  } else {
    if (!security_sha256_concat(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)o_obj->as.text + 32,
            8,
            (const uint8_t*)u_obj->as.text,
            48,
            hash)) {
      return 0;
    }
  }
  if (memcmp(hash, o_obj->as.text, 32) != 0) {
    return 0;
  }
  if (revision >= 6) {
    if (!security_hash_r6(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)o_obj->as.text + 40,
            8,
            (const uint8_t*)u_obj->as.text,
            48,
            hash)) {
      return 0;
    }
  } else {
    if (!security_sha256_concat(
            context,
            candidate,
            candidate_len,
            (const uint8_t*)o_obj->as.text + 40,
            8,
            (const uint8_t*)u_obj->as.text,
            48,
            hash)) {
      return 0;
    }
  }
  nanopdf_aes256_set_key(&aes256, hash);
  nanopdf_aes256_decrypt_cbc(
      &aes256, (const uint8_t*)oe_obj->as.text, out_file_key, 32, zero_iv);
  *out_file_key_length = 32;
  return 1;
}

static int security_compute_user_key(
    const char* password,
    const char* owner_key,
    int32_t permissions,
    const char* file_id,
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t out_user[32]) {
  uint8_t file_key[16];
  uint8_t file_key_length = 0;
  security_compute_file_key(
      password,
      owner_key,
      permissions,
      file_id,
      key_bits,
      revision,
      encrypt_metadata,
      file_key,
      &file_key_length);
  if (file_key_length == 0) {
    return 0;
  }
  if (revision == 2) {
    nanopdf_rc4 rc4;
    memcpy(out_user, k_pdf_padding, 32);
    nanopdf_rc4_init(&rc4, file_key, file_key_length);
    nanopdf_rc4_crypt(&rc4, out_user, 32);
    return 1;
  } else {
    uint8_t digest[16];
    size_t file_id_len = file_id ? strlen(file_id) : 0;
    int i = 0;
    nanopdf_md5 md5;
    nanopdf_md5_init(&md5);
    nanopdf_md5_update(&md5, k_pdf_padding, 32);
    if (file_id_len > 0) {
      nanopdf_md5_update(&md5, (const uint8_t*)file_id, file_id_len);
    }
    nanopdf_md5_final(&md5, digest);
    memcpy(out_user, digest, 16);
    memcpy(out_user + 16, k_pdf_padding, 16);
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
    return 1;
  }
}

static int security_verify_user_password(
    const char* password,
    const char* owner_key,
    const char* user_key,
    int32_t permissions,
    const char* file_id,
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t* out_file_key,
    uint8_t* out_file_key_length) {
  uint8_t computed_user[32];
  if (!password || !owner_key || !user_key || !file_id) {
    return 0;
  }
  if (!security_compute_user_key(
          password,
          owner_key,
          permissions,
          file_id,
          key_bits,
          revision,
          encrypt_metadata,
          computed_user)) {
    return 0;
  }
  if ((revision == 2 && memcmp(computed_user, user_key, 32) != 0) ||
      (revision >= 3 && memcmp(computed_user, user_key, 16) != 0)) {
    return 0;
  }
  security_compute_file_key(
      password,
      owner_key,
      permissions,
      file_id,
      key_bits,
      revision,
      encrypt_metadata,
      out_file_key,
      out_file_key_length);
  return *out_file_key_length > 0;
}

static int security_verify_owner_password(
    const char* owner_key,
    const char* user_key,
    const char* password,
    int32_t permissions,
    const char* file_id,
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t* out_file_key,
    uint8_t* out_file_key_length) {
  uint8_t padded_owner[32];
  uint8_t digest[16];
  uint8_t decrypted[33];
  char user_password[33];
  size_t key_len_bytes = 0;
  int i = 0;

  if (!owner_key || !user_key || !password || !file_id) {
    return 0;
  }

  security_pad_password(password, padded_owner);
  nanopdf_md5_hash(padded_owner, 32, digest);
  key_len_bytes = (size_t)(key_bits / 8);
  if (key_len_bytes == 0) {
    key_len_bytes = 5;
  }
  if (key_len_bytes > 16) {
    key_len_bytes = 16;
  }

  if (revision >= 3) {
    for (i = 0; i < 50; ++i) {
      nanopdf_md5_hash(digest, key_len_bytes, digest);
    }
  }

  memcpy(decrypted, owner_key, 32);
  if (revision == 2) {
    nanopdf_rc4 rc4;
    nanopdf_rc4_init(&rc4, digest, key_len_bytes);
    nanopdf_rc4_crypt(&rc4, decrypted, 32);
  } else {
    for (i = 19; i >= 0; --i) {
      uint8_t iter_key[16];
      size_t j = 0;
      for (j = 0; j < key_len_bytes; ++j) {
        iter_key[j] = (uint8_t)(digest[j] ^ i);
      }
      {
        nanopdf_rc4 rc4;
        nanopdf_rc4_init(&rc4, iter_key, key_len_bytes);
        nanopdf_rc4_crypt(&rc4, decrypted, 32);
      }
    }
  }
  decrypted[32] = '\0';

  {
    size_t pwd_len = 32;
    size_t k = 0;
    for (k = 0; k < 32; ++k) {
      size_t j = 0;
      int is_padding = 1;
      for (j = k; j < 32 && (j - k) < 32; ++j) {
        if (decrypted[j] != k_pdf_padding[j - k]) {
          is_padding = 0;
          break;
        }
      }
      if (is_padding) {
        pwd_len = k;
        break;
      }
    }
    memcpy(user_password, decrypted, pwd_len);
    user_password[pwd_len] = '\0';
  }

  return security_verify_user_password(
      user_password,
      owner_key,
      user_key,
      permissions,
      file_id,
      key_bits,
      revision,
      encrypt_metadata,
      out_file_key,
      out_file_key_length);
}

static int security_authenticate_password(
    const char* password,
    const char* owner_key,
    const char* user_key,
    int32_t permissions,
    const char* file_id,
    int key_bits,
    int revision,
    int encrypt_metadata,
    uint8_t* out_file_key,
    uint8_t* out_file_key_length) {
  const char* candidate = password ? password : "";

  if (security_verify_user_password(
          candidate,
          owner_key,
          user_key,
          permissions,
          file_id,
          key_bits,
          revision,
          encrypt_metadata,
          out_file_key,
          out_file_key_length)) {
    return 1;
  }

  if (!candidate[0]) {
    return 0;
  }

  return security_verify_owner_password(
      owner_key,
      user_key,
      candidate,
      permissions,
      file_id,
      key_bits,
      revision,
      encrypt_metadata,
      out_file_key,
      out_file_key_length);
}

static nanopdf_status resolve_encrypt_dict_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    nanopdf_basic_object* out_loaded,
    const nanopdf_basic_object** out_resolved) {
  nanopdf_ref ref;

  if (!object || !out_resolved) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid encrypt object");
  }

  *out_resolved = object;
  if (out_loaded) {
    nanopdf_basic_object_init(out_loaded);
  }
  if (object->type != NANOPDF_BASIC_OBJECT_REF || !basic_object_to_ref(object, &ref)) {
    return clear_status(context);
  }
  if (!out_loaded) {
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "indirect encrypt object unsupported");
  }
  return nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){ref.object_number, ref.generation, ref.valid},
      out_loaded);
}

static nanopdf_status parse_v4_crypt_filter_algorithm(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_dict* encrypt_dict,
    const char* filter_name,
    int key_bits,
    uint8_t* out_algorithm) {
  const nanopdf_basic_object* cf_obj = NULL;
  const nanopdf_basic_object* entry_obj = NULL;
  const nanopdf_basic_object* cfm_obj = NULL;
  const nanopdf_basic_dict* cf_dict = NULL;
  const nanopdf_basic_dict* entry_dict = NULL;
  nanopdf_basic_object loaded_cf;
  nanopdf_basic_object loaded_entry;
  const nanopdf_basic_object* resolved_cf = NULL;
  const nanopdf_basic_object* resolved_entry = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!out_algorithm) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid crypt filter output");
  }

  *out_algorithm = NANOPDF_BASIC_SECURITY_NONE;
  if (!filter_name || strcmp(filter_name, "Identity") == 0) {
    return clear_status(context);
  }

  cf_obj = encrypt_dict ? nanopdf_basic_dict_get(encrypt_dict, "CF") : NULL;
  if (!cf_obj) {
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "missing crypt filter dictionary");
  }

  nanopdf_basic_object_init(&loaded_cf);
  nanopdf_basic_object_init(&loaded_entry);
  status = resolve_encrypt_dict_object(context, document, cf_obj, &loaded_cf, &resolved_cf);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return status;
  }
  cf_dict = nanopdf_basic_object_as_dict(
      resolved_cf->type == NANOPDF_BASIC_OBJECT_REF ? NULL : resolved_cf);
  if (!cf_dict) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "invalid crypt filter dictionary");
  }

  entry_obj = nanopdf_basic_dict_get(cf_dict, filter_name);
  if (!entry_obj) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "missing crypt filter entry");
  }
  status = resolve_encrypt_dict_object(context, document, entry_obj, &loaded_entry, &resolved_entry);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return status;
  }
  entry_dict = nanopdf_basic_object_as_dict(
      resolved_entry->type == NANOPDF_BASIC_OBJECT_REF ? NULL : resolved_entry);
  if (!entry_dict) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "invalid crypt filter entry");
  }

  cfm_obj = nanopdf_basic_dict_get(entry_dict, "CFM");
  if (!cfm_obj || cfm_obj->type != NANOPDF_BASIC_OBJECT_NAME) {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "missing crypt filter method");
  }

  if (strcmp(cfm_obj->as.text, "None") == 0) {
    *out_algorithm = NANOPDF_BASIC_SECURITY_NONE;
  } else if (strcmp(cfm_obj->as.text, "V2") == 0) {
    *out_algorithm =
        (uint8_t)(key_bits <= 40 ? NANOPDF_BASIC_SECURITY_RC4_40
                                 : NANOPDF_BASIC_SECURITY_RC4_128);
  } else if (strcmp(cfm_obj->as.text, "AESV2") == 0) {
    *out_algorithm = NANOPDF_BASIC_SECURITY_AES_128;
  } else if (strcmp(cfm_obj->as.text, "AESV3") == 0) {
    *out_algorithm = NANOPDF_BASIC_SECURITY_AES_256;
  } else {
    nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
    nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "unsupported crypt filter method");
  }

  nanopdf_basic_object_destroy(&context->allocator, &loaded_cf);
  nanopdf_basic_object_destroy(&context->allocator, &loaded_entry);
  return clear_status(context);
}

static int repair_scan_object_offsets(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count) {
  const char* text = (const char*)data;
  size_t pos = 0;
  size_t capacity = 0;
  size_t count = 0;
  nanopdf_basic_xref_entry* xrefs = NULL;

  while (pos + 6 < size) {
    size_t cursor = pos;
    uint32_t object_number = 0;
    uint32_t generation = 0;
    if (!isdigit((unsigned char)text[cursor])) {
      pos++;
      continue;
    }
    if (pos > 0 && !isspace((unsigned char)text[pos - 1]) &&
        text[pos - 1] != '\0') {
      pos++;
      continue;
    }
    if (!parse_unsigned_value(text, size, &cursor, &object_number)) {
      pos++;
      continue;
    }
    if (cursor >= size || text[cursor] != ' ') {
      pos++;
      continue;
    }
    cursor++;
    if (!parse_unsigned_value(text, size, &cursor, &generation)) {
      pos++;
      continue;
    }
    if (cursor >= size || text[cursor] != ' ') {
      pos++;
      continue;
    }
    cursor++;
    if (!match_literal(text, size, cursor, "obj")) {
      pos++;
      continue;
    }
    if (ensure_xref_capacity(context, &xrefs, &capacity, (size_t)object_number + 1) !=
        NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, xrefs);
      return 0;
    }
    if (count <= object_number) {
      count = (size_t)object_number + 1;
    }
    xrefs[object_number].present = 1;
    xrefs[object_number].in_use = 1;
    xrefs[object_number].compressed = 0;
    xrefs[object_number].offset = pos;
    xrefs[object_number].generation = (uint16_t)generation;
    pos = cursor + 3;
  }

  *out_xrefs = xrefs;
  *out_xref_count = count;
  return count > 0;
}

static void repair_extract_trailer_refs(
    const char* data,
    size_t size,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt) {
  size_t pos = size;
  while (pos > 7) {
    size_t value_start = 0;
    size_t value_end = 0;
    size_t dict_end = 0;
    const char* trailer = NULL;
    size_t trailer_length = 0;
    pos--;
    if (!match_literal(data, size, pos, "trailer")) {
      continue;
    }
    pos += 7;
    skip_ws(data, size, &pos);
    if (pos + 1 >= size || data[pos] != '<' || data[pos + 1] != '<') {
      continue;
    }
    if (!find_matching_dict_end(data, size, pos, &dict_end)) {
      continue;
    }
    trailer = data + pos;
    trailer_length = dict_end - pos;
    if (find_top_level_key_value(trailer, trailer_length, "Root", &value_start, &value_end)) {
      parse_ref_value(trailer, value_start, value_end, out_root_ref);
    }
    if (find_top_level_key_value(trailer, trailer_length, "Info", &value_start, &value_end)) {
      parse_ref_value(trailer, value_start, value_end, out_info_ref);
    }
    if (find_top_level_key_value(trailer, trailer_length, "Encrypt", &value_start, &value_end)) {
      if (out_has_encrypt) {
        *out_has_encrypt = 1;
      }
    }
    return;
  }
}

static int repair_find_catalog_root(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const nanopdf_basic_xref_entry* xrefs,
    size_t xref_count,
    nanopdf_ref* out_root_ref) {
  size_t i = 0;
  for (i = 1; i < xref_count; ++i) {
    nanopdf_basic_object object;
    const nanopdf_basic_dict* dict = NULL;
    const nanopdf_basic_object* type_obj = NULL;
    if (!xrefs[i].present || !xrefs[i].in_use || xrefs[i].compressed) {
      continue;
    }
    nanopdf_basic_object_init(&object);
    if (nanopdf_basic_parse_indirect_object_at(
            context, data, size, xrefs[i].offset, &object) != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &object);
      nanopdf__clear_error(context);
      continue;
    }
    dict = nanopdf_basic_object_as_dict(&object);
    type_obj = dict ? nanopdf_basic_dict_get(dict, "Type") : NULL;
    if (type_obj && type_obj->type == NANOPDF_BASIC_OBJECT_NAME &&
        strcmp(type_obj->as.text, "Catalog") == 0) {
      out_root_ref->object_number = (uint32_t)i;
      out_root_ref->generation = xrefs[i].generation;
      out_root_ref->valid = 1;
      nanopdf_basic_object_destroy(&context->allocator, &object);
      return 1;
    }
    nanopdf_basic_object_destroy(&context->allocator, &object);
  }
  return 0;
}

static nanopdf_status repair_xref_scan(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt) {
  if (!repair_scan_object_offsets(context, data, size, out_xrefs, out_xref_count)) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "xref repair failed");
  }
  if (out_root_ref) {
    memset(out_root_ref, 0, sizeof(*out_root_ref));
  }
  if (out_info_ref) {
    memset(out_info_ref, 0, sizeof(*out_info_ref));
  }
  if (out_has_encrypt) {
    *out_has_encrypt = 0;
  }

  repair_extract_trailer_refs(
      (const char*)data, size, out_root_ref, out_info_ref, out_has_encrypt);
  if ((!out_root_ref || !out_root_ref->valid) &&
      out_root_ref &&
      !repair_find_catalog_root(
          context, data, size, *out_xrefs, *out_xref_count, out_root_ref)) {
    nanopdf__allocator_free(&context->allocator, *out_xrefs);
    *out_xrefs = NULL;
    *out_xref_count = 0;
    return set_status(context, NANOPDF_STATUS_MALFORMED, "xref repair could not find catalog");
  }

  return clear_status(context);
}

static int find_matching_dict_end(
    const char* data,
    size_t size,
    size_t pos,
    size_t* out_end) {
  int depth = 0;
  int in_string = 0;
  size_t cursor = pos;

  while (cursor + 1 < size) {
    if (in_string) {
      if (data[cursor] == '\\' && cursor + 1 < size) {
        cursor += 2;
        continue;
      }
      if (data[cursor] == ')') {
        in_string = 0;
      }
      cursor++;
      continue;
    }

    if (data[cursor] == '(') {
      in_string = 1;
      cursor++;
      continue;
    }

    if (data[cursor] == '<' && data[cursor + 1] == '<') {
      depth++;
      cursor += 2;
      continue;
    }

    if (data[cursor] == '>' && data[cursor + 1] == '>') {
      depth--;
      cursor += 2;
      if (depth == 0) {
        *out_end = cursor;
        return 1;
      }
      continue;
    }

    cursor++;
  }

  return 0;
}

static int find_object_slice(
    const char* data,
    size_t size,
    size_t offset,
    const char** out_object,
    size_t* out_length) {
  size_t pos = offset;
  uint32_t ignored = 0;
  size_t end = 0;

  if (offset >= size) {
    return 0;
  }

  skip_ws(data, size, &pos);
  if (!parse_unsigned_value(data, size, &pos, &ignored)) {
    return 0;
  }
  skip_ws(data, size, &pos);
  if (!parse_unsigned_value(data, size, &pos, &ignored)) {
    return 0;
  }
  skip_ws(data, size, &pos);
  if (!match_literal(data, size, pos, "obj")) {
    return 0;
  }
  pos += 3;
  skip_ws(data, size, &pos);

  if (pos >= size || data[pos] != '<' || pos + 1 >= size || data[pos + 1] != '<') {
    return 0;
  }
  if (!find_matching_dict_end(data, size, pos, &end)) {
    return 0;
  }

  *out_object = data + pos;
  *out_length = end - pos;
  return 1;
}

static int NANOPDF_MAYBE_UNUSED find_stream_slice(
    const char* data,
    size_t size,
    size_t offset,
    const char** out_stream,
    size_t* out_length) {
  const char* object = NULL;
  size_t object_length = 0;
  const char* object_start = data + offset;
  const char* stream_kw = NULL;
  const char* stream_begin = NULL;
  const char* stream_end = NULL;

  if (!find_object_slice(data, size, offset, &object, &object_length)) {
    return 0;
  }

  stream_kw = strstr(object_start, "stream");
  if (!stream_kw) {
    return 0;
  }

  stream_begin = stream_kw + 6;
  if ((size_t)(stream_begin - data) < size && *stream_begin == '\r') {
    stream_begin++;
  }
  if ((size_t)(stream_begin - data) < size && *stream_begin == '\n') {
    stream_begin++;
  }

  stream_end = strstr(stream_begin, "endstream");
  if (!stream_end) {
    return 0;
  }

  while (stream_end > stream_begin &&
         (stream_end[-1] == '\n' || stream_end[-1] == '\r')) {
    stream_end--;
  }

  *out_stream = stream_begin;
  *out_length = (size_t)(stream_end - stream_begin);
  return 1;
}

static int parse_string_token(
    const char* data,
    size_t size,
    size_t* pos,
    char* out_buffer,
    size_t out_capacity) {
  size_t cursor = *pos;
  size_t length = 0;
  int depth = 0;

  if (cursor >= size || data[cursor] != '(') {
    return 0;
  }

  cursor++;
  depth = 1;
  while (cursor < size && depth > 0) {
    const char ch = data[cursor++];
    if (ch == '\\' && cursor < size) {
      const char escaped = data[cursor++];
      char resolved = escaped;
      if (escaped == 'n') resolved = '\n';
      else if (escaped == 'r') resolved = '\r';
      else if (escaped == 't') resolved = '\t';
      else if (escaped == 'b') resolved = '\b';
      else if (escaped == 'f') resolved = '\f';
      if (depth == 1 && length + 1 < out_capacity) {
        out_buffer[length++] = resolved;
      }
      continue;
    }
    if (ch == '(') {
      depth++;
      if (depth == 1 && length + 1 < out_capacity) {
        out_buffer[length++] = ch;
      }
      continue;
    }
    if (ch == ')') {
      depth--;
      if (depth == 0) {
        break;
      }
    }
    if (depth == 1 && length + 1 < out_capacity) {
      out_buffer[length++] = ch;
    }
  }

  if (depth != 0 || out_capacity == 0) {
    return 0;
  }

  out_buffer[length] = '\0';
  *pos = cursor;
  return 1;
}

static int parse_hex_string_token(
    const char* data,
    size_t size,
    size_t* pos,
    char* out_buffer,
    size_t out_capacity) {
  size_t cursor = *pos;
  size_t length = 0;
  int hi = -1;

  if (cursor >= size || data[cursor] != '<' ||
      (cursor + 1 < size && data[cursor + 1] == '<') ||
      out_capacity == 0) {
    return 0;
  }

  cursor++;
  while (cursor < size && data[cursor] != '>') {
    int value = 0;
    if (isspace((unsigned char)data[cursor])) {
      cursor++;
      continue;
    }
    value = hex_value(data[cursor]);
    if (value < 0) {
      return 0;
    }
    if (hi < 0) {
      hi = value;
    } else {
      if (length + 1 >= out_capacity) {
        return 0;
      }
      out_buffer[length++] = (char)((hi << 4) | value);
      hi = -1;
    }
    cursor++;
  }

  if (cursor >= size || data[cursor] != '>') {
    return 0;
  }
  cursor++;
  if (hi >= 0) {
    if (length + 1 >= out_capacity) {
      return 0;
    }
    out_buffer[length++] = (char)(hi << 4);
  }
  out_buffer[length] = '\0';
  *pos = cursor;
  return 1;
}

static int find_top_level_key_value(
    const char* object,
    size_t length,
    const char* key,
    size_t* out_value_start,
    size_t* out_value_end) {
  size_t pos = 0;
  int dict_depth = 0;
  int array_depth = 0;
  int in_string = 0;
  const size_t key_length = strlen(key);

  while (pos < length) {
    const char ch = object[pos];

    if (in_string) {
      if (ch == '\\' && pos + 1 < length) {
        pos += 2;
        continue;
      }
      if (ch == ')') {
        in_string = 0;
      }
      pos++;
      continue;
    }

    if (ch == '(') {
      in_string = 1;
      pos++;
      continue;
    }

    if (ch == '<' && pos + 1 < length && object[pos + 1] == '<') {
      dict_depth++;
      pos += 2;
      continue;
    }
    if (ch == '>' && pos + 1 < length && object[pos + 1] == '>') {
      dict_depth--;
      pos += 2;
      continue;
    }
    if (ch == '[') {
      array_depth++;
      pos++;
      continue;
    }
    if (ch == ']') {
      array_depth--;
      pos++;
      continue;
    }

    if (dict_depth == 1 && array_depth == 0 && ch == '/' &&
        pos + 1 + key_length <= length &&
        memcmp(object + pos + 1, key, key_length) == 0) {
      const size_t key_end = pos + 1 + key_length;
      if (key_end == length || isspace((unsigned char)object[key_end]) ||
          is_delim(object[key_end])) {
        size_t value_pos = key_end;
        size_t value_end = value_pos;
        int nested_array = 0;
        int nested_dict = 0;
        int nested_string = 0;

        skip_ws(object, length, &value_pos);
        value_end = value_pos;
        while (value_end < length) {
          const char value_ch = object[value_end];
          if (nested_string) {
            if (value_ch == '\\' && value_end + 1 < length) {
              value_end += 2;
              continue;
            }
            if (value_ch == ')') {
              nested_string = 0;
            }
            value_end++;
            continue;
          }
          if (value_ch == '(') {
            nested_string = 1;
            value_end++;
            continue;
          }
          if (value_ch == '[') {
            nested_array++;
            value_end++;
            continue;
          }
          if (value_ch == ']') {
            nested_array--;
            value_end++;
            if (nested_array <= 0 && nested_dict == 0) {
              break;
            }
            continue;
          }
          if (value_ch == '<' && value_end + 1 < length &&
              object[value_end + 1] == '<') {
            nested_dict++;
            value_end += 2;
            continue;
          }
          if (value_ch == '>' && value_end + 1 < length &&
              object[value_end + 1] == '>') {
            nested_dict--;
            value_end += 2;
            if (nested_dict <= 0 && nested_array == 0) {
              break;
            }
            continue;
          }
          if (nested_array == 0 && nested_dict == 0 &&
              value_ch == '/' && value_end + 1 < length &&
              value_end > value_pos &&
              isalpha((unsigned char)object[value_end + 1])) {
            break;
          }
          if (nested_array == 0 && nested_dict == 0 &&
              isspace((unsigned char)value_ch)) {
            size_t lookahead = value_end;
            skip_ws(object, length, &lookahead);
            if (lookahead < length && object[lookahead] == '/' &&
                (lookahead + 1 >= length ||
                 isalpha((unsigned char)object[lookahead + 1]))) {
              break;
            }
          }
          value_end++;
        }

        *out_value_start = value_pos;
        *out_value_end = value_end;
        return 1;
      }
    }

    pos++;
  }

  return 0;
}

static int parse_ref_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_ref* out_ref) {
  size_t pos = start;
  uint32_t object_number = 0;
  uint32_t generation = 0;

  skip_ws(object, end, &pos);
  if (!parse_unsigned_value(object, end, &pos, &object_number)) {
    return 0;
  }
  skip_ws(object, end, &pos);
  if (!parse_unsigned_value(object, end, &pos, &generation)) {
    return 0;
  }
  skip_ws(object, end, &pos);
  if (pos >= end || object[pos] != 'R') {
    return 0;
  }

  out_ref->object_number = object_number;
  out_ref->generation = (uint16_t)generation;
  out_ref->valid = 1;
  return 1;
}

static int parse_name_value(
    const char* object,
    size_t start,
    size_t end,
    char* out_name,
    size_t out_capacity) {
  size_t pos = start;
  size_t length = 0;

  skip_ws(object, end, &pos);
  if (pos >= end || object[pos] != '/' || out_capacity == 0) {
    return 0;
  }

  pos++;
  while (pos < end && !isspace((unsigned char)object[pos]) &&
         !is_delim(object[pos]) && length + 1 < out_capacity) {
    out_name[length++] = object[pos++];
  }
  out_name[length] = '\0';
  return length > 0;
}

static int parse_int_value(
    const char* object,
    size_t start,
    size_t end,
    int* out_value) {
  size_t pos = start;
  double value = 0.0;
  skip_ws(object, end, &pos);
  if (!parse_number_token(object, end, &pos, &value)) {
    return 0;
  }
  *out_value = (int)value;
  return 1;
}

static int parse_box_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_box* out_box) {
  size_t pos = start;
  int i;

  skip_ws(object, end, &pos);
  if (pos >= end || object[pos] != '[') {
    return 0;
  }
  pos++;
  for (i = 0; i < 4; ++i) {
    skip_ws(object, end, &pos);
    if (!parse_number_token(object, end, &pos, &out_box->values[i])) {
      return 0;
    }
  }
  out_box->valid = 1;
  return 1;
}

static int parse_info_string(
    const char* object,
    size_t start,
    size_t end,
    char* out_buffer,
    size_t out_capacity) {
  size_t pos = start;
  skip_ws(object, end, &pos);
  if (pos < end && object[pos] == '(') {
    return parse_string_token(object, end, &pos, out_buffer, out_capacity);
  }
  if (pos < end && object[pos] == '<' &&
      !(pos + 1 < end && object[pos + 1] == '<')) {
    return parse_hex_string_token(object, end, &pos, out_buffer, out_capacity);
  }
  if (pos < end && object[pos] == '/') {
    return parse_name_value(object, pos, end, out_buffer, out_capacity);
  }
  return 0;
}

static int append_output_char(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity,
    char ch);
static int append_output_text(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity,
    const char* text);
static int decode_pdf_string_buffer(
    nanopdf_context* context,
    const char* input,
    size_t input_length,
    int decode_pdfdoc_encoding,
    char** out_text);

static nanopdf_status ensure_xref_capacity(
    nanopdf_context* context,
    nanopdf_basic_xref_entry** entries,
    size_t* capacity,
    size_t required) {
  nanopdf_basic_xref_entry* resized;
  size_t new_capacity = *capacity;

  if (required <= *capacity) {
    return NANOPDF_STATUS_OK;
  }

  if (new_capacity == 0) {
    new_capacity = 16;
  }
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  resized = (nanopdf_basic_xref_entry*)nanopdf__allocator_realloc(
      &context->allocator, *entries, new_capacity * sizeof(**entries));
  if (!resized) {
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memset(resized + *capacity, 0, (new_capacity - *capacity) * sizeof(**entries));
  *entries = resized;
  *capacity = new_capacity;
  return NANOPDF_STATUS_OK;
}

static nanopdf_status append_page(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    const nanopdf_box* media_box,
    double rotation,
    const nanopdf_ref* content_refs,
    size_t content_count) {
  nanopdf_basic_page* resized;
  size_t new_capacity;
  size_t page_index;

  if (document->page_count == document->page_capacity) {
    new_capacity = document->page_capacity == 0 ? 8 : document->page_capacity * 2;
    resized = (nanopdf_basic_page*)nanopdf__allocator_realloc(
        &context->allocator,
        document->pages,
        new_capacity * sizeof(*document->pages));
    if (!resized) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow page list");
    }
    document->pages = resized;
    document->page_capacity = new_capacity;
  }

  page_index = document->page_count;
  memset(&document->pages[page_index], 0, sizeof(document->pages[page_index]));
  document->pages[page_index].rotation = rotation;
  if (media_box->valid) {
    document->pages[page_index].width =
        media_box->values[2] - media_box->values[0];
    document->pages[page_index].height =
        media_box->values[3] - media_box->values[1];
  } else {
    document->pages[page_index].width = 0.0;
    document->pages[page_index].height = 0.0;
  }
  if (content_count > 0) {
    size_t i;
    document->pages[page_index].contents =
        (nanopdf_basic_content_ref*)nanopdf__allocator_alloc(
            &context->allocator,
            content_count * sizeof(nanopdf_basic_content_ref));
    if (!document->pages[page_index].contents) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store page content refs");
    }
    document->pages[page_index].content_capacity = content_count;
    document->pages[page_index].content_count = content_count;
    for (i = 0; i < content_count; ++i) {
      document->pages[page_index].contents[i].object_number = content_refs[i].object_number;
      document->pages[page_index].contents[i].generation = content_refs[i].generation;
      document->pages[page_index].contents[i].valid = content_refs[i].valid;
    }
  }
  document->page_count++;
  return NANOPDF_STATUS_OK;
}

static void destroy_basic_content(
    const nanopdf_allocator* allocator,
    nanopdf_basic_content_ref* content) {
  if (!allocator || !content) {
    return;
  }
  nanopdf__allocator_free(allocator, content->decoded_data);
  memset(content, 0, sizeof(*content));
}

static int duplicate_basic_content(
    nanopdf_context* context,
    const nanopdf_basic_content_ref* source,
    nanopdf_basic_content_ref* destination) {
  size_t alloc_size = 0;

  if (!context || !source || !destination) {
    return 0;
  }

  memset(destination, 0, sizeof(*destination));
  destination->kind = source->kind;
  destination->object_number = source->object_number;
  destination->generation = source->generation;
  destination->valid = source->valid;
  destination->decoded_size = source->decoded_size;
  if (!source->decoded_data && source->decoded_size == 0) {
    return 1;
  }

  alloc_size = source->decoded_size == 0 ? 1 : source->decoded_size;
  destination->decoded_data =
      (uint8_t*)nanopdf__allocator_alloc(&context->allocator, alloc_size);
  if (!destination->decoded_data) {
    memset(destination, 0, sizeof(*destination));
    return 0;
  }
  if (source->decoded_size > 0) {
    memcpy(destination->decoded_data, source->decoded_data, source->decoded_size);
  }
  return 1;
}

static nanopdf_status append_page_contents(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    nanopdf_ref page_ref,
    const nanopdf_box* media_box,
    double rotation,
    const nanopdf_basic_content_ref* contents,
    size_t content_count) {
  nanopdf_basic_page* resized;
  size_t new_capacity;
  size_t page_index;

  if (document->page_count == document->page_capacity) {
    new_capacity = document->page_capacity == 0 ? 8 : document->page_capacity * 2;
    resized = (nanopdf_basic_page*)nanopdf__allocator_realloc(
        &context->allocator,
        document->pages,
        new_capacity * sizeof(*document->pages));
    if (!resized) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow page list");
    }
    document->pages = resized;
    document->page_capacity = new_capacity;
  }

  page_index = document->page_count;
  memset(&document->pages[page_index], 0, sizeof(document->pages[page_index]));
  document->pages[page_index].rotation = rotation;
  document->pages[page_index].object_number = page_ref.object_number;
  document->pages[page_index].generation = page_ref.generation;
  document->pages[page_index].valid = page_ref.valid;
  if (media_box->valid) {
    document->pages[page_index].width =
        media_box->values[2] - media_box->values[0];
    document->pages[page_index].height =
        media_box->values[3] - media_box->values[1];
  } else {
    document->pages[page_index].width = 0.0;
    document->pages[page_index].height = 0.0;
  }
  if (content_count > 0) {
    size_t i;
    document->pages[page_index].contents =
        (nanopdf_basic_content_ref*)nanopdf__allocator_alloc(
            &context->allocator,
            content_count * sizeof(*document->pages[page_index].contents));
    if (!document->pages[page_index].contents) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store page contents");
    }
    document->pages[page_index].content_capacity = content_count;
    document->pages[page_index].content_count = content_count;
    for (i = 0; i < content_count; ++i) {
      if (!duplicate_basic_content(
              context, &contents[i], &document->pages[page_index].contents[i])) {
        size_t j;
        for (j = 0; j < i; ++j) {
          destroy_basic_content(
              &context->allocator, &document->pages[page_index].contents[j]);
        }
        nanopdf__allocator_free(
            &context->allocator, document->pages[page_index].contents);
        memset(&document->pages[page_index], 0, sizeof(document->pages[page_index]));
        return set_status(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to duplicate page contents");
      }
    }
  }
  document->page_count++;
  return NANOPDF_STATUS_OK;
}

static int parse_contents_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_ref** out_refs,
    size_t* out_count,
    nanopdf_context* context) {
  size_t pos = start;
  nanopdf_ref single = {0, 0, 0};
  nanopdf_ref* refs = NULL;
  size_t count = 0;
  size_t capacity = 0;

  skip_ws(object, end, &pos);
  if (parse_ref_value(object, pos, end, &single)) {
    refs = (nanopdf_ref*)nanopdf__allocator_alloc(&context->allocator, sizeof(*refs));
    if (!refs) {
      return -1;
    }
    refs[0] = single;
    *out_refs = refs;
    *out_count = 1;
    return 1;
  }

  if (pos >= end || object[pos] != '[') {
    return 0;
  }
  pos++;
  while (pos < end) {
    nanopdf_ref ref = {0, 0, 0};
    skip_ws(object, end, &pos);
    if (pos < end && object[pos] == ']') {
      break;
    }
    if (!parse_ref_value(object, pos, end, &ref)) {
      break;
    }
    while (pos < end && object[pos] != 'R') pos++;
    if (pos < end && object[pos] == 'R') pos++;
    if (count == capacity) {
      size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
      nanopdf_ref* resized = (nanopdf_ref*)nanopdf__allocator_realloc(
          &context->allocator, refs, new_capacity * sizeof(*refs));
      if (!resized) {
        nanopdf__allocator_free(&context->allocator, refs);
        return -1;
      }
      refs = resized;
      capacity = new_capacity;
    }
    refs[count++] = ref;
  }

  if (count == 0) {
    nanopdf__allocator_free(&context->allocator, refs);
    return 0;
  }

  *out_refs = refs;
  *out_count = count;
  return 1;
}

static nanopdf_status NANOPDF_MAYBE_UNUSED parse_document_info(
    nanopdf_context* context,
    const char* object,
    size_t length,
    nanopdf_basic_document* document) {
  static const char* keys[] = {
      "Title", "Author", "Subject", "Keywords",
      "Creator", "Producer", "CreationDate", "ModDate", "Trapped"};
  char** targets[] = {
      &document->title, &document->author, &document->subject, &document->keywords,
      &document->creator, &document->producer, &document->creation_date,
      &document->mod_date, &document->trapped};
  size_t i;

  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    size_t value_start = 0;
    size_t value_end = 0;
    char buffer[1024];
    if (!find_top_level_key_value(
            object, length, keys[i], &value_start, &value_end)) {
      continue;
    }
    if (!parse_info_string(object, value_start, value_end, buffer, sizeof(buffer))) {
      continue;
    }
    *targets[i] = nanopdf__strdup(&context->allocator, buffer);
    if (!*targets[i]) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store document info");
    }
  }

  return NANOPDF_STATUS_OK;
}

static void destroy_basic_form_field(
    const nanopdf_allocator* allocator,
    nanopdf_basic_form_field* field) {
  if (!allocator || !field) {
    return;
  }

  nanopdf__allocator_free(allocator, field->partial_name);
  nanopdf__allocator_free(allocator, field->full_name);
  nanopdf__allocator_free(allocator, field->alternate_name);
  nanopdf__allocator_free(allocator, field->mapping_name);
  nanopdf__allocator_free(allocator, field->value);
  memset(field, 0, sizeof(*field));
}

static void destroy_basic_output_intent(
    const nanopdf_allocator* allocator,
    nanopdf_basic_output_intent* output_intent) {
  if (!allocator || !output_intent) {
    return;
  }

  nanopdf__allocator_free(allocator, output_intent->subtype);
  nanopdf__allocator_free(allocator, output_intent->output_condition);
  nanopdf__allocator_free(allocator, output_intent->output_condition_identifier);
  nanopdf__allocator_free(allocator, output_intent->registry_name);
  nanopdf__allocator_free(allocator, output_intent->info);
  nanopdf__allocator_free(allocator, output_intent->dest_output_profile_data);
  memset(output_intent, 0, sizeof(*output_intent));
}

static void destroy_basic_page_label_entry(
    const nanopdf_allocator* allocator,
    nanopdf_basic_page_label_entry* page_label) {
  if (!allocator || !page_label) {
    return;
  }

  nanopdf__allocator_free(allocator, page_label->prefix);
  memset(page_label, 0, sizeof(*page_label));
}

static void destroy_basic_named_destination(
    const nanopdf_allocator* allocator,
    nanopdf_basic_named_destination* destination) {
  if (!allocator || !destination) {
    return;
  }

  nanopdf__allocator_free(allocator, destination->name);
  nanopdf__allocator_free(allocator, destination->fit_type);
  nanopdf__allocator_free(allocator, destination->position);
  memset(destination, 0, sizeof(*destination));
}

static char* duplicate_joined_name(
    nanopdf_context* context,
    const char* parent_name,
    const char* partial_name) {
  const size_t parent_length = parent_name ? strlen(parent_name) : 0;
  const size_t partial_length = partial_name ? strlen(partial_name) : 0;
  char* joined = NULL;

  if (parent_length == 0 && partial_length == 0) {
    return nanopdf__strdup(&context->allocator, "");
  }
  if (parent_length == 0) {
    return nanopdf__strdup(&context->allocator, partial_name);
  }
  if (partial_length == 0) {
    return nanopdf__strdup(&context->allocator, parent_name);
  }

  joined = (char*)nanopdf__allocator_alloc(
      &context->allocator, parent_length + partial_length + 2);
  if (!joined) {
    return NULL;
  }

  memcpy(joined, parent_name, parent_length);
  joined[parent_length] = '.';
  memcpy(joined + parent_length + 1, partial_name, partial_length);
  joined[parent_length + partial_length + 1] = '\0';
  return joined;
}

static int parse_form_string_like_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_context* context,
    char** out_value) {
  char buffer[1024];
  if (!parse_info_string(object, start, end, buffer, sizeof(buffer))) {
    return 0;
  }
  *out_value = nanopdf__strdup(&context->allocator, buffer);
  return *out_value != NULL;
}

static int parse_form_value(
    const char* object,
    size_t start,
    size_t end,
    nanopdf_context* context,
    char** out_value) {
  size_t pos = start;
  char* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  int first_item = 1;

  *out_value = NULL;
  skip_ws(object, end, &pos);
  if (pos >= end) {
    return 0;
  }

  if (object[pos] == '(' || object[pos] == '/') {
    return parse_form_string_like_value(object, pos, end, context, out_value);
  }
  if (object[pos] != '[') {
    return 0;
  }

  pos++;
  while (pos < end) {
    char* item = NULL;
    skip_ws(object, end, &pos);
    if (pos < end && object[pos] == ']') {
      pos++;
      break;
    }
    if (!parse_form_string_like_value(object, pos, end, context, &item)) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return 0;
    }
    while (pos < end && !isspace((unsigned char)object[pos]) && object[pos] != ']') {
      if (object[pos] == ')' || (object[pos] == '/' && pos != start)) {
        pos++;
        break;
      }
      pos++;
    }
    if (!first_item) {
      if (!append_output_char(context, &buffer, &length, &capacity, ',')) {
        nanopdf__allocator_free(&context->allocator, item);
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
    }
    if (!append_output_text(context, &buffer, &length, &capacity, item)) {
      nanopdf__allocator_free(&context->allocator, item);
      nanopdf__allocator_free(&context->allocator, buffer);
      return 0;
    }
    first_item = 0;
    nanopdf__allocator_free(&context->allocator, item);
  }

  if (!buffer) {
    buffer = nanopdf__strdup(&context->allocator, "");
  }
  *out_value = buffer;
  return *out_value != NULL;
}

static int parse_field_type(
    const char* object,
    size_t length,
    nanopdf_field_type* out_type) {
  size_t value_start = 0;
  size_t value_end = 0;
  char type_name[16];

  if (!find_top_level_key_value(object, length, "FT", &value_start, &value_end)) {
    return 0;
  }
  if (!parse_name_value(object, value_start, value_end, type_name, sizeof(type_name))) {
    return 0;
  }

  if (strcmp(type_name, "Btn") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_BUTTON;
    return 1;
  }
  if (strcmp(type_name, "Tx") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_TEXT;
    return 1;
  }
  if (strcmp(type_name, "Ch") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_CHOICE;
    return 1;
  }
  if (strcmp(type_name, "Sig") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_SIGNATURE;
    return 1;
  }

  return 0;
}

static int append_form_field(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    nanopdf_field_type type,
    char* partial_name,
    char* full_name,
    char* alternate_name,
    char* mapping_name,
    char* value,
    uint32_t flags) {
  nanopdf_basic_form_field* resized = NULL;
  size_t new_capacity = 0;
  nanopdf_basic_form_field* field = NULL;

  if (document->form_field_count == document->form_field_capacity) {
    new_capacity =
        document->form_field_capacity == 0 ? 4 : document->form_field_capacity * 2;
    resized = (nanopdf_basic_form_field*)nanopdf__allocator_realloc(
        &context->allocator,
        document->form_fields,
        new_capacity * sizeof(*document->form_fields));
    if (!resized) {
      return 0;
    }
    memset(
        resized + document->form_field_capacity,
        0,
        (new_capacity - document->form_field_capacity) * sizeof(*document->form_fields));
    document->form_fields = resized;
    document->form_field_capacity = new_capacity;
  }

  field = &document->form_fields[document->form_field_count++];
  memset(field, 0, sizeof(*field));
  field->type = type;
  field->partial_name = partial_name;
  field->full_name = full_name;
  field->alternate_name = alternate_name;
  field->mapping_name = mapping_name;
  field->value = value;
  field->flags = flags;
  return 1;
}

static nanopdf_status walk_form_field_tree(
    nanopdf_context* context,
    const char* data,
    size_t size,
    const nanopdf_basic_xref_entry* xrefs,
    size_t xref_count,
    nanopdf_ref field_ref,
    const char* parent_full_name,
    int inherited_has_type,
    nanopdf_field_type inherited_type,
    uint32_t inherited_flags,
    nanopdf_basic_document* document) {
  const char* object = NULL;
  size_t object_length = 0;
  size_t value_start = 0;
  size_t value_end = 0;
  nanopdf_ref* kids = NULL;
  size_t kid_count = 0;
  nanopdf_field_type current_type = inherited_type;
  int has_current_type = inherited_has_type;
  uint32_t current_flags = inherited_flags;
  char buffer[1024];
  char* partial_name = NULL;
  char* full_name = NULL;
  char* alternate_name = NULL;
  char* mapping_name = NULL;
  char* value = NULL;
  char subtype_name[32];
  int has_value = 0;
  int has_field_identity = 0;
  int should_append = 0;
  int is_widget = 0;
  const char* next_parent_full_name = parent_full_name;
  size_t i;

  if (!field_ref.valid || field_ref.object_number >= xref_count ||
      !xrefs[field_ref.object_number].in_use) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "invalid form field reference");
  }
  if (!find_object_slice(
          data, size, xrefs[field_ref.object_number].offset, &object, &object_length)) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "failed to parse form field object");
  }

  subtype_name[0] = '\0';
  if (find_top_level_key_value(object, object_length, "Subtype", &value_start, &value_end) &&
      parse_name_value(object, value_start, value_end, subtype_name, sizeof(subtype_name)) &&
      strcmp(subtype_name, "Widget") == 0) {
    is_widget = 1;
  }

  if (parse_field_type(object, object_length, &current_type)) {
    has_current_type = 1;
    has_field_identity = 1;
  }
  if (find_top_level_key_value(object, object_length, "Ff", &value_start, &value_end)) {
    int parsed_flags = 0;
    if (parse_int_value(object, value_start, value_end, &parsed_flags)) {
      current_flags = (uint32_t)parsed_flags;
      has_field_identity = 1;
    }
  }
  if (find_top_level_key_value(object, object_length, "T", &value_start, &value_end) &&
      parse_info_string(object, value_start, value_end, buffer, sizeof(buffer))) {
    partial_name = nanopdf__strdup(&context->allocator, buffer);
    if (!partial_name) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store partial field name");
    }
    has_field_identity = 1;
  }
  full_name = duplicate_joined_name(context, parent_full_name, partial_name);
  if (!full_name) {
    nanopdf__allocator_free(&context->allocator, partial_name);
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store full field name");
  }
  if (find_top_level_key_value(object, object_length, "TU", &value_start, &value_end) &&
      parse_info_string(object, value_start, value_end, buffer, sizeof(buffer))) {
    alternate_name = nanopdf__strdup(&context->allocator, buffer);
    if (!alternate_name) {
      destroy_basic_form_field(
          &context->allocator,
          &(nanopdf_basic_form_field){0, partial_name, full_name, NULL, NULL, NULL, 0});
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store alternate field name");
    }
    has_field_identity = 1;
  }
  if (find_top_level_key_value(object, object_length, "TM", &value_start, &value_end) &&
      parse_info_string(object, value_start, value_end, buffer, sizeof(buffer))) {
    mapping_name = nanopdf__strdup(&context->allocator, buffer);
    if (!mapping_name) {
      destroy_basic_form_field(
          &context->allocator,
          &(nanopdf_basic_form_field){0, partial_name, full_name, alternate_name, NULL, NULL, 0});
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store mapping field name");
    }
    has_field_identity = 1;
  }
  if (find_top_level_key_value(object, object_length, "V", &value_start, &value_end)) {
    if (parse_form_value(object, value_start, value_end, context, &value)) {
      has_value = 1;
      has_field_identity = 1;
    }
  }

  should_append = has_current_type && (has_field_identity || !is_widget);
  next_parent_full_name = full_name ? full_name : parent_full_name;
  if (should_append) {
    if (!append_form_field(
            context,
            document,
            current_type,
            partial_name,
            full_name,
            alternate_name,
            mapping_name,
            value,
            current_flags)) {
      destroy_basic_form_field(
          &context->allocator,
          &(nanopdf_basic_form_field){
              current_type,
              partial_name,
              full_name,
              alternate_name,
              mapping_name,
              value,
              current_flags});
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to append form field");
    }
    partial_name = NULL;
    full_name = NULL;
    alternate_name = NULL;
    mapping_name = NULL;
    value = NULL;
  }

  if (find_top_level_key_value(object, object_length, "Kids", &value_start, &value_end)) {
    int result = parse_contents_value(
        object, value_start, value_end, &kids, &kid_count, context);
    if (result < 0) {
      nanopdf__allocator_free(&context->allocator, kids);
      destroy_basic_form_field(
          &context->allocator,
          &(nanopdf_basic_form_field){
              current_type,
              partial_name,
              full_name,
              alternate_name,
              mapping_name,
              value,
              current_flags});
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to parse form field kids");
    }
  }

  for (i = 0; i < kid_count; ++i) {
    nanopdf_status status = walk_form_field_tree(
        context,
        data,
        size,
        xrefs,
        xref_count,
        kids[i],
        next_parent_full_name,
        has_current_type,
        current_type,
        current_flags,
        document);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, kids);
      destroy_basic_form_field(
          &context->allocator,
          &(nanopdf_basic_form_field){
              current_type,
              partial_name,
              full_name,
              alternate_name,
              mapping_name,
              value,
              current_flags});
      return status;
    }
  }

  nanopdf__allocator_free(&context->allocator, kids);
  destroy_basic_form_field(
      &context->allocator,
      &(nanopdf_basic_form_field){
          current_type,
          partial_name,
          full_name,
          alternate_name,
          mapping_name,
          value,
          current_flags});
  if (has_value) {
    clear_status(context);
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_status NANOPDF_MAYBE_UNUSED parse_acro_form(
    nanopdf_context* context,
    const char* data,
    size_t size,
    const nanopdf_basic_xref_entry* xrefs,
    size_t xref_count,
    const char* root_object,
    size_t root_length,
    nanopdf_basic_document* document) {
  size_t value_start = 0;
  size_t value_end = 0;
  nanopdf_ref acro_form_ref = {0, 0, 0};
  const char* acro_form_object = NULL;
  size_t acro_form_length = 0;
  nanopdf_ref* field_refs = NULL;
  size_t field_count = 0;
  size_t i;

  document->forms_parsed = 1;
  if (!find_top_level_key_value(root_object, root_length, "AcroForm", &value_start, &value_end) ||
      !parse_ref_value(root_object, value_start, value_end, &acro_form_ref)) {
    return NANOPDF_STATUS_OK;
  }
  if (!acro_form_ref.valid || acro_form_ref.object_number >= xref_count ||
      !xrefs[acro_form_ref.object_number].in_use) {
    document->forms_parsed = 0;
    return NANOPDF_STATUS_OK;
  }
  if (!find_object_slice(
          data,
          size,
          xrefs[acro_form_ref.object_number].offset,
          &acro_form_object,
          &acro_form_length)) {
    document->forms_parsed = 0;
    return NANOPDF_STATUS_OK;
  }
  if (!find_top_level_key_value(
          acro_form_object, acro_form_length, "Fields", &value_start, &value_end)) {
    return NANOPDF_STATUS_OK;
  }
  {
    int result = parse_contents_value(
        acro_form_object, value_start, value_end, &field_refs, &field_count, context);
    if (result < 0) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to parse AcroForm field refs");
    }
    if (result == 0) {
      document->forms_parsed = 0;
      return NANOPDF_STATUS_OK;
    }
  }

  for (i = 0; i < field_count; ++i) {
    nanopdf_status status = walk_form_field_tree(
        context,
        data,
        size,
        xrefs,
        xref_count,
        field_refs[i],
        "",
        0,
        NANOPDF_FIELD_TYPE_TEXT,
        0u,
        document);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, field_refs);
      return status;
    }
  }

  nanopdf__allocator_free(&context->allocator, field_refs);
  return NANOPDF_STATUS_OK;
}

static nanopdf_status NANOPDF_MAYBE_UNUSED walk_page_tree(
    nanopdf_context* context,
    const char* data,
    size_t size,
    const nanopdf_basic_xref_entry* xrefs,
    size_t xref_count,
    nanopdf_ref node_ref,
    const nanopdf_box* inherited_media_box,
    double inherited_rotate,
    nanopdf_basic_document* document) {
  const char* object = NULL;
  size_t object_length = 0;
  size_t value_start = 0;
  size_t value_end = 0;
  char type_name[32];
  nanopdf_box media_box = *inherited_media_box;
  double rotate = inherited_rotate;
  nanopdf_ref* content_refs = NULL;
  size_t content_count = 0;
  int has_kids = 0;

  if (!node_ref.valid || node_ref.object_number >= xref_count ||
      !xrefs[node_ref.object_number].in_use) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "invalid page tree reference");
  }

  if (!find_object_slice(
          data, size, xrefs[node_ref.object_number].offset, &object, &object_length)) {
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "failed to load page tree object");
  }

  if (find_top_level_key_value(object, object_length, "MediaBox", &value_start, &value_end)) {
    parse_box_value(object, value_start, value_end, &media_box);
  }
  if (find_top_level_key_value(object, object_length, "Rotate", &value_start, &value_end)) {
    int rotate_value = 0;
    if (parse_int_value(object, value_start, value_end, &rotate_value)) {
      rotate = (double)rotate_value;
    }
  }
  if (find_top_level_key_value(object, object_length, "Contents", &value_start, &value_end)) {
    int content_result = parse_contents_value(
        object, value_start, value_end, &content_refs, &content_count, context);
    if (content_result < 0) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to parse page contents");
    }
  }

  type_name[0] = '\0';
  if (find_top_level_key_value(object, object_length, "Type", &value_start, &value_end)) {
    parse_name_value(object, value_start, value_end, type_name, sizeof(type_name));
  }

  if (find_top_level_key_value(object, object_length, "Kids", &value_start, &value_end)) {
    size_t pos = value_start;
    has_kids = 1;
    skip_ws(object, value_end, &pos);
    if (pos >= value_end || object[pos] != '[') {
      return set_status(
          context, NANOPDF_STATUS_MALFORMED, "invalid Kids array");
    }
    pos++;
    while (pos < value_end) {
      nanopdf_ref child = {0, 0, 0};
      skip_ws(object, value_end, &pos);
      if (pos < value_end && object[pos] == ']') {
        break;
      }
      if (!parse_ref_value(object, pos, value_end, &child)) {
        break;
      }
      while (pos < value_end && object[pos] != 'R') pos++;
      if (pos < value_end && object[pos] == 'R') pos++;
      {
        nanopdf_status status = walk_page_tree(
            context, data, size, xrefs, xref_count, child, &media_box, rotate, document);
        if (status != NANOPDF_STATUS_OK) {
          nanopdf__allocator_free(&context->allocator, content_refs);
          return status;
        }
      }
    }
  }

  if (!has_kids || strcmp(type_name, "Page") == 0) {
    nanopdf_status status = append_page(
        context, document, &media_box, rotate, content_refs, content_count);
    nanopdf__allocator_free(&context->allocator, content_refs);
    return status;
  }

  nanopdf__allocator_free(&context->allocator, content_refs);

  return NANOPDF_STATUS_OK;
}

static nanopdf_status parse_classic_xref(
    nanopdf_context* context,
    const char* data,
    size_t size,
    size_t xref_offset,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt,
    size_t* out_xrefstm_offset,
    int* out_has_xrefstm,
    size_t* out_prev_offset,
    int* out_has_prev) {
  size_t pos = xref_offset;
  nanopdf_basic_xref_entry* xrefs = NULL;
  size_t xref_capacity = 0;
  size_t xref_count = 0;

  if (!match_literal(data, size, pos, "xref")) {
    return set_status(
        context, NANOPDF_STATUS_UNSUPPORTED, "C11 parser currently supports classic xref tables only");
  }
  pos += 4;

  while (pos < size) {
    uint32_t subsection_start = 0;
    uint32_t subsection_count = 0;
    size_t i;

    skip_ws(data, size, &pos);
    if (match_literal(data, size, pos, "trailer")) {
      break;
    }
    if (!parse_unsigned_value(data, size, &pos, &subsection_start)) {
      return set_status(
          context, NANOPDF_STATUS_MALFORMED, "invalid xref subsection start");
    }
    skip_ws(data, size, &pos);
    if (!parse_unsigned_value(data, size, &pos, &subsection_count)) {
      return set_status(
          context, NANOPDF_STATUS_MALFORMED, "invalid xref subsection count");
    }

    if (ensure_xref_capacity(
            context, &xrefs, &xref_capacity, (size_t)subsection_start + subsection_count) !=
        NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, xrefs);
      return context->last_status;
    }
    if (xref_count < (size_t)subsection_start + subsection_count) {
      xref_count = (size_t)subsection_start + subsection_count;
    }

    for (i = 0; i < subsection_count; ++i) {
      uint32_t offset_value = 0;
      uint32_t generation = 0;
      skip_ws(data, size, &pos);
      if (!parse_unsigned_value(data, size, &pos, &offset_value)) {
        nanopdf__allocator_free(&context->allocator, xrefs);
        return set_status(
            context, NANOPDF_STATUS_MALFORMED, "invalid xref entry offset");
      }
      skip_ws(data, size, &pos);
      if (!parse_unsigned_value(data, size, &pos, &generation)) {
        nanopdf__allocator_free(&context->allocator, xrefs);
        return set_status(
            context, NANOPDF_STATUS_MALFORMED, "invalid xref entry generation");
      }
      skip_ws(data, size, &pos);
      if (pos >= size || (data[pos] != 'n' && data[pos] != 'f')) {
        nanopdf__allocator_free(&context->allocator, xrefs);
        return set_status(
            context, NANOPDF_STATUS_MALFORMED, "invalid xref entry flag");
      }
      xrefs[subsection_start + i].offset = (size_t)offset_value;
      xrefs[subsection_start + i].generation = (uint16_t)generation;
      xrefs[subsection_start + i].present = 1;
      xrefs[subsection_start + i].in_use = (uint8_t)(data[pos] == 'n');
      pos++;
    }
  }

  if (!match_literal(data, size, pos, "trailer")) {
    nanopdf__allocator_free(&context->allocator, xrefs);
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "missing trailer after xref");
  }
  pos += 7;
  skip_ws(data, size, &pos);
  if (pos + 1 >= size || data[pos] != '<' || data[pos + 1] != '<') {
    nanopdf__allocator_free(&context->allocator, xrefs);
    return set_status(
        context, NANOPDF_STATUS_MALFORMED, "invalid trailer dictionary");
  }

  {
    size_t trailer_end = 0;
    size_t value_start = 0;
    size_t value_end = 0;
    const char* trailer = data + pos;
    size_t trailer_length = 0;
    if (!find_matching_dict_end(data, size, pos, &trailer_end)) {
      nanopdf__allocator_free(&context->allocator, xrefs);
      return set_status(
          context, NANOPDF_STATUS_MALFORMED, "unterminated trailer dictionary");
    }
    trailer_length = trailer_end - pos;
    if (find_top_level_key_value(trailer, trailer_length, "Root", &value_start, &value_end)) {
      parse_ref_value(trailer, value_start, value_end, out_root_ref);
    }
    if (find_top_level_key_value(trailer, trailer_length, "Info", &value_start, &value_end)) {
      parse_ref_value(trailer, value_start, value_end, out_info_ref);
    }
    if (find_top_level_key_value(trailer, trailer_length, "Encrypt", &value_start, &value_end)) {
      if (out_has_encrypt) {
        *out_has_encrypt = 1;
      }
    }
    if (find_top_level_key_value(trailer, trailer_length, "XRefStm", &value_start, &value_end)) {
      uint32_t xrefstm_value = 0;
      size_t xrefstm_pos = value_start;
      skip_ws(trailer, trailer_length, &xrefstm_pos);
      if (parse_unsigned_value(trailer, trailer_length, &xrefstm_pos, &xrefstm_value)) {
        if (out_xrefstm_offset) {
          *out_xrefstm_offset = (size_t)xrefstm_value;
        }
        if (out_has_xrefstm) {
          *out_has_xrefstm = 1;
        }
      }
    }
    if (find_top_level_key_value(trailer, trailer_length, "Prev", &value_start, &value_end)) {
      uint32_t prev_value = 0;
      size_t prev_pos = value_start;
      skip_ws(trailer, trailer_length, &prev_pos);
      if (parse_unsigned_value(trailer, trailer_length, &prev_pos, &prev_value)) {
        if (out_prev_offset) {
          *out_prev_offset = (size_t)prev_value;
        }
        if (out_has_prev) {
          *out_has_prev = 1;
        }
      }
    }
  }

  *out_xrefs = xrefs;
  *out_xref_count = xref_count;
  return NANOPDF_STATUS_OK;
}

static int basic_object_to_ref(
    const nanopdf_basic_object* object,
    nanopdf_ref* out_ref) {
  if (!object || !out_ref || object->type != NANOPDF_BASIC_OBJECT_REF) {
    return 0;
  }

  out_ref->object_number = object->as.ref.object_number;
  out_ref->generation = object->as.ref.generation;
  out_ref->valid = object->as.ref.valid;
  return out_ref->valid != 0;
}

static int basic_object_to_box(
    const nanopdf_basic_object* object,
    nanopdf_box* out_box) {
  size_t i = 0;
  if (!object || !out_box || object->type != NANOPDF_BASIC_OBJECT_ARRAY ||
      object->as.array.count < 4) {
    return 0;
  }
  memset(out_box, 0, sizeof(*out_box));
  for (i = 0; i < 4; ++i) {
    if (object->as.array.items[i].type != NANOPDF_BASIC_OBJECT_NUMBER) {
      return 0;
    }
    out_box->values[i] = object->as.array.items[i].as.number;
  }
  out_box->valid = 1;
  return 1;
}

static int duplicate_object_text(
    nanopdf_context* context,
    const nanopdf_basic_object* object,
    char** out_text) {
  size_t i = 0;
  char* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;

  if (!context || !object || !out_text) {
    return 0;
  }

  *out_text = NULL;
  if (object->type == NANOPDF_BASIC_OBJECT_STRING) {
    return decode_pdf_string_buffer(context, object->as.text, object->length, 1, out_text);
  }
  if (object->type == NANOPDF_BASIC_OBJECT_NAME) {
    *out_text = (char*)nanopdf__allocator_alloc(&context->allocator, object->length + 1);
    if (!*out_text) {
      return 0;
    }
    memcpy(*out_text, object->as.text, object->length);
    (*out_text)[object->length] = '\0';
    return *out_text != NULL;
  }
  if (object->type != NANOPDF_BASIC_OBJECT_ARRAY) {
    return 0;
  }

  for (i = 0; i < object->as.array.count; ++i) {
    const nanopdf_basic_object* item = &object->as.array.items[i];
    if (item->type != NANOPDF_BASIC_OBJECT_STRING &&
        item->type != NANOPDF_BASIC_OBJECT_NAME) {
      continue;
    }
    if (buffer) {
      if (!append_output_char(context, &buffer, &length, &capacity, ',')) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
    }
    if (item->type == NANOPDF_BASIC_OBJECT_STRING) {
      char* decoded = NULL;
      if (!decode_pdf_string_buffer(context, item->as.text, item->length, 1, &decoded)) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
      if (!append_output_text(context, &buffer, &length, &capacity, decoded)) {
        nanopdf__allocator_free(&context->allocator, decoded);
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
      nanopdf__allocator_free(&context->allocator, decoded);
      continue;
    }
    if (!append_output_text(context, &buffer, &length, &capacity, item->as.text)) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return 0;
    }
  }

  if (!buffer) {
    buffer = nanopdf__strdup(&context->allocator, "");
  }
  *out_text = buffer;
  return *out_text != NULL;
}

static nanopdf_status content_refs_from_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    nanopdf_basic_content_ref** out_refs,
    size_t* out_count) {
  nanopdf_basic_content_ref* refs = NULL;
  size_t count = 0;
  size_t i = 0;

  if (!context || !object || !out_refs || !out_count) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid content refs arguments");
  }

  *out_refs = NULL;
  *out_count = 0;
  if (object->type == NANOPDF_BASIC_OBJECT_REF) {
    refs = (nanopdf_basic_content_ref*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(*refs));
    if (!refs) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate content refs");
    }
    memset(refs, 0, sizeof(*refs));
    refs[0].kind = 0;
    refs[0].object_number = object->as.ref.object_number;
    refs[0].generation = object->as.ref.generation;
    refs[0].valid = object->as.ref.valid;
    *out_refs = refs;
    *out_count = 1;
    return clear_status(context);
  }
  if (object->type == NANOPDF_BASIC_OBJECT_STREAM) {
    nanopdf_status status = NANOPDF_STATUS_OK;
    refs = (nanopdf_basic_content_ref*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(*refs));
    if (!refs) {
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate content stream");
    }
    memset(refs, 0, sizeof(*refs));
    status = nanopdf_basic_decode_stream_with_document(
        context, document, object, &refs[0].decoded_data, &refs[0].decoded_size);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, refs);
      return status;
    }
    refs[0].kind = 1;
    refs[0].valid = 1;
    *out_refs = refs;
    *out_count = 1;
    return clear_status(context);
  }
  if (object->type != NANOPDF_BASIC_OBJECT_ARRAY) {
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "unsupported contents value");
  }

  refs = (nanopdf_basic_content_ref*)nanopdf__allocator_alloc(
      &context->allocator, object->as.array.count * sizeof(*refs));
  if (!refs) {
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate content refs");
  }
  memset(refs, 0, object->as.array.count * sizeof(*refs));

  for (i = 0; i < object->as.array.count; ++i) {
    if (object->as.array.items[i].type == NANOPDF_BASIC_OBJECT_REF) {
      refs[count].kind = 0;
      refs[count].object_number = object->as.array.items[i].as.ref.object_number;
      refs[count].generation = object->as.array.items[i].as.ref.generation;
      refs[count].valid = object->as.array.items[i].as.ref.valid;
      count++;
      continue;
    }
    if (object->as.array.items[i].type == NANOPDF_BASIC_OBJECT_STREAM) {
      nanopdf_status status = nanopdf_basic_decode_stream_with_document(
          context,
          document,
          &object->as.array.items[i],
          &refs[count].decoded_data,
          &refs[count].decoded_size);
      if (status != NANOPDF_STATUS_OK) {
        size_t j;
        for (j = 0; j < count; ++j) {
          destroy_basic_content(&context->allocator, &refs[j]);
        }
        nanopdf__allocator_free(&context->allocator, refs);
        return status;
      }
      refs[count].kind = 1;
      refs[count].valid = 1;
      count++;
    }
  }

  if (count == 0) {
    nanopdf__allocator_free(&context->allocator, refs);
    return set_status(context, NANOPDF_STATUS_UNSUPPORTED, "contents array has no refs");
  }

  *out_refs = refs;
  *out_count = count;
  return clear_status(context);
}

static const nanopdf_basic_object* resolve_object_reference(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    nanopdf_basic_object* resolved_storage,
    nanopdf_status* out_status) {
  if (out_status) {
    *out_status = NANOPDF_STATUS_OK;
  }
  if (!object) {
    return NULL;
  }
  if (object->type != NANOPDF_BASIC_OBJECT_REF) {
    return object;
  }
  if (!document || !object->as.ref.valid || !resolved_storage) {
    if (out_status) {
      *out_status = set_status(
          context, NANOPDF_STATUS_MALFORMED, "invalid indirect object reference");
    }
    return NULL;
  }

  nanopdf_basic_object_init(resolved_storage);
  if (out_status) {
    *out_status = nanopdf_basic_load_object(
        context, document, object->as.ref, resolved_storage);
    if (*out_status != NANOPDF_STATUS_OK) {
      return NULL;
    }
  } else {
    nanopdf_status status = nanopdf_basic_load_object(
        context, document, object->as.ref, resolved_storage);
    if (status != NANOPDF_STATUS_OK) {
      return NULL;
    }
  }
  return resolved_storage;
}

static const nanopdf_basic_object* dict_get_resolved(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_dict* dict,
    const char* key,
    nanopdf_basic_object* resolved_storage,
    nanopdf_status* out_status) {
  const nanopdf_basic_object* object = nanopdf_basic_dict_get(dict, key);
  if (!object) {
    if (out_status) {
      *out_status = NANOPDF_STATUS_OK;
    }
    return NULL;
  }
  return resolve_object_reference(
      context, document, object, resolved_storage, out_status);
}

static nanopdf_status walk_page_tree_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_ref node_ref,
    const nanopdf_box* inherited_media_box,
    double inherited_rotate,
    nanopdf_basic_document* out_document) {
  nanopdf_basic_object node_object;
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* type_obj = NULL;
  const nanopdf_basic_object* media_box_obj = NULL;
  const nanopdf_basic_object* rotate_obj = NULL;
  const nanopdf_basic_object* contents_obj = NULL;
  const nanopdf_basic_object* kids_obj = NULL;
  nanopdf_box media_box = *inherited_media_box;
  double rotate = inherited_rotate;
  nanopdf_basic_content_ref* content_refs = NULL;
  nanopdf_basic_object resolved_type_obj;
  nanopdf_basic_object resolved_media_box_obj;
  nanopdf_basic_object resolved_rotate_obj;
  nanopdf_basic_object resolved_contents_obj;
  nanopdf_basic_object resolved_kids_obj;
  size_t content_count = 0;
  int has_kids = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;
  size_t i = 0;

  nanopdf_basic_object_init(&node_object);
  nanopdf_basic_object_init(&resolved_type_obj);
  nanopdf_basic_object_init(&resolved_media_box_obj);
  nanopdf_basic_object_init(&resolved_rotate_obj);
  nanopdf_basic_object_init(&resolved_contents_obj);
  nanopdf_basic_object_init(&resolved_kids_obj);
  status = nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){node_ref.object_number, node_ref.generation, node_ref.valid},
      &node_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  dict = nanopdf_basic_object_as_dict(&node_object);
  if (!dict) {
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "page tree node is not a dictionary");
  }

  media_box_obj = dict_get_resolved(
      context,
      document,
      dict,
      "MediaBox",
      &resolved_media_box_obj,
      &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }
  if (media_box_obj) {
    basic_object_to_box(media_box_obj, &media_box);
  }
  rotate_obj = dict_get_resolved(
      context,
      document,
      dict,
      "Rotate",
      &resolved_rotate_obj,
      &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }
  if (rotate_obj && rotate_obj->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    rotate = rotate_obj->as.number;
  }
  contents_obj = dict_get_resolved(
      context,
      document,
      dict,
      "Contents",
      &resolved_contents_obj,
      &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }
  if (contents_obj) {
    status = content_refs_from_object(
        context, document, contents_obj, &content_refs, &content_count);
    if (status != NANOPDF_STATUS_OK &&
        status != NANOPDF_STATUS_UNSUPPORTED) {
      nanopdf_basic_object_destroy(&context->allocator, &node_object);
      return status;
    }
    if (status == NANOPDF_STATUS_UNSUPPORTED) {
      clear_status(context);
      status = NANOPDF_STATUS_OK;
    }
  }

  type_obj = dict_get_resolved(
      context, document, dict, "Type", &resolved_type_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }
  kids_obj = dict_get_resolved(
      context, document, dict, "Kids", &resolved_kids_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }
  if (kids_obj && kids_obj->type == NANOPDF_BASIC_OBJECT_ARRAY) {
    has_kids = 1;
    for (i = 0; i < kids_obj->as.array.count; ++i) {
      nanopdf_ref child = {0, 0, 0};
      if (!basic_object_to_ref(&kids_obj->as.array.items[i], &child)) {
        continue;
      }
      status = walk_page_tree_object(
          context, document, child, &media_box, rotate, out_document);
      if (status != NANOPDF_STATUS_OK) {
        size_t j;
        for (j = 0; j < content_count; ++j) {
          destroy_basic_content(&context->allocator, &content_refs[j]);
        }
        nanopdf__allocator_free(&context->allocator, content_refs);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
        nanopdf_basic_object_destroy(&context->allocator, &node_object);
        return status;
      }
    }
  }

  if (!has_kids ||
      (type_obj && type_obj->type == NANOPDF_BASIC_OBJECT_NAME &&
       strcmp(type_obj->as.text, "Page") == 0)) {
    status = append_page_contents(
        context, out_document, node_ref, &media_box, rotate, content_refs, content_count);
    if (content_refs) {
      size_t j;
      for (j = 0; j < content_count; ++j) {
        destroy_basic_content(&context->allocator, &content_refs[j]);
      }
      nanopdf__allocator_free(&context->allocator, content_refs);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
    nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
    nanopdf_basic_object_destroy(&context->allocator, &node_object);
    return status;
  }

  if (content_refs) {
    size_t j;
    for (j = 0; j < content_count; ++j) {
      destroy_basic_content(&context->allocator, &content_refs[j]);
    }
    nanopdf__allocator_free(&context->allocator, content_refs);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_type_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_media_box_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_rotate_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_contents_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
  nanopdf_basic_object_destroy(&context->allocator, &node_object);
  return clear_status(context);
}

static int is_standard_document_info_key(const char* key) {
  return key &&
      (strcmp(key, "Title") == 0 || strcmp(key, "Author") == 0 ||
       strcmp(key, "Subject") == 0 || strcmp(key, "Keywords") == 0 ||
       strcmp(key, "Creator") == 0 || strcmp(key, "Producer") == 0 ||
       strcmp(key, "CreationDate") == 0 || strcmp(key, "ModDate") == 0 ||
       strcmp(key, "Trapped") == 0);
}

static int append_custom_document_info(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    const char* key,
    const nanopdf_basic_object* value) {
  nanopdf_basic_info_entry* resized = NULL;
  char* key_copy = NULL;
  char* value_copy = NULL;
  size_t new_capacity = 0;
  nanopdf_basic_info_entry* entry = NULL;

  if (!context || !document || !key || !value) {
    return 0;
  }
  if (document->custom_info_count == document->custom_info_capacity) {
    new_capacity =
        document->custom_info_capacity == 0 ? 4 : document->custom_info_capacity * 2;
    resized = (nanopdf_basic_info_entry*)nanopdf__allocator_realloc(
        &context->allocator,
        document->custom_info,
        new_capacity * sizeof(*document->custom_info));
    if (!resized) {
      return 0;
    }
    memset(
        resized + document->custom_info_capacity,
        0,
        (new_capacity - document->custom_info_capacity) * sizeof(*document->custom_info));
    document->custom_info = resized;
    document->custom_info_capacity = new_capacity;
  }

  key_copy = (char*)nanopdf__allocator_alloc(&context->allocator, strlen(key) + 1);
  if (!key_copy) {
    return 0;
  }
  memcpy(key_copy, key, strlen(key) + 1);
  if (!duplicate_object_text(context, value, &value_copy)) {
    nanopdf__allocator_free(&context->allocator, key_copy);
    return 0;
  }

  entry = &document->custom_info[document->custom_info_count++];
  entry->key = key_copy;
  entry->value = value_copy;
  return 1;
}

static nanopdf_status parse_document_info_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_ref info_ref,
    nanopdf_basic_document* out_document) {
  static const char* keys[] = {
      "Title", "Author", "Subject", "Keywords",
      "Creator", "Producer", "CreationDate", "ModDate", "Trapped"};
  char** targets[] = {
      &out_document->title, &out_document->author, &out_document->subject,
      &out_document->keywords, &out_document->creator, &out_document->producer,
      &out_document->creation_date, &out_document->mod_date, &out_document->trapped};
  nanopdf_basic_object info_object;
  const nanopdf_basic_dict* dict = NULL;
  size_t i = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;

  nanopdf_basic_object_init(&info_object);
  status = nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){info_ref.object_number, info_ref.generation, info_ref.valid},
      &info_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  dict = nanopdf_basic_object_as_dict(&info_object);
  if (!dict) {
    nanopdf_basic_object_destroy(&context->allocator, &info_object);
    return clear_status(context);
  }

  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    nanopdf_basic_object resolved_value;
    const nanopdf_basic_object* value = NULL;
    nanopdf_basic_object_init(&resolved_value);
    value = dict_get_resolved(
        context, document, dict, keys[i], &resolved_value, &status);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      nanopdf_basic_object_destroy(&context->allocator, &info_object);
      return status;
    }
    if (!value) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      continue;
    }
    if (!duplicate_object_text(context, value, targets[i])) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      nanopdf_basic_object_destroy(&context->allocator, &info_object);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store document info");
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
  }

  for (i = 0; i < dict->count; ++i) {
    nanopdf_basic_object resolved_value;
    const nanopdf_basic_object* value = NULL;
    const char* key = dict->entries[i].key;

    if (!key || is_standard_document_info_key(key)) {
      continue;
    }

    nanopdf_basic_object_init(&resolved_value);
    value = dict_get_resolved(
        context, document, dict, key, &resolved_value, &status);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      nanopdf_basic_object_destroy(&context->allocator, &info_object);
      return status;
    }
    if (value &&
        value->type != NANOPDF_BASIC_OBJECT_STRING &&
        value->type != NANOPDF_BASIC_OBJECT_NAME &&
        value->type != NANOPDF_BASIC_OBJECT_ARRAY) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      continue;
    }
    if (value &&
        !append_custom_document_info(context, out_document, key, value)) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
      nanopdf_basic_object_destroy(&context->allocator, &info_object);
      return set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store custom document info");
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_value);
  }

  nanopdf_basic_object_destroy(&context->allocator, &info_object);
  return clear_status(context);
}

static int map_catalog_page_layout(
    const nanopdf_basic_object* object,
    nanopdf_page_layout* out_layout) {
  if (!object || !out_layout || object->type != NANOPDF_BASIC_OBJECT_NAME) {
    return 0;
  }
  if (strcmp(object->as.text, "SinglePage") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_SINGLE_PAGE;
    return 1;
  }
  if (strcmp(object->as.text, "OneColumn") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_ONE_COLUMN;
    return 1;
  }
  if (strcmp(object->as.text, "TwoColumnLeft") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_TWO_COLUMN_LEFT;
    return 1;
  }
  if (strcmp(object->as.text, "TwoColumnRight") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_TWO_COLUMN_RIGHT;
    return 1;
  }
  if (strcmp(object->as.text, "TwoPageLeft") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_TWO_PAGE_LEFT;
    return 1;
  }
  if (strcmp(object->as.text, "TwoPageRight") == 0) {
    *out_layout = NANOPDF_PAGE_LAYOUT_TWO_PAGE_RIGHT;
    return 1;
  }
  return 0;
}

static int map_catalog_page_mode(
    const nanopdf_basic_object* object,
    nanopdf_page_mode* out_mode) {
  if (!object || !out_mode || object->type != NANOPDF_BASIC_OBJECT_NAME) {
    return 0;
  }
  if (strcmp(object->as.text, "UseNone") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_USE_NONE;
    return 1;
  }
  if (strcmp(object->as.text, "UseOutlines") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_USE_OUTLINES;
    return 1;
  }
  if (strcmp(object->as.text, "UseThumbs") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_USE_THUMBS;
    return 1;
  }
  if (strcmp(object->as.text, "FullScreen") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_FULL_SCREEN;
    return 1;
  }
  if (strcmp(object->as.text, "UseOC") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_USE_OC;
    return 1;
  }
  if (strcmp(object->as.text, "UseAttachments") == 0) {
    *out_mode = NANOPDF_PAGE_MODE_USE_ATTACHMENTS;
    return 1;
  }
  return 0;
}

static int duplicate_stream_bytes_as_text(
    nanopdf_context* context,
    const nanopdf_basic_object* object,
    char** out_text) {
  char* buffer = NULL;

  if (!context || !object || !out_text ||
      object->type != NANOPDF_BASIC_OBJECT_STREAM) {
    return 0;
  }

  buffer = (char*)nanopdf__allocator_alloc(
      &context->allocator, object->as.stream.size + 1);
  if (!buffer) {
    return 0;
  }
  if (object->as.stream.size > 0) {
    memcpy(buffer, object->as.stream.data, object->as.stream.size);
  }
  buffer[object->as.stream.size] = '\0';
  *out_text = buffer;
  return 1;
}

static int duplicate_stream_bytes(
    nanopdf_context* context,
    const nanopdf_basic_object* object,
    uint8_t** out_data,
    size_t* out_size) {
  uint8_t* buffer = NULL;

  if (!context || !object || !out_data || !out_size ||
      object->type != NANOPDF_BASIC_OBJECT_STREAM) {
    return 0;
  }

  *out_data = NULL;
  *out_size = 0;
  if (object->as.stream.size == 0) {
    return 1;
  }

  buffer = (uint8_t*)nanopdf__allocator_alloc(
      &context->allocator, object->as.stream.size);
  if (!buffer) {
    return 0;
  }
  memcpy(buffer, object->as.stream.data, object->as.stream.size);
  *out_data = buffer;
  *out_size = object->as.stream.size;
  return 1;
}

static int append_output_intent(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    const nanopdf_basic_dict* dict,
    const nanopdf_basic_document* source_document) {
  nanopdf_basic_output_intent* resized = NULL;
  size_t new_capacity = 0;
  nanopdf_basic_output_intent* entry = NULL;
  nanopdf_basic_object resolved_obj;
  const nanopdf_basic_object* value = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!context || !document || !dict || !source_document) {
    return 0;
  }
  if (document->output_intent_count == document->output_intent_capacity) {
    new_capacity = document->output_intent_capacity == 0 ?
        2 : document->output_intent_capacity * 2;
    resized = (nanopdf_basic_output_intent*)nanopdf__allocator_realloc(
        &context->allocator,
        document->output_intents,
        new_capacity * sizeof(*document->output_intents));
    if (!resized) {
      return 0;
    }
    memset(
        resized + document->output_intent_capacity,
        0,
        (new_capacity - document->output_intent_capacity) * sizeof(*document->output_intents));
    document->output_intents = resized;
    document->output_intent_capacity = new_capacity;
  }

  entry = &document->output_intents[document->output_intent_count];
  memset(entry, 0, sizeof(*entry));

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "S", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return 0;
  }
  if (value && !duplicate_object_text(context, value, &entry->subtype)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "OutputCondition", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value && !duplicate_object_text(context, value, &entry->output_condition)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "OutputConditionIdentifier", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value &&
      !duplicate_object_text(context, value, &entry->output_condition_identifier)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "RegistryName", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value && !duplicate_object_text(context, value, &entry->registry_name)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "Info", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value && !duplicate_object_text(context, value, &entry->info)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, dict, "DestOutputProfile", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_STREAM &&
      !duplicate_stream_bytes(
          context,
          value,
          &entry->dest_output_profile_data,
          &entry->dest_output_profile_size)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_output_intent(&context->allocator, entry);
    return 0;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_STREAM) {
    nanopdf_basic_object resolved_profile_n;
    const nanopdf_basic_object* profile_n = NULL;

    nanopdf_basic_object_init(&resolved_profile_n);
    profile_n = dict_get_resolved(
        context,
        source_document,
        &value->as.stream.dict,
        "N",
        &resolved_profile_n,
        &status);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_profile_n);
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      destroy_basic_output_intent(&context->allocator, entry);
      return 0;
    }
    if (profile_n && profile_n->type == NANOPDF_BASIC_OBJECT_NUMBER) {
      entry->color_components = (int32_t)profile_n->as.number;
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_profile_n);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  document->output_intent_count++;
  return 1;
}

static int map_page_label_style(
    const nanopdf_basic_object* object,
    nanopdf_page_label_style* out_style) {
  if (!object || !out_style) {
    return 0;
  }
  if (object->type != NANOPDF_BASIC_OBJECT_NAME) {
    return 0;
  }
  if (strcmp(object->as.text, "D") == 0) {
    *out_style = NANOPDF_PAGE_LABEL_STYLE_DECIMAL_ARABIC;
    return 1;
  }
  if (strcmp(object->as.text, "R") == 0) {
    *out_style = NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_ROMAN;
    return 1;
  }
  if (strcmp(object->as.text, "r") == 0) {
    *out_style = NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_ROMAN;
    return 1;
  }
  if (strcmp(object->as.text, "A") == 0) {
    *out_style = NANOPDF_PAGE_LABEL_STYLE_UPPERCASE_LETTERS;
    return 1;
  }
  if (strcmp(object->as.text, "a") == 0) {
    *out_style = NANOPDF_PAGE_LABEL_STYLE_LOWERCASE_LETTERS;
    return 1;
  }
  return 0;
}

static int append_page_label_entry(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    uint32_t page_index,
    const nanopdf_basic_dict* label_dict,
    const nanopdf_basic_document* source_document) {
  nanopdf_basic_page_label_entry* resized = NULL;
  size_t new_capacity = 0;
  nanopdf_basic_page_label_entry* entry = NULL;
  nanopdf_basic_object resolved_obj;
  const nanopdf_basic_object* value = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!context || !document || !label_dict || !source_document) {
    return 0;
  }
  if (document->page_label_count == document->page_label_capacity) {
    new_capacity = document->page_label_capacity == 0 ?
        4 : document->page_label_capacity * 2;
    resized = (nanopdf_basic_page_label_entry*)nanopdf__allocator_realloc(
        &context->allocator,
        document->page_labels,
        new_capacity * sizeof(*document->page_labels));
    if (!resized) {
      return 0;
    }
    memset(
        resized + document->page_label_capacity,
        0,
        (new_capacity - document->page_label_capacity) * sizeof(*document->page_labels));
    document->page_labels = resized;
    document->page_label_capacity = new_capacity;
  }

  entry = &document->page_labels[document->page_label_count];
  memset(entry, 0, sizeof(*entry));
  entry->page_index = page_index;
  entry->style = NANOPDF_PAGE_LABEL_STYLE_NONE;
  entry->start_value = 1u;

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, label_dict, "S", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return 0;
  }
  if (value) {
    map_page_label_style(value, &entry->style);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, label_dict, "P", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_page_label_entry(&context->allocator, entry);
    return 0;
  }
  if (value && !duplicate_object_text(context, value, &entry->prefix)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_page_label_entry(&context->allocator, entry);
    return 0;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, source_document, label_dict, "St", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    destroy_basic_page_label_entry(&context->allocator, entry);
    return 0;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_NUMBER &&
      value->as.number > 0.0) {
    entry->start_value = (uint32_t)value->as.number;
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  document->page_label_count++;
  return 1;
}

static int find_page_index_for_ref(
    const nanopdf_basic_document* document,
    nanopdf_ref page_ref,
    uint32_t* out_page_index) {
  size_t i = 0;
  if (!document || !out_page_index || !page_ref.valid) {
    return 0;
  }
  for (i = 0; i < document->page_count; ++i) {
    if (document->pages[i].valid &&
        document->pages[i].object_number == page_ref.object_number &&
        document->pages[i].generation == page_ref.generation) {
      *out_page_index = (uint32_t)i;
      return 1;
    }
  }
  return 0;
}

static int append_named_destination_entry(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    const char* name,
    const nanopdf_basic_object* destination_array) {
  nanopdf_basic_named_destination* resized = NULL;
  size_t new_capacity = 0;
  nanopdf_basic_named_destination* entry = NULL;
  nanopdf_ref page_ref = {0, 0, 0};
  uint32_t page_index = 0;
  size_t numeric_count = 0;
  size_t i = 0;

  if (!context || !document || !name || !destination_array ||
      destination_array->type != NANOPDF_BASIC_OBJECT_ARRAY ||
      destination_array->as.array.count < 2) {
    return 0;
  }
  if (!basic_object_to_ref(&destination_array->as.array.items[0], &page_ref) ||
      !find_page_index_for_ref(document, page_ref, &page_index)) {
    return 0;
  }

  if (document->named_destination_count == document->named_destination_capacity) {
    new_capacity = document->named_destination_capacity == 0 ?
        4 : document->named_destination_capacity * 2;
    resized = (nanopdf_basic_named_destination*)nanopdf__allocator_realloc(
        &context->allocator,
        document->named_destinations,
        new_capacity * sizeof(*document->named_destinations));
    if (!resized) {
      return 0;
    }
    memset(
        resized + document->named_destination_capacity,
        0,
        (new_capacity - document->named_destination_capacity) *
            sizeof(*document->named_destinations));
    document->named_destinations = resized;
    document->named_destination_capacity = new_capacity;
  }

  entry = &document->named_destinations[document->named_destination_count];
  memset(entry, 0, sizeof(*entry));
  entry->page_index = page_index;
  entry->name = nanopdf__strdup(&context->allocator, name);
  if (!entry->name) {
    return 0;
  }
  if (!duplicate_object_text(
          context, &destination_array->as.array.items[1], &entry->fit_type)) {
    destroy_basic_named_destination(&context->allocator, entry);
    return 0;
  }

  for (i = 2; i < destination_array->as.array.count; ++i) {
    if (destination_array->as.array.items[i].type == NANOPDF_BASIC_OBJECT_NUMBER) {
      numeric_count++;
    }
  }
  if (numeric_count > 0) {
    size_t numeric_index = 0;
    entry->position = (double*)nanopdf__allocator_alloc(
        &context->allocator, numeric_count * sizeof(*entry->position));
    if (!entry->position) {
      destroy_basic_named_destination(&context->allocator, entry);
      return 0;
    }
    for (i = 2; i < destination_array->as.array.count; ++i) {
      if (destination_array->as.array.items[i].type == NANOPDF_BASIC_OBJECT_NUMBER) {
        entry->position[numeric_index++] = destination_array->as.array.items[i].as.number;
      }
    }
    entry->position_count = numeric_count;
  }

  document->named_destination_count++;
  return 1;
}

static nanopdf_status parse_catalog_preferences(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_dict* root_dict,
    nanopdf_basic_document* out_document) {
  nanopdf_basic_object resolved_obj;
  const nanopdf_basic_object* value = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!context || !document || !root_dict || !out_document) {
    return set_status(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to parse catalog preferences");
  }

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "Lang", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value &&
      !duplicate_object_text(context, value, &out_document->language)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store document language");
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "Metadata", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_STREAM &&
      !duplicate_stream_bytes_as_text(context, value, &out_document->xmp_metadata)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store XMP metadata");
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "OpenAction", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value &&
      (value->type == NANOPDF_BASIC_OBJECT_STRING ||
       value->type == NANOPDF_BASIC_OBJECT_NAME ||
       value->type == NANOPDF_BASIC_OBJECT_ARRAY) &&
      !duplicate_object_text(
          context, value, &out_document->open_action_named_destination)) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return set_status(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to store open action named destination");
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "PageLayout", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value) {
    map_catalog_page_layout(value, &out_document->page_layout);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "PageMode", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value) {
    map_catalog_page_mode(value, &out_document->page_mode);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "ViewerPreferences", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_DICT) {
    const nanopdf_basic_dict* prefs = &value->as.dict;
    const nanopdf_basic_object* bool_obj = NULL;
    nanopdf_basic_object resolved_bool;

    out_document->has_viewer_preferences = 1u;

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "HideToolbar", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.hide_toolbar =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "HideMenubar", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.hide_menubar =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "HideWindowUI", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.hide_window_ui =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "FitWindow", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.fit_window =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "CenterWindow", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.center_window =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, prefs, "DisplayDocTitle", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->viewer_preferences.display_doc_title =
          (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "MarkInfo", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_DICT) {
    const nanopdf_basic_dict* mark_info = &value->as.dict;
    const nanopdf_basic_object* bool_obj = NULL;
    nanopdf_basic_object resolved_bool;

    out_document->has_mark_info = 1u;

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, mark_info, "Marked", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->mark_info.marked = (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }

    nanopdf_basic_object_init(&resolved_bool);
    bool_obj = dict_get_resolved(
        context, document, mark_info, "Suspects", &resolved_bool, &status);
    if (status == NANOPDF_STATUS_OK && bool_obj &&
        bool_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
      out_document->mark_info.suspects = (uint8_t)(bool_obj->as.boolean ? 1 : 0);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_bool);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "OutputIntents", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_ARRAY) {
    size_t i;
    for (i = 0; i < value->as.array.count; ++i) {
      nanopdf_basic_object resolved_item;
      const nanopdf_basic_object* item = NULL;

      nanopdf_basic_object_init(&resolved_item);
      item = resolve_object_reference(
          context, document, &value->as.array.items[i], &resolved_item, &status);
      if (status != NANOPDF_STATUS_OK) {
        nanopdf_basic_object_destroy(&context->allocator, &resolved_item);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
        return status;
      }
      if (item && item->type == NANOPDF_BASIC_OBJECT_DICT &&
          !append_output_intent(
              context, out_document, &item->as.dict, document)) {
        nanopdf_basic_object_destroy(&context->allocator, &resolved_item);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
        return set_status(
            context,
            NANOPDF_STATUS_OUT_OF_MEMORY,
            "failed to store output intent");
      }
      nanopdf_basic_object_destroy(&context->allocator, &resolved_item);
    }
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "PageLabels", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_DICT) {
    nanopdf_basic_object resolved_nums;
    const nanopdf_basic_object* nums = NULL;

    nanopdf_basic_object_init(&resolved_nums);
    nums = dict_get_resolved(
        context, document, &value->as.dict, "Nums", &resolved_nums, &status);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_nums);
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }
    if (nums && nums->type == NANOPDF_BASIC_OBJECT_ARRAY) {
      size_t i;
      for (i = 0; i + 1 < nums->as.array.count; i += 2) {
        const nanopdf_basic_object* page_index_obj = &nums->as.array.items[i];
        nanopdf_basic_object resolved_label;
        const nanopdf_basic_object* label_obj = NULL;

        if (page_index_obj->type != NANOPDF_BASIC_OBJECT_NUMBER ||
            page_index_obj->as.number < 0.0) {
          continue;
        }
        nanopdf_basic_object_init(&resolved_label);
        label_obj = resolve_object_reference(
            context, document, &nums->as.array.items[i + 1], &resolved_label, &status);
        if (status != NANOPDF_STATUS_OK) {
          nanopdf_basic_object_destroy(&context->allocator, &resolved_label);
          nanopdf_basic_object_destroy(&context->allocator, &resolved_nums);
          nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
          return status;
        }
        if (label_obj && label_obj->type == NANOPDF_BASIC_OBJECT_DICT &&
            !append_page_label_entry(
                context,
                out_document,
                (uint32_t)page_index_obj->as.number,
                &label_obj->as.dict,
                document)) {
          nanopdf_basic_object_destroy(&context->allocator, &resolved_label);
          nanopdf_basic_object_destroy(&context->allocator, &resolved_nums);
          nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
          return set_status(
              context,
              NANOPDF_STATUS_OUT_OF_MEMORY,
              "failed to store page label");
        }
        nanopdf_basic_object_destroy(&context->allocator, &resolved_label);
      }
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_nums);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);

  return clear_status(context);
}

static nanopdf_status parse_catalog_named_destinations(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_dict* root_dict,
    nanopdf_basic_document* out_document) {
  nanopdf_basic_object resolved_obj;
  const nanopdf_basic_object* value = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  nanopdf_basic_object_init(&resolved_obj);
  value = dict_get_resolved(
      context, document, root_dict, "Names", &resolved_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
    return status;
  }
  if (value && value->type == NANOPDF_BASIC_OBJECT_DICT) {
    nanopdf_basic_object resolved_dests;
    const nanopdf_basic_object* dests = NULL;

    nanopdf_basic_object_init(&resolved_dests);
    dests = dict_get_resolved(
        context, document, &value->as.dict, "Dests", &resolved_dests, &status);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
      nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
      return status;
    }
    if (dests && dests->type == NANOPDF_BASIC_OBJECT_DICT) {
      nanopdf_basic_object resolved_names;
      const nanopdf_basic_object* names = NULL;

      nanopdf_basic_object_init(&resolved_names);
      names = dict_get_resolved(
          context, document, &dests->as.dict, "Names", &resolved_names, &status);
      if (status != NANOPDF_STATUS_OK) {
        nanopdf_basic_object_destroy(&context->allocator, &resolved_names);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
        nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
        return status;
      }
      if (names && names->type == NANOPDF_BASIC_OBJECT_ARRAY) {
        size_t i;
        for (i = 0; i + 1 < names->as.array.count; i += 2) {
          char* destination_name = NULL;
          nanopdf_basic_object resolved_destination;
          const nanopdf_basic_object* destination_value = NULL;

          if (!duplicate_object_text(context, &names->as.array.items[i], &destination_name)) {
            nanopdf_basic_object_destroy(&context->allocator, &resolved_names);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
            return set_status(
                context,
                NANOPDF_STATUS_OUT_OF_MEMORY,
                "failed to store named destination name");
          }
          nanopdf_basic_object_init(&resolved_destination);
          destination_value = resolve_object_reference(
              context,
              document,
              &names->as.array.items[i + 1],
              &resolved_destination,
              &status);
          if (status != NANOPDF_STATUS_OK) {
            nanopdf__allocator_free(&context->allocator, destination_name);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_destination);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_names);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
            return status;
          }
          if (destination_value &&
              !append_named_destination_entry(
                  context, out_document, destination_name, destination_value)) {
            nanopdf__allocator_free(&context->allocator, destination_name);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_destination);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_names);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
            nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
            return set_status(
                context,
                NANOPDF_STATUS_OUT_OF_MEMORY,
                "failed to store named destination");
          }
          nanopdf__allocator_free(&context->allocator, destination_name);
          nanopdf_basic_object_destroy(&context->allocator, &resolved_destination);
        }
      }
      nanopdf_basic_object_destroy(&context->allocator, &resolved_names);
    }
    nanopdf_basic_object_destroy(&context->allocator, &resolved_dests);
  }
  nanopdf_basic_object_destroy(&context->allocator, &resolved_obj);
  return clear_status(context);
}

static int field_type_from_object(
    const nanopdf_basic_object* object,
    nanopdf_field_type* out_type) {
  if (!object || !out_type || object->type != NANOPDF_BASIC_OBJECT_NAME) {
    return 0;
  }
  if (strcmp(object->as.text, "Btn") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_BUTTON;
    return 1;
  }
  if (strcmp(object->as.text, "Tx") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_TEXT;
    return 1;
  }
  if (strcmp(object->as.text, "Ch") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_CHOICE;
    return 1;
  }
  if (strcmp(object->as.text, "Sig") == 0) {
    *out_type = NANOPDF_FIELD_TYPE_SIGNATURE;
    return 1;
  }
  return 0;
}

static nanopdf_status walk_form_field_tree_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_ref field_ref,
    const char* parent_full_name,
    int inherited_has_type,
    nanopdf_field_type inherited_type,
    uint32_t inherited_flags,
    nanopdf_basic_document* out_document) {
  nanopdf_basic_object field_object;
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* ft_obj = NULL;
  const nanopdf_basic_object* ff_obj = NULL;
  const nanopdf_basic_object* t_obj = NULL;
  const nanopdf_basic_object* tu_obj = NULL;
  const nanopdf_basic_object* tm_obj = NULL;
  const nanopdf_basic_object* v_obj = NULL;
  const nanopdf_basic_object* kids_obj = NULL;
  const nanopdf_basic_object* subtype_obj = NULL;
  nanopdf_basic_object resolved_ft_obj;
  nanopdf_basic_object resolved_ff_obj;
  nanopdf_basic_object resolved_t_obj;
  nanopdf_basic_object resolved_tu_obj;
  nanopdf_basic_object resolved_tm_obj;
  nanopdf_basic_object resolved_v_obj;
  nanopdf_basic_object resolved_kids_obj;
  nanopdf_basic_object resolved_subtype_obj;
  nanopdf_field_type current_type = inherited_type;
  int has_current_type = inherited_has_type;
  uint32_t current_flags = inherited_flags;
  char* partial_name = NULL;
  char* full_name = NULL;
  char* alternate_name = NULL;
  char* mapping_name = NULL;
  char* value = NULL;
  int has_field_identity = 0;
  int should_append = 0;
  int is_widget = 0;
  const char* next_parent = parent_full_name;
  nanopdf_status status = NANOPDF_STATUS_OK;
  size_t i = 0;

  nanopdf_basic_object_init(&field_object);
  nanopdf_basic_object_init(&resolved_ft_obj);
  nanopdf_basic_object_init(&resolved_ff_obj);
  nanopdf_basic_object_init(&resolved_t_obj);
  nanopdf_basic_object_init(&resolved_tu_obj);
  nanopdf_basic_object_init(&resolved_tm_obj);
  nanopdf_basic_object_init(&resolved_v_obj);
  nanopdf_basic_object_init(&resolved_kids_obj);
  nanopdf_basic_object_init(&resolved_subtype_obj);
  status = nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){field_ref.object_number, field_ref.generation, field_ref.valid},
      &field_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  dict = nanopdf_basic_object_as_dict(&field_object);
  if (!dict) {
    nanopdf_basic_object_destroy(&context->allocator, &field_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "form field is not a dictionary");
  }

  subtype_obj = dict_get_resolved(
      context, document, dict, "Subtype", &resolved_subtype_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (subtype_obj && subtype_obj->type == NANOPDF_BASIC_OBJECT_NAME &&
      strcmp(subtype_obj->as.text, "Widget") == 0) {
    is_widget = 1;
  }

  ft_obj = dict_get_resolved(context, document, dict, "FT", &resolved_ft_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (field_type_from_object(ft_obj, &current_type)) {
    has_current_type = 1;
    has_field_identity = 1;
  }

  ff_obj = dict_get_resolved(context, document, dict, "Ff", &resolved_ff_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (ff_obj && ff_obj->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    current_flags = (uint32_t)ff_obj->as.number;
    has_field_identity = 1;
  }

  t_obj = dict_get_resolved(context, document, dict, "T", &resolved_t_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (t_obj && duplicate_object_text(context, t_obj, &partial_name)) {
    has_field_identity = 1;
  }
  full_name = duplicate_joined_name(context, parent_full_name, partial_name);
  if (!full_name) {
    status = set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store full field name");
    goto cleanup_error;
  }

  tu_obj = dict_get_resolved(context, document, dict, "TU", &resolved_tu_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (tu_obj && !duplicate_object_text(context, tu_obj, &alternate_name)) {
    alternate_name = NULL;
  } else if (tu_obj) {
    has_field_identity = 1;
  }
  tm_obj = dict_get_resolved(context, document, dict, "TM", &resolved_tm_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (tm_obj && !duplicate_object_text(context, tm_obj, &mapping_name)) {
    mapping_name = NULL;
  } else if (tm_obj) {
    has_field_identity = 1;
  }
  v_obj = dict_get_resolved(context, document, dict, "V", &resolved_v_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (v_obj && duplicate_object_text(context, v_obj, &value)) {
    has_field_identity = 1;
  }

  should_append = has_current_type && (has_field_identity || !is_widget);
  next_parent = full_name ? full_name : parent_full_name;
  if (should_append) {
    if (!append_form_field(
            context,
            out_document,
            current_type,
            partial_name,
            full_name,
            alternate_name,
            mapping_name,
            value,
            current_flags)) {
      status = set_status(
          context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to append form field");
      goto cleanup_error;
    }
    partial_name = NULL;
    full_name = NULL;
    alternate_name = NULL;
    mapping_name = NULL;
    value = NULL;
  }

  kids_obj = dict_get_resolved(
      context, document, dict, "Kids", &resolved_kids_obj, &status);
  if (status != NANOPDF_STATUS_OK) {
    goto cleanup_error;
  }
  if (kids_obj && kids_obj->type == NANOPDF_BASIC_OBJECT_ARRAY) {
    for (i = 0; i < kids_obj->as.array.count; ++i) {
      nanopdf_ref child = {0, 0, 0};
      if (!basic_object_to_ref(&kids_obj->as.array.items[i], &child)) {
        continue;
      }
      status = walk_form_field_tree_object(
          context,
          document,
          child,
          next_parent,
          has_current_type,
          current_type,
          current_flags,
          out_document);
      if (status != NANOPDF_STATUS_OK) {
        goto cleanup_error;
      }
    }
  }

  nanopdf_basic_object_destroy(&context->allocator, &resolved_ft_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_ff_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_t_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_tu_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_tm_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_v_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_subtype_obj);
  nanopdf_basic_object_destroy(&context->allocator, &field_object);
  destroy_basic_form_field(
      &context->allocator,
      &(nanopdf_basic_form_field){
          current_type, partial_name, full_name, alternate_name,
          mapping_name, value, current_flags});
  return clear_status(context);

cleanup_error:
  nanopdf_basic_object_destroy(&context->allocator, &resolved_ft_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_ff_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_t_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_tu_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_tm_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_v_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_kids_obj);
  nanopdf_basic_object_destroy(&context->allocator, &resolved_subtype_obj);
  nanopdf_basic_object_destroy(&context->allocator, &field_object);
  destroy_basic_form_field(
      &context->allocator,
      &(nanopdf_basic_form_field){
          current_type, partial_name, full_name, alternate_name,
          mapping_name, value, current_flags});
  return status;
}

static nanopdf_status parse_acro_form_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_dict* root_dict,
    nanopdf_basic_document* out_document) {
  const nanopdf_basic_object* acro_form_obj = NULL;
  nanopdf_basic_object acro_form_loaded;
  nanopdf_basic_object fields_loaded;
  const nanopdf_basic_dict* acro_form_dict = NULL;
  const nanopdf_basic_object* fields_obj = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;
  size_t i = 0;

  out_document->forms_parsed = 1;
  nanopdf_basic_object_init(&acro_form_loaded);
  nanopdf_basic_object_init(&fields_loaded);

  acro_form_obj = nanopdf_basic_dict_get(root_dict, "AcroForm");
  if (!acro_form_obj) {
    return clear_status(context);
  }
  if (acro_form_obj->type == NANOPDF_BASIC_OBJECT_REF) {
    status = nanopdf_basic_load_object(
        context,
        document,
        acro_form_obj->as.ref,
        &acro_form_loaded);
    if (status != NANOPDF_STATUS_OK) {
      out_document->forms_parsed = 0;
      return clear_status(context);
    }
    acro_form_dict = nanopdf_basic_object_as_dict(&acro_form_loaded);
  } else {
    acro_form_dict = nanopdf_basic_object_as_dict(acro_form_obj);
  }

  if (!acro_form_dict) {
    nanopdf_basic_object_destroy(&context->allocator, &acro_form_loaded);
    out_document->forms_parsed = 0;
    return clear_status(context);
  }

  fields_obj = dict_get_resolved(
      context, document, acro_form_dict, "Fields", &fields_loaded, &status);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &fields_loaded);
    nanopdf_basic_object_destroy(&context->allocator, &acro_form_loaded);
    out_document->forms_parsed = 0;
    return clear_status(context);
  }
  if (!fields_obj || fields_obj->type != NANOPDF_BASIC_OBJECT_ARRAY) {
    nanopdf_basic_object_destroy(&context->allocator, &fields_loaded);
    nanopdf_basic_object_destroy(&context->allocator, &acro_form_loaded);
    return clear_status(context);
  }

  for (i = 0; i < fields_obj->as.array.count; ++i) {
    nanopdf_ref field_ref = {0, 0, 0};
    if (!basic_object_to_ref(&fields_obj->as.array.items[i], &field_ref)) {
      continue;
    }
    status = walk_form_field_tree_object(
        context,
        document,
        field_ref,
        "",
        0,
        NANOPDF_FIELD_TYPE_TEXT,
        0u,
        out_document);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &fields_loaded);
      nanopdf_basic_object_destroy(&context->allocator, &acro_form_loaded);
      return status;
    }
  }

  nanopdf_basic_object_destroy(&context->allocator, &fields_loaded);
  nanopdf_basic_object_destroy(&context->allocator, &acro_form_loaded);
  return clear_status(context);
}

static nanopdf_status parse_xref_stream(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t xref_offset,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt,
    size_t* out_prev_offset,
    int* out_has_prev) {
  nanopdf_basic_object xref_object;
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* type_obj = NULL;
  const nanopdf_basic_object* w_obj = NULL;
  const nanopdf_basic_object* size_obj = NULL;
  const nanopdf_basic_object* index_obj = NULL;
  const nanopdf_basic_object* root_obj = NULL;
  const nanopdf_basic_object* info_obj = NULL;
  const nanopdf_basic_object* encrypt_obj = NULL;
  const nanopdf_basic_object* prev_obj = NULL;
  nanopdf_basic_xref_entry* xrefs = NULL;
  size_t xref_capacity = 0;
  size_t xref_count = 0;
  uint8_t* decoded = NULL;
  size_t decoded_size = 0;
  int w[3] = {0, 0, 0};
  size_t pos = 0;
  size_t i = 0;
  nanopdf_status status = NANOPDF_STATUS_OK;

  nanopdf_basic_object_init(&xref_object);
  status = nanopdf_basic_parse_indirect_object_at(
      context, data, size, xref_offset, &xref_object);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }
  if (xref_object.type != NANOPDF_BASIC_OBJECT_STREAM) {
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "xref stream is not a stream");
  }

  dict = &xref_object.as.stream.dict;
  type_obj = nanopdf_basic_dict_get(dict, "Type");
  if (type_obj && (type_obj->type != NANOPDF_BASIC_OBJECT_NAME ||
                   strcmp(type_obj->as.text, "XRef") != 0)) {
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "object is not an xref stream");
  }

  w_obj = nanopdf_basic_dict_get(dict, "W");
  size_obj = nanopdf_basic_dict_get(dict, "Size");
  if (!w_obj || w_obj->type != NANOPDF_BASIC_OBJECT_ARRAY ||
      w_obj->as.array.count != 3 || !size_obj ||
      size_obj->type != NANOPDF_BASIC_OBJECT_NUMBER) {
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid xref stream dictionary");
  }
  for (i = 0; i < 3; ++i) {
    if (w_obj->as.array.items[i].type != NANOPDF_BASIC_OBJECT_NUMBER ||
        w_obj->as.array.items[i].as.number < 0.0 ||
        w_obj->as.array.items[i].as.number > 8.0) {
      nanopdf_basic_object_destroy(&context->allocator, &xref_object);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid xref stream W entry");
    }
    w[i] = (int)w_obj->as.array.items[i].as.number;
  }

  status = ensure_xref_capacity(
      context,
      &xrefs,
      &xref_capacity,
      (size_t)size_obj->as.number);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    return status;
  }
  xref_count = (size_t)size_obj->as.number;

  root_obj = nanopdf_basic_dict_get(dict, "Root");
  info_obj = nanopdf_basic_dict_get(dict, "Info");
  encrypt_obj = nanopdf_basic_dict_get(dict, "Encrypt");
  index_obj = nanopdf_basic_dict_get(dict, "Index");
  prev_obj = nanopdf_basic_dict_get(dict, "Prev");
  if (root_obj) {
    basic_object_to_ref(root_obj, out_root_ref);
  }
  if (info_obj) {
    basic_object_to_ref(info_obj, out_info_ref);
  }
  if (encrypt_obj && encrypt_obj->type != NANOPDF_BASIC_OBJECT_NULL) {
    if (out_has_encrypt) {
      *out_has_encrypt = 1;
    }
  }
  if (prev_obj && prev_obj->type == NANOPDF_BASIC_OBJECT_NUMBER &&
      prev_obj->as.number >= 0.0) {
    if (out_prev_offset) {
      *out_prev_offset = (size_t)prev_obj->as.number;
    }
    if (out_has_prev) {
      *out_has_prev = 1;
    }
  }

  status = nanopdf_basic_decode_stream(context, &xref_object, &decoded, &decoded_size);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    nanopdf__allocator_free(&context->allocator, xrefs);
    return status;
  }

  if (index_obj && index_obj->type == NANOPDF_BASIC_OBJECT_ARRAY &&
      index_obj->as.array.count % 2 == 0) {
    for (i = 0; i + 1 < index_obj->as.array.count; i += 2) {
      uint64_t object_number = 0;
      uint64_t count = 0;
      size_t j = 0;
      if (index_obj->as.array.items[i].type != NANOPDF_BASIC_OBJECT_NUMBER ||
          index_obj->as.array.items[i + 1].type != NANOPDF_BASIC_OBJECT_NUMBER) {
        nanopdf__allocator_free(&context->allocator, decoded);
        nanopdf__allocator_free(&context->allocator, xrefs);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid xref Index array");
      }
      object_number = (uint64_t)index_obj->as.array.items[i].as.number;
      count = (uint64_t)index_obj->as.array.items[i + 1].as.number;
      for (j = 0; j < count; ++j, ++object_number) {
        uint64_t type_field = w[0] ? 0 : 1;
        uint64_t field2 = 0;
        uint64_t field3 = 0;
        int k = 0;
        if (pos > decoded_size) {
          nanopdf__allocator_free(&context->allocator, decoded);
          nanopdf__allocator_free(&context->allocator, xrefs);
          return set_status(context, NANOPDF_STATUS_MALFORMED, "xref stream truncated");
        }
        for (k = 0; k < w[0]; ++k) type_field = (type_field << 8) | decoded[pos++];
        for (k = 0; k < w[1]; ++k) field2 = (field2 << 8) | decoded[pos++];
        for (k = 0; k < w[2]; ++k) field3 = (field3 << 8) | decoded[pos++];
        if (object_number >= xref_count) {
          continue;
        }
        if (type_field == 1) {
          xrefs[object_number].present = 1;
          xrefs[object_number].in_use = 1;
          xrefs[object_number].compressed = 0;
          xrefs[object_number].offset = (size_t)field2;
          xrefs[object_number].generation = (uint16_t)field3;
        } else if (type_field == 0) {
          xrefs[object_number].present = 1;
          xrefs[object_number].in_use = 0;
          xrefs[object_number].compressed = 0;
          xrefs[object_number].generation = (uint16_t)field3;
        } else if (type_field == 2) {
          xrefs[object_number].present = 1;
          xrefs[object_number].in_use = 1;
          xrefs[object_number].compressed = 1;
          xrefs[object_number].object_stream_number = (uint32_t)field2;
          xrefs[object_number].object_stream_index = (uint32_t)field3;
        }
      }
    }
  } else {
    for (i = 0; i < xref_count; ++i) {
      uint64_t type_field = w[0] ? 0 : 1;
      uint64_t field2 = 0;
      uint64_t field3 = 0;
      int k = 0;
      for (k = 0; k < w[0]; ++k) type_field = (type_field << 8) | decoded[pos++];
      for (k = 0; k < w[1]; ++k) field2 = (field2 << 8) | decoded[pos++];
      for (k = 0; k < w[2]; ++k) field3 = (field3 << 8) | decoded[pos++];
      if (type_field == 1) {
        xrefs[i].present = 1;
        xrefs[i].in_use = 1;
        xrefs[i].compressed = 0;
        xrefs[i].offset = (size_t)field2;
        xrefs[i].generation = (uint16_t)field3;
      } else if (type_field == 0) {
        xrefs[i].present = 1;
        xrefs[i].in_use = 0;
        xrefs[i].compressed = 0;
        xrefs[i].generation = (uint16_t)field3;
      } else if (type_field == 2) {
        xrefs[i].present = 1;
        xrefs[i].in_use = 1;
        xrefs[i].compressed = 1;
        xrefs[i].object_stream_number = (uint32_t)field2;
        xrefs[i].object_stream_index = (uint32_t)field3;
      }
    }
  }

  nanopdf__allocator_free(&context->allocator, decoded);
  nanopdf_basic_object_destroy(&context->allocator, &xref_object);
  *out_xrefs = xrefs;
  *out_xref_count = xref_count;
  return clear_status(context);
}

static nanopdf_status NANOPDF_MAYBE_UNUSED parse_any_xref(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t xref_offset,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt) {
  size_t prev_offset = 0;
  size_t xrefstm_offset = 0;
  int has_prev = 0;
  int has_xrefstm = 0;
  if (xref_offset + 4 <= size &&
      memcmp(data + xref_offset, "xref", 4) == 0) {
    return parse_classic_xref(
        context,
        (const char*)data,
        size,
        xref_offset,
        out_xrefs,
        out_xref_count,
        out_root_ref,
        out_info_ref,
        out_has_encrypt,
        &xrefstm_offset,
        &has_xrefstm,
        &prev_offset,
        &has_prev);
  }
  return parse_xref_stream(
      context,
      data,
      size,
      xref_offset,
      out_xrefs,
      out_xref_count,
      out_root_ref,
      out_info_ref,
      out_has_encrypt,
      &prev_offset,
      &has_prev);
}

static nanopdf_status merge_xref_section(
    nanopdf_context* context,
    nanopdf_basic_xref_entry** io_xrefs,
    size_t* io_xref_count,
    const nanopdf_basic_xref_entry* section_xrefs,
    size_t section_count,
    int replace_free_with_in_use) {
  nanopdf_basic_xref_entry* merged = NULL;
  size_t target_count = 0;
  size_t i = 0;

  if (!context || !io_xrefs || !io_xref_count) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid xref merge arguments");
  }

  target_count = *io_xref_count > section_count ? *io_xref_count : section_count;
  merged = *io_xrefs;
  if (target_count > *io_xref_count) {
    merged = (nanopdf_basic_xref_entry*)nanopdf__allocator_realloc(
        &context->allocator, merged, target_count * sizeof(*merged));
    if (!merged) {
      return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow merged xref");
    }
    memset(merged + *io_xref_count, 0, (target_count - *io_xref_count) * sizeof(*merged));
    *io_xrefs = merged;
    *io_xref_count = target_count;
  }

  for (i = 0; i < section_count; ++i) {
    if (!section_xrefs[i].present) {
      continue;
    }
    if (!merged[i].present ||
        (replace_free_with_in_use && !merged[i].in_use && section_xrefs[i].in_use)) {
      merged[i] = section_xrefs[i];
    }
  }

  return clear_status(context);
}

static nanopdf_status parse_xref_chain(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t xref_offset,
    nanopdf_basic_xref_entry** out_xrefs,
    size_t* out_xref_count,
    nanopdf_ref* out_root_ref,
    nanopdf_ref* out_info_ref,
    int* out_has_encrypt) {
  nanopdf_basic_xref_entry* merged_xrefs = NULL;
  size_t merged_count = 0;
  nanopdf_ref latest_root = {0, 0, 0};
  nanopdf_ref latest_info = {0, 0, 0};
  int latest_has_encrypt = 0;
  size_t pending_offsets[96];
  uint8_t pending_modes[96];
  size_t processed_offsets[96];
  uint8_t processed_modes[96];
  size_t pending_count = 0;
  size_t processed_count = 0;
  size_t depth = 0;

  pending_offsets[pending_count++] = xref_offset;
  pending_modes[0] = 0;
  while (pending_count > 0) {
    nanopdf_basic_xref_entry* section_xrefs = NULL;
    size_t section_count = 0;
    nanopdf_ref section_root = {0, 0, 0};
    nanopdf_ref section_info = {0, 0, 0};
    uint8_t current_mode = pending_modes[--pending_count];
    size_t current_offset = pending_offsets[pending_count];
    size_t xrefstm_offset = 0;
    int section_has_encrypt = 0;
    int has_xrefstm = 0;
    size_t prev_offset = 0;
    int has_prev = 0;
    nanopdf_status status = NANOPDF_STATUS_OK;
    size_t i = 0;
    int already_processed = 0;

    if (depth++ > 32) {
      nanopdf__allocator_free(&context->allocator, merged_xrefs);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "xref Prev chain too deep");
    }
    for (i = 0; i < processed_count; ++i) {
      if (processed_offsets[i] == current_offset && processed_modes[i] == current_mode) {
        already_processed = 1;
        break;
      }
    }
    if (already_processed) {
      continue;
    }
    if (processed_count >= sizeof(processed_offsets) / sizeof(processed_offsets[0])) {
      nanopdf__allocator_free(&context->allocator, merged_xrefs);
      return set_status(context, NANOPDF_STATUS_MALFORMED, "xref chain is too complex");
    }
    processed_offsets[processed_count++] = current_offset;
    processed_modes[processed_count - 1] = current_mode;

    if (current_offset + 4 <= size && memcmp(data + current_offset, "xref", 4) == 0) {
      status = parse_classic_xref(
          context,
          (const char*)data,
          size,
          current_offset,
          &section_xrefs,
          &section_count,
          &section_root,
          &section_info,
          &section_has_encrypt,
          &xrefstm_offset,
          &has_xrefstm,
          &prev_offset,
          &has_prev);
    } else {
      status = parse_xref_stream(
          context,
          data,
          size,
          current_offset,
          &section_xrefs,
          &section_count,
          &section_root,
          &section_info,
          &section_has_encrypt,
          &prev_offset,
          &has_prev);
    }
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, merged_xrefs);
      return status;
    }

    if (!latest_root.valid && section_root.valid) {
      latest_root = section_root;
    }
    if (!latest_info.valid && section_info.valid) {
      latest_info = section_info;
    }
    if (!latest_has_encrypt && section_has_encrypt) {
      latest_has_encrypt = 1;
    }

    status = merge_xref_section(
        context,
        &merged_xrefs,
        &merged_count,
        section_xrefs,
        section_count,
        current_mode == 1);
    nanopdf__allocator_free(&context->allocator, section_xrefs);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf__allocator_free(&context->allocator, merged_xrefs);
      return status;
    }

    if (has_prev) {
      if (pending_count >= sizeof(pending_offsets) / sizeof(pending_offsets[0])) {
        nanopdf__allocator_free(&context->allocator, merged_xrefs);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "xref worklist overflow");
      }
      pending_offsets[pending_count++] = prev_offset;
      pending_modes[pending_count - 1] = 0;
    }
    if (has_xrefstm) {
      if (pending_count >= sizeof(pending_offsets) / sizeof(pending_offsets[0])) {
        nanopdf__allocator_free(&context->allocator, merged_xrefs);
        return set_status(context, NANOPDF_STATUS_MALFORMED, "xref worklist overflow");
      }
      pending_offsets[pending_count++] = xrefstm_offset;
      pending_modes[pending_count - 1] = 1;
    }
  }

  *out_xrefs = merged_xrefs;
  *out_xref_count = merged_count;
  if (out_root_ref) {
    *out_root_ref = latest_root;
  }
  if (out_info_ref) {
    *out_info_ref = latest_info;
  }
  if (out_has_encrypt) {
    *out_has_encrypt = latest_has_encrypt;
  }
  return clear_status(context);
}

static nanopdf_status extract_security_fields_at_offset(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t xref_offset,
    nanopdf_ref* out_encrypt_ref,
    char** out_file_id,
    int* out_has_encrypt) {
  size_t value_start = 0;
  size_t value_end = 0;
  size_t trailer_end = 0;
  const char* trailer = NULL;
  size_t trailer_length = 0;

  if (out_encrypt_ref) {
    memset(out_encrypt_ref, 0, sizeof(*out_encrypt_ref));
  }
  if (out_file_id) {
    *out_file_id = NULL;
  }
  if (out_has_encrypt) {
    *out_has_encrypt = 0;
  }

  if (xref_offset + 4 <= size && memcmp(data + xref_offset, "xref", 4) == 0) {
    size_t pos = xref_offset + 4;
    while (pos < size) {
      skip_ws((const char*)data, size, &pos);
      if (match_literal((const char*)data, size, pos, "trailer")) {
        break;
      }
      while (pos < size && data[pos] != '\n' && data[pos] != '\r') {
        pos++;
      }
    }
    if (!match_literal((const char*)data, size, pos, "trailer")) {
      return clear_status(context);
    }
    pos += 7;
    skip_ws((const char*)data, size, &pos);
    if (pos + 1 >= size || data[pos] != '<' || data[pos + 1] != '<' ||
        !find_matching_dict_end((const char*)data, size, pos, &trailer_end)) {
      return clear_status(context);
    }
    trailer = (const char*)data + pos;
    trailer_length = trailer_end - pos;
    if (find_top_level_key_value(trailer, trailer_length, "Encrypt", &value_start, &value_end)) {
      if (out_has_encrypt) {
        *out_has_encrypt = 1;
      }
      if (out_encrypt_ref) {
        parse_ref_value(trailer, value_start, value_end, out_encrypt_ref);
      }
    }
    if (out_file_id &&
        find_top_level_key_value(trailer, trailer_length, "ID", &value_start, &value_end)) {
      size_t id_pos = value_start;
      char buffer[128];
      skip_ws(trailer, trailer_length, &id_pos);
      if (id_pos < value_end && trailer[id_pos] == '[') {
        id_pos++;
        if (parse_info_string(trailer, id_pos, value_end, buffer, sizeof(buffer))) {
          *out_file_id = nanopdf__strdup(&context->allocator, buffer);
          if (!*out_file_id) {
            return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store file id");
          }
        }
      }
    }
    return clear_status(context);
  } else {
    nanopdf_basic_object xref_object;
    const nanopdf_basic_dict* dict = NULL;
    const nanopdf_basic_object* encrypt_obj = NULL;
    const nanopdf_basic_object* id_obj = NULL;
    nanopdf_basic_object_init(&xref_object);
    if (nanopdf_basic_parse_indirect_object_at(context, data, size, xref_offset, &xref_object) !=
        NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &xref_object);
      return clear_status(context);
    }
    dict = nanopdf_basic_object_as_dict(&xref_object);
    if (!dict) {
      nanopdf_basic_object_destroy(&context->allocator, &xref_object);
      return clear_status(context);
    }
    encrypt_obj = nanopdf_basic_dict_get(dict, "Encrypt");
    if (encrypt_obj) {
      if (out_has_encrypt) {
        *out_has_encrypt = 1;
      }
      if (out_encrypt_ref) {
        basic_object_to_ref(encrypt_obj, out_encrypt_ref);
      }
    }
    id_obj = nanopdf_basic_dict_get(dict, "ID");
    if (out_file_id && id_obj && id_obj->type == NANOPDF_BASIC_OBJECT_ARRAY &&
        id_obj->as.array.count > 0 &&
        id_obj->as.array.items[0].type == NANOPDF_BASIC_OBJECT_STRING) {
      *out_file_id = nanopdf__strdup(&context->allocator, id_obj->as.array.items[0].as.text);
      if (!*out_file_id) {
        nanopdf_basic_object_destroy(&context->allocator, &xref_object);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to store file id");
      }
    }
    nanopdf_basic_object_destroy(&context->allocator, &xref_object);
    return clear_status(context);
  }
}

static nanopdf_status initialize_password_security(
    nanopdf_context* context,
    nanopdf_basic_document* document,
    const nanopdf_parse_options* options,
    size_t xref_offset) {
  nanopdf_ref encrypt_ref;
  char* file_id = NULL;
  int has_encrypt = 0;
  nanopdf_basic_object encrypt_object;
  const nanopdf_basic_dict* dict = NULL;
  const nanopdf_basic_object* filter_obj = NULL;
  const nanopdf_basic_object* v_obj = NULL;
  const nanopdf_basic_object* r_obj = NULL;
  const nanopdf_basic_object* len_obj = NULL;
  const nanopdf_basic_object* o_obj = NULL;
  const nanopdf_basic_object* u_obj = NULL;
  const nanopdf_basic_object* p_obj = NULL;
  const nanopdf_basic_object* em_obj = NULL;
  const nanopdf_basic_object* stmf_obj = NULL;
  const nanopdf_basic_object* strf_obj = NULL;
  const nanopdf_basic_object* oe_obj = NULL;
  const nanopdf_basic_object* ue_obj = NULL;
  const nanopdf_basic_object* perms_obj = NULL;
  uint8_t string_algorithm = NANOPDF_BASIC_SECURITY_NONE;
  uint8_t stream_algorithm = NANOPDF_BASIC_SECURITY_NONE;
  int version = 0;
  int revision = 0;
  int key_bits = 40;
  int encrypt_metadata = 1;
  nanopdf_status status = NANOPDF_STATUS_OK;

  memset(&encrypt_ref, 0, sizeof(encrypt_ref));
  nanopdf_basic_object_init(&encrypt_object);
  status = extract_security_fields_at_offset(
      context,
      document->data,
      document->data_size,
      xref_offset,
      &encrypt_ref,
      &file_id,
      &has_encrypt);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf__allocator_free(&context->allocator, file_id);
    return status;
  }
  if (!has_encrypt) {
    nanopdf__allocator_free(&context->allocator, file_id);
    return clear_status(context);
  }
  if (!encrypt_ref.valid || !file_id) {
    nanopdf__allocator_free(&context->allocator, file_id);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }

  status = nanopdf_basic_load_object(
      context,
      document,
      (nanopdf_basic_ref){
          encrypt_ref.object_number,
          encrypt_ref.generation,
          encrypt_ref.valid},
      &encrypt_object);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf__allocator_free(&context->allocator, file_id);
    nanopdf__clear_error(context);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }
  dict = nanopdf_basic_object_as_dict(&encrypt_object);
  filter_obj = dict ? nanopdf_basic_dict_get(dict, "Filter") : NULL;
  v_obj = dict ? nanopdf_basic_dict_get(dict, "V") : NULL;
  r_obj = dict ? nanopdf_basic_dict_get(dict, "R") : NULL;
  len_obj = dict ? nanopdf_basic_dict_get(dict, "Length") : NULL;
  o_obj = dict ? nanopdf_basic_dict_get(dict, "O") : NULL;
  u_obj = dict ? nanopdf_basic_dict_get(dict, "U") : NULL;
  p_obj = dict ? nanopdf_basic_dict_get(dict, "P") : NULL;
  em_obj = dict ? nanopdf_basic_dict_get(dict, "EncryptMetadata") : NULL;
  stmf_obj = dict ? nanopdf_basic_dict_get(dict, "StmF") : NULL;
  strf_obj = dict ? nanopdf_basic_dict_get(dict, "StrF") : NULL;
  oe_obj = dict ? nanopdf_basic_dict_get(dict, "OE") : NULL;
  ue_obj = dict ? nanopdf_basic_dict_get(dict, "UE") : NULL;
  perms_obj = dict ? nanopdf_basic_dict_get(dict, "Perms") : NULL;

  if (!dict || !filter_obj || filter_obj->type != NANOPDF_BASIC_OBJECT_NAME ||
      strcmp(filter_obj->as.text, "Standard") != 0 ||
      !v_obj || v_obj->type != NANOPDF_BASIC_OBJECT_NUMBER ||
      !r_obj || r_obj->type != NANOPDF_BASIC_OBJECT_NUMBER ||
      !o_obj || o_obj->type != NANOPDF_BASIC_OBJECT_STRING ||
      !u_obj || u_obj->type != NANOPDF_BASIC_OBJECT_STRING ||
      !p_obj || p_obj->type != NANOPDF_BASIC_OBJECT_NUMBER) {
    nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
    nanopdf__allocator_free(&context->allocator, file_id);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }

  version = (int)v_obj->as.number;
  revision = (int)r_obj->as.number;
  if (len_obj && len_obj->type == NANOPDF_BASIC_OBJECT_NUMBER) {
    key_bits = (int)len_obj->as.number;
  } else if (version == 1 || revision == 2) {
    key_bits = 40;
  } else if (version == 4) {
    key_bits = 128;
  }
  if (em_obj && em_obj->type == NANOPDF_BASIC_OBJECT_BOOL) {
    encrypt_metadata = em_obj->as.boolean ? 1 : 0;
  }
  if (version == 5) {
    key_bits = 256;
  }
  if (!((version == 1 || version == 2 || version == 4 || version == 5) &&
        revision >= 2 && revision <= 6)) {
    nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
    nanopdf__allocator_free(&context->allocator, file_id);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }
  if (version == 5) {
    if (o_obj->length < 48 || u_obj->length < 48 ||
        !oe_obj || oe_obj->type != NANOPDF_BASIC_OBJECT_STRING || oe_obj->length < 32 ||
        !ue_obj || ue_obj->type != NANOPDF_BASIC_OBJECT_STRING || ue_obj->length < 32 ||
        !perms_obj || perms_obj->type != NANOPDF_BASIC_OBJECT_STRING || perms_obj->length < 16) {
      nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
      nanopdf__allocator_free(&context->allocator, file_id);
      return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
    }
  } else if (o_obj->length < 32 || u_obj->length < 32) {
    nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
    nanopdf__allocator_free(&context->allocator, file_id);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }

  if (version == 4 || version == 5) {
    const char* strf_name = "Identity";
    const char* stmf_name = "Identity";
    if (strf_obj && strf_obj->type == NANOPDF_BASIC_OBJECT_NAME) {
      strf_name = strf_obj->as.text;
    }
    if (stmf_obj && stmf_obj->type == NANOPDF_BASIC_OBJECT_NAME) {
      stmf_name = stmf_obj->as.text;
    }
    status = parse_v4_crypt_filter_algorithm(
        context, document, dict, strf_name, key_bits, &string_algorithm);
    if (status == NANOPDF_STATUS_OK) {
      status = parse_v4_crypt_filter_algorithm(
          context, document, dict, stmf_name, key_bits, &stream_algorithm);
    }
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
      nanopdf__allocator_free(&context->allocator, file_id);
      return status;
    }
  } else {
    string_algorithm =
        (uint8_t)(key_bits <= 40 ? NANOPDF_BASIC_SECURITY_RC4_40
                                 : NANOPDF_BASIC_SECURITY_RC4_128);
    stream_algorithm = string_algorithm;
  }

  if (version == 5) {
    if (!security_authenticate_aes256_password(
            context,
            options ? options->password : "",
            o_obj,
            u_obj,
            oe_obj,
            ue_obj,
            revision,
            document->security.key,
            &document->security.key_length)) {
      nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
      nanopdf__allocator_free(&context->allocator, file_id);
      return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
    }
  } else if (!security_authenticate_password(
                 options ? options->password : "",
                 o_obj->as.text,
                 u_obj->as.text,
                 (int32_t)p_obj->as.number,
                 file_id,
                 key_bits,
                 revision,
                 encrypt_metadata,
                 document->security.key,
                 &document->security.key_length)) {
    nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
    nanopdf__allocator_free(&context->allocator, file_id);
    return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
  }

  document->security.active = 1;
  document->security.authenticated = 1;
  document->security.algorithm = stream_algorithm != NANOPDF_BASIC_SECURITY_NONE
                                     ? stream_algorithm
                                     : string_algorithm;
  document->security.string_algorithm = string_algorithm;
  document->security.stream_algorithm = stream_algorithm;
  document->security.encrypt_metadata = (uint8_t)encrypt_metadata;
  document->security.permissions = (int32_t)p_obj->as.number;
  document->security.file_id = file_id;
  nanopdf_basic_object_destroy(&context->allocator, &encrypt_object);
  return clear_status(context);
}

void nanopdf_basic_document_init(nanopdf_basic_document* document) {
  if (!document) {
    return;
  }
  memset(document, 0, sizeof(*document));
}

void nanopdf_basic_document_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_document* document) {
  if (!allocator || !document) {
    return;
  }

  if (document->pages) {
    size_t i;
    for (i = 0; i < document->page_count; ++i) {
      size_t j;
      for (j = 0; j < document->pages[i].content_count; ++j) {
        destroy_basic_content(allocator, &document->pages[i].contents[j]);
      }
      nanopdf__allocator_free(allocator, document->pages[i].contents);
    }
  }
  nanopdf__allocator_free(allocator, document->xrefs);
  nanopdf__allocator_free(allocator, document->pages);
  nanopdf__allocator_free(allocator, document->title);
  nanopdf__allocator_free(allocator, document->author);
  nanopdf__allocator_free(allocator, document->subject);
  nanopdf__allocator_free(allocator, document->keywords);
  nanopdf__allocator_free(allocator, document->creator);
  nanopdf__allocator_free(allocator, document->producer);
  nanopdf__allocator_free(allocator, document->creation_date);
  nanopdf__allocator_free(allocator, document->mod_date);
  nanopdf__allocator_free(allocator, document->trapped);
  nanopdf__allocator_free(allocator, document->language);
  nanopdf__allocator_free(allocator, document->xmp_metadata);
  nanopdf__allocator_free(allocator, document->open_action_named_destination);
  if (document->output_intents) {
    size_t i;
    for (i = 0; i < document->output_intent_count; ++i) {
      destroy_basic_output_intent(allocator, &document->output_intents[i]);
    }
  }
  nanopdf__allocator_free(allocator, document->output_intents);
  if (document->page_labels) {
    size_t i;
    for (i = 0; i < document->page_label_count; ++i) {
      destroy_basic_page_label_entry(allocator, &document->page_labels[i]);
    }
  }
  nanopdf__allocator_free(allocator, document->page_labels);
  if (document->named_destinations) {
    size_t i;
    for (i = 0; i < document->named_destination_count; ++i) {
      destroy_basic_named_destination(allocator, &document->named_destinations[i]);
    }
  }
  nanopdf__allocator_free(allocator, document->named_destinations);
  if (document->custom_info) {
    size_t i;
    for (i = 0; i < document->custom_info_count; ++i) {
      nanopdf__allocator_free(allocator, document->custom_info[i].key);
      nanopdf__allocator_free(allocator, document->custom_info[i].value);
    }
  }
  nanopdf__allocator_free(allocator, document->custom_info);
  nanopdf__allocator_free(allocator, document->security.file_id);
  if (document->form_fields) {
    size_t i;
    for (i = 0; i < document->form_field_count; ++i) {
      destroy_basic_form_field(allocator, &document->form_fields[i]);
    }
  }
  nanopdf__allocator_free(allocator, document->form_fields);
  memset(document, 0, sizeof(*document));
}

nanopdf_status nanopdf_basic_document_parse(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const nanopdf_parse_options* options,
    nanopdf_basic_document* out_document) {
  size_t xref_offset = 0;
  nanopdf_basic_xref_entry* xrefs = NULL;
  size_t xref_count = 0;
  nanopdf_ref root_ref = {0, 0, 0};
  nanopdf_ref info_ref = {0, 0, 0};
  int has_encrypt = 0;
  nanopdf_basic_object root_object;
  const nanopdf_basic_dict* root_dict = NULL;
  const nanopdf_basic_object* pages_obj = NULL;
  nanopdf_ref pages_ref = {0, 0, 0};
  nanopdf_box inherited_media_box;
  nanopdf_status status;

  if (!context || !data || size < 8 || !out_document) {
    return set_status(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to basic C parser");
  }

  nanopdf_basic_document_init(out_document);
  memset(&inherited_media_box, 0, sizeof(inherited_media_box));
  out_document->recover_stream_length_enabled =
      options && options->recover_stream_length ? 1u : 0u;

  if (!(data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F' &&
        data[4] == '-')) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "missing PDF header");
  }
  out_document->version_major = (uint32_t)(data[5] - '0');
  out_document->version_minor = (uint32_t)(data[7] - '0');
  out_document->data = data;
  out_document->data_size = size;
  nanopdf_basic_object_init(&root_object);

  if (!find_startxref((const char*)data, size, &xref_offset)) {
    if (!options || !options->auto_repair) {
      return set_status(context, NANOPDF_STATUS_MALFORMED, "missing startxref");
    }
    status = repair_xref_scan(
        context, data, size, &xrefs, &xref_count, &root_ref, &info_ref, &has_encrypt);
    if (status != NANOPDF_STATUS_OK) {
      return status;
    }
    goto xref_ready;
  }

  status = parse_xref_chain(
      context,
      data,
      size,
      xref_offset,
      &xrefs,
      &xref_count,
      &root_ref,
      &info_ref,
      &has_encrypt);
  if (status != NANOPDF_STATUS_OK) {
    if (!options || !options->auto_repair) {
      return status;
    }
    nanopdf__clear_error(context);
    status = repair_xref_scan(
        context, data, size, &xrefs, &xref_count, &root_ref, &info_ref, &has_encrypt);
    if (status != NANOPDF_STATUS_OK) {
      return status;
    }
  }
xref_ready:
  out_document->xrefs = xrefs;
  out_document->xref_count = xref_count;

  if (has_encrypt) {
    if (!find_startxref((const char*)data, size, &xref_offset)) {
      nanopdf_basic_document_destroy(&context->allocator, out_document);
      return set_status(context, NANOPDF_STATUS_ENCRYPTED, "PDF is encrypted");
    }
    status = initialize_password_security(context, out_document, options, xref_offset);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_document_destroy(&context->allocator, out_document);
      return status;
    }
  }

  if (!root_ref.valid || root_ref.object_number >= xref_count ||
      !xrefs[root_ref.object_number].present ||
      !xrefs[root_ref.object_number].in_use) {
    nanopdf__allocator_free(&context->allocator, out_document->xrefs);
    out_document->xrefs = NULL;
    return set_status(context, NANOPDF_STATUS_MALFORMED, "missing Root reference");
  }

  status = nanopdf_basic_load_object(
      context,
      out_document,
      (nanopdf_basic_ref){root_ref.object_number, root_ref.generation, root_ref.valid},
      &root_object);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return status;
  }
  root_dict = nanopdf_basic_object_as_dict(&root_object);
  if (!root_dict) {
    nanopdf_basic_object_destroy(&context->allocator, &root_object);
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "catalog is not a dictionary");
  }

  pages_obj = nanopdf_basic_dict_get(root_dict, "Pages");
  if (!pages_obj || !basic_object_to_ref(pages_obj, &pages_ref)) {
    nanopdf_basic_object_destroy(&context->allocator, &root_object);
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return set_status(context, NANOPDF_STATUS_MALFORMED, "catalog Pages reference missing");
  }

  status = parse_catalog_preferences(context, out_document, root_dict, out_document);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &root_object);
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return status;
  }

  status = walk_page_tree_object(
      context, out_document, pages_ref, &inherited_media_box, 0.0, out_document);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &root_object);
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return status;
  }

  status = parse_catalog_named_destinations(context, out_document, root_dict, out_document);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_object_destroy(&context->allocator, &root_object);
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return status;
  }

  if (info_ref.valid && info_ref.object_number < xref_count &&
      out_document->xrefs[info_ref.object_number].present &&
      out_document->xrefs[info_ref.object_number].in_use) {
    status = parse_document_info_object(context, out_document, info_ref, out_document);
    if (status != NANOPDF_STATUS_OK) {
      nanopdf_basic_object_destroy(&context->allocator, &root_object);
      nanopdf_basic_document_destroy(&context->allocator, out_document);
      return status;
    }
  }

  status = parse_acro_form_object(context, out_document, root_dict, out_document);
  nanopdf_basic_object_destroy(&context->allocator, &root_object);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_document_destroy(&context->allocator, out_document);
    return status;
  }

  return clear_status(context);
}

static int append_output_char(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity,
    char ch) {
  char* resized;
  size_t new_capacity;

  if (*length + 2 <= *capacity) {
    (*buffer)[(*length)++] = ch;
    (*buffer)[*length] = '\0';
    return 1;
  }

  new_capacity = *capacity == 0 ? 64 : (*capacity * 2);
  while (new_capacity < *length + 2) {
    new_capacity *= 2;
  }

  resized = (char*)nanopdf__allocator_realloc(
      &context->allocator, *buffer, new_capacity);
  if (!resized) {
    return 0;
  }
  *buffer = resized;
  *capacity = new_capacity;
  (*buffer)[(*length)++] = ch;
  (*buffer)[*length] = '\0';
  return 1;
}

static int append_output_text(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity,
    const char* text) {
  size_t i;
  for (i = 0; text[i] != '\0'; ++i) {
    if (!append_output_char(context, buffer, length, capacity, text[i])) {
      return 0;
    }
  }
  return 1;
}

static int append_utf8_codepoint(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity,
    uint32_t codepoint) {
  if (codepoint <= 0x7fu) {
    return append_output_char(context, buffer, length, capacity, (char)codepoint);
  }
  if (codepoint <= 0x7ffu) {
    return append_output_char(
               context, buffer, length, capacity, (char)(0xc0u | ((codepoint >> 6) & 0x1fu))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | (codepoint & 0x3fu)));
  }
  if (codepoint <= 0xffffu) {
    if (codepoint >= 0xd800u && codepoint <= 0xdfffu) {
      return 1;
    }
    return append_output_char(
               context, buffer, length, capacity, (char)(0xe0u | ((codepoint >> 12) & 0x0fu))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | (codepoint & 0x3fu)));
  }
  if (codepoint <= 0x10ffffu) {
    return append_output_char(
               context, buffer, length, capacity, (char)(0xf0u | ((codepoint >> 18) & 0x07u))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | ((codepoint >> 12) & 0x3fu))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
           append_output_char(
               context, buffer, length, capacity, (char)(0x80u | (codepoint & 0x3fu)));
  }
  return append_output_char(context, buffer, length, capacity, '?');
}

static int decode_pdf_string_buffer(
    nanopdf_context* context,
    const char* input,
    size_t input_length,
    int decode_pdfdoc_encoding,
    char** out_text) {
  char* buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t pos = 0;

  *out_text = NULL;
  if (input_length >= 2 &&
      (uint8_t)input[0] == 0xfeu &&
      (uint8_t)input[1] == 0xffu) {
    pos = 2;
    while (pos + 1 < input_length) {
      uint16_t code = (uint16_t)(((uint16_t)(uint8_t)input[pos] << 8) |
                                 (uint16_t)(uint8_t)input[pos + 1]);
      pos += 2;
      if (code >= 0xd800u && code <= 0xdbffu) {
        if (pos + 1 >= input_length) {
          break;
        }
        {
          uint16_t low = (uint16_t)(((uint16_t)(uint8_t)input[pos] << 8) |
                                    (uint16_t)(uint8_t)input[pos + 1]);
          pos += 2;
          if (low >= 0xdc00u && low <= 0xdfffu) {
            uint32_t full = 0x10000u +
                ((((uint32_t)code - 0xd800u) << 10) | ((uint32_t)low - 0xdc00u));
            if (!append_utf8_codepoint(context, &buffer, &length, &capacity, full)) {
              nanopdf__allocator_free(&context->allocator, buffer);
              return 0;
            }
          }
        }
        continue;
      }
      if (!append_utf8_codepoint(context, &buffer, &length, &capacity, code)) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return 0;
      }
    }
  } else {
    static const uint16_t pdfdoc_special[0xa1] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0x02d8, 0x02c7, 0x02c6, 0x02d9, 0x02dd, 0x02db, 0x02da, 0x02dc,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044,
        0x2039, 0x203a, 0x2212, 0x2030, 0x201e, 0x201c, 0x201d, 0x2018,
        0x2019, 0x201a, 0x2122, 0xfb01, 0xfb02, 0x0141, 0x0152, 0x0160,
        0x0178, 0x017d, 0x0131, 0x0142, 0x0153, 0x0161, 0x017e, 0,
        0x20ac};
    for (pos = 0; pos < input_length; ++pos) {
      uint8_t ch = (uint8_t)input[pos];
      uint32_t codepoint = ch;
      if (decode_pdfdoc_encoding) {
        if (ch <= 0xa0u && pdfdoc_special[ch] != 0) {
          codepoint = pdfdoc_special[ch];
        } else if (ch >= 0xa1u) {
          codepoint = ch;
        }
        if (!append_utf8_codepoint(context, &buffer, &length, &capacity, codepoint)) {
          nanopdf__allocator_free(&context->allocator, buffer);
          return 0;
        }
      } else {
        if (!append_output_char(context, &buffer, &length, &capacity, input[pos])) {
          nanopdf__allocator_free(&context->allocator, buffer);
          return 0;
        }
      }
    }
  }

  if (!buffer) {
    buffer = (char*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return 0;
    }
    buffer[0] = '\0';
  }
  *out_text = buffer;
  return 1;
}

static int append_output_space_if_needed(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity) {
  if (*length > 0 && (*buffer)[*length - 1] != ' ' && (*buffer)[*length - 1] != '\n') {
    return append_output_char(context, buffer, length, capacity, ' ');
  }
  return 1;
}

static int append_output_newline_if_needed(
    nanopdf_context* context,
    char** buffer,
    size_t* length,
    size_t* capacity) {
  if (*length > 0 && (*buffer)[*length - 1] != '\n') {
    return append_output_char(context, buffer, length, capacity, '\n');
  }
  return 1;
}

static int extract_literal_string(
    const char* data,
    size_t size,
    size_t* pos,
    char** out_text,
    nanopdf_context* context) {
  size_t cursor = *pos;
  size_t capacity = 0;
  size_t length = 0;
  char* buffer = NULL;
  int depth = 0;

  if (cursor >= size || data[cursor] != '(') {
    return 0;
  }

  cursor++;
  depth = 1;
  while (cursor < size && depth > 0) {
    char ch = data[cursor++];
    if (ch == '\\' && cursor < size) {
      char escaped = data[cursor++];
      if (escaped >= '0' && escaped <= '7') {
        int value = escaped - '0';
        int count = 1;
        while (cursor < size && count < 3 &&
               data[cursor] >= '0' && data[cursor] <= '7') {
          value = value * 8 + (data[cursor] - '0');
          cursor++;
          count++;
        }
        ch = (char)(value & 0xff);
      } else if (escaped == '\r') {
        if (cursor < size && data[cursor] == '\n') {
          cursor++;
        }
        continue;
      } else if (escaped == '\n') {
        continue;
      } else if (escaped == 'n') ch = '\n';
      else if (escaped == 'r') ch = '\r';
      else if (escaped == 't') ch = '\t';
      else if (escaped == 'b') ch = '\b';
      else if (escaped == 'f') ch = '\f';
      else ch = escaped;
      if (depth == 1 && !append_output_char(context, &buffer, &length, &capacity, ch)) {
        nanopdf__allocator_free(&context->allocator, buffer);
        return -1;
      }
      continue;
    }
    if (ch == '(') {
      depth++;
      if (depth == 2 && !append_output_char(context, &buffer, &length, &capacity, '(')) {
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
    if (depth == 1 && !append_output_char(context, &buffer, &length, &capacity, ch)) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return -1;
    }
  }

  if (!buffer) {
    buffer = (char*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return -1;
    }
    buffer[0] = '\0';
  }

  if (depth != 0) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return 0;
  }
  *pos = cursor;
  if (!decode_pdf_string_buffer(context, buffer, length, 0, out_text)) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return -1;
  }
  nanopdf__allocator_free(&context->allocator, buffer);
  return 1;
}

static int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static int extract_hex_string(
    const char* data,
    size_t size,
    size_t* pos,
    char** out_text,
    nanopdf_context* context) {
  size_t cursor = *pos;
  size_t capacity = 0;
  size_t length = 0;
  char* buffer = NULL;
  int hi = -1;

  if (cursor + 1 >= size || data[cursor] != '<' || data[cursor + 1] == '<') {
    return 0;
  }
  cursor++;

  while (cursor < size && data[cursor] != '>') {
    int v;
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
      char ch = (char)((hi << 4) | v);
      if (!append_output_char(context, &buffer, &length, &capacity, ch)) {
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

  if (hi >= 0) {
    char ch = (char)(hi << 4);
    if (!append_output_char(context, &buffer, &length, &capacity, ch)) {
      nanopdf__allocator_free(&context->allocator, buffer);
      return -1;
    }
  }

  if (!buffer) {
    buffer = (char*)nanopdf__allocator_alloc(&context->allocator, 1);
    if (!buffer) {
      return -1;
    }
    buffer[0] = '\0';
  }

  *pos = cursor;
  if (!decode_pdf_string_buffer(context, buffer, length, 0, out_text)) {
    nanopdf__allocator_free(&context->allocator, buffer);
    return -1;
  }
  nanopdf__allocator_free(&context->allocator, buffer);
  return 1;
}

static int skip_number_token(const char* data, size_t size, size_t* pos) {
  double ignored = 0.0;
  size_t cursor = *pos;
  skip_ws(data, size, &cursor);
  if (!parse_number_token(data, size, &cursor, &ignored)) {
    return 0;
  }
  *pos = cursor;
  return 1;
}

static int text_show_operator_after(const char* data, size_t size, size_t pos) {
  skip_ws(data, size, &pos);
  if (pos >= size) {
    return 0;
  }
  if (data[pos] == '\'' || data[pos] == '"') {
    return 1;
  }
  if (pos + 1 < size && data[pos] == 'T' &&
      (data[pos + 1] == 'j' || data[pos + 1] == 'J')) {
    return 1;
  }
  return 0;
}

static nanopdf_status extract_text_from_array(
    nanopdf_context* context,
    const char* stream,
    size_t length,
    size_t* pos,
    char** buffer,
    size_t* buffer_length,
    size_t* buffer_capacity) {
  size_t cursor = *pos;
  char* array_buffer = NULL;
  size_t array_length = 0;
  size_t array_capacity = 0;

  if (cursor >= length || stream[cursor] != '[') {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "invalid TJ array");
  }
  cursor++;

  while (cursor < length) {
    skip_ws(stream, length, &cursor);
    if (cursor < length && stream[cursor] == ']') {
      cursor++;
      if (text_show_operator_after(stream, length, cursor) && array_buffer) {
        if (!append_output_space_if_needed(
                context, buffer, buffer_length, buffer_capacity) ||
            !append_output_text(
                context, buffer, buffer_length, buffer_capacity, array_buffer)) {
          nanopdf__allocator_free(&context->allocator, array_buffer);
          return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow TJ output");
        }
      }
      nanopdf__allocator_free(&context->allocator, array_buffer);
      *pos = cursor;
      return clear_status(context);
    }

    if (cursor < length && stream[cursor] == '(') {
      char* literal = NULL;
      int result = extract_literal_string(stream, length, &cursor, &literal, context);
      if (result < 0) {
        nanopdf__allocator_free(&context->allocator, array_buffer);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to extract TJ string");
      }
      if (result > 0 && literal) {
        if (!append_output_space_if_needed(
                context, &array_buffer, &array_length, &array_capacity) ||
            !append_output_text(
                context, &array_buffer, &array_length, &array_capacity, literal)) {
          nanopdf__allocator_free(&context->allocator, literal);
          nanopdf__allocator_free(&context->allocator, array_buffer);
          return set_status(
              context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow TJ array output");
        }
        nanopdf__allocator_free(&context->allocator, literal);
      }
      if (result == 0) {
        break;
      }
      continue;
    }

    if (cursor < length && stream[cursor] == '<' &&
        !(cursor + 1 < length && stream[cursor + 1] == '<')) {
      char* literal = NULL;
      int result = extract_hex_string(stream, length, &cursor, &literal, context);
      if (result < 0) {
        nanopdf__allocator_free(&context->allocator, array_buffer);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to extract TJ hex string");
      }
      if (result > 0 && literal) {
        if (!append_output_space_if_needed(
                context, &array_buffer, &array_length, &array_capacity) ||
            !append_output_text(
                context, &array_buffer, &array_length, &array_capacity, literal)) {
          nanopdf__allocator_free(&context->allocator, literal);
          nanopdf__allocator_free(&context->allocator, array_buffer);
          return set_status(
              context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow TJ array output");
        }
        nanopdf__allocator_free(&context->allocator, literal);
      }
      if (result == 0) {
        break;
      }
      continue;
    }

    if (!skip_number_token(stream, length, &cursor)) {
      break;
    }
  }

  nanopdf__allocator_free(&context->allocator, array_buffer);
  return set_status(context, NANOPDF_STATUS_MALFORMED, "unterminated TJ array");
}

static nanopdf_status extract_text_from_stream(
    nanopdf_context* context,
    const char* stream,
    size_t length,
    char** out_text) {
  char* output = NULL;
  size_t output_length = 0;
  size_t output_capacity = 0;
  size_t pos = 0;

  while (pos < length) {
    if (stream[pos] == '(') {
      char* literal = NULL;
      int result = extract_literal_string(stream, length, &pos, &literal, context);
      if (result < 0) {
        nanopdf__allocator_free(&context->allocator, output);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to extract text");
      }
      if (result > 0 && literal) {
        if (text_show_operator_after(stream, length, pos) &&
            (!append_output_space_if_needed(
                 context, &output, &output_length, &output_capacity) ||
             !append_output_text(context, &output, &output_length, &output_capacity, literal))) {
          nanopdf__allocator_free(&context->allocator, literal);
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow text output");
        }
        nanopdf__allocator_free(&context->allocator, literal);
      }
      if (result == 0) {
        pos++;
      }
      continue;
    }
    if (stream[pos] == '<' && !(pos + 1 < length && stream[pos + 1] == '<')) {
      char* literal = NULL;
      int result = extract_hex_string(stream, length, &pos, &literal, context);
      if (result < 0) {
        nanopdf__allocator_free(&context->allocator, output);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to extract hex text");
      }
      if (result > 0 && literal) {
        if (text_show_operator_after(stream, length, pos) &&
            (!append_output_space_if_needed(
                 context, &output, &output_length, &output_capacity) ||
             !append_output_text(context, &output, &output_length, &output_capacity, literal))) {
          nanopdf__allocator_free(&context->allocator, literal);
          nanopdf__allocator_free(&context->allocator, output);
          return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow text output");
        }
        nanopdf__allocator_free(&context->allocator, literal);
      }
      if (result == 0) {
        pos++;
      }
      continue;
    }
    if (stream[pos] == '[') {
      nanopdf_status array_status = extract_text_from_array(
          context, stream, length, &pos, &output, &output_length, &output_capacity);
      if (array_status == NANOPDF_STATUS_OK) {
        continue;
      }
      if (array_status != NANOPDF_STATUS_MALFORMED) {
        nanopdf__allocator_free(&context->allocator, output);
        return array_status;
      }
    }

    if ((stream[pos] == 'T' && pos + 1 < length &&
         (stream[pos + 1] == '*' || stream[pos + 1] == 'd')) ||
        (stream[pos] == '\'' || stream[pos] == '"')) {
      if (!append_output_newline_if_needed(
              context, &output, &output_length, &output_capacity)) {
        nanopdf__allocator_free(&context->allocator, output);
        return set_status(context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to grow text output");
      }
    }
    pos++;
  }

  if (!output) {
    return nanopdf__copy_owned_string(context, "", out_text);
  }

  *out_text = output;
  return clear_status(context);
}

nanopdf_status nanopdf_basic_document_extract_text(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    uint32_t page_index,
    char** out_text) {
  const nanopdf_basic_page* page = NULL;
  char* combined_stream = NULL;
  size_t combined_length = 0;
  size_t combined_capacity = 0;
  size_t i;

  if (!context || !document || !out_text) {
    return set_status(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid basic text extraction arguments");
  }

  *out_text = NULL;
  if ((size_t)page_index >= document->page_count) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "page index is out of range");
  }

  page = &document->pages[page_index];
  if (page->content_count == 0) {
    return nanopdf__copy_owned_string(context, "", out_text);
  }

  for (i = 0; i < page->content_count; ++i) {
    uint8_t* decoded = NULL;
    size_t stream_length = 0;
    const nanopdf_basic_content_ref* ref = &page->contents[i];
    nanopdf_status load_status = NANOPDF_STATUS_OK;

    if (ref->kind == 1) {
      stream_length = ref->decoded_size;
      decoded = ref->decoded_data;
    } else {
      nanopdf_basic_object content_object;
      nanopdf_basic_object_init(&content_object);
      if (!ref->valid || ref->object_number >= document->xref_count ||
          !document->xrefs[ref->object_number].present ||
          !document->xrefs[ref->object_number].in_use) {
        nanopdf__allocator_free(&context->allocator, combined_stream);
        return set_status(
            context, NANOPDF_STATUS_UNSUPPORTED, "page content reference is unsupported");
      }

      load_status = nanopdf_basic_load_object(
          context,
          document,
          (nanopdf_basic_ref){ref->object_number, ref->generation, ref->valid},
          &content_object);
      if (load_status != NANOPDF_STATUS_OK ||
          content_object.type != NANOPDF_BASIC_OBJECT_STREAM) {
        nanopdf_basic_object_destroy(&context->allocator, &content_object);
        nanopdf__allocator_free(&context->allocator, combined_stream);
        return set_status(
            context, NANOPDF_STATUS_UNSUPPORTED, "page content stream is unsupported");
      }
      load_status = nanopdf_basic_decode_stream_with_document(
          context, document, &content_object, &decoded, &stream_length);
      nanopdf_basic_object_destroy(&context->allocator, &content_object);
      if (load_status != NANOPDF_STATUS_OK) {
        nanopdf__allocator_free(&context->allocator, combined_stream);
        nanopdf__allocator_free(&context->allocator, decoded);
        return load_status;
      }
    }

    while (combined_length + stream_length + 2 > combined_capacity) {
      size_t new_capacity = combined_capacity == 0 ? 128 : combined_capacity * 2;
      char* resized = (char*)nanopdf__allocator_realloc(
          &context->allocator, combined_stream, new_capacity);
      if (!resized) {
        nanopdf__allocator_free(&context->allocator, combined_stream);
        nanopdf__allocator_free(&context->allocator, decoded);
        return set_status(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to combine page content streams");
      }
      combined_stream = resized;
      combined_capacity = new_capacity;
    }

    memcpy(combined_stream + combined_length, decoded, stream_length);
    combined_length += stream_length;
    combined_stream[combined_length++] = '\n';
    combined_stream[combined_length] = '\0';
    if (ref->kind != 1) {
      nanopdf__allocator_free(&context->allocator, decoded);
    }
  }

  if (!combined_stream) {
    return nanopdf__copy_owned_string(context, "", out_text);
  }

  {
    nanopdf_status status =
        extract_text_from_stream(context, combined_stream, combined_length, out_text);
    nanopdf__allocator_free(&context->allocator, combined_stream);
    return status;
  }
}
