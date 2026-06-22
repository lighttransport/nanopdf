/* SPDX-License-Identifier: Apache-2.0
 * Fixed-capacity arbitrary-precision non-negative integers (base 2^32 limbs,
 * little-endian) for RSA / ECDSA. Capacity supports up to ~4096-bit moduli with
 * room for multiplication intermediates. */
#ifndef NCRYPTO_NC_BIGINT_H_
#define NCRYPTO_NC_BIGINT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 264 limbs = 8448 bits: a 4096-bit (128-limb) multiply -> 256 limbs fits. */
#define NC_BIGINT_MAX_LIMBS 264

typedef struct {
  uint32_t limb[NC_BIGINT_MAX_LIMBS];  /* little-endian */
  int len;                             /* number of significant limbs (no
                                          trailing-zero limbs); 0 means zero */
} nc_bigint;

void nc_bi_zero(nc_bigint* a);
void nc_bi_set_u32(nc_bigint* a, uint32_t v);
void nc_bi_copy(nc_bigint* dst, const nc_bigint* src);

/* Big-endian byte parsing/serialization. */
void nc_bi_from_bytes(nc_bigint* a, const uint8_t* data, size_t len);
/* Writes big-endian bytes. If fixed_len > 0, output is left-padded/truncated to
 * fixed_len; returns fixed_len. If fixed_len == 0, writes the minimal encoding
 * and returns its length. Returns -1 if the value does not fit in fixed_len. */
int nc_bi_to_bytes(const nc_bigint* a, uint8_t* out, size_t fixed_len);

int nc_bi_is_zero(const nc_bigint* a);
size_t nc_bi_bitlen(const nc_bigint* a);
int nc_bi_cmp(const nc_bigint* a, const nc_bigint* b);  /* -1, 0, 1 */

/* All of the following are alias-safe: the result pointer may equal any input
 * pointer. */
void nc_bi_add(nc_bigint* r, const nc_bigint* a, const nc_bigint* b);
void nc_bi_sub(nc_bigint* r, const nc_bigint* a, const nc_bigint* b); /* a>=b */
void nc_bi_mul(nc_bigint* r, const nc_bigint* a, const nc_bigint* b);
void nc_bi_divmod(const nc_bigint* a, const nc_bigint* m, nc_bigint* q,
                  nc_bigint* rem);                       /* q and/or rem may be NULL */
void nc_bi_mod(nc_bigint* r, const nc_bigint* a, const nc_bigint* m);
void nc_bi_modexp(nc_bigint* r, const nc_bigint* base, const nc_bigint* exp,
                  const nc_bigint* m);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_BIGINT_H_ */
