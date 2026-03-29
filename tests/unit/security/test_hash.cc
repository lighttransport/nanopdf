/**
 * Cryptographic hash function unit tests
 *
 * Tests MD5 and SHA-256 hash functions used in PDF security and
 * encryption. Includes NIST test vectors and RFC test vectors.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "crypto.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;
using namespace nanopdf::crypto;

TEST_SUITE("MD5") {
    TEST_CASE("RFC 1321: 'The quick brown fox...'") {
        const char* test_str = "The quick brown fox jumps over the lazy dog";
        uint8_t expected[16] = {
            0x9e, 0x10, 0x7d, 0x9d, 0x37, 0x2b, 0xb6, 0x82,
            0x6b, 0xd8, 0x1d, 0x35, 0x42, 0xa4, 0x19, 0xd6
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>(test_str),
                 strlen(test_str), hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 of empty string") {
        // MD5("") = d41d8cd98f00b204e9800998ecf8427e
        uint8_t expected[16] = {
            0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
            0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>(""), 0, hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 of single character 'a'") {
        // MD5("a") = 0cc175b9c0f1b6a831c399e269772661
        uint8_t expected[16] = {
            0x0c, 0xc1, 0x75, 0xb9, 0xc0, 0xf1, 0xb6, 0xa8,
            0x31, 0xc3, 0x99, 0xe2, 0x69, 0x77, 0x26, 0x61
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>("a"), 1, hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 of 'abc'") {
        // MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
        uint8_t expected[16] = {
            0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
            0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>("abc"), 3, hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 of 'message digest'") {
        // MD5("message digest") = f96b697d7cb7938d525a2f31aaf161d0
        const char* input = "message digest";
        uint8_t expected[16] = {
            0xf9, 0x6b, 0x69, 0x7d, 0x7c, 0xb7, 0x93, 0x8d,
            0x52, 0x5a, 0x2f, 0x31, 0xaa, 0xf1, 0x61, 0xd0
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>(input),
                 strlen(input), hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 of alphabet") {
        // MD5("abcdefghijklmnopqrstuvwxyz") = c3fcd3d76192e4007dfb496cca67e13b
        const char* input = "abcdefghijklmnopqrstuvwxyz";
        uint8_t expected[16] = {
            0xc3, 0xfc, 0xd3, 0xd7, 0x61, 0x92, 0xe4, 0x00,
            0x7d, 0xfb, 0x49, 0x6c, 0xca, 0x67, 0xe1, 0x3b
        };

        uint8_t hash[16];
        MD5::hash(reinterpret_cast<const uint8_t*>(input),
                 strlen(input), hash);

        CHECK(memcmp(hash, expected, 16) == 0);
    }

    TEST_CASE("MD5 produces different hashes for different inputs") {
        uint8_t hash1[16], hash2[16];

        MD5::hash(reinterpret_cast<const uint8_t*>("test1"), 5, hash1);
        MD5::hash(reinterpret_cast<const uint8_t*>("test2"), 5, hash2);

        CHECK(memcmp(hash1, hash2, 16) != 0);
    }
}

TEST_SUITE("SHA-256") {
    TEST_CASE("NIST: SHA-256('abc')") {
        const char* input = "abc";
        uint8_t expected[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };

        uint8_t hash[32];
        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash);

        CHECK(memcmp(hash, expected, 32) == 0);
    }

    TEST_CASE("NIST: SHA-256 of empty string") {
        uint8_t expected[32] = {
            0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
            0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
            0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
            0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
        };

        uint8_t hash[32];
        SHA256::hash(nullptr, 0, hash);

        CHECK(memcmp(hash, expected, 32) == 0);
    }

    TEST_CASE("NIST: SHA-256 of 448-bit message") {
        const char* input =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        uint8_t expected[32] = {
            0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
            0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
            0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
            0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
        };

        uint8_t hash[32];
        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash);

        CHECK(memcmp(hash, expected, 32) == 0);
    }

    TEST_CASE("SHA-256 of 'The quick brown fox...'") {
        const char* input = "The quick brown fox jumps over the lazy dog";
        // Known SHA-256 hash for this string
        uint8_t expected[32] = {
            0xd7, 0xa8, 0xfb, 0xb3, 0x07, 0xd7, 0x80, 0x94,
            0x69, 0xca, 0x9a, 0xbc, 0xb0, 0x08, 0x2e, 0x4f,
            0x8d, 0x56, 0x51, 0xe4, 0x6d, 0x3c, 0xdb, 0x76,
            0x2d, 0x02, 0xd0, 0xbf, 0x37, 0xc9, 0xe5, 0x92
        };

        uint8_t hash[32];
        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash);

        CHECK(memcmp(hash, expected, 32) == 0);
    }

    TEST_CASE("SHA-256 produces different hashes for different inputs") {
        uint8_t hash1[32], hash2[32];

        SHA256::hash(reinterpret_cast<const uint8_t*>("test1"), 5, hash1);
        SHA256::hash(reinterpret_cast<const uint8_t*>("test2"), 5, hash2);

        CHECK(memcmp(hash1, hash2, 32) != 0);
    }

    TEST_CASE("SHA-256 of single character") {
        const char* input = "a";
        // SHA-256("a") = ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb
        uint8_t expected[32] = {
            0xca, 0x97, 0x81, 0x12, 0xca, 0x1b, 0xbd, 0xca,
            0xfa, 0xc2, 0x31, 0xb3, 0x9a, 0x23, 0xdc, 0x4d,
            0xa7, 0x86, 0xef, 0xf8, 0x14, 0x7c, 0x4e, 0x72,
            0xb9, 0x80, 0x77, 0x85, 0xaf, 0xee, 0x48, 0xbb
        };

        uint8_t hash[32];
        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash);

        CHECK(memcmp(hash, expected, 32) == 0);
    }
}

TEST_SUITE("Hash Consistency") {
    TEST_CASE("Same input produces same MD5 hash") {
        const char* input = "consistent test";
        uint8_t hash1[16], hash2[16];

        MD5::hash(reinterpret_cast<const uint8_t*>(input),
                 strlen(input), hash1);
        MD5::hash(reinterpret_cast<const uint8_t*>(input),
                 strlen(input), hash2);

        CHECK(memcmp(hash1, hash2, 16) == 0);
    }

    TEST_CASE("Same input produces same SHA-256 hash") {
        const char* input = "consistent test";
        uint8_t hash1[32], hash2[32];

        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash1);
        SHA256::hash(reinterpret_cast<const uint8_t*>(input),
                    strlen(input), hash2);

        CHECK(memcmp(hash1, hash2, 32) == 0);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
