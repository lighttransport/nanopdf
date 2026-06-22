import { defineConfig } from 'vite';
import { resolve } from 'path';

const srcDir = resolve(__dirname, 'src');

export default defineConfig({
  // Base public path. Defaults to '/' for local dev/preview; the GitHub Pages
  // deploy workflow sets VITE_BASE=/nanopdf/ to match the project-pages URL
  // https://lighttransport.github.io/nanopdf/ .
  base: process.env.VITE_BASE || '/',
  resolve: {
    alias: {
      // Allow `import createModule from 'nanopdf-wasm'` in source
      'nanopdf-wasm': resolve(srcDir, 'nanopdf.js'),
      // Import .wasm file as a URL asset
      'nanopdf-wasm-bin': resolve(srcDir, 'nanopdf.wasm?url'),
    },
  },
  optimizeDeps: {
    // Emscripten glue code must not be pre-bundled
    exclude: ['nanopdf-wasm'],
  },
  build: {
    target: 'es2020',
    outDir: 'dist',
  },
});
