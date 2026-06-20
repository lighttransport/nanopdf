/* SPDX-License-Identifier: Apache-2.0
 * CMS / PKCS#7 SignedData: build a detached RSA+SHA-256 signature (with signed
 * attributes and an optional RFC 3161 timestamp token) and verify one. */
#ifndef NCRYPTO_NC_CMS_H_
#define NCRYPTO_NC_CMS_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_rsa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timestamp callback: given the signature bytes (the value to be stamped),
 * append a DER RFC 3161 timeStampToken (a ContentInfo) to @token_out. Return 0
 * on success, non-zero to skip the timestamp. @user is passed through. */
typedef int (*nc_tsa_cb)(const uint8_t* signature, size_t sig_len,
                         nc_buf* token_out, void* user);

/* Build a detached CMS SignedData (adbe.pkcs7.detached style) over @content.
 * @signer_cert is the signer's DER certificate; @chain are extra certs.
 * @key is the signer's RSA private key. @signing_time_utc is "YYMMDDhhmmssZ".
 * If @tsa is non-NULL it is invoked to embed an unsigned timeStampToken
 * attribute. Writes the DER to @out. Returns 0 on success. */
int nc_cms_build_signed_data(nc_buf* out, const uint8_t* content,
                             size_t content_len, const uint8_t* signer_cert,
                             size_t signer_cert_len,
                             const uint8_t* const* chain,
                             const size_t* chain_lens, int chain_n,
                             const nc_rsa_privkey* key,
                             const char* signing_time_utc, nc_tsa_cb tsa,
                             void* tsa_user);

typedef struct {
  int parsed;            /* the CMS structure parsed */
  int signature_valid;   /* RSA signature over signedAttrs verified */
  int digest_valid;      /* messageDigest attr == SHA-256(content) */
  char signer_cn[256];
  char digest_algorithm[16];   /* e.g. "SHA-256" */
  char signing_time[32];       /* signingTime signed attribute (raw) */
  int has_timestamp;
  char timestamp_time[32];     /* RFC 3161 TSTInfo genTime (raw) */
  char timestamp_authority[256];
  char error[160];
} nc_cms_verify_info;

/* Verify a detached CMS SignedData @cms_der over @content (RSA + SHA-256). */
int nc_cms_verify_signed_data(nc_cms_verify_info* info, const uint8_t* cms_der,
                              size_t cms_len, const uint8_t* content,
                              size_t content_len);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_CMS_H_ */
