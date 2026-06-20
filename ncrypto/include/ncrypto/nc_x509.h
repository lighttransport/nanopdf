/* SPDX-License-Identifier: Apache-2.0
 * X.509 certificate parsing + chain verification (RSA PKCS#1/PSS, ECDSA
 * P-256/P-384). Parsed certs reference the caller's DER buffer (zero-copy):
 * keep the DER alive for the lifetime of the nc_x509_cert. */
#ifndef NCRYPTO_NC_X509_H_
#define NCRYPTO_NC_X509_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_ecc.h"
#include "ncrypto/nc_rsa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NC_SIG_UNKNOWN = 0,
  NC_SIG_RSA_PKCS1_SHA1,
  NC_SIG_RSA_PKCS1_SHA256,
  NC_SIG_RSA_PKCS1_SHA384,
  NC_SIG_RSA_PKCS1_SHA512,
  NC_SIG_RSA_PSS,
  NC_SIG_ECDSA_SHA1,
  NC_SIG_ECDSA_SHA256,
  NC_SIG_ECDSA_SHA384,
  NC_SIG_ECDSA_SHA512
} nc_sig_alg;

typedef enum { NC_KEY_UNKNOWN = 0, NC_KEY_RSA, NC_KEY_EC } nc_key_type;

#define NC_X509_MAX_SAN 24

typedef struct {
  int parsed;

  const uint8_t* tbs;        /* raw TBSCertificate (tag..value) */
  size_t tbs_len;
  nc_sig_alg sig_alg;
  size_t pss_hash_len;       /* for RSA-PSS: message/MGF hash length */
  const uint8_t* sig;        /* signatureValue (unused-bits byte stripped) */
  size_t sig_len;

  const uint8_t* issuer;     /* DER of issuer Name */
  size_t issuer_len;
  const uint8_t* subject;    /* DER of subject Name */
  size_t subject_len;

  int64_t not_before;        /* epoch seconds */
  int64_t not_after;

  nc_key_type key_type;
  nc_rsa_pubkey rsa_pub;             /* valid if key_type == NC_KEY_RSA */
  const nc_ec_curve* ec_curve;       /* if key_type == NC_KEY_EC */
  const uint8_t* ec_point;           /* uncompressed point 0x04||X||Y */
  size_t ec_point_len;

  const char* san[NC_X509_MAX_SAN];  /* dNSName entries (into the DER) */
  size_t san_len[NC_X509_MAX_SAN];
  int san_count;
  int is_ca;
} nc_x509_cert;

/* Parse a DER certificate. Returns 0 on success (c->parsed set), -1 on failure.
 * @der must outlive @c. */
int nc_x509_parse(nc_x509_cert* c, const uint8_t* der, size_t len);

/* Subject commonName into @out (NUL-terminated); returns its length or 0. */
int nc_x509_subject_cn(const nc_x509_cert* c, char* out, size_t outcap);
int nc_x509_self_issued(const nc_x509_cert* c);

/* Verify @child's signature with @issuer's public key. Returns 1 if valid. */
int nc_x509_verify_signature(const nc_x509_cert* child,
                             const nc_x509_cert* issuer);

/* Verify a TLS 1.3 CertificateVerify-style signature (@sig over @msg) with
 * @leaf's key, dispatched by SignatureScheme @scheme. Returns 1 if valid. */
int nc_x509_verify_tls13(const nc_x509_cert* leaf, uint16_t scheme,
                         const uint8_t* msg, size_t msg_len, const uint8_t* sig,
                         size_t sig_len);

/* ---- trust store ---------------------------------------------------------- */
typedef struct {
  nc_x509_cert* roots;     /* parsed root certs */
  uint8_t** root_der;      /* owned DER copies the roots point into */
  size_t* root_der_len;
  int count;
  int loaded;
} nc_trust_store;

/* Load the system CA bundle (@path NULL -> autodetect common locations).
 * Returns 0 on success. Call nc_trust_store_free when done. */
int nc_trust_store_load(nc_trust_store* s, const char* path);
void nc_trust_store_free(nc_trust_store* s);

typedef struct {
  int ok;
  char error[192];
  char subject_cn[256];
} nc_verify_result;

/* Verify a server cert chain (@der_chain[0] = leaf; rest = intermediates, any
 * order) against @store at @now_epoch (0 to skip validity), requiring the leaf
 * to authorize @hostname (NULL/empty to skip; SAN dNSName + leftmost wildcard).
 * Returns 0 if the chain is trusted (res->ok also set), -1 otherwise. */
int nc_x509_verify_chain(nc_verify_result* res, const uint8_t* const* der_chain,
                         const size_t* der_lens, int n,
                         const nc_trust_store* store, const char* hostname,
                         int64_t now_epoch);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_X509_H_ */
