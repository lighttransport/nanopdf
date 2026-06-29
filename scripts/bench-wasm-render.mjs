#!/usr/bin/env node
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { performance } from 'node:perf_hooks';

const [modulePath, pdfPath = 'data/dcsdd.pdf'] = process.argv.slice(2);
if (!modulePath) {
  console.error('usage: node scripts/bench-wasm-render.mjs <nanopdf.js> [pdf]');
  process.exit(2);
}

const createModule = (await import(pathToFileURL(modulePath))).default;
const moduleDir = new URL('.', pathToFileURL(modulePath));
const Module = await createModule({
  locateFile: (path) => path.endsWith('.wasm') ? new URL(path, moduleDir).pathname : path,
});

function wasmString(ptr) {
  return ptr ? Module.UTF8ToString(ptr) : '';
}

function copyToWasm(bytes) {
  const ptr = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, ptr);
  return ptr;
}

function stats(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const sum = values.reduce((a, b) => a + b, 0);
  return {
    mean: sum / values.length,
    median: sorted[Math.floor(sorted.length / 2)],
    min: sorted[0],
    max: sorted[sorted.length - 1],
  };
}

function fmt(value) {
  return Number(value).toFixed(2);
}

if (Module._nanopdf_init() !== 1) {
  throw new Error('nanopdf_init failed');
}

const pdfBytes = readFileSync(pdfPath);
let ptr = copyToWasm(pdfBytes);
const loadStart = performance.now();
const pageCount = Module._nanopdf_load_pdf(ptr, pdfBytes.length);
const loadMs = performance.now() - loadStart;
Module._free(ptr);

if (pageCount <= 0) {
  throw new Error(`load failed: ${wasmString(Module._nanopdf_get_last_error?.() || 0)}`);
}

const probe = {
  simd: Module._nanopdf_wasm_simd_enabled ? Module._nanopdf_wasm_simd_enabled() : 0,
  fpnge: Module._nanopdf_fpnge_available ? Module._nanopdf_fpnge_available() : 0,
  fpngeIsa: Module._nanopdf_fpnge_active_isa
    ? wasmString(Module._nanopdf_fpnge_active_isa())
    : 'none',
};

const pages = [0, 4, 6, 7].filter((p) => p < pageCount);
const jobs = pages.map((page, i) => {
  const widthPt = Module._nanopdf_get_page_width(page);
  const heightPt = Module._nanopdf_get_page_height(page);
  const scale = i % 2 === 0 ? 1.5 : 1.75;
  return {
    page,
    width: Math.max(1, Math.round(widthPt * scale)),
    height: Math.max(1, Math.round(heightPt * scale)),
    dpi: 72 * scale,
  };
});

// Warm up code paths, allocations, font lookup, and backend creation. The
// measured loop still alternates pages, so the single-page cache is avoided.
for (let i = 0; i < jobs.length * 2; i++) {
  const job = jobs[i % jobs.length];
  if (Module._nanopdf_render_page(job.page, job.width, job.height, job.dpi) !== 1) {
    throw new Error(`warmup render failed: ${wasmString(Module._nanopdf_get_last_error?.() || 0)}`);
  }
  Module._nanopdf_release_render_buffer?.();
}

const samples = [];
const bytes = [];
const iterations = 24;
for (let i = 0; i < iterations; i++) {
  const job = jobs[i % jobs.length];
  const start = performance.now();
  if (Module._nanopdf_render_page(job.page, job.width, job.height, job.dpi) !== 1) {
    throw new Error(`render failed: ${wasmString(Module._nanopdf_get_last_error?.() || 0)}`);
  }
  samples.push(performance.now() - start);
  bytes.push(Module._nanopdf_get_render_buffer_size?.() || 0);
  Module._nanopdf_release_render_buffer?.();
}

const render = stats(samples);
const mpix = jobs.reduce((sum, job) => sum + job.width * job.height, 0) / jobs.length / 1e6;
console.log(JSON.stringify({
  modulePath,
  pdfPath,
  pageCount,
  loadMs,
  probe,
  jobs,
  avgMPixPerRender: mpix,
  iterations,
  renderMs: render,
  avgOutputBytes: bytes.reduce((a, b) => a + b, 0) / bytes.length,
}, null, 2));

Module._nanopdf_shutdown?.();
