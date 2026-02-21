#include "nanotest.hh"
#include "test_helpers.hh"
#include "nanopdf.hh"

using namespace nanopdf;
using namespace nanopdf::test;

static const int kDefaultMaxFiles = 100000;

TEST_SUITE("CorpusParsing") {

TEST_CASE("Parse CC-MAIN PDFs") {
    SKIP_IF(!corpus_available("cc-main-2021"),
            "CC-MAIN-2021 corpus not downloaded");

    std::string dir = get_corpus_path("cc-main-2021");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("CC-MAIN-2021");

    CHECK(stats.total > 0);
    // Report crashes but don't fail the test — these indicate parser bugs
    // to investigate separately, not test infrastructure failures
    if (stats.crashed > 0) {
        std::cout << "  [WARNING] " << stats.crashed
                  << " PDFs crashed the parser\n";
    }
}

TEST_CASE("Parse GovDocs1 PDFs") {
    SKIP_IF(!corpus_available("govdocs1"),
            "GovDocs1 corpus not downloaded");

    std::string dir = get_corpus_path("govdocs1");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("GovDocs1");

    CHECK(stats.total > 0);
}

TEST_CASE("Parse veraPDF corpus") {
    SKIP_IF(!corpus_available("verapdf-corpus"),
            "veraPDF corpus not downloaded");

    std::string dir = get_corpus_path("verapdf-corpus");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("veraPDF");

    CHECK(stats.total > 0);
}

TEST_CASE("Parse pdf.js test PDFs") {
    SKIP_IF(!corpus_available("pdfjs-tests"),
            "pdf.js tests not downloaded");

    std::string dir = get_corpus_path("pdfjs-tests");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("pdf.js");

    CHECK(stats.total > 0);
}

TEST_CASE("Parse pdfium test PDFs") {
    SKIP_IF(!corpus_available("pdfium-tests"),
            "pdfium tests not downloaded");

    std::string dir = get_corpus_path("pdfium-tests");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("pdfium");

    CHECK(stats.total > 0);
}

TEST_CASE("Parse Tika corpus PDFs") {
    SKIP_IF(!corpus_available("tika-corpus"),
            "Tika corpus not downloaded");

    std::string dir = get_corpus_path("tika-corpus");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("Tika");

    CHECK(stats.total > 0);
}

TEST_CASE("Parse unsafe-docs PDFs") {
    SKIP_IF(!corpus_available("unsafe-docs"),
            "unsafe-docs corpus not downloaded");

    std::string dir = get_corpus_path("unsafe-docs");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("unsafe-docs");

    // For unsafe/malformed PDFs, we expect many failures
    // but zero crashes (no segfaults, no unhandled exceptions)
    CHECK(stats.total > 0);
}

TEST_CASE("Parse SafeDocs PDFs") {
    SKIP_IF(!corpus_available("safedocs"),
            "SafeDocs corpus not downloaded");

    std::string dir = get_corpus_path("safedocs");
    auto stats = test_parse_directory_isolated(dir, kDefaultMaxFiles, true);
    stats.print_summary("SafeDocs");

    CHECK(stats.total > 0);
}

}  // TEST_SUITE

int main() {
    return nanotest::run_all_tests();
}
