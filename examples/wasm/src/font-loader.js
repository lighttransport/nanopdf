// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// External font loader for nanopdf WASM builds.
// Fetches fonts from a served directory or CDN and registers them via FontProvider.

// ============================================================================
// Built-in CDN font manifest using @embedpdf/fonts-* packages on jsDelivr.
// Same CDN packages used by embed-pdf-viewer.
// ============================================================================

const CDN_BASE = 'https://cdn.jsdelivr.net/npm';

// Standard (Latin) fonts — Noto Sans from @embedpdf/fonts-latin
// 4 key variants: Regular, Italic, Bold, BoldItalic
const CDN_STANDARD_FONTS = [
  // Sans-serif (category 0) — primary fallback for Helvetica/Arial
  { name: 'NotoSans-Regular',    url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Regular.ttf`,    category: 0, weight: 400, italic: false },
  { name: 'NotoSans-Italic',     url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Italic.ttf`,     category: 0, weight: 400, italic: true },
  { name: 'NotoSans-Bold',       url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Bold.ttf`,       category: 0, weight: 700, italic: false },
  { name: 'NotoSans-BoldItalic', url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-BoldItalic.ttf`, category: 0, weight: 700, italic: true },
  // Mono (category 1) — reuse Noto Sans as fallback (no mono package available)
  { name: 'NotoSans-Mono-Regular', url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Regular.ttf`, category: 1, weight: 400, italic: false },
  { name: 'NotoSans-Mono-Bold',    url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Bold.ttf`,    category: 1, weight: 700, italic: false },
  // Serif (category 2) — reuse Noto Sans as fallback (no serif package available)
  { name: 'NotoSans-Serif-Regular',    url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Regular.ttf`,    category: 2, weight: 400, italic: false },
  { name: 'NotoSans-Serif-Italic',     url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Italic.ttf`,     category: 2, weight: 400, italic: true },
  { name: 'NotoSans-Serif-Bold',       url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-Bold.ttf`,       category: 2, weight: 700, italic: false },
  { name: 'NotoSans-Serif-BoldItalic', url: `${CDN_BASE}/@embedpdf/fonts-latin@latest/fonts/NotoSans-BoldItalic.ttf`, category: 2, weight: 700, italic: true },
];

// CJK fonts from @embedpdf/fonts-jp, @embedpdf/fonts-kr, @embedpdf/fonts-sc, @embedpdf/fonts-tc
// Regular + Bold for Japanese (primary CJK fallback)
const CDN_CJK_FONTS = [
  { name: 'NotoSansJP-Regular', url: `${CDN_BASE}/@embedpdf/fonts-jp@latest/fonts/NotoSansJP-Regular.otf`, category: 4, weight: 400, italic: false },
  { name: 'NotoSansJP-Bold',    url: `${CDN_BASE}/@embedpdf/fonts-jp@latest/fonts/NotoSansJP-Bold.otf`,    category: 4, weight: 700, italic: false },
];

/**
 * Load the font manifest from the given base path.
 * @param {string} basePath - URL path to the fonts directory (e.g., '/fonts')
 * @returns {Promise<Object>} The parsed manifest
 */
export async function loadManifest(basePath) {
  const res = await fetch(`${basePath}/fonts-manifest.json`);
  if (!res.ok) throw new Error(`Failed to load font manifest: ${res.status}`);
  return res.json();
}

/**
 * Register a single font with the WASM module (legacy, weight=400, italic=false).
 * @param {Object} Module - Emscripten module
 * @param {ArrayBuffer} data - Font file data
 * @param {number} category - Font category (0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif)
 */
export function registerFont(Module, data, category) {
  const bytes = new Uint8Array(data);
  const ptr = Module._malloc(bytes.byteLength);
  Module.HEAPU8.set(bytes, ptr);
  Module._nanopdf_register_font(ptr, bytes.byteLength, category);
  Module._free(ptr);
}

/**
 * Register a single font with weight and italic metadata.
 * @param {Object} Module - Emscripten module
 * @param {ArrayBuffer} data - Font file data
 * @param {number} category - Font category (0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif)
 * @param {number} weight - CSS font weight 100-900 (default 400)
 * @param {boolean} italic - Whether this is an italic font (default false)
 */
export function registerFontEx(Module, data, category, weight = 400, italic = false) {
  const bytes = new Uint8Array(data);
  const ptr = Module._malloc(bytes.byteLength);
  Module.HEAPU8.set(bytes, ptr);
  Module._nanopdf_register_font_ex(ptr, bytes.byteLength, category, weight, italic ? 1 : 0);
  Module._free(ptr);
}

/**
 * Fetch a font from a URL and register it.
 * @param {Object} Module - Emscripten module
 * @param {Object} font - Font descriptor { name, url, category, weight, italic }
 * @returns {Promise<boolean>} Whether the font was loaded successfully
 */
async function fetchAndRegister(Module, font) {
  const res = await fetch(font.url);
  if (!res.ok) {
    console.warn(`Failed to fetch font ${font.name}: ${res.status}`);
    return false;
  }
  const data = await res.arrayBuffer();
  registerFontEx(Module, data, font.category, font.weight ?? 400, font.italic ?? false);
  return true;
}

/**
 * Load and register standard (non-CJK) fonts from CDN (@embedpdf/fonts-* packages).
 * No manifest fetch needed — uses built-in CDN font list.
 * @param {Object} Module - Emscripten module
 * @param {Function} [onProgress] - Progress callback (ratio: number, name: string)
 * @returns {Promise<number>} Number of fonts loaded
 */
export async function loadCDNStandardFonts(Module, onProgress) {
  let loaded = 0;
  for (const [i, font] of CDN_STANDARD_FONTS.entries()) {
    try {
      if (await fetchAndRegister(Module, font)) loaded++;
    } catch (e) {
      console.warn(`Failed to load CDN font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / CDN_STANDARD_FONTS.length, font.name);
  }
  return loaded;
}

/**
 * Load and register CJK fonts from CDN (@embedpdf/fonts-jp).
 * @param {Object} Module - Emscripten module
 * @param {Function} [onProgress] - Progress callback (ratio: number, name: string)
 * @returns {Promise<number>} Number of fonts loaded
 */
export async function loadCDNCJKFonts(Module, onProgress) {
  let loaded = 0;
  for (const [i, font] of CDN_CJK_FONTS.entries()) {
    try {
      if (await fetchAndRegister(Module, font)) loaded++;
    } catch (e) {
      console.warn(`Failed to load CDN CJK font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / CDN_CJK_FONTS.length, font.name);
  }
  return loaded;
}

/**
 * Load and register standard (non-CJK) fonts from a self-hosted directory.
 * @param {Object} Module - Emscripten module
 * @param {string} basePath - URL path to the fonts directory
 * @param {Function} [onProgress] - Progress callback (ratio: number, name: string)
 * @returns {Promise<number>} Number of fonts loaded
 */
export async function loadStandardFonts(Module, basePath, onProgress) {
  const manifest = await loadManifest(basePath);
  const standard = manifest.fonts.filter(f => f.category <= 3);

  let loaded = 0;
  for (const [i, font] of standard.entries()) {
    try {
      const res = await fetch(`${basePath}/${font.file}`);
      if (!res.ok) {
        console.warn(`Failed to fetch font ${font.file}: ${res.status}`);
        continue;
      }
      const data = await res.arrayBuffer();
      const weight = font.weight ?? 400;
      const italic = font.italic ?? false;
      registerFontEx(Module, data, font.category, weight, italic);
      loaded++;
    } catch (e) {
      console.warn(`Failed to load font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / standard.length, font.name);
  }

  return loaded;
}

/**
 * Load and register CJK fonts (category 4 and 5) from a self-hosted directory.
 * @param {Object} Module - Emscripten module
 * @param {string} basePath - URL path to the fonts directory
 * @param {Function} [onProgress] - Progress callback (ratio: number, name: string)
 * @returns {Promise<number>} Number of fonts loaded
 */
export async function loadCJKFonts(Module, basePath, onProgress) {
  const manifest = await loadManifest(basePath);
  const cjk = manifest.fonts.filter(f => f.category >= 4);

  let loaded = 0;
  for (const [i, font] of cjk.entries()) {
    try {
      const res = await fetch(`${basePath}/${font.file}`);
      if (!res.ok) {
        console.warn(`Failed to fetch CJK font ${font.file}: ${res.status}`);
        continue;
      }
      const data = await res.arrayBuffer();
      const weight = font.weight ?? 400;
      const italic = font.italic ?? false;
      registerFontEx(Module, data, font.category, weight, italic);
      loaded++;
    } catch (e) {
      console.warn(`Failed to load CJK font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / cjk.length, font.name);
  }

  return loaded;
}

/**
 * Load all fonts (standard + CJK) from a self-hosted directory.
 * @param {Object} Module - Emscripten module
 * @param {string} basePath - URL path to the fonts directory
 * @param {Function} [onProgress] - Progress callback (ratio: number, name: string, phase: string)
 * @returns {Promise<{standard: number, cjk: number}>} Number of fonts loaded
 */
export async function loadAllFonts(Module, basePath, onProgress) {
  const manifest = await loadManifest(basePath);
  const all = manifest.fonts;
  const total = all.length;

  let standard = 0;
  let cjk = 0;
  for (const [i, font] of all.entries()) {
    try {
      const res = await fetch(`${basePath}/${font.file}`);
      if (!res.ok) {
        console.warn(`Failed to fetch font ${font.file}: ${res.status}`);
        continue;
      }
      const data = await res.arrayBuffer();
      const weight = font.weight ?? 400;
      const italic = font.italic ?? false;
      registerFontEx(Module, data, font.category, weight, italic);
      if (font.category >= 4) cjk++;
      else standard++;
    } catch (e) {
      console.warn(`Failed to load font ${font.name}:`, e);
    }
    const phase = font.category >= 4 ? 'cjk' : 'standard';
    onProgress?.((i + 1) / total, font.name, phase);
  }

  return { standard, cjk };
}
