// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// crypto-pk — minimal public-key primitives (big integers + RSA PKCS#1 v1.5)
// implemented in pure C++17, as the foundation for OpenSSL-free PDF signing.
//
// SCOPE / STATUS: this is the first building block toward removing the OpenSSL
// dependency. It currently provides big-integer arithmetic and RSA signature
// generation/verification (PKCS#1 v1.5, the scheme PDF adbe.pkcs7 signatures
// use). It is NOT constant-time and must not be used where private-key timing
// side-channels matter against an adversary who can measure signing time;
// for PDF document signing (a local, offline operation) that threat model does
// not apply. ASN.1/CMS/X.509/RFC-3161/TLS are intentionally out of scope here.

#ifndef NANOPDF_CRYPTO_PK_HH_
#define NANOPDF_CRYPTO_PK_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace crypto {

// Arbitrary-precision non-negative integer (base 2^32 limbs, little-endian).
class BigInt {
 public:
  BigInt() = default;
  explicit BigInt(uint32_t v) { if (v) limbs_.push_back(v); }

  // Big-endian byte parsing/serialization (as used by crypto formats).
  static BigInt from_bytes(const uint8_t* data, size_t len);
  std::vector<uint8_t> to_bytes(size_t fixed_len = 0) const;  // big-endian

  bool is_zero() const { return limbs_.empty(); }
  size_t bit_length() const;

  // Comparison: -1 / 0 / 1.
  static int cmp(const BigInt& a, const BigInt& b);

  static BigInt add(const BigInt& a, const BigInt& b);
  static BigInt sub(const BigInt& a, const BigInt& b);   // requires a >= b
  static BigInt mul(const BigInt& a, const BigInt& b);
  static void divmod(const BigInt& a, const BigInt& b, BigInt* q, BigInt* r);
  static BigInt mod(const BigInt& a, const BigInt& m);

  // Modular exponentiation: base^exp mod m.
  static BigInt modexp(const BigInt& base, const BigInt& exp, const BigInt& m);

 private:
  std::vector<uint32_t> limbs_;  // little-endian, no trailing zero limbs
  void trim();
};

// An RSA private key (modulus n, private exponent d). e is kept for reference.
struct RsaPrivateKey {
  BigInt n, e, d;
  size_t modulus_bytes = 0;  // ceil(bit_length(n)/8)
  bool valid = false;
};

// An RSA public key (n, e).
struct RsaPublicKey {
  BigInt n, e;
  size_t modulus_bytes = 0;
  bool valid = false;
};

// Parse a DER-encoded PKCS#1 RSAPrivateKey (the body of an
// "-----BEGIN RSA PRIVATE KEY-----" PEM) or a PKCS#8 PrivateKeyInfo wrapping
// one. Returns valid=false on failure.
RsaPrivateKey rsa_parse_private_key_der(const uint8_t* der, size_t len);
// Parse PEM (either RSA PRIVATE KEY or PRIVATE KEY). Unencrypted only.
RsaPrivateKey rsa_parse_private_key_pem(const std::string& pem);
// Parse PEM, decrypting an "ENCRYPTED PRIVATE KEY" (PKCS#8 PBES2: PBKDF2 +
// AES-CBC) with @password; unencrypted PEM is accepted too.
RsaPrivateKey rsa_parse_private_key_pem(const std::string& pem,
                                        const std::string& password);

// Decrypt a PKCS#8 EncryptedPrivateKeyInfo (PBES2: PBKDF2 + AES-128/256-CBC)
// to the inner PrivateKeyInfo DER, or empty on failure / unsupported scheme.
std::vector<uint8_t> decrypt_pkcs8_pbes2(const uint8_t* der, size_t len,
                                         const std::string& password);

// Decrypt PBES2 ciphertext given the AlgorithmIdentifier *content* (the OID +
// params inside the AlgorithmIdentifier SEQUENCE) and @enc. Returns the
// PKCS#7-unpadded plaintext, or empty. Shared by PKCS#8 and PKCS#12.
std::vector<uint8_t> pbes2_decrypt(const uint8_t* alg_content, size_t alg_len,
                                   const uint8_t* enc, size_t enc_len,
                                   const std::string& password);

// Sign @digest (a raw hash) with PKCS#1 v1.5 using the given DigestInfo ASN.1
// prefix for the hash algorithm. Returns the signature (modulus_bytes long),
// or empty on failure. For SHA-256 use rsa_sign_sha256().
std::vector<uint8_t> rsa_sign_pkcs1v15(const RsaPrivateKey& key,
                                       const uint8_t* digest_info,
                                       size_t digest_info_len);

// Convenience: hash is the 32-byte SHA-256 of the message; this prepends the
// standard SHA-256 DigestInfo and signs.
std::vector<uint8_t> rsa_sign_sha256(const RsaPrivateKey& key,
                                     const uint8_t sha256[32]);

// Verify a PKCS#1 v1.5 signature against an expected DigestInfo.
bool rsa_verify_pkcs1v15(const RsaPublicKey& key, const uint8_t* sig,
                         size_t sig_len, const uint8_t* digest_info,
                         size_t digest_info_len);

// Verify an RSASSA-PSS signature (EMSA-PSS-VERIFY with MGF1). @mhash is the
// already-computed message hash of length @hlen; @hlen selects both the message
// hash and the MGF1/H' hash (32 -> SHA-256, 48 -> SHA-384, 64 -> SHA-512). The
// salt length is auto-detected from the encoded message, so this accepts the
// TLS 1.3 rsa_pss_rsae_* schemes (salt == hash length) and PSS X.509 signatures
// with any salt length.
bool rsa_verify_pss(const RsaPublicKey& key, const uint8_t* sig, size_t sig_len,
                    const uint8_t* mhash, size_t hlen);

}  // namespace crypto
}  // namespace nanopdf

#endif  // NANOPDF_CRYPTO_PK_HH_
