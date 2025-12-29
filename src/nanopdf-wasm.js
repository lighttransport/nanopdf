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
   * Get the last error message
   * @returns {string} Error message
   */
  getLastError() {
    const ptr = this._module._nanopdf_get_last_error();
    return this._module.UTF8ToString(ptr);
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
