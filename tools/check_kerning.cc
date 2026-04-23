#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <font.ttf>" << std::endl;
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open font file: " << argv[1] << std::endl;
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> font_data(size);
    if (!file.read(reinterpret_cast<char*>(font_data.data()), size)) {
        std::cerr << "Failed to read font file" << std::endl;
        return 1;
    }

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, font_data.data(), 0)) {
        std::cerr << "Failed to init font" << std::endl;
        return 1;
    }

    int x0, y0, x1, y1;
    stbtt_GetFontBoundingBox(&info, &x0, &y0, &x1, &y1);
    std::cout << "Font bounding box: " << x0 << "," << y0 << " to " << x1 << "," << y1 << std::endl;

    int num_kern_pairs = 0;
    // Check if kern table exists
    if (stbtt_GetKerningTable(&info, nullptr, 0) > 0) {
        std::cout << "Font has kerning table" << std::endl;

        // Get kerning pairs
        int num_pairs = stbtt_GetKerningTable(&info, nullptr, 0);
        std::vector<stbtt_kerningentry> entries(num_pairs);
        stbtt_GetKerningTable(&info, entries.data(), num_pairs);

        std::cout << "Number of kerning pairs: " << num_pairs << std::endl;

        // Print first few kerning pairs
        int print_count = 0;
        for (const auto& e : entries) {
            std::cout << "  Kern: glyph" << e.glyph1 << " + glyph" << e.glyph2
                      << " = " << e.advance << std::endl;
            if (++print_count >= 20) {
                std::cout << "  ... (showing first 20)" << std::endl;
                break;
            }
        }
    } else {
        std::cout << "Font does NOT have kerning table" << std::endl;
    }

    // Test some specific character pairs
    std::cout << "\nTesting specific character pairs:" << std::endl;
    const char* test_pairs[] = {"AV", "To", "We", "AT", "TY", "AV", nullptr};
    for (int i = 0; test_pairs[i] != nullptr; i += 2) {
        int ch1 = static_cast<unsigned char>(test_pairs[i][0]);
        int ch2 = static_cast<unsigned char>(test_pairs[i + 1][0]);
        int gid1 = stbtt_FindGlyphIndex(&info, ch1);
        int gid2 = stbtt_FindGlyphIndex(&info, ch2);
        int kern = stbtt_GetGlyphKernAdvance(&info, gid1, gid2);
        int codepoint_kern = stbtt_GetCodepointKernAdvance(&info, ch1, ch2);
        std::cout << "  '" << test_pairs[i] << "'->'" << test_pairs[i+1] << "': "
                  << "glyphs=" << gid1 << "," << gid2
                  << " kern=" << kern << " codepoint_kern=" << codepoint_kern << std::endl;
    }

    return 0;
}