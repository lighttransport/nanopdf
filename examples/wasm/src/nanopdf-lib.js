// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// High-level nanopdf wrapper with automatic font loading (pdf.js-style).
//
// Usage (CDN fonts loaded by default — no self-hosting required):
//   import { createNanoPDF } from './nanopdf-lib.js';
//
//   const pdf = await createNanoPDF();   // fonts loaded from CDN by default
//
//   // pdf.module  — the raw Emscripten Module (for low-level calls)
//   // pdf.hasRendering  — boolean
//   // pdf.cjkFontsReady — Promise that resolves when CJK fonts finish loading
//
// Self-hosted fonts:
//   const pdf = await createNanoPDF({
//     standardFontDataUrl: '/fonts',   // like pdf.js's standardFontDataUrl
//   });

import createModule from 'nanopdf-wasm';
import wasmUrl from 'nanopdf-wasm-bin';
import {
  loadStandardFonts,
  loadCJKFonts,
  loadCDNStandardFonts,
  loadCDNCJKFonts,
} from './font-loader.js';

/**
 * Create and initialize a ready-to-use nanopdf instance with fonts loaded.
 *
 * When no font URL is specified, fonts are loaded from CDN using
 * @embedpdf/fonts-* packages on jsDelivr (same CDN used by embed-pdf-viewer).
 *
 * @param {Object} [options]
 * @param {string} [options.wasmUrl]             Override URL for nanopdf.wasm
 * @param {string} [options.standardFontDataUrl] Base URL for self-hosted font files (disables CDN)
 * @param {boolean} [options.enableCJKFonts]     Whether to load CJK fonts in the background (default: true)
 * @param {(msg: string) => void} [options.onProgress] Optional progress callback
 * @returns {Promise<{module: Object, hasRendering: boolean, cjkFontsReady: Promise<number>}>}
 */
export async function createNanoPDF(options = {}) {
  const {
    wasmUrl: customWasmUrl,
    standardFontDataUrl,
    enableCJKFonts = true,
    onProgress,
  } = options;

  const useCDN = !standardFontDataUrl;
  const resolvedWasmUrl = customWasmUrl || wasmUrl;

  // 1. Instantiate Emscripten module
  onProgress?.('Loading WASM module...');
  const Module = await createModule({
    locateFile: (path) => {
      if (path.endsWith('.wasm')) return resolvedWasmUrl;
      return path;
    },
  });

  const result = Module._nanopdf_init();
  if (result !== 1) {
    throw new Error('Failed to initialize nanopdf');
  }

  const hasRendering = Module._nanopdf_has_rendering() === 1;

  // 2. Load fonts based on build type
  const embeddedFontsAvailable = Module._nanopdf_fonts_available() === 1;

  if (embeddedFontsAvailable) {
    // Embedded build: eagerly register all embedded fonts with FontProvider
    onProgress?.('Registering embedded fonts...');
    const count = Module._nanopdf_register_embedded_fonts();
    console.log(`Registered ${count} embedded fonts with FontProvider`);
  } else if (useCDN) {
    // CDN mode (default): fetch from @embedpdf/fonts-* packages on jsDelivr
    onProgress?.('Loading fonts from CDN...');
    try {
      const stdCount = await loadCDNStandardFonts(Module, (ratio, name) => {
        onProgress?.(`Loading font: ${name} (${Math.round(ratio * 100)}%)`);
      });
      console.log(`Loaded ${stdCount} standard fonts from CDN (@embedpdf/fonts-latin)`);
    } catch (e) {
      console.warn('CDN fonts not available:', e.message);
    }
  } else {
    // Self-hosted mode: fetch from standardFontDataUrl with manifest
    onProgress?.(`Loading fonts from ${standardFontDataUrl}...`);
    try {
      const stdCount = await loadStandardFonts(Module, standardFontDataUrl, (ratio, name) => {
        onProgress?.(`Loading font: ${name} (${Math.round(ratio * 100)}%)`);
      });
      console.log(`Loaded ${stdCount} standard fonts from ${standardFontDataUrl}`);
    } catch (e) {
      console.warn('External fonts not available:', e.message);
    }
  }

  // 3. Start CJK loading in background (non-blocking)
  let cjkFontsReady;
  if (enableCJKFonts && !embeddedFontsAvailable) {
    const cjkLoader = useCDN
      ? loadCDNCJKFonts(Module, (ratio, name) => {
          console.log(`CJK font: ${name} (${Math.round(ratio * 100)}%)`);
        })
      : loadCJKFonts(Module, standardFontDataUrl, (ratio, name) => {
          console.log(`CJK font: ${name} (${Math.round(ratio * 100)}%)`);
        });
    cjkFontsReady = cjkLoader.then(count => {
      if (count > 0) {
        const source = useCDN ? 'CDN (@embedpdf/fonts-jp)' : standardFontDataUrl;
        console.log(`Loaded ${count} CJK fonts from ${source}`);
      }
      return count;
    }).catch(e => {
      console.log('CJK fonts not available:', e.message);
      return 0;
    });
  } else {
    cjkFontsReady = Promise.resolve(0);
  }

  onProgress?.('Ready');

  return {
    module: Module,
    hasRendering,
    cjkFontsReady,
  };
}
