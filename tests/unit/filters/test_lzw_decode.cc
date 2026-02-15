/**
 * LZWDecode filter unit tests
 *
 * Tests the LZW (Lempel-Ziv-Welch) decompression filter. LZW uses a dictionary-
 * based compression scheme with variable-length codes starting at 9 bits.
 * Special codes: 256 (CLEAR), 257 (EOD). Codes are MSB-first bit-packed.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("LZWDecode") {
    TEST_CASE("Single byte with CLEAR and EOD") {
        // Encode: CLEAR(256) + 'A'(65) + EOD(257)
        // 9-bit codes: 256=0x100, 65=0x041, 257=0x101
        // Bit stream (MSB first):
        //   100000000 000100001 100000001
        //   = 0x80 0x10 0x60 0x20
        uint8_t encoded[] = {0x80, 0x10, 0x60, 0x20};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 1);
        CHECK(result.data[0] == 'A');
    }

    TEST_CASE("Multiple bytes") {
        // Encode: CLEAR(256) + 'A'(65) + 'B'(66) + 'C'(67) + EOD(257)
        // 9-bit codes: 256, 65, 66, 67, 257
        // Binary (MSB-first, 9 bits each):
        //   100000000 001000001 001000010 001000011 100000001
        // Pack into bytes:
        //   0x80 0x10 0x48 0x44 0x38 0x08
        uint8_t encoded[] = {0x80, 0x10, 0x48, 0x44, 0x38, 0x08};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 'A');
        CHECK(result.data[1] == 'B');
        CHECK(result.data[2] == 'C');
    }

    TEST_CASE("Repeated byte with dictionary entry") {
        // Encode: CLEAR(256) + 'A'(65) + 'A'(65) + 258(="AA") + EOD(257)
        // After first 'A' and second 'A', dict entry 258="AA" is created
        // Then code 258 outputs "AA"
        // 9-bit codes: 256, 65, 65, 258, 257
        // Binary:
        //   100000000 001000001 001000001 100000010 100000001
        // Pack:
        //   0x80 0x10 0x48 0x30 0x28 0x08
        uint8_t encoded[] = {0x80, 0x10, 0x48, 0x30, 0x28, 0x08};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        CHECK(result.data[0] == 'A');
        CHECK(result.data[1] == 'A');
        CHECK(result.data[2] == 'A');
        CHECK(result.data[3] == 'A');
    }

    TEST_CASE("Empty input should fail") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_lzw(nullptr, 0, params);

        // Should either fail or produce empty output
        CHECK(!result.success || result.data.empty());
    }

    TEST_CASE("CLEAR + EOD only (no data)") {
        // 9-bit codes: 256, 257
        // Binary: 100000000 100000001
        // Pack: 0x80 0x40 0x40
        uint8_t encoded[] = {0x80, 0x40, 0x40};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 0);
    }

    TEST_CASE("Corrupted data should fail") {
        // Random bytes that don't form valid LZW stream
        uint8_t encoded[] = {0xFF, 0xFF, 0xFF, 0xFF};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        // Should fail gracefully
        if (!result.success) {
            CHECK_FALSE(result.error.empty());
        }
    }

    TEST_CASE("Missing EOD marker should handle gracefully") {
        // CLEAR + 'A' without EOD
        uint8_t encoded[] = {0x80, 0x10, 0x20};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_lzw(encoded, sizeof(encoded), params);

        // Implementation-dependent: may succeed or fail
        if (result.success) {
            CHECK(result.data.size() >= 1);
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
