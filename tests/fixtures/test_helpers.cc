#include "test_helpers.hh"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <sys/stat.h>

namespace nanopdf {
namespace test {

// ============================================================================
// Path Utilities
// ============================================================================

std::string get_test_data_dir() {
#ifdef NANOPDF_TEST_DATA_DIR
    return NANOPDF_TEST_DATA_DIR;
#else
    return "./tests/data";
#endif
}

std::string get_test_pdf_path(const std::string& filename,
                               const std::string& subdir) {
    std::string path = get_test_data_dir() + "/pdfs/";
    if (!subdir.empty()) {
        path += subdir + "/";
    }
    path += filename;
    return path;
}

std::string get_test_font_path(const std::string& filename) {
    return get_test_data_dir() + "/fonts/" + filename;
}

// ============================================================================
// File I/O Utilities
// ============================================================================

bool read_file(const std::string& filepath, std::vector<uint8_t>& out_data) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    out_data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out_data.data()), size)) {
        return false;
    }

    return true;
}

bool write_file(const std::string& filepath, const std::vector<uint8_t>& data) {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

bool file_exists(const std::string& filepath) {
    struct stat buffer;
    return (stat(filepath.c_str(), &buffer) == 0);
}

// ============================================================================
// Binary Data Comparison
// ============================================================================

bool compare_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

bool compare_bytes(const std::vector<uint8_t>& vec,
                   const uint8_t* arr,
                   size_t arr_size) {
    if (vec.size() != arr_size) {
        return false;
    }
    return std::memcmp(vec.data(), arr, arr_size) == 0;
}

// ============================================================================
// String Utilities
// ============================================================================

std::string bytes_to_hex(const std::vector<uint8_t>& data, size_t max_bytes) {
    size_t num_bytes = max_bytes > 0 ? std::min(max_bytes, data.size()) : data.size();
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < num_bytes; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    if (max_bytes > 0 && data.size() > max_bytes) {
        oss << "...";
    }
    return oss.str();
}

bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out_data) {
    if (hex.length() % 2 != 0) {
        return false;
    }

    out_data.clear();
    out_data.reserve(hex.length() / 2);

    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        char* end;
        long byte_val = std::strtol(byte_str.c_str(), &end, 16);
        if (end != byte_str.c_str() + 2) {
            return false;
        }
        out_data.push_back(static_cast<uint8_t>(byte_val));
    }

    return true;
}

// ============================================================================
// Minimal PDF Building Utilities
// ============================================================================

std::vector<uint8_t> create_minimal_pdf() {
    const char* pdf_content =
        "%PDF-1.4\n"
        "1 0 obj\n"
        "<< /Type /Catalog /Pages 2 0 R >>\n"
        "endobj\n"
        "2 0 obj\n"
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
        "endobj\n"
        "3 0 obj\n"
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\n"
        "endobj\n"
        "xref\n"
        "0 4\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000117 00000 n \n"
        "trailer\n"
        "<< /Size 4 /Root 1 0 R >>\n"
        "startxref\n"
        "197\n"
        "%%EOF\n";

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pdf_content);
    return std::vector<uint8_t>(bytes, bytes + std::strlen(pdf_content));
}

std::vector<uint8_t> create_text_pdf(const std::string& text) {
    // Create a simple PDF with text content
    std::ostringstream oss;
    oss << "%PDF-1.4\n"
        << "1 0 obj\n"
        << "<< /Type /Catalog /Pages 2 0 R >>\n"
        << "endobj\n"
        << "2 0 obj\n"
        << "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
        << "endobj\n"
        << "3 0 obj\n"
        << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        << "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\n"
        << "endobj\n"
        << "4 0 obj\n"
        << "<< /Length " << (text.length() + 50) << " >>\n"
        << "stream\n"
        << "BT\n"
        << "/F1 12 Tf\n"
        << "100 700 Td\n"
        << "(" << text << ") Tj\n"
        << "ET\n"
        << "endstream\n"
        << "endobj\n"
        << "5 0 obj\n"
        << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
        << "endobj\n"
        << "xref\n"
        << "0 6\n"
        << "0000000000 65535 f \n"
        << "0000000009 00000 n \n"
        << "0000000058 00000 n \n"
        << "0000000117 00000 n \n"
        << "0000000270 00000 n \n"
        << "0000000380 00000 n \n"
        << "trailer\n"
        << "<< /Size 6 /Root 1 0 R >>\n"
        << "startxref\n"
        << "460\n"
        << "%%EOF\n";

    std::string pdf_str = oss.str();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pdf_str.c_str());
    return std::vector<uint8_t>(bytes, bytes + pdf_str.length());
}

std::vector<uint8_t> create_encrypted_pdf(const std::string& user_password,
                                          const std::string& owner_password) {
    // Note: This is a simplified version - full encryption would require
    // implementing the encryption algorithm. For now, return a placeholder.
    // Real implementation would need to call the crypto functions.
    return create_minimal_pdf();
}

// ============================================================================
// Stream Testing Utilities
// ============================================================================

std::vector<uint8_t> create_pattern_data(const uint8_t* pattern,
                                         size_t pattern_size,
                                         size_t count) {
    std::vector<uint8_t> result;
    result.reserve(pattern_size * count);
    for (size_t i = 0; i < count; ++i) {
        result.insert(result.end(), pattern, pattern + pattern_size);
    }
    return result;
}

std::vector<uint8_t> create_test_data(size_t size, bool compressible) {
    std::vector<uint8_t> result(size);
    if (compressible) {
        // Highly compressible: repeated pattern
        for (size_t i = 0; i < size; ++i) {
            result[i] = static_cast<uint8_t>(i % 16);
        }
    } else {
        // Less compressible: pseudo-random
        uint32_t seed = 12345;
        for (size_t i = 0; i < size; ++i) {
            seed = seed * 1103515245 + 12345;  // Simple LCG
            result[i] = static_cast<uint8_t>(seed >> 16);
        }
    }
    return result;
}

}  // namespace test
}  // namespace nanopdf
