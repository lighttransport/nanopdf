import { defineConfig } from 'vite';
import { resolve } from 'path';

const srcDir = resolve(__dirname, 'src');

export default defineConfig({
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
