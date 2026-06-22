/* SPDX-License-Identifier: Apache-2.0
 * Tests for nc_cms: build + verify a detached CMS SignedData round-trip. */
#include "ncrypto/nc_cms.h"

#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_rsa.h"
#include "ncrypto/nc_test.h"

static const char* kKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCV1/53XI0Ms7C2\n"
    "h3GqpHxt8fHjCAsucSCv40fTLf2yR+5Iwa63gZZekLTWy0aXYA+D4QfeUpVB+0VX\n"
    "ENtYHcpzodWbiFhgiAYRss4SIFxYVBknVK5Yrdu3jpS67HI2gdlU3lkgpkzClZWV\n"
    "hCDCnMT9UVIlsNPav4tIeI/bPWhC6arBi+t8I4H4qN3pOheRGn63U1QZe//SfYUJ\n"
    "Co3UkaWck/81iJJOFOvmfzV6ybfFABCq7Qv3lG9opZuRgI2iOyZx4SIq4gKasTrG\n"
    "n2NOlMy3MyT4Ifgb1m3ebNVA1Kmd0phC87AHXcdf6i/mma1WJQLZi08ClPLfpat6\n"
    "X1KnLZD/AgMBAAECggEAGiQ8G0PUwb4JmtaHYI9gu+elXQfWt4v5+WWL7s5Lv9In\n"
    "wp6zpgUgIDBbtEtzbc5O9qGIN2Ot8XxuTPG+aqGL4998fi1rya+Ba44HwIbPxYMq\n"
    "fj+aSRNPzTa9QN0mUdcWfOVrpDQPGTVyisqp0nn6Lf2FeNjgJP4cxRBvfH66lU2d\n"
    "TVpCiKgimIJxMYJ4ADNR5s8IuqzaxrPP/n+e3xntY7Qib0bf9P1kvnGMTKYQDQxM\n"
    "2+Axc4g20cfwKXddn+2QYErwCe6FkfF1YbIBQC8Dl5BWOfXtRia7uakftT/gkJb1\n"
    "M2eLjbcvOVIirlJgDxdpmRcWpUgAiw60c8SRZrnKvQKBgQDPrvpEok2a1subflPP\n"
    "y375u0SvV5ZjkU++Z073g24w1EApdZ0DQyNwMmUpREw6XBWNhw73NX7Vyt4W5nVN\n"
    "kOiIq/z+5OiEO1pL5t1DOsafzNFZxQNXBJAUXmQNrAaSDWVcfiFpco7XRtc4SFOo\n"
    "vi2dCvXDJ7MZoR5UgBAdSZer1QKBgQC4tD+MhZDSr8jeVXUHnqnbsxPK4iu0VYp0\n"
    "0lnPu2V5bPOy7Gn95n7C1IFXEhBu6NuaNZxV8U1JxcBmPebO1UdemhPSYURd7foE\n"
    "YGSz6qVfwyT/WfDqA7d36Jm/s+Riw0wLYuoCqmBJQZTUyoEes5XlCkAQ3iXc+2tU\n"
    "R8SysF6XgwKBgDKCiOWWX69v3BcWM0YPZPNRw1OtxqqylaVmNMNn2K3RgUVUEHrR\n"
    "olXQO+A8dxmeebNxDIe9H/rZGwiQxii2PIe45JANlitK0Bwzqs6GBfapdqURkE1i\n"
    "k7QQfN8CXpq43VUQbAncTbc3yHIszQP2NNvD5Z+wERQDpn/Aoaqt0lB9AoGBAJW5\n"
    "H06EsvnmntiDw4MyvZQnXOL4Zd8f/EVogn8e9Eny5LVkaSL1Jko+wr3XGdUeE221\n"
    "CY6tNS6ZC7aVdTSytNDd6zV9vVK65xGHJfqmOfofkS0hNmYsLxwXRBKe+4KHt2v/\n"
    "eZxBih23+LJpmTNO9jIdFgPWYDD66Wz/ZFZJG9SLAoGBAL1dnI/Eg7tQlu8qH4pJ\n"
    "o8npG16kdFU/4aEHloshI6JSoK4ViblsZrNJroNmtZZKrFMPT/KX4Ou0cGVNK1x9\n"
    "ny82wp3jWggFK8Q9tUArIFlcoJIp1d2kYDRfuo4N6ewedrWjcXfhvbAXkPThiYXq\n"
    "uBzlshvC1tKm6n59MxVFvgDZ\n"
    "-----END PRIVATE KEY-----\n";

static const char* kCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDGzCCAgOgAwIBAgIUNO37dXB6UB5eRiPgKpuNGGUlWxUwDQYJKoZIhvcNAQEL\n"
    "BQAwHTEbMBkGA1UEAwwSbmFub3BkZiBjbXMgc2lnbmVyMB4XDTI2MDYyMDE1NDkz\n"
    "NVoXDTM2MDYxNzE1NDkzNVowHTEbMBkGA1UEAwwSbmFub3BkZiBjbXMgc2lnbmVy\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAldf+d1yNDLOwtodxqqR8\n"
    "bfHx4wgLLnEgr+NH0y39skfuSMGut4GWXpC01stGl2APg+EH3lKVQftFVxDbWB3K\n"
    "c6HVm4hYYIgGEbLOEiBcWFQZJ1SuWK3bt46UuuxyNoHZVN5ZIKZMwpWVlYQgwpzE\n"
    "/VFSJbDT2r+LSHiP2z1oQumqwYvrfCOB+Kjd6ToXkRp+t1NUGXv/0n2FCQqN1JGl\n"
    "nJP/NYiSThTr5n81esm3xQAQqu0L95RvaKWbkYCNojsmceEiKuICmrE6xp9jTpTM\n"
    "tzMk+CH4G9Zt3mzVQNSpndKYQvOwB13HX+ov5pmtViUC2YtPApTy36Wrel9Spy2Q\n"
    "/wIDAQABo1MwUTAdBgNVHQ4EFgQUnccbyGOTEy1oN8/wH5MWLQDrFg8wHwYDVR0j\n"
    "BBgwFoAUnccbyGOTEy1oN8/wH5MWLQDrFg8wDwYDVR0TAQH/BAUwAwEB/zANBgkq\n"
    "hkiG9w0BAQsFAAOCAQEANxcUFi0sg4XsY/rCZWupnfLrYtAGOjOnDrU7Ih7QgQQV\n"
    "RHEBwEXcrBJJpLFhJgMYVMlJ/JTSvSDU7OkHSATyVoe4ZhRnKSurt7hKEkmhF6et\n"
    "J1snFZEsms7s9atl/M0FPEjH0I5Ptt4vzlK907T+TjGmV8WIVd55F2ynJ/hkj6Re\n"
    "lCK++t4OtfcnF2BI4ojEddtoT0cVofSeM8B2x0rXOGwkYzbpgJhtzyUvkcln5k8L\n"
    "s26vQcyO5gntm9+0V47y/MdqCu1qJIyWEiPr27PNRzZsEmdlYizACuF9BNbgNJtf\n"
    "RlRH6r4zPsieBh4L0zJPNP+JSEMs3dPXhhEqQSKh/A==\n"
    "-----END CERTIFICATE-----\n";

/* Minimal base64 PEM->DER decoder. Returns DER length or 0. */
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
  {
    const char* B64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (i = 0; i < 64; ++i) tbl[(unsigned char)B64[i]] = (int8_t)i;
  }
  for (; body < e; ++body) {
    int dv = tbl[(unsigned char)*body];
    if (dv < 0) continue;
    acc = (acc << 6) | (uint32_t)dv;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outn < outcap) out[outn++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return outn;
}

/* A fake TSA: appends a dummy ContentInfo SEQUENCE so has_timestamp triggers. */
static int fake_tsa(const uint8_t* sig, size_t sig_len, nc_buf* token,
                    void* user) {
  (void)sig;
  (void)sig_len;
  (void)user;
  /* minimal SEQUENCE { OID, [0] { ... } } enough to be a non-empty token */
  nc_der_oid(token, "1.2.840.113549.1.7.2");
  return 0;
}

int main(void) {
  static uint8_t cert_der[4096];
  size_t cert_len;
  nc_rsa_privkey key;
  const char* content = "hello cms world";
  size_t content_len = strlen(content);
  nc_buf out;
  nc_cms_verify_info info;

  (void)nc_test_to_hex;   /* header helpers unused by this test */
  (void)nc_test_from_hex;

  cert_len = pem_to_der(kCertPem, cert_der, sizeof(cert_der));
  NC_CHECK(cert_len > 0);
  NC_CHECK(nc_rsa_parse_privkey_pem(&key, kKeyPem, NULL) == 0);
  NC_CHECK(key.valid);

  /* ---- build ---- */
  nc_buf_init(&out);
  NC_CHECK(nc_cms_build_signed_data(&out, (const uint8_t*)content, content_len,
                                    cert_der, cert_len, NULL, NULL, 0, &key,
                                    "260101000000Z", NULL, NULL) == 0);
  NC_CHECK(out.len > 0);

  /* ---- verify (positive) ---- */
  NC_CHECK(nc_cms_verify_signed_data(&info, out.data, out.len,
                                     (const uint8_t*)content, content_len) == 0);
  NC_CHECK(info.parsed);
  NC_CHECK(info.signature_valid);
  NC_CHECK(info.digest_valid);
  NC_CHECK(strcmp(info.signer_cn, "nanopdf cms signer") == 0);
  NC_CHECK(strcmp(info.digest_algorithm, "SHA-256") == 0);
  NC_CHECK(strcmp(info.signing_time, "260101000000Z") == 0);
  NC_CHECK(!info.has_timestamp);

  /* ---- verify over DIFFERENT content (digest must fail) ---- */
  {
    const char* other = "different content";
    nc_cms_verify_info bad;
    NC_CHECK(nc_cms_verify_signed_data(&bad, out.data, out.len,
                                       (const uint8_t*)other,
                                       strlen(other)) == 0);
    NC_CHECK(bad.parsed);
    NC_CHECK(bad.signature_valid); /* signature itself is still valid */
    NC_CHECK(!bad.digest_valid);   /* but the content digest does not match */
  }

  /* ---- timestamp round-trip ---- */
  {
    nc_buf out_ts;
    nc_cms_verify_info ts;
    nc_buf_init(&out_ts);
    NC_CHECK(nc_cms_build_signed_data(&out_ts, (const uint8_t*)content,
                                      content_len, cert_der, cert_len, NULL,
                                      NULL, 0, &key, "260101000000Z", fake_tsa,
                                      NULL) == 0);
    NC_CHECK(nc_cms_verify_signed_data(&ts, out_ts.data, out_ts.len,
                                       (const uint8_t*)content,
                                       content_len) == 0);
    NC_CHECK(ts.has_timestamp);
    NC_CHECK(ts.signature_valid);
    NC_CHECK(ts.digest_valid);
    nc_buf_free(&out_ts);
  }

  nc_buf_free(&out);
  return nc_test_report();
}
