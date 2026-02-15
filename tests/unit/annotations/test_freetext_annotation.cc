/**
 * FreeText annotation unit tests
 *
 * Tests FreeTextAnnotation which displays text directly on the page
 * (unlike TextAnnotation which is a popup). FreeText annotations
 * support font specification, alignment (quadding), and callouts.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("FreeTextAnnotation") {
    TEST_CASE("Default values") {
        FreeTextAnnotation annot;

        CHECK(annot.type == AnnotationType::FreeText);
        CHECK(annot.quadding == 0);  // Left justified
    }

    TEST_CASE("Basic properties") {
        FreeTextAnnotation annot;
        annot.contents = "Sample text";
        annot.default_appearance = "/Helv 12 Tf 0 g";
        annot.quadding = 1;  // Center

        CHECK(annot.contents == "Sample text");
        CHECK(annot.default_appearance == "/Helv 12 Tf 0 g");
        CHECK(annot.quadding == 1);
    }

    TEST_CASE("Left justification") {
        FreeTextAnnotation annot;
        annot.quadding = 0;

        CHECK(annot.quadding == 0);
    }

    TEST_CASE("Center justification") {
        FreeTextAnnotation annot;
        annot.quadding = 1;

        CHECK(annot.quadding == 1);
    }

    TEST_CASE("Right justification") {
        FreeTextAnnotation annot;
        annot.quadding = 2;

        CHECK(annot.quadding == 2);
    }

    TEST_CASE("Default appearance string") {
        FreeTextAnnotation annot;

        // Helvetica 12pt, black color
        annot.default_appearance = "/Helv 12 Tf 0 g";
        CHECK(annot.default_appearance == "/Helv 12 Tf 0 g");

        // Times 14pt, red color
        annot.default_appearance = "/Times 14 Tf 1 0 0 rg";
        CHECK(annot.default_appearance == "/Times 14 Tf 1 0 0 rg");
    }

    TEST_CASE("Rectangle bounds") {
        FreeTextAnnotation annot;
        annot.rect = {100, 200, 300, 250};

        REQUIRE(annot.rect.size() == 4);
        CHECK(annot.rect[0] == 100);
        CHECK(annot.rect[1] == 200);
        CHECK(annot.rect[2] == 300);
        CHECK(annot.rect[3] == 250);
    }

    TEST_CASE("Multi-line text") {
        FreeTextAnnotation annot;
        annot.contents = "Line one\nLine two\nLine three";

        CHECK(annot.contents.find("Line one") != std::string::npos);
        CHECK(annot.contents.find("Line two") != std::string::npos);
        CHECK(annot.contents.find("Line three") != std::string::npos);
    }

    TEST_CASE("Border and fill") {
        FreeTextAnnotation annot;
        annot.border.width = 1.5;
        annot.border.style = AnnotationBorder::Solid;
        annot.color = {1.0, 1.0, 0.8};  // Light yellow background

        CHECK(annot.border.width == 1.5);
        REQUIRE(annot.color.size() == 3);
        CHECK(annot.color[2] == 0.8);
    }

    TEST_CASE("Callout line") {
        FreeTextAnnotation annot;
        // Callout line endpoints: [x1 y1 x2 y2]
        annot.callout_line = {150, 150, 200, 200};

        REQUIRE(annot.callout_line.size() == 4);
        CHECK(annot.callout_line[0] == 150);
        CHECK(annot.callout_line[3] == 200);
    }

    TEST_CASE("Default style") {
        FreeTextAnnotation annot;
        annot.default_style = "font: Helvetica 12pt";

        CHECK(annot.default_style == "font: Helvetica 12pt");
    }

    TEST_CASE("Annotation name") {
        FreeTextAnnotation annot;
        annot.name = "FreeText1";
        annot.contents = "Sample text";

        CHECK(annot.name == "FreeText1");
        CHECK(annot.contents == "Sample text");
    }
}

int main() {
    return nanotest::run_all_tests();
}
