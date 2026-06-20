// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// tls-crypto — primitives needed by the minimal TLS 1.3 client: X25519 key
// exchange (RFC 7748), AES-128-GCM AEAD (the mandatory TLS 1.3 cipher suite),
// and HKDF-SHA256 with the TLS 1.3 HkdfLabel (RFC 8446 / RFC 5869). Pure C++,
// built on the existing SHA-256 / AES-128 / HMAC.

#ifndef NANOPDF_TLS_CRYPTO_HH_
#define NANOPDF_TLS_CRYPTO_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace tlscrypto {

using Bytes = std::vector<uint8_t>;

// X25519 (RFC 7748). All buffers are 32 bytes. out = scalar * point.
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
// out = scalar * basepoint(9): the public key for a private @scalar.
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

// AES-128-GCM. @iv is 12 bytes. seal() appends the 16-byte tag to the output.
// open() returns false on auth failure.
Bytes aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                      const Bytes& aad, const Bytes& plaintext);
bool aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                     const Bytes& aad, const Bytes& ciphertext_and_tag,
                     Bytes* plaintext);

// HKDF-SHA256 (RFC 5869).
Bytes hkdf_extract(const Bytes& salt, const Bytes& ikm);
Bytes hkdf_expand(const Bytes& prk, const Bytes& info, size_t length);
// TLS 1.3 HKDF-Expand-Label (RFC 8446 7.1).
Bytes hkdf_expand_label(const Bytes& secret, const std::string& label,
                        const Bytes& context, size_t length);

}  // namespace tlscrypto
}  // namespace nanopdf

#endif  // NANOPDF_TLS_CRYPTO_HH_
