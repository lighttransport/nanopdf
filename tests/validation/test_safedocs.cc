#include "nanotest.hh"
#include "test_helpers.hh"
#include "nanopdf.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("SafeDocsParsing") {

TEST_CASE("Parse all SafeDocs PDFs") {
    SKIP_IF(!corpus_available("safedocs"), "SafeDocs corpus not downloaded");

    std::string safedocs_dir = get_corpus_path("safedocs");
    auto stats = test_parse_directory(safedocs_dir, 0, true);
    stats.print_summary("SafeDocs");

    CHECK(stats.total > 0);
    CHECK(stats.crashed == 0);
}

TEST_CASE("Parse SafeDocs CompactedSyntax") {
    std::string dir = get_corpus_path("safedocs/CompactedSyntax");
    SKIP_IF(!file_exists(dir), "SafeDocs CompactedSyntax not available");

    auto pdfs = list_pdf_files_recursive(dir);
    SKIP_IF(pdfs.empty(), "No PDFs in CompactedSyntax");

    CorpusTestStats stats;
    for (const auto& filepath : pdfs) {
        stats.total++;
        try {
            std::vector<uint8_t> pdf_data;
            Pdf pdf;
            if (parse_pdf_file(filepath, pdf_data, pdf)) {
                stats.ok++;
            } else {
                stats.failed++;
            }
        } catch (...) {
            stats.crashed++;
        }
    }
    stats.print_summary("SafeDocs/CompactedSyntax");
    CHECK(stats.crashed == 0);
}

TEST_CASE("Parse SafeDocs Dialects") {
    std::string dir = get_corpus_path("safedocs/Dialects");
    SKIP_IF(!file_exists(dir), "SafeDocs Dialects not available");

    auto pdfs = list_pdf_files_recursive(dir);
    SKIP_IF(pdfs.empty(), "No PDFs in Dialects");

    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary("SafeDocs/Dialects");
    CHECK(stats.crashed == 0);
}

TEST_CASE("Parse SafeDocs InlineImage") {
    std::string dir = get_corpus_path("safedocs/InlineImage");
    SKIP_IF(!file_exists(dir), "SafeDocs InlineImage not available");

    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary("SafeDocs/InlineImage");
    CHECK(stats.crashed == 0);
}

TEST_CASE("Parse SafeDocs Miscellaneous") {
    std::string dir = get_corpus_path("safedocs/Miscellaneous");
    SKIP_IF(!file_exists(dir), "SafeDocs Miscellaneous not available");

    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary("SafeDocs/Miscellaneous");
    CHECK(stats.crashed == 0);
}

TEST_CASE("Parse SafeDocs UnicodePasswords") {
    std::string dir = get_corpus_path("safedocs/UnicodePasswords");
    SKIP_IF(!file_exists(dir), "SafeDocs UnicodePasswords not available");

    auto stats = test_parse_directory(dir, 0, true);
    stats.print_summary("SafeDocs/UnicodePasswords");
    CHECK(stats.crashed == 0);
}

}  // TEST_SUITE

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}
#endif
