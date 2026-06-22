/* SPDX-License-Identifier: Apache-2.0
 * AES-128/256 block cipher with GCM (AEAD) and CBC modes. */
#ifndef NCRYPTO_NC_AES_H_
#define NCRYPTO_NC_AES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t rk[60];  /* expanded round keys */
  int rounds;       /* 10 (AES-128) or 14 (AES-256) */
} nc_aes_ctx;

/* keylen must be 16 (AES-128) or 32 (AES-256). Returns 0 on success. */
int nc_aes_init(nc_aes_ctx* c, const uint8_t* key, size_t keylen);
void nc_aes_encrypt_block(const nc_aes_ctx* c, const uint8_t in[16],
                          uint8_t out[16]);
void nc_aes_decrypt_block(const nc_aes_ctx* c, const uint8_t in[16],
                          uint8_t out[16]);

/* AES-GCM with a 12-byte IV. @keylen is 16 or 32.
 * seal: encrypts ptlen bytes pt -> ct (ptlen bytes) and writes the 16-byte tag.
 * open: decrypts ctlen bytes ct -> pt; returns 1 if the tag verifies, else 0
 *       (and pt must not be used).
 * ct may alias pt. aad may be NULL when aadlen == 0. */
void nc_aes_gcm_seal(const uint8_t* key, size_t keylen, const uint8_t iv[12],
                     const uint8_t* aad, size_t aadlen, const uint8_t* pt,
                     size_t ptlen, uint8_t* ct, uint8_t tag[16]);
int nc_aes_gcm_open(const uint8_t* key, size_t keylen, const uint8_t iv[12],
                    const uint8_t* aad, size_t aadlen, const uint8_t* ct,
                    size_t ctlen, const uint8_t tag[16], uint8_t* pt);

/* AES-CBC with a 16-byte IV; @inlen must be a multiple of 16. No padding is
 * applied/removed. Returns 0 on success. out may alias in. */
int nc_aes_cbc_encrypt(const uint8_t* key, size_t keylen, const uint8_t iv[16],
                       const uint8_t* in, size_t inlen, uint8_t* out);
int nc_aes_cbc_decrypt(const uint8_t* key, size_t keylen, const uint8_t iv[16],
                       const uint8_t* in, size_t inlen, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_AES_H_ */
