# NanoPDF

NanoPDF is a lightweight C++14 library for parsing and inspecting PDF files. It focuses on
read-only workflows such as structure inspection, text extraction, annotations, and form
metadata. The codebase is self-contained and ships with optional miniature STL replacements
for constrained environments.

## Features

### Core Parsing
- Document structure parsing (catalog, pages, outlines, named destinations)
- Cross-reference tables (traditional and compressed object streams)
- Linearized PDF support (including truncated partial downloads)
- Robust xref repair for malformed/corrupted PDFs
- Trailer chain merging for incremental updates

### Stream Filters
- FlateDecode (zlib/deflate)
- ASCII85Decode, ASCIIHexDecode
- LZWDecode, RunLengthDecode
- DCTDecode (JPEG via stb_image)
- CCITTFaxDecode (Group 3/4 fax, 1D and 2D modes)
- JBIG2Decode (monochrome bitmap compression)
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
- Text matrix transformations and rendering modes
- Character/word spacing support
- Automatic line break injection
- Table structure detection

### Graphics
- Color spaces: DeviceGray, DeviceRGB, DeviceCMYK, CalRGB, CalGray, Lab, ICCBased, Indexed, Separation, DeviceN
- Image XObjects with decode arrays and masks
- Extended graphics state (transparency, blend modes)
- Pattern and shading parsing
- Color space transformations

### Interactive Features
- Annotations: Text, Link, Markup, FreeText, Stamp, Ink, Line, Shape, Widget
- Form fields: Text, Button, Choice, Signature (AcroForm)
- Form manipulation (fill, flatten)
- Appearance streams (Normal, Rollover, Down states)
- Bookmarks/outlines with destinations
- Page labels and named destinations
- File attachments

### Security
- Standard security handler (RC4/AES-128/AES-256)
- User/owner password authentication
- Permission flags
- Pure C++ crypto implementation (no external libraries)

### Rendering Backends
- Canvas export (HTML5 Canvas commands)
- SVG export
- ThorVG vector graphics backend (optional)
- Blend2D rasterization backend (optional)

### Other
- PDF writing/generation
- MCP (Model Context Protocol) server for AI integration
- WebAssembly/Emscripten support

## Building

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
| `NANOPDF_USE_THORVG` | `OFF` | Build the ThorVG raster backend |
| `NANOPDF_USE_BLEND2D` | `OFF` | Build the Blend2D raster backend |
| `NANOPDF_BUILD_TESTS` | `ON` | Build test executables |
| `NANOPDF_BUILD_VALIDATION_TESTS` | `ON` | Build PDF spec validation tests |
| `NANOPDF_BUILD_LEGACY_TESTS` | `ON` | Build legacy phase test executables |
| `NANOPDF_BUILD_WASM` | `OFF` | Target WebAssembly (requires Emscripten) |
| `NANOPDF_EMBED_FONTS` | `OFF` | Embed Standard 14 font replacements |
| `NANOPDF_EMBED_CJK_FONTS` | `OFF` | Embed CJK fonts (~61 MB) |
| `NANOPDF_LAZY_METADATA` | `OFF` | Lazy loading of metadata (forms, outlines, labels) |
| `SANITIZE_ADDRESS` | `OFF` | Enable AddressSanitizer |

## Tests

### Unit and validation tests

```bash
cmake --build build
ctest --test-dir build                     # Run all tests
ctest --test-dir build -L unit             # Unit tests only
ctest --test-dir build -L validation       # Validation tests only
```

### Corpus testing

Download real-world PDF corpora for large-scale parser validation:

```bash
scripts/download-corpora.sh
```

Run corpus parsing tests (requires downloaded corpora):

```bash
NANOPDF_TEST_DATA_DIR=tests/data ./build/tests/validation/test_corpus_parsing
```

Supported corpora:
- **CC-MAIN-2021** — 100k+ real-world PDFs from Common Crawl (99.66% parse rate, 0 crashes)
- **UNSAFE-DOCS** — 14k+ synthetically malformed PDFs from DARPA SafeDocs (98.18% parse rate, 0 crashes)
- **SafeDocs** — hand-crafted edge-case PDFs (100% parse rate)
- **GovDocs1**, **veraPDF**, **pdf.js**, **pdfium**, **Tika** — additional corpora

### Arlington PDF model validation

Validate parsed PDF structure against the Arlington PDF model (machine-readable PDF spec):

```bash
NANOPDF_TEST_DATA_DIR=tests/data ./build/tests/validation/test_arlington_validate
```

### CI helper

```bash
cmake -P scripts/run-ci.cmake
```

### Maintaining the glyph list

`src/adobe_glyph_list.inc` is auto-generated from Adobe's canonical glyph list:

```bash
scripts/update-glyph-list-inc.py
```

## Limitations

- JPXDecode (JPEG2000) filter not implemented
- Advanced transparency, blending, and pattern rendering are parsed but not fully rendered
- Tagged PDF / accessibility structure trees not yet supported
- PDF writing support is experimental

## Contributing

1. Keep headers self-contained (`#pragma once`) and prefer STL over custom containers unless building with `NANOSTL`
2. Add unit tests under `tests/` for new features
3. Run `ctest` and corpus tests before submitting changes
4. Format code with `clang-format -i` (Google style, 2-space indent)

## License

Apache 2.0 © 2024-present Light Transport Entertainment Inc.
