# TASKS

Remaining work, known issues, and improvement ideas for nanopdf.
Last updated: 2026-02-22.

---

## Completed Features Summary

The following major features are implemented and working:

- **Core PDF Parsing**: Header, xref tables/streams, trailer, object resolution, linearized PDF detection, xref repair
- **Stream Filters**: FlateDecode, ASCII85Decode, ASCIIHexDecode, LZWDecode, RunLengthDecode, DCTDecode, CCITTFaxDecode (1D/2D)
- **Fonts**: Type0 (CID), Type1, TrueType, Type3, MMType1, CIDFont0/2, CMap, ToUnicode, font substitution (Arimo/Tinos/Cousine), lazy per-page loading
- **Text Extraction**: Content stream operators (Td, TD, Tm, T*, Tj, TJ, etc.), text rendering modes, StandardEncoding with Differences, Adobe Glyph List
- **Color Spaces**: DeviceGray, DeviceRGB, DeviceCMYK, CalGray, CalRGB, Lab, ICCBased, Indexed, Pattern, Separation, DeviceN
- **Annotations**: 24 types (Text, Link, FreeText, Highlight, Underline, StrikeOut, etc.) with appearance streams
- **Forms**: TextField, ButtonField, ChoiceField, SignatureField, FDF import/export, widget annotations
- **Document Structure**: Outlines/bookmarks, page labels, named destinations, DocumentInfo, XMP metadata
- **Security**: RC4-40/128, AES-128/256, MD5, SHA-256/SHA-1, owner/user password auth, permission flags
- **Rendering Backends**: Canvas (HTML5), SVG, ThorVG (optional), Blend2D (optional)
- **Graphics State**: ExtGState parsing, blend mode parsing, soft mask parsing, pattern parsing, transparency group parsing
- **Infrastructure**: CI with ASan/UBSan/TSan, nanotest framework (520+ tests, 1080+ assertions), ccache, cross-compilation (Windows x64/x86, WASM, iOS, ARM64)

---

## Remaining Work

### Parser Robustness

- [ ] **Object stream recovery for truncated PDFs** — 336 UNSAFE-DOCS files use compressed Object Streams (/Type/ObjStm) and are truncated mid-stream. The repair_xref scanner finds container objects but can't decompress the inner objects. Could attempt partial ObjStm decompression to recover more objects.
- [ ] **Startxref offset tolerance** — Some web-crawled PDFs have startxref offsets off by a few bytes. A previous attempt to add +/-4 tolerance caused 17 crashes (reverted in 369849c). Needs a safer approach: validate that the target offset contains `xref` or `N G obj` before accepting it.
- [ ] **Incremental update chain validation** — The trailer /Prev chain is followed but not validated for circular references beyond the visited_offsets set. Add explicit depth limit.

### Stream Filters

- [ ] **JPXDecode (JPEG2000)** — Not implemented. A jpx-decoder.cc/hh exists but status is unclear. Requires integrating OpenJPEG or a lightweight decoder. This is the main missing filter for real-world PDF coverage.
- [x] **JBIG2Decode integration** — Full JBIG2 decoder (src/jbig2/, ported from PDFium) is integrated as the primary decoder with JBIG2Globals stream extraction support. Old stub removed.
- [ ] **Crypt filter for individual streams** — Per-stream encryption via /Crypt filter name is parsed but not fully exercised against real-world PDFs.

### Security / Crypto

- [ ] **AES-256 revision 6 key derivation** — AES-256 encryption/decryption is implemented in crypto.cc but the full revision 6 key derivation (SHA-256/384/512 with extension algorithm per ISO 32000-2) needs testing against real encrypted PDFs.
- [ ] **Public-key encryption** — Not supported (only standard security handler).
- [ ] **Digital signature verification** — Signature field structure and PKCS#7 parsing works but RSA/ECDSA cryptographic verification is not implemented.

### Text Extraction

- [ ] **Text positioning accuracy** — Character spacing and word spacing are tracked but not always applied correctly for complex layouts (multi-column, tables).
- [ ] **Right-to-left and bidirectional text** — Not handled. Arabic/Hebrew text may appear in reverse order.
- [ ] **Vertical writing mode** — CJK vertical text layout not implemented.
- [ ] **Ligature decomposition** — Some fonts use ligature glyphs that should be decomposed for searchable text.

### Rendering

- [ ] **Transparency group compositing** — Extended graphics state transparency is parsed but not rendered correctly in all cases.
- [ ] **Blend mode rendering** — All 16 blend modes are parsed but only Normal mode is rendered in raster output.
- [ ] **Tiling pattern painting** — Pattern definitions are parsed but not painted in raster exports.
- [ ] **Shading pattern rendering** — Gradient/shading definitions are parsed but not fully rendered.
- [ ] **Type 3 font rendering** — Glyph procedures are parsed but rendering through content stream interpretation is incomplete.
- [ ] **Soft mask compositing** — Parsed but not applied during compositing.

### Fonts

- [ ] **OpenType/CFF glyph rendering** — CFF parser (cff-parser.cc) exists but glyph rendering from CFF outlines is incomplete.
- [ ] **Subsetting validation** — Some embedded fonts are subsetted with non-standard glyph mappings that may cause extraction issues.
- [ ] **Fallback font coverage** — The embedded Arimo/Tinos/Cousine fonts don't cover all glyphs. Missing glyph handling could be improved (e.g., .notdef substitution, Noto fallback chain).

### Document Features

- [ ] **Tagged PDF / Accessibility** — Structure tree and marked content types are defined in nanopdf.hh but parsing is not exercised in tests. Required for PDF/UA compliance.
- [ ] **Optional content groups (layers)** — OCG visibility tracking exists in canvas-exporter.cc but full OCG/OCMD dictionary parsing and layer management is incomplete.
- [ ] **Multimedia objects** — Sound, movie, and 3D annotation types are defined but content is not extracted or played.
- [ ] **JavaScript actions** — Document-level and annotation-level JavaScript is not extracted or evaluated.
- [ ] **Embedded file streams** — Attachment parsing (pdf-attachments.cc) exists but may not handle all packaging formats (PDF Portfolios).

### Arlington Validation

- [ ] **Fix evaluate_required nested fn:SinceVersion** — `fn:IsRequired(fn:SinceVersion(1.4,fn:IsPresent(KEY)))` incorrectly triggers on version alone, ignoring the inner fn:IsPresent condition. Affects 43 TSV entries.
- [ ] **XRef stream trailer validation** — Modern PDFs using XRef streams should be validated against `XRefStream.tsv` instead of `FileTrailer.tsv` when `Type=XRef` is present.
- [ ] **Inheritance support for required keys** — Required keys marked `inheritable=TRUE` (e.g., Resources, MediaBox on pages) are flagged as missing when inherited from a parent page tree node.
- [ ] **Array-based definition discrimination** — When following links to array-type definitions (e.g., color spaces), the validator doesn't use index-0 values to pick the right definition (CalRGB vs CalGray).
- [ ] **Expand fn: expression coverage** — Many Arlington fn: expressions beyond fn:SinceVersion and fn:IsPresent are not evaluated (fn:BeforeVersion, fn:Eval, fn:BitSet, etc.).

### Performance

- [ ] **Lazy page tree walking** — The entire page tree is traversed at parse time. Defer until first page access for large documents.
- [ ] **Object offset cache optimization** — The object_offsets map uses linear scanning. Consider a more efficient structure for very large PDFs (>100k objects).
- [ ] **Memory-mapped I/O** — parse_from_memory requires the entire file in memory. Add mmap-based parsing for large files.

### Testing

- [ ] **Core parser unit tests** — `tests/unit/core/` directory exists but is empty. Add tests for PDF object parsing, xref handling, object resolution, and stream processing.
- [ ] **Graphics unit tests** — `tests/unit/graphics/` directory exists but is empty. Add tests for path construction, color space rendering, clipping, and graphics state operations.
- [ ] **Rendering backend tests** — `tests/unit/backends/` directory exists but is empty. Add tests for Canvas, SVG, ThorVG, and Blend2D output.
- [ ] **PDF/A compliance test suite** — No tests for PDF/A-1b, PDF/A-2b, PDF/A-3b compliance.
- [ ] **Encrypted PDF corpus testing** — No systematic testing against encrypted PDFs with various encryption schemes.
- [ ] **Visual regression testing** — Infrastructure exists (visual_compare, test_visual_thorvg, test_visual_blend2d) but no automated visual regression suite is active.
- [ ] **Fuzz testing** — No fuzzer harness exists. Add libFuzzer or AFL++ integration for parser robustness.
- [ ] **Performance benchmarks** — Benchmark infrastructure exists but no CI-integrated regression tracking.
- [ ] **DCT/JPEG test images** — test_dct_decode.cc has a TODO to add tests with actual JPEG test images.
- [ ] **Flate large data test** — test_flate_decode.cc has a TODO to add tests for larger data decompression.

### Build / Infrastructure

- [ ] **Reduce nanopdf.cc size** — The main source file is ~12k LOC. Consider splitting into logical modules (xref parsing, content stream, object resolution, font loading).
- [ ] **PDF writer testing** — pdf-writer.cc is ~7k LOC with form manipulation, font embedding, encryption, and content stream generation, but has limited test coverage.
- [ ] **MCP server documentation** — MCP_USAGE.md exists but may need updates as the protocol evolves.
- [ ] **nanostl completeness** — nanostl is missing thread, mutex, and some container features needed for full compatibility.

---

## Known Issues

- **CC-MAIN false failures** — ~337/100k failures are HTML error pages or truncated fragments from web crawling, not parser bugs.
- **UNSAFE-DOCS remaining failures** — ~255/14k failures are severely truncated files using compressed Object Streams with no recoverable structure.
- **Large PDF memory usage** — Parsing very large PDFs (>1GB) requires the entire file in memory due to parse_from_memory API design.
- ~~**JBIG2 stub vs full decoder**~~ — Resolved. Full JBIG2 decoder is now the primary decoder; stub removed.
- **ThorVG anti-aliasing** — Built-in anti-aliasing cannot be disabled (ThorVG limitation).
