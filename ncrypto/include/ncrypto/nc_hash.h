/* SPDX-License-Identifier: Apache-2.0
 * SHA-1 / SHA-256 / SHA-384 / SHA-512 for the ncrypto C11 library. */
#ifndef NCRYPTO_NC_HASH_H_
#define NCRYPTO_NC_HASH_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_SHA1_LEN 20
#define NC_SHA256_LEN 32
#define NC_SHA384_LEN 48
#define NC_SHA512_LEN 64

typedef struct {
  uint32_t state[5];
  uint64_t count;     /* total message bytes */
  uint8_t buf[64];
  size_t buflen;      /* bytes buffered (< 64) */
} nc_sha1_ctx;

typedef struct {
  uint32_t state[8];
  uint64_t count;
  uint8_t buf[64];
  size_t buflen;
} nc_sha256_ctx;

typedef struct {
  uint64_t state[8];
  uint64_t count;     /* total message bytes (low 64 bits is enough here) */
  uint8_t buf[128];
  size_t buflen;
} nc_sha512_ctx;        /* also used for SHA-384 (different IV, truncated) */

void nc_sha1_init(nc_sha1_ctx* c);
void nc_sha1_update(nc_sha1_ctx* c, const uint8_t* data, size_t len);
void nc_sha1_final(nc_sha1_ctx* c, uint8_t out[NC_SHA1_LEN]);
void nc_sha1(const uint8_t* data, size_t len, uint8_t out[NC_SHA1_LEN]);

void nc_sha256_init(nc_sha256_ctx* c);
void nc_sha256_update(nc_sha256_ctx* c, const uint8_t* data, size_t len);
void nc_sha256_final(nc_sha256_ctx* c, uint8_t out[NC_SHA256_LEN]);
void nc_sha256(const uint8_t* data, size_t len, uint8_t out[NC_SHA256_LEN]);

void nc_sha384_init(nc_sha512_ctx* c);
void nc_sha384_update(nc_sha512_ctx* c, const uint8_t* data, size_t len);
void nc_sha384_final(nc_sha512_ctx* c, uint8_t out[NC_SHA384_LEN]);
void nc_sha384(const uint8_t* data, size_t len, uint8_t out[NC_SHA384_LEN]);

void nc_sha512_init(nc_sha512_ctx* c);
void nc_sha512_update(nc_sha512_ctx* c, const uint8_t* data, size_t len);
void nc_sha512_final(nc_sha512_ctx* c, uint8_t out[NC_SHA512_LEN]);
void nc_sha512(const uint8_t* data, size_t len, uint8_t out[NC_SHA512_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_HASH_H_ */
