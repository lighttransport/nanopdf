/* SPDX-License-Identifier: Apache-2.0
 * X25519 (RFC 7748) known-answer and Diffie-Hellman tests. */

#include "ncrypto/nc_test.h"
#include "ncrypto/nc_x25519.h"

int main(void) {
  uint8_t scalar[32], point[32], out[32];

  /* RFC 7748 section 5.2, first scalarmult KAT. */
  nc_test_from_hex(
      "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
      scalar);
  nc_test_from_hex(
      "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
      point);
  nc_x25519(out, scalar, point);
  NC_CHECK_EQ_HEX(
      out, 32,
      "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

  /* RFC 7748 section 6.1, Alice's public key from her private scalar. */
  {
    uint8_t pub[32];
    nc_test_from_hex(
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
        scalar);
    nc_x25519_base(pub, scalar);
    NC_CHECK_EQ_HEX(
        pub, 32,
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  }

  /* Diffie-Hellman agreement: base(a)*b == base(b)*a. */
  {
    uint8_t a[32], b[32], A[32], B[32], ssa[32], ssb[32];
    for (int i = 0; i < 32; ++i) {
      a[i] = (uint8_t)(i + 1);
      b[i] = (uint8_t)(0xff - i);
    }
    nc_x25519_base(A, a);
    nc_x25519_base(B, b);
    nc_x25519(ssa, b, A); /* B's view: a's public * b */
    nc_x25519(ssb, a, B); /* A's view: b's public * a */
    NC_CHECK(memcmp(ssa, ssb, 32) == 0);
  }

  return nc_test_report();
}
