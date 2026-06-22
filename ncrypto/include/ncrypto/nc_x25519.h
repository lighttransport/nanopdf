/* SPDX-License-Identifier: Apache-2.0
 * X25519 (RFC 7748) scalar multiplication. */
#ifndef NCRYPTO_NC_X25519_H_
#define NCRYPTO_NC_X25519_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* out = scalar * point (Curve25519). */
void nc_x25519(uint8_t out[32], const uint8_t scalar[32],
               const uint8_t point[32]);

/* out = scalar * basepoint (public key from a private scalar). */
void nc_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_X25519_H_ */
