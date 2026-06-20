/* SPDX-License-Identifier: Apache-2.0
 * PKCS#12 (PFX) parsing: extract the private key + certificates from a .p12,
 * decrypting PBES2/AES SafeBags. (No 3DES/RC2; MAC not verified.) */
#ifndef NCRYPTO_NC_PKCS12_H_
#define NCRYPTO_NC_PKCS12_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_rsa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PKCS12_MAX_CERTS 16

typedef struct {
  int ok;
  nc_rsa_privkey key;      /* the private key (key.valid if found) */
  uint8_t* certs[NC_PKCS12_MAX_CERTS];  /* owned DER cert copies; signer first */
  size_t cert_lens[NC_PKCS12_MAX_CERTS];
  int cert_count;
  char error[160];
} nc_pkcs12_bundle;

/* Parse a PKCS#12 @data with @password. Returns 0 on success (bundle->ok set).
 * Call nc_pkcs12_bundle_free when done. */
int nc_pkcs12_parse(nc_pkcs12_bundle* bundle, const uint8_t* data, size_t len,
                    const char* password);
void nc_pkcs12_bundle_free(nc_pkcs12_bundle* bundle);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_PKCS12_H_ */
