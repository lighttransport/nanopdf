/* SPDX-License-Identifier: Apache-2.0
 * X25519 (RFC 7748) scalar multiplication.
 * Ported from the public-domain TweetNaCl crypto_scalarmult. */

#include "ncrypto/nc_x25519.h"

typedef int64_t gf[16];

static const gf k121665 = {0xDB41, 1};

static void car25519(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (1LL << 16);
    int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1 + 37 * (c - 1) * (i == 15 ? 1 : 0);
    o[i] -= c << 16;
  }
}

static void sel25519(gf p, gf q, int b) {
  int64_t t, c = ~(b - 1);
  for (int i = 0; i < 16; ++i) {
    t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

static void pack25519(uint8_t *o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = (uint8_t)(t[i] & 0xff);
    o[2 * i + 1] = (uint8_t)(t[i] >> 8);
  }
}

static void unpack25519(gf o, const uint8_t *n) {
  for (int i = 0; i < 16; ++i)
    o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

static void fe_add(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

static void fe_sub(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

static void fe_mul(gf o, const gf a, const gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; ++i) t[i] = 0;
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car25519(o);
  car25519(o);
}

/* o = i^(p-2) mod p, i.e. the multiplicative inverse. */
static void inv25519(gf o, const gf i) {
  gf c;
  for (int a = 0; a < 16; ++a) c[a] = i[a];
  for (int a = 253; a >= 0; --a) {
    fe_mul(c, c, c);
    if (a != 2 && a != 4) fe_mul(c, c, i);
  }
  for (int a = 0; a < 16; ++a) o[a] = c[a];
}

static void scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p) {
  uint8_t z[32];
  gf x, a, b, c, d, e, f;
  for (int i = 0; i < 31; ++i) z[i] = n[i];
  z[31] = (uint8_t)((n[31] & 127) | 64);
  z[0] &= 248;
  unpack25519(x, p);
  for (int i = 0; i < 16; ++i) {
    b[i] = x[i];
    d[i] = a[i] = c[i] = 0;
  }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    int r = (z[i >> 3] >> (i & 7)) & 1;
    sel25519(a, b, r);
    sel25519(c, d, r);
    fe_add(e, a, c);
    fe_sub(a, a, c);
    fe_add(c, b, d);
    fe_sub(b, b, d);
    fe_mul(d, e, e);
    fe_mul(f, a, a);
    fe_mul(a, c, a);
    fe_mul(c, b, e);
    fe_add(e, a, c);
    fe_sub(a, a, c);
    fe_mul(b, a, a);
    fe_sub(c, d, f);
    fe_mul(a, c, k121665);
    fe_add(a, a, d);
    fe_mul(c, c, a);
    fe_mul(a, d, f);
    fe_mul(d, b, x);
    fe_mul(b, e, e);
    sel25519(a, b, r);
    sel25519(c, d, r);
  }
  inv25519(c, c); /* result = a / c */
  fe_mul(a, a, c);
  pack25519(q, a);
}

void nc_x25519(uint8_t out[32], const uint8_t scalar[32],
               const uint8_t point[32]) {
  scalarmult(out, scalar, point);
}

void nc_x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
  uint8_t base[32] = {9};
  scalarmult(out, scalar, base);
}
