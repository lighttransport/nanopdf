/* SPDX-License-Identifier: Apache-2.0
 * Known-answer tests for the ncrypto AES module. */
#include "ncrypto/nc_aes.h"
#include "ncrypto/nc_test.h"

int main(void) {
  uint8_t key[32], iv[16], pt[256], ct[256], buf[256], aad[64], tag[16];
  size_t klen, ptlen, ctlen, aadlen;

  /* --- AES-128 block (FIPS-197 C.1) --- */
  {
    nc_aes_ctx c;
    klen = nc_test_from_hex("000102030405060708090a0b0c0d0e0f", key);
    ptlen = nc_test_from_hex("00112233445566778899aabbccddeeff", pt);
    NC_CHECK(nc_aes_init(&c, key, klen) == 0);
    nc_aes_encrypt_block(&c, pt, ct);
    NC_CHECK_EQ_HEX(ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
    nc_aes_decrypt_block(&c, ct, buf);
    NC_CHECK_EQ_HEX(buf, 16, "00112233445566778899aabbccddeeff");
    (void)ptlen;
  }

  /* --- AES-256 block (FIPS-197 C.3) --- */
  {
    nc_aes_ctx c;
    klen = nc_test_from_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        key);
    nc_test_from_hex("00112233445566778899aabbccddeeff", pt);
    NC_CHECK(nc_aes_init(&c, key, klen) == 0);
    nc_aes_encrypt_block(&c, pt, ct);
    NC_CHECK_EQ_HEX(ct, 16, "8ea2b7ca516745bfeafc49904b496089");
    nc_aes_decrypt_block(&c, ct, buf);
    NC_CHECK_EQ_HEX(buf, 16, "00112233445566778899aabbccddeeff");
  }

  /* --- AES-128-GCM (NIST test case 3) --- */
  {
    klen = nc_test_from_hex("feffe9928665731c6d6a8f9467308308", key);
    nc_test_from_hex("cafebabefacedbaddecaf888", iv);
    aadlen = 0;
    ptlen = nc_test_from_hex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c"
        "3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
        pt);
    nc_aes_gcm_seal(key, klen, iv, aad, aadlen, pt, ptlen, ct, tag);
    NC_CHECK_EQ_HEX(
        ct, ptlen,
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21"
        "d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985");
    NC_CHECK_EQ_HEX(tag, 16, "4d5c2af327cd64a62cf35abd2ba6fab4");

    /* open with correct tag recovers pt */
    int ok = nc_aes_gcm_open(key, klen, iv, aad, aadlen, ct, ptlen, tag, buf);
    NC_CHECK(ok == 1);
    NC_CHECK_EQ_HEX(
        buf, ptlen,
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c"
        "3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255");

    /* flip one ciphertext byte -> open fails */
    uint8_t bad[256];
    memcpy(bad, ct, ptlen);
    bad[0] ^= 0x01;
    int bad_ok =
        nc_aes_gcm_open(key, klen, iv, aad, aadlen, bad, ptlen, tag, buf);
    NC_CHECK(bad_ok == 0);
  }

  /* --- AES-128-CBC (SP800-38A F.2) --- */
  {
    klen = nc_test_from_hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    nc_test_from_hex("000102030405060708090a0b0c0d0e0f", iv);
    ctlen = nc_test_from_hex("6bc1bee22e409f96e93d7e117393172a", pt);
    NC_CHECK(nc_aes_cbc_encrypt(key, klen, iv, pt, ctlen, ct) == 0);
    NC_CHECK_EQ_HEX(ct, 16, "7649abac8119b246cee98e9b12e9197d");
    NC_CHECK(nc_aes_cbc_decrypt(key, klen, iv, ct, ctlen, buf) == 0);
    NC_CHECK_EQ_HEX(buf, 16, "6bc1bee22e409f96e93d7e117393172a");

    /* in-place (aliasing) decrypt round-trip */
    memcpy(buf, ct, 16);
    NC_CHECK(nc_aes_cbc_decrypt(key, klen, iv, buf, 16, buf) == 0);
    NC_CHECK_EQ_HEX(buf, 16, "6bc1bee22e409f96e93d7e117393172a");
  }

  return nc_test_report();
}
