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
