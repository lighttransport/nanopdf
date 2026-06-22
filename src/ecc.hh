// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// ecc — minimal ECDSA signature verification over the NIST prime curves
// P-256 (secp256r1) and P-384 (secp384r1), built on the crypto-pk BigInt.
// Verification only (no key generation / signing); used for X.509 certificate
// chains and TLS 1.3 CertificateVerify with ECDSA server certificates.
//
// Not constant-time. Inversions use Fermat's little theorem (the field/order
// moduli are prime), point arithmetic uses Jacobian projective coordinates so a
// scalar multiplication needs only one final modular inversion.

#ifndef NANOPDF_ECC_HH_
#define NANOPDF_ECC_HH_

#include <cstdint>
#include <cstddef>

#include "crypto-pk.hh"

namespace nanopdf {
namespace crypto {

// A short-Weierstrass curve y^2 = x^3 + a*x + b over GF(p), with base point G
// of prime order n.
struct EcCurve {
  BigInt p, a, b, gx, gy, n;
  size_t field_bytes = 0;  // ceil(bit_length(p)/8)
  bool valid = false;
};

const EcCurve& curve_p256();
const EcCurve& curve_p384();

// Verify an ECDSA signature. @pub is the public point as an uncompressed
// SEC1 octet string (0x04 || X || Y), or the bare X||Y (2*field_bytes). @hash is
// the message digest. @r and @s are the signature scalars as big-endian
// magnitudes. Returns true iff the signature is valid.
bool ecdsa_verify(const EcCurve& curve, const uint8_t* pub, size_t pub_len,
                  const uint8_t* hash, size_t hash_len, const uint8_t* r,
                  size_t r_len, const uint8_t* s, size_t s_len);

}  // namespace crypto
}  // namespace nanopdf

#endif  // NANOPDF_ECC_HH_
