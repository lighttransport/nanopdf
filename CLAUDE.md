# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

nanopdf is a lightweight, minimal PDF parsing library written in C++11. It focuses on parsing PDF document structure, extracting text content, and decoding various stream filters. The library is designed to be embedded in other applications with minimal dependencies.

## Build System

This project uses CMake as its build system with C++11 standard requirement.

### Build Commands

```bash
# Standard CMake build
mkdir build
cd build
cmake ..
make

# Using the bootstrap script with Clang
./scripts/bootstrap-clang.sh
cd build
make

# Build for WebAssembly/Emscripten
./scripts/bootstrap-emscripten.sh
cd build
make

# Build specific targets
make nanopdf          # Build library
make test_nanopdf     # Build test executable
```

### CMake Options

- `NANOPDF_USE_CCACHE`: Use ccache for faster recompilation (default: ON)
- `NANOPDF_USE_NANOSTL`: Use nanostl instead of standard library (default: OFF)
- `NANOPDF_USE_STB_TRUETYPE`: Use stb_truetype for font loading (default: ON)
- `NANOPDF_BUILD_TESTS`: Build test executables (default: ON)
- `NANOPDF_BUILD_WASM`: Build for WebAssembly with Emscripten (default: OFF)
- `NANOPDF_USE_THORVG`: Use ThorVG for rendering backend (default: OFF)
- `SANITIZE_ADDRESS`: Enable address sanitizer for debugging

### Testing

Run the test executable with a PDF file:
```bash
./test_nanopdf path/to/file.pdf

# Test with provided sample
./test_nanopdf ../data/blank.pdf
```

### Code Formatting

Uses Google C++ style with 2-space indentation. Format code with:
```bash
clang-format -i src/*.cc src/*.hh
```

## Code Architecture

### Core Components

**nanopdf.hh/nanopdf.cc**: Main parsing engine containing:
- `Pdf` struct: Main document container with xref tables, trailer, and document catalog
- `Value` class: Polymorphic type system for PDF objects (boolean, number, string, name, array, dictionary, stream, reference)
- `Parser` class: Low-level PDF syntax parser
- `DocumentCatalog`: High-level document structure with pages
- `Page` struct: Individual page with resources, content streams, and font information

**stream-reader.hh**: Low-level byte stream reader with endianness handling for binary data parsing.

**jbig2-decoder.hh/cc**: JBIG2 monochrome bitmap decoder for compressed image streams.

**ttf-loader.cc**: TrueType font file loader and parser.

**canvas-exporter.hh/cc**: Canvas and SVG export functionality for rendering PDF content to HTML5 Canvas commands or SVG elements.

**thorvg-backend.hh/cc**: ThorVG rendering backend for vector graphics rendering (optional, enabled with NANOPDF_USE_THORVG).

**crypto.hh/cc**: Pure C++ cryptographic implementations for PDF security (RC4, AES-128/256, MD5, SHA-256).

**pdf-security.cc**: PDF encryption handling including standard security handler, password authentication, and permission flags.

### Key Data Structures

- `XRef`/`XRefSection`: Cross-reference table entries for object location tracking
- `BaseFont`/`FontDescriptor`: Font resource management with metrics and embedded font data
- `Type0Font`: CID font support with CMap and ToUnicode mappings
- `Type3Font`: User-defined font support with glyph procedures
- `DecodedStream`: Result container for stream filter decoding operations
- `ResolvedObject`: Container for dereferenced PDF objects with error handling
- `ColorSpace`: Color space representation (DeviceGray, DeviceRGB, DeviceCMYK, CalRGB, CalGray, ICCBased, Indexed)
- `ImageXObject`: Image resource with dimensions, color space, and decoded data
- `TextState`: Complete text state tracking for content stream processing
- `CMap`: Character to Unicode mapping for font encoding
- `FontSubstitution`: Font fallback system for missing fonts
- `Annotation`: Base class for all annotation types with appearance streams
- `TextAnnotation`, `LinkAnnotation`, `MarkupAnnotation`, `FreeTextAnnotation`: Specific annotation types
- `FormField`: Base class for interactive form fields
- `TextField`, `ButtonField`, `ChoiceField`: Specific form field types
- `WidgetAnnotation`: Form field widget annotations

### Stream Filters

The `filters` namespace provides decoders for:
- FlateDecode (zlib/deflate compression)
- ASCII85Decode (Base85 encoding)
- ASCIIHexDecode (Hexadecimal encoding)
- LZWDecode (Lempel-Ziv-Welch compression)
- RunLengthDecode (Run-length encoding compression)
- DCTDecode (JPEG compression via stb_image)
- CCITTFaxDecode (Group 3/4 fax, 1D and 2D modes)
- JBIG2Decode (monochrome bitmap compression - partial)

### Dependencies

- **Required**: zlib for FlateDecode stream decompression
- **Embedded**: miniz.c/h, stb_truetype.h, stb_image.h, stb_image_write.h, tiny_dng_loader.h, fpng for various format support
- **Optional**: nanostl for reduced standard library footprint, ThorVG for rasterization

## Development Patterns

### Error Handling
Functions return boolean success/failure or use result structs with `success` flag and `error` message fields.

### Memory Management  
Uses RAII with smart pointers (`std::unique_ptr`) for font resources and manual resource cleanup in destructors.

### PDF Object Resolution
Objects are resolved through the `resolve_reference()` function using object number and generation number from xref tables.

### Stream Processing
Stream data is decoded through a pipeline: raw bytes → filter decoding → final decoded content using the `decode_stream()` function. Filter chains (multiple filters) are supported and applied in sequence.

### Lazy Loading
Fonts are loaded lazily per page - only when text extraction is performed. Use `Page::ensure_fonts_loaded()` or `Page::get_font()` for explicit control. Metadata (forms, outlines, labels) can be loaded lazily by compiling with `NANOPDF_LAZY_METADATA=1`.

## Compilation Defines

- `NANOPDF_DEBUG_PRINT`: Enable debug output (set to 1 in CMakeLists.txt by default)
- `NANOPDF_USE_NANOSTL`: Use nanostl library instead of standard library
- `NANOPDF_USE_STB_TRUETYPE`: Enable TrueType font support via stb_truetype
- `NANOPDF_LAZY_METADATA`: Enable lazy loading of metadata (forms, outlines, labels)

## Testing Files

The repository includes `data/blank.pdf` as a sample PDF file for testing. The main test executable is `test_nanopdf` which accepts a PDF file path as command-line argument and demonstrates library usage for parsing and extracting content.

Test executables:
- `test_nanopdf`: Main PDF parsing test
- `test_phase1`, `test_phase1_simple`: Phase 1 features (filters, images, color spaces)
- `test_phase2_features`, `test_phase2_text_extraction`, `test_phase2_standardencoding`, `test_phase2_rendering`, `test_phase2_real_pdfs`: Phase 2 features (text extraction, fonts, encoding)
- `test_phase3`: Phase 3 features (annotations, forms, interactivity)
- `test_phase4`: Phase 4 features (document navigation, metadata, outlines)
- `test_phase5`: Phase 5 features (security, encryption, cryptographic algorithms)
- `test_phase6`: Phase 6 features (CCITTFaxDecode, advanced filters)
- `test_thorvg`: ThorVG backend test (when NANOPDF_USE_THORVG is enabled)