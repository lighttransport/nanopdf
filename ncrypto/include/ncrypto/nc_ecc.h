/* SPDX-License-Identifier: Apache-2.0
 * ECDSA signature verification over NIST P-256 / P-384. Built on nc_bigint
 * (Jacobian point math, Fermat inversion). Verification only. */
#ifndef NCRYPTO_NC_ECC_H_
#define NCRYPTO_NC_ECC_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_bigint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Short-Weierstrass curve y^2 = x^3 + a*x + b over GF(p), base point (gx,gy)
 * of prime order n. */
typedef struct {
  nc_bigint p, a, b, gx, gy, n;
  size_t field_bytes;  /* ceil(bitlen(p)/8): 32 for P-256, 48 for P-384 */
  int valid;
} nc_ec_curve;

const nc_ec_curve* nc_curve_p256(void);
const nc_ec_curve* nc_curve_p384(void);

/* Verify an ECDSA signature. @pub is the uncompressed point (0x04||X||Y) or the
 * bare X||Y (2*field_bytes). @hash is the message digest. @r,@s are big-endian
 * magnitudes. Returns 1 if valid, 0 otherwise. */
int nc_ecdsa_verify(const nc_ec_curve* curve, const uint8_t* pub, size_t pub_len,
                    const uint8_t* hash, size_t hash_len, const uint8_t* r,
                    size_t r_len, const uint8_t* s, size_t s_len);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_ECC_H_ */
