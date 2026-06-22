/* SPDX-License-Identifier: Apache-2.0
 * Minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519) over the ncrypto
 * primitives. Validates the server certificate chain by default (X.509 +
 * system trust store + hostname). For fetching RFC 3161 / OpenTimestamps
 * responses without OpenSSL. POSIX sockets. */
#ifndef NCRYPTO_NC_TLS_H_
#define NCRYPTO_NC_TLS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTPS POST to https://host[:port]/path over a hand-rolled TLS 1.3 connection.
 * On success returns 0 and sets *resp_out (malloc'd; caller frees) + *resp_len
 * to the response body (HTTP status must be 200). On failure returns -1 and
 * writes a message to @err (NUL-terminated, up to @errcap). When @verify_cert
 * is non-zero the server chain is validated against the system trust store and
 * @host; pass 0 to accept any certificate. */
int nc_tls_https_post(const char* host, const char* port, const char* path,
                      const char* content_type, const char* accept,
                      const uint8_t* body, size_t body_len, int verify_cert,
                      uint8_t** resp_out, size_t* resp_len, char* err,
                      size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_TLS_H_ */
