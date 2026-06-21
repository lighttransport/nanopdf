# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

nanopdf is a lightweight PDF parsing and writing library written in C++17. It handles document structure parsing, text extraction, stream decoding, form manipulation, annotations, encryption, and optional rasterization. The library is designed to be embedded in other applications with minimal dependencies (only bundled miniz for zlib).

## Build System

This project uses CMake (3.16+) with C++17 standard requirement.

### Build Commands

```bash
# Standard build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Using the bootstrap script with Clang
./scripts/bootstrap-clang.sh && cd build && make

# Build for WebAssembly/Emscripten
./scripts/bootstrap-emscripten.sh && cd build && make

# Key targets
make nanopdf          # Library
make nanopdf-mcp      # MCP server executable
make benchmark        # Benchmark tool
make nanopdf_unit_suite  # Unit test suite
```

### CMake Options

| Flag | Default | Description |
| --- | --- | --- |
| `NANOPDF_USE_CCACHE` | ON | Use ccache for faster recompilation |
| `NANOPDF_USE_NANOSTL` | OFF | Use nanostl instead of standard library |
| `NANOPDF_USE_STB_TRUETYPE` | ON | Use stb_truetype for font loading |
| `NANOPDF_BUILD_TESTS` | ON | Build test executables |
| `NANOPDF_BUILD_VALIDATION_TESTS` | ON | Build PDF spec validation tests |
| `NANOPDF_BUILD_WASM` | OFF | Build for WebAssembly with Emscripten |
| `NANOPDF_USE_THORVG` | OFF | ThorVG rendering backend |
| `NANOPDF_USE_BLEND2D` | OFF | Blend2D rendering backend |
| `NANOPDF_USE_LIBTIFF` | OFF | Use libtiff for CCITT/Fax decoding |
| `NANOPDF_EMBED_FONTS` | OFF | Embed Standard 14 font replacements |
| `NANOPDF_EMBED_CJK_FONTS` | OFF | Embed CJK fonts (~61 MB) |
| `SANITIZE_ADDRESS` | OFF | Enable AddressSanitizer |

### Testing

```bash
cd build
ctest --output-on-failure              # Run all tests (unit + integration + validation)
ctest -L unit --output-on-failure      # Unit tests only
ctest -L integration --output-on-failure  # Integration tests only
ctest -L validation --output-on-failure   # Validation tests only
```

### Code Formatting

Uses Google C++ style with 2-space indentation. Format code with:
```bash
clang-format -i src/*.cc src/*.hh
```

## Source Directory Layout

```
src/                    Core library source (flat layout)
  nanopdf.hh/cc         Main parsing engine, Value class, Pdf struct
  pdf-writer.hh/cc      PDF creation and incremental writing
  pdf-security.cc       Encryption/decryption, password auth
  crypto.hh/cc          Pure C++ crypto (RC4, AES, MD5, SHA-256)
  document-structure.cc Document outline, page labels, destinations
  text-layout.hh/cc     Text layout analysis (lines, words, columns)
  table-extraction.hh/cc Table detection and export (CSV/HTML/JSON)
  canvas-exporter.hh/cc HTML5 Canvas and SVG export
  thorvg-backend.hh/cc  ThorVG rendering backend (optional)
  blend2d-backend.hh/cc Blend2D rendering backend (optional)
  color-transform.hh/cc Color space conversions
  font-provider.hh/cc   Font discovery and substitution
  cff-parser.hh/cc      Compact Font Format parser
  type1-parser.hh/cc    PostScript Type 1 font parser
  jpx-decoder.hh/cc     JPEG2000 decoder (single-tile)
  pdf-attachments.hh/cc File attachment extraction
  form-manipulation.cc  Form field value manipulation, FDF import/export
  stream-reader.hh      Byte stream reader with endianness handling
  nanopdf-log.hh        Logging utilities
  jbig2/                JBIG2 monochrome bitmap decoder (ported from PDFium)
  mcp/                  MCP (Model Context Protocol) server
  third_party/          Embedded libraries (miniz, stb_image, stb_truetype, etc.)
  nanostl/              Optional minimal STL replacement

tests/
  nanotest.hh           Custom lightweight test framework
  fixtures/             Shared test helpers (test_helpers.hh/cc)
  unit/                 Feature-organized unit tests
    core/               Value, XRef, PDF parsing tests
    filters/            Stream decoder tests (flate, ascii85, lzw, dct, ccitt, etc.)
    fonts/              CMap, Type0, Type3, font substitution tests
    text/               Text extraction, layout, table extraction tests
    document/           Catalog, outline, page labels, PDF/A validation tests
    annotations/        Annotation type tests
    forms/              Form field manipulation tests
    security/           AES, RC4, hash tests
    graphics/           Color transform tests
    backends/           (conditional on ThorVG/Blend2D)
  integration/          End-to-end tests with real PDF files
  validation/           PDF spec compliance (Arlington model, SafeDocs, corpus)

tools/                  Standalone utilities (ccitt_compare, visual_compare)
examples/               Example apps (pdfdump, rasterize, pdfsign, img2pdf, wasm, mcp)
fonts/                  Open-source font files (Arimo, Tinos, Cousine, Noto, STIX)
cmake/                  CMake modules (sanitizers, toolchains, font embedding)
scripts/                Build and utility scripts
data/                   Test PDF files and resources
docs/                   Active documentation (THIRD_PARTY, MCP_USAGE, FONTS_EMBEDDING, WASM,
                        C_WRITER_USAGE, C11_MIGRATION roadmap)
```

## Code Architecture

### Core Components

**nanopdf.hh/cc**: Main parsing engine containing:
- `Pdf` struct: Document container with thread-safe caches, xref tables, trailer, catalog
- `Value` class: Polymorphic PDF object type system (9 types)
- `Parser` class: Low-level PDF syntax parser
- `DocumentCatalog`: Pages, fonts, annotations, forms, outlines, metadata
- `Page` struct: Per-page resources, content streams, font management
- `ErrorKind` enum: Categorized error types (Malformed, Unsupported, Encrypted, IOError, Internal)
- `ParseResult` struct: Rich parse results with error classification
- `PdfAValidationResult` / `validate_pdfa()`: PDF/A conformance validation

**pdf-writer.hh/cc**: PDF creation and modification:
- `PdfWriter` class: Create PDFs from scratch or load existing for incremental updates
- `PageBuilder`: Fluent API for drawing on pages
- `SignaturePlaceholder` / `SigningCallback` / `apply_signature()`: Digital signature workflow
- Form filling, annotation CRUD, watermarks, bookmarks

### Key Data Structures

- `XRef`/`XRefSection`: Cross-reference table entries
- `BaseFont`/`FontDescriptor`: Font resources with metrics and embedded data
- `Type0Font`/`Type3Font`: CID and user-defined font support
- `DecodedStream`: Stream decode result with error classification
- `ResolvedObject`: Dereferenced PDF object container
- `ParseResult`: Parse result with `ErrorKind` classification
- `ColorSpace`: Full color space representation (12 types)
- `ImageXObject`: Image resource with decode arrays and masks
- `TextState`: Complete text state machine
- `CMap`: Character-to-Unicode mapping
- `Annotation` hierarchy: 27 annotation types
- `FormField` hierarchy: Text, Button, Choice, Signature fields
- `OutlineItem`: Bookmark tree with nested hierarchy
- `XMPMetadata`: XMP metadata with PDF/A identification
- `OutputIntentInfo`: PDF/A and PDF/X color intent profiles

### Stream Filters

The `filters` namespace provides decoders for:
- FlateDecode (zlib/deflate, with PNG predictor support)
- ASCII85Decode, ASCIIHexDecode
- LZWDecode, RunLengthDecode
- DCTDecode (JPEG via stb_image)
- CCITTFaxDecode (Group 3/4 fax)
- JBIG2Decode (monochrome bitmap)
- JPXDecode (JPEG2000, single-tile)
- Filter chains (multiple filters in sequence)

### Dependencies

- **Embedded** (in `src/third_party/`): miniz (zlib replacement), stb_image, stb_image_write, stb_truetype, tiny_dng_loader
- **Optional**: nanostl, ThorVG, Blend2D, libtiff

## Development Patterns

### Error Handling
Public API uses `ParseResult`/`DecodedStream`/`ResolvedObject` result structs with `success`, `error`, and `ErrorKind` fields. The `ErrorKind` enum classifies errors as `Malformed`, `Unsupported`, `Encrypted`, `IOError`, or `Internal`. Legacy functions return `bool` for backward compatibility (delegating to `ParseResult` internally).

### Thread Safety
The `Pdf` struct contains a `cache_mutex` protecting all mutable caches (object cache, decoded stream cache, object stream cache, offset cache). Individual `Pdf` instances can be used from multiple threads. Create separate `Pdf` instances for parallel document processing.

### Memory Management
Uses RAII with `std::unique_ptr` for font resources, outline trees, and structure elements.

### PDF Object Resolution
Objects are resolved through `resolve_reference()` using object number and generation number from xref tables. Results are cached in the thread-safe object cache.

### Stream Processing
Stream data is decoded through: raw bytes -> filter chain -> decoded content using `decode_stream()`. Results are cached in an LRU decoded stream cache.

### Lazy Loading
Fonts are loaded lazily per page. Document metadata (forms, outlines, labels) can be loaded on demand via `ensure_*_loaded()` methods.

## Testing

### Test Framework
Uses `nanotest` - a custom header-only C++17 test framework (~300 LOC) with:
- `TEST_SUITE()` / `TEST_CASE()` macros for organization
- `CHECK()` / `REQUIRE()` / `CHECK_EQ()` / `CHECK_APPROX()` assertions
- Auto-registration and CTest integration

### Test Executables

Tests are aggregated into a few binaries (one per category) that all use the nanotest
global registry; CTest selects them by label (`unit` / `integration` / `validation`):

- `nanopdf_unit_suite`: All unit tests (~500 cases across core, filters, fonts, text, document, annotations, forms, security, graphics) + MCP JSON tests
- `nanopdf_integration_suite`: All integration tests (parse real PDFs + text extraction) merged into one binary
- `nanopdf_validation_suite`: All validation tests (Arlington parse/validate, SafeDocs, corpus parsing, PDF differences) merged into one binary
- `nanopdf_c_api_smoke`: C API smoke test (separate, C language)
- `test_visual_thorvg`, `test_visual_blend2d`, `test_visual_lightvg`: Visual regression (per backend)
- `ncrypto/tests/test_*`: 13 standalone pure-C11 crypto tests (separate project)

### Compilation Defines

- `NANOPDF_LOG_LEVEL`: Log verbosity 0-5 (None, Error, Warn, Info, Debug, Trace)
- `NANOPDF_USE_NANOSTL`: Use nanostl library
- `NANOPDF_USE_STB_TRUETYPE`: Enable TrueType font support
- `NANOPDF_LAZY_METADATA`: Lazy load metadata
- `NANOPDF_USE_MINIZ`: Use embedded miniz (set automatically for non-WASM builds)
