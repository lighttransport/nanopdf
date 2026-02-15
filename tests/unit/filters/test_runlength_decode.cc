/**
 * RunLengthDecode filter unit tests
 *
 * Tests the RunLengthDecode filter which decompresses run-length encoded
 * data as specified in the PDF specification.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("RunLengthDecode") {
    TEST_CASE("Simple literal copy") {
        // Copy 3 bytes literally, then EOD marker (128)
        uint8_t encoded[] = {0x02, 'A', 'B', 'C', 128};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 'A');
        CHECK(result.data[1] == 'B');
        CHECK(result.data[2] == 'C');
    }

    TEST_CASE("Run-length encoding") {
        // Repeat 'X' 3 times (257-254=3), then EOD
        uint8_t encoded[] = {254, 'X', 128};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 'X');
        CHECK(result.data[1] == 'X');
        CHECK(result.data[2] == 'X');
    }

    TEST_CASE("Mixed literal and run-length") {
        // Copy 2 bytes ('A', 'B'), then repeat 'Z' 4 times (257-253=4)
        uint8_t encoded[] = {0x01, 'A', 'B', 253, 'Z', 128};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 6);
        CHECK(result.data[0] == 'A');
        CHECK(result.data[1] == 'B');
        CHECK(result.data[2] == 'Z');
        CHECK(result.data[3] == 'Z');
        CHECK(result.data[4] == 'Z');
        CHECK(result.data[5] == 'Z');
    }

    TEST_CASE("Empty input handling") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(nullptr, 0, params);

        // Implementation may succeed with empty output or fail
        // Just check it doesn't crash
        if (!result.success) {
            CHECK_FALSE(result.error.empty());
        } else {
            CHECK(result.data.empty());
        }
    }

    TEST_CASE("Maximum run-length (128 bytes)") {
        // 0 means repeat next byte 257-0=257 times? Actually 0 means 1 copy
        // 128 is the only reserved value (EOD marker)
        // Run length: 257 - length_byte
        // So 129 means 257-129=128 repeats
        uint8_t encoded[] = {129, 'M', 128};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 128);
        for (size_t i = 0; i < 128; i++) {
            CHECK(result.data[i] == 'M');
        }
    }

    TEST_CASE("Maximum literal copy (128 bytes)") {
        // 127 means copy next 128 bytes (length+1)
        std::vector<uint8_t> encoded;
        encoded.push_back(127);  // Copy next 128 bytes
        for (int i = 0; i < 128; i++) {
            encoded.push_back(static_cast<uint8_t>(i));
        }
        encoded.push_back(128);  // EOD

        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(
            encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 128);
        for (size_t i = 0; i < 128; i++) {
            CHECK(result.data[i] == static_cast<uint8_t>(i));
        }
    }

    TEST_CASE("Only EOD marker") {
        uint8_t encoded[] = {128};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 0);
    }

    TEST_CASE("Missing EOD marker should handle gracefully") {
        // Data without EOD marker - implementation-dependent behavior
        uint8_t encoded[] = {0x01, 'A', 'B'};
        filters::DecodeParams params;
        DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

        // May succeed or fail depending on implementation
        // If it succeeds, should have decoded the literal copy
        if (result.success) {
            CHECK(result.data.size() == 2);
            CHECK(result.data[0] == 'A');
            CHECK(result.data[1] == 'B');
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
