/**
 * Font substitution unit tests
 *
 * Tests the font substitution system which maps missing PDF fonts
 * to available system fonts. This is essential for rendering PDFs
 * that reference fonts not embedded in the document.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("FontSubstitution") {
    TEST_CASE("Basic structure") {
        FontSubstitution sub;

        sub.original_name = "Times-Roman";
        sub.substitute_name = "serif";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Times-Roman");
        CHECK(sub.substitute_name == "serif");
        CHECK(sub.substitute_path == "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf");
        CHECK(sub.is_system_font == true);
    }

    TEST_CASE("Standard font substitution") {
        FontSubstitution sub;

        // Substitute Times-Roman with Liberation Serif
        sub.original_name = "Times-Roman";
        sub.substitute_name = "LiberationSerif-Regular";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Times-Roman");
        CHECK(sub.substitute_name == "LiberationSerif-Regular");
    }

    TEST_CASE("Embedded font (no substitution needed)") {
        FontSubstitution sub;

        sub.original_name = "ABCDEF+TimesNewRoman";
        sub.substitute_name = "";
        sub.substitute_path = "";
        sub.is_system_font = false;

        CHECK(sub.original_name == "ABCDEF+TimesNewRoman");
        CHECK(sub.substitute_name.empty());
        CHECK(sub.is_system_font == false);
    }

    TEST_CASE("Sans-serif substitution") {
        FontSubstitution sub;

        sub.original_name = "Helvetica";
        sub.substitute_name = "LiberationSans-Regular";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Helvetica");
        CHECK(sub.substitute_name == "LiberationSans-Regular");
    }

    TEST_CASE("Monospace font substitution") {
        FontSubstitution sub;

        sub.original_name = "Courier";
        sub.substitute_name = "LiberationMono-Regular";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationMono-Regular.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Courier");
        CHECK(sub.substitute_name == "LiberationMono-Regular");
    }

    TEST_CASE("Bold font substitution") {
        FontSubstitution sub;

        sub.original_name = "Helvetica-Bold";
        sub.substitute_name = "LiberationSans-Bold";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Helvetica-Bold");
        CHECK(sub.substitute_name == "LiberationSans-Bold");
    }

    TEST_CASE("Italic font substitution") {
        FontSubstitution sub;

        sub.original_name = "Times-Italic";
        sub.substitute_name = "LiberationSerif-Italic";
        sub.substitute_path = "/usr/share/fonts/liberation/LiberationSerif-Italic.ttf";
        sub.is_system_font = true;

        CHECK(sub.original_name == "Times-Italic");
        CHECK(sub.substitute_name == "LiberationSerif-Italic");
    }

    TEST_CASE("CJK font substitution") {
        FontSubstitution sub;

        sub.original_name = "HeiseiKakuGo-W5";
        sub.substitute_name = "NotoSansCJK-Regular";
        sub.substitute_path = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
        sub.is_system_font = true;

        CHECK(sub.original_name == "HeiseiKakuGo-W5");
        CHECK(sub.substitute_name == "NotoSansCJK-Regular");
    }

    TEST_CASE("Fallback to generic font") {
        FontSubstitution sub;

        sub.original_name = "UnknownFont";
        sub.substitute_name = "serif";
        sub.substitute_path = "";
        sub.is_system_font = false;

        CHECK(sub.original_name == "UnknownFont");
        CHECK(sub.substitute_name == "serif");
    }
}

int main() {
    return nanotest::run_all_tests();
}
