/**
 * ASCII85Decode filter unit tests
 *
 * Tests the ASCII85Decode filter (also known as Base85) which encodes binary
 * data using ASCII characters from '!' to 'u' (85 characters). Groups of 4 bytes
 * are encoded as 5 ASCII characters. The special character 'z' represents four
 * zero bytes. The encoded data is terminated by '~>'.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("ASCII85Decode") {
    TEST_CASE("Standard encoding of 'Man '") {
        // "Man " = 0x4D616E20 => "9jqo^"
        const char *encoded = "9jqo^~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
        CHECK(result.data[2] == 'n');
        CHECK(result.data[3] == ' ');
    }

    TEST_CASE("'z' shorthand for four zero bytes") {
        const char *encoded = "z~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        CHECK(result.data[0] == 0);
        CHECK(result.data[1] == 0);
        CHECK(result.data[2] == 0);
        CHECK(result.data[3] == 0);
    }

    TEST_CASE("Partial group (fewer than 5 encoded chars)") {
        // Encoding of "Ma" (2 bytes) → "9jq"
        const char *encoded = "9jq~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 2);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
    }

    TEST_CASE("Whitespace is ignored") {
        const char *encoded = "9jqo ^\r\n~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
        CHECK(result.data[2] == 'n');
        CHECK(result.data[3] == ' ');
    }

    TEST_CASE("Multiple groups with 'z' shorthand") {
        // "Man " + 4 zeros = "9jqo^" + "z"
        const char *encoded = "9jqo^z~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 8);
        CHECK(result.data[0] == 'M');
        CHECK(result.data[1] == 'a');
        CHECK(result.data[2] == 'n');
        CHECK(result.data[3] == ' ');
        CHECK(result.data[4] == 0);
        CHECK(result.data[5] == 0);
        CHECK(result.data[6] == 0);
        CHECK(result.data[7] == 0);
    }

    TEST_CASE("Empty input (just end marker)") {
        const char *encoded = "~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 0);
    }

    TEST_CASE("Invalid character should fail") {
        // '{' is outside the '!' to 'u' range
        const char *encoded = "9jqo{~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }

    TEST_CASE("'Hello' encoding") {
        // "Hello" = 0x48656C6C 6F => "87cUR" + "DZ"
        const char *encoded = "87cURDZ~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 5);
        CHECK(result.data[0] == 'H');
        CHECK(result.data[1] == 'e');
        CHECK(result.data[2] == 'l');
        CHECK(result.data[3] == 'l');
        CHECK(result.data[4] == 'o');
    }

    TEST_CASE("Null input handling") {
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(nullptr, 0, params);

        // Implementation may succeed with empty output or fail
        // Just check it doesn't crash
        if (!result.success) {
            CHECK_FALSE(result.error.empty());
        } else {
            CHECK(result.data.empty());
        }
    }

    TEST_CASE("Missing end marker should fail or handle gracefully") {
        const char *encoded = "9jqo^";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        // Implementation may accept or reject this
        if (result.success) {
            CHECK(result.data.size() == 4);
        }
    }

    TEST_CASE("Single 'z' with whitespace") {
        const char *encoded = " z \t~>";
        filters::DecodeParams params;
        DecodedStream result = filters::decode_ascii85(
            reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

        REQUIRE(result.success);
        CHECK(result.data.size() == 4);
        for (int i = 0; i < 4; i++) {
            CHECK(result.data[i] == 0);
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
