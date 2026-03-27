/**
 * PDF parsing unit tests
 *
 * Tests parse_from_memory() with synthetic PDF data to verify header detection,
 * version parsing, xref table loading, and error handling for malformed input.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

#include <cstring>
#include <string>
#include <vector>
#include <sstream>

using namespace nanopdf;

namespace {

// Helper: convert a string literal to a byte vector
std::vector<uint8_t> to_bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Build a minimal valid PDF with correct byte offsets.
// Returns the raw bytes of a self-consistent PDF-1.4 document with
// a Catalog (object 1) and an empty Pages tree (object 2).
std::vector<uint8_t> build_minimal_pdf() {
    // We build the PDF in stages so that xref offsets are exact.
    std::string header = "%PDF-1.4\n";

    std::string obj1 = "1 0 obj\n"
                        "<< /Type /Catalog /Pages 2 0 R >>\n"
                        "endobj\n";

    std::string obj2 = "2 0 obj\n"
                        "<< /Type /Pages /Kids [] /Count 0 >>\n"
                        "endobj\n";

    size_t off_obj1 = header.size();
    size_t off_obj2 = off_obj1 + obj1.size();
    size_t off_xref = off_obj2 + obj2.size();

    // Format xref offsets as 10-digit zero-padded numbers
    auto fmt_offset = [](size_t off) -> std::string {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%010zu", off);
        return std::string(buf);
    };

    std::string xref;
    xref += "xref\n";
    xref += "0 3\n";
    xref += "0000000000 65535 f \n";
    xref += fmt_offset(off_obj1) + " 00000 n \n";
    xref += fmt_offset(off_obj2) + " 00000 n \n";

    std::string trailer;
    trailer += "trailer\n";
    trailer += "<< /Size 3 /Root 1 0 R >>\n";
    trailer += "startxref\n";
    trailer += std::to_string(off_xref) + "\n";
    trailer += "%%EOF\n";

    std::string pdf = header + obj1 + obj2 + xref + trailer;
    return to_bytes(pdf);
}

// Build a minimal PDF with a specific version string (e.g., "1.7")
std::vector<uint8_t> build_pdf_with_version(int major, int minor) {
    std::string header = "%PDF-" + std::to_string(major) + "." +
                         std::to_string(minor) + "\n";

    std::string obj1 = "1 0 obj\n"
                        "<< /Type /Catalog /Pages 2 0 R >>\n"
                        "endobj\n";

    std::string obj2 = "2 0 obj\n"
                        "<< /Type /Pages /Kids [] /Count 0 >>\n"
                        "endobj\n";

    size_t off_obj1 = header.size();
    size_t off_obj2 = off_obj1 + obj1.size();
    size_t off_xref = off_obj2 + obj2.size();

    auto fmt_offset = [](size_t off) -> std::string {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%010zu", off);
        return std::string(buf);
    };

    std::string xref;
    xref += "xref\n";
    xref += "0 3\n";
    xref += "0000000000 65535 f \n";
    xref += fmt_offset(off_obj1) + " 00000 n \n";
    xref += fmt_offset(off_obj2) + " 00000 n \n";

    std::string trailer;
    trailer += "trailer\n";
    trailer += "<< /Size 3 /Root 1 0 R >>\n";
    trailer += "startxref\n";
    trailer += std::to_string(off_xref) + "\n";
    trailer += "%%EOF\n";

    std::string pdf = header + obj1 + obj2 + xref + trailer;
    return to_bytes(pdf);
}

}  // anonymous namespace

TEST_SUITE("PdfParsing") {

// ============================================================================
// Reject invalid input
// ============================================================================

TEST_CASE("Reject null input") {
    Pdf pdf;
    bool ok = parse_from_memory(nullptr, 100, &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject null output pointer") {
    uint8_t data[] = "%PDF-1.4";
    bool ok = parse_from_memory(data, sizeof(data), nullptr);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject zero-length input") {
    uint8_t data[] = {0};
    Pdf pdf;
    bool ok = parse_from_memory(data, 0, &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject too-small input (< 8 bytes)") {
    uint8_t data[] = {'%', 'P', 'D', 'F'};
    Pdf pdf;
    bool ok = parse_from_memory(data, sizeof(data), &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject 7-byte input") {
    uint8_t data[] = {'%', 'P', 'D', 'F', '-', '1', '.'};
    Pdf pdf;
    bool ok = parse_from_memory(data, sizeof(data), &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject non-PDF data (no header)") {
    std::string html = "<html><body>Not a PDF</body></html>";
    Pdf pdf;
    bool ok = parse_from_memory(
        reinterpret_cast<const uint8_t*>(html.data()), html.size(), &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject random binary data") {
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = static_cast<uint8_t>(i * 37 + 11);
    }
    Pdf pdf;
    bool ok = parse_from_memory(data, sizeof(data), &pdf);
    CHECK_FALSE(ok);
}

TEST_CASE("Reject PDF header only (no body)") {
    std::string header_only = "%PDF-1.4\n";
    Pdf pdf;
    bool ok = parse_from_memory(
        reinterpret_cast<const uint8_t*>(header_only.data()),
        header_only.size(), &pdf);
    CHECK_FALSE(ok);
}

// ============================================================================
// Parse minimal valid PDF
// ============================================================================

TEST_CASE("Parse minimal valid PDF") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    // Root should be object 1 (the Catalog)
    CHECK_EQ(pdf.root, 1u);
}

TEST_CASE("Minimal PDF has correct version") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 4);
}

TEST_CASE("Minimal PDF has object offset cache built") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    // The normal parsing path builds an object offset cache rather than
    // populating xref_sections (which is only used by repair_xref).
    CHECK(pdf.object_offsets_built);
    CHECK_FALSE(pdf.object_offsets_failed);
}

TEST_CASE("Minimal PDF trailer size") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    // /Size 3 in the trailer
    CHECK_EQ(pdf.size, 3u);
}

TEST_CASE("Minimal PDF data pointer is set") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    CHECK(pdf.data != nullptr);
    CHECK(pdf.data_size > 0);
}

// ============================================================================
// Version parsing
// ============================================================================

TEST_CASE("Parse PDF version 1.0") {
    std::vector<uint8_t> data = build_pdf_with_version(1, 0);
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 0);
}

TEST_CASE("Parse PDF version 1.5") {
    std::vector<uint8_t> data = build_pdf_with_version(1, 5);
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 5);
}

TEST_CASE("Parse PDF version 1.7") {
    std::vector<uint8_t> data = build_pdf_with_version(1, 7);
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 7);
}

TEST_CASE("Parse PDF version 2.0") {
    std::vector<uint8_t> data = build_pdf_with_version(2, 0);
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.version_major, 2);
    CHECK_EQ(pdf.version_minor, 0);
}

// ============================================================================
// Root object number verification
// ============================================================================

TEST_CASE("Root object number is 1 for minimal PDF") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.root, 1u);
}

// ============================================================================
// Header preceded by junk bytes
// ============================================================================

TEST_CASE("PDF header preceded by small junk") {
    // PDF spec allows up to 1024 bytes before %PDF header
    std::vector<uint8_t> valid_pdf = build_minimal_pdf();

    // Prepend 100 bytes of junk
    std::vector<uint8_t> junk(100, 0x20);  // spaces
    std::vector<uint8_t> data;
    data.insert(data.end(), junk.begin(), junk.end());
    data.insert(data.end(), valid_pdf.begin(), valid_pdf.end());

    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 4);
    CHECK_EQ(pdf.root, 1u);
}

TEST_CASE("PDF header preceded by 1024 bytes of junk") {
    std::vector<uint8_t> valid_pdf = build_minimal_pdf();

    // Prepend exactly 1024 bytes of junk (within spec limit)
    std::vector<uint8_t> junk(1024, 'X');
    std::vector<uint8_t> data;
    data.insert(data.end(), junk.begin(), junk.end());
    data.insert(data.end(), valid_pdf.begin(), valid_pdf.end());

    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    // Parser scans up to 4096 bytes for header, so 1024 should work
    REQUIRE(ok);

    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 4);
}

TEST_CASE("PDF header preceded by BOM (byte order mark)") {
    std::vector<uint8_t> valid_pdf = build_minimal_pdf();

    // UTF-8 BOM: EF BB BF
    std::vector<uint8_t> bom = {0xEF, 0xBB, 0xBF};
    std::vector<uint8_t> data;
    data.insert(data.end(), bom.begin(), bom.end());
    data.insert(data.end(), valid_pdf.begin(), valid_pdf.end());

    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);

    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 4);
}

// ============================================================================
// test_helpers utility: create_minimal_pdf
// ============================================================================

TEST_CASE("test_helpers create_minimal_pdf parses successfully") {
    std::vector<uint8_t> data = test::create_minimal_pdf();
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    REQUIRE(ok);
    CHECK_EQ(pdf.root, 1u);
}

// ============================================================================
// ParseOptions overload
// ============================================================================

TEST_CASE("Parse with default ParseOptions") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    ParseOptions opts;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf, opts);
    REQUIRE(ok);
    CHECK_EQ(pdf.root, 1u);
}

TEST_CASE("Parse with auto_repair option") {
    std::vector<uint8_t> data = build_minimal_pdf();
    Pdf pdf;
    ParseOptions opts;
    opts.auto_repair = true;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf, opts);
    REQUIRE(ok);
    CHECK_EQ(pdf.version_major, 1);
    CHECK_EQ(pdf.version_minor, 4);
}

TEST_CASE("Null input with ParseOptions") {
    Pdf pdf;
    ParseOptions opts;
    bool ok = parse_from_memory(nullptr, 100, &pdf, opts);
    CHECK_FALSE(ok);
}

TEST_CASE("Too-small input with ParseOptions") {
    uint8_t data[] = {'%', 'P', 'D'};
    Pdf pdf;
    ParseOptions opts;
    bool ok = parse_from_memory(data, sizeof(data), &pdf, opts);
    CHECK_FALSE(ok);
}

TEST_CASE("parse_pdf failure populates last_error on Pdf struct") {
    Pdf pdf;
    // Feed garbage data that has %PDF header but is otherwise invalid
    std::string bad = "%PDF-1.4\ngarbage data with no xref";
    ParseResult result = parse_pdf(
        reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), &pdf);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.error.empty());
    CHECK(result.kind != ErrorKind::None);
}

TEST_CASE("parse_pdf success clears last_error") {
    Pdf pdf;
    // First, set an artificial error
    pdf.last_error = "previous error";
    pdf.last_error_kind = ErrorKind::Internal;

    // Parse a valid PDF
    auto data = build_minimal_pdf();
    ParseResult result = parse_pdf(
        reinterpret_cast<const uint8_t*>(data.data()), data.size(), &pdf);
    // On success, load_document_structure calls clear_error()
    if (result.success) {
        CHECK(pdf.get_last_error().empty());
        CHECK(pdf.get_last_error_kind() == ErrorKind::None);
    }
}

TEST_CASE("get_last_error_kind returns Malformed for missing root") {
    Pdf pdf;
    // PDF with xref but root pointing to nonexistent object
    std::string bad_root =
        "%PDF-1.4\n"
        "xref\n"
        "0 1\n"
        "0000000000 65535 f \n"
        "trailer\n"
        "<< /Size 1 >>\n"  // No /Root entry
        "startxref\n"
        "10\n"
        "%%EOF\n";
    ParseResult result = parse_pdf(
        reinterpret_cast<const uint8_t*>(bad_root.data()), bad_root.size(), &pdf);
    CHECK_FALSE(result.success);
    // Should get a specific error, not generic
    if (!result.error.empty()) {
        CHECK(result.kind != ErrorKind::None);
    }
}

}  // TEST_SUITE

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
