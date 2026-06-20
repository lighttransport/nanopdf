/* SPDX-License-Identifier: Apache-2.0
 * Tests for nc_x509: parsing, signature verification, chain verification. */

#include "ncrypto/nc_x509.h"

#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_test.h"

static const char* kRsaPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNDCCAhygAwIBAgIUMyvowQIoDOIz6XWLARhjnkhBOAMwDQYJKoZIhvcNAQEL\n"
    "BQAwGzEZMBcGA1UEAwwQbmFub3BkZiByc2EgdGVzdDAeFw0yNjA2MjAxNDM2NTla\n"
    "Fw0zNjA2MTcxNDM2NTlaMBsxGTAXBgNVBAMMEG5hbm9wZGYgcnNhIHRlc3QwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCKxXXiirToOa5gLQUmBqeAW5Cq\n"
    "TpeSn2vRbl5XoFN0pTv7xLH/ZEpB3hIh3mGzdMydTUI3tMJ73Q188Wyi3GxUaqAE\n"
    "GtGWIaI3JfQienFTp/LSzF9czimkw3xzCk6kRxqChHP4CJCO+lDZcaQ+rFowRQDs\n"
    "2FVbFAavC5p0AVzpCvvHh1RBz058BGVM83j45kAr9u0LmO2NaWJtgMSu4Zhd+aqK\n"
    "s6JJ/JNxUyFwwSVdN5u2NZzXaLNKt3MOLZ9PzVVpg+vztVYID1nQIRdSdTihX6rA\n"
    "RNljMP9YH8EzfadVwA+t9N4xxIP5NNuXOEh3g5G8hxzL2y1vog29e/5tYnozAgMB\n"
    "AAGjcDBuMB0GA1UdDgQWBBQiK59V1QYXVXTVEFGI9NAGPTeeqzAfBgNVHSMEGDAW\n"
    "gBQiK59V1QYXVXTVEFGI9NAGPTeeqzAPBgNVHRMBAf8EBTADAQH/MBsGA1UdEQQU\n"
    "MBKCEHJzYS50ZXN0LmV4YW1wbGUwDQYJKoZIhvcNAQELBQADggEBAHvvhBizZjko\n"
    "Hm6XB5XMjWjUdRut2tx59/5L30FU/MPTw+geEi+vcyKhRyBw+FPflwTrqqp5cLt/\n"
    "3kByVzoe6IaNdig560C4R3mw70EXZKTtp/pimtgm74wp3oFURQS3rGkVo3gXk1EI\n"
    "tlC68W0di6OllvPQj0NyNNLBdvAYXjLn+HNY0dLHk6CGRKczZfEW2FZAdJN1fgGl\n"
    "/3+DbJdjUAXodZGYlvpxuOONtZ/TubM2pG2iWrFslSr2WFJFXRkTaYs1BIrz2It3\n"
    "tCki6zBaWBnBdpOV0m2Nisn/0o/95zsn/NTcgpS8gRE5adAmqPZL9l9qUDT8PrOD\n"
    "SFc0lWDbS+w=\n"
    "-----END CERTIFICATE-----\n";

static const char* kEcPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBpDCCAUugAwIBAgIUEoN1oIogurqZkRlhx1/rgzjmbjgwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPbmFub3BkZiBlYyB0ZXN0MB4XDTI2MDYyMDE0MzY1OVoXDTM2\n"
    "MDYxNzE0MzY1OVowGjEYMBYGA1UEAwwPbmFub3BkZiBlYyB0ZXN0MFkwEwYHKoZI\n"
    "zj0CAQYIKoZIzj0DAQcDQgAEbjHIHQn55eJGolig+RzceIMFuL0lFn1rNGGof8x0\n"
    "OJwWBg/B/oeXouTB9n4fTrZtDEiYHFJsQt43DzQWwZfWnaNvMG0wHQYDVR0OBBYE\n"
    "FNjfBpEMKw5nU1y5Cw4ngW+M0p76MB8GA1UdIwQYMBaAFNjfBpEMKw5nU1y5Cw4n\n"
    "gW+M0p76MA8GA1UdEwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIPZWMudGVzdC5leGFt\n"
    "cGxlMAoGCCqGSM49BAMCA0cAMEQCIBAU2PlV8vyVkd8dtZ/WmHU3DoombjGHSMK+\n"
    "lue1oEbkAiA8e3ddvZck1ioeJEnYNejoN5o34Kt4XwkGlx2Yw06FAg==\n"
    "-----END CERTIFICATE-----\n";

/* Minimal base64 PEM->DER decoder for the test. Returns DER length or 0. */
static size_t pem_to_der(const char* pem, uint8_t* out, size_t outcap) {
  static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
  static const char kEnd[] = "-----END CERTIFICATE-----";
  int8_t tbl[256];
  int i;
  const char* b = strstr(pem, kBegin);
  const char* body;
  const char* e;
  int bits = 0;
  uint32_t acc = 0;
  size_t outn = 0;
  if (!b) return 0;
  body = b + (sizeof(kBegin) - 1);
  e = strstr(body, kEnd);
  if (!e) return 0;
  for (i = 0; i < 256; ++i) tbl[i] = -1;
  for (i = 0; i < 26; ++i) {
    tbl['A' + i] = (int8_t)i;
    tbl['a' + i] = (int8_t)(26 + i);
  }
  for (i = 0; i < 10; ++i) tbl['0' + i] = (int8_t)(52 + i);
  tbl['+'] = 62;
  tbl['/'] = 63;
  for (; body < e; ++body) {
    unsigned char ch = (unsigned char)*body;
    int8_t v;
    if (ch == '=') break;
    v = tbl[ch];
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outn >= outcap) return 0;
      out[outn++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return outn;
}

static void test_rsa(void) {
  uint8_t der[4096];
  size_t der_len = pem_to_der(kRsaPem, der, sizeof(der));
  nc_x509_cert c;
  char cn[256];
  int cnlen;
  NC_CHECK(der_len > 0);
  NC_CHECK(nc_x509_parse(&c, der, der_len) == 0);
  NC_CHECK(c.parsed);
  NC_CHECK(c.key_type == NC_KEY_RSA);
  NC_CHECK(c.rsa_pub.valid);
  NC_CHECK(c.sig_alg == NC_SIG_RSA_PKCS1_SHA256);
  NC_CHECK(c.is_ca);
  NC_CHECK(nc_x509_self_issued(&c));
  cnlen = nc_x509_subject_cn(&c, cn, sizeof(cn));
  NC_CHECK(cnlen == (int)strlen("nanopdf rsa test"));
  NC_CHECK(strcmp(cn, "nanopdf rsa test") == 0);
  NC_CHECK(c.san_count == 1);
  NC_CHECK(c.san_len[0] == strlen("rsa.test.example"));
  NC_CHECK(c.san_count == 1 &&
           memcmp(c.san[0], "rsa.test.example", c.san_len[0]) == 0);
  NC_CHECK(c.not_before > 0);
  NC_CHECK(c.not_after > c.not_before);
  /* Self-signed: verify against itself. */
  NC_CHECK(nc_x509_verify_signature(&c, &c) == 1);

  /* Tamper: copy the cert struct but point its TBS at a mutated DER copy so the
   * hashed message differs -> signature verification must fail. */
  {
    static uint8_t tampered[4096];
    nc_x509_cert tc = c;
    size_t off = (size_t)(c.tbs - der) + c.tbs_len / 2;
    memcpy(tampered, der, der_len);
    tampered[off] ^= 0xFF;
    tc.tbs = tampered + (size_t)(c.tbs - der);
    NC_CHECK(nc_x509_verify_signature(&tc, &c) == 0);
  }
}

static void test_ec(void) {
  uint8_t der[4096];
  size_t der_len = pem_to_der(kEcPem, der, sizeof(der));
  nc_x509_cert c;
  char cn[256];
  NC_CHECK(der_len > 0);
  NC_CHECK(nc_x509_parse(&c, der, der_len) == 0);
  NC_CHECK(c.parsed);
  NC_CHECK(c.key_type == NC_KEY_EC);
  NC_CHECK(c.ec_curve == nc_curve_p256());
  NC_CHECK(c.sig_alg == NC_SIG_ECDSA_SHA256);
  nc_x509_subject_cn(&c, cn, sizeof(cn));
  NC_CHECK(strcmp(cn, "nanopdf ec test") == 0);
  NC_CHECK(c.san_count == 1);
  NC_CHECK(c.san_len[0] == strlen("ec.test.example"));
  NC_CHECK(memcmp(c.san[0], "ec.test.example", c.san_len[0]) == 0);
  NC_CHECK(nc_x509_verify_signature(&c, &c) == 1);

  {
    static uint8_t tampered[4096];
    nc_x509_cert tc = c;
    size_t off = (size_t)(c.tbs - der) + c.tbs_len / 2;
    memcpy(tampered, der, der_len);
    tampered[off] ^= 0xFF;
    tc.tbs = tampered + (size_t)(c.tbs - der);
    NC_CHECK(nc_x509_verify_signature(&tc, &c) == 0);
  }
}

static void test_chain(void) {
  uint8_t der[4096];
  size_t der_len = pem_to_der(kRsaPem, der, sizeof(der));
  nc_x509_cert root;
  nc_trust_store store;
  const uint8_t* chain[1];
  size_t lens[1];
  nc_verify_result res;

  NC_CHECK(der_len > 0);
  NC_CHECK(nc_x509_parse(&root, der, der_len) == 0);

  /* Manual stack trust store referencing the parsed self-signed cert. */
  memset(&store, 0, sizeof(store));
  store.roots = &root;
  store.root_der = NULL;
  store.root_der_len = NULL;
  store.count = 1;
  store.loaded = 1;

  chain[0] = der;
  lens[0] = der_len;

  /* Correct hostname. */
  NC_CHECK(nc_x509_verify_chain(&res, chain, lens, 1, &store,
                                "rsa.test.example", 0) == 0);
  NC_CHECK(res.ok == 1);

  /* Wrong hostname. */
  NC_CHECK(nc_x509_verify_chain(&res, chain, lens, 1, &store, "other.example",
                                0) == -1);
  NC_CHECK(res.ok == 0);

  /* Empty store. */
  {
    nc_trust_store empty;
    memset(&empty, 0, sizeof(empty));
    empty.count = 0;
    NC_CHECK(nc_x509_verify_chain(&res, chain, lens, 1, &empty,
                                  "rsa.test.example", 0) == -1);
  }
}

static void test_trust_store_load(void) {
  nc_trust_store s;
  if (nc_trust_store_load(&s, NULL) == 0) {
    NC_CHECK(s.count > 0);
    nc_trust_store_free(&s);
  }
  /* else: no system CA bundle present; skip without failing. */
}

int main(void) {
  (void)nc_test_to_hex;
  (void)nc_test_from_hex;
  test_rsa();
  test_ec();
  test_chain();
  test_trust_store_load();
  return nc_test_report();
}
