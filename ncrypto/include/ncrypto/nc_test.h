/* SPDX-License-Identifier: Apache-2.0
 * Tiny test harness for the ncrypto C11 library. Each test file has its own
 * main() and returns nc_test_report(). */
#ifndef NCRYPTO_NC_TEST_H_
#define NCRYPTO_NC_TEST_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int nc_test_pass = 0;
static int nc_test_fail = 0;

#define NC_CHECK(cond)                                                      \
  do {                                                                      \
    if (cond) {                                                            \
      nc_test_pass++;                                                       \
    } else {                                                                \
      nc_test_fail++;                                                       \
      printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                       \
  } while (0)

#define NC_CHECK_EQ_HEX(buf, len, expect_hex)                              \
  do {                                                                      \
    char nc__got[1024];                                                     \
    nc_test_to_hex((buf), (len), nc__got);                                  \
    if (strcmp(nc__got, (expect_hex)) == 0) {                              \
      nc_test_pass++;                                                       \
    } else {                                                                \
      nc_test_fail++;                                                       \
      printf("  FAIL %s:%d  got %s want %s\n", __FILE__, __LINE__, nc__got, \
             (expect_hex));                                                 \
    }                                                                       \
  } while (0)

static void nc_test_to_hex(const uint8_t* b, size_t n, char* out) {
  static const char* H = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; i++) {
    out[2 * i] = H[b[i] >> 4];
    out[2 * i + 1] = H[b[i] & 0xF];
  }
  out[2 * n] = 0;
}

/* Parse a hex string into bytes; returns the byte count. */
static size_t nc_test_from_hex(const char* h, uint8_t* out) {
  size_t i = 0, j = 0;
  while (h[i] && h[i + 1]) {
    int hi = h[i] <= '9' ? h[i] - '0' : (h[i] | 32) - 'a' + 10;
    int lo = h[i + 1] <= '9' ? h[i + 1] - '0' : (h[i + 1] | 32) - 'a' + 10;
    out[j++] = (uint8_t)(hi * 16 + lo);
    i += 2;
  }
  return j;
}

static int nc_test_report(void) {
  printf("  %d passed, %d failed\n", nc_test_pass, nc_test_fail);
  return nc_test_fail ? 1 : 0;
}

#endif /* NCRYPTO_NC_TEST_H_ */
