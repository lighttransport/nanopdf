# TODO

## Stream Filters
- [ ] Replace the CCITTFaxDecode placeholder with a compliant Group 3/4 decoder (1D & 2D)
- [ ] Integrate JPXDecode via OpenJPEG or a lightweight JPEG2000 decoder
- [ ] Harden filter error reporting with sample regression PDFs

## Text & Fonts
- [x] Capture `/Encoding` dictionaries and `Differences` for simple fonts
- [x] Add StandardEncoding glyph-name fallback for text extraction
- [x] Expand glyph-name map to cover the full Adobe Glyph List (current table covers common Latin-1)
- [x] Add automated regression that exercises StandardEncoding plus `Differences`

## Rendering & Graphics
- [ ] Flesh out transparency and blending support beyond parsing (soft masks, blend modes)
- [ ] Implement tiling/shading pattern painting for raster exporters

## Testing & Tooling
- [x] Promote `build/manual_extract.cc` into a formal regression test (Phase 2/3)
- [x] Capture real-world PDFs exhibiting StandardEncoding differences and add to `data/`
- [x] Add CI target that runs all `test_phase*` executables in release + debug modes

## Documentation
- [x] Refresh README with current feature set and StandardEncoding changes
- [x] Document optional build flags and nanostl usage in a dedicated section or wiki page
