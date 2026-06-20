/* SPDX-License-Identifier: Apache-2.0
 * Tests for the PKCS#12 (PFX) parser. */

#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_pkcs12.h"
#include "ncrypto/nc_test.h"

/* PKCS#12, password "p12pass": 2048-bit RSA key + cert CN="nanopdf cms signer". */
static const char* P12_BASE64 =
    "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"
    "IL6fbVLJWMwztahGoopDBAjHe7QD5Vqu3wICCAA=";

/* Minimal base64 decoder. Returns the decoded byte count, -1 on error. */
static int b64_decode(const char* in, uint8_t* out, size_t outcap) {
  static const int8_t T_INIT = -1;
  int8_t tbl[256];
  int i;
  int bits = 0, acc = 0;
  size_t olen = 0;
  for (i = 0; i < 256; i++) tbl[i] = T_INIT;
  for (i = 0; i < 26; i++) {
    tbl[(int)('A' + i)] = (int8_t)i;
    tbl[(int)('a' + i)] = (int8_t)(26 + i);
  }
  for (i = 0; i < 10; i++) tbl[(int)('0' + i)] = (int8_t)(52 + i);
  tbl[(int)'+'] = 62;
  tbl[(int)'/'] = 63;

  for (; *in; in++) {
    unsigned char c = (unsigned char)*in;
    int v;
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      if (c == '=') break;
      continue;
    }
    v = tbl[c];
    if (v < 0) return -1;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (olen >= outcap) return -1;
      out[olen++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)olen;
}

int main(void) {
  static uint8_t der[16384];
  int derlen;
  nc_pkcs12_bundle b;

  /* nc_test.h's hex helpers are unused here; reference them to avoid warnings. */
  (void)nc_test_to_hex;
  (void)nc_test_from_hex;

  derlen = b64_decode(P12_BASE64, der, sizeof(der));
  NC_CHECK(derlen > 0);

  /* Correct password. */
  NC_CHECK(nc_pkcs12_parse(&b, der, (size_t)derlen, "p12pass") == 0);
  NC_CHECK(b.ok);
  NC_CHECK(b.key.valid);
  NC_CHECK(b.key.modulus_bytes == 256);
  NC_CHECK(b.cert_count >= 1);
  nc_pkcs12_bundle_free(&b);

  /* Wrong password: decryption must fail. */
  {
    nc_pkcs12_bundle bw;
    NC_CHECK(nc_pkcs12_parse(&bw, der, (size_t)derlen, "nope") == -1);
    NC_CHECK(!bw.ok);
    nc_pkcs12_bundle_free(&bw);
  }

  return nc_test_report();
}
