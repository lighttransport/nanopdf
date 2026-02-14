# Font Embedding Quick Reference

## Build Options

### Enable Font Embedding

```bash
# Main library
cmake -DNANOPDF_EMBED_FONTS=ON ..

# Rasterize (ON by default)
cd examples/rasterize/build
cmake -DEMBED_FONTS=ON ..

# WASM (auto-enabled)
./scripts/bootstrap-emscripten.sh
```

### Disable Font Embedding

```bash
# Main library
cmake -DNANOPDF_EMBED_FONTS=OFF ..

# Rasterize
cmake -DEMBED_FONTS=OFF ..
```

## C++ API

### Include Header

```cpp
#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#endif
```

### Get PDF Standard 14 Font (with Decompression)

```cpp
const auto* font = nanopdf::embedded_fonts::get_pdf_standard_font("Helvetica-Bold");
if (font) {
  // Fonts are stored compressed - decompress to use
  std::vector<uint8_t> decompressed;
  if (nanopdf::embedded_fonts::decompress_font(font, decompressed)) {
    // Use decompressed.data() and decompressed.size()
    const unsigned char* data = decompressed.data();
    size_t size = decompressed.size();  // font->original_size
  }

  // Or check compressed stats
  size_t compressed = font->compressed_size;
  size_t original = font->original_size;
  const char* name = font->filename;  // "Arimo-Bold.ttf"
}
```

### Find Font by Name (with Decompression)

```cpp
const auto* font = nanopdf::embedded_fonts::find_font("Arimo-Regular");
// Case-insensitive: "arimo-regular" also works

if (font) {
  std::vector<uint8_t> decompressed;
  nanopdf::embedded_fonts::decompress_font(font, decompressed);
  // Use decompressed font data
}
```

### List All Fonts (with Compression Stats)

```cpp
for (size_t i = 0; i < nanopdf::embedded_fonts::font_count; ++i) {
  const auto& f = nanopdf::embedded_fonts::font_registry[i];
  float ratio = 100.0f * (1.0f - (float)f.compressed_size / f.original_size);
  printf("%s: %zu bytes (compressed: %zu, %.1f%% reduction)\n",
         f.filename, f.original_size, f.compressed_size, ratio);
}
```

## PDF Standard 14 Mapping

| PDF Font | Substitute |
|----------|------------|
| Helvetica | Arimo-Regular |
| Helvetica-Bold | Arimo-Bold |
| Helvetica-Oblique | Arimo-Italic |
| Helvetica-BoldOblique | Arimo-BoldItalic |
| Times-Roman | Tinos-Regular |
| Times-Bold | Tinos-Bold |
| Times-Italic | Tinos-Italic |
| Times-BoldItalic | Tinos-BoldItalic |
| Courier | Cousine-Regular |
| Courier-Bold | Cousine-Bold |
| Courier-Oblique | Cousine-Italic |
| Courier-BoldOblique | Cousine-BoldItalic |
| Symbol | STIXTwoMath-Regular |
| ZapfDingbats | NotoSansSymbols-Regular |

## CMake Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NANOPDF_EMBED_FONTS` | OFF | Enable for main library |
| `EMBED_FONTS` | ON | Enable for rasterize |

Auto-enabled for:
- WASM builds (`NANOPDF_BUILD_WASM=ON`)

## Requirements

- CMake 3.16+
- Python 3.6+
- C++14 compiler

## Size Impact (with Compression)

| Component | Size |
|-----------|------|
| Font files (original) | 5.18 MB |
| Font files (compressed) | **3.06 MB** |
| Generated header (source) | ~19 MB |
| Binary increase | **+3.06 MB** |
| **Space saved** | **2.12 MB (40.9%)** |

## Files Generated

```
build/
└── embedded-fonts.hh      # Main library

examples/rasterize/build/
└── embedded-fonts.hh      # Rasterize standalone
```

**Note**: Generated files are in `.gitignore` and rebuilt automatically.

## Common Issues

### Python not found
```
CMake Error: Python3 not found
```
**Solution**: Install Python 3.6+

### No fonts found
```
Error: No font files found in fonts/
```
**Solution**: Ensure fonts are in `fonts/` directory

### Header too large
The ~32 MB source header is normal. It compiles to only +3.4 MB binary size.

## Example: Load with stb_truetype (Decompressed)

```cpp
#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#include <stb_truetype.h>

const auto* font_entry =
  nanopdf::embedded_fonts::get_pdf_standard_font("Helvetica");

if (font_entry) {
  // Decompress the font data
  std::vector<uint8_t> decompressed;
  if (nanopdf::embedded_fonts::decompress_font(font_entry, decompressed)) {
    // Load with stb_truetype
    stbtt_fontinfo font;
    if (stbtt_InitFont(&font, decompressed.data(), 0)) {
      // Font loaded successfully
      float scale = stbtt_ScaleForPixelHeight(&font, 32.0f);
      // ... use font (keep decompressed buffer alive!)
    }
  }
}
#endif
```

## Documentation

- **fonts/README.md** - Font details and licenses
- **fonts/USAGE.md** - Complete API reference
- **FONTS_EMBEDDING.md** - Technical documentation
- **IMPLEMENTATION_SUMMARY.md** - Implementation overview

## License

All fonts use permissive open-source licenses:
- **Apache 2.0**: Arimo, Tinos, Cousine
- **SIL OFL 1.1**: STIX Two Math, Noto Sans Symbols

Both allow commercial use, modification, and redistribution.
