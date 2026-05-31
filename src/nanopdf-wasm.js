/**
 * nanopdf WASM API wrapper
 * Provides a convenient JavaScript interface to the nanopdf WASM module
 */

class NanoPDF {
  constructor(module) {
    this._module = module;
    this._initialized = false;
  }

  /**
   * Initialize the nanopdf module
   * @returns {boolean} True if initialization succeeded
   */
  init() {
    if (this._initialized) return true;
    const result = this._module._nanopdf_init();
    this._initialized = result === 1;
    return this._initialized;
  }

  /**
   * Shutdown and cleanup
   */
  shutdown() {
    if (this._initialized) {
      this._module._nanopdf_shutdown();
      this._initialized = false;
    }
  }

  /**
   * Load a PDF from an ArrayBuffer or Uint8Array
   * @param {ArrayBuffer|Uint8Array} data - PDF file data
   * @returns {number} Number of pages, or 0 on failure
   */
  loadPDF(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data;

    // Allocate memory in WASM and copy data
    const ptr = this._module._nanopdf_malloc(bytes.length);
    if (!ptr) {
      console.error('Failed to allocate memory for PDF data');
      return 0;
    }

    this._module.HEAPU8.set(bytes, ptr);

    // Load the PDF
    const pageCount = this._module._nanopdf_load_pdf(ptr, bytes.length);

    // Free the input buffer (the module keeps its own copy)
    this._module._nanopdf_free(ptr);

    return pageCount;
  }

  /**
   * Get the number of pages in the loaded PDF
   * @returns {number} Page count
   */
  getPageCount() {
    return this._module._nanopdf_get_page_count();
  }

  /**
   * Get page dimensions in points (1/72 inch)
   * @param {number} pageIndex - Zero-based page index
   * @returns {{width: number, height: number}} Page dimensions
   */
  getPageSize(pageIndex) {
    return {
      width: this._module._nanopdf_get_page_width(pageIndex),
      height: this._module._nanopdf_get_page_height(pageIndex)
    };
  }

  /**
   * Check if rendering is available
   * @returns {boolean} True if a rendering backend (ThorVG or Blend2D) is available
   */
  hasRendering() {
    return this._module._nanopdf_has_rendering() === 1;
  }

  /**
   * Render a page to an ImageData object
   * @param {number} pageIndex - Zero-based page index
   * @param {number} width - Output width in pixels
   * @param {number} height - Output height in pixels
   * @param {number} [dpi=72] - DPI for rendering
   * @returns {ImageData|null} Rendered page as ImageData, or null on failure
   */
  renderPage(pageIndex, width, height, dpi = 72) {
    const result = this._module._nanopdf_render_page(pageIndex, width, height, dpi);
    if (result !== 1) {
      console.error('Failed to render page:', this.getLastError());
      return null;
    }

    const bufferPtr = this._module._nanopdf_get_render_buffer();
    const bufferSize = this._module._nanopdf_get_render_buffer_size();
    const renderWidth = this._module._nanopdf_get_render_width();
    const renderHeight = this._module._nanopdf_get_render_height();

    if (!bufferPtr || bufferSize === 0) {
      console.error('Render buffer is empty');
      return null;
    }

    // Copy the RGBA data from WASM memory
    const rgbaData = new Uint8ClampedArray(
      this._module.HEAPU8.buffer,
      bufferPtr,
      bufferSize
    ).slice(); // Make a copy since WASM memory may be invalidated

    return new ImageData(rgbaData, renderWidth, renderHeight);
  }

  /**
   * Render a page to a canvas
   * @param {number} pageIndex - Zero-based page index
   * @param {HTMLCanvasElement} canvas - Target canvas element
   * @param {number} [dpi=72] - DPI for rendering
   * @returns {boolean} True if rendering succeeded
   */
  renderToCanvas(pageIndex, canvas, dpi = 72) {
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      console.error('Could not get canvas 2D context');
      return false;
    }

    const imageData = this.renderPage(pageIndex, canvas.width, canvas.height, dpi);
    if (!imageData) {
      return false;
    }

    ctx.putImageData(imageData, 0, 0);
    return true;
  }

  /**
   * Extract text from a page
   * @param {number} pageIndex - Zero-based page index
   * @returns {string} Extracted text
   */
  extractText(pageIndex) {
    const ptr = this._module._nanopdf_extract_text(pageIndex);
    return this._module.UTF8ToString(ptr);
  }

  /**
   * Extract text with position information from a page
   * @param {number} pageIndex - Zero-based page index
   * @returns {Object} Text layout with chars, lines, and words with positions
   * @property {number} pageWidth - Page width in PDF units
   * @property {number} pageHeight - Page height in PDF units
   * @property {number} numColumns - Detected number of text columns
   * @property {Array} chars - Array of character objects with position info
   * @property {Array} lines - Array of line objects with bounding boxes
   * @property {Array} words - Array of word objects with bounding boxes
   */
  extractTextLayout(pageIndex) {
    const ptr = this._module._nanopdf_extract_text_layout(pageIndex);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse text layout JSON' };
    }
  }

  /**
   * Get text within a rectangular region
   * @param {number} pageIndex - Zero-based page index
   * @param {number} x1 - Left X coordinate
   * @param {number} y1 - Bottom Y coordinate
   * @param {number} x2 - Right X coordinate
   * @param {number} y2 - Top Y coordinate
   * @returns {string} Text within the rectangle
   */
  getTextInRect(pageIndex, x1, y1, x2, y2) {
    const ptr = this._module._nanopdf_get_text_in_rect(pageIndex, x1, y1, x2, y2);
    return this._module.UTF8ToString(ptr);
  }

  /**
   * Find text occurrences in a page
   * @param {number} pageIndex - Zero-based page index
   * @param {string} searchTerm - Text to search for
   * @returns {Array<number>} Array of character indices where matches start
   */
  findText(pageIndex, searchTerm) {
    const termPtr = this._module.allocateUTF8(searchTerm);
    const ptr = this._module._nanopdf_find_text(pageIndex, termPtr);
    this._module._free(termPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return [];
    }
  }

  // ============================================================
  // Batch Processing API
  // ============================================================

  /**
   * Batch extract text from multiple pages
   * @param {string} [pageIndices="all"] - Comma-separated page indices (e.g., "0,1,2") or "all"
   * @returns {Object} Object with pages array containing page index and text
   */
  batchExtractText(pageIndices = "all") {
    const indicesPtr = this._module.allocateUTF8(pageIndices);
    const ptr = this._module._nanopdf_batch_extract_text(indicesPtr);
    this._module._free(indicesPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse batch text JSON' };
    }
  }

  /**
   * Batch get page info for multiple pages
   * @param {string} [pageIndices="all"] - Comma-separated page indices or "all"
   * @returns {Object} Object with pages array containing dimensions, rotation, etc.
   */
  batchGetPageInfo(pageIndices = "all") {
    const indicesPtr = this._module.allocateUTF8(pageIndices);
    const ptr = this._module._nanopdf_batch_get_page_info(indicesPtr);
    this._module._free(indicesPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse batch page info JSON' };
    }
  }

  /**
   * Batch search text across multiple pages
   * @param {string} searchTerm - Text to search for
   * @param {string} [pageIndices="all"] - Comma-separated page indices or "all"
   * @returns {Object} Search results with matches per page
   */
  batchFindText(searchTerm, pageIndices = "all") {
    const termPtr = this._module.allocateUTF8(searchTerm);
    const indicesPtr = this._module.allocateUTF8(pageIndices);
    const ptr = this._module._nanopdf_batch_find_text(termPtr, indicesPtr);
    this._module._free(termPtr);
    this._module._free(indicesPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse batch find JSON' };
    }
  }

  /**
   * Search text with highlight geometry.
   * @param {string} searchTerm - Text to search for
   * @param {Object} [options]
   * @param {string} [options.pageIndices="all"] - Comma-separated page indices or "all"
   * @param {boolean} [options.caseSensitive=false] - Case-sensitive search
   * @param {boolean} [options.fuzzy=false] - Include approximate matches
   * @param {number} [options.maxResults=-1] - Maximum result count
   * @returns {Object} Search results with quads and writing-mode metadata
   */
  searchText(searchTerm, options = {}) {
    const pageIndices = options.pageIndices || "all";
    const termPtr = this._module.stringToNewUTF8(searchTerm);
    const indicesPtr = this._module.stringToNewUTF8(pageIndices);
    const ptr = this._module._nanopdf_search_text(
      termPtr,
      indicesPtr,
      options.caseSensitive ? 1 : 0,
      options.fuzzy ? 1 : 0,
      options.maxResults ?? -1
    );
    this._module._free(termPtr);
    this._module._free(indicesPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse search JSON' };
    }
  }

  /**
   * Select text by reading-order range.
   */
  selectTextRange(pageIndex, start, length) {
    const ptr = this._module._nanopdf_select_text_range(pageIndex, start, length);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse selection JSON' };
    }
  }

  /**
   * Select text by rectangle in PDF page coordinates.
   */
  selectTextRect(pageIndex, x1, y1, x2, y2) {
    const ptr = this._module._nanopdf_select_text_rect(pageIndex, x1, y1, x2, y2);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse selection JSON' };
    }
  }

  /**
   * Batch extract text with layout info from multiple pages
   * @param {string} [pageIndices="all"] - Comma-separated page indices or "all"
   * @returns {Object} Object with pages array containing layout info
   */
  batchExtractTextLayout(pageIndices = "all") {
    const indicesPtr = this._module.allocateUTF8(pageIndices);
    const ptr = this._module._nanopdf_batch_extract_text_layout(indicesPtr);
    this._module._free(indicesPtr);
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse batch text layout JSON' };
    }
  }

  // ============================================================
  // Bookmark/Outline API
  // ============================================================

  /**
   * Get document outline (bookmarks) as hierarchical structure
   * @returns {Object} Outline with nested children
   */
  getOutline() {
    const ptr = this._module._nanopdf_get_outline();
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse outline JSON' };
    }
  }

  /**
   * Get flat list of bookmarks with depth info
   * @returns {Object} Flat array of bookmarks with depth
   */
  getOutlineFlat() {
    const ptr = this._module._nanopdf_get_outline_flat();
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse outline JSON' };
    }
  }

  /**
   * Check if document has bookmarks
   * @returns {boolean} True if document has outline/bookmarks
   */
  hasOutline() {
    return this._module._nanopdf_has_outline() === 1;
  }

  /**
   * Get total bookmark count (including nested)
   * @returns {number} Bookmark count
   */
  getOutlineCount() {
    return this._module._nanopdf_get_outline_count();
  }

  // ============================================================
  // Form Field API
  // ============================================================

  /**
   * Get all form fields in the document
   * @returns {Object} Form fields with values and properties
   */
  getFormFields() {
    const ptr = this._module._nanopdf_get_form_fields();
    const jsonStr = this._module.UTF8ToString(ptr);
    try {
      return JSON.parse(jsonStr);
    } catch (e) {
      return { error: 'Failed to parse form fields JSON' };
    }
  }

  /**
   * Check if document has form fields
   * @returns {boolean} True if document has interactive forms
   */
  hasFormFields() {
    return this._module._nanopdf_has_form_fields() === 1;
  }

  /**
   * Get form field count
   * @returns {number} Number of form fields
   */
  getFormFieldCount() {
    return this._module._nanopdf_get_form_field_count();
  }

  /**
   * Get the last error message
   * @returns {string} Error message
   */
  getLastError() {
    const ptr = this._module._nanopdf_get_last_error();
    return this._module.UTF8ToString(ptr);
  }

  // ============================================================
  // Document Management (for editing/concatenation)
  // ============================================================

  /**
   * Load a PDF document for editing
   * @param {ArrayBuffer|Uint8Array} data - PDF file data
   * @returns {number} Document ID, or -1 on failure
   */
  docLoad(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data;
    const ptr = this._module._nanopdf_malloc(bytes.length);
    if (!ptr) return -1;

    this._module.HEAPU8.set(bytes, ptr);
    const docId = this._module._nanopdf_doc_load(ptr, bytes.length);
    this._module._nanopdf_free(ptr);

    return docId;
  }

  /**
   * Get page count in a loaded document
   * @param {number} docId - Document ID
   * @returns {number} Page count
   */
  docGetPageCount(docId) {
    return this._module._nanopdf_doc_get_page_count(docId);
  }

  /**
   * Get page size from a loaded document
   * @param {number} docId - Document ID
   * @param {number} pageIndex - Page index
   * @returns {{width: number, height: number}} Page dimensions
   */
  docGetPageSize(docId, pageIndex) {
    return {
      width: this._module._nanopdf_doc_get_page_width(docId, pageIndex),
      height: this._module._nanopdf_doc_get_page_height(docId, pageIndex)
    };
  }

  /**
   * Close a loaded document
   * @param {number} docId - Document ID
   */
  docClose(docId) {
    this._module._nanopdf_doc_close(docId);
  }

  /**
   * Render a page from loaded document
   * @param {number} docId - Document ID
   * @param {number} pageIndex - Page index
   * @param {number} width - Output width
   * @param {number} height - Output height
   * @param {number} [dpi=72] - DPI
   * @returns {ImageData|null} Rendered page
   */
  docRenderPage(docId, pageIndex, width, height, dpi = 72) {
    const result = this._module._nanopdf_doc_render_page(docId, pageIndex, width, height, dpi);
    if (result !== 1) return null;

    const bufferPtr = this._module._nanopdf_get_render_buffer();
    const bufferSize = this._module._nanopdf_get_render_buffer_size();
    const renderWidth = this._module._nanopdf_get_render_width();
    const renderHeight = this._module._nanopdf_get_render_height();

    if (!bufferPtr || bufferSize === 0) return null;

    const rgbaData = new Uint8ClampedArray(
      this._module.HEAPU8.buffer, bufferPtr, bufferSize
    ).slice();

    return new ImageData(rgbaData, renderWidth, renderHeight);
  }

  // ============================================================
  // Working Document Operations
  // ============================================================

  /**
   * Clear the working document
   */
  workClear() {
    this._module._nanopdf_work_clear();
  }

  /**
   * Get working document page count
   * @returns {number} Page count
   */
  workGetPageCount() {
    return this._module._nanopdf_work_get_page_count();
  }

  /**
   * Add a page from a loaded document to the working document
   * @param {number} docId - Source document ID
   * @param {number} pageIndex - Page index in source document
   * @returns {number} New page index in working document, or -1 on failure
   */
  workAddPage(docId, pageIndex) {
    return this._module._nanopdf_work_add_page(docId, pageIndex);
  }

  /**
   * Add all pages from a document to the working document
   * @param {number} docId - Source document ID
   * @returns {number} Index of first added page, or -1 on failure
   */
  workAddAllPages(docId) {
    return this._module._nanopdf_work_add_all_pages(docId);
  }

  /**
   * Remove a page from the working document
   * @param {number} pageIndex - Page index to remove
   * @returns {boolean} True if successful
   */
  workRemovePage(pageIndex) {
    return this._module._nanopdf_work_remove_page(pageIndex) === 1;
  }

  /**
   * Move a page in the working document
   * @param {number} fromIndex - Current page index
   * @param {number} toIndex - New page index
   * @returns {boolean} True if successful
   */
  workMovePage(fromIndex, toIndex) {
    return this._module._nanopdf_work_move_page(fromIndex, toIndex) === 1;
  }

  /**
   * Get working page dimensions
   * @param {number} pageIndex - Page index
   * @returns {{width: number, height: number}} Page dimensions
   */
  workGetPageSize(pageIndex) {
    return {
      width: this._module._nanopdf_work_get_page_width(pageIndex),
      height: this._module._nanopdf_work_get_page_height(pageIndex)
    };
  }

  /**
   * Get page rotation
   * @param {number} pageIndex - Page index
   * @returns {number} Rotation in degrees (0, 90, 180, 270)
   */
  workGetRotation(pageIndex) {
    return this._module._nanopdf_work_get_rotation(pageIndex);
  }

  /**
   * Rotate a page
   * @param {number} pageIndex - Page index
   * @param {number} angle - Rotation angle (0, 90, 180, 270)
   * @returns {boolean} True if successful
   */
  workRotatePage(pageIndex, angle) {
    return this._module._nanopdf_work_rotate_page(pageIndex, angle) === 1;
  }

  /**
   * Crop a page
   * @param {number} pageIndex - Page index
   * @param {number} x - Crop box X
   * @param {number} y - Crop box Y
   * @param {number} w - Crop box width
   * @param {number} h - Crop box height
   * @returns {boolean} True if successful
   */
  workCropPage(pageIndex, x, y, w, h) {
    return this._module._nanopdf_work_crop_page(pageIndex, x, y, w, h) === 1;
  }

  /**
   * Reset page crop to original dimensions
   * @param {number} pageIndex - Page index
   * @returns {boolean} True if successful
   */
  workResetCrop(pageIndex) {
    return this._module._nanopdf_work_reset_crop(pageIndex) === 1;
  }

  // ============================================================
  // Annotations
  // ============================================================

  /**
   * Add a text annotation
   * @param {number} pageIndex - Page index
   * @param {number} x - X position
   * @param {number} y - Y position
   * @param {string} text - Text content
   * @param {string} [fontName='Helvetica'] - Font name
   * @param {number} [fontSize=12] - Font size
   * @param {number} [r=0] - Red (0-1)
   * @param {number} [g=0] - Green (0-1)
   * @param {number} [b=0] - Blue (0-1)
   * @param {number} [a=1] - Alpha (0-1)
   * @returns {number} Annotation index, or -1 on failure
   */
  annotAddText(pageIndex, x, y, text, fontName = 'Helvetica', fontSize = 12, r = 0, g = 0, b = 0, a = 1) {
    const textPtr = this._module.allocateUTF8(text);
    const fontPtr = this._module.allocateUTF8(fontName);
    const result = this._module._nanopdf_annot_add_text(pageIndex, x, y, textPtr, fontPtr, fontSize, r, g, b, a);
    this._module._free(textPtr);
    this._module._free(fontPtr);
    return result;
  }

  /**
   * Add a line annotation
   * @param {number} pageIndex - Page index
   * @param {number} x1 - Start X
   * @param {number} y1 - Start Y
   * @param {number} x2 - End X
   * @param {number} y2 - End Y
   * @param {number} [lineWidth=1] - Line width
   * @param {number} [r=0] - Red (0-1)
   * @param {number} [g=0] - Green (0-1)
   * @param {number} [b=0] - Blue (0-1)
   * @param {number} [a=1] - Alpha (0-1)
   * @returns {number} Annotation index
   */
  annotAddLine(pageIndex, x1, y1, x2, y2, lineWidth = 1, r = 0, g = 0, b = 0, a = 1) {
    return this._module._nanopdf_annot_add_line(pageIndex, x1, y1, x2, y2, lineWidth, r, g, b, a);
  }

  /**
   * Add a rectangle annotation
   * @param {number} pageIndex - Page index
   * @param {number} x - X position
   * @param {number} y - Y position
   * @param {number} w - Width
   * @param {number} h - Height
   * @param {number} [lineWidth=1] - Line width
   * @param {number} [r=0] - Red (0-1)
   * @param {number} [g=0] - Green (0-1)
   * @param {number} [b=0] - Blue (0-1)
   * @param {number} [a=1] - Alpha (0-1)
   * @param {boolean} [filled=false] - Whether to fill the rectangle
   * @returns {number} Annotation index
   */
  annotAddRect(pageIndex, x, y, w, h, lineWidth = 1, r = 0, g = 0, b = 0, a = 1, filled = false) {
    return this._module._nanopdf_annot_add_rect(pageIndex, x, y, w, h, lineWidth, r, g, b, a, filled ? 1 : 0);
  }

  /**
   * Add an oval/ellipse annotation
   * @param {number} pageIndex - Page index
   * @param {number} cx - Center X
   * @param {number} cy - Center Y
   * @param {number} rx - Radius X
   * @param {number} ry - Radius Y
   * @param {number} [lineWidth=1] - Line width
   * @param {number} [r=0] - Red (0-1)
   * @param {number} [g=0] - Green (0-1)
   * @param {number} [b=0] - Blue (0-1)
   * @param {number} [a=1] - Alpha (0-1)
   * @param {boolean} [filled=false] - Whether to fill the oval
   * @returns {number} Annotation index
   */
  annotAddOval(pageIndex, cx, cy, rx, ry, lineWidth = 1, r = 0, g = 0, b = 0, a = 1, filled = false) {
    return this._module._nanopdf_annot_add_oval(pageIndex, cx, cy, rx, ry, lineWidth, r, g, b, a, filled ? 1 : 0);
  }

  /**
   * Add a highlight annotation
   * @param {number} pageIndex - Page index
   * @param {number} x - X position
   * @param {number} y - Y position
   * @param {number} w - Width
   * @param {number} h - Height
   * @param {number} [r=1] - Red (0-1)
   * @param {number} [g=1] - Green (0-1)
   * @param {number} [b=0] - Blue (0-1)
   * @param {number} [a=0.3] - Alpha (0-1)
   * @returns {number} Annotation index
   */
  annotAddHighlight(pageIndex, x, y, w, h, r = 1, g = 1, b = 0, a = 0.3) {
    return this._module._nanopdf_annot_add_highlight(pageIndex, x, y, w, h, r, g, b, a);
  }

  /**
   * Remove an annotation
   * @param {number} pageIndex - Page index
   * @param {number} annotIndex - Annotation index
   * @returns {boolean} True if successful
   */
  annotRemove(pageIndex, annotIndex) {
    return this._module._nanopdf_annot_remove(pageIndex, annotIndex) === 1;
  }

  /**
   * Clear all annotations on a page
   * @param {number} pageIndex - Page index
   */
  annotClearPage(pageIndex) {
    this._module._nanopdf_annot_clear_page(pageIndex);
  }

  /**
   * Get annotation count on a page
   * @param {number} pageIndex - Page index
   * @returns {number} Annotation count
   */
  annotGetCount(pageIndex) {
    return this._module._nanopdf_annot_get_count(pageIndex);
  }

  // ============================================================
  // PDF Export
  // ============================================================

  /**
   * Set export quality (affects embedded page image resolution)
   * @param {number} scale - Quality scale (1-4)
   *   1 = 72 DPI (fast, smaller files)
   *   2 = 144 DPI (good quality, default)
   *   3 = 216 DPI (high quality)
   *   4 = 288 DPI (maximum quality, large files)
   */
  exportSetQuality(scale) {
    this._module._nanopdf_export_set_quality(scale);
  }

  /**
   * Get current export quality setting
   * @returns {number} Quality scale (1-4)
   */
  exportGetQuality() {
    return this._module._nanopdf_export_get_quality();
  }

  /**
   * Export the working document to PDF
   * @returns {Uint8Array|null} PDF data, or null on failure
   */
  exportPDF() {
    const result = this._module._nanopdf_export_pdf();
    if (result !== 1) {
      console.error('Failed to export PDF:', this.getLastError());
      return null;
    }

    const bufferPtr = this._module._nanopdf_export_get_buffer();
    const bufferSize = this._module._nanopdf_export_get_size();

    if (!bufferPtr || bufferSize === 0) {
      console.error('Export buffer is empty');
      return null;
    }

    // Copy data from WASM memory
    return new Uint8Array(
      this._module.HEAPU8.buffer,
      bufferPtr,
      bufferSize
    ).slice();
  }

  /**
   * Export and download the working document as a PDF file
   * @param {string} [filename='document.pdf'] - Download filename
   * @returns {boolean} True if successful
   */
  exportAndDownload(filename = 'document.pdf') {
    const data = this.exportPDF();
    if (!data) return false;

    const blob = new Blob([data], { type: 'application/pdf' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    return true;
  }

  // ============================================================
  // CJK Font API
  // ============================================================

  /**
   * Load a CJK font from a URL (async fetch → blob registration)
   * @param {string} url - URL to the font file (.otf, .ttf, .ttc)
   * @param {number} [category=4] - Font category: 4 = CJK sans, 5 = CJK serif
   * @returns {Promise<boolean>} True if font was loaded and registered
   */
  async loadCJKFontFromURL(url, category = 4) {
    const response = await fetch(url);
    if (!response.ok) return false;
    const buffer = await response.arrayBuffer();
    return this.registerCJKFont(new Uint8Array(buffer), category);
  }

  /**
   * Register a CJK font from an ArrayBuffer or Uint8Array
   * @param {ArrayBuffer|Uint8Array} data - Font file data
   * @param {number} [category=4] - Font category: 4 = CJK sans, 5 = CJK serif
   * @returns {boolean} True if font was registered
   */
  registerCJKFont(data, category = 4) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data;
    const ptr = this._module._malloc(bytes.length);
    if (!ptr) return false;
    this._module.HEAPU8.set(bytes, ptr);
    const result = this._module._nanopdf_register_cjk_font(ptr, bytes.length, category);
    this._module._free(ptr);
    return result === 1;
  }

  /**
   * Check if CJK fonts are available (registered or embedded)
   * @returns {boolean} True if CJK fonts are ready for use
   */
  hasCJKFonts() {
    return this._module._nanopdf_cjk_fonts_ready() === 1;
  }
}

/**
 * Create a NanoPDF instance from the WASM module
 * @param {object} Module - Emscripten module
 * @returns {NanoPDF} NanoPDF instance
 */
export function createNanoPDF(Module) {
  const pdf = new NanoPDF(Module);
  pdf.init();
  return pdf;
}

/**
 * Load and initialize NanoPDF from a module URL
 * @param {string} [moduleUrl] - URL to the nanopdf.js module
 * @returns {Promise<NanoPDF>} Initialized NanoPDF instance
 */
export async function loadNanoPDF(moduleUrl = './nanopdf.js') {
  const { default: createModule } = await import(moduleUrl);
  const Module = await createModule();
  return createNanoPDF(Module);
}

export default NanoPDF;
