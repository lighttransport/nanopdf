#ifndef NANOPDF_CRYPTO_H_
#define NANOPDF_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>

typedef struct nanopdf_md5 {
  uint32_t state[4];
  uint64_t count;
  uint8_t buffer[64];
} nanopdf_md5;

typedef struct nanopdf_rc4 {
  uint8_t s[256];
  uint8_t i;
  uint8_t j;
} nanopdf_rc4;

typedef struct nanopdf_aes128 {
  uint8_t round_keys[11][16];
} nanopdf_aes128;

typedef struct nanopdf_aes256 {
  uint8_t round_keys[15][16];
} nanopdf_aes256;

void nanopdf_md5_init(nanopdf_md5* md5);
void nanopdf_md5_update(nanopdf_md5* md5, const uint8_t* data, size_t len);
void nanopdf_md5_final(nanopdf_md5* md5, uint8_t out[16]);
void nanopdf_md5_hash(const uint8_t* data, size_t len, uint8_t out[16]);
void nanopdf_sha256_hash(const uint8_t* data, size_t len, uint8_t out[32]);
void nanopdf_sha384_hash(const uint8_t* data, size_t len, uint8_t out[48]);
void nanopdf_sha512_hash(const uint8_t* data, size_t len, uint8_t out[64]);

void nanopdf_rc4_init(nanopdf_rc4* rc4, const uint8_t* key, size_t key_len);
void nanopdf_rc4_crypt(nanopdf_rc4* rc4, uint8_t* data, size_t len);

void nanopdf_aes128_set_key(nanopdf_aes128* aes, const uint8_t* key);
void nanopdf_aes128_encrypt_cbc(
    nanopdf_aes128* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]);
void nanopdf_aes128_decrypt_cbc(
    nanopdf_aes128* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]);

void nanopdf_aes256_set_key(nanopdf_aes256* aes, const uint8_t* key);
void nanopdf_aes256_encrypt_block(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out);
void nanopdf_aes256_encrypt_cbc(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]);
void nanopdf_aes256_decrypt_cbc(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]);

int nanopdf_pkcs7_unpad(
    const uint8_t* data,
    size_t len,
    size_t block_size,
    size_t* out_len);

#endif
