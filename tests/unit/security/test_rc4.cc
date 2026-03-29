/**
 * RC4 encryption unit tests
 *
 * Tests the RC4 stream cipher used in PDF encryption (standard security
 * handler, revision 2 and 3). RC4 is a symmetric stream cipher where the
 * same operation is used for both encryption and decryption.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "crypto.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;
using namespace nanopdf::crypto;

TEST_SUITE("RC4") {
    TEST_CASE("RFC 6229 test vector") {
        // Test vector from RFC 6229
        RC4 rc4;
        uint8_t key[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t plaintext[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t expected[] = {0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27};

        rc4.init(key, sizeof(key));
        rc4.crypt(plaintext, sizeof(plaintext));

        CHECK(memcmp(plaintext, expected, sizeof(expected)) == 0);
    }

    TEST_CASE("RC4 encryption and decryption are symmetric") {
        // RC4 is its own inverse
        RC4 rc4_encrypt;
        uint8_t key[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        rc4_encrypt.init(key, sizeof(key));
        rc4_encrypt.crypt(data, sizeof(data));

        // Now decrypt (apply RC4 again with same key)
        RC4 rc4_decrypt;
        rc4_decrypt.init(key, sizeof(key));
        rc4_decrypt.crypt(data, sizeof(data));

        // Should decrypt back to zeros
        for (size_t i = 0; i < sizeof(data); i++) {
            CHECK(data[i] == 0x00);
        }
    }

    TEST_CASE("Known test vector: RC4('Key', 'Plaintext')") {
        // RC4("Key", "Plaintext") = BBF316E8D940AF0AD3
        RC4 rc4;
        uint8_t key[] = {'K', 'e', 'y'};
        uint8_t data[] = {'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
        uint8_t expected[] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};

        rc4.init(key, sizeof(key));
        rc4.crypt(data, sizeof(data));

        CHECK(memcmp(data, expected, sizeof(expected)) == 0);
    }

    TEST_CASE("Empty data encryption") {
        // Encrypting zero bytes should not crash
        RC4 rc4;
        uint8_t key[] = {0x01, 0x02, 0x03};
        uint8_t data[1];  // Single byte array

        rc4.init(key, sizeof(key));
        rc4.crypt(data, 0);  // Encrypt 0 bytes

        // Should complete without error
        CHECK(true);
    }

    TEST_CASE("Single byte encryption") {
        RC4 rc4;
        uint8_t key[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t data[] = {0x42};
        uint8_t original = data[0];

        rc4.init(key, sizeof(key));
        rc4.crypt(data, sizeof(data));

        // Should be encrypted (different from original)
        CHECK(data[0] != original);

        // Decrypt
        RC4 rc4_decrypt;
        rc4_decrypt.init(key, sizeof(key));
        rc4_decrypt.crypt(data, sizeof(data));

        // Should be back to original
        CHECK(data[0] == original);
    }

    TEST_CASE("Large data encryption") {
        // Test with larger data
        RC4 rc4;
        uint8_t key[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t data[256];
        for (size_t i = 0; i < sizeof(data); i++) {
            data[i] = static_cast<uint8_t>(i);
        }
        uint8_t original[256];
        memcpy(original, data, sizeof(data));

        rc4.init(key, sizeof(key));
        rc4.crypt(data, sizeof(data));

        // Should be encrypted
        bool different = false;
        for (size_t i = 0; i < sizeof(data); i++) {
            if (data[i] != original[i]) {
                different = true;
                break;
            }
        }
        CHECK(different);

        // Decrypt
        RC4 rc4_decrypt;
        rc4_decrypt.init(key, sizeof(key));
        rc4_decrypt.crypt(data, sizeof(data));

        // Should be back to original
        CHECK(memcmp(data, original, sizeof(data)) == 0);
    }

    TEST_CASE("Different keys produce different outputs") {
        uint8_t key1[] = {0x01, 0x02, 0x03};
        uint8_t key2[] = {0x04, 0x05, 0x06};
        uint8_t data1[] = {0x00, 0x00, 0x00, 0x00};
        uint8_t data2[] = {0x00, 0x00, 0x00, 0x00};

        RC4 rc4_1;
        rc4_1.init(key1, sizeof(key1));
        rc4_1.crypt(data1, sizeof(data1));

        RC4 rc4_2;
        rc4_2.init(key2, sizeof(key2));
        rc4_2.crypt(data2, sizeof(data2));

        // Different keys should produce different outputs
        CHECK(memcmp(data1, data2, sizeof(data1)) != 0);
    }

    TEST_CASE("Key length variations") {
        // RC4 supports variable key lengths (typically 40-256 bits)
        uint8_t short_key[] = {0x01, 0x02, 0x03, 0x04, 0x05};  // 40 bits
        uint8_t long_key[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};  // 128 bits
        uint8_t data1[] = {0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t data2[] = {0xFF, 0xFF, 0xFF, 0xFF};

        RC4 rc4_short;
        rc4_short.init(short_key, sizeof(short_key));
        rc4_short.crypt(data1, sizeof(data1));

        RC4 rc4_long;
        rc4_long.init(long_key, sizeof(long_key));
        rc4_long.crypt(data2, sizeof(data2));

        // Different key lengths should produce different outputs
        CHECK(memcmp(data1, data2, sizeof(data1)) != 0);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
