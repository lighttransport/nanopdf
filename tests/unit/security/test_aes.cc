/**
 * AES encryption unit tests
 *
 * Tests AES-128 block cipher used in PDF encryption (standard security
 * handler revision 4+). Tests both ECB (single block) and CBC (cipher
 * block chaining) modes.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "crypto.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;
using namespace nanopdf::crypto;

TEST_SUITE("AES-128 ECB") {
    TEST_CASE("NIST test vector encryption") {
        AES128 aes;

        // NIST test vector
        uint8_t key[16] = {
            0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
            0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
        };

        uint8_t plaintext[16] = {
            0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
            0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34
        };

        uint8_t expected[16] = {
            0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
            0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32
        };

        aes.set_key(key);

        uint8_t ciphertext[16];
        aes.encrypt_block(plaintext, ciphertext);

        // Verify encryption
        CHECK(memcmp(ciphertext, expected, 16) == 0);
    }

    TEST_CASE("NIST test vector decryption") {
        AES128 aes;

        uint8_t key[16] = {
            0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
            0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
        };

        uint8_t ciphertext[16] = {
            0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
            0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32
        };

        uint8_t expected_plaintext[16] = {
            0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
            0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34
        };

        aes.set_key(key);

        uint8_t decrypted[16];
        aes.decrypt_block(ciphertext, decrypted);

        CHECK(memcmp(decrypted, expected_plaintext, 16) == 0);
    }

    TEST_CASE("Encrypt and decrypt roundtrip") {
        AES128 aes;

        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t plaintext[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };

        aes.set_key(key);

        // Encrypt
        uint8_t ciphertext[16];
        aes.encrypt_block(plaintext, ciphertext);

        // Ciphertext should be different from plaintext
        CHECK(memcmp(ciphertext, plaintext, 16) != 0);

        // Decrypt
        uint8_t decrypted[16];
        aes.decrypt_block(ciphertext, decrypted);

        // Should match original plaintext
        CHECK(memcmp(decrypted, plaintext, 16) == 0);
    }

    TEST_CASE("All zeros encryption") {
        AES128 aes;

        uint8_t key[16] = {0};
        uint8_t plaintext[16] = {0};

        aes.set_key(key);

        uint8_t ciphertext[16];
        aes.encrypt_block(plaintext, ciphertext);

        // Even all zeros should produce non-zero ciphertext
        bool has_nonzero = false;
        for (int i = 0; i < 16; i++) {
            if (ciphertext[i] != 0) {
                has_nonzero = true;
                break;
            }
        }
        CHECK(has_nonzero);
    }

    TEST_CASE("Different keys produce different outputs") {
        uint8_t key1[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        uint8_t key2[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
        uint8_t plaintext[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

        AES128 aes1, aes2;
        aes1.set_key(key1);
        aes2.set_key(key2);

        uint8_t ciphertext1[16], ciphertext2[16];
        aes1.encrypt_block(plaintext, ciphertext1);
        aes2.encrypt_block(plaintext, ciphertext2);

        // Different keys should produce different ciphertexts
        CHECK(memcmp(ciphertext1, ciphertext2, 16) != 0);
    }
}

TEST_SUITE("AES-128 CBC") {
    TEST_CASE("CBC mode encryption and decryption") {
        AES128 aes;

        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t iv[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };

        uint8_t plaintext[32] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
        };

        aes.set_key(key);

        // Encrypt
        uint8_t ciphertext[32];
        aes.encrypt_cbc(plaintext, ciphertext, 32, iv);

        // Decrypt
        uint8_t decrypted[32];
        aes.decrypt_cbc(ciphertext, decrypted, 32, iv);

        // Should match original
        CHECK(memcmp(decrypted, plaintext, 32) == 0);
    }

    TEST_CASE("CBC with single block") {
        AES128 aes;

        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t iv[16] = {0};
        uint8_t plaintext[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };

        aes.set_key(key);

        uint8_t ciphertext[16];
        aes.encrypt_cbc(plaintext, ciphertext, 16, iv);

        uint8_t decrypted[16];
        aes.decrypt_cbc(ciphertext, decrypted, 16, iv);

        CHECK(memcmp(decrypted, plaintext, 16) == 0);
    }

    TEST_CASE("CBC with multiple blocks") {
        AES128 aes;

        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t iv[16] = {
            0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
            0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00
        };

        // 64 bytes = 4 blocks
        uint8_t plaintext[64];
        for (int i = 0; i < 64; i++) {
            plaintext[i] = static_cast<uint8_t>(i);
        }

        aes.set_key(key);

        uint8_t ciphertext[64];
        aes.encrypt_cbc(plaintext, ciphertext, 64, iv);

        uint8_t decrypted[64];
        aes.decrypt_cbc(ciphertext, decrypted, 64, iv);

        CHECK(memcmp(decrypted, plaintext, 64) == 0);
    }

    TEST_CASE("Different IVs produce different ciphertexts") {
        AES128 aes;

        uint8_t key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };

        uint8_t iv1[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                           0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        uint8_t iv2[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                           0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

        uint8_t plaintext[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

        aes.set_key(key);

        uint8_t ciphertext1[16], ciphertext2[16];
        aes.encrypt_cbc(plaintext, ciphertext1, 16, iv1);
        aes.encrypt_cbc(plaintext, ciphertext2, 16, iv2);

        // Different IVs should produce different ciphertexts
        CHECK(memcmp(ciphertext1, ciphertext2, 16) != 0);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
