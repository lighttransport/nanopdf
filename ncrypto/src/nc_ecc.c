/* SPDX-License-Identifier: Apache-2.0
 * ECDSA signature verification over NIST P-256 / P-384, ported from the C++
 * implementation in src/ecc.cc. Prime-field short-Weierstrass curves with
 * Jacobian projective point arithmetic and Fermat-based modular inversion.
 * Verification only; not constant-time. */

#include "ncrypto/nc_ecc.h"

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_bigint.h"

/* ----------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

/* Parse a (space-free) hex string into a big-endian magnitude. */
static void big_hex(nc_bigint* out, const char* h) {
  uint8_t bytes[128];
  size_t nhex = 0, nbytes = 0, i;
  char buf[256];
  /* Strip spaces. */
  for (i = 0; h[i]; ++i) {
    if (h[i] != ' ') buf[nhex++] = h[i];
  }
  buf[nhex] = 0;
  /* Decode (assumes even length, which all curve constants have). */
  for (i = 0; i + 1 < nhex + 1 && buf[i] && buf[i + 1]; i += 2) {
    int hi = buf[i] <= '9' ? buf[i] - '0' : (buf[i] | 32) - 'a' + 10;
    int lo = buf[i + 1] <= '9' ? buf[i + 1] - '0' : (buf[i + 1] | 32) - 'a' + 10;
    bytes[nbytes++] = (uint8_t)(hi * 16 + lo);
  }
  nc_bi_from_bytes(out, bytes, nbytes);
}

/* a mod m */
static void mod_m(nc_bigint* r, const nc_bigint* a, const nc_bigint* m) {
  nc_bi_mod(r, a, m);
}

/* (a * b) mod m */
static void mmul(nc_bigint* r, const nc_bigint* a, const nc_bigint* b,
                 const nc_bigint* m) {
  nc_bigint t;
  nc_bi_mul(&t, a, b);
  nc_bi_mod(r, &t, m);
}

/* (a + b) mod m */
static void madd(nc_bigint* r, const nc_bigint* a, const nc_bigint* b,
                 const nc_bigint* m) {
  nc_bigint t;
  nc_bi_add(&t, a, b);
  nc_bi_mod(r, &t, m);
}

/* (a - b) mod m, for any a,b >= 0. */
static void msub(nc_bigint* r, const nc_bigint* a, const nc_bigint* b,
                 const nc_bigint* m) {
  nc_bigint am, bm, t;
  nc_bi_mod(&am, a, m);
  nc_bi_mod(&bm, b, m);
  if (nc_bi_cmp(&am, &bm) >= 0) {
    nc_bi_sub(r, &am, &bm);
  } else {
    nc_bi_add(&t, &am, m);
    nc_bi_sub(r, &t, &bm);
  }
}

/* Modular inverse via Fermat: a^(m-2) mod m (m prime). */
static void minv(nc_bigint* r, const nc_bigint* a, const nc_bigint* m) {
  nc_bigint two, e, am;
  nc_bi_set_u32(&two, 2);
  nc_bi_sub(&e, m, &two);
  nc_bi_mod(&am, a, m);
  nc_bi_modexp(r, &am, &e, m);
}

/* ----------------------------------------------------------------------------
 * Jacobian point arithmetic
 * ------------------------------------------------------------------------- */

typedef struct {
  nc_bigint X, Y, Z;
  int inf;
} Jac;

static void jac_set_inf(Jac* p) {
  nc_bi_zero(&p->X);
  nc_bi_zero(&p->Y);
  nc_bi_zero(&p->Z);
  p->inf = 1;
}

static Jac jac_double(const Jac* P, const nc_ec_curve* c) {
  Jac r;
  const nc_bigint* p = &c->p;
  nc_bigint k2, k3, k4, k8;
  nc_bigint YY, S, ZZ, Z4, M, X3, YYYY, Y3, Z3, t1, t2;
  if (P->inf || nc_bi_is_zero(&P->Y)) {
    jac_set_inf(&r);
    return r;
  }
  nc_bi_set_u32(&k2, 2);
  nc_bi_set_u32(&k3, 3);
  nc_bi_set_u32(&k4, 4);
  nc_bi_set_u32(&k8, 8);

  mmul(&YY, &P->Y, &P->Y, p);              /* YY = Y^2 */
  mmul(&t1, &P->X, &YY, p);                /* X*Y^2 */
  mmul(&S, &k4, &t1, p);                   /* S = 4*X*Y^2 */
  mmul(&ZZ, &P->Z, &P->Z, p);             /* Z^2 */
  mmul(&Z4, &ZZ, &ZZ, p);                  /* Z^4 */
  mmul(&t1, &P->X, &P->X, p);              /* X^2 */
  mmul(&t1, &k3, &t1, p);                  /* 3*X^2 */
  mmul(&t2, &c->a, &Z4, p);                /* a*Z^4 */
  madd(&M, &t1, &t2, p);                   /* M = 3X^2 + a*Z^4 */
  mmul(&t1, &M, &M, p);                    /* M^2 */
  mmul(&t2, &k2, &S, p);                   /* 2S */
  msub(&X3, &t1, &t2, p);                  /* X3 = M^2 - 2S */
  mmul(&YYYY, &YY, &YY, p);               /* Y^4 */
  msub(&t1, &S, &X3, p);                   /* S - X3 */
  mmul(&t1, &M, &t1, p);                   /* M*(S-X3) */
  mmul(&t2, &k8, &YYYY, p);                /* 8*Y^4 */
  msub(&Y3, &t1, &t2, p);                  /* Y3 = M(S-X3) - 8Y^4 */
  mmul(&t1, &P->Y, &P->Z, p);             /* Y*Z */
  mmul(&Z3, &k2, &t1, p);                  /* Z3 = 2YZ */

  nc_bi_copy(&r.X, &X3);
  nc_bi_copy(&r.Y, &Y3);
  nc_bi_copy(&r.Z, &Z3);
  r.inf = 0;
  return r;
}

static Jac jac_add(const Jac* P, const Jac* Q, const nc_ec_curve* c) {
  Jac r;
  const nc_bigint* p = &c->p;
  nc_bigint k2;
  nc_bigint Z1Z1, Z2Z2, U1, U2, S1, S2, H, R, HH, HHH, U1HH, X3, Y3, Z3, t1, t2;
  if (P->inf) {
    r = *Q;
    return r;
  }
  if (Q->inf) {
    r = *P;
    return r;
  }
  nc_bi_set_u32(&k2, 2);

  mmul(&Z1Z1, &P->Z, &P->Z, p);
  mmul(&Z2Z2, &Q->Z, &Q->Z, p);
  mmul(&U1, &P->X, &Z2Z2, p);              /* U1 = X1*Z2^2 */
  mmul(&U2, &Q->X, &Z1Z1, p);              /* U2 = X2*Z1^2 */
  mmul(&t1, &Z2Z2, &Q->Z, p);             /* Z2^3 */
  mmul(&S1, &P->Y, &t1, p);                /* S1 = Y1*Z2^3 */
  mmul(&t1, &Z1Z1, &P->Z, p);             /* Z1^3 */
  mmul(&S2, &Q->Y, &t1, p);                /* S2 = Y2*Z1^3 */

  if (nc_bi_cmp(&U1, &U2) == 0) {
    if (nc_bi_cmp(&S1, &S2) != 0) {
      jac_set_inf(&r);  /* P + (-P) = O */
      return r;
    }
    return jac_double(P, c);
  }

  msub(&H, &U2, &U1, p);
  msub(&R, &S2, &S1, p);
  mmul(&HH, &H, &H, p);
  mmul(&HHH, &HH, &H, p);
  mmul(&U1HH, &U1, &HH, p);
  mmul(&t1, &R, &R, p);                    /* R^2 */
  msub(&t1, &t1, &HHH, p);                 /* R^2 - H^3 */
  mmul(&t2, &k2, &U1HH, p);                /* 2*U1*H^2 */
  msub(&X3, &t1, &t2, p);                  /* X3 = R^2 - H^3 - 2*U1*H^2 */
  msub(&t1, &U1HH, &X3, p);                /* U1*H^2 - X3 */
  mmul(&t1, &R, &t1, p);                   /* R*(U1*H^2 - X3) */
  mmul(&t2, &S1, &HHH, p);                 /* S1*H^3 */
  msub(&Y3, &t1, &t2, p);                  /* Y3 = R*(U1*H^2-X3) - S1*H^3 */
  mmul(&t1, &P->Z, &Q->Z, p);             /* Z1*Z2 */
  mmul(&Z3, &H, &t1, p);                   /* Z3 = H*Z1*Z2 */

  nc_bi_copy(&r.X, &X3);
  nc_bi_copy(&r.Y, &Y3);
  nc_bi_copy(&r.Z, &Z3);
  r.inf = 0;
  return r;
}

/* k*P via left-to-right double-and-add over the big-endian bits of k. */
static Jac jac_mul(const nc_bigint* k, const Jac* P, const nc_ec_curve* c) {
  Jac R;
  uint8_t kb[256];
  int klen, b;
  size_t i;
  int started = 0;
  klen = nc_bi_to_bytes(k, kb, 0);
  jac_set_inf(&R);
  if (klen <= 0) return R;
  for (i = 0; i < (size_t)klen; ++i) {
    for (b = 7; b >= 0; --b) {
      if (started) R = jac_double(&R, c);
      if ((kb[i] >> b) & 1) {
        if (started) {
          R = jac_add(&R, P, c);
        } else {
          R = *P;
          started = 1;
        }
      }
    }
  }
  return R;
}

/* Affine x-coordinate of a Jacobian point; returns 0 if point at infinity. */
static int jac_affine_x(const Jac* P, const nc_ec_curve* c, nc_bigint* x) {
  nc_bigint zinv, zinv2;
  if (P->inf || nc_bi_is_zero(&P->Z)) return 0;
  minv(&zinv, &P->Z, &c->p);
  mmul(&zinv2, &zinv, &zinv, &c->p);
  mmul(x, &P->X, &zinv2, &c->p);
  return 1;
}

/* ----------------------------------------------------------------------------
 * Curve constants
 * ------------------------------------------------------------------------- */

static nc_ec_curve g_p256;
static nc_ec_curve g_p384;
static int g_p256_done = 0;
static int g_p384_done = 0;

const nc_ec_curve* nc_curve_p256(void) {
  if (!g_p256_done) {
    nc_ec_curve* c = &g_p256;
    big_hex(&c->p,
            "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF");
    big_hex(&c->a,
            "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC");
    big_hex(&c->b,
            "5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B");
    big_hex(&c->gx,
            "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
    big_hex(&c->gy,
            "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");
    big_hex(&c->n,
            "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");
    c->field_bytes = 32;
    c->valid = 1;
    g_p256_done = 1;
  }
  return &g_p256;
}

const nc_ec_curve* nc_curve_p384(void) {
  if (!g_p384_done) {
    nc_ec_curve* c = &g_p384;
    big_hex(&c->p,
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE"
            "FFFFFFFF0000000000000000FFFFFFFF");
    big_hex(&c->a,
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE"
            "FFFFFFFF0000000000000000FFFFFFFC");
    big_hex(&c->b,
            "B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875A"
            "C656398D8A2ED19D2A85C8EDD3EC2AEF");
    big_hex(&c->gx,
            "AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A38"
            "5502F25DBF55296C3A545E3872760AB7");
    big_hex(&c->gy,
            "3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C0"
            "0A60B1CE1D7E819D7A431D7C90EA0E5F");
    big_hex(&c->n,
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF"
            "581A0DB248B0A77AECEC196ACCC52973");
    c->field_bytes = 48;
    c->valid = 1;
    g_p384_done = 1;
  }
  return &g_p384;
}

/* ----------------------------------------------------------------------------
 * ECDSA verification
 * ------------------------------------------------------------------------- */

int nc_ecdsa_verify(const nc_ec_curve* curve, const uint8_t* pub,
                    size_t pub_len, const uint8_t* hash, size_t hash_len,
                    const uint8_t* r_be, size_t r_len, const uint8_t* s_be,
                    size_t s_len) {
  size_t fb;
  nc_bigint Qx, Qy, r, s, z, w, u1, u2, zn, rn, x, v, one;
  Jac G, Q, Ru1, Ru2, R;

  if (!curve || !curve->valid) return 0;
  fb = curve->field_bytes;

  /* Parse the public point (uncompressed SEC1, optional 0x04 prefix). */
  if (pub_len == 2 * fb + 1) {
    if (pub[0] != 0x04) return 0;
    ++pub;
    pub_len -= 1;
  }
  if (pub_len != 2 * fb) return 0;
  nc_bi_from_bytes(&Qx, pub, fb);
  nc_bi_from_bytes(&Qy, pub + fb, fb);

  /* Validate the public key: coordinates in [0,p), not the identity, and on the
     curve (y^2 == x^3 + a*x + b mod p). Rejects invalid-curve points. */
  if (nc_bi_cmp(&Qx, &curve->p) >= 0 || nc_bi_cmp(&Qy, &curve->p) >= 0) return 0;
  if (nc_bi_is_zero(&Qx) && nc_bi_is_zero(&Qy)) return 0;
  {
    nc_bigint lhs, rhs, t;
    mmul(&lhs, &Qy, &Qy, &curve->p);            /* y^2 */
    mmul(&t, &Qx, &Qx, &curve->p);
    mmul(&rhs, &t, &Qx, &curve->p);             /* x^3 */
    mmul(&t, &curve->a, &Qx, &curve->p);        /* a*x */
    madd(&rhs, &rhs, &t, &curve->p);
    madd(&rhs, &rhs, &curve->b, &curve->p);     /* + b */
    if (nc_bi_cmp(&lhs, &rhs) != 0) return 0;
  }

  nc_bi_from_bytes(&r, r_be, r_len);
  nc_bi_from_bytes(&s, s_be, s_len);

  /* 1 <= r,s < n */
  if (nc_bi_is_zero(&r) || nc_bi_is_zero(&s)) return 0;
  if (nc_bi_cmp(&r, &curve->n) >= 0 || nc_bi_cmp(&s, &curve->n) >= 0) return 0;

  /* z = the leftmost Ln bits of the hash (Ln = bit length of n). */
  nc_bi_from_bytes(&z, hash, hash_len);
  {
    size_t nbits = nc_bi_bitlen(&curve->n);
    if (hash_len * 8 > nbits) {
      size_t shift = hash_len * 8 - nbits;
      uint8_t div[64];
      size_t dlen = shift / 8 + 1, i;
      nc_bigint divisor, q;
      for (i = 0; i < dlen; ++i) div[i] = 0;
      /* 2^shift, big-endian: high byte is div[0]. */
      div[0] = (uint8_t)(1u << (shift % 8));
      nc_bi_from_bytes(&divisor, div, dlen);
      nc_bi_divmod(&z, &divisor, &q, NULL);
      nc_bi_copy(&z, &q);
    }
  }

  minv(&w, &s, &curve->n);
  mod_m(&zn, &z, &curve->n);
  mod_m(&rn, &r, &curve->n);
  mmul(&u1, &zn, &w, &curve->n);
  mmul(&u2, &rn, &w, &curve->n);

  nc_bi_set_u32(&one, 1);
  nc_bi_copy(&G.X, &curve->gx);
  nc_bi_copy(&G.Y, &curve->gy);
  nc_bi_copy(&G.Z, &one);
  G.inf = 0;
  nc_bi_copy(&Q.X, &Qx);
  nc_bi_copy(&Q.Y, &Qy);
  nc_bi_copy(&Q.Z, &one);
  Q.inf = 0;

  Ru1 = jac_mul(&u1, &G, curve);
  Ru2 = jac_mul(&u2, &Q, curve);
  R = jac_add(&Ru1, &Ru2, curve);

  if (!jac_affine_x(&R, curve, &x)) return 0;
  mod_m(&v, &x, &curve->n);
  return nc_bi_cmp(&v, &r) == 0 ? 1 : 0;
}
