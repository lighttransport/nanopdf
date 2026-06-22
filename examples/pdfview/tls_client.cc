// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// tls_https_post is a thin C++ wrapper over the ncrypto pure-C11 TLS 1.3 client
// (nc_tls_https_post). The handshake, record layer, and certificate-chain
// validation all live in ncrypto now; this file just adapts the buffer types.

#include "tls_client.hh"

#include <cstdlib>

#include "ncrypto/nc_tls.h"

namespace pdfview {

std::vector<uint8_t> tls_https_post(const std::string& host,
                                    const std::string& port,
                                    const std::string& path,
                                    const std::string& content_type,
                                    const std::string& accept,
                                    const std::vector<uint8_t>& body,
                                    std::string* err, bool verify_cert) {
  uint8_t* resp = nullptr;
  size_t resp_len = 0;
  char e[256] = {0};
  int rc = nc_tls_https_post(host.c_str(), port.c_str(), path.c_str(),
                             content_type.c_str(), accept.c_str(), body.data(),
                             body.size(), verify_cert ? 1 : 0, &resp, &resp_len,
                             e, sizeof(e));
  if (rc != 0) {
    if (err) *err = e[0] ? e : "tls error";
    if (resp) free(resp);
    return {};
  }
  std::vector<uint8_t> out(resp, resp + resp_len);
  free(resp);
  return out;
}

}  // namespace pdfview
