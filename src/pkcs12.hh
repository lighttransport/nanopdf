// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pkcs12 — pure-C++ reader for PKCS#12 (.p12/.pfx) bundles that use PBES2
// (PBKDF2 + AES-CBC) for both the shrouded key bag and the cert bags — the
// OpenSSL 3 default. Legacy PBES1 ciphers (3DES/RC2) are not supported. The MAC
// is not verified (the password is validated implicitly by key decryption).

#ifndef NANOPDF_PKCS12_HH_
#define NANOPDF_PKCS12_HH_

#include <cstdint>
#include <string>
#include <vector>

#include "crypto-pk.hh"

namespace nanopdf {
namespace pkcs12 {

struct Bundle {
  crypto::RsaPrivateKey key;
  std::vector<std::vector<uint8_t>> certs;  // DER certificates
  bool valid = false;
  std::string error;
};

// Parse a PKCS#12 bundle with @password. Returns valid=true with the private
// key and certificate(s) on success.
Bundle parse(const uint8_t* data, size_t len, const std::string& password);

}  // namespace pkcs12
}  // namespace nanopdf

#endif  // NANOPDF_PKCS12_HH_
