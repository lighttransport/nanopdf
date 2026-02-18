#include "nanotest.hh"
#include "test_helpers.hh"
#include "nanopdf.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("PdfDifferencesParsing") {

TEST_CASE("Parse all pdf-differences PDFs") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");

    std::string dir = get_corpus_path("pdf-differences");
    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary("pdf-differences");

    CHECK(stats.total > 0);
    CHECK(stats.crashed == 0);
}

// Test each known category individually
static void test_category(const char* category) {
    std::string dir = get_corpus_path(std::string("pdf-differences/") + category);
    if (!file_exists(dir)) {
        std::cout << "[nanotest] SKIP: " << category << " not available\n";
        return;
    }

    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary(std::string("pdf-differences/") + category);
}

TEST_CASE("Parse DefaultColorSpaces") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");
    test_category("DefaultColorSpaces");
}

TEST_CASE("Parse NegativeFontSize") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");
    test_category("NegativeFontSize");
}

TEST_CASE("Parse Type3WordSpacing") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");
    test_category("Type3WordSpacing");
}

TEST_CASE("Parse UnknownFilter") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");
    test_category("UnknownFilter");
}

TEST_CASE("Parse all remaining categories") {
    SKIP_IF(!corpus_available("pdf-differences"),
            "pdf-differences corpus not downloaded");

    const char* categories[] = {
        "AnnotationFlags", "BlendMode", "CMaps",
        "ColorSpace", "Coordinates", "Encryption",
        "FontDescriptor", "Functions", "Linearized",
        "OptionalContent", "OutputIntents", "PageLabels",
        "Transparency"
    };

    for (const char* cat : categories) {
        test_category(cat);
    }
}

}  // TEST_SUITE

int main() {
    return nanotest::run_all_tests();
}
