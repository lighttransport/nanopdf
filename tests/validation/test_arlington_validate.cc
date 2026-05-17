#include "nanotest.hh"
#include "arlington_parser.hh"
#include "arlington_validator.hh"
#include "test_helpers.hh"
#include "nanopdf.hh"
#include <sys/wait.h>
#include <unistd.h>

using namespace nanopdf;
using namespace nanopdf::arlington;
using namespace nanopdf::test;

// Run parse + validate in a forked subprocess to isolate crashes.
// Returns: 0 = parse failed, 1 = validated ok, 2 = validated with errors,
//          -1 = crashed/timeout
struct SubprocessResult {
    int status;  // 0=parse_fail, 1=clean, 2=has_errors, -1=crash
    int error_count;
    int warning_count;
};

static SubprocessResult validate_in_subprocess(
        const std::string& filepath, const ArlingtonModel& model) {
    // Pipe to send results from child to parent
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, 0, 0};

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {-1, 0, 0}; }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);  // Close read end

        std::vector<uint8_t> pdf_data;
        Pdf pdf;
        int result[3] = {0, 0, 0};  // status, errors, warnings

        if (!parse_pdf_file(filepath, pdf_data, pdf)) {
            result[0] = 0;  // parse failed
        } else {
            Validator validator(model);
            ValidationResult vr = validator.validate_document(pdf);
            result[0] = (vr.error_count == 0) ? 1 : 2;
            result[1] = vr.error_count;
            result[2] = vr.warning_count;
        }

        ssize_t written = write(pipefd[1], result, sizeof(result));
        (void)written;
        close(pipefd[1]);
        _exit(0);
    }

    // Parent process
    close(pipefd[1]);  // Close write end

    int result[3] = {-1, 0, 0};
    int wstatus;
    pid_t wpid = waitpid(pid, &wstatus, 0);

    if (wpid == pid && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
        ssize_t n = read(pipefd[0], result, sizeof(result));
        if (n != sizeof(result)) result[0] = -1;
    }
    // else: child crashed or was signaled → status stays -1

    close(pipefd[0]);
    return {result[0], result[1], result[2]};
}

TEST_SUITE("ArlingtonValidator") {

TEST_CASE("Validate blank.pdf against Arlington model") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    std::string pdf_path =
        std::string(NANOPDF_PROJECT_DIR) + "/tests/fixtures/visual/blank.pdf";
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

TEST_CASE("Validate CC-MAIN PDFs if available") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");
    SKIP_IF(!corpus_available("cc-main-2021"), "CC-MAIN-2021 data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    std::string cc_dir = get_corpus_path("cc-main-2021");
    auto pdfs = list_pdf_files_recursive(cc_dir);
    SKIP_IF(pdfs.empty(), "No PDFs found in CC-MAIN corpus");

    int validated = 0;
    int parse_failed = 0;
    int crashed = 0;
    int zero_errors = 0;
    int total_errors = 0;
    int total_warnings = 0;
    int max_files = 10000;

    for (const auto& filepath : pdfs) {
        if (validated + parse_failed + crashed >= max_files) break;

        // Use fork() to isolate each PDF — parser bugs can corrupt memory
        auto sr = validate_in_subprocess(filepath, model);

        if (sr.status == 0) {
            parse_failed++;
        } else if (sr.status == -1) {
            crashed++;
            size_t slash = filepath.rfind('/');
            std::string name = (slash != std::string::npos) ?
                filepath.substr(slash + 1) : filepath;
            std::cout << "  " << name << ": CRASHED\n";
        } else {
            validated++;
            total_errors += sr.error_count;
            total_warnings += sr.warning_count;
            if (sr.error_count == 0) zero_errors++;

            if (sr.error_count > 0) {
                size_t slash = filepath.rfind('/');
                std::string name = (slash != std::string::npos) ?
                    filepath.substr(slash + 1) : filepath;
                std::cout << "  " << name << ": "
                          << sr.error_count << " errors, "
                          << sr.warning_count << " warnings\n";
            }
        }
    }

    std::cout << "\n[validation] CC-MAIN-2021 Summary:\n"
              << "  Attempted:     " << (validated + parse_failed + crashed)
              << "\n"
              << "  Parsed+Valid:  " << validated << "\n"
              << "  Parse failed:  " << parse_failed << "\n"
              << "  Crashed:       " << crashed << "\n"
              << "  Clean (0 err): " << zero_errors << " / " << validated
              << " (" << (validated > 0 ? (100 * zero_errors / validated) : 0)
              << "%)\n"
              << "  Total errors:  " << total_errors << "\n"
              << "  Total warnings:" << total_warnings << "\n";

    CHECK(validated > 0);
    // Parser crashes should be investigated separately, but must not
    // abort the entire test run.
}

}  // TEST_SUITE

int main() {
    return nanotest::run_all_tests();
}
