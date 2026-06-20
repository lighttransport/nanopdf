// SPDX-License-Identifier: Apache-2.0
// Unit tests for the pure-C++ public-key primitives (crypto-pk).

#include "nanotest.hh"

#include "crypto.hh"
#include "crypto-pk.hh"

using namespace nanopdf::crypto;

static std::string hex(const std::vector<uint8_t>& v) {
  static const char* H = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) { s += H[b >> 4]; s += H[b & 0xF]; }
  return s;
}

// Embedded 1024-bit RSA test key (PKCS#8 PEM, unencrypted; test-only).
static const char* kTestKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIICdgIBADANBgkqhkiG9w0BAQEFAASCAmAwggJcAgEAAoGBAJ9zoJtx3xa0MoY/\n"
    "PHLP60zbe8Zh+5hhGmAvIhm3AdGx+wvVr6eqA7dPAPDw9Jajii+Polpsn/nTVhT+\n"
    "B5cWYWkoFxlBDdOJNZ+uWEhQ5Hns6SOw0c4Q3PCY9wOEkgTF++XPwrv5R7PXuulr\n"
    "FGDir6/re+gnoZMsTsiNfatD08bDAgMBAAECgYAl0Y7uT3vSLrstDCKSOK5edFBP\n"
    "JT4/Tgird4JnBvjve39Ht08KPVDUUXCvtPbOI8vKrA6d09W9s1pfcEDsnOpNXsU2\n"
    "QCr4IsZeWGkh7logpLcLGaxyTB0U6RzYgXkOgPhSBiP5MgNwfvEO9dN6xGfU4Hm5\n"
    "lHpSjKNZeehwtKjcAQJBANGaUEWZ0y3mtJzgqxEWh3ikGS2RND0LyUDLlhss/v/l\n"
    "ihrC474xc4B0KjkE+vyRWhVUuU/VAoVB0IHJEwu9bgMCQQDCv17UsnLB1puCIFqW\n"
    "9Z80Pe3o+YUAOop0NFwt8ocfszqpcBxhnWQB354C+8G76gHnTeKWHu2OlC4+ARnp\n"
    "7khBAkBiDgHNfbfYelw2I7iDhvmbS5Fnys0YXeIpiNRRJEyq4/QmrtOiDzpMdoum\n"
    "HxlXfJwa7IbAvRKvymlDvhBF5rmHAkEAsS6/rrB0bBs+/NNG2FW8dSFrFS3/bcfV\n"
    "NAh3XW5stdCxLHtNtxolZPb4oio/hyJOfQ2Fe6PD6payw8LouscKgQJAQg92pwa8\n"
    "jCa5Co9j/i38Pkw7bsFaXSajA9TlWlmvsjwMZl32ShqxuPxHmTqfXVWDdyBYHr0Z\n"
    "PGthNcFmbmZAYQ==\n"
    "-----END PRIVATE KEY-----\n";

TEST_SUITE("BigInt arithmetic") {
  TEST_CASE("modexp small known values") {
    // 4^13 mod 497 = 445 (a classic RSA textbook example).
    BigInt r = BigInt::modexp(BigInt(4), BigInt(13), BigInt(497));
    auto b = r.to_bytes();
    CHECK_EQ((int)b.size(), 2);
    CHECK_EQ((int)((b[0] << 8) | b[1]), 445);
  }

  TEST_CASE("mul and divmod round-trip") {
    BigInt a = BigInt::from_bytes((const uint8_t*)"\xDE\xAD\xBE\xEF", 4);
    BigInt b = BigInt::from_bytes((const uint8_t*)"\x12\x34\x56", 3);
    BigInt prod = BigInt::mul(a, b);
    BigInt q, rem;
    BigInt::divmod(prod, b, &q, &rem);
    CHECK(rem.is_zero());
    CHECK_EQ(BigInt::cmp(q, a), 0);
  }

  TEST_CASE("byte round-trip with fixed length") {
    const uint8_t in[5] = {0x01, 0x00, 0x00, 0x00, 0x07};
    BigInt v = BigInt::from_bytes(in, 5);
    auto out = v.to_bytes(5);
    CHECK_EQ((int)out.size(), 5);
    for (int i = 0; i < 5; ++i) CHECK_EQ((int)out[i], (int)in[i]);
  }
}

TEST_SUITE("HMAC + PBKDF2") {
  TEST_CASE("HMAC-SHA256 (RFC 4231 case 1)") {
    std::vector<uint8_t> key(20, 0x0b);
    const char* msg = "Hi There";
    std::vector<uint8_t> h = hmac(Prf::Sha256, key.data(), key.size(),
                                  (const uint8_t*)msg, 8);
    CHECK_EQ(hex(h),
             std::string("b0344c61d8db38535ca8afceaf0bf12b"
                         "881dc200c9833da726e9376c2e32cff7"));
  }

  TEST_CASE("PBKDF2-HMAC-SHA1 (RFC 6070 c=1)") {
    std::vector<uint8_t> dk =
        pbkdf2(Prf::Sha1, (const uint8_t*)"password", 8, (const uint8_t*)"salt",
               4, 1, 20);
    CHECK_EQ(hex(dk), std::string("0c60c80f961f0e71f3a9b524af6012062fe037a6"));
  }

  TEST_CASE("PBKDF2-HMAC-SHA1 (RFC 6070 c=4096)") {
    std::vector<uint8_t> dk =
        pbkdf2(Prf::Sha1, (const uint8_t*)"password", 8, (const uint8_t*)"salt",
               4, 4096, 20);
    CHECK_EQ(hex(dk), std::string("4b007901b765489abead49d926f721d065a429c1"));
  }
}

TEST_SUITE("RSA PKCS#1 v1.5") {
  TEST_CASE("parse PKCS#8 PEM key") {
    RsaPrivateKey k = rsa_parse_private_key_pem(kTestKeyPem);
    CHECK(k.valid);
    CHECK_EQ((int)k.modulus_bytes, 128);  // 1024-bit
  }

  TEST_CASE("sign then verify round-trips; tamper is rejected") {
    RsaPrivateKey priv = rsa_parse_private_key_pem(kTestKeyPem);
    REQUIRE(priv.valid);

    RsaPublicKey pub;
    pub.n = priv.n;
    pub.e = priv.e;
    pub.modulus_bytes = priv.modulus_bytes;
    pub.valid = true;

    uint8_t hash[32];
    for (int i = 0; i < 32; ++i) hash[i] = (uint8_t)(i * 7 + 1);

    std::vector<uint8_t> sig = rsa_sign_sha256(priv, hash);
    CHECK_EQ((int)sig.size(), 128);

    // Rebuild the SHA-256 DigestInfo for verification.
    static const uint8_t prefix[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                     0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                     0x01, 0x05, 0x00, 0x04, 0x20};
    std::vector<uint8_t> di(prefix, prefix + sizeof(prefix));
    di.insert(di.end(), hash, hash + 32);

    CHECK(rsa_verify_pkcs1v15(pub, sig.data(), sig.size(), di.data(), di.size()));

    // Tamper the digest: verification must fail.
    di.back() ^= 0xFF;
    CHECK(!rsa_verify_pkcs1v15(pub, sig.data(), sig.size(), di.data(),
                               di.size()));
  }

  // RSASSA-PSS (rsa_pss_rsae_sha256, salt=32) signature produced by OpenSSL over
  // SHA-256("nanopdf pss verify test") with a 2048-bit key. Verifies the PSS
  // path used by TLS 1.3 CertificateVerify and PSS X.509 signatures.
  TEST_CASE("RSA-PSS verify (OpenSSL KAT) + tamper rejected") {
    auto unhex = [](const char* h) {
      std::vector<uint8_t> v;
      auto nib = [](char c) {
        return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
      };
      for (size_t i = 0; h[i] && h[i + 1]; i += 2)
        v.push_back((uint8_t)(nib(h[i]) * 16 + nib(h[i + 1])));
      return v;
    };
    std::vector<uint8_t> n = unhex(
        "E3C699F557C5ACC4D3B24D5E053C3E6916E9A83EA0ACB667955583F85C92B12C"
        "442F6BD7E4BE9E3320E7E39253FA00EB81FED4D7DBD168D6CE4819F4F9784A94"
        "A6D7520B071432F652D5BAE2C59E7E8EA1D846B5525107BDD57B536723F9E62D"
        "0B2EE9DB8FBC30BD4C1FDF6C21021E9485864BED0838036E57EFB27F1DF246B2"
        "889838383F6A3166DDC51C6F594D70E876EFC32EB79ED9E1FD2D69D88A33332B"
        "19988CA82D7AFB2EB2629544CA359F3BDBA9E9F6F73CC47656274C8F039ECC87"
        "4A2BC8780D54776CFD8EC3E787A8BA9950493CF92C02E6DAE91278F5CF1DEABC"
        "857EF8669E748CB18F99767ECF08F0C674B41F1DD5DC3AE3A0EEDD5A35F0B213");
    std::vector<uint8_t> sig = unhex(
        "c6ee7c434bdebcbba7e34a2fe0fd3d1d5956906aa25f2a035ad4cbf27cbd4427"
        "a090a2e4fbb26ab090e634741c6c742d4be1217c5cf7cca4f9dbd11bfc3f7f6d"
        "784ef7e52784f2d2bea59935acf214cab658d7e1c1ae6c9ac6034f1e4685c908"
        "2e7577fdbd8fea3df8a198fc8fa59faaf138eec23ca5679efb8735ffb7ffe221"
        "3afd1db6171d726ffc0460db6aec1646d6921d5269bafe2f1f71976e17ad6300"
        "c8b81f82dad10855c4e9de451f8eab08b21b61688be76b19e8842597d3a622b4"
        "2a5ca82cd8adca0e4089b6a6c997fa3ee65dbd4ccc61daa42e5796c6e349e839"
        "13b4adfb585254c5386d2090a61a693acba9896de4180b309f2233a0f51d1fe8");
    std::vector<uint8_t> mhash = unhex(
        "5aa1cf3b0a7f11d6b5c7307232f4d25a7fc67ac27cfb5946f48c89283031d3d1");

    RsaPublicKey pub;
    pub.n = BigInt::from_bytes(n.data(), n.size());
    pub.e = BigInt(0x10001);
    pub.modulus_bytes = 256;
    pub.valid = true;

    CHECK(rsa_verify_pss(pub, sig.data(), sig.size(), mhash.data(),
                         mhash.size()));
    // Tamper the message hash -> reject.
    mhash[0] ^= 0xFF;
    CHECK(!rsa_verify_pss(pub, sig.data(), sig.size(), mhash.data(),
                          mhash.size()));
    mhash[0] ^= 0xFF;
    // Tamper the signature -> reject.
    sig[10] ^= 0x01;
    CHECK(!rsa_verify_pss(pub, sig.data(), sig.size(), mhash.data(),
                          mhash.size()));
  }
}
