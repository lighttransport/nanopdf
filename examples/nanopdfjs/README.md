# nanopdfjs - PDF Editor Demo

A web-based PDF editor built with nanopdf's WebAssembly module. Features include:

- **Upload multiple PDFs** and combine them
- **Page thumbnails** panel for navigation
- **Annotation tools**: Text, Line, Rectangle, Oval, Highlight
- **Page manipulation**: Rotate (90/180/270), Crop, Delete, Reorder
- **Export** edited document as PDF

## Building

### Prerequisites

- Emscripten SDK (emsdk) installed and activated
- CMake 3.5+
- A web server for local testing

### Build Steps

1. Build nanopdf with WASM support:

```bash
# From nanopdf root directory
./scripts/bootstrap-emscripten.sh
cd build_wasm
emmake make -j$(nproc)
```

2. Serve the demo:

```bash
# From examples/nanopdfjs directory
python3 -m http.server 8000
```

3. Open in browser:

```
http://localhost:8000/
```

## Project Structure

```
nanopdfjs/
├── index.html          # Main demo page (HTML/CSS/JS)
├── src/
│   └── nanopdf.js      # JavaScript wrapper for WASM module
├── assets/             # (optional) Static assets
└── README.md           # This file
```

## Features

### File Upload
- Drag and drop PDF files onto the dropzone
- Click to browse and select files
- Multiple files can be loaded and combined

### Page Thumbnails
- Left sidebar shows page thumbnails
- Click to navigate to page
- Move pages up/down with arrows
- Delete individual pages

### Annotation Tools
- **Text**: Click to place text annotation
- **Line**: Click and drag to draw lines
- **Rectangle**: Click and drag for rectangles (filled or stroke)
- **Oval**: Click and drag for ellipses
- **Highlight**: Semi-transparent highlight overlay

### Page Operations
- **Rotate**: Rotate page by 90 degrees left or right
- **Crop**: Draw a crop region to trim the page
- **Delete**: Remove current page from document
- **Zoom**: Zoom in/out or fit to window

### Export
- Click "Export PDF" to download the edited document
- All pages, annotations, rotations, and crops are included

## API Reference

The `NanoPDF` JavaScript class provides these methods:

### Document Loading
```javascript
const pageCount = nanopdf.loadPDF(arrayBuffer);  // Quick view
const docId = nanopdf.docLoad(arrayBuffer);       // For editing
```

### Working Document
```javascript
nanopdf.workAddAllPages(docId);        // Add all pages from document
nanopdf.workRemovePage(index);         // Remove page
nanopdf.workMovePage(from, to);        // Reorder pages
nanopdf.workRotatePage(index, angle);  // Rotate (0, 90, 180, 270)
nanopdf.workCropPage(index, x, y, w, h); // Crop page
```

### Annotations
```javascript
nanopdf.addTextAnnotation(pageIndex, x, y, text, options);
nanopdf.addLineAnnotation(pageIndex, x1, y1, x2, y2, options);
nanopdf.addRectAnnotation(pageIndex, x, y, w, h, options);
nanopdf.addOvalAnnotation(pageIndex, cx, cy, rx, ry, options);
nanopdf.addHighlightAnnotation(pageIndex, x, y, w, h, options);
```

### Export
```javascript
const pdfData = nanopdf.exportPDF();           // Returns Uint8Array
nanopdf.exportAndDownload('output.pdf');       // Download directly
```

## Browser Support

- Chrome 57+
- Firefox 52+
- Safari 11+
- Edge 16+

Requires WebAssembly support.

## Known Limitations

- Rendering requires ThorVG backend (build with `-DNANOPDF_USE_THORVG=ON`)
- Without rendering backend, pages show as placeholders
- Large PDFs may be slow to process
- Some PDF features may not be fully preserved in export

## License

Apache 2.0 - See LICENSE file in root directory.
