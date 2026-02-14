# Font Embedding System

This document describes the font embedding system implemented for nanopdf to support WASM and standalone builds.

## Overview

The font embedding system allows PDF Standard 14 alternative fonts to be compiled directly into the binary, eliminating the need for external font files at runtime. This is particularly useful for:

- **WASM builds**: Where file system access is limited
- **Standalone executables**: Like the rasterize tool that need bundled fonts
- **Embedded systems**: Where deploying font files separately is impractical

## Implementation

### Files

- `cmake/embed_fonts.py` - Python script that generates C++ headers with embedded font data
- `cmake/embed_fonts.cmake` - CMake wrapper that invokes the Python script
- `fonts/` - Directory containing the open-source font files
- `fonts/README.md` - Documentation of font alternatives
- `fonts/USAGE.md` - API usage documentation

### Build System Integration

#### Main Library (CMakeLists.txt)

Font embedding is controlled by the `NANOPDF_EMBED_FONTS` option:

```cmake
option(NANOPDF_EMBED_FONTS "Embed PDF Standard 14 alternative fonts as binary data" OFF)
```

For WASM builds, this is automatically enabled:

```cmake
if(NANOPDF_BUILD_WASM)
  set(NANOPDF_EMBED_FONTS ON CACHE BOOL "Embed fonts for WASM build" FORCE)
endif()
```

When enabled, the build system:

1. Finds all `.ttf` and `.otf` files in the `fonts/` directory
2. Generates `embedded-fonts.hh` in the build directory
3. Adds the build directory to include paths
4. Defines `NANOPDF_EMBED_FONTS=1` macro

#### Rasterize Example (examples/rasterize/CMakeLists.txt)

The rasterize tool has independent font embedding enabled by default:

```cmake
option(EMBED_FONTS "Embed PDF Standard 14 alternative fonts as binary data" ON)
```

This allows the rasterize tool to have embedded fonts even if the main library doesn't.

## Generated Header Structure

The generated `embedded-fonts.hh` contains:

```cpp
namespace nanopdf {
namespace embedded_fonts {

// Font data arrays (one per font file)
static const unsigned char arimo_regular_data[] = { /* binary data */ };
static const size_t arimo_regular_size = 308080;
static const char* arimo_regular_name = "Arimo-Regular.ttf";

// Font registry
struct FontEntry {
  const char* base_name;        // Base name (e.g., "Arimo-Regular")
  const unsigned char* data;    // Font binary data
  size_t size;                  // Data size in bytes
  const char* filename;         // Original filename
};

static const FontEntry font_registry[] = { /* all fonts */ };
static const size_t font_count = 14;

// Helper functions
const FontEntry* find_font(const char* name);
const FontEntry* get_pdf_standard_font(const char* pdf_name);

// PDF Standard 14 mapping
struct FontMapping {
  const char* pdf_name;         // PDF Standard 14 name
  const char* substitute_name;  // Substitute font base name
};

static const FontMapping pdf_standard_14_mapping[] = { /* mappings */ };

}  // namespace embedded_fonts
}  // namespace nanopdf
```

## Usage in Code

### Basic Usage

```cpp
#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"

// Find a PDF Standard 14 substitute font
const auto* font = nanopdf::embedded_fonts::get_pdf_standard_font("Helvetica-Bold");
if (font) {
  // Use font->data and font->size
  // Example: load with stb_truetype
  stbtt_fontinfo fontinfo;
  if (stbtt_InitFont(&fontinfo, font->data, 0)) {
    // Font loaded successfully
  }
}
#endif
```

### Listing Available Fonts

```cpp
#ifdef NANOPDF_EMBED_FONTS
for (size_t i = 0; i < nanopdf::embedded_fonts::font_count; ++i) {
  const auto& entry = nanopdf::embedded_fonts::font_registry[i];
  std::cout << entry.filename << " (" << entry.size << " bytes)\n";
}
#endif
```

### Finding Fonts by Name

```cpp
#ifdef NANOPDF_EMBED_FONTS
// Case-insensitive search
const auto* font = nanopdf::embedded_fonts::find_font("arimo-bold");
if (font) {
  // Use font data
}
#endif
```

## Performance

### Generation Speed

- **Python script**: Processes all 14 fonts (~3.4 MB) in < 2 seconds
- **CMake-only approach**: Would take > 2 minutes (too slow)

The Python-based implementation is ~60x faster than pure CMake string processing.

### Binary Size Impact

| Build Type | Without Fonts | With Fonts | Increase |
|-----------|---------------|------------|----------|
| WASM (.wasm) | ~500 KB | ~3.9 MB | +3.4 MB |
| Static lib (.a) | ~2 MB | ~5.4 MB | +3.4 MB |
| Rasterize (exe) | ~2.5 MB | ~5.9 MB | +3.4 MB |

Note: Linker optimization (`-Wl,--gc-sections`) can remove unused fonts if individual font arrays are referenced.

### Runtime Performance

- **Zero overhead**: Fonts are in read-only data segment
- **No I/O**: No file system access required
- **Instant access**: Direct pointer to font data

## Cross-Platform Compatibility

The implementation is fully portable across:

- **Linux**: ✓ Tested
- **macOS**: ✓ Compatible (requires Python3)
- **Windows**: ✓ Compatible (requires Python3)
- **Emscripten/WASM**: ✓ Tested

### Requirements

- CMake 3.16+
- Python 3.6+
- C++14 compiler

## Build Examples

### Build with embedded fonts

```bash
# Main library with fonts
mkdir build && cd build
cmake -DNANOPDF_EMBED_FONTS=ON ..
make

# WASM build (fonts auto-enabled)
./scripts/bootstrap-emscripten.sh
cd build && make

# Rasterize with fonts (default)
cd examples/rasterize
mkdir build && cd build
cmake ..
make
```

### Build without embedded fonts

```bash
# Main library without fonts
cmake -DNANOPDF_EMBED_FONTS=OFF ..

# Rasterize without fonts
cd examples/rasterize/build
cmake -DEMBED_FONTS=OFF ..
```

## Font Mapping

All 14 PDF Standard fonts are mapped to open-source alternatives:

| PDF Standard | Alternative | License | Size |
|-------------|-------------|---------|------|
| Helvetica | Arimo-Regular | Apache 2.0 | 308 KB |
| Helvetica-Bold | Arimo-Bold | Apache 2.0 | 310 KB |
| Helvetica-Oblique | Arimo-Italic | Apache 2.0 | 330 KB |
| Helvetica-BoldOblique | Arimo-BoldItalic | Apache 2.0 | 332 KB |
| Times-Roman | Tinos-Regular | Apache 2.0 | 464 KB |
| Times-Bold | Tinos-Bold | Apache 2.0 | 441 KB |
| Times-Italic | Tinos-Italic | Apache 2.0 | 445 KB |
| Times-BoldItalic | Tinos-BoldItalic | Apache 2.0 | 446 KB |
| Courier | Cousine-Regular | Apache 2.0 | 301 KB |
| Courier-Bold | Cousine-Bold | Apache 2.0 | 290 KB |
| Courier-Oblique | Cousine-Italic | Apache 2.0 | 264 KB |
| Courier-BoldOblique | Cousine-BoldItalic | Apache 2.0 | 266 KB |
| Symbol | STIXTwoMath-Regular | SIL OFL 1.1 | 819 KB |
| ZapfDingbats | NotoSansSymbols-Regular | SIL OFL 1.1 | 291 KB |

**Total**: 14 fonts, ~3.4 MB

## License Compatibility

All embedded fonts use permissive open-source licenses:

- **Apache License 2.0**: Arimo, Tinos, Cousine (12 fonts)
- **SIL Open Font License 1.1**: STIX Two Math, Noto Sans Symbols (2 fonts)

Both licenses are compatible with commercial use and redistribution.

## Future Enhancements

Potential improvements:

1. **Selective embedding**: Only embed fonts actually used
2. **Compression**: Use zlib/gzip compression on font data
3. **Lazy loading**: For WASM, load fonts on-demand
4. **Font subsetting**: Include only needed glyphs (major size reduction)
5. **Alternative generators**: Support xxd, bin2c as Python alternatives

## Troubleshooting

### Python not found

```
CMake Error: Python3 not found
```

**Solution**: Install Python 3.6+ and ensure it's in PATH.

### Fonts not found

```
Error: No font files found in fonts/
```

**Solution**: Ensure fonts are downloaded to the `fonts/` directory.

### Generated header too large

The ~32 MB generated header is normal. It compiles quickly and the resulting binary is only ~3.4 MB larger.

### Linker errors about duplicate symbols

If you get duplicate symbol errors with `embedded_fonts`, ensure you're not including the header in multiple translation units without proper guards, or make the symbols `static inline`.

## Technical Details

### Why Python instead of pure CMake?

CMake's string processing is extremely slow for large binary files:

- CMake loops: ~2-5 minutes for 3.4 MB
- Python: < 2 seconds for 3.4 MB

### Why not use xxd?

While `xxd -i` is fast, it's not available on all platforms (especially Windows). Python 3 is more universally available and provides better cross-platform compatibility.

### Data Format

Fonts are stored as:
- C arrays: `static const unsigned char font_data[]`
- Hex bytes: `0x00, 0x01, 0x02, ...`
- 16 bytes per line for readability

This format is:
- Portable across compilers
- Optimizable by the linker
- Compatible with C++14
- Zero runtime cost

## See Also

- `fonts/README.md` - Font family documentation
- `fonts/USAGE.md` - Detailed API reference
- `CLAUDE.md` - Project build instructions
