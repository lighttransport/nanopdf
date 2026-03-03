/**
 * FlateDecode filter unit tests
 *
 * Tests the FlateDecode (zlib/deflate) decompression filter used for
 * compressed PDF streams.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("FlateDecode") {
    TEST_CASE("Decompress 'Hello, World!'") {
        // zlib-compressed "Hello, World!"
        uint8_t encoded[] = {0x78, 0x9C, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
                            0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
                            0x00, 0x1F, 0x9E, 0x04, 0x6A};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 13);

        std::string decoded(result.data.begin(), result.data.end());
        CHECK(decoded == "Hello, World!");
    }

    TEST_CASE("Decompress repeated data (high compression ratio)") {
        // zlib-compressed "AAAAAAAAAAAAAAAA" (16 As)
        uint8_t encoded[] = {0x78, 0x9C, 0x73, 0x74, 0x44, 0x05, 0x00, 0x22,
                            0x98, 0x04, 0x11};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 16);

        for (size_t i = 0; i < 16; i++) {
            CHECK(result.data[i] == 'A');
        }
    }

    TEST_CASE("Decompress single byte") {
        // zlib-compressed "X"
        uint8_t encoded[] = {0x78, 0x9C, 0x8B, 0x00, 0x00, 0x00, 0x59, 0x00,
                            0x59};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 1);
        CHECK(result.data[0] == 'X');
    }

    TEST_CASE("Decompress empty stream") {
        // zlib-compressed empty data
        uint8_t encoded[] = {0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 0);
    }

    TEST_CASE("Empty input should fail") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_flate(nullptr, 0, params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("Corrupted data should fail") {
        uint8_t encoded[] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("Invalid zlib header should fail") {
        // Invalid zlib header (first byte should be 0x78 for standard zlib)
        uint8_t encoded[] = {0x00, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        CHECK_FALSE(result.success);
    }

    TEST_CASE("Partial/truncated compressed data should fail") {
        // Truncated version of "Hello, World!" encoding
        uint8_t encoded[] = {0x78, 0x9C, 0xF3, 0x48, 0xCD};
        filters::DecodeParams params;
        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        CHECK_FALSE(result.success);
    }

    // TODO: Add test for larger data decompression
    // Need to generate proper zlib-compressed test data

    TEST_CASE("DecodeParams predictor (not supported yet)") {
        // When predictor is specified but not supported, should either:
        // 1. Ignore it and decompress anyway (if predictor=1/None)
        // 2. Fail gracefully with error message
        uint8_t encoded[] = {0x78, 0x9C, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
                            0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
                            0x00, 0x1F, 0x9E, 0x04, 0x6A};
        filters::DecodeParams params;
        params.predictor = 1;  // No predictor (should work)

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        // With predictor=1 (no prediction), should succeed
        CHECK(result.success);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
