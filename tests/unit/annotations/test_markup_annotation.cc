/**
 * Markup annotation unit tests
 *
 * Tests MarkupAnnotation which is the base class for text markup
 * annotations including Highlight, Underline, Squiggly, and StrikeOut.
 * Markup annotations use QuadPoints to define the affected text regions.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("MarkupAnnotation") {
    TEST_CASE("Highlight annotation") {
        MarkupAnnotation markup(AnnotationType::Highlight);

        CHECK(markup.type == AnnotationType::Highlight);
        CHECK(markup.opacity == 1.0);
    }

    TEST_CASE("Basic properties") {
        MarkupAnnotation markup(AnnotationType::Highlight);
        markup.opacity = 0.5;
        markup.title = "Reviewer";
        markup.subject = "Important";

        CHECK(markup.opacity == 0.5);
        CHECK(markup.title == "Reviewer");
        CHECK(markup.subject == "Important");
    }

    TEST_CASE("QuadPoints for text region") {
        MarkupAnnotation markup(AnnotationType::Highlight);

        // QuadPoints specify a quadrilateral in x,y pairs:
        // [x1 y1 x2 y2 x3 y3 x4 y4]
        std::vector<double> quad = {100, 100, 200, 100, 200, 120, 100, 120};
        markup.quad_points.push_back(quad);

        REQUIRE(markup.quad_points.size() == 1);
        REQUIRE(markup.quad_points[0].size() == 8);
        CHECK(markup.quad_points[0][0] == 100);
        CHECK(markup.quad_points[0][1] == 100);
        CHECK(markup.quad_points[0][4] == 200);
        CHECK(markup.quad_points[0][5] == 120);
    }

    TEST_CASE("Multiple quad regions") {
        MarkupAnnotation markup(AnnotationType::Highlight);

        // First quad
        std::vector<double> quad1 = {10, 10, 100, 10, 100, 20, 10, 20};
        markup.quad_points.push_back(quad1);

        // Second quad
        std::vector<double> quad2 = {10, 30, 50, 30, 50, 40, 10, 40};
        markup.quad_points.push_back(quad2);

        CHECK(markup.quad_points.size() == 2);
    }

    TEST_CASE("Underline annotation") {
        MarkupAnnotation underline(AnnotationType::Underline);

        CHECK(underline.type == AnnotationType::Underline);
    }

    TEST_CASE("Squiggly annotation") {
        MarkupAnnotation squiggly(AnnotationType::Squiggly);

        CHECK(squiggly.type == AnnotationType::Squiggly);
    }

    TEST_CASE("StrikeOut annotation") {
        MarkupAnnotation strikeout(AnnotationType::StrikeOut);

        CHECK(strikeout.type == AnnotationType::StrikeOut);
    }

    TEST_CASE("Yellow highlight color") {
        MarkupAnnotation markup(AnnotationType::Highlight);
        markup.color = {1.0, 1.0, 0.0};  // Yellow

        REQUIRE(markup.color.size() == 3);
        CHECK(markup.color[0] == 1.0);  // Red
        CHECK(markup.color[1] == 1.0);  // Green
        CHECK(markup.color[2] == 0.0);  // Blue
    }

    TEST_CASE("Opacity values") {
        MarkupAnnotation markup(AnnotationType::Highlight);

        // Full opacity
        markup.opacity = 1.0;
        CHECK(markup.opacity == 1.0);

        // Semi-transparent
        markup.opacity = 0.5;
        CHECK(markup.opacity == 0.5);

        // Very transparent
        markup.opacity = 0.1;
        CHECK(markup.opacity == 0.1);
    }

    TEST_CASE("Annotation name") {
        MarkupAnnotation markup(AnnotationType::Highlight);
        markup.title = "Author";
        markup.contents = "Original comment";
        markup.name = "Highlight1";

        CHECK(markup.name == "Highlight1");
        CHECK(markup.title == "Author");
    }

    TEST_CASE("Modified date") {
        MarkupAnnotation markup(AnnotationType::Highlight);
        markup.modified_date = "D:20240102140000";

        CHECK(markup.modified_date == "D:20240102140000");
    }
}

int main() {
    return nanotest::run_all_tests();
}
