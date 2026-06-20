/* SPDX-License-Identifier: Apache-2.0
 * HMAC, HKDF (incl. TLS 1.3 HKDF-Expand-Label) and PBKDF2 over the ncrypto
 * SHA family. */
#ifndef NCRYPTO_NC_KDF_H_
#define NCRYPTO_NC_KDF_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NC_PRF_SHA1 = 1,
  NC_PRF_SHA256 = 2,
  NC_PRF_SHA384 = 3,
  NC_PRF_SHA512 = 4
} nc_prf;

/* Digest length (bytes) of a PRF. */
size_t nc_prf_len(nc_prf prf);

/* HMAC-<prf>. Writes the digest to out (>= nc_prf_len(prf) bytes); returns the
 * digest length. */
size_t nc_hmac(nc_prf prf, const uint8_t* key, size_t keylen,
               const uint8_t* msg, size_t msglen, uint8_t* out);

/* HKDF-Extract (RFC 5869). Writes a prf-length PRK to prk; returns its length. */
size_t nc_hkdf_extract(nc_prf prf, const uint8_t* salt, size_t saltlen,
                       const uint8_t* ikm, size_t ikmlen, uint8_t* prk);

/* HKDF-Expand (RFC 5869). Writes outlen bytes to out. Returns 0 on success. */
int nc_hkdf_expand(nc_prf prf, const uint8_t* prk, size_t prklen,
                   const uint8_t* info, size_t infolen, uint8_t* out,
                   size_t outlen);

/* TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1):
 *   HkdfLabel = len(2) || "tls13 "+label (1-byte len prefix) ||
 *               context (1-byte len prefix)
 * Writes outlen bytes to out. Returns 0 on success. */
int nc_hkdf_expand_label(nc_prf prf, const uint8_t* secret, size_t secretlen,
                         const char* label, const uint8_t* context,
                         size_t contextlen, uint8_t* out, size_t outlen);

/* PBKDF2 (RFC 8018). Writes outlen bytes of derived key. Returns 0 on success. */
int nc_pbkdf2(nc_prf prf, const uint8_t* pass, size_t passlen,
              const uint8_t* salt, size_t saltlen, uint32_t iterations,
              uint8_t* out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_KDF_H_ */
