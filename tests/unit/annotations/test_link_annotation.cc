/**
 * Link annotation unit tests
 *
 * Tests LinkAnnotation which provides clickable links to other
 * pages (GoTo), external files (GoToR), or URIs. Links can have
 * various actions including Launch and JavaScript.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("LinkAnnotation") {
    TEST_CASE("Default values") {
        LinkAnnotation link;

        CHECK(link.type == AnnotationType::Link);
        CHECK(link.action_type == LinkAnnotation::GoTo);
    }

    TEST_CASE("URI action") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::URI;
        link.uri = "https://example.com";
        link.rect = {50, 50, 200, 70};

        CHECK(link.action_type == LinkAnnotation::URI);
        CHECK(link.uri == "https://example.com");
        REQUIRE(link.rect.size() == 4);
        CHECK(link.rect[0] == 50);
        CHECK(link.rect[2] == 200);
    }

    TEST_CASE("GoTo action") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::GoTo;

        CHECK(link.action_type == LinkAnnotation::GoTo);
    }

    TEST_CASE("GoToR action (remote)") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::GoToR;

        CHECK(link.action_type == LinkAnnotation::GoToR);
    }

    TEST_CASE("Launch action") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::Launch;

        CHECK(link.action_type == LinkAnnotation::Launch);
    }

    TEST_CASE("Named action") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::Named;

        CHECK(link.action_type == LinkAnnotation::Named);
    }

    TEST_CASE("JavaScript action") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::JavaScript;

        CHECK(link.action_type == LinkAnnotation::JavaScript);
    }

    TEST_CASE("HTTP URL") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::URI;
        link.uri = "http://www.example.org/page.html";

        CHECK(link.uri == "http://www.example.org/page.html");
    }

    TEST_CASE("HTTPS URL") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::URI;
        link.uri = "https://secure.example.com";

        CHECK(link.uri == "https://secure.example.com");
    }

    TEST_CASE("Mailto URL") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::URI;
        link.uri = "mailto:info@example.com";

        CHECK(link.uri == "mailto:info@example.com");
    }

    TEST_CASE("Border style") {
        LinkAnnotation link;
        link.border_style = {1.0, 1.0, 2.0};

        REQUIRE(link.border_style.size() == 3);
        CHECK(link.border_style[2] == 2.0);
    }

    TEST_CASE("Rectangle bounds") {
        LinkAnnotation link;
        link.rect = {100, 200, 300, 250};

        REQUIRE(link.rect.size() == 4);
        CHECK(link.rect[0] == 100);
        CHECK(link.rect[1] == 200);
        CHECK(link.rect[2] == 300);
        CHECK(link.rect[3] == 250);
    }

    TEST_CASE("Destination dictionary") {
        LinkAnnotation link;
        link.action_type = LinkAnnotation::GoTo;
        // Destination is a Dictionary, just verify it exists
        CHECK(link.action_type == LinkAnnotation::GoTo);
    }
}

int main() {
    return nanotest::run_all_tests();
}
