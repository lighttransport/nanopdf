/**
 * Type0 (CID) font unit tests
 *
 * Tests Type0 fonts (composite fonts) which are used for CJK
 * (Chinese, Japanese, Korean) and other large character sets.
 * Type0 fonts use CID (Character ID) to GID (Glyph ID) mappings.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("Type0Font") {
    TEST_CASE("Basic structure") {
        Type0Font font;

        // Type0 fonts should have "Type0" subtype
        CHECK(font.subtype == "Type0");
    }

    TEST_CASE("Registry, Ordering, Supplement (ROS)") {
        Type0Font font;

        font.registry = "Adobe";
        font.ordering = "Identity";
        font.supplement = 0;
        font.base_font = "HeiseiKakuGo-W5";

        CHECK(font.registry == "Adobe");
        CHECK(font.ordering == "Identity");
        CHECK(font.supplement == 0);
        CHECK(font.base_font == "HeiseiKakuGo-W5");
    }

    TEST_CASE("CID to GID mapping") {
        Type0Font font;

        // Allocate CID to GID map
        font.cid_to_gid_map.resize(256, 0);

        // Set specific mapping
        font.cid_to_gid_map[100] = 150;

        CHECK(font.cid_to_gid_map.size() == 256);
        CHECK(font.cid_to_gid_map[100] == 150);
    }

    TEST_CASE("Identity mapping") {
        Type0Font font;

        // Identity CID ordering: CID == GID
        font.registry = "Adobe";
        font.ordering = "Identity";

        // Simulate identity mapping
        font.cid_to_gid_map.resize(1000);
        for (size_t i = 0; i < font.cid_to_gid_map.size(); i++) {
            font.cid_to_gid_map[i] = static_cast<uint16_t>(i);
        }

        // Verify identity mapping
        CHECK(font.cid_to_gid_map[0] == 0);
        CHECK(font.cid_to_gid_map[500] == 500);
        CHECK(font.cid_to_gid_map[999] == 999);
    }

    TEST_CASE("Large character set") {
        Type0Font font;
        font.base_font = "HeiseiMin-W3";
        font.registry = "Adobe";
        font.ordering = "Japan1";
        font.supplement = 6;

        // Adobe-Japan1-6 has ~23,000 glyphs
        font.cid_to_gid_map.resize(23000, 0);

        CHECK(font.cid_to_gid_map.size() == 23000);
        CHECK(font.supplement == 6);
    }

    TEST_CASE("Default glyph (CID 0)") {
        Type0Font font;
        font.cid_to_gid_map.resize(256, 0);

        // CID 0 is typically the default/notdef glyph
        CHECK(font.cid_to_gid_map[0] == 0);
    }

    TEST_CASE("Sparse CID mapping") {
        Type0Font font;
        font.cid_to_gid_map.resize(1000, 0);

        // Set only a few specific CID mappings
        font.cid_to_gid_map[100] = 50;
        font.cid_to_gid_map[200] = 75;
        font.cid_to_gid_map[500] = 120;

        CHECK(font.cid_to_gid_map[100] == 50);
        CHECK(font.cid_to_gid_map[200] == 75);
        CHECK(font.cid_to_gid_map[500] == 120);

        // Unmapped CIDs default to 0
        CHECK(font.cid_to_gid_map[150] == 0);
        CHECK(font.cid_to_gid_map[999] == 0);
    }
}

int main() {
    return nanotest::run_all_tests();
}
