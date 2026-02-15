/**
 * DCTDecode filter unit tests
 *
 * Tests the DCTDecode filter which decodes JPEG (DCT) compressed image data.
 * DCTDecode uses the stb_image library for JPEG decompression.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("DCTDecode") {
    TEST_CASE("Empty input should fail") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_dct(nullptr, 0, params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("Invalid JPEG data should fail") {
        // Not valid JPEG data (missing JPEG header)
        uint8_t invalid_data[] = {0x00, 0x01, 0x02, 0x03};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_dct(
            invalid_data, sizeof(invalid_data), params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("Minimal valid JPEG header recognition") {
        // A minimal JPEG should start with SOI marker (0xFFD8)
        uint8_t minimal_jpeg[] = {
            0xFF, 0xD8,  // SOI (Start of Image)
            0xFF, 0xD9   // EOI (End of Image)
        };
        filters::DecodeParams params;
        DecodedStream result = filters::decode_dct(
            minimal_jpeg, sizeof(minimal_jpeg), params);

        // This minimal JPEG may fail (no image data) or succeed with 0x0 image
        // Just check that it doesn't crash
        if (!result.success) {
            CHECK_FALSE(result.error.empty());
        }
    }

    // TODO: Add tests with actual JPEG test images
    // These would require embedding small JPEG files as test data
    // For now, we test the error handling and interface
}

int main() {
    return nanotest::run_all_tests();
}
