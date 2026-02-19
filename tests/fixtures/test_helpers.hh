#ifndef NANOPDF_TEST_HELPERS_HH
#define NANOPDF_TEST_HELPERS_HH

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include "nanopdf.hh"

namespace nanopdf {
namespace test {

// ============================================================================
// Path Utilities
// ============================================================================

/// Get the path to the test data directory
/// Defined by NANOPDF_TEST_DATA_DIR compile definition
std::string get_test_data_dir();

/// Get the path to a test PDF file
/// @param filename The PDF filename (e.g., "blank.pdf")
/// @param subdir Optional subdirectory under data/pdfs/ (e.g., "minimal", "encrypted")
std::string get_test_pdf_path(const std::string& filename,
                               const std::string& subdir = "");

/// Get the path to a test font file
/// @param filename The font filename
std::string get_test_font_path(const std::string& filename);

// ============================================================================
// File I/O Utilities
// ============================================================================

/// Read an entire file into a byte vector
/// @param filepath The path to the file
/// @param out_data Output vector to receive file contents
/// @return true if successful, false on error
bool read_file(const std::string& filepath, std::vector<uint8_t>& out_data);

/// Write a byte vector to a file
/// @param filepath The path to the file
/// @param data The data to write
/// @return true if successful, false on error
bool write_file(const std::string& filepath, const std::vector<uint8_t>& data);

/// Check if a file exists
/// @param filepath The path to the file
/// @return true if file exists and is readable
bool file_exists(const std::string& filepath);

// ============================================================================
// Binary Data Comparison
// ============================================================================

/// Compare two byte vectors for equality
/// @param a First vector
/// @param b Second vector
/// @return true if vectors have same size and contents
bool compare_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

/// Compare byte vector to C array
/// @param vec Vector to compare
/// @param arr C array to compare
/// @param arr_size Size of the C array
/// @return true if contents match
bool compare_bytes(const std::vector<uint8_t>& vec,
                   const uint8_t* arr,
                   size_t arr_size);

// ============================================================================
// String Utilities
// ============================================================================

/// Convert byte vector to hex string for debugging
/// @param data The byte data
/// @param max_bytes Maximum number of bytes to convert (default: all)
/// @return Hex string like "48656c6c6f" for "Hello"
std::string bytes_to_hex(const std::vector<uint8_t>& data, size_t max_bytes = 0);

/// Convert hex string to byte vector
/// @param hex Hex string like "48656c6c6f"
/// @param out_data Output vector to receive decoded bytes
/// @return true if successful, false on invalid hex
bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out_data);

// ============================================================================
// Minimal PDF Building Utilities
// ============================================================================

/// Create a minimal valid PDF document in memory
/// @return PDF bytes as a vector
std::vector<uint8_t> create_minimal_pdf();

/// Create a minimal PDF with one page containing text
/// @param text The text to include on the page
/// @return PDF bytes as a vector
std::vector<uint8_t> create_text_pdf(const std::string& text);

/// Create a minimal encrypted PDF
/// @param user_password User password (empty for no user password)
/// @param owner_password Owner password
/// @return PDF bytes as a vector
std::vector<uint8_t> create_encrypted_pdf(const std::string& user_password,
                                          const std::string& owner_password);

// ============================================================================
// Stream Testing Utilities
// ============================================================================

/// Create test data for filter testing (repeated pattern)
/// @param pattern The byte pattern to repeat
/// @param pattern_size Size of the pattern
/// @param count Number of times to repeat
/// @return Vector containing repeated pattern
std::vector<uint8_t> create_pattern_data(const uint8_t* pattern,
                                         size_t pattern_size,
                                         size_t count);

/// Create test data with specific characteristics for compression testing
/// @param size Size of data to generate
/// @param compressible If true, generate highly compressible data
/// @return Vector containing test data
std::vector<uint8_t> create_test_data(size_t size, bool compressible = true);

// ============================================================================
// Corpus Testing Utilities
// ============================================================================

/// Get the path to the corpora directory
std::string get_corpora_dir();

/// Get the path to a specific corpus
/// @param name Corpus name (e.g., "arlington", "safedocs")
std::string get_corpus_path(const std::string& name);

/// Check if a corpus is downloaded and available
/// @param name Corpus name
/// @return true if the corpus directory exists and is non-empty
bool corpus_available(const std::string& name);

/// List PDF files in a directory (non-recursive)
/// @param dir Directory path
/// @return Vector of full file paths ending in .pdf (case-insensitive)
std::vector<std::string> list_pdf_files(const std::string& dir);

/// List PDF files recursively
/// @param dir Root directory path
/// @return Vector of full file paths ending in .pdf (case-insensitive)
std::vector<std::string> list_pdf_files_recursive(const std::string& dir);

/// List TSV files in a directory (non-recursive)
/// @param dir Directory path
/// @return Vector of full file paths ending in .tsv
std::vector<std::string> list_tsv_files(const std::string& dir);

/// Parse a PDF file using nanopdf
/// @param filepath Path to PDF file
/// @param out_data Output buffer (must outlive out_pdf since Pdf holds raw pointers)
/// @param out_pdf Output Pdf struct
/// @return true if parsing succeeded
bool parse_pdf_file(const std::string& filepath,
                    std::vector<uint8_t>& out_data, nanopdf::Pdf& out_pdf);

/// Statistics for corpus parsing tests
struct CorpusTestStats {
    int total = 0;
    int ok = 0;
    int failed = 0;
    int crashed = 0;

    void print_summary(const std::string& corpus_name) const;
};

/// Parse all PDFs in a directory and collect statistics
/// @param dir Directory containing PDF files
/// @param max_files Maximum number of files to parse (0 = unlimited)
/// @param recursive Whether to search recursively
/// @return Statistics about parsing results
CorpusTestStats test_parse_directory(const std::string& dir, int max_files = 0,
                                     bool recursive = false);

}  // namespace test
}  // namespace nanopdf

#endif  // NANOPDF_TEST_HELPERS_HH
