#include "nanotest.hh"
#include "arlington_parser.hh"
#include "arlington_validator.hh"
#include "test_helpers.hh"
#include "nanopdf.hh"

using namespace nanopdf;
using namespace nanopdf::arlington;
using namespace nanopdf::test;

TEST_SUITE("ArlingtonValidator") {

TEST_CASE("Validate blank.pdf against Arlington model") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    std::string pdf_path = std::string(NANOPDF_PROJECT_DIR) + "/data/blank.pdf";
    SKIP_IF(!file_exists(pdf_path), "blank.pdf not found");

    std::vector<uint8_t> pdf_data;
    Pdf pdf;
    REQUIRE(parse_pdf_file(pdf_path, pdf_data, pdf));

    Validator validator(model);
    ValidationResult result = validator.validate_document(pdf);

    result.print_summary();
    result.print_findings(Severity::Warning);

    // blank.pdf should validate cleanly: no errors and no warnings
    CHECK_EQ(result.error_count, 0);
    CHECK_EQ(result.warning_count, 0);
}

TEST_CASE("Validate minimal in-memory PDF") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    auto pdf_bytes = create_minimal_pdf();
    Pdf pdf;
    REQUIRE(parse_from_memory(pdf_bytes.data(), pdf_bytes.size(), &pdf));

    Validator validator(model);
    ValidationResult result = validator.validate_document(pdf);

    result.print_summary();
    result.print_findings(Severity::Info);

    // Check that the Info false positive is gone:
    // fn:IsRequired(fn:SinceVersion(VER,fn:IsPresent(Info))) should not fire
    // when Info key is absent from the trailer
    for (const auto& f : result.findings) {
        if (f.severity == Severity::Error) {
            CHECK_FALSE(f.key == "Info" && f.message == "Required key missing");
        }
    }

    // Resources is required=TRUE, inheritable=TRUE on PageObject.
    // The minimal PDF has no Resources anywhere in the page tree,
    // so it's legitimately missing. Verify the error count is exactly 1
    // (only the Resources error).
    CHECK_EQ(result.error_count, 1);
}

TEST_CASE("Validate SafeDocs PDFs if available") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");
    SKIP_IF(!corpus_available("safedocs"), "SafeDocs data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    std::string safedocs_dir = get_corpus_path("safedocs");
    auto pdfs = list_pdf_files_recursive(safedocs_dir);
    SKIP_IF(pdfs.empty(), "No PDFs found in SafeDocs corpus");

    int validated = 0;
    int max_files = 20;
    for (const auto& filepath : pdfs) {
        if (validated >= max_files) break;

        std::vector<uint8_t> pdf_data;
        Pdf pdf;
        if (!parse_pdf_file(filepath, pdf_data, pdf)) continue;

        Validator validator(model);
        ValidationResult result = validator.validate_document(pdf);
        validated++;

        // Extract filename for display
        size_t slash = filepath.rfind('/');
        std::string name = (slash != std::string::npos) ?
            filepath.substr(slash + 1) : filepath;

        std::cout << "  " << name << ": "
                  << result.error_count << " errors, "
                  << result.warning_count << " warnings\n";
    }

    std::cout << "[validation] Validated " << validated << " SafeDocs PDFs\n";
    CHECK(validated > 0);
}

}  // TEST_SUITE

int main() {
    return nanotest::run_all_tests();
}
