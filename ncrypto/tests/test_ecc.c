/* SPDX-License-Identifier: Apache-2.0
 * ECDSA verification known-answer tests (OpenSSL-generated KATs). */

#include <stdint.h>

#include "ncrypto/nc_ecc.h"
#include "ncrypto/nc_test.h"

int main(void) {
  /* ---- P-256 / SHA-256 ---- */
  {
    uint8_t pub[128], hash[64], r[64], s[64];
    size_t publen, hashlen, rlen, slen;
    publen = nc_test_from_hex(
        "04fbf4ebfd3e1b9e4003751414eeb081550c9a420676bc64f5ee4a3f02611f0197"
        "63d5233410d99afed1a30b4689231857b861e38e1466c64e6cb7853b061ed194",
        pub);
    hashlen = nc_test_from_hex(
        "8ab884083c0fa6cdd345fca442b693fe6a76749b2003da8d9b955d47ca856189",
        hash);
    rlen = nc_test_from_hex(
        "e7c360ad8cf41293e727a18658a6faa615b95efb3de3cba6b518349e3e51d0cc", r);
    slen = nc_test_from_hex(
        "9ea213d898c4d01e271770ed48477a78fad2320dff65cd389b78f41d9bd6dba3", s);

    NC_CHECK(publen == 65 && hashlen == 32 && rlen == 32 && slen == 32);
    NC_CHECK_EQ_HEX(
        r, rlen,
        "e7c360ad8cf41293e727a18658a6faa615b95efb3de3cba6b518349e3e51d0cc");
    NC_CHECK(nc_ecdsa_verify(nc_curve_p256(), pub, publen, hash, hashlen, r,
                             rlen, s, slen) == 1);

    /* Flip hash[0] -> invalid. */
    hash[0] ^= 0xFF;
    NC_CHECK(nc_ecdsa_verify(nc_curve_p256(), pub, publen, hash, hashlen, r,
                             rlen, s, slen) == 0);
    hash[0] ^= 0xFF;  /* restore */

    /* Flip an r byte -> invalid. */
    r[0] ^= 0xFF;
    NC_CHECK(nc_ecdsa_verify(nc_curve_p256(), pub, publen, hash, hashlen, r,
                             rlen, s, slen) == 0);
    r[0] ^= 0xFF;  /* restore */
  }

  /* ---- P-384 / SHA-384 ---- */
  {
    uint8_t pub[128], hash[64], r[64], s[64];
    size_t publen, hashlen, rlen, slen;
    publen = nc_test_from_hex(
        "04ef2c299ddb626105c55f97690dc1c0d2d002dc430872cefed2cf16fc3b8b9950"
        "3a14d77fe60449c0d985e8f931295b32a57de9d5d854962221564141dfcaf47bf"
        "2aac7531cf40a0d92d8d31169cc7647bd2673829bc891bcdac9271484cf9906",
        pub);
    hashlen = nc_test_from_hex(
        "03261e554e45676cfcc1f3c10c0be61bef26b687ea8afda221736ef3966c5133a1"
        "78f1701a2923cd8cab92799158bffb",
        hash);
    rlen = nc_test_from_hex(
        "ec59feed9c6e0027a042d19c96e80e21e5404aebc2d777b233da23a6b0b75eecf3"
        "ca44213274667731ecf557f326cb08",
        r);
    slen = nc_test_from_hex(
        "acf51b385b03c0d4a5447b6212ebe62ab62afa795fd4a33489062cbfeed3c1fe01"
        "5e6bea50d111f6e218ec6bbddbceb3",
        s);

    NC_CHECK(publen == 97 && hashlen == 48 && rlen == 48 && slen == 48);
    NC_CHECK(nc_ecdsa_verify(nc_curve_p384(), pub, publen, hash, hashlen, r,
                             rlen, s, slen) == 1);

    /* Flip an s byte -> invalid. */
    s[0] ^= 0xFF;
    NC_CHECK(nc_ecdsa_verify(nc_curve_p384(), pub, publen, hash, hashlen, r,
                             rlen, s, slen) == 0);
    s[0] ^= 0xFF;  /* restore */
  }

  return nc_test_report();
}
