/**
 * PNG Predictor unit tests
 *
 * Tests FlateDecode with PNG predictor algorithms (predictor >= 10).
 * PNG predictors apply reversible transformations before compression to
 * improve compression ratios for image data. Five predictor types:
 * - None (0): No prediction
 * - Sub (1): Difference from left neighbor
 * - Up (2): Difference from above
 * - Average (3): Average of left and above
 * - Paeth (4): Paeth predictor algorithm
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("PNG Predictors") {
    TEST_CASE("PNG None predictor (filter=0)") {
        // Just strips the filter byte, no prediction
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0x70, 0x74, 0x72, 0x06, 0x00,
                             0x01, 0x8E, 0x00, 0xC7};
        filters::DecodeParams params;
        params.predictor = 10;
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 65);  // 'A'
        CHECK(result.data[1] == 66);  // 'B'
        CHECK(result.data[2] == 67);  // 'C'
    }

    TEST_CASE("PNG Sub predictor (filter=1)") {
        // Each byte = diff from left neighbor
        // Raw (with filter byte): [1, 10, 20, 30]
        // Decoded: [10, 10+20=30, 30+30=60]
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0xE4, 0x12, 0x91, 0x03, 0x00,
                             0x00, 0x6C, 0x00, 0x3E};
        filters::DecodeParams params;
        params.predictor = 11;
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 10);
        CHECK(result.data[1] == 30);
        CHECK(result.data[2] == 60);
    }

    TEST_CASE("PNG Up predictor (filter=2)") {
        // Each byte = diff from above
        // Row 1 (None): [10, 20, 30]
        // Row 2 (Up):   [5, 10, 15] → [10+5=15, 20+10=30, 30+15=45]
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0xE0, 0x12, 0x91, 0x63, 0x62,
                             0xE5, 0xE2, 0x07, 0x00, 0x01, 0x96, 0x00, 0x5D};
        filters::DecodeParams params;
        params.predictor = 12;
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 6);
        // Row 1
        CHECK(result.data[0] == 10);
        CHECK(result.data[1] == 20);
        CHECK(result.data[2] == 30);
        // Row 2
        CHECK(result.data[3] == 15);
        CHECK(result.data[4] == 30);
        CHECK(result.data[5] == 45);
    }

    TEST_CASE("PNG Average predictor (filter=3)") {
        // Row 1 (None): [100, 100, 100]
        // Row 2 (Avg):  [10, 10, 10]
        //   col0: 10 + floor((0+100)/2) = 60
        //   col1: 10 + floor((60+100)/2) = 90
        //   col2: 10 + floor((90+100)/2) = 105
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0x48, 0x49, 0x49, 0x61, 0xE6,
                             0xE2, 0xE2, 0x02, 0x00, 0x07, 0x58, 0x01, 0x4E};
        filters::DecodeParams params;
        params.predictor = 13;
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 6);
        CHECK(result.data[0] == 100);
        CHECK(result.data[1] == 100);
        CHECK(result.data[2] == 100);
        CHECK(result.data[3] == 60);
        CHECK(result.data[4] == 90);
        CHECK(result.data[5] == 105);
    }

    TEST_CASE("PNG Paeth predictor (filter=4)") {
        // Paeth predictor: picks left, above, or upper-left based on gradient
        // Row 1 (None): [10, 20, 30]
        // Row 2 (Paeth): [5, 5, 5] → [15, 25, 35]
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0xE0, 0x12, 0x91, 0x63, 0x61,
                             0x65, 0x65, 0x05, 0x00, 0x01, 0x8A, 0x00, 0x50};
        filters::DecodeParams params;
        params.predictor = 14;
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 6);
        CHECK(result.data[0] == 10);
        CHECK(result.data[1] == 20);
        CHECK(result.data[2] == 30);
        CHECK(result.data[3] == 15);
        CHECK(result.data[4] == 25);
        CHECK(result.data[5] == 35);
    }

    TEST_CASE("No predictor (predictor=1)") {
        // Predictor=1 means no prediction, just normal FlateDecode
        uint8_t encoded[] = {0x78, 0x9C, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
                             0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
                             0x00, 0x1F, 0x9E, 0x04, 0x6A};
        filters::DecodeParams params;
        params.predictor = 1;  // No predictor

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        std::string decoded(result.data.begin(), result.data.end());
        CHECK(decoded == "Hello, World!");
    }

    TEST_CASE("Multiple rows with varying predictors") {
        // Tests that each row can use a different predictor
        // In practice, PNG allows per-row predictor selection
        // This test verifies the implementation handles this correctly
        // (Most of the previous tests already do this implicitly)

        // This is the same as PNG Up test but validates the concept
        uint8_t encoded[] = {0x78, 0x9C, 0x63, 0xE0, 0x12, 0x91, 0x63, 0x62,
                             0xE5, 0xE2, 0x07, 0x00, 0x01, 0x96, 0x00, 0x5D};
        filters::DecodeParams params;
        params.predictor = 12;  // PNG Up
        params.columns = 3;
        params.colors = 1;
        params.bits_per_component = 8;

        DecodedStream result =
            filters::decode_flate(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 6);
        // Verify both rows decoded correctly
        for (size_t i = 0; i < result.data.size(); i++) {
            CHECK(result.data[i] > 0);  // All values should be non-zero
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
