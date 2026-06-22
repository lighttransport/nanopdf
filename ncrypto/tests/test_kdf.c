/* SPDX-License-Identifier: Apache-2.0
 * Known-answer tests for the ncrypto HMAC / HKDF / PBKDF2. */
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_kdf.h"
#include "ncrypto/nc_test.h"

int main(void) {
  /* nc_prf_len sanity. */
  NC_CHECK(nc_prf_len(NC_PRF_SHA1) == 20);
  NC_CHECK(nc_prf_len(NC_PRF_SHA256) == 32);
  NC_CHECK(nc_prf_len(NC_PRF_SHA384) == 48);
  NC_CHECK(nc_prf_len(NC_PRF_SHA512) == 64);

  /* HMAC-SHA256, RFC 4231 TC1. */
  {
    uint8_t key[20];
    uint8_t out[NC_SHA256_LEN];
    memset(key, 0x0b, sizeof(key));
    nc_hmac(NC_PRF_SHA256, key, sizeof(key), (const uint8_t*)"Hi There", 8, out);
    NC_CHECK_EQ_HEX(
        out, NC_SHA256_LEN,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  }

  /* HKDF-SHA256, RFC 5869 TC1. */
  {
    uint8_t ikm[22];
    uint8_t salt[13];
    uint8_t info[10];
    uint8_t prk[NC_SHA256_LEN];
    uint8_t okm[42];
    size_t prklen;
    memset(ikm, 0x0b, sizeof(ikm));
    nc_test_from_hex("000102030405060708090a0b0c", salt);
    nc_test_from_hex("f0f1f2f3f4f5f6f7f8f9", info);

    prklen = nc_hkdf_extract(NC_PRF_SHA256, salt, sizeof(salt), ikm, sizeof(ikm),
                             prk);
    NC_CHECK(prklen == NC_SHA256_LEN);
    NC_CHECK_EQ_HEX(
        prk, NC_SHA256_LEN,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");

    NC_CHECK(nc_hkdf_expand(NC_PRF_SHA256, prk, prklen, info, sizeof(info), okm,
                            sizeof(okm)) == 0);
    NC_CHECK_EQ_HEX(okm, sizeof(okm),
                    "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56e"
                    "cc4c5bf34007208d5b887185865");
  }

  /* PBKDF2-HMAC-SHA1, RFC 6070. */
  {
    uint8_t dk[20];
    NC_CHECK(nc_pbkdf2(NC_PRF_SHA1, (const uint8_t*)"password", 8,
                       (const uint8_t*)"salt", 4, 1, dk, sizeof(dk)) == 0);
    NC_CHECK_EQ_HEX(dk, sizeof(dk),
                    "0c60c80f961f0e71f3a9b524af6012062fe037a6");

    NC_CHECK(nc_pbkdf2(NC_PRF_SHA1, (const uint8_t*)"password", 8,
                       (const uint8_t*)"salt", 4, 4096, dk, sizeof(dk)) == 0);
    NC_CHECK_EQ_HEX(dk, sizeof(dk),
                    "4b007901b765489abead49d926f721d065a429c1");
  }

  /* HKDF-Expand-Label smoke test: must succeed and be deterministic. */
  {
    uint8_t secret[NC_SHA256_LEN];
    uint8_t a[16];
    uint8_t b[16];
    memset(secret, 0x42, sizeof(secret));
    NC_CHECK(nc_hkdf_expand_label(NC_PRF_SHA256, secret, sizeof(secret), "key",
                                  NULL, 0, a, sizeof(a)) == 0);
    NC_CHECK(nc_hkdf_expand_label(NC_PRF_SHA256, secret, sizeof(secret), "key",
                                  NULL, 0, b, sizeof(b)) == 0);
    NC_CHECK(memcmp(a, b, sizeof(a)) == 0);
  }

  return nc_test_report();
}
