#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <font.ttf> <char1> <char2> ..." << std::endl;
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

    std::cout << "Font info:" << std::endl;
    std::cout << "  unitsPerEm: " << info.unitsPerEm << std::endl;

    // Get kerning table
    int num_kern = stbtt_GetKerningTable(&info, nullptr, 0);
    std::cout << "  Kerning entries: " << num_kern << std::endl;

    if (num_kern > 0) {
        std::vector<stbtt_kerningentry> entries(num_kern);
        stbtt_GetKerningTable(&info, entries.data(), num_kern);

        // Print first 10 entries
        int count = 0;
        for (const auto& e : entries) {
            std::cout << "  Kern: g" << e.glyph1 << " + g" << e.glyph2
                      << " = " << e.advance << std::endl;
            if (++count >= 10) break;
        }
    }

    // Test character pairs from command line
    std::cout << "\nTesting pairs from command line:" << std::endl;
    for (int i = 2; i + 1 < argc; i += 2) {
        int ch1 = static_cast<unsigned char>(argv[i][0]);
        int ch2 = static_cast<unsigned char>(argv[i + 1][0]);
        int gid1 = stbtt_FindGlyphIndex(&info, ch1);
        int gid2 = stbtt_FindGlyphIndex(&info, ch2);
        int kern_glyph = stbtt_GetGlyphKernAdvance(&info, gid1, gid2);
        int kern_codepoint = stbtt_GetCodepointKernAdvance(&info, ch1, ch2);

        // Also get advance widths
        int adv1, lb1;
        stbtt_GetCodepointHMetrics(&info, ch1, &adv1, &lb1);
        int adv2, lb2;
        stbtt_GetCodepointHMetrics(&info, ch2, &adv2, &lb2);

        std::cout << "  '" << argv[i] << "'->'" << argv[i+1] << "':"
                  << " chars=" << ch1 << "," << ch2
                  << " glyphs=" << gid1 << "," << gid2
                  << " adv=" << adv1 << "," << adv2
                  << " kern_glyph=" << kern_glyph
                  << " kern_cp=" << kern_codepoint
                  << std::endl;
    }

    return 0;
}