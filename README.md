# NanoPDF

NanoPDF is a lightweight C++11 library for parsing and inspecting PDF files. It focuses on
read-only workflows such as structure inspection, text extraction, annotations, and form
metadata. The codebase is self-contained and ships with optional miniature STL replacements
for constrained environments.

## Features

- Document structure parsing (catalog, pages, outlines, named destinations)
- Stream decoding (Flate, ASCII85, LZW, RunLength, DCT, JBIG2 – partial)
- Fonts: Type1, TrueType, Type0 (CID), Type3, font descriptors, substitution fallbacks
- Text extraction with matrix tracking, word spacing, StandardEncoding + Differences support
- Color spaces, image XObjects, ICC profiles, basic raster helpers
- Annotation and form field parsing (AcroForm widgets, appearance streams)
- Encryption handler for RC4/AES (standard security handler)

## Recent Updates

- Capture `/Encoding` dictionaries for simple fonts, recording `Differences`
- StandardEncoding fallback now maps ligatures/accents via glyph names instead of raw bytes
- Text extractor injects line breaks when the content stream moves to a new line or block

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
| `NANOPDF_BUILD_WASM` | `OFF` | Target WebAssembly (requires configuring with Emscripten’s toolchain). |

Pass any flag with `-DNAME=VALUE` while configuring via CMake.

## Tests

Phase executables live under `build/` after configuration. Typical runs:

```bash
cmake --build build --target test_phase3
./build/test_phase3 data/HelloWorld_xr.pdf
```

A small harness (`build/manual_extract.cc`) can be compiled against the static library to
exercise StandardEncoding text extraction.

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

- CCITTFaxDecode and JPXDecode filters are stubs; monochrome fax streams are only partially supported
- Advanced transparency, blending, and pattern rendering are parsed but not rendered
- No writing support (modifying or regenerating PDFs)

## Contributing

1. Keep headers self-contained (`#pragma once`) and prefer STL over custom containers unless building with `NANOSTL`
2. Add targeted regression tests (phase executables) for new parser features
3. Document any new data fixtures under `data/`
4. Run the relevant `test_phase*` executables before submitting changes

## License

Apache 2.0 © 2024–present Light Transport Entertainment Inc.
