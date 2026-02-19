# PDF Standard 14 Fonts - Permissive License Alternatives

This directory contains open-source, permissive-licensed alternatives to the PDF Standard 14 fonts. These fonts are metrics-compatible with their PDF standard counterparts, making them suitable for PDF rendering and text substitution.

When embedded in builds, fonts are **compressed with zlib** (level 9) to reduce binary size by approximately **41%**.

## Font Families

### Arimo (Helvetica Alternative)
- **License**: Apache License 2.0
- **Replaces**: Helvetica, Helvetica-Bold, Helvetica-Oblique, Helvetica-BoldOblique
- **Features**: Metrics-compatible with Helvetica
- **Original Size**: 1.28 MB → **Compressed**: 0.75 MB (41.4% reduction)
- **Files**:
  - `arimo/Arimo-Regular.ttf` (308 KB → 181 KB compressed)
  - `arimo/Arimo-Bold.ttf` (310 KB → 182 KB compressed)
  - `arimo/Arimo-Italic.ttf` (330 KB → 195 KB compressed)
  - `arimo/Arimo-BoldItalic.ttf` (332 KB → 195 KB compressed)

### Tinos (Times Alternative)
- **License**: Apache License 2.0
- **Replaces**: Times-Roman, Times-Bold, Times-Italic, Times-BoldItalic
- **Features**: Metrics-compatible with Times New Roman
- **Original Size**: 1.79 MB → **Compressed**: 0.97 MB (45.8% reduction)
- **Files**:
  - `tinos/Tinos-Regular.ttf` (464 KB → 248 KB compressed)
  - `tinos/Tinos-Bold.ttf` (441 KB → 239 KB compressed)
  - `tinos/Tinos-Italic.ttf` (445 KB → 245 KB compressed)
  - `tinos/Tinos-BoldItalic.ttf` (447 KB → 247 KB compressed)

### Cousine (Courier Alternative)
- **License**: Apache License 2.0
- **Replaces**: Courier, Courier-Bold, Courier-Oblique, Courier-BoldOblique
- **Features**: Metrics-compatible with Courier New (monospace)
- **Original Size**: 1.12 MB → **Compressed**: 0.66 MB (41.1% reduction)
- **Files**:
  - `cousine/Cousine-Regular.ttf` (301 KB → 173 KB compressed)
  - `cousine/Cousine-Bold.ttf` (290 KB → 169 KB compressed)
  - `cousine/Cousine-Italic.ttf` (264 KB → 158 KB compressed)
  - `cousine/Cousine-BoldItalic.ttf` (267 KB → 160 KB compressed)

### STIX Two Math (Symbol Alternative)
- **License**: SIL Open Font License 1.1
- **Replaces**: Symbol
- **Features**: Comprehensive mathematical symbols and special characters
- **Original Size**: 819 KB → **Compressed**: 629 KB (23.2% reduction)
- **Files**:
  - `stix/STIXTwoMath-Regular.otf`

### Noto Sans Symbols (ZapfDingbats Alternative)
- **License**: SIL Open Font License 1.1
- **Replaces**: ZapfDingbats
- **Features**: Wide coverage of decorative symbols and dingbats
- **Original Size**: 291 KB → **Compressed**: 121 KB (58.5% reduction)
- **Files**:
  - `noto-symbols/NotoSansSymbols-Regular.ttf`

### Noto Sans JP (CJK Sans-Serif / Gothic)
- **License**: SIL Open Font License 1.1
- **Purpose**: Japanese CJK fallback font (sans-serif / Gothic style)
- **Source**: [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk) Sans2.004
- **Features**: Full Japanese character coverage including kanji, hiragana, katakana
- **Total Size**: ~31 MB (7 weights)
- **Files** (OpenType-CFF format):
  - `noto-sans-jp/NotoSansJP-Thin.otf` (4.0 MB)
  - `noto-sans-jp/NotoSansJP-Light.otf` (4.3 MB)
  - `noto-sans-jp/NotoSansJP-DemiLight.otf` (4.3 MB)
  - `noto-sans-jp/NotoSansJP-Regular.otf` (4.3 MB)
  - `noto-sans-jp/NotoSansJP-Medium.otf` (4.3 MB)
  - `noto-sans-jp/NotoSansJP-Bold.otf` (4.4 MB)
  - `noto-sans-jp/NotoSansJP-Black.otf` (4.6 MB)

### Noto Serif JP (CJK Serif / Mincho)
- **License**: SIL Open Font License 1.1
- **Purpose**: Japanese CJK fallback font (serif / Mincho style)
- **Source**: [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk) Serif2.003 (SubsetOTF)
- **Features**: Full Japanese character coverage in serif style
- **Total Size**: ~43 MB (7 weights)
- **Files** (OpenType-CFF format):
  - `noto-serif-jp/NotoSerifJP-ExtraLight.otf` (5.4 MB)
  - `noto-serif-jp/NotoSerifJP-Light.otf` (5.9 MB)
  - `noto-serif-jp/NotoSerifJP-Regular.otf` (5.9 MB)
  - `noto-serif-jp/NotoSerifJP-Medium.otf` (6.0 MB)
  - `noto-serif-jp/NotoSerifJP-SemiBold.otf` (6.0 MB)
  - `noto-serif-jp/NotoSerifJP-Bold.otf` (6.2 MB)
  - `noto-serif-jp/NotoSerifJP-Black.otf` (6.0 MB)

## Compression Summary

When embedded in binaries with `NANOPDF_EMBED_FONTS=ON`, all fonts are compressed:

| Metric | Value |
|--------|-------|
| Total original size | 5.18 MB |
| Total compressed size | **3.06 MB** |
| Space saved | 2.12 MB |
| **Compression ratio** | **40.9%** |

**Binary size impact**: +3.06 MB (instead of +5.18 MB without compression)

## License Information

All fonts in this directory are licensed under permissive open-source licenses:
- **Apache License 2.0**: Arimo, Tinos, Cousine (12 fonts)
- **SIL Open Font License 1.1**: STIX Two Math, Noto Sans Symbols, Noto Sans JP, Noto Serif JP (16 fonts)

Both licenses allow:
- ✓ Commercial use
- ✓ Modification
- ✓ Distribution
- ✓ Binary redistribution without attribution

See individual `LICENSE.txt` or `OFL.txt` files in each subdirectory for full license terms.

## Sources

- **Arimo, Tinos, Cousine**: Google Fonts (https://fonts.google.com/)
- **STIX Two Math**: STI Pub (https://github.com/stipub/stixfonts)
- **Noto Sans Symbols**: Google Noto Fonts (https://github.com/notofonts/symbols)
- **Noto Sans JP, Noto Serif JP**: Google Noto CJK (https://github.com/notofonts/noto-cjk)

## Usage in nanopdf

These fonts can be used as substitutes when the PDF Standard 14 fonts are not available on the system. The metrics compatibility ensures that text layout and positioning remain accurate when rendering PDFs.

### Embedding in Builds

Fonts are automatically embedded and compressed in WASM builds. For other builds:

```bash
cmake -DNANOPDF_EMBED_FONTS=ON ..
make
```

### Runtime Decompression

Embedded fonts are stored compressed and decompressed on-demand:

```cpp
#ifdef NANOPDF_EMBED_FONTS
const auto* font = nanopdf::embedded_fonts::get_pdf_standard_font("Helvetica");
if (font) {
  std::vector<uint8_t> decompressed;
  if (nanopdf::embedded_fonts::decompress_font(font, decompressed)) {
    // Use decompressed.data() and decompressed.size()
  }
}
#endif
```

Decompression is fast (~1-2ms per font) and only done when fonts are actually used.

## PDF Standard 14 Font Mapping

| PDF Standard Font | Alternative Font | Family | Style | Compressed Size |
|------------------|------------------|--------|-------|-----------------|
| Helvetica | Arimo-Regular | Arimo | Regular | 181 KB |
| Helvetica-Bold | Arimo-Bold | Arimo | Bold | 182 KB |
| Helvetica-Oblique | Arimo-Italic | Arimo | Italic | 195 KB |
| Helvetica-BoldOblique | Arimo-BoldItalic | Arimo | Bold Italic | 195 KB |
| Times-Roman | Tinos-Regular | Tinos | Regular | 248 KB |
| Times-Bold | Tinos-Bold | Tinos | Bold | 239 KB |
| Times-Italic | Tinos-Italic | Tinos | Italic | 245 KB |
| Times-BoldItalic | Tinos-BoldItalic | Tinos | Bold Italic | 247 KB |
| Courier | Cousine-Regular | Cousine | Regular | 173 KB |
| Courier-Bold | Cousine-Bold | Cousine | Bold | 169 KB |
| Courier-Oblique | Cousine-Italic | Cousine | Italic | 158 KB |
| Courier-BoldOblique | Cousine-BoldItalic | Cousine | Bold Italic | 160 KB |
| Symbol | STIXTwoMath-Regular | STIX | Regular | 629 KB |
| ZapfDingbats | NotoSansSymbols-Regular | Noto | Regular | 121 KB |

## File Format

- `.ttf` - TrueType Font format
- `.otf` - OpenType Font format (used for STIX Two Math due to advanced math layout features)

Both formats are widely supported and can be loaded using standard font libraries like FreeType or stb_truetype.

## Documentation

- **USAGE.md** - Complete API reference and usage examples
- **QUICK_REFERENCE.md** - Quick reference card
- **../FONTS_EMBEDDING.md** - Technical implementation details

## Performance

### Generation Time
- Font compression: < 2 seconds for all 14 fonts
- Uses Python with zlib (level 9 compression)

### Runtime Performance
- Decompression: ~1-2ms per font on typical hardware
- Memory: Temporary allocation of original size during decompression
- Thread-safe: Read-only compressed data

### Binary Size
- Compressed: **+3.06 MB** to binary
- Uncompressed: +5.18 MB to binary
- **Savings: 2.12 MB (40.9%)**
