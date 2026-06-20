// SPDX-License-Identifier: Apache-2.0
// Known-answer tests for the TLS 1.3 crypto primitives.

#include "nanotest.hh"

#include "tls-crypto.hh"

using namespace nanopdf::tlscrypto;

static Bytes hb(const char* h) {
  Bytes b;
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) b.push_back(v(h[i]) * 16 + v(h[i + 1]));
  return b;
}
static std::string hx(const Bytes& b) {
  static const char* H = "0123456789abcdef";
  std::string s;
  for (uint8_t x : b) { s += H[x >> 4]; s += H[x & 0xF]; }
  return s;
}

TEST_SUITE("TLS crypto primitives") {
  TEST_CASE("X25519 RFC 7748 scalarmult vector") {
    Bytes s = hb("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    Bytes u = hb("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    uint8_t out[32];
    x25519(out, s.data(), u.data());
    CHECK_EQ(hx(Bytes(out, out + 32)),
             std::string("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754"
                         "b4075577a28552"));
  }

  TEST_CASE("X25519 base-point keygen vector") {
    Bytes priv = hb("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    uint8_t pub[32];
    x25519_base(pub, priv.data());
    CHECK_EQ(hx(Bytes(pub, pub + 32)),
             std::string("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba"
                         "4a98eaa9b4e6a"));
  }

  TEST_CASE("AES-128-GCM seal/open (NIST test case 3)") {
    Bytes k = hb("feffe9928665731c6d6a8f9467308308");
    Bytes iv = hb("cafebabefacedbaddecaf888");
    Bytes p = hb("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318"
                 "a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391"
                 "aafd255");
    Bytes ct = aes128_gcm_seal(k.data(), iv.data(), {}, p);
    CHECK_EQ(hx(ct),
             std::string("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c1"
                         "7e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396"
                         "a0aac973d58e091473f59854d5c2af327cd64a62cf35abd2ba6fa"
                         "b4"));
    Bytes dec;
    CHECK(aes128_gcm_open(k.data(), iv.data(), {}, ct, &dec));
    CHECK(dec == p);
    ct[0] ^= 0xFF;  // tamper -> auth must fail
    Bytes bad;
    CHECK(!aes128_gcm_open(k.data(), iv.data(), {}, ct, &bad));
  }

  TEST_CASE("HKDF-SHA256 RFC 5869 test case 1") {
    Bytes ikm(22, 0x0b), salt = hb("000102030405060708090a0b0c"),
          info = hb("f0f1f2f3f4f5f6f7f8f9");
    Bytes prk = hkdf_extract(salt, ikm);
    CHECK_EQ(hx(prk),
             std::string("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec"
                         "844ad7c2b3e5"));
    Bytes okm = hkdf_expand(prk, info, 42);
    CHECK_EQ(hx(okm),
             std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db0"
                         "2d56ecc4c5bf34007208d5b887185865"));
  }
}
