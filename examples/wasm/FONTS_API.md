# Embedded Fonts WASM API

The nanopdf WASM build includes 14 embedded PDF Standard 14 alternative fonts (compressed with zlib). This document describes the JavaScript API for accessing these fonts.

## Font Statistics

- **Total fonts**: 14 fonts (Arimo, Tinos, Cousine, STIX Two Math, Noto Sans Symbols)
- **Original size**: 5.18 MB
- **Compressed size**: 3.06 MB (in WASM bundle)
- **Space saved**: 40.9% reduction
- **WASM bundle size**: ~4.0 MB (includes fonts + nanopdf library)

## Available Functions

### Check Font Availability

```javascript
const available = Module._nanopdf_fonts_available();
// Returns: 1 if fonts are embedded, 0 otherwise
```

### Get Font Count

```javascript
const count = Module._nanopdf_fonts_get_count();
// Returns: 14 (number of embedded fonts)
```

### List All Fonts

```javascript
const listPtr = Module._nanopdf_fonts_list();
const listJson = Module.UTF8ToString(listPtr);
const fontsList = JSON.parse(listJson);

console.log(fontsList);
// {
//   "fonts": [
//     {
//       "name": "Arimo-Regular",
//       "filename": "Arimo-Regular.ttf",
//       "originalSize": 315432,
//       "compressedSize": 185234,
//       "compressionRatio": 0.413
//     },
//     // ... more fonts
//   ],
//   "count": 14
// }
```

### Get PDF Standard 14 Mapping

```javascript
const mappingPtr = Module._nanopdf_fonts_get_pdf_mapping();
const mappingJson = Module.UTF8ToString(mappingPtr);
const mapping = JSON.parse(mappingJson);

console.log(mapping);
// {
//   "mapping": [
//     {
//       "pdfName": "Helvetica",
//       "substituteName": "Arimo-Regular"
//     },
//     {
//       "pdfName": "Helvetica-Bold",
//       "substituteName": "Arimo-Bold"
//     },
//     // ... more mappings
//   ],
//   "count": 14
// }
```

### Load Font by Base Name

```javascript
// Allocate font name in WASM memory
const namePtr = Module.allocateUTF8("Arimo-Regular");

// Load and decompress font
const dataPtr = Module._nanopdf_fonts_load(namePtr);

if (dataPtr === 0) {
  // Error occurred
  const errorPtr = Module._nanopdf_get_last_error();
  const error = Module.UTF8ToString(errorPtr);
  console.error("Font load error:", error);
} else {
  // Get decompressed font size
  const size = Module._nanopdf_fonts_get_loaded_size();

  // Read font data from WASM memory
  const fontData = new Uint8Array(Module.HEAPU8.buffer, dataPtr, size);

  // Use the font data (valid until next font load)
  console.log("Loaded font:", fontData.length, "bytes");

  // Example: Create a Blob for use with FontFace API
  const blob = new Blob([fontData], { type: 'font/ttf' });
  const url = URL.createObjectURL(blob);

  const font = new FontFace('Arimo-Regular', `url(${url})`);
  await font.load();
  document.fonts.add(font);
}

// Clean up
Module._free(namePtr);
```

### Load PDF Standard 14 Font

```javascript
// Load a PDF Standard 14 font (e.g., "Helvetica")
const namePtr = Module.allocateUTF8("Helvetica");
const dataPtr = Module._nanopdf_fonts_load_pdf_standard(namePtr);

if (dataPtr !== 0) {
  const size = Module._nanopdf_fonts_get_loaded_size();
  const fontData = new Uint8Array(Module.HEAPU8.buffer, dataPtr, size);

  // Helvetica maps to Arimo-Regular
  console.log("Loaded Helvetica substitute:", fontData.length, "bytes");
}

Module._free(namePtr);
```

### Get Font Info

```javascript
const namePtr = Module.allocateUTF8("Tinos-Bold");
const infoPtr = Module._nanopdf_fonts_get_info(namePtr);
const infoJson = Module.UTF8ToString(infoPtr);
const info = JSON.parse(infoJson);

console.log(info);
// {
//   "found": true,
//   "name": "Tinos-Bold",
//   "filename": "Tinos-Bold.ttf",
//   "originalSize": 451234,
//   "compressedSize": 244567,
//   "compressionRatio": 0.458
// }

Module._free(namePtr);
```

## Available Fonts

### Font Families

| Family | Fonts | Use Case |
|--------|-------|----------|
| **Arimo** | Regular, Bold, Italic, BoldItalic | Helvetica substitute (sans-serif) |
| **Tinos** | Regular, Bold, Italic, BoldItalic | Times substitute (serif) |
| **Cousine** | Regular, Bold, Italic, BoldItalic | Courier substitute (monospace) |
| **STIX Two Math** | Regular | Symbol font |
| **Noto Sans Symbols** | Regular | Dingbats font |

### PDF Standard 14 Mapping

| PDF Font | Substitute |
|----------|------------|
| Helvetica | Arimo-Regular |
| Helvetica-Bold | Arimo-Bold |
| Helvetica-Oblique | Arimo-Italic |
| Helvetica-BoldOblique | Arimo-BoldItalic |
| Times-Roman | Tinos-Regular |
| Times-Bold | Tinos-Bold |
| Times-Italic | Tinos-Italic |
| Times-BoldItalic | Tinos-BoldItalic |
| Courier | Cousine-Regular |
| Courier-Bold | Cousine-Bold |
| Courier-Oblique | Cousine-Italic |
| Courier-BoldOblique | Cousine-BoldItalic |
| Symbol | STIXTwoMath-Regular |
| ZapfDingbats | NotoSansSymbols-Regular |

## Complete Example

```javascript
async function loadAndUseFont(pdfFontName) {
  // Check if fonts are available
  if (!Module._nanopdf_fonts_available()) {
    console.error("Embedded fonts not available");
    return null;
  }

  // Load the font
  const namePtr = Module.allocateUTF8(pdfFontName);
  const dataPtr = Module._nanopdf_fonts_load_pdf_standard(namePtr);
  Module._free(namePtr);

  if (dataPtr === 0) {
    const errorPtr = Module._nanopdf_get_last_error();
    console.error("Error:", Module.UTF8ToString(errorPtr));
    return null;
  }

  // Get font data
  const size = Module._nanopdf_fonts_get_loaded_size();
  const fontData = new Uint8Array(Module.HEAPU8.buffer, dataPtr, size);

  // Copy to JavaScript (font buffer is reused on next load)
  const fontCopy = new Uint8Array(fontData);

  // Create FontFace
  const blob = new Blob([fontCopy], { type: 'font/ttf' });
  const url = URL.createObjectURL(blob);

  const fontFace = new FontFace(pdfFontName, `url(${url})`);
  await fontFace.load();
  document.fonts.add(fontFace);

  console.log(`Loaded ${pdfFontName} (${size} bytes)`);

  return fontFace;
}

// Usage
Module.onRuntimeInitialized = async function() {
  Module._nanopdf_init();

  // Load all PDF Standard 14 fonts
  const fonts = [
    'Helvetica', 'Helvetica-Bold',
    'Times-Roman', 'Times-Bold',
    'Courier', 'Courier-Bold'
  ];

  for (const fontName of fonts) {
    await loadAndUseFont(fontName);
  }

  console.log("All fonts loaded!");
};
```

## Memory Management

**Important**: The font buffer returned by `nanopdf_fonts_load()` and `nanopdf_fonts_load_pdf_standard()` is **reused** on each call. If you need to keep the font data, copy it to JavaScript:

```javascript
const dataPtr = Module._nanopdf_fonts_load(namePtr);
const size = Module._nanopdf_fonts_get_loaded_size();

// ❌ Bad: Reference only (will be overwritten on next load)
const fontRef = new Uint8Array(Module.HEAPU8.buffer, dataPtr, size);

// ✅ Good: Copy to JavaScript
const fontCopy = new Uint8Array(fontRef);
```

## Performance

- **Decompression time**: ~1-2ms per font (zlib decompression)
- **Font size**: 100-800 KB per font (decompressed)
- **Recommendation**: Load fonts lazily (on-demand) or cache decompressed data

## Testing

Open `examples/wasm/test-fonts.html` in a web browser to:
- View all embedded fonts
- See compression statistics
- Test loading individual fonts
- Verify font data integrity

## Build Configuration

Fonts are automatically embedded when building for WASM:

```bash
./scripts/bootstrap-emscripten.sh
cd build_wasm
emmake make -j4
```

The fonts are enabled by default via `NANOPDF_EMBED_FONTS=ON` in CMake for WASM builds.

## License

All embedded fonts use permissive open-source licenses:
- **Arimo, Tinos, Cousine**: Apache License 2.0
- **STIX Two Math, Noto Sans Symbols**: SIL Open Font License 1.1

Both licenses allow commercial use, modification, and redistribution.
