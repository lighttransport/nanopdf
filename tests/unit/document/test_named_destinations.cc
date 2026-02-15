/**
 * Named destinations unit tests
 *
 * Tests NamedDestination which provides named references to specific
 * locations in a PDF document. Named destinations can specify fit types
 * (FitH, FitV, FitR, etc.) and position coordinates.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("NamedDestination") {
    TEST_CASE("Default values") {
        NamedDestination dest;

        CHECK(dest.page_number == 0);
        CHECK(dest.position.empty());
        CHECK(dest.fit_type.empty());
    }

    TEST_CASE("Basic destination") {
        NamedDestination dest;
        dest.name = "chapter1";
        dest.page_number = 5;

        CHECK(dest.name == "chapter1");
        CHECK(dest.page_number == 5);
    }

    TEST_CASE("Destination with position") {
        NamedDestination dest;
        dest.name = "section2.3";
        dest.page_number = 10;
        dest.position = {100, 200, 1.5};

        CHECK(dest.name == "section2.3");
        CHECK(dest.page_number == 10);
        REQUIRE(dest.position.size() == 3);
        CHECK(dest.position[0] == 100);
        CHECK(dest.position[1] == 200);
        CHECK(dest.position[2] == 1.5);
    }

    TEST_CASE("FitR destination type") {
        NamedDestination dest;
        dest.name = "figure1";
        dest.page_number = 3;
        dest.position = {100, 200, 1.5};
        dest.fit_type = "FitR";

        CHECK(dest.fit_type == "FitR");
    }

    TEST_CASE("FitH destination type") {
        NamedDestination dest;
        dest.fit_type = "FitH";
        dest.position = {0, 720};  // Top of page

        CHECK(dest.fit_type == "FitH");
        CHECK(dest.position[1] == 720);
    }

    TEST_CASE("FitV destination type") {
        NamedDestination dest;
        dest.fit_type = "FitV";
        dest.position = {300};  // Left margin

        CHECK(dest.fit_type == "FitV");
        CHECK(dest.position[0] == 300);
    }

    TEST_CASE("FitBH destination type") {
        NamedDestination dest;
        dest.fit_type = "FitBH";

        CHECK(dest.fit_type == "FitBH");
    }

    TEST_CASE("FitBV destination type") {
        NamedDestination dest;
        dest.fit_type = "FitBV";

        CHECK(dest.fit_type == "FitBV");
    }

    TEST_CASE("Fit (whole page) destination") {
        NamedDestination dest;
        dest.fit_type = "Fit";
        dest.page_number = 1;

        CHECK(dest.fit_type == "Fit");
        CHECK(dest.page_number == 1);
    }

    TEST_CASE("XYZ destination with zoom") {
        NamedDestination dest;
        dest.fit_type = "XYZ";
        dest.position = {72, 720, 1.25};  // x, y, zoom

        CHECK(dest.fit_type == "XYZ");
        CHECK(dest.position[2] == 1.25);  // 125% zoom
    }

    TEST_CASE("Named destination with special characters") {
        NamedDestination dest;
        dest.name = "section_2.1-alpha";
        dest.page_number = 15;

        CHECK(dest.name == "section_2.1-alpha");
    }

    TEST_CASE("Empty name") {
        NamedDestination dest;
        dest.name = "";
        dest.page_number = 0;

        CHECK(dest.name.empty());
    }
}

int main() {
    return nanotest::run_all_tests();
}
