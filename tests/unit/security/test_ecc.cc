// SPDX-License-Identifier: Apache-2.0
// Known-answer tests for ECDSA verification over NIST P-256 / P-384.

#include "nanotest.hh"

#include "ecc.hh"

using namespace nanopdf::crypto;

static std::vector<uint8_t> uh(const char* h) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; h[i] && h[i + 1]; i += 2)
    v.push_back((uint8_t)(nib(h[i]) * 16 + nib(h[i + 1])));
  return v;
}

TEST_SUITE("ECDSA verification") {
  // OpenSSL-generated signature over SHA-256("nanopdf ecdsa verify test").
  TEST_CASE("P-256 / SHA-256 verify + tamper rejected") {
    auto pub = uh(
        "04fbf4ebfd3e1b9e4003751414eeb081550c9a420676bc64f5ee4a3f02611f0197"
        "63d5233410d99afed1a30b4689231857b861e38e1466c64e6cb7853b061ed194");
    auto hash = uh(
        "8ab884083c0fa6cdd345fca442b693fe6a76749b2003da8d9b955d47ca856189");
    auto r = uh(
        "e7c360ad8cf41293e727a18658a6faa615b95efb3de3cba6b518349e3e51d0cc");
    auto s = uh(
        "9ea213d898c4d01e271770ed48477a78fad2320dff65cd389b78f41d9bd6dba3");

    CHECK(ecdsa_verify(curve_p256(), pub.data(), pub.size(), hash.data(),
                       hash.size(), r.data(), r.size(), s.data(), s.size()));
    // Tamper the hash -> reject.
    hash[0] ^= 0xFF;
    CHECK(!ecdsa_verify(curve_p256(), pub.data(), pub.size(), hash.data(),
                        hash.size(), r.data(), r.size(), s.data(), s.size()));
    hash[0] ^= 0xFF;
    // Tamper r -> reject.
    r[5] ^= 0x01;
    CHECK(!ecdsa_verify(curve_p256(), pub.data(), pub.size(), hash.data(),
                        hash.size(), r.data(), r.size(), s.data(), s.size()));
  }

  TEST_CASE("P-384 / SHA-384 verify + tamper rejected") {
    auto pub = uh(
        "04ef2c299ddb626105c55f97690dc1c0d2d002dc430872cefed2cf16fc3b8b9950"
        "3a14d77fe60449c0d985e8f931295b32a57de9d5d854962221564141dfcaf47bf2"
        "aac7531cf40a0d92d8d31169cc7647bd2673829bc891bcdac9271484cf9906");
    auto hash = uh(
        "03261e554e45676cfcc1f3c10c0be61bef26b687ea8afda221736ef3966c5133a1"
        "78f1701a2923cd8cab92799158bffb");
    auto r = uh(
        "ec59feed9c6e0027a042d19c96e80e21e5404aebc2d777b233da23a6b0b75eecf3"
        "ca44213274667731ecf557f326cb08");
    auto s = uh(
        "acf51b385b03c0d4a5447b6212ebe62ab62afa795fd4a33489062cbfeed3c1fe01"
        "5e6bea50d111f6e218ec6bbddbceb3");

    CHECK(ecdsa_verify(curve_p384(), pub.data(), pub.size(), hash.data(),
                       hash.size(), r.data(), r.size(), s.data(), s.size()));
    s[3] ^= 0x02;
    CHECK(!ecdsa_verify(curve_p384(), pub.data(), pub.size(), hash.data(),
                        hash.size(), r.data(), r.size(), s.data(), s.size()));
  }
}
