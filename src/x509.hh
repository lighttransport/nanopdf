// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// x509 — minimal X.509 certificate parsing and chain verification, built on the
// pure-C++ RSA (crypto-pk) and ECDSA (ecc) primitives. Supports the validation
// path needed for TLS server authentication: signature verification of RSA
// (PKCS#1 v1.5 + PSS) and ECDSA (P-256/P-384) certificates, chain building up to
// a system trust anchor, validity-period checks, and hostname (SAN) matching.
//
// Out of scope (deliberately, for this offline-document / TSA-fetch use case):
// CRL/OCSP revocation, name constraints, extended key usage, policy mapping.

#ifndef NANOPDF_X509_HH_
#define NANOPDF_X509_HH_

#include <cstdint>
#include <string>
#include <vector>

#include "crypto-pk.hh"
#include "ecc.hh"

namespace nanopdf {
namespace x509 {

using Bytes = std::vector<uint8_t>;

enum class SigAlg {
  Unknown,
  RsaPkcs1Sha1,
  RsaPkcs1Sha256,
  RsaPkcs1Sha384,
  RsaPkcs1Sha512,
  RsaPss,
  EcdsaSha1,
  EcdsaSha256,
  EcdsaSha384,
  EcdsaSha512,
};

enum class KeyType { Unknown, Rsa, Ec };

struct Certificate {
  bool parsed = false;

  Bytes tbs;          // raw DER of TBSCertificate (tag..value), for sig verify
  SigAlg sig_alg = SigAlg::Unknown;
  size_t pss_hash_len = 32;  // for RsaPss: the MGF/message hash length
  Bytes signature;    // signatureValue bits (unused-bits byte stripped)

  Bytes issuer_der;   // full DER of issuer Name
  Bytes subject_der;  // full DER of subject Name

  int64_t not_before = 0;  // epoch seconds
  int64_t not_after = 0;

  KeyType key_type = KeyType::Unknown;
  crypto::RsaPublicKey rsa_pub;          // valid if key_type == Rsa
  const crypto::EcCurve* ec_curve = nullptr;  // for key_type == Ec
  Bytes ec_point;                        // uncompressed point (0x04||X||Y)

  std::vector<std::string> san_dns;      // dNSName SAN entries
  bool is_ca = false;                    // basicConstraints cA

  bool self_issued() const { return !issuer_der.empty() &&
                                    issuer_der == subject_der; }
  std::string subject_cn() const;
};

// Parse a DER certificate. Returns parsed=false on failure.
Certificate parse(const uint8_t* der, size_t len);

// Verify @child's signature using @issuer's public key. Returns true iff valid.
bool verify_signature(const Certificate& child, const Certificate& issuer);

// Verify a TLS 1.3 CertificateVerify-style signature: @sig over @msg using
// @leaf's public key, dispatched by the TLS SignatureScheme @scheme (e.g.
// 0x0804 rsa_pss_rsae_sha256, 0x0403 ecdsa_secp256r1_sha256). @sig for ECDSA is
// the DER ECDSA-Sig-Value. Returns true iff valid.
bool verify_tls13_signature(const Certificate& leaf, uint16_t scheme,
                            const uint8_t* msg, size_t msg_len,
                            const uint8_t* sig, size_t sig_len);

// A set of trusted root certificates.
struct TrustStore {
  std::vector<Certificate> roots;
  bool loaded = false;
};

// Load the system CA bundle (tries common Linux/BSD paths). Also accepts an
// explicit PEM bundle path via @path (empty -> autodetect).
TrustStore load_trust_store(const std::string& path = "");

struct VerifyResult {
  bool ok = false;
  std::string error;
  std::string subject_cn;
};

// Verify a server certificate chain (@der_chain[0] = leaf, rest = intermediates
// in any order) against @store, checking validity at @now_epoch and that the
// leaf authorizes @hostname (SAN dNSName, with wildcard support). If @hostname
// is empty the hostname check is skipped.
VerifyResult verify_chain(const std::vector<Bytes>& der_chain,
                          const TrustStore& store, const std::string& hostname,
                          int64_t now_epoch);

}  // namespace x509
}  // namespace nanopdf

#endif  // NANOPDF_X509_HH_
