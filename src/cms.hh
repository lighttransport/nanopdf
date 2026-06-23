// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// cms — pure-C++ PKCS#7 / CMS SignedData builder for PDF signatures
// (adbe.pkcs7.detached), built on asn1-der + crypto-pk. No OpenSSL.

#ifndef NANOPDF_CMS_HH_
#define NANOPDF_CMS_HH_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "crypto-pk.hh"

namespace nanopdf {
namespace cms {

using Bytes = std::vector<uint8_t>;

// Minimal X.509 view: the raw DER of the issuer Name and the serialNumber
// INTEGER value bytes (without tag/length), extracted from a certificate.
struct CertInfo {
  Bytes issuer_der;     // full DER of the issuer Name (a SEQUENCE)
  Bytes serial_be;      // serial number, big-endian magnitude (DER INTEGER body)
  bool valid = false;
};

// Extract issuer + serial from a DER-encoded X.509 certificate.
CertInfo parse_certificate(const Bytes& cert_der);

// Decode every "-----BEGIN CERTIFICATE-----" block in @pem to DER (in order).
std::vector<Bytes> pem_to_certs(const std::string& pem);

// Extract the embedded certificates (DER) from a CMS/PKCS#7 SignedData, in the
// order they appear in the [0] certificates field (signer first for documents
// nanopdf produces). Empty if none / parse failure.
std::vector<Bytes> extract_certificates(const Bytes& cms_der);

// Extract the RSA public key from an X.509 certificate's SubjectPublicKeyInfo.
crypto::RsaPublicKey extract_rsa_public_key(const Bytes& cert_der);

// Subject commonName of a certificate (best-effort; empty if absent).
std::string subject_common_name(const Bytes& cert_der);

// Result of verifying a CMS SignedData over detached content.
struct VerifyInfo {
  bool parsed = false;            // the CMS structure was parsed
  bool signature_valid = false;   // RSA signature over signedAttrs verified
  bool digest_valid = false;      // messageDigest attr == SHA-256(content)
  std::string signer_cn;
  std::string digest_algorithm;   // e.g. "SHA-256"
  std::string signing_time;       // signingTime signed attribute (raw)
  bool has_timestamp = false;
  std::string timestamp_time;     // RFC 3161 TSTInfo genTime (raw)
  std::string timestamp_authority;
  std::string error;
};

// Verify a detached PKCS#7/CMS SignedData @cms_der over @content (RSA + SHA-256;
// PKCS#1 v1.5). Does not validate the certificate chain against trust anchors —
// it confirms the signature math and that the embedded digest covers @content,
// and surfaces signer + timestamp metadata.
VerifyInfo verify_signed_data(const Bytes& cms_der, const Bytes& content);

// Given the raw RSA signature value, return an RFC 3161 TimeStampToken (a CMS
// ContentInfo, DER) to embed as the id-aa-timeStampToken unsigned attribute, or
// empty to skip timestamping. Used to turn a basic signature into PAdES-T.
using TsaCallback = std::function<Bytes(const Bytes& signature)>;

// Build a detached PKCS#7/CMS SignedData (RSA + SHA-256) over @content.
// @signer_cert_der is the signer's certificate; @chain_der are extra certs to
// embed. @signing_time_utc is "YYMMDDhhmmssZ" (UTCTime). If @tsa is set and
// returns a non-empty token, it is embedded as a signature timestamp. Returns
// DER bytes, or empty on failure.
Bytes build_signed_data(const Bytes& content, const Bytes& signer_cert_der,
                        const std::vector<Bytes>& chain_der,
                        const crypto::RsaPrivateKey& key,
                        const std::string& signing_time_utc,
                        const TsaCallback& tsa = {});

}  // namespace cms
}  // namespace nanopdf

#endif  // NANOPDF_CMS_HH_
