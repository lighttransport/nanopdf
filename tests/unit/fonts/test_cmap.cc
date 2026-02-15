/**
 * CMap (Character Map) unit tests
 *
 * Tests the CMap structure used for mapping character codes to
 * Unicode values in Type0 (CID) fonts. CMaps support both direct
 * mappings and range mappings for efficient encoding coverage.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("CMap") {
    TEST_CASE("Basic structure") {
        CMap cmap;
        cmap.name = "TestCMap";
        cmap.registry = "Adobe";
        cmap.ordering = "Identity";
        cmap.supplement = 0;

        CHECK(cmap.name == "TestCMap");
        CHECK(cmap.registry == "Adobe");
        CHECK(cmap.ordering == "Identity");
        CHECK(cmap.supplement == 0);
    }

    TEST_CASE("Direct character mapping") {
        CMap cmap;

        // Add direct mappings for A and B
        cmap.code_to_unicode[65] = 0x0041;  // A
        cmap.code_to_unicode[66] = 0x0042;  // B

        // Test direct mapping
        CHECK(cmap.map_code_to_unicode(65) == 0x0041);
        CHECK(cmap.map_code_to_unicode(66) == 0x0042);
    }

    TEST_CASE("Range mapping") {
        CMap cmap;

        // Add a range mapping for ASCII printable characters
        // Range: 0x20 (space) to 0x7E (~)
        cmap.range_mappings[std::make_pair(0x20, 0x7E)] = 0x0020;

        // Test range mapping - characters within range
        CHECK(cmap.map_code_to_unicode(0x20) == 0x0020);  // space
        CHECK(cmap.map_code_to_unicode(0x21) == 0x0021);  // !
        CHECK(cmap.map_code_to_unicode(0x41) == 0x0041);  // A
        CHECK(cmap.map_code_to_unicode(0x7E) == 0x007E);  // ~
    }

    TEST_CASE("Fallback mapping") {
        CMap cmap;

        // No mapping defined for code 255
        // Should fall back to returning the code itself
        CHECK(cmap.map_code_to_unicode(255) == 255);
    }

    TEST_CASE("Direct mapping overrides range") {
        CMap cmap;

        // Add range mapping
        cmap.range_mappings[std::make_pair(0x20, 0x7E)] = 0x0020;

        // Add direct mapping for 'A' that overrides range
        cmap.code_to_unicode[0x41] = 0x3042;  // Map A to hiragana A

        // Direct mapping should take precedence
        CHECK(cmap.map_code_to_unicode(0x41) == 0x3042);

        // But adjacent characters should still use range mapping
        CHECK(cmap.map_code_to_unicode(0x40) == 0x0040);  // @
        CHECK(cmap.map_code_to_unicode(0x42) == 0x0042);  // B
    }

    TEST_CASE("Multiple ranges") {
        CMap cmap;

        // Add multiple non-overlapping ranges
        cmap.range_mappings[std::make_pair(0x20, 0x7E)] = 0x0020;  // ASCII
        cmap.range_mappings[std::make_pair(0xA0, 0xFF)] = 0x00A0;  // Latin-1 Supplement

        // Test first range
        CHECK(cmap.map_code_to_unicode(0x41) == 0x0041);  // A

        // Test second range
        CHECK(cmap.map_code_to_unicode(0xA0) == 0x00A0);  // non-breaking space
        CHECK(cmap.map_code_to_unicode(0xFF) == 0x00FF);  // ÿ
    }

    TEST_CASE("CID to Unicode mapping") {
        CMap cmap;
        cmap.name = "Adobe-Identity-0";
        cmap.registry = "Adobe";
        cmap.ordering = "Identity";

        // Typical CID font mapping
        for (uint32_t i = 0; i < 256; i++) {
            cmap.code_to_unicode[i] = i;
        }

        CHECK(cmap.map_code_to_unicode(32) == 32);
        CHECK(cmap.map_code_to_unicode(65) == 65);
        CHECK(cmap.map_code_to_unicode(128) == 128);
    }

    TEST_CASE("Empty CMap") {
        CMap cmap;

        // Empty CMap should use fallback for all codes
        CHECK(cmap.map_code_to_unicode(0) == 0);
        CHECK(cmap.map_code_to_unicode(65) == 65);
        CHECK(cmap.map_code_to_unicode(255) == 255);
    }
}

int main() {
    return nanotest::run_all_tests();
}
