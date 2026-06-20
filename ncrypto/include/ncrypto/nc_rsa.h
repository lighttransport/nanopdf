/* SPDX-License-Identifier: Apache-2.0
 * RSA: PKCS#1 v1.5 sign/verify, RSASSA-PSS verify, and private-key parsing
 * (PKCS#1 / PKCS#8, incl. PBES2-encrypted PKCS#8). Built on nc_bigint. */
#ifndef NCRYPTO_NC_RSA_H_
#define NCRYPTO_NC_RSA_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_bigint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  nc_bigint n, e;
  size_t modulus_bytes;  /* ceil(bitlen(n)/8) */
  int valid;
} nc_rsa_pubkey;

typedef struct {
  nc_bigint n, e, d;
  size_t modulus_bytes;
  int valid;
} nc_rsa_privkey;

/* Parse a DER PKCS#1 RSAPrivateKey or a PKCS#8 PrivateKeyInfo wrapping one.
 * Returns 0 on success (key->valid set), -1 on failure. */
int nc_rsa_parse_privkey_der(nc_rsa_privkey* key, const uint8_t* der,
                             size_t len);

/* Parse a PEM private key: "RSA PRIVATE KEY", "PRIVATE KEY", or an
 * "ENCRYPTED PRIVATE KEY" (PKCS#8 PBES2: PBKDF2 + AES-CBC) decrypted with
 * @password (may be NULL for unencrypted). Returns 0 on success, -1 on failure. */
int nc_rsa_parse_privkey_pem(nc_rsa_privkey* key, const char* pem,
                             const char* password);

/* Decrypt a PKCS#8 EncryptedPrivateKeyInfo (PBES2) to the inner PrivateKeyInfo
 * DER, written to out (up to outcap). Returns the plaintext length, or -1. */
int nc_pbes2_decrypt_pkcs8(const uint8_t* der, size_t len, const char* password,
                           uint8_t* out, size_t outcap);

/* Low-level PBES2: given the PBES2 AlgorithmIdentifier *content* bytes
 * (the OID + params inside the AlgorithmIdentifier SEQUENCE) and the
 * ciphertext, decrypt with @password. Returns plaintext length (PKCS#7 unpad
 * applied) or -1. Shared by PKCS#8 and PKCS#12. */
int nc_pbes2_decrypt(const uint8_t* algid, size_t algidlen, const uint8_t* enc,
                     size_t enclen, const char* password, uint8_t* out,
                     size_t outcap);

/* Sign a precomputed DigestInfo (the ASN.1 prefix + raw hash) with PKCS#1 v1.5.
 * Writes modulus_bytes of signature to sig; returns its length or -1. */
int nc_rsa_sign_pkcs1v15(const nc_rsa_privkey* key, const uint8_t* digest_info,
                         size_t di_len, uint8_t* sig);
/* Convenience: prepend the SHA-256 DigestInfo to hash[32] and sign. */
int nc_rsa_sign_sha256(const nc_rsa_privkey* key, const uint8_t hash[32],
                       uint8_t* sig);

/* Verify a PKCS#1 v1.5 signature against an expected DigestInfo. Returns 1 if
 * valid, 0 otherwise. */
int nc_rsa_verify_pkcs1v15(const nc_rsa_pubkey* key, const uint8_t* sig,
                           size_t sig_len, const uint8_t* digest_info,
                           size_t di_len);

/* Verify an RSASSA-PSS signature (EMSA-PSS-VERIFY, MGF1, auto salt length).
 * @mhash is the message hash of length @hlen (32/48/64 -> SHA-256/384/512).
 * Returns 1 if valid, 0 otherwise. */
int nc_rsa_verify_pss(const nc_rsa_pubkey* key, const uint8_t* sig,
                      size_t sig_len, const uint8_t* mhash, size_t hlen);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_RSA_H_ */
