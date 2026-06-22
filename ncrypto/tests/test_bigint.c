/* SPDX-License-Identifier: Apache-2.0 */
#include "ncrypto/nc_bigint.h"
#include "ncrypto/nc_test.h"

/* Build an nc_bigint from a hex string. */
static void bi_from_hex(nc_bigint* a, const char* hex) {
  uint8_t buf[1024];
  size_t n = nc_test_from_hex(hex, buf);
  nc_bi_from_bytes(a, buf, n);
}

/* Compare an nc_bigint against an expected minimal-encoding hex string. */
static int bi_eq_hex(const nc_bigint* a, const char* expect_hex) {
  uint8_t buf[1024];
  char got[2048];
  int n = nc_bi_to_bytes(a, buf, 0);
  if (n < 0) return 0;
  nc_test_to_hex(buf, (size_t)n, got);
  return strcmp(got, expect_hex) == 0;
}

int main(void) {
  /* round-trip for a 256-bit value */
  {
    const char* h =
        "deadbeefcafebabe0011223344556677"
        "8899aabbccddeeff0123456789abcdef";
    nc_bigint a;
    bi_from_hex(&a, h);
    NC_CHECK(bi_eq_hex(&a, h));
  }

  /* cmp: basic ordering */
  {
    nc_bigint a, b;
    nc_bi_set_u32(&a, 5);
    nc_bi_set_u32(&b, 9);
    NC_CHECK(nc_bi_cmp(&a, &b) < 0);
    NC_CHECK(nc_bi_cmp(&a, &a) == 0);
    NC_CHECK(nc_bi_cmp(&b, &a) > 0);
    /* multi-limb ordering */
    nc_bigint c, d;
    bi_from_hex(&c, "0100000000");      /* 2^32 */
    bi_from_hex(&d, "ffffffff");        /* 2^32 - 1 */
    NC_CHECK(nc_bi_cmp(&c, &d) > 0);
    NC_CHECK(nc_bi_cmp(&d, &c) < 0);
  }

  /* mul: 0xffffffff * 0xffffffff == 0xfffffffe00000001 */
  {
    nc_bigint a, b, r;
    bi_from_hex(&a, "ffffffff");
    bi_from_hex(&b, "ffffffff");
    nc_bi_mul(&r, &a, &b);
    NC_CHECK(bi_eq_hex(&r, "fffffffe00000001"));
  }

  /* add/sub inverse: (a+b)-b == a */
  {
    nc_bigint a, b, s, r;
    bi_from_hex(&a, "123456789abcdef0fedcba9876543210");
    bi_from_hex(&b, "0fedcba987654321123456789abcdef0");
    nc_bi_add(&s, &a, &b);
    nc_bi_sub(&r, &s, &b);
    NC_CHECK(nc_bi_cmp(&r, &a) == 0);
  }

  /* modexp(4, 13, 497) == 445 (0x1bd) */
  {
    nc_bigint base, exp, mod, r;
    nc_bi_set_u32(&base, 4);
    nc_bi_set_u32(&exp, 13);
    nc_bi_set_u32(&mod, 497);
    nc_bi_modexp(&r, &base, &exp, &mod);
    NC_CHECK(bi_eq_hex(&r, "01bd"));
  }

  /* Fermat: modexp(3, 256, 257) == 1 */
  {
    nc_bigint base, exp, mod, r;
    nc_bi_set_u32(&base, 3);
    nc_bi_set_u32(&exp, 256);
    nc_bi_set_u32(&mod, 257);
    nc_bi_modexp(&r, &base, &exp, &mod);
    NC_CHECK(bi_eq_hex(&r, "01"));
  }

  /* modexp big #1 */
  {
    nc_bigint base, exp, mod, r;
    bi_from_hex(&base, "deadbeefcafebabe1234567890abcdef");
    bi_from_hex(&exp, "010001");
    bi_from_hex(&mod, "fffffffffffffffffffffffffffffffeffffffffffffffff");
    nc_bi_modexp(&r, &base, &exp, &mod);
    NC_CHECK(bi_eq_hex(
        &r, "37058e009e637a12ec4fe26bd9f9ae4d5ea412a957495092"));
  }

  /* modexp big #2 */
  {
    nc_bigint base, exp, mod, r;
    bi_from_hex(&base,
                "b10a8db16ba3e3be1c1b2e1b0a0f0e0d0c0b0a09080706050"
                "403020100ffeeddccbbaa99887766554433221100");
    bi_from_hex(&exp, "01234567");
    bi_from_hex(&mod,
                "e3c699f557c5acc4d3b24d5e053c3e6916e9a83ea0acb6679"
                "55583f85c92b12c");
    nc_bi_modexp(&r, &base, &exp, &mod);
    NC_CHECK(bi_eq_hex(
        &r, "a38c0f6376342f9c4a56c0096a50bae7275cdf4d7e373b0e01d0ae656369d67c"));
  }

  /* alias-safety: t = a; mul(&t,&t,&t) == a*a */
  {
    nc_bigint a, t, expect;
    bi_from_hex(&a, "123456789abcdef0fedcba9876543210");
    nc_bi_mul(&expect, &a, &a);
    nc_bi_copy(&t, &a);
    nc_bi_mul(&t, &t, &t);
    NC_CHECK(nc_bi_cmp(&t, &expect) == 0);

    /* alias-safety for add/sub/mod/modexp too */
    nc_bigint b, x, y;
    bi_from_hex(&b, "0fedcba987654321");
    nc_bi_copy(&x, &a);
    nc_bi_add(&x, &x, &b);   /* r == a */
    nc_bi_add(&y, &a, &b);
    NC_CHECK(nc_bi_cmp(&x, &y) == 0);

    nc_bi_copy(&x, &a);
    nc_bi_mod(&x, &x, &b);   /* r == a */
    nc_bi_mod(&y, &a, &b);
    NC_CHECK(nc_bi_cmp(&x, &y) == 0);
  }

  /* to_bytes: zero -> length 0; fixed_len padding and overflow */
  {
    nc_bigint z;
    uint8_t buf[64];
    nc_bi_zero(&z);
    NC_CHECK(nc_bi_to_bytes(&z, buf, 0) == 0);

    nc_bigint a;
    nc_bi_set_u32(&a, 0xabcd);
    int n = nc_bi_to_bytes(&a, buf, 4);
    NC_CHECK(n == 4);
    NC_CHECK_EQ_HEX(buf, 4, "0000abcd");
    /* overflow: 0xabcd needs 2 bytes, fixed_len 1 -> -1 */
    NC_CHECK(nc_bi_to_bytes(&a, buf, 1) == -1);
  }

  return nc_test_report();
}
