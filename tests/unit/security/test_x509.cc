// SPDX-License-Identifier: Apache-2.0
// Unit tests for X.509 parsing + signature verification (network-independent;
// uses embedded self-signed certificates).

#include "nanotest.hh"

#include "cms.hh"
#include "x509.hh"

using namespace nanopdf;

// Self-signed RSA cert, CN="nanopdf rsa test", SAN dNSName "rsa.test.example".
static const char* kRsaCertPem =
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

// Self-signed EC P-256 cert, CN="nanopdf ec test", SAN dNSName "ec.test.example".
static const char* kEcCertPem =
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

TEST_SUITE("X.509 parse + verify") {
  TEST_CASE("RSA self-signed: parse fields + self-signature verifies") {
    auto ders = cms::pem_to_certs(kRsaCertPem);
    REQUIRE(ders.size() == 1);
    x509::Certificate c = x509::parse(ders[0].data(), ders[0].size());
    REQUIRE(c.parsed);
    CHECK(c.key_type == x509::KeyType::Rsa);
    CHECK(c.rsa_pub.valid);
    CHECK(c.sig_alg == x509::SigAlg::RsaPkcs1Sha256);
    CHECK(c.is_ca);
    CHECK(c.self_issued());
    CHECK_EQ(c.subject_cn(), std::string("nanopdf rsa test"));
    REQUIRE(c.san_dns.size() == 1);
    CHECK_EQ(c.san_dns[0], std::string("rsa.test.example"));
    CHECK(c.not_before > 0);
    CHECK(c.not_after > c.not_before);

    // Self-signed: the cert verifies against itself.
    CHECK(x509::verify_signature(c, c));
    // Tamper the TBS -> signature must fail.
    x509::Certificate t = c;
    t.tbs[20] ^= 0xFF;
    CHECK(!x509::verify_signature(t, c));
  }

  TEST_CASE("EC P-256 self-signed: parse + self-signature verifies") {
    auto ders = cms::pem_to_certs(kEcCertPem);
    REQUIRE(ders.size() == 1);
    x509::Certificate c = x509::parse(ders[0].data(), ders[0].size());
    REQUIRE(c.parsed);
    CHECK(c.key_type == x509::KeyType::Ec);
    CHECK(c.ec_curve == &crypto::curve_p256());
    CHECK(c.sig_alg == x509::SigAlg::EcdsaSha256);
    CHECK_EQ(c.subject_cn(), std::string("nanopdf ec test"));
    REQUIRE(c.san_dns.size() == 1);
    CHECK_EQ(c.san_dns[0], std::string("ec.test.example"));

    CHECK(x509::verify_signature(c, c));
    x509::Certificate t = c;
    t.tbs[18] ^= 0xFF;
    CHECK(!x509::verify_signature(t, c));
  }

  TEST_CASE("verify_chain: self-signed cert as its own trust anchor + hostname") {
    auto ders = cms::pem_to_certs(kRsaCertPem);
    x509::Certificate root = x509::parse(ders[0].data(), ders[0].size());
    x509::TrustStore store;
    store.roots.push_back(root);
    store.loaded = true;

    // now=0 skips validity; matching SAN succeeds.
    auto ok = x509::verify_chain(ders, store, "rsa.test.example", 0);
    CHECK(ok.ok);
    // Wrong hostname is rejected.
    auto bad = x509::verify_chain(ders, store, "other.example", 0);
    CHECK(!bad.ok);
    // Empty trust store -> cannot anchor.
    x509::TrustStore empty;
    auto untrusted = x509::verify_chain(ders, empty, "rsa.test.example", 0);
    CHECK(!untrusted.ok);
  }
}
