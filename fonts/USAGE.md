# Embedded Fonts Usage (with zlib Compression)

This document explains how to use the embedded PDF Standard 14 alternative fonts in nanopdf. Fonts are compressed with zlib to reduce binary size by ~41%.

## CMake Configuration

### For Main Library (WASM builds)

Fonts are automatically embedded for WASM builds. To manually enable for other builds:

```cmake
cmake -DNANOPDF_EMBED_FONTS=ON ..
```

### For Rasterize Example

Fonts are embedded by default. To disable:

```cmake
cd examples/rasterize/build
cmake -DEMBED_FONTS=OFF ..
```

## C++ Usage

When `NANOPDF_EMBED_FONTS` is defined, you can access the embedded fonts:

```cpp
#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#endif

// Example 1: Find and decompress a PDF Standard 14 font
#ifdef NANOPDF_EMBED_FONTS
const auto* font_entry = nanopdf::embedded_fonts::get_pdf_standard_font("Helvetica-Bold");
if (font_entry) {
  std::cout << "Found font: " << font_entry->filename << std::endl;
  std::cout << "Compressed size: " << font_entry->compressed_size << " bytes" << std::endl;
  std::cout << "Original size: " << font_entry->original_size << " bytes" << std::endl;

  // Decompress the font
  std::vector<uint8_t> decompressed_data;
  if (nanopdf::embedded_fonts::decompress_font(font_entry, decompressed_data)) {
    // Font successfully decompressed, use decompressed_data
    stbtt_fontinfo font;
    if (stbtt_InitFont(&font, decompressed_data.data(), 0)) {
      // Font loaded successfully
    }
  }
}
#endif

// Example 2: Decompress to a pre-allocated buffer (C-style)
#ifdef NANOPDF_EMBED_FONTS
const auto* font_entry = nanopdf::embedded_fonts::find_font("Arimo-Regular");
if (font_entry) {
  unsigned char* buffer = new unsigned char[font_entry->original_size];

  if (nanopdf::embedded_fonts::decompress_font_to_buffer(font_entry, buffer, font_entry->original_size)) {
    // Use buffer (original_size bytes of uncompressed font data)
    use_font(buffer, font_entry->original_size);
  }

  delete[] buffer;
}
#endif

// Example 3: List all available fonts with compression stats
#ifdef NANOPDF_EMBED_FONTS
std::cout << "Available embedded fonts:\n";
for (size_t i = 0; i < nanopdf::embedded_fonts::font_count; ++i) {
  const auto& entry = nanopdf::embedded_fonts::font_registry[i];
  float ratio = 100.0f * (1.0f - (float)entry.compressed_size / entry.original_size);
  std::cout << "  " << entry.filename
            << " (original: " << entry.original_size
            << ", compressed: " << entry.compressed_size
            << ", " << std::fixed << std::setprecision(1) << ratio << "% reduction)\n";
}
#endif
```

## Compression Details

### Compression Statistics

- **Algorithm**: zlib with maximum compression (level 9)
- **Average reduction**: ~41% across all fonts
- **Binary size**: Compressed fonts add ~3.1 MB instead of ~5.2 MB

| Font Family | Original Size | Compressed Size | Reduction |
|-------------|---------------|-----------------|-----------|
| Arimo (4)   | 1.28 MB      | 0.75 MB        | 41.4%     |
| Tinos (4)   | 1.79 MB      | 0.97 MB        | 45.8%     |
| Cousine (4) | 1.12 MB      | 0.66 MB        | 41.1%     |
| STIX Math   | 0.82 MB      | 0.63 MB        | 23.2%     |
| Noto Symbols| 0.29 MB      | 0.12 MB        | 58.5%     |
| **Total**   | **5.18 MB**  | **3.06 MB**    | **40.9%** |

### Runtime Decompression

- **Performance**: Decompression is fast (~1-2ms per font on typical hardware)
- **Memory**: Temporary allocation of uncompressed size during decompression
- **Thread-safe**: Read-only compressed data, decompression creates new buffers

## Font Mapping

The following PDF Standard 14 fonts are mapped to open-source alternatives:

| PDF Standard Font     | Substitute Font        | Original | Compressed | License      |
|-----------------------|------------------------|----------|------------|--------------|
| Helvetica             | Arimo-Regular          | 308 KB   | 181 KB     | Apache 2.0   |
| Helvetica-Bold        | Arimo-Bold             | 310 KB   | 182 KB     | Apache 2.0   |
| Helvetica-Oblique     | Arimo-Italic           | 330 KB   | 195 KB     | Apache 2.0   |
| Helvetica-BoldOblique | Arimo-BoldItalic       | 332 KB   | 195 KB     | Apache 2.0   |
| Times-Roman           | Tinos-Regular          | 464 KB   | 248 KB     | Apache 2.0   |
| Times-Bold            | Tinos-Bold             | 441 KB   | 239 KB     | Apache 2.0   |
| Times-Italic          | Tinos-Italic           | 445 KB   | 245 KB     | Apache 2.0   |
| Times-BoldItalic      | Tinos-BoldItalic       | 447 KB   | 247 KB     | Apache 2.0   |
| Courier               | Cousine-Regular        | 301 KB   | 173 KB     | Apache 2.0   |
| Courier-Bold          | Cousine-Bold           | 290 KB   | 169 KB     | Apache 2.0   |
| Courier-Oblique       | Cousine-Italic         | 264 KB   | 158 KB     | Apache 2.0   |
| Courier-BoldOblique   | Cousine-BoldItalic     | 267 KB   | 160 KB     | Apache 2.0   |
| Symbol                | STIXTwoMath-Regular    | 819 KB   | 629 KB     | SIL OFL 1.1  |
| ZapfDingbats          | NotoSansSymbols-Regular| 291 KB   | 121 KB     | SIL OFL 1.1  |

## API Reference

### Data Structures

```cpp
namespace nanopdf {
namespace embedded_fonts {

// Font entry containing compressed font data
struct FontEntry {
  const char* base_name;              // Base name (e.g., "Arimo-Regular")
  const unsigned char* compressed_data; // Compressed font binary data
  size_t compressed_size;             // Compressed data size in bytes
  size_t original_size;               // Uncompressed data size in bytes
  const char* filename;               // Original filename
};

// Font mapping for PDF Standard 14 fonts
struct FontMapping {
  const char* pdf_name;         // PDF Standard 14 name
  const char* substitute_name;  // Substitute font base name
};

}  // namespace embedded_fonts
}  // namespace nanopdf
```

### Functions

```cpp
// Decompress a font to a std::vector
bool decompress_font(const FontEntry* entry, std::vector<uint8_t>& output);

// Decompress a font to a pre-allocated buffer
// buffer must be at least entry->original_size bytes
bool decompress_font_to_buffer(const FontEntry* entry, unsigned char* buffer, size_t buffer_size);

// Find a font by base name (case-insensitive)
const FontEntry* find_font(const char* name);

// Get substitute font for a PDF Standard 14 font name
const FontEntry* get_pdf_standard_font(const char* pdf_name);
```

### Constants

```cpp
// Array of all embedded fonts (compressed)
extern const FontEntry font_registry[];

// Number of embedded fonts
extern const size_t font_count;

// PDF Standard 14 mapping table
extern const FontMapping pdf_standard_14_mapping[];

// Number of mappings
extern const size_t pdf_mapping_count;
```

## Build Output

When fonts are embedded, the build process will:

1. Read all `.ttf` and `.otf` files from the `fonts/` directory
2. Compress each file using zlib (level 9)
3. Convert compressed data to C++ byte arrays
4. Generate `embedded-fonts.hh` in the build directory
5. Include decompression helpers
6. Include the header in your build

The generated header is **not checked into version control** - it's regenerated each time you build.

## Size Considerations

### With Compression (Current)

- All 14 fonts total approximately **3.06 MB** (compressed)
- Header file size: ~19 MB (source code)
- Binary size increase: **+3.06 MB**
- Decompression overhead: ~1-2ms per font (one-time)

### Without Compression (Legacy)

- All 14 fonts total approximately **5.18 MB** (uncompressed)
- Header file size: ~32 MB (source code)
- Binary size increase: **+5.18 MB**
- No decompression needed

**Savings**: Compression saves **2.12 MB** (~41%) in binary size

## Disabling Embedded Fonts

To build without embedded fonts:

```bash
# Main library
cmake -DNANOPDF_EMBED_FONTS=OFF ..

# Rasterize example
cd examples/rasterize/build
cmake -DEMBED_FONTS=OFF ..
```

When disabled, your code can still check for the macro:

```cpp
#ifndef NANOPDF_EMBED_FONTS
  // Load fonts from filesystem or use system fonts
#endif
```

## Performance Tips

### Decompression Caching

If you use the same font multiple times, decompress once and cache:

```cpp
#ifdef NANOPDF_EMBED_FONTS
class FontCache {
  std::unordered_map<std::string, std::vector<uint8_t>> cache_;

public:
  const uint8_t* get_font(const char* pdf_name, size_t& size) {
    auto it = cache_.find(pdf_name);
    if (it != cache_.end()) {
      size = it->second.size();
      return it->second.data();
    }

    const auto* entry = nanopdf::embedded_fonts::get_pdf_standard_font(pdf_name);
    if (!entry) return nullptr;

    std::vector<uint8_t> data;
    if (!nanopdf::embedded_fonts::decompress_font(entry, data)) return nullptr;

    cache_[pdf_name] = std::move(data);
    size = cache_[pdf_name].size();
    return cache_[pdf_name].data();
  }
};
#endif
```

### Lazy Decompression

Only decompress fonts when actually needed:

```cpp
#ifdef NANOPDF_EMBED_FONTS
// Don't decompress until font is used
const auto* font_entry = nanopdf::embedded_fonts::get_pdf_standard_font(font_name);

// ... later, when font is needed ...
if (font_entry && !decompressed) {
  nanopdf::embedded_fonts::decompress_font(font_entry, font_data);
  decompressed = true;
}
#endif
```

## Compatibility

### zlib vs miniz

The decompression code automatically uses the correct zlib API:

- **System zlib**: Uses standard zlib (`uncompress`, etc.)
- **miniz** (`NANOPDF_USE_MINIZ`): Uses miniz API (`mz_uncompress`, etc.)

Both are fully compatible and produce identical results.

### Error Handling

Always check decompression return values:

```cpp
#ifdef NANOPDF_EMBED_FONTS
std::vector<uint8_t> font_data;
if (!nanopdf::embedded_fonts::decompress_font(entry, font_data)) {
  // Handle decompression error
  std::cerr << "Failed to decompress font: " << entry->filename << std::endl;
  return false;
}
// font_data now contains uncompressed font
#endif
```

## Advanced Usage

### Direct Access to Compressed Data

For custom decompression or analysis:

```cpp
#ifdef NANOPDF_EMBED_FONTS
const auto* font = nanopdf::embedded_fonts::find_font("Arimo-Bold");
if (font) {
  // Access compressed data directly
  const unsigned char* compressed = font->compressed_data;
  size_t compressed_size = font->compressed_size;
  size_t original_size = font->original_size;

  // Custom decompression with your preferred library
  custom_decompress(compressed, compressed_size, original_size);
}
#endif
```

### Integration with stb_truetype

```cpp
#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#include <stb_truetype.h>

bool load_pdf_font(const char* pdf_font_name, stbtt_fontinfo& font_info) {
  const auto* entry = nanopdf::embedded_fonts::get_pdf_standard_font(pdf_font_name);
  if (!entry) return false;

  static std::vector<uint8_t> font_data;  // Keep alive
  if (!nanopdf::embedded_fonts::decompress_font(entry, font_data)) {
    return false;
  }

  return stbtt_InitFont(&font_info, font_data.data(), 0) != 0;
}
#endif
```
