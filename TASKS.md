# TASKS

Remaining work, known issues, and improvement ideas for nanopdf.

## Parser Robustness

- [ ] **Object stream recovery for truncated PDFs** — 336 UNSAFE-DOCS files use compressed Object Streams (/Type/ObjStm) and are truncated mid-stream. The repair_xref scanner finds container objects but can't decompress the inner objects. Could attempt partial ObjStm decompression to recover more objects.
- [ ] **Startxref offset tolerance** — Some web-crawled PDFs have startxref offsets off by a few bytes. A previous attempt to add ±4 tolerance caused 17 crashes (reverted in 369849c). Needs a safer approach — perhaps validate that the target offset contains `xref` or `N G obj` before accepting it.
- [ ] **Incremental update chain validation** — The trailer /Prev chain is followed but not validated for circular references beyond the visited_offsets set. Add explicit depth limit.

## Stream Filters

- [ ] **JPXDecode (JPEG2000)** — Not implemented. Requires integrating OpenJPEG or a lightweight decoder. This is the main missing filter.
- [ ] **JBIG2Decode completeness** — Partial implementation. Some advanced JBIG2 features (refinement coding, Huffman tables) may not be fully supported.
- [ ] **Crypt filter for individual streams** — Per-stream encryption via /Crypt filter name is parsed but not fully exercised against real-world PDFs.

## Security / Crypto

- [ ] **AES-256 (revision 6) encryption** — Basic structure exists but full revision 6 key derivation (using SHA-256/384/512 with extension algorithm) needs testing against real encrypted PDFs.
- [ ] **Public-key encryption** — Not supported (only standard security handler).
- [ ] **Certificate-based signatures** — Signature field structure is parsed but cryptographic verification is not implemented.

## Text Extraction

- [ ] **Actualized text positioning** — Character spacing and word spacing are tracked but not always applied correctly for complex layouts.
- [ ] **Right-to-left and bidirectional text** — Not handled. Arabic/Hebrew text may appear in reverse order.
- [ ] **Vertical writing mode** — CJK vertical text layout not implemented.
- [ ] **Ligature decomposition** — Some fonts use ligature glyphs that should be decomposed for searchable text.

## Rendering

- [ ] **Transparency group compositing** — Extended graphics state transparency is parsed but not rendered correctly in all cases.
- [ ] **Blend mode rendering** — Blend modes are parsed but only Normal mode is rendered.
- [ ] **Tiling pattern painting** — Pattern definitions are parsed but not painted in raster exports.
- [ ] **Shading pattern rendering** — Gradient/shading definitions are parsed but not rendered.
- [ ] **Type 3 font rendering** — Glyph procedures are parsed but rendering through content stream interpretation is incomplete.
- [ ] **Soft mask support** — Parsed but not applied during compositing.

## Fonts

- [ ] **OpenType/CFF rendering** — CFF parser exists but glyph rendering from CFF outlines is incomplete.
- [ ] **Subsetting validation** — Some embedded fonts are subsetted with non-standard glyph mappings that may cause extraction issues.
- [ ] **Fallback font coverage** — The embedded Arimo/Tinos/Cousine fonts don't cover all glyphs. Missing glyph handling could be improved.

## Document Features

- [ ] **Tagged PDF / Accessibility** — Structure trees and marked content are not parsed. Required for PDF/UA compliance.
- [ ] **Optional content groups (layers)** — OCG/OCMD parsing is not implemented.
- [ ] **Multimedia objects** — Sound, movie, and 3D content are not supported.
- [ ] **JavaScript actions** — Document-level and annotation-level JavaScript is not extracted or evaluated.
- [ ] **Embedded file streams** — Attachment parsing exists but may not handle all packaging formats (PDF Portfolios).

## Arlington Validation

- [ ] **Fix evaluate_required nested fn:SinceVersion** — `fn:IsRequired(fn:SinceVersion(1.4,fn:IsPresent(KEY)))` incorrectly triggers on version alone, ignoring the inner fn:IsPresent condition. Affects 43 TSV entries.
- [ ] **XRef stream trailer validation** — Modern PDFs using XRef streams should be validated against `XRefStream.tsv` instead of `FileTrailer.tsv` when `Type=XRef` is present.
- [ ] **Inheritance support for required keys** — Required keys marked `inheritable=TRUE` (e.g., Resources, MediaBox on pages) are flagged as missing when inherited from a parent page tree node.
- [ ] **Array-based definition discrimination** — When following links to array-type definitions (e.g., color spaces), the validator doesn't use index-0 values to pick the right definition (CalRGB vs CalGray).
- [ ] **Expand fn: expression coverage** — Many Arlington fn: expressions beyond fn:SinceVersion and fn:IsPresent are not evaluated (fn:BeforeVersion, fn:Eval, fn:BitSet, etc.).

## Performance

- [ ] **Decoded stream caching** — Streams are re-decoded on every access. Add an LRU cache for decoded stream data.
- [ ] **Lazy page tree walking** — The entire page tree is traversed at parse time. Defer until first page access for large documents.
- [ ] **Object offset cache optimization** — The object_offsets map uses linear scanning. Consider a more efficient structure for very large PDFs (>100k objects).
- [ ] **Memory-mapped I/O** — parse_from_memory requires the entire file in memory. Add mmap-based parsing for large files.

## Testing

- [ ] **PDF/A compliance test suite** — No tests for PDF/A-1b, PDF/A-2b, PDF/A-3b compliance.
- [ ] **Encrypted PDF corpus testing** — No systematic testing against encrypted PDFs with various encryption schemes.
- [ ] **Visual regression testing** — Infrastructure exists (visual_compare tool) but no automated visual regression suite is active.
- [ ] **Fuzz testing** — No fuzzer harness exists. Add libFuzzer or AFL++ integration for parser robustness.
- [ ] **Performance benchmarks** — Benchmark infrastructure exists but no CI-integrated regression tracking.

## Build / Infrastructure

- [ ] **Reduce nanopdf.cc size** — The main source file is ~10k LOC / 400KB. Consider splitting into logical modules (xref parsing, content stream, object resolution).
- [ ] **PDF writer maturity** — pdf-writer.cc exists but is experimental. Needs more testing and documentation.
- [ ] **MCP server documentation** — MCP_USAGE.md exists but may need updates as the protocol evolves.
- [ ] **nanostl completeness** — nanostl is missing thread, mutex, and some container features needed for full compatibility.

## Known Issues

- **CC-MAIN false failures** — ~337/100k failures are HTML error pages or truncated fragments from web crawling, not parser bugs.
- **UNSAFE-DOCS remaining failures** — ~255/14k failures are severely truncated files using compressed Object Streams with no recoverable structure.
- **Large PDF memory usage** — Parsing very large PDFs (>1GB) requires the entire file in memory due to parse_from_memory API design.
