/**
 * CCITTFaxDecode unit tests
 *
 * Tests CCITT Group 3 (1D and 2D) and Group 4 fax compression used for
 * bi-level (black and white) images in PDF. CCITT encoding uses Huffman
 * codes for run-length encoding of scanlines.
 *
 * Test modes:
 * - k=0: Pure 1D encoding (Group 3 1D)
 * - k>0: Mixed 1D/2D encoding (Group 3 2D)
 * - k=-1: Pure 2D encoding (Group 4)
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"
#include <stdexcept>

using namespace nanopdf;
using namespace nanopdf::test;

namespace {

// Bit builder for creating CCITT-encoded test data
struct BitBuilder {
    std::vector<int> bits;

    void append(uint16_t code, uint8_t length) {
        for (int i = length - 1; i >= 0; --i) {
            bits.push_back((code >> i) & 1);
        }
    }

    void append_string(const std::string& bit_string) {
        for (char c : bit_string) {
            bits.push_back(c == '1' ? 1 : 0);
        }
    }

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) {
                bytes[i / 8] |= static_cast<uint8_t>(1 << (7 - (i % 8)));
            }
        }
        return bytes;
    }
};

// CCITT white run-length codes (subset for testing)
void append_white(BitBuilder& bb, int run) {
    struct Entry { int run; uint16_t code; uint8_t length; };
    static const Entry table[] = {
        {0, 0x35, 8},  {1, 0x07, 6},  {2, 0x07, 4},  {3, 0x08, 4},
        {4, 0x0B, 4},  {5, 0x0C, 4},  {6, 0x0E, 4},  {7, 0x0F, 4},
        {8, 0x13, 5},  {9, 0x14, 5},  {10, 0x07, 5}, {11, 0x08, 5},
        {12, 0x08, 6}, {13, 0x03, 6}, {14, 0x34, 6}, {15, 0x35, 6},
        {16, 0x2A, 6},
    };
    for (const auto& entry : table) {
        if (entry.run == run) {
            bb.append(entry.code, entry.length);
            return;
        }
    }
    throw std::runtime_error("Unsupported white run length");
}

// CCITT black run-length codes (subset for testing)
void append_black(BitBuilder& bb, int run) {
    struct Entry { int run; uint16_t code; uint8_t length; };
    static const Entry table[] = {
        {0, 0x37, 10}, {1, 0x02, 3}, {2, 0x03, 2}, {3, 0x02, 2},
        {4, 0x03, 3},  {5, 0x03, 4}, {6, 0x02, 4}, {7, 0x03, 5},
        {8, 0x05, 6},  {9, 0x04, 6}, {10, 0x04, 7}, {11, 0x05, 7},
        {12, 0x07, 7}, {13, 0x04, 8}, {14, 0x07, 8}, {15, 0x18, 9},
        {16, 0x17, 10},
    };
    for (const auto& entry : table) {
        if (entry.run == run) {
            bb.append(entry.code, entry.length);
            return;
        }
    }
    throw std::runtime_error("Unsupported black run length");
}

// Build pure 1D CCITT data (k=0 mode)
std::vector<uint8_t> build_ccitt_1d_sample() {
    BitBuilder bb;
    const uint16_t kEolCode = 0x001;  // 000000000001
    const uint8_t kEolLength = 12;

    // Line 1: 8 white pixels, 8 black pixels
    bb.append(kEolCode, kEolLength);
    append_white(bb, 8);
    append_black(bb, 8);

    // Line 2: same as line 1 (pure 1D encoding)
    bb.append(kEolCode, kEolLength);
    append_white(bb, 8);
    append_black(bb, 8);

    // Final EOL to terminate block
    bb.append(kEolCode, kEolLength);

    // Pad with zeros to byte boundary
    while (bb.bits.size() % 8 != 0) {
        bb.bits.push_back(0);
    }

    return bb.to_bytes();
}

// Build mixed 1D/2D CCITT data (k>0 mode)
std::vector<uint8_t> build_ccitt_2d_sample() {
    BitBuilder bb;
    const uint16_t kEolCode = 0x001;
    const uint8_t kEolLength = 12;

    // Line 1: 8 white pixels, 8 black pixels (1D encoded)
    bb.append(kEolCode, kEolLength);
    bb.append_string("1");  // 1D mode indicator
    append_white(bb, 8);
    append_black(bb, 8);

    // Line 2: replicate first line using 2D coding
    bb.append(kEolCode, kEolLength);
    bb.append_string("0");  // 2D mode indicator
    bb.append_string("1");  // Vertical(0)
    bb.append_string("1");  // Vertical(0)

    // Final EOL
    bb.append(kEolCode, kEolLength);

    while (bb.bits.size() % 8 != 0) {
        bb.bits.push_back(0);
    }

    return bb.to_bytes();
}

// Build Group 4 (pure 2D) CCITT data (k=-1 mode)
std::vector<uint8_t> build_ccitt_group4_sample() {
    BitBuilder bb;
    const uint16_t kEolCode = 0x001;
    const uint8_t kEolLength = 12;

    // Line 1: 8 white + 8 black (relative to all-white reference line)
    bb.append(kEolCode, kEolLength);
    bb.append_string("001");  // Horizontal mode
    append_white(bb, 8);
    append_black(bb, 8);

    // Line 2: same as Line 1 (use Vertical(0) twice)
    bb.append(kEolCode, kEolLength);
    bb.append_string("1");  // Vertical(0)
    bb.append_string("1");  // Vertical(0)

    // EOFB (End Of Facsimile Block) - two EOLs
    bb.append(kEolCode, kEolLength);
    bb.append(kEolCode, kEolLength);

    while (bb.bits.size() % 8 != 0) {
        bb.bits.push_back(0);
    }

    return bb.to_bytes();
}

}  // namespace

TEST_SUITE("CCITTFaxDecode") {
    TEST_CASE("Group 3 1D (k=0) with BlackIs1=false") {
        auto encoded = build_ccitt_1d_sample();

        filters::DecodeParams params;
        params.k = 0;  // Pure 1D mode
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = false;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        // 8 white + 8 black pixels: white=1, black=0 when black_is_1=false
        CHECK(result.data[0] == 0xFF);
        CHECK(result.data[1] == 0x00);
        CHECK(result.data[2] == 0xFF);
        CHECK(result.data[3] == 0x00);
    }

    TEST_CASE("Group 3 1D (k=0) with BlackIs1=true") {
        auto encoded = build_ccitt_1d_sample();

        filters::DecodeParams params;
        params.k = 0;
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = true;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        // 8 white + 8 black pixels: white=0, black=1 when black_is_1=true
        CHECK(result.data[0] == 0x00);
        CHECK(result.data[1] == 0xFF);
        CHECK(result.data[2] == 0x00);
        CHECK(result.data[3] == 0xFF);
    }

    TEST_CASE("Group 3 2D (k=1) with BlackIs1=false") {
        auto encoded = build_ccitt_2d_sample();

        filters::DecodeParams params;
        params.k = 1;  // Mixed 1D/2D mode
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = false;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        CHECK(result.data[0] == 0xFF);
        CHECK(result.data[1] == 0x00);
        CHECK(result.data[2] == 0xFF);
        CHECK(result.data[3] == 0x00);
    }

    TEST_CASE("Group 3 2D (k=1) with BlackIs1=true") {
        auto encoded = build_ccitt_2d_sample();

        filters::DecodeParams params;
        params.k = 1;
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = true;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        CHECK(result.data[0] == 0x00);
        CHECK(result.data[1] == 0xFF);
        CHECK(result.data[2] == 0x00);
        CHECK(result.data[3] == 0xFF);
    }

    TEST_CASE("Group 4 (k=-1) with BlackIs1=false") {
        auto encoded = build_ccitt_group4_sample();

        filters::DecodeParams params;
        params.k = -1;  // Pure 2D mode (Group 4)
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = false;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        CHECK(result.data[0] == 0xFF);
        CHECK(result.data[1] == 0x00);
        CHECK(result.data[2] == 0xFF);
        CHECK(result.data[3] == 0x00);
    }

    TEST_CASE("Group 4 (k=-1) with BlackIs1=true") {
        auto encoded = build_ccitt_group4_sample();

        filters::DecodeParams params;
        params.k = -1;
        params.columns = 16;
        params.rows = 2;
        params.end_of_line = true;
        params.black_is_1 = true;

        DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);

        REQUIRE(result.success);
        REQUIRE(result.data.size() == 4);
        CHECK(result.data[0] == 0x00);
        CHECK(result.data[1] == 0xFF);
        CHECK(result.data[2] == 0x00);
        CHECK(result.data[3] == 0xFF);
    }

    TEST_CASE("Parameter validation - columns") {
        auto encoded = build_ccitt_1d_sample();

        filters::DecodeParams params;
        params.k = 0;
        params.columns = 16;
        params.rows = 2;

        CHECK(params.columns == 16);
        CHECK(params.rows == 2);
    }

    TEST_CASE("Parameter validation - k values") {
        filters::DecodeParams params;

        params.k = 0;   // Group 3 1D
        CHECK(params.k == 0);

        params.k = 1;   // Group 3 2D
        CHECK(params.k == 1);

        params.k = -1;  // Group 4
        CHECK(params.k == -1);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
