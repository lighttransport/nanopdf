/**
 * ASCIIHexDecode filter unit tests
 *
 * Tests the ASCIIHexDecode filter which decodes hexadecimal-encoded ASCII data.
 * The encoded data consists of hexadecimal digit pairs (0-9, A-F) and is
 * terminated by a '>' character. Whitespace is ignored.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("ASCIIHexDecode") {
    TEST_CASE("Simple hex pairs") {
        const char *encoded = "48656C6C6F>";  // "Hello"
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 5);
        CHECK(result.data[0] == 'H');
        CHECK(result.data[1] == 'e');
        CHECK(result.data[2] == 'l');
        CHECK(result.data[3] == 'l');
        CHECK(result.data[4] == 'o');
    }

    TEST_CASE("Lowercase hex digits") {
        const char *encoded = "4d616e>";  // "Man"
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
        CHECK(result.data[2] == 'n');
    }

    TEST_CASE("Mixed case hex digits") {
        const char *encoded = "4d61 6E>";  // "Man" with space
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 3);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
        CHECK(result.data[2] == 'n');
    }

    TEST_CASE("Whitespace is ignored") {
        const char *encoded = "48 65\t6C\r\n6C 6F>";  // "Hello" with spaces, tabs, CRLF
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 5);
        CHECK(result.data[0] == 'H');
        CHECK(result.data[1] == 'e');
        CHECK(result.data[2] == 'l');
        CHECK(result.data[3] == 'l');
        CHECK(result.data[4] == 'o');
    }

    TEST_CASE("Odd number of hex digits - trailing nibble padded with 0") {
        const char *encoded = "ABC>";  // Should decode to AB C0
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 2);
        CHECK(result.data[0] == 0xAB);
        CHECK(result.data[1] == 0xC0);
    }

    TEST_CASE("Empty input (just end marker)") {
        const char *encoded = ">";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 0);
    }

    TEST_CASE("Invalid hex character should fail") {
        const char *encoded = "48GG>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("All zero bytes") {
        const char *encoded = "00000000>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        CHECK(result.data[0] == 0);
        CHECK(result.data[1] == 0);
        CHECK(result.data[2] == 0);
        CHECK(result.data[3] == 0);
    }

    TEST_CASE("All FF bytes") {
        const char *encoded = "FFFF>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 2);
        CHECK(result.data[0] == 0xFF);
        CHECK(result.data[1] == 0xFF);
    }

    TEST_CASE("Missing end marker should fail or handle gracefully") {
        const char *encoded = "48656C6C6F";  // "Hello" without '>'
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        // Implementation may accept or reject this
        // If accepted, should decode correctly
        if (result.success) {
            CHECK(result.data.size() == 5);
        }
    }

    TEST_CASE("Null input handling") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_asciihex(nullptr, 0, params);

        // Implementation may succeed with empty output or fail
        // Just check it doesn't crash
        if (!result.success) {
            CHECK_FALSE(result.error.empty());
        } else {
            CHECK(result.data.empty());
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
