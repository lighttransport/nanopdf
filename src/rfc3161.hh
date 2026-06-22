// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// rfc3161 — pure-C++ RFC 3161 timestamp request builder + response parser,
// built on asn1-der. Networking is intentionally NOT here: the caller provides
// HTTP transport (so the core stays dependency-free).

#ifndef NANOPDF_RFC3161_HH_
#define NANOPDF_RFC3161_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace rfc3161 {

using Bytes = std::vector<uint8_t>;

// Hash-algorithm OID for the message imprint. "sha256" | "sha384" | "sha512".
std::string hash_oid(const std::string& name);

// Build a DER-encoded TimeStampReq over @imprint (the hash of the data to be
// timestamped, computed with @hash_oid). @nonce is a random 64-bit value
// (echoed back by the TSA); @cert_req asks the TSA to include its certificate.
Bytes build_request(const Bytes& imprint, const std::string& hash_oid,
                    uint64_t nonce, bool cert_req = true);

// Parse a DER TimeStampResp and return the embedded TimeStampToken (a CMS
// ContentInfo, DER) on success, or empty on failure. @status (optional) gets
// the PKIStatus value (0 = granted, 1 = grantedWithMods).
Bytes parse_response(const Bytes& tsr, int* status = nullptr);

}  // namespace rfc3161
}  // namespace nanopdf

#endif  // NANOPDF_RFC3161_HH_
