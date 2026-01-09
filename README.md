# NanoPDF

NanoPDF is a lightweight C++11 library for parsing and inspecting PDF files. It focuses on
read-only workflows such as structure inspection, text extraction, annotations, and form
metadata. The codebase is self-contained and ships with optional miniature STL replacements
for constrained environments.

## Features

### Core Parsing
- Document structure parsing (catalog, pages, outlines, named destinations)
- Cross-reference tables (linear and compressed object streams)
- Fallback xref scanning for malformed PDFs

### Stream Filters
- FlateDecode (zlib/deflate)
- ASCII85Decode, ASCIIHexDecode
- LZWDecode, RunLengthDecode
- DCTDecode (JPEG via stb_image)
- CCITTFaxDecode (Group 3/4 fax, 1D and 2D modes)
- JBIG2Decode (partial)

### Fonts
- Type1, TrueType, Type0 (CID), Type3 fonts
- Font descriptors and metrics
- CMap and ToUnicode mappings
- StandardEncoding with glyph-name fallback via Adobe Glyph List
- Font substitution for Standard 14 fonts

### Text Extraction
- Text positioning operators (Td, TD, Tm, T*)
- Text matrix transformations and rendering modes
- Character/word spacing support
- Automatic line break injection

### Graphics
- Color spaces: DeviceGray, DeviceRGB, DeviceCMYK, CalRGB, CalGray, Lab, ICCBased, Indexed, Separation, DeviceN
- Image XObjects with decode arrays and masks
- Extended graphics state (transparency, blend modes)
- Pattern and shading parsing

### Interactive Features
- Annotations: Text, Link, Markup, FreeText, Stamp, Ink, Line, Shape, Widget
- Form fields: Text, Button, Choice, Signature (AcroForm)
- Appearance streams (Normal, Rollover, Down states)
- Bookmarks/outlines with destinations
- Page labels and named destinations

### Security
- Standard security handler (RC4/AES-128/AES-256)
- User/owner password authentication
- Permission flags
- Pure C++ crypto implementation (no external libraries)

## Recent Updates

- Lazy loading for fonts (loaded on first text extraction per page)
- Filter chain support (multiple filters applied in sequence)
- Improved error reporting with detailed messages and bounds checking
- CCITTFaxDecode filter with Group 3/4 support (1D and 2D modes)
- ASCIIHexDecode filter
- CI workflow with AddressSanitizer, UBSan, and ThreadSanitizer builds
- Fallback xref scanning for corrupted or non-standard PDFs

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Optional build flags

| Flag | Default | Description |
| --- | --- | --- |
| `NANOPDF_USE_CCACHE` | `ON` | Enable ccache when available to speed up incremental builds. |
| `NANOPDF_USE_NANOSTL` | `OFF` | Use the bundled `nanostl` headers instead of the system STL (handy for restricted platforms). |
| `NANOPDF_USE_STB_TRUETYPE` | `ON` | Include `stb_truetype` to parse embedded TrueType fonts. Disable if you ship your own font loader. |
| `NANOPDF_USE_THORVG` | `OFF` | Build the ThorVG raster backend. Requires the ThorVG headers/libraries on your system. |
| `NANOPDF_BUILD_TESTS` | `ON` (honours `BUILD_TESTING`) | Build the phase executables under `src/test-*.cc`. |
| `NANOPDF_BUILD_WASM` | `OFF` | Target WebAssembly (requires configuring with Emscripten's toolchain). |
| `NANOPDF_LAZY_METADATA` | `OFF` | Enable lazy loading of metadata (forms, outlines, labels). |

Pass any flag with `-DNAME=VALUE` while configuring via CMake.

## Tests

Phase executables live under `build/` after configuration. Typical runs:

```bash
cmake --build build --target test_phase3
./build/test_phase3 data/HelloWorld_xr.pdf
```

A small harness (`build/manual_extract.cc`) can be compiled against the static library to
exercise StandardEncoding text extraction.

### CI helper

Run the full Release + Debug test sweep with CMake:

```bash
cmake -P scripts/run-ci.cmake
```

If you have an existing build tree, the helper is also exposed as `cmake --build build --target ci`.
Set `NANOPDF_CI_CMAKE_ARGS` (e.g. `-DNANOPDF_USE_THORVG=ON`) to forward extra configure flags, and
`NANOPDF_CI_CONFIGS` to override the configuration list.

GitHub Actions runs this helper on `ubuntu-latest` and `macos-latest` for every push and pull request, plus dedicated AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer Debug builds on `ubuntu-latest`. Each job uploads the generated CMake and ctest logs as artifacts for debugging failures.

### Maintaining the glyph list

`src/adobe_glyph_list.inc` is auto-generated from Adobe’s canonical glyph list. If the upstream
data changes, refresh the header with:

```bash
scripts/update-glyph-list-inc.py
```

The script reads `data/adobe_glyph_list.txt` and rewrites the generated header in-place.

### Sample PDFs

`data/standardencoding/` contains real PDFs collected from PDF Association, SAS Global Forum, and
NIST publications. They exercise StandardEncoding glyph differences in the wild—handy when you need
to sanity-check text extraction regressions.

## Limitations

- JPXDecode (JPEG2000) filter not implemented
- Advanced transparency, blending, and pattern rendering are parsed but not fully rendered
- No writing support (read-only library)

## Contributing

1. Keep headers self-contained (`#pragma once`) and prefer STL over custom containers unless building with `NANOSTL`
2. Add targeted regression tests (phase executables) for new parser features
3. Document any new data fixtures under `data/`
4. Run the relevant `test_phase*` executables before submitting changes

## License

Apache 2.0 © 2024–present Light Transport Entertainment Inc.
