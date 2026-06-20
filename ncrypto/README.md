# ncrypto — a pure C11 crypto stack

`ncrypto` is a small, dependency-free (libc only) cryptography library written in
**C11**. It is a from-scratch, OpenSSL-free stack providing exactly what is
needed to sign and timestamp PDFs and to talk to TLS-protected timestamp
authorities. It mirrors the C++ implementation embedded in nanopdf
(`src/crypto-pk`, `ecc`, `x509`, `cms`, `rfc3161`, `pkcs12`, and
`examples/pdfview/tls_client`) as a standalone C library.

## Modules

| Header | Provides |
| --- | --- |
| `nc_hash.h`    | SHA-1 / SHA-256 / SHA-384 / SHA-512 (streaming + one-shot) |
| `nc_kdf.h`     | HMAC, HKDF (+ TLS 1.3 HKDF-Expand-Label), PBKDF2 |
| `nc_aes.h`     | AES-128/256 block cipher, GCM (AEAD), CBC |
| `nc_x25519.h`  | X25519 (RFC 7748) |
| `nc_bigint.h`  | Fixed-capacity big integers (add/sub/mul/divmod/modexp) |
| `nc_rsa.h`     | RSA PKCS#1 v1.5 sign/verify, RSASSA-PSS verify, key parsing (PKCS#1/#8, PBES2) |
| `nc_ecc.h`     | ECDSA verify over NIST P-256 / P-384 |
| `nc_asn1.h`    | ASN.1 DER encoder (growable buffer) + TLV reader |
| `nc_x509.h`    | X.509 parse, signature/chain verification, system trust store, hostname check |
| `nc_cms.h`     | CMS / PKCS#7 detached SignedData build + verify (with RFC 3161 timestamp attr) |
| `nc_rfc3161.h` | RFC 3161 TimeStampReq build / TimeStampResp parse |
| `nc_pkcs12.h`  | PKCS#12 (PFX) key + certificate extraction (PBES2/AES) |
| `nc_tls.h`     | Minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256 + X25519) with cert validation |

## Build & test

```sh
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

Every module has a known-answer test (vectors cross-checked against OpenSSL /
the relevant RFCs). The whole library builds warning-clean under `-Wall -Wextra`.

## Scope / non-goals

Verification-only ECDSA (no EC keygen/sign); TLS is 1.3-only with one cipher
suite and validates RSA-PSS / ECDSA P-256 / P-384 server certificates (no
P-521 / EdDSA, no CRL/OCSP revocation). PKCS#12 / PBES2 cover the
OpenSSL-3-default AES schemes (no 3DES/RC2). Not constant-time; intended for
local, offline document signing and fetching self-verifying TSA/OTS tokens, not
as a general-purpose TLS server stack.
