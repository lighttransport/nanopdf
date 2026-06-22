# NanoPDF

NanoPDF is a lightweight C++17 library for parsing, inspecting, and writing PDF files. It handles
document structure, text extraction, form manipulation, annotations, encryption, and optional
rasterization. The codebase is self-contained with no external dependencies (zlib is provided by
the bundled miniz library).

**🌐 Live demo (WebAssembly viewer):** https://lighttransport.github.io/nanopdf/ — runs entirely
in your browser (built from `examples/wasm/`).

## Features

### Core Parsing
- Document structure parsing (catalog, pages, outlines, named destinations)
- Cross-reference tables (traditional and compressed object streams)
- Linearized PDF support (including truncated partial downloads)
- Robust xref repair for malformed/corrupted PDFs
- Thread-safe object and stream caches
- Structured error handling with `ParseResult` and `ErrorKind` classification

### Stream Filters
- FlateDecode (zlib/deflate with PNG predictor support)
- ASCII85Decode, ASCIIHexDecode
- LZWDecode, RunLengthDecode
- DCTDecode (JPEG via stb_image)
- CCITTFaxDecode (Group 3/4 fax, 1D and 2D modes)
- JBIG2Decode (monochrome bitmap compression)
- JPXDecode (JPEG2000, single-tile)
- Filter chains (multiple filters applied in sequence)

### Fonts
- Type1, TrueType, Type0 (CID), Type3 fonts
- CFF (Compact Font Format) and PostScript Type 1 parsing
- Font descriptors and metrics
- CMap and ToUnicode mappings
- StandardEncoding with glyph-name fallback via Adobe Glyph List
- Font substitution for Standard 14 fonts
- Optional embedded fonts (Arimo/Tinos/Cousine metric-compatible replacements)
- CJK font embedding support

### Text Extraction
- Text positioning operators (Td, TD, Tm, T*)
- Text layout analysis with line, word, and column detection
- Character/word spacing support
- Reading order detection (including RTL)
- Table structure detection with CSV/HTML/JSON/Markdown export
- Spatial queries (text in rectangle)

### Graphics
- Color spaces: DeviceGray, DeviceRGB, DeviceCMYK, CalRGB, CalGray, Lab, ICCBased, Indexed, Separation, DeviceN, Pattern
- Image XObjects with decode arrays and masks
- Extended graphics state (transparency, blend modes)
- Pattern and shading parsing
- Color space transformations (CMYK/Lab/CalRGB to RGB)
- ICC profile parsing

### Interactive Features
- 27 annotation types (Text, Link, Markup, FreeText, Stamp, Ink, Line, Shape, Widget, Redaction, etc.)
- Form fields: Text, Button, Choice, Signature (AcroForm)
- Form manipulation (fill, validate, FDF import/export)
- Appearance streams (Normal, Rollover, Down states)
- Bookmarks/outlines with nested hierarchy
- Page labels and named destinations
- File attachments with metadata

### Security
- Standard security handler (RC4/AES-128/AES-256)
- User/owner password authentication
- Permission flags
- Digital signature validation (PKCS#7 structure)
- Signature creation with callback-based signing API
- Pure C++ crypto implementation (no external libraries)

### Document Metadata
- Document info dictionary (title, author, subject, keywords, dates)
- XMP metadata parsing (PDF/A identification, Dublin Core, XMP-MM)
- PDF/A conformance validation (font embedding, output intents, transparency, encryption)
- Output intents (PDF/A, PDF/X) with ICC profiles
- Optional content groups (layers) with visibility control
- Tagged PDF structure trees (40+ element types)

### PDF Writing
- Create PDFs from scratch with pages, text, images, shapes
- Incremental updates for existing PDFs (form filling, annotations, signatures)
- Digital signature placeholders with `apply_signature()` callback API
- Font embedding (TrueType/OpenType subsetting)
- Encryption support (AES-128/256)
- Watermarks, bookmarks, layer creation

### Rendering Backends
- Canvas export (HTML5 Canvas commands)
- SVG export (paths, text, gradients, patterns)
- LightVG software rasterization backend (default)
- ThorVG vector graphics backend (optional)
- Blend2D rasterization backend (optional)
- Render progress callbacks for dense pages (1% steps once the object threshold is reached)

### Example tools
- `examples/pdfdump/pdfdump.cc` can dump PDF structure as YAML/JSON and export a page as SVG with `-f svg -p 1 -o page1.svg`

### Other
- MCP (Model Context Protocol) server for AI integration
- WebAssembly/Emscripten support with font embedding
- Benchmark tool for performance profiling

## Building

Requirements:
- CMake 3.16+
- C++17 compiler

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Optional build flags

| Flag | Default | Description |
| --- | --- | --- |
| `NANOPDF_USE_CCACHE` | `ON` | Use ccache for faster incremental builds |
| `NANOPDF_USE_NANOSTL` | `OFF` | Use bundled nanostl instead of system STL |
| `NANOPDF_USE_STB_TRUETYPE` | `ON` | Include stb_truetype for TrueType font parsing |
| `NANOPDF_USE_LIGHTVG` | `ON` | Build the default LightVG software raster backend |
| `NANOPDF_USE_THORVG` | `OFF` | Build the ThorVG raster backend |
| `NANOPDF_USE_BLEND2D` | `OFF` | Build the Blend2D raster backend |
| `NANOPDF_BUILD_TESTS` | `ON` | Build test executables |
| `NANOPDF_BUILD_VALIDATION_TESTS` | `ON` | Build PDF spec validation tests |
| `NANOPDF_BUILD_WASM` | `OFF` | Target WebAssembly (requires Emscripten) |
| `NANOPDF_EMBED_FONTS` | `OFF` | Embed Standard 14 font replacements |
| `NANOPDF_EMBED_CJK_FONTS` | `OFF` | Embed CJK fonts (~61 MB) |
| `SANITIZE_ADDRESS` | `OFF` | Enable AddressSanitizer |

## Tests

### Unit, integration, and validation tests

```bash
cmake --build build
ctest --test-dir build                        # Run all tests
ctest --test-dir build -L unit                # Unit tests only (~500 test cases)
ctest --test-dir build -L integration         # Integration tests (real PDF parsing)
ctest --test-dir build -L validation          # Validation tests
```

### Corpus testing

Download real-world PDF corpora for large-scale parser validation:

```bash
scripts/download-corpora.sh
```

Run corpus parsing tests (requires downloaded corpora):

```bash
NANOPDF_TEST_DATA_DIR=tests/data ./build/tests/validation/nanopdf_validation_suite
```

(All validation tests -- corpus, SafeDocs, Arlington, PDF-differences -- are built into
the single `nanopdf_validation_suite` binary; run `ctest --test-dir build -L validation`
to drive it via CTest.)

Supported corpora:
- **CC-MAIN-2021** -- 100k+ real-world PDFs from Common Crawl (99.66% parse rate, 0 crashes)
- **UNSAFE-DOCS** -- 14k+ synthetically malformed PDFs from DARPA SafeDocs (98.18% parse rate, 0 crashes)
- **SafeDocs** -- hand-crafted edge-case PDFs (100% parse rate)
- **GovDocs1**, **veraPDF**, **pdf.js**, **pdfium**, **Tika** -- additional corpora

### Arlington PDF model validation

Validate parsed PDF structure against the Arlington PDF model (machine-readable PDF spec):

```bash
NANOPDF_TEST_DATA_DIR=tests/data ./build/tests/validation/nanopdf_validation_suite
```

### CI helper

```bash
cmake -P scripts/run-ci.cmake
```

## Project Layout

```
src/                    Core library source
  nanopdf.hh/cc         Main parsing engine
  pdf-writer.hh/cc      PDF creation and incremental writing
  crypto.hh/cc          Cryptographic implementations
  mcp/                  MCP server (JSON-RPC protocol)
  jbig2/                JBIG2 decoder (ported from PDFium)
  third_party/          Embedded libraries (miniz, stb_image, stb_truetype)
tests/
  unit/                 Feature-organized unit tests (nanotest framework)
  integration/          End-to-end tests with real PDF files
  validation/           PDF spec compliance tests (Arlington, SafeDocs)
examples/               Example apps (pdfdump, rasterize, pdfsign, wasm, mcp)
fonts/                  Open-source font files for embedding
tools/                  Standalone comparison utilities
data/                   Test PDF files
docs/                   Documentation
```

## Limitations

- JPXDecode (JPEG2000) supports single-tile images only
- Advanced transparency and pattern rendering are parsed but not fully rendered
- PDF writing is functional but does not support content stream editing of existing pages
- Digital signing requires a user-provided PKCS#7 callback (no built-in crypto signing)
- XFA forms are not supported

## Contributing

1. Keep headers self-contained and prefer STL over custom containers unless building with `NANOSTL`
2. Add unit tests under `tests/unit/` for new features using the nanotest framework
3. Run `ctest` and corpus tests before submitting changes
4. Format code with `clang-format -i` (Google style, 2-space indent)

## Third-Party Licenses

nanopdf embeds or links the following third-party libraries:

| Library | Version | License | Copyright |
| --- | --- | --- | --- |
| [miniz](https://github.com/richgel999/miniz) | 3.0.0 | Public Domain / Unlicense | Rich Geldreich |
| [stb_truetype](https://github.com/nothings/stb) | 1.26 | MIT / Public Domain | Sean Barrett |
| [stb_image](https://github.com/nothings/stb) | 2.30 | MIT / Public Domain | Sean Barrett |
| [stb_image_write](https://github.com/nothings/stb) | 1.16 | MIT / Public Domain | Sean Barrett |
| [fpng](https://github.com/richgel999/fpng) | 1.0.6 | Public Domain / Unlicense | Rich Geldreich |
| [fpnge](https://github.com/veluca93/fpnge) | — | Apache 2.0 | Google LLC, Luca Versari |
| [libdeflate](https://github.com/ebiggers/libdeflate) | 1.25 | MIT | Eric Biggers |
| [TinyDNGLoader](https://github.com/syoyo/tinydng) | — | MIT | Syoyo Fujita |
| lightvg (software vector graphics, from [lightui](https://github.com/lighttransport/lightui)) | — | Apache 2.0 | Light Transport Entertainment Inc. |
| [nanostl](https://github.com/lighttransport/nanostl) | — | MIT | Light Transport Entertainment Inc. |
| [fast_float](https://github.com/fastfloat/fast_float) (via nanostl) | — | Apache 2.0 / Boost 1.0 | Daniel Lemire et al. |
| [Ryu](https://github.com/ulfjack/ryu) (via nanostl) | — | Apache 2.0 / Boost 1.0 | Ulf Adams |
| JBIG2 decoder (from [PDFium](https://pdfium.googlesource.com/pdfium/)) | — | BSD-3-Clause | The PDFium Authors, Foxit Software Inc. |
| CCITT Fax decoder (from [PDFium](https://pdfium.googlesource.com/pdfium/)) | — | BSD-3-Clause | The PDFium Authors, Foxit Software Inc. |
| [ThorVG](https://github.com/thorvg/thorvg) (optional) | 1.0.1 | MIT | ThorVG Project |
| [Blend2D](https://blend2d.com/) (optional) | — | Zlib | Blend2D Authors |
| [libtiff](http://www.libtiff.org/) (optional, `NANOPDF_USE_LIBTIFF`) | — | libtiff (BSD-style) | Sam Leffler, Silicon Graphics Inc. |
| [Adobe Glyph List](https://github.com/adobe-type-tools/agl-aglfn) (data, embedded) | — | BSD-3-Clause | Adobe Inc. |

All cryptography (RSA, ECDSA, AES, SHA, X.509, CMS, RFC 3161, TLS 1.3) is
implemented in-tree (`src/` C++ and the pure-C11 `ncrypto/` stack) — nanopdf has
**no OpenSSL or other external crypto dependency**.

### Bundled Fonts (optional)

Embedded when building with `NANOPDF_EMBED_FONTS` or `NANOPDF_EMBED_CJK_FONTS`,
and served by the WASM viewer. Two licenses apply:

**Apache License 2.0** (Google Croscore fonts; `LICENSE.txt` in each folder):

- **Arimo** — metric-compatible Arial/Helvetica replacement
- **Tinos** — metric-compatible Times New Roman replacement
- **Cousine** — metric-compatible Courier New replacement

**SIL Open Font License 1.1** (`OFL.txt` in each folder):

- **Noto Sans JP / Noto Serif JP** — Japanese (CJK) support (Google)
- **Noto Sans Symbols** — symbol / ZapfDingbats fallback (Google)
- **STIX Two Math** — Symbol / math glyph fallback (STIX Fonts Project / IEEE)

## License

Apache 2.0 (c) 2024-present Light Transport Entertainment Inc.
