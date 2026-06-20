/* SPDX-License-Identifier: Apache-2.0
 * Known-answer tests for the ncrypto SHA family. */
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_test.h"

int main(void) {
  uint8_t abc[8];

  /* Decode "abc" via the shared hex helper (also exercises nc_test_from_hex). */
  NC_CHECK(nc_test_from_hex("616263", abc) == 3);

  /* SHA-256 */
  {
    uint8_t d[NC_SHA256_LEN];
    nc_sha256(abc, 3, d);
    NC_CHECK_EQ_HEX(
        d, NC_SHA256_LEN,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    nc_sha256((const uint8_t*)"", 0, d);
    NC_CHECK_EQ_HEX(
        d, NC_SHA256_LEN,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  }

  /* SHA-1 */
  {
    uint8_t d[NC_SHA1_LEN];
    nc_sha1(abc, 3, d);
    NC_CHECK_EQ_HEX(d, NC_SHA1_LEN,
                    "a9993e364706816aba3e25717850c26c9cd0d89d");
  }

  /* SHA-384 */
  {
    uint8_t d[NC_SHA384_LEN];
    nc_sha384(abc, 3, d);
    NC_CHECK_EQ_HEX(d, NC_SHA384_LEN,
                    "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a4"
                    "3ff5bed8086072ba1e7cc2358baeca134c825a7");
  }

  /* SHA-512 */
  {
    uint8_t d[NC_SHA512_LEN];
    nc_sha512(abc, 3, d);
    NC_CHECK_EQ_HEX(d, NC_SHA512_LEN,
                    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64"
                    "b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a"
                    "9ac94fa54ca49f");
  }

  /* Streaming vs one-shot over a longer input (spanning block boundaries). */
  {
    uint8_t msg[1000];
    uint8_t one[NC_SHA256_LEN];
    uint8_t two[NC_SHA256_LEN];
    nc_sha256_ctx c;
    size_t i;
    for (i = 0; i < sizeof(msg); i++) msg[i] = (uint8_t)(i * 7 + 3);

    nc_sha256(msg, sizeof(msg), one);

    nc_sha256_init(&c);
    nc_sha256_update(&c, msg, 137);
    nc_sha256_update(&c, msg + 137, 200);
    nc_sha256_update(&c, msg + 337, sizeof(msg) - 337);
    nc_sha256_final(&c, two);
    NC_CHECK(memcmp(one, two, NC_SHA256_LEN) == 0);
  }

  /* Streaming SHA-512 over a longer input too. */
  {
    uint8_t msg[500];
    uint8_t one[NC_SHA512_LEN];
    uint8_t two[NC_SHA512_LEN];
    nc_sha512_ctx c;
    size_t i;
    for (i = 0; i < sizeof(msg); i++) msg[i] = (uint8_t)(i ^ 0x5a);

    nc_sha512(msg, sizeof(msg), one);

    nc_sha512_init(&c);
    nc_sha512_update(&c, msg, 1);
    nc_sha512_update(&c, msg + 1, 199);
    nc_sha512_update(&c, msg + 200, sizeof(msg) - 200);
    nc_sha512_final(&c, two);
    NC_CHECK(memcmp(one, two, NC_SHA512_LEN) == 0);
  }

  return nc_test_report();
}
