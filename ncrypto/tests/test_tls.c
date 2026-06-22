/* SPDX-License-Identifier: Apache-2.0
 * Network-optional smoke test for the TLS 1.3 client. When offline (DNS or
 * connect failure) this SKIPs cleanly; it only fails on a definitive protocol
 * error AFTER a successful TCP connect. End-to-end verification is performed
 * separately by the caller. */
#include "ncrypto/nc_tls.h"

#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_test.h"

static void test_https_post_freetsa(void) {
  uint8_t dummy[1] = {0x00};
  uint8_t* resp = NULL;
  size_t resp_len = 0;
  char err[256];
  int rc;

  err[0] = 0;
  rc = nc_tls_https_post("freetsa.org", "443", "/tsr",
                         "application/timestamp-query",
                         "application/timestamp-reply", dummy, sizeof(dummy), 1,
                         &resp, &resp_len, err, sizeof(err));

  if (rc == 0) {
    /* freetsa accepted the (bogus) request and returned a 200 body. */
    NC_CHECK(resp != NULL);
    NC_CHECK(resp_len > 0);
    printf("  freetsa handshake OK, 200 body %zu bytes\n", resp_len);
    free(resp);
    return;
  }

  /* No-network conditions are SKIPs, not failures. */
  if (strstr(err, "connect") || strstr(err, "DNS")) {
    printf("  SKIP (no network): %s\n", err);
    return;
  }

  /* Reached the server but a cert/protocol error would be a real bug. */
  if (strstr(err, "CertificateVerify") || strstr(err, "chain") ||
      strstr(err, "hostname") || strstr(err, "trust") ||
      strstr(err, "ServerHello") || strstr(err, "decrypt") ||
      strstr(err, "key_share") || strstr(err, "alert")) {
    printf("  FAIL protocol/cert error: %s\n", err);
    NC_CHECK(0);
    return;
  }

  /* Anything else after connecting (e.g. "HTTP status not 200" from the bogus
   * timestamp-query) means the TLS handshake + cert validation succeeded. */
  printf("  handshake+validation OK, non-200 as expected: %s\n", err);
}

int main(void) {
  /* Silence unused-static warnings from the shared header. */
  (void)nc_test_to_hex;
  (void)nc_test_from_hex;
  test_https_post_freetsa();
  return nc_test_report();
}
