// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// tls_client — a minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519) built
// on nanopdf's pure-C++ tls-crypto. Encryption only: the server certificate is
// NOT validated (matching the prior SSL_VERIFY_NONE posture; the TSA/OTS tokens
// fetched over it are self-verifying). For fetching RFC 3161 timestamps and
// OpenTimestamps calendar responses without OpenSSL.

#ifndef PDFVIEW_TLS_CLIENT_HH_
#define PDFVIEW_TLS_CLIENT_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace pdfview {

// Perform an HTTPS POST to https://host[:port]/path over a hand-rolled TLS 1.3
// connection. Returns the response body bytes (status must be 200), or empty
// with @err set. @accept/@content_type set the corresponding headers.
std::vector<uint8_t> tls_https_post(const std::string& host,
                                    const std::string& port,
                                    const std::string& path,
                                    const std::string& content_type,
                                    const std::string& accept,
                                    const std::vector<uint8_t>& body,
                                    std::string* err);

}  // namespace pdfview

#endif  // PDFVIEW_TLS_CLIENT_HH_
