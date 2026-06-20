/* SPDX-License-Identifier: Apache-2.0
 * Tests for the ncrypto RSA module. */
#include <string.h>

#include "ncrypto/nc_bigint.h"
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_rsa.h"
#include "ncrypto/nc_test.h"

static const char kPkcs8Pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIICdgIBADANBgkqhkiG9w0BAQEFAASCAmAwggJcAgEAAoGBAJ9zoJtx3xa0MoY/\n"
    "PHLP60zbe8Zh+5hhGmAvIhm3AdGx+wvVr6eqA7dPAPDw9Jajii+Polpsn/nTVhT+\n"
    "B5cWYWkoFxlBDdOJNZ+uWEhQ5Hns6SOw0c4Q3PCY9wOEkgTF++XPwrv5R7PXuulr\n"
    "FGDir6/re+gnoZMsTsiNfatD08bDAgMBAAECgYAl0Y7uT3vSLrstDCKSOK5edFBP\n"
    "JT4/Tgird4JnBvjve39Ht08KPVDUUXCvtPbOI8vKrA6d09W9s1pfcEDsnOpNXsU2\n"
    "QCr4IsZeWGkh7logpLcLGaxyTB0U6RzYgXkOgPhSBiP5MgNwfvEO9dN6xGfU4Hm5\n"
    "lHpSjKNZeehwtKjcAQJBANGaUEWZ0y3mtJzgqxEWh3ikGS2RND0LyUDLlhss/v/l\n"
    "ihrC474xc4B0KjkE+vyRWhVUuU/VAoVB0IHJEwu9bgMCQQDCv17UsnLB1puCIFqW\n"
    "9Z80Pe3o+YUAOop0NFwt8ocfszqpcBxhnWQB354C+8G76gHnTeKWHu2OlC4+ARnp\n"
    "7khBAkBiDgHNfbfYelw2I7iDhvmbS5Fnys0YXeIpiNRRJEyq4/QmrtOiDzpMdoum\n"
    "HxlXfJwa7IbAvRKvymlDvhBF5rmHAkEAsS6/rrB0bBs+/NNG2FW8dSFrFS3/bcfV\n"
    "NAh3XW5stdCxLHtNtxolZPb4oio/hyJOfQ2Fe6PD6payw8LouscKgQJAQg92pwa8\n"
    "jCa5Co9j/i38Pkw7bsFaXSajA9TlWlmvsjwMZl32ShqxuPxHmTqfXVWDdyBYHr0Z\n"
    "PGthNcFmbmZAYQ==\n"
    "-----END PRIVATE KEY-----\n";

static const char kEncPem[] =
    "-----BEGIN ENCRYPTED PRIVATE KEY-----\n"
    "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\n"
    "-----END ENCRYPTED PRIVATE KEY-----\n";

static const uint8_t kSha256Prefix[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
    0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

static void test_sign_verify(void) {
  nc_rsa_privkey priv;
  NC_CHECK(nc_rsa_parse_privkey_pem(&priv, kPkcs8Pem, NULL) == 0);
  NC_CHECK(priv.valid == 1);
  NC_CHECK(priv.modulus_bytes == 128); /* 1024-bit key */

  /* Build a public key from the private key. */
  nc_rsa_pubkey pub;
  memset(&pub, 0, sizeof(pub));
  nc_bi_copy(&pub.n, &priv.n);
  nc_bi_copy(&pub.e, &priv.e);
  pub.modulus_bytes = priv.modulus_bytes;
  pub.valid = 1;

  uint8_t hash[32];
  for (int i = 0; i < 32; ++i) hash[i] = (uint8_t)(i * 7 + 1);

  uint8_t sig[128];
  int siglen = nc_rsa_sign_sha256(&priv, hash, sig);
  NC_CHECK(siglen == 128);

  /* PKCS#1 v1.5 signing is deterministic: signing again yields the same bytes.
   * (Also exercises the header's hex helper.) */
  uint8_t sig2[128];
  char sighex[257];
  NC_CHECK(nc_rsa_sign_sha256(&priv, hash, sig2) == 128);
  nc_test_to_hex(sig, 128, sighex);
  NC_CHECK_EQ_HEX(sig2, 128, sighex);

  uint8_t di[sizeof(kSha256Prefix) + 32];
  memcpy(di, kSha256Prefix, sizeof(kSha256Prefix));
  memcpy(di + sizeof(kSha256Prefix), hash, 32);
  NC_CHECK(nc_rsa_verify_pkcs1v15(&pub, sig, (size_t)siglen, di, sizeof(di)) ==
           1);

  /* Tamper the digest -> verification must fail. */
  di[sizeof(kSha256Prefix)] ^= 0xFF;
  NC_CHECK(nc_rsa_verify_pkcs1v15(&pub, sig, (size_t)siglen, di, sizeof(di)) ==
           0);
}

static void test_pss_kat(void) {
  static const char* n_hex =
      "E3C699F557C5ACC4D3B24D5E053C3E6916E9A83EA0ACB667955583F85C92B12C"
      "442F6BD7E4BE9E3320E7E39253FA00EB81FED4D7DBD168D6CE4819F4F9784A94"
      "A6D7520B071432F652D5BAE2C59E7E8EA1D846B5525107BDD57B536723F9E62D"
      "0B2EE9DB8FBC30BD4C1FDF6C21021E9485864BED0838036E57EFB27F1DF246B2"
      "889838383F6A3166DDC51C6F594D70E876EFC32EB79ED9E1FD2D69D88A33332B"
      "19988CA82D7AFB2EB2629544CA359F3BDBA9E9F6F73CC47656274C8F039ECC87"
      "4A2BC8780D54776CFD8EC3E787A8BA9950493CF92C02E6DAE91278F5CF1DEABC"
      "857EF8669E748CB18F99767ECF08F0C674B41F1DD5DC3AE3A0EEDD5A35F0B213";
  static const char* sig_hex =
      "c6ee7c434bdebcbba7e34a2fe0fd3d1d5956906aa25f2a035ad4cbf27cbd4427"
      "a090a2e4fbb26ab090e634741c6c742d4be1217c5cf7cca4f9dbd11bfc3f7f6d"
      "784ef7e52784f2d2bea59935acf214cab658d7e1c1ae6c9ac6034f1e4685c908"
      "2e7577fdbd8fea3df8a198fc8fa59faaf138eec23ca5679efb8735ffb7ffe221"
      "3afd1db6171d726ffc0460db6aec1646d6921d5269bafe2f1f71976e17ad6300"
      "c8b81f82dad10855c4e9de451f8eab08b21b61688be76b19e8842597d3a622b4"
      "2a5ca82cd8adca0e4089b6a6c997fa3ee65dbd4ccc61daa42e5796c6e349e839"
      "13b4adfb585254c5386d2090a61a693acba9896de4180b309f2233a0f51d1fe8";
  static const char* mhash_hex =
      "5aa1cf3b0a7f11d6b5c7307232f4d25a7fc67ac27cfb5946f48c89283031d3d1";

  uint8_t n[256], sig[256], mhash[32];
  size_t nlen = nc_test_from_hex(n_hex, n);
  size_t siglen = nc_test_from_hex(sig_hex, sig);
  size_t mhlen = nc_test_from_hex(mhash_hex, mhash);
  NC_CHECK(nlen == 256);
  NC_CHECK(siglen == 256);
  NC_CHECK(mhlen == 32);

  nc_rsa_pubkey pub;
  memset(&pub, 0, sizeof(pub));
  nc_bi_from_bytes(&pub.n, n, nlen);
  nc_bi_set_u32(&pub.e, 0x10001);
  pub.modulus_bytes = 256;
  pub.valid = 1;

  NC_CHECK(nc_rsa_verify_pss(&pub, sig, siglen, mhash, 32) == 1);

  /* Flip the message hash -> fail. */
  mhash[0] ^= 0x01;
  NC_CHECK(nc_rsa_verify_pss(&pub, sig, siglen, mhash, 32) == 0);
  mhash[0] ^= 0x01;

  /* Flip a signature byte -> fail. */
  sig[10] ^= 0x01;
  NC_CHECK(nc_rsa_verify_pss(&pub, sig, siglen, mhash, 32) == 0);
}

static void test_pbes2(void) {
  nc_rsa_privkey key;
  NC_CHECK(nc_rsa_parse_privkey_pem(&key, kEncPem, "testpass") == 0);
  NC_CHECK(key.valid == 1);
  NC_CHECK(key.modulus_bytes == 256); /* 2048-bit key */

  nc_rsa_privkey bad;
  NC_CHECK(nc_rsa_parse_privkey_pem(&bad, kEncPem, "nope") == -1);
}

int main(void) {
  test_sign_verify();
  test_pss_kat();
  test_pbes2();
  return nc_test_report();
}
