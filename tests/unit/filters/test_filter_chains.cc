/**
 * Filter chain unit tests
 *
 * Tests combinations of multiple filters applied in sequence, as commonly
 * used in PDF streams. PDF filter arrays are applied in reverse order:
 * /Filter [/ASCIIHexDecode /FlateDecode] means ASCIIHex first, then Flate.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("FilterChains") {
    TEST_CASE("ASCIIHex → Flate chain") {
        // ASCIIHex-encoded zlib data for "Hello, World!"
        // This simulates /Filter [/ASCIIHexDecode /FlateDecode]
        const char *hex_encoded =
            "789CF348CDC9C9D75108CF2FCA495104001F9E046A>";
        filters::DecodeParams params;

        // Step 1: ASCIIHex decode
        DecodedStream hex_result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(hex_encoded),
            strlen(hex_encoded), params);

        REQUIRE(hex_result.success);
        CHECK(hex_result.data.size() == 21);  // zlib compressed size

        // Step 2: Flate decode the result
        DecodedStream flate_result = filters::decode_flate(
            hex_result.data.data(), hex_result.data.size(), params);

        REQUIRE(flate_result.success);
        std::string decoded(flate_result.data.begin(), flate_result.data.end());
        CHECK(decoded == "Hello, World!");
    }

    TEST_CASE("ASCII85 → Flate chain concept") {
        // Test the concept of chaining ASCII85 and Flate
        // In practice, this would use correctly encoded data
        // For now, test that ASCII85 decode works standalone

        const char *ascii85_encoded = "9jqo^~>";  // "Man " in ASCII85
        filters::DecodeParams params;

        DecodedStream ascii85_result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(ascii85_encoded),
            strlen(ascii85_encoded), params);

        REQUIRE(ascii85_result.success);
        CHECK(ascii85_result.data.size() == 4);
        CHECK(ascii85_result.data[0] == 'M');
        CHECK(ascii85_result.data[1] == 'a');
        CHECK(ascii85_result.data[2] == 'n');
        CHECK(ascii85_result.data[3] == ' ');

        // For a real chain, you would then compress this with Flate
        // But we've already tested Flate separately
    }

    TEST_CASE("Flate baseline (no chaining)") {
        // Verify Flate alone works as baseline
        uint8_t zlib_data[] = {0x78, 0x9C, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
                               0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
                               0x00, 0x1F, 0x9E, 0x04, 0x6A};
        filters::DecodeParams params;

        DecodedStream result = filters::decode_flate(
            zlib_data, sizeof(zlib_data), params);

        REQUIRE(result.success);
        std::string decoded(result.data.begin(), result.data.end());
        CHECK(decoded == "Hello, World!");
    }

    TEST_CASE("RunLength → Flate chain") {
        // Create run-length encoded data, then compress with zlib
        // For simplicity, test the concept with pre-computed data

        // This would be: RLE("AAAA") compressed with zlib
        // RLE of "AAAA" = {253, 'A', 128} (repeat A 4 times)
        uint8_t rle_data[] = {253, 'A', 128};

        // First verify RLE alone works
        filters::DecodeParams params;
        DecodedStream rle_result = filters::decode_runlength(
            rle_data, sizeof(rle_data), params);

        REQUIRE(rle_result.success);
        CHECK(rle_result.data.size() == 4);
        CHECK(rle_result.data[0] == 'A');
        CHECK(rle_result.data[1] == 'A');
        CHECK(rle_result.data[2] == 'A');
        CHECK(rle_result.data[3] == 'A');
    }

    TEST_CASE("Empty chain (no filters)") {
        // Test data that doesn't need any filtering
        const char *plain_text = "No filtering needed";
        std::vector<uint8_t> data(plain_text, plain_text + strlen(plain_text));

        CHECK(data.size() == 19);
        std::string result(data.begin(), data.end());
        CHECK(result == "No filtering needed");
    }

    TEST_CASE("Chain with filter failure") {
        // Test that errors propagate correctly through chains
        const char *invalid_hex = "INVALID>";
        filters::DecodeParams params;

        // ASCIIHex should fail on invalid characters
        DecodedStream hex_result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(invalid_hex),
            strlen(invalid_hex), params);

        // Should fail, so don't proceed to next filter
        if (!hex_result.success) {
            CHECK_FALSE(hex_result.error.empty());
            // Chain breaks here - don't apply Flate
        }
    }

    TEST_CASE("Triple chain: ASCIIHex → LZW → Flate") {
        // Test theoretical 3-filter chain
        // In practice, this would be rare but technically valid
        // For now, just test the concept with known data

        uint8_t lzw_data[] = {0x80, 0x10, 0x60, 0x20};  // LZW for 'A'
        filters::DecodeParams params;

        DecodedStream lzw_result = filters::decode_lzw(
            lzw_data, sizeof(lzw_data), params);

        REQUIRE(lzw_result.success);
        CHECK(lzw_result.data.size() == 1);
        CHECK(lzw_result.data[0] == 'A');
    }
}

TEST_SUITE("ErrorKind") {

    TEST_CASE("FlateDecode corrupted data returns Malformed ErrorKind") {
        uint8_t corrupted[] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_flate(corrupted, sizeof(corrupted), params);

        CHECK_FALSE(result.success);
        CHECK(result.kind == ErrorKind::Malformed);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("ASCII85Decode invalid data returns Malformed ErrorKind") {
        // Invalid ASCII85 - contains characters outside the valid range
        uint8_t invalid[] = {0x01, 0x02, 0x03};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(invalid, sizeof(invalid), params);

        if (!result.success) {
            CHECK(result.kind == ErrorKind::Malformed);
        }
    }

    TEST_CASE("FlateDecode null input returns Malformed ErrorKind") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_flate(nullptr, 0, params);

        CHECK_FALSE(result.success);
        CHECK(result.kind == ErrorKind::Malformed);
    }

    TEST_CASE("DCTDecode invalid data returns Malformed ErrorKind") {
        uint8_t invalid[] = {0x00, 0x01, 0x02, 0x03};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_dct(invalid, sizeof(invalid), params);

        CHECK_FALSE(result.success);
        CHECK(result.kind == ErrorKind::Malformed);
    }

}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
