/**
 * Annotation types and enums unit tests
 *
 * Tests the basic annotation type enumerations, flags, and
 * common annotation structures used across all annotation types.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("AnnotationType") {
    TEST_CASE("Annotation type enum values") {
        // Verify annotation type enum matches PDF spec
        CHECK(static_cast<int>(AnnotationType::Text) == 0);
        CHECK(static_cast<int>(AnnotationType::Link) == 1);
        CHECK(static_cast<int>(AnnotationType::FreeText) == 2);
        CHECK(static_cast<int>(AnnotationType::Line) == 3);
        CHECK(static_cast<int>(AnnotationType::Square) == 4);
        CHECK(static_cast<int>(AnnotationType::Circle) == 5);
        CHECK(static_cast<int>(AnnotationType::Polygon) == 6);
        CHECK(static_cast<int>(AnnotationType::PolyLine) == 7);
        CHECK(static_cast<int>(AnnotationType::Highlight) == 8);
        CHECK(static_cast<int>(AnnotationType::Underline) == 9);
        CHECK(static_cast<int>(AnnotationType::Squiggly) == 10);
        CHECK(static_cast<int>(AnnotationType::StrikeOut) == 11);
        CHECK(static_cast<int>(AnnotationType::Stamp) == 12);
        CHECK(static_cast<int>(AnnotationType::Caret) == 13);
        CHECK(static_cast<int>(AnnotationType::Ink) == 14);
        CHECK(static_cast<int>(AnnotationType::Popup) == 15);
        CHECK(static_cast<int>(AnnotationType::FileAttachment) == 16);
        CHECK(static_cast<int>(AnnotationType::Sound) == 17);
        CHECK(static_cast<int>(AnnotationType::Movie) == 18);
        CHECK(static_cast<int>(AnnotationType::Widget) == 19);
        CHECK(static_cast<int>(AnnotationType::Screen) == 20);
        CHECK(static_cast<int>(AnnotationType::PrinterMark) == 21);
        CHECK(static_cast<int>(AnnotationType::TrapNet) == 22);
        CHECK(static_cast<int>(AnnotationType::Watermark) == 23);
        CHECK(static_cast<int>(AnnotationType::ThreeD) == 24);
        CHECK(static_cast<int>(AnnotationType::Redact) == 25);
    }
}

TEST_SUITE("AnnotationFlags") {
    TEST_CASE("Annotation flag bit values") {
        // Verify flag bit values match PDF spec
        CHECK(static_cast<uint32_t>(AnnotationFlags::Invisible) == 0x0001);
        CHECK(static_cast<uint32_t>(AnnotationFlags::Hidden) == 0x0002);
        CHECK(static_cast<uint32_t>(AnnotationFlags::Print) == 0x0004);
        CHECK(static_cast<uint32_t>(AnnotationFlags::NoZoom) == 0x0008);
        CHECK(static_cast<uint32_t>(AnnotationFlags::NoRotate) == 0x0010);
        CHECK(static_cast<uint32_t>(AnnotationFlags::NoView) == 0x0020);
        CHECK(static_cast<uint32_t>(AnnotationFlags::ReadOnly) == 0x0040);
        CHECK(static_cast<uint32_t>(AnnotationFlags::Locked) == 0x0080);
        CHECK(static_cast<uint32_t>(AnnotationFlags::ToggleNoView) == 0x0100);
        CHECK(static_cast<uint32_t>(AnnotationFlags::LockedContents) == 0x0200);
    }

    TEST_CASE("Combined flags") {
        // Test combining multiple flags
        uint32_t flags = static_cast<uint32_t>(AnnotationFlags::Print) |
                        static_cast<uint32_t>(AnnotationFlags::ReadOnly);

        CHECK(flags == 0x0044);
        CHECK((flags & static_cast<uint32_t>(AnnotationFlags::Print)) != 0);
        CHECK((flags & static_cast<uint32_t>(AnnotationFlags::ReadOnly)) != 0);
        CHECK((flags & static_cast<uint32_t>(AnnotationFlags::Hidden)) == 0);
    }
}

TEST_SUITE("AnnotationBorder") {
    TEST_CASE("Default border style") {
        AnnotationBorder border;

        CHECK(border.width == 1.0);
        CHECK(border.style == AnnotationBorder::Solid);
        CHECK(border.dash_pattern.empty());
    }

    TEST_CASE("Solid border") {
        AnnotationBorder border;
        border.width = 2.5;
        border.style = AnnotationBorder::Solid;

        CHECK(border.width == 2.5);
        CHECK(border.style == AnnotationBorder::Solid);
    }

    TEST_CASE("Dashed border") {
        AnnotationBorder border;
        border.width = 1.5;
        border.style = AnnotationBorder::Dashed;
        border.dash_pattern = {3, 2};

        CHECK(border.width == 1.5);
        CHECK(border.style == AnnotationBorder::Dashed);
        REQUIRE(border.dash_pattern.size() == 2);
        CHECK(border.dash_pattern[0] == 3);
        CHECK(border.dash_pattern[1] == 2);
    }

    TEST_CASE("Beveled border") {
        AnnotationBorder border;
        border.style = AnnotationBorder::Beveled;

        CHECK(border.style == AnnotationBorder::Beveled);
    }

    TEST_CASE("Inset border") {
        AnnotationBorder border;
        border.style = AnnotationBorder::Inset;

        CHECK(border.style == AnnotationBorder::Inset);
    }

    TEST_CASE("Underline border") {
        AnnotationBorder border;
        border.style = AnnotationBorder::Underline;

        CHECK(border.style == AnnotationBorder::Underline);
    }

    TEST_CASE("Complex dash pattern") {
        AnnotationBorder border;
        border.style = AnnotationBorder::Dashed;
        border.dash_pattern = {5, 3, 1, 3};  // Morse code pattern

        REQUIRE(border.dash_pattern.size() == 4);
        CHECK(border.dash_pattern[0] == 5);
        CHECK(border.dash_pattern[3] == 3);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
