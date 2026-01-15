// nanopdf.js - JavaScript wrapper for nanopdf WASM module
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

/**
 * NanoPDF - PDF manipulation library
 */
class NanoPDF {
  constructor(module) {
    this.module = module;
    this._initialized = false;
  }

  /**
   * Initialize the WASM module
   */
  init() {
    if (this._initialized) return true;
    const result = this.module._nanopdf_init();
    this._initialized = result === 1;
    return this._initialized;
  }

  /**
   * Shutdown and cleanup
   */
  shutdown() {
    if (this._initialized) {
      this.module._nanopdf_shutdown();
      this._initialized = false;
    }
  }

  /**
   * Get the last error message
   */
  getLastError() {
    const ptr = this.module._nanopdf_get_last_error();
    return this.module.UTF8ToString(ptr);
  }

  /**
   * Check if rendering backend is available
   */
  hasRendering() {
    return this.module._nanopdf_has_rendering() === 1;
  }

  // ============================================================
  // Simple PDF Loading (for quick viewing)
  // ============================================================

  /**
   * Load a PDF for viewing
   * @param {ArrayBuffer|Uint8Array} data - PDF file data
   * @returns {number} Number of pages, or 0 on error
   */
  loadPDF(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data;
    const ptr = this.module._nanopdf_malloc(bytes.length);
    this.module.HEAPU8.set(bytes, ptr);
    const pageCount = this.module._nanopdf_load_pdf(ptr, bytes.length);
    this.module._nanopdf_free(ptr);
    return pageCount;
  }

  /**
   * Get page count of loaded PDF
   */
  getPageCount() {
    return this.module._nanopdf_get_page_count();
  }

  /**
   * Get page dimensions
   */
  getPageSize(pageIndex) {
    return {
      width: this.module._nanopdf_get_page_width(pageIndex),
      height: this.module._nanopdf_get_page_height(pageIndex)
    };
  }

  /**
   * Render a page to ImageData
   */
  renderPage(pageIndex, width, height, dpi = 72) {
    if (!this.module._nanopdf_render_page(pageIndex, width, height, dpi)) {
      return null;
    }

    const bufPtr = this.module._nanopdf_get_render_buffer();
    const bufSize = this.module._nanopdf_get_render_buffer_size();
    const w = this.module._nanopdf_get_render_width();
    const h = this.module._nanopdf_get_render_height();

    if (!bufPtr || bufSize === 0) return null;

    const pixels = new Uint8ClampedArray(this.module.HEAPU8.buffer, bufPtr, bufSize);
    return new ImageData(new Uint8ClampedArray(pixels), w, h);
  }

  /**
   * Render page directly to canvas
   */
  renderToCanvas(pageIndex, canvas, dpi = 72) {
    const pageSize = this.getPageSize(pageIndex);
    const scale = dpi / 72;
    const width = Math.floor(pageSize.width * scale);
    const height = Math.floor(pageSize.height * scale);

    canvas.width = width;
    canvas.height = height;

    const imageData = this.renderPage(pageIndex, width, height, dpi);
    if (!imageData) return false;

    const ctx = canvas.getContext('2d');
    ctx.putImageData(imageData, 0, 0);
    return true;
  }

  /**
   * Extract text from a page
   */
  extractText(pageIndex) {
    const ptr = this.module._nanopdf_extract_text(pageIndex);
    return this.module.UTF8ToString(ptr);
  }

  // ============================================================
  // Document Management (for editing/concatenation)
  // ============================================================

  /**
   * Load a PDF document for editing
   * @returns {number} Document ID, or -1 on error
   */
  docLoad(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data;
    const ptr = this.module._nanopdf_malloc(bytes.length);
    this.module.HEAPU8.set(bytes, ptr);
    const docId = this.module._nanopdf_doc_load(ptr, bytes.length);
    this.module._nanopdf_free(ptr);
    return docId;
  }

  /**
   * Get page count in a loaded document
   */
  docGetPageCount(docId) {
    return this.module._nanopdf_doc_get_page_count(docId);
  }

  /**
   * Get page dimensions from a loaded document
   */
  docGetPageSize(docId, pageIndex) {
    return {
      width: this.module._nanopdf_doc_get_page_width(docId, pageIndex),
      height: this.module._nanopdf_doc_get_page_height(docId, pageIndex)
    };
  }

  /**
   * Close a loaded document
   */
  docClose(docId) {
    this.module._nanopdf_doc_close(docId);
  }

  /**
   * Render a page from a loaded document
   */
  docRenderPage(docId, pageIndex, width, height, dpi = 72) {
    if (!this.module._nanopdf_doc_render_page(docId, pageIndex, width, height, dpi)) {
      return null;
    }

    const bufPtr = this.module._nanopdf_get_render_buffer();
    const bufSize = this.module._nanopdf_get_render_buffer_size();
    const w = this.module._nanopdf_get_render_width();
    const h = this.module._nanopdf_get_render_height();

    if (!bufPtr || bufSize === 0) return null;

    const pixels = new Uint8ClampedArray(this.module.HEAPU8.buffer, bufPtr, bufSize);
    return new ImageData(new Uint8ClampedArray(pixels), w, h);
  }

  // ============================================================
  // Working Document Operations
  // ============================================================

  /**
   * Clear working document
   */
  workClear() {
    this.module._nanopdf_work_clear();
  }

  /**
   * Get working document page count
   */
  workGetPageCount() {
    return this.module._nanopdf_work_get_page_count();
  }

  /**
   * Add a page from a loaded document to the working document
   * @returns {number} New page index in working document, or -1 on error
   */
  workAddPage(docId, pageIndex) {
    return this.module._nanopdf_work_add_page(docId, pageIndex);
  }

  /**
   * Add all pages from a document to the working document
   * @returns {number} First page index, or -1 on error
   */
  workAddAllPages(docId) {
    return this.module._nanopdf_work_add_all_pages(docId);
  }

  /**
   * Remove a page from the working document
   */
  workRemovePage(pageIndex) {
    return this.module._nanopdf_work_remove_page(pageIndex) === 1;
  }

  /**
   * Move a page in the working document
   */
  workMovePage(fromIndex, toIndex) {
    return this.module._nanopdf_work_move_page(fromIndex, toIndex) === 1;
  }

  /**
   * Get working page dimensions
   */
  workGetPageSize(pageIndex) {
    return {
      width: this.module._nanopdf_work_get_page_width(pageIndex),
      height: this.module._nanopdf_work_get_page_height(pageIndex)
    };
  }

  /**
   * Get page rotation
   */
  workGetRotation(pageIndex) {
    return this.module._nanopdf_work_get_rotation(pageIndex);
  }

  /**
   * Rotate a page (0, 90, 180, 270)
   */
  workRotatePage(pageIndex, angle) {
    return this.module._nanopdf_work_rotate_page(pageIndex, angle) === 1;
  }

  /**
   * Crop a page
   */
  workCropPage(pageIndex, x, y, w, h) {
    return this.module._nanopdf_work_crop_page(pageIndex, x, y, w, h) === 1;
  }

  /**
   * Reset page crop
   */
  workResetCrop(pageIndex) {
    return this.module._nanopdf_work_reset_crop(pageIndex) === 1;
  }

  // ============================================================
  // Annotations
  // ============================================================

  /**
   * Add text annotation
   * @returns {number} Annotation index, or -1 on error
   */
  addTextAnnotation(pageIndex, x, y, text, options = {}) {
    const fontName = options.fontName || 'Helvetica';
    const fontSize = options.fontSize || 12;
    const r = options.r ?? 0;
    const g = options.g ?? 0;
    const b = options.b ?? 0;
    const a = options.a ?? 1;

    const textPtr = this.module.allocateUTF8(text);
    const fontPtr = this.module.allocateUTF8(fontName);

    const result = this.module._nanopdf_annot_add_text(
      pageIndex, x, y, textPtr, fontPtr, fontSize, r, g, b, a
    );

    this.module._free(textPtr);
    this.module._free(fontPtr);

    return result;
  }

  /**
   * Add line annotation
   * @returns {number} Annotation index, or -1 on error
   */
  addLineAnnotation(pageIndex, x1, y1, x2, y2, options = {}) {
    const lineWidth = options.lineWidth || 1;
    const r = options.r ?? 0;
    const g = options.g ?? 0;
    const b = options.b ?? 0;
    const a = options.a ?? 1;

    return this.module._nanopdf_annot_add_line(
      pageIndex, x1, y1, x2, y2, lineWidth, r, g, b, a
    );
  }

  /**
   * Add rectangle annotation
   * @returns {number} Annotation index, or -1 on error
   */
  addRectAnnotation(pageIndex, x, y, w, h, options = {}) {
    const lineWidth = options.lineWidth || 1;
    const r = options.r ?? 0;
    const g = options.g ?? 0;
    const b = options.b ?? 0;
    const a = options.a ?? 1;
    const filled = options.filled ? 1 : 0;

    return this.module._nanopdf_annot_add_rect(
      pageIndex, x, y, w, h, lineWidth, r, g, b, a, filled
    );
  }

  /**
   * Add oval/ellipse annotation
   * @returns {number} Annotation index, or -1 on error
   */
  addOvalAnnotation(pageIndex, cx, cy, rx, ry, options = {}) {
    const lineWidth = options.lineWidth || 1;
    const r = options.r ?? 0;
    const g = options.g ?? 0;
    const b = options.b ?? 0;
    const a = options.a ?? 1;
    const filled = options.filled ? 1 : 0;

    return this.module._nanopdf_annot_add_oval(
      pageIndex, cx, cy, rx, ry, lineWidth, r, g, b, a, filled
    );
  }

  /**
   * Add highlight annotation
   * @returns {number} Annotation index, or -1 on error
   */
  addHighlightAnnotation(pageIndex, x, y, w, h, options = {}) {
    const r = options.r ?? 1;
    const g = options.g ?? 1;
    const b = options.b ?? 0;
    const a = options.a ?? 0.3;

    return this.module._nanopdf_annot_add_highlight(
      pageIndex, x, y, w, h, r, g, b, a
    );
  }

  /**
   * Remove an annotation
   */
  removeAnnotation(pageIndex, annotIndex) {
    return this.module._nanopdf_annot_remove(pageIndex, annotIndex) === 1;
  }

  /**
   * Clear all annotations on a page
   */
  clearPageAnnotations(pageIndex) {
    this.module._nanopdf_annot_clear_page(pageIndex);
  }

  /**
   * Get annotation count on a page
   */
  getAnnotationCount(pageIndex) {
    return this.module._nanopdf_annot_get_count(pageIndex);
  }

  // ============================================================
  // PDF Export
  // ============================================================

  /**
   * Export working document to PDF
   * @returns {Uint8Array|null} PDF data, or null on error
   */
  exportPDF() {
    if (!this.module._nanopdf_export_pdf()) {
      return null;
    }

    const bufPtr = this.module._nanopdf_export_get_buffer();
    const bufSize = this.module._nanopdf_export_get_size();

    if (!bufPtr || bufSize === 0) return null;

    // Copy the data (important - don't just create a view)
    const data = new Uint8Array(bufSize);
    data.set(new Uint8Array(this.module.HEAPU8.buffer, bufPtr, bufSize));

    return data;
  }

  /**
   * Export and trigger download
   */
  exportAndDownload(filename = 'document.pdf') {
    const data = this.exportPDF();
    if (!data) return false;

    const blob = new Blob([data], { type: 'application/pdf' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
    return true;
  }
}

/**
 * Create NanoPDF instance from Emscripten module
 */
function createNanoPDF(Module) {
  return new NanoPDF(Module);
}

/**
 * Load NanoPDF WASM module
 */
async function loadNanoPDF(moduleUrl = './nanopdf.js') {
  const NanoPDFModule = (await import(moduleUrl)).default;
  const module = await NanoPDFModule();
  return createNanoPDF(module);
}

// Export for both ES modules and script tags
if (typeof window !== 'undefined') {
  window.NanoPDF = NanoPDF;
  window.createNanoPDF = createNanoPDF;
  window.loadNanoPDF = loadNanoPDF;
}

export { NanoPDF, createNanoPDF, loadNanoPDF };
