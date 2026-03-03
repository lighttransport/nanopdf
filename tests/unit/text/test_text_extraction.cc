/**
 * Text extraction unit tests
 *
 * Tests the text extraction functionality for various PDF operators
 * including Tj (show text), T* (next line), ' and " (quote operators),
 * Td/TD (text positioning), and hex string literals.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"
#include <cstring>

using namespace nanopdf;
using namespace nanopdf::test;

namespace {

// Helper function to create a stream Value
Value make_stream(const char* content) {
    Value val;
    val.SetType(Value::STREAM);
    val.stream.data.assign(content, content + std::strlen(content));
    return val;
}

// Helper function to set up a simple test font
void setup_simple_font(Page& page, const std::string& font_name) {
    auto font = std::unique_ptr<BaseFont>(new BaseFont());
    font->subtype = "Type1";
    font->base_font = "Helvetica";
    font->encoding = "WinAnsiEncoding";
    page.fonts[font_name] = std::move(font);
    page.fonts_loaded = true;
}

}  // namespace

TEST_SUITE("TextExtraction") {
    TEST_CASE("Basic Tj operator") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "72 720 Td\n"
            "(Hello World) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Hello World") != std::string::npos);
    }

    TEST_CASE("Multiple Tj calls") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "72 720 Td\n"
            "(Hello ) Tj\n"
            "(World) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Hello") != std::string::npos);
        CHECK(text.find("World") != std::string::npos);
    }

    TEST_CASE("Multiline with T* operator") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "14 TL\n"
            "72 720 Td\n"
            "(Line One) Tj\n"
            "T*\n"
            "(Line Two) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Line One") != std::string::npos);
        CHECK(text.find("Line Two") != std::string::npos);
    }

    TEST_CASE("Quote operator (')") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        // ' operator: move to next line and show text
        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "14 TL\n"
            "72 720 Td\n"
            "(First) Tj\n"
            "(Second) '\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("First") != std::string::npos);
        CHECK(text.find("Second") != std::string::npos);
    }

    TEST_CASE("Td positioning operator") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "72 720 Td\n"
            "(Part A) Tj\n"
            "200 0 Td\n"
            "(Part B) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Part A") != std::string::npos);
        CHECK(text.find("Part B") != std::string::npos);
    }

    TEST_CASE("Hex string literal") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        // "Hi" in hex = 4869
        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "72 720 Td\n"
            "<4869> Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Hi") != std::string::npos);
    }

    TEST_CASE("Empty page with no content") {
        Pdf pdf;
        Page page;

        // No content at all
        std::string text = extract_text_from_page(pdf, page);
        // Should not crash, may return empty
        CHECK(true);  // If we get here without crashing, test passes
    }

    TEST_CASE("Empty BT/ET block") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream("BT\nET\n"));
        std::string text = extract_text_from_page(pdf, page);
        // Should not crash
        CHECK(true);
    }

    TEST_CASE("TD positioning operator (with line leading)") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "72 720 Td\n"
            "(First) Tj\n"
            "0 -14 TD\n"
            "(Second) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("First") != std::string::npos);
        CHECK(text.find("Second") != std::string::npos);
    }

    TEST_CASE("Tm matrix operator") {
        Pdf pdf;
        Page page;
        setup_simple_font(page, "F1");

        page.contents.push_back(make_stream(
            "BT\n"
            "/F1 12 Tf\n"
            "1 0 0 1 72 720 Tm\n"
            "(Matrix Text) Tj\n"
            "ET\n"));

        std::string text = extract_text_from_page(pdf, page);
        CHECK(text.find("Matrix Text") != std::string::npos);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
