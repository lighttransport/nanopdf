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

Optional flags: `-DNANOPDF_USE_THORVG=ON`, `-DNANOPDF_USE_STB_TRUETYPE=ON`, etc.

## Tests

Phase executables live under `build/` after configuration. Typical runs:

```bash
cmake --build build --target test_phase3
./build/test_phase3 data/HelloWorld_xr.pdf
```

A small harness (`build/manual_extract.cc`) can be compiled against the static library to
exercise StandardEncoding text extraction.

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
