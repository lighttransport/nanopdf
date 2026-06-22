/* SPDX-License-Identifier: Apache-2.0
 * Fixed-capacity arbitrary-precision non-negative integers (base 2^32 limbs,
 * little-endian). Ported faithfully from the C++ BigInt class in
 * src/crypto-pk.cc / crypto-pk.hh. */
#include "ncrypto/nc_bigint.h"

#include <string.h>

/* ---- basics --------------------------------------------------------------- */

void nc_bi_zero(nc_bigint* a) {
  a->len = 0;
}

void nc_bi_set_u32(nc_bigint* a, uint32_t v) {
  if (v) {
    a->limb[0] = v;
    a->len = 1;
  } else {
    a->len = 0;
  }
}

void nc_bi_copy(nc_bigint* dst, const nc_bigint* src) {
  if (dst == src) return;
  dst->len = src->len;
  if (src->len > 0) {
    memcpy(dst->limb, src->limb, (size_t)src->len * sizeof(uint32_t));
  }
}

/* "trim" = recompute len by dropping high zero limbs. */
static void nc_bi_trim(nc_bigint* a) {
  while (a->len > 0 && a->limb[a->len - 1] == 0) a->len--;
}

int nc_bi_is_zero(const nc_bigint* a) {
  return a->len == 0;
}

size_t nc_bi_bitlen(const nc_bigint* a) {
  if (a->len == 0) return 0;
  uint32_t top = a->limb[a->len - 1];
  size_t bits = (size_t)(a->len - 1) * 32;
  while (top) {
    bits++;
    top >>= 1;
  }
  return bits;
}

int nc_bi_cmp(const nc_bigint* a, const nc_bigint* b) {
  if (a->len != b->len) return a->len < b->len ? -1 : 1;
  for (int i = a->len; i-- > 0;) {
    if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
  }
  return 0;
}

/* ---- byte conversion ------------------------------------------------------ */

void nc_bi_from_bytes(nc_bigint* a, const uint8_t* data, size_t len) {
  nc_bi_zero(a);
  /* Strip leading zero bytes, then pack big-endian bytes into 32-bit limbs. */
  size_t start = 0;
  while (start < len && data[start] == 0) ++start;
  size_t n = len - start;
  if (n == 0) return;
  int nlimbs = (int)((n + 3) / 4);
  if (nlimbs > NC_BIGINT_MAX_LIMBS) nlimbs = NC_BIGINT_MAX_LIMBS;
  for (int i = 0; i < nlimbs; ++i) a->limb[i] = 0;
  for (size_t i = 0; i < n; ++i) {
    size_t limb_index = i / 4;
    if ((int)limb_index >= nlimbs) break;
    uint8_t b = data[len - 1 - i]; /* least-significant byte first */
    a->limb[limb_index] |= (uint32_t)b << (8 * (i % 4));
  }
  a->len = nlimbs;
  nc_bi_trim(a);
}

int nc_bi_to_bytes(const nc_bigint* a, uint8_t* out, size_t fixed_len) {
  /* Build big-endian, minimal representation first. */
  uint8_t be[NC_BIGINT_MAX_LIMBS * 4];
  size_t be_len = 0; /* number of significant big-endian bytes */

  if (a->len > 0) {
    /* little-endian byte stream from limbs */
    uint8_t le[NC_BIGINT_MAX_LIMBS * 4];
    size_t total = (size_t)a->len * 4;
    for (int i = 0; i < a->len; ++i) {
      uint32_t l = a->limb[i];
      le[i * 4 + 0] = (uint8_t)(l & 0xFF);
      le[i * 4 + 1] = (uint8_t)((l >> 8) & 0xFF);
      le[i * 4 + 2] = (uint8_t)((l >> 16) & 0xFF);
      le[i * 4 + 3] = (uint8_t)((l >> 24) & 0xFF);
    }
    /* drop high (most-significant) zero bytes */
    while (total > 1 && le[total - 1] == 0) total--;
    /* reverse into big-endian */
    for (size_t i = 0; i < total; ++i) be[i] = le[total - 1 - i];
    be_len = total;
  }
  /* For zero, be_len == 0 (writes nothing in minimal mode). */

  if (fixed_len == 0) {
    if (be_len > 0) memcpy(out, be, be_len);
    return (int)be_len;
  }

  if (be_len > fixed_len) return -1;
  size_t pad = fixed_len - be_len;
  memset(out, 0, pad);
  if (be_len > 0) memcpy(out + pad, be, be_len);
  return (int)fixed_len;
}

/* ---- arithmetic (all alias-safe via local temporary) ---------------------- */

void nc_bi_add(nc_bigint* r, const nc_bigint* a, const nc_bigint* b) {
  nc_bigint t;
  int n = a->len > b->len ? a->len : b->len;
  uint64_t carry = 0;
  int i;
  for (i = 0; i < n; ++i) {
    uint64_t s = carry;
    if (i < a->len) s += a->limb[i];
    if (i < b->len) s += b->limb[i];
    t.limb[i] = (uint32_t)s;
    carry = s >> 32;
  }
  if (carry && i < NC_BIGINT_MAX_LIMBS) {
    t.limb[i++] = (uint32_t)carry;
  }
  t.len = i;
  nc_bi_trim(&t);
  nc_bi_copy(r, &t);
}

void nc_bi_sub(nc_bigint* r, const nc_bigint* a, const nc_bigint* b) {
  nc_bigint t; /* assumes a >= b */
  int64_t borrow = 0;
  for (int i = 0; i < a->len; ++i) {
    int64_t s = (int64_t)a->limb[i] - borrow -
                (i < b->len ? (int64_t)b->limb[i] : 0);
    if (s < 0) {
      s += ((int64_t)1 << 32);
      borrow = 1;
    } else {
      borrow = 0;
    }
    t.limb[i] = (uint32_t)s;
  }
  t.len = a->len;
  nc_bi_trim(&t);
  nc_bi_copy(r, &t);
}

void nc_bi_mul(nc_bigint* r, const nc_bigint* a, const nc_bigint* b) {
  nc_bigint t;
  if (a->len == 0 || b->len == 0) {
    nc_bi_zero(r);
    return;
  }
  int n = a->len + b->len;
  /* Defensive bound: the fixed-capacity struct cannot hold a larger product.
     Callers reject oversized RSA moduli at parse time; this prevents an OOB
     write should one slip through. */
  if (n > NC_BIGINT_MAX_LIMBS) { nc_bi_zero(r); return; }
  for (int i = 0; i < n; ++i) t.limb[i] = 0;
  for (int i = 0; i < a->len; ++i) {
    uint64_t carry = 0;
    uint64_t ai = a->limb[i];
    for (int j = 0; j < b->len; ++j) {
      uint64_t cur = (uint64_t)t.limb[i + j] + ai * b->limb[j] + carry;
      t.limb[i + j] = (uint32_t)cur;
      carry = cur >> 32;
    }
    t.limb[i + b->len] += (uint32_t)carry;
  }
  t.len = n;
  nc_bi_trim(&t);
  nc_bi_copy(r, &t);
}

/* Shift left by one bit (in place). */
static void nc_bi_shl1(nc_bigint* v) {
  uint32_t carry = 0;
  for (int i = 0; i < v->len; ++i) {
    uint32_t nc = v->limb[i] >> 31;
    v->limb[i] = (v->limb[i] << 1) | carry;
    carry = nc;
  }
  if (carry && v->len < NC_BIGINT_MAX_LIMBS) {
    v->limb[v->len] = carry;
    v->len++;
  }
}

void nc_bi_divmod(const nc_bigint* a, const nc_bigint* m, nc_bigint* q,
                  nc_bigint* rem) {
  /* Bit-at-a-time long division. */
  nc_bigint quotient, remainder;
  nc_bi_zero(&quotient);
  nc_bi_zero(&remainder);

  if (nc_bi_cmp(a, m) < 0) {
    if (q) nc_bi_zero(q);
    if (rem) nc_bi_copy(rem, a);
    return;
  }

  size_t bits = nc_bi_bitlen(a);
  int qlimbs = (int)((bits + 31) / 32);
  for (int i = 0; i < qlimbs; ++i) quotient.limb[i] = 0;
  quotient.len = qlimbs;

  for (size_t i = bits; i-- > 0;) {
    /* remainder = (remainder << 1) | bit i of a */
    nc_bi_shl1(&remainder);
    uint32_t bit = (a->limb[i / 32] >> (i % 32)) & 1u;
    if (bit) {
      if (remainder.len == 0) {
        remainder.limb[0] = 1;
        remainder.len = 1;
      } else {
        remainder.limb[0] |= 1u;
      }
    }
    nc_bi_trim(&remainder);
    if (nc_bi_cmp(&remainder, m) >= 0) {
      nc_bi_sub(&remainder, &remainder, m);
      quotient.limb[i / 32] |= (1u << (i % 32));
    }
  }
  nc_bi_trim(&quotient);
  if (q) nc_bi_copy(q, &quotient);
  if (rem) nc_bi_copy(rem, &remainder);
}

void nc_bi_mod(nc_bigint* r, const nc_bigint* a, const nc_bigint* m) {
  nc_bigint t;
  nc_bi_divmod(a, m, NULL, &t);
  nc_bi_copy(r, &t);
}

void nc_bi_modexp(nc_bigint* r, const nc_bigint* base, const nc_bigint* exp,
                  const nc_bigint* m) {
  nc_bigint result, b, tmp;
  nc_bi_set_u32(&result, 1);
  nc_bi_mod(&b, base, m);
  size_t bits = nc_bi_bitlen(exp);
  for (size_t i = 0; i < bits; ++i) {
    if ((exp->limb[i / 32] >> (i % 32)) & 1u) {
      nc_bi_mul(&tmp, &result, &b);
      nc_bi_mod(&result, &tmp, m);
    }
    nc_bi_mul(&tmp, &b, &b);
    nc_bi_mod(&b, &tmp, m);
  }
  nc_bi_copy(r, &result);
}
