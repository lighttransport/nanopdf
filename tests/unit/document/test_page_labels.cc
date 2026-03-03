/**
 * Page labels unit tests
 *
 * Tests PageLabels which provide custom numbering schemes for PDF pages
 * including decimal (1, 2, 3), roman numerals (i, ii, iii), and letters
 * (A, B, C). Page labels can have prefixes and different starting values.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("PageLabelStyle") {
    TEST_CASE("Style enum values") {
        // Verify page label style enum
        CHECK(static_cast<int>(PageLabelStyle::None) == 0);
        CHECK(static_cast<int>(PageLabelStyle::DecimalArabic) == 1);
        CHECK(static_cast<int>(PageLabelStyle::UppercaseRoman) == 2);
        CHECK(static_cast<int>(PageLabelStyle::LowercaseRoman) == 3);
        CHECK(static_cast<int>(PageLabelStyle::UppercaseLetters) == 4);
        CHECK(static_cast<int>(PageLabelStyle::LowercaseLetters) == 5);
    }
}

TEST_SUITE("PageLabels") {
    TEST_CASE("Decimal arabic numbering") {
        PageLabels labels;
        PageLabel decimal;
        decimal.style = PageLabelStyle::DecimalArabic;
        decimal.start_value = 1;
        labels.labels[0] = decimal;

        CHECK(labels.get_label(0) == "1");
        CHECK(labels.get_label(1) == "2");
        CHECK(labels.get_label(9) == "10");
        CHECK(labels.get_label(99) == "100");
    }

    TEST_CASE("Lowercase roman numerals") {
        PageLabels labels;
        PageLabel roman;
        roman.style = PageLabelStyle::LowercaseRoman;
        roman.start_value = 1;
        labels.labels[0] = roman;

        CHECK(labels.get_label(0) == "i");
        CHECK(labels.get_label(1) == "ii");
        CHECK(labels.get_label(2) == "iii");
        CHECK(labels.get_label(3) == "iv");
        CHECK(labels.get_label(4) == "v");
        CHECK(labels.get_label(8) == "ix");
        CHECK(labels.get_label(9) == "x");
    }

    TEST_CASE("Uppercase roman numerals") {
        PageLabels labels;
        PageLabel roman;
        roman.style = PageLabelStyle::UppercaseRoman;
        roman.start_value = 1;
        labels.labels[0] = roman;

        CHECK(labels.get_label(0) == "I");
        CHECK(labels.get_label(3) == "IV");
        CHECK(labels.get_label(8) == "IX");
        CHECK(labels.get_label(39) == "XL");
        CHECK(labels.get_label(89) == "XC");
        CHECK(labels.get_label(399) == "CD");
        CHECK(labels.get_label(899) == "CM");
        CHECK(labels.get_label(1993) == "MCMXCIV");
    }

    TEST_CASE("Uppercase letters") {
        PageLabels labels;
        PageLabel letters;
        letters.style = PageLabelStyle::UppercaseLetters;
        letters.start_value = 1;
        labels.labels[0] = letters;

        CHECK(labels.get_label(0) == "A");
        CHECK(labels.get_label(1) == "B");
        CHECK(labels.get_label(25) == "Z");
        CHECK(labels.get_label(26) == "AA");
        CHECK(labels.get_label(27) == "AB");
        CHECK(labels.get_label(51) == "AZ");
        CHECK(labels.get_label(52) == "BA");
    }

    TEST_CASE("Lowercase letters") {
        PageLabels labels;
        PageLabel letters;
        letters.style = PageLabelStyle::LowercaseLetters;
        letters.start_value = 1;
        labels.labels[0] = letters;

        CHECK(labels.get_label(0) == "a");
        CHECK(labels.get_label(1) == "b");
        CHECK(labels.get_label(25) == "z");
        CHECK(labels.get_label(26) == "aa");
        CHECK(labels.get_label(51) == "az");
    }

    TEST_CASE("Label with prefix") {
        PageLabels labels;
        PageLabel main;
        main.style = PageLabelStyle::DecimalArabic;
        main.prefix = "Page ";
        main.start_value = 1;
        labels.labels[0] = main;

        CHECK(labels.get_label(0) == "Page 1");
        CHECK(labels.get_label(1) == "Page 2");
        CHECK(labels.get_label(9) == "Page 10");
    }

    TEST_CASE("Custom start value") {
        PageLabels labels;
        PageLabel custom;
        custom.style = PageLabelStyle::DecimalArabic;
        custom.start_value = 100;
        labels.labels[0] = custom;

        CHECK(labels.get_label(0) == "100");
        CHECK(labels.get_label(1) == "101");
        CHECK(labels.get_label(5) == "105");
    }

    TEST_CASE("Multiple label ranges") {
        PageLabels labels;

        // Cover pages: i, ii, iii (lowercase roman)
        PageLabel cover;
        cover.style = PageLabelStyle::LowercaseRoman;
        cover.start_value = 1;
        labels.labels[0] = cover;

        // Main content: 1, 2, 3... (decimal arabic with prefix)
        PageLabel main;
        main.style = PageLabelStyle::DecimalArabic;
        main.prefix = "Page ";
        main.start_value = 1;
        labels.labels[3] = main;

        // Appendix: A, B, C... (uppercase letters)
        PageLabel appendix;
        appendix.style = PageLabelStyle::UppercaseLetters;
        appendix.prefix = "Appendix ";
        appendix.start_value = 1;
        labels.labels[10] = appendix;

        // Verify labels across ranges
        CHECK(labels.get_label(0) == "i");
        CHECK(labels.get_label(1) == "ii");
        CHECK(labels.get_label(2) == "iii");
        CHECK(labels.get_label(3) == "Page 1");
        CHECK(labels.get_label(4) == "Page 2");
        CHECK(labels.get_label(9) == "Page 7");
        CHECK(labels.get_label(10) == "Appendix A");
        CHECK(labels.get_label(11) == "Appendix B");
    }

    TEST_CASE("No style (empty label)") {
        PageLabels labels;
        PageLabel none;
        none.style = PageLabelStyle::None;
        none.prefix = "Section ";
        labels.labels[0] = none;

        // With None style, only prefix is shown
        CHECK(labels.get_label(0) == "Section ");
        CHECK(labels.get_label(1) == "Section ");
    }

    TEST_CASE("Empty prefix") {
        PageLabels labels;
        PageLabel decimal;
        decimal.style = PageLabelStyle::DecimalArabic;
        decimal.prefix = "";
        decimal.start_value = 1;
        labels.labels[0] = decimal;

        CHECK(labels.get_label(0) == "1");
        CHECK(labels.get_label(5) == "6");
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
