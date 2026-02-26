// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// External font loader for nanopdf WASM builds.
// Fetches fonts from a served directory and registers them via FontProvider.

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
 * Register a single font with the WASM module.
 * @param {Object} Module - Emscripten module
 * @param {ArrayBuffer} data - Font file data
 * @param {number} category - Font category (0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif)
 */
function registerFont(Module, data, category) {
  const bytes = new Uint8Array(data);
  const ptr = Module._malloc(bytes.byteLength);
  Module.HEAPU8.set(bytes, ptr);
  Module._nanopdf_register_font(ptr, bytes.byteLength, category);
  Module._free(ptr);
}

/**
 * Load and register standard (non-CJK) fonts.
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
      registerFont(Module, data, font.category);
      loaded++;
    } catch (e) {
      console.warn(`Failed to load font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / standard.length, font.name);
  }

  return loaded;
}

/**
 * Load and register CJK fonts (category 4 and 5).
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
      registerFont(Module, data, font.category);
      loaded++;
    } catch (e) {
      console.warn(`Failed to load CJK font ${font.name}:`, e);
    }
    onProgress?.((i + 1) / cjk.length, font.name);
  }

  return loaded;
}

/**
 * Load all fonts (standard + CJK).
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
      registerFont(Module, data, font.category);
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
