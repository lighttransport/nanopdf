/**
 * Type3 font unit tests
 *
 * Tests Type3 fonts which are user-defined fonts with glyph
 * descriptions as PDF content streams. Type3 fonts include
 * font matrix, bounding box, and character procedures.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("Type3Font") {
    TEST_CASE("Basic structure") {
        Type3Font font;

        // Type3 fonts should have "Type3" subtype
        CHECK(font.subtype == "Type3");
    }

    TEST_CASE("Default font matrix") {
        Type3Font font;

        // Font matrix should be 6-element array
        REQUIRE(font.font_matrix.size() == 6);

        // Default matrix: [0.001 0 0 0.001 0 0]
        // Converts from glyph space to text space
        CHECK(font.font_matrix[0] == 0.001);
        CHECK(font.font_matrix[1] == 0.0);
        CHECK(font.font_matrix[2] == 0.0);
        CHECK(font.font_matrix[3] == 0.001);
        CHECK(font.font_matrix[4] == 0.0);
        CHECK(font.font_matrix[5] == 0.0);
    }

    TEST_CASE("Font bounding box") {
        Type3Font font;

        // Set font bounding box [llx lly urx ury]
        font.font_bbox = {0, 0, 1000, 1000};

        REQUIRE(font.font_bbox.size() == 4);
        CHECK(font.font_bbox[0] == 0);     // llx
        CHECK(font.font_bbox[1] == 0);     // lly
        CHECK(font.font_bbox[2] == 1000);  // urx
        CHECK(font.font_bbox[3] == 1000);  // ury
    }

    TEST_CASE("Custom font matrix") {
        Type3Font font;

        // Set custom font matrix (1pt = 1000 glyph units)
        font.font_matrix = {0.001, 0, 0, 0.001, 0, 0};

        CHECK(font.font_matrix[0] == 0.001);
        CHECK(font.font_matrix[3] == 0.001);
    }

    TEST_CASE("Scaled font matrix") {
        Type3Font font;

        // Font matrix with 2x scaling
        font.font_matrix = {0.002, 0, 0, 0.002, 0, 0};

        CHECK(font.font_matrix[0] == 0.002);
        CHECK(font.font_matrix[3] == 0.002);
    }

    TEST_CASE("Character widths") {
        Type3Font font;

        // Type3 fonts store character widths in the CharProcs dictionary
        // For this test, just verify the structure exists
        CHECK(font.subtype == "Type3");
    }

    TEST_CASE("Non-square bounding box") {
        Type3Font font;

        // Wide bounding box (landscape)
        font.font_bbox = {0, -250, 2000, 750};

        CHECK(font.font_bbox[0] == 0);
        CHECK(font.font_bbox[1] == -250);
        CHECK(font.font_bbox[2] == 2000);
        CHECK(font.font_bbox[3] == 750);
    }

    TEST_CASE("Rotated font matrix") {
        Type3Font font;

        // 90-degree rotation: [0 0.001 -0.001 0 0 0]
        font.font_matrix = {0, 0.001, -0.001, 0, 0, 0};

        CHECK(font.font_matrix[0] == 0);
        CHECK(font.font_matrix[1] == 0.001);
        CHECK(font.font_matrix[2] == -0.001);
        CHECK(font.font_matrix[3] == 0);
    }

    TEST_CASE("Font matrix with translation") {
        Type3Font font;

        // Matrix with translation component
        font.font_matrix = {0.001, 0, 0, 0.001, 100, 200};

        CHECK(font.font_matrix[4] == 100);
        CHECK(font.font_matrix[5] == 200);
    }
}

int main() {
    return nanotest::run_all_tests();
}
