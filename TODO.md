# TODO

## Stream Filters
- [x] Implement CCITTFaxDecode with Group 3/4 support (1D & 2D modes)
- [ ] Integrate JPXDecode via OpenJPEG or a lightweight JPEG2000 decoder
- [x] Harden filter error reporting (bounds checking, detailed errors, filter chaining)

## Text & Fonts
- [x] Capture `/Encoding` dictionaries and `Differences` for simple fonts
- [x] Add StandardEncoding glyph-name fallback for text extraction
- [x] Expand glyph-name map to cover the full Adobe Glyph List
- [x] Add automated regression that exercises StandardEncoding plus `Differences`

## Rendering & Graphics
- [ ] Flesh out transparency and blending support beyond parsing (soft masks, blend modes)
- [ ] Implement tiling/shading pattern painting for raster exporters

## Testing & Tooling
- [x] Promote `build/manual_extract.cc` into a formal regression test (Phase 2/3)
- [x] Capture real-world PDFs exhibiting StandardEncoding differences and add to `data/`
- [x] Add CI target that runs all `test_phase*` executables in release + debug modes
- [x] Add sanitizer builds (ASan, UBSan, TSan) to CI workflow

## Documentation
- [x] Refresh README with current feature set and StandardEncoding changes
- [x] Document optional build flags and nanostl usage in a dedicated section or wiki page

## Future Enhancements
- [ ] Tagged PDF / accessibility support (structure trees, marked content)
- [x] Lazy loading for fonts (loaded on first text extraction per page)
- [x] Lazy metadata loading (compile with NANOPDF_LAZY_METADATA=1)
- [ ] Lazy page tree walking (defer until first page access)
- [ ] Decoded stream caching
- [ ] Multimedia support (sound, movie, 3D objects)
