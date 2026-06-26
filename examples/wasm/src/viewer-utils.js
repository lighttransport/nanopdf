const HTML_ESCAPE = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
};

export function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (ch) => HTML_ESCAPE[ch]);
}

export function formatFileSize(bytes) {
  const size = Number.isFinite(Number(bytes)) && Number(bytes) > 0 ? Number(bytes) : 0;
  if (size < 1024) return size + ' B';
  if (size < 1024 * 1024) return (size / 1024).toFixed(1) + ' KB';
  return (size / (1024 * 1024)).toFixed(1) + ' MB';
}

export function formatPdfDate(value) {
  if (!value) return '';
  const match = /^D:(\d{4})(\d{2})?(\d{2})?(\d{2})?(\d{2})?(\d{2})?/.exec(value);
  if (!match) return String(value);
  const [, year, month = '01', day = '01', hour = '00', minute = '00', second = '00'] = match;
  return `${year}-${month}-${day} ${hour}:${minute}:${second}`;
}

export function getMdpPermissionLabel(value) {
  const permission = Number(value);
  switch (permission) {
    case 1: return 'No changes allowed';
    case 2: return 'Form fill and sign only';
    case 3: return 'Form fill, sign, annotate';
    default: return Number.isFinite(permission) && permission !== 0
      ? `Unknown (${permission})`
      : 'Not specified';
  }
}

export function computeRenderSize(pageWidth, pageHeight, scale, maxDim = 4096) {
  const safeWidth = Number.isFinite(pageWidth) && pageWidth > 0 ? pageWidth : 1;
  const safeHeight = Number.isFinite(pageHeight) && pageHeight > 0 ? pageHeight : 1;
  const safeScale = Number.isFinite(scale) && scale > 0 ? scale : 1;
  const safeMaxDim = Number.isFinite(maxDim) && maxDim > 0 ? maxDim : 4096;
  const rawWidth = Math.max(1, Math.round(safeWidth * safeScale));
  const rawHeight = Math.max(1, Math.round(safeHeight * safeScale));
  const capScale = Math.min(1, safeMaxDim / rawWidth, safeMaxDim / rawHeight);
  return {
    width: Math.max(1, Math.round(rawWidth * capScale)),
    height: Math.max(1, Math.round(rawHeight * capScale)),
  };
}

export function chooseSelectedExportRoute({ hasRedaction, protect, hasBakeable, canSplit }) {
  if (hasRedaction) return 'burn-redaction';
  if (protect || hasBakeable) return 'wasm-flatten';
  return canSplit ? 'vector-split' : 'unavailable';
}

export function resolvePdfDeepLink(search, hash) {
  const params = new URLSearchParams(search || '');
  const hashText = (hash || '').replace(/^#/, '');
  const hashParams = new URLSearchParams(hashText);
  return params.get('pdf') || hashParams.get('pdf') || hashParams.get('url') || '';
}

export function shouldRenderPageStatus(now, holdUntil) {
  const current = Number.isFinite(now) ? now : 0;
  const hold = Number.isFinite(holdUntil) ? holdUntil : 0;
  return current >= hold;
}

// Emscripten's UTF8ToString(0) is undefined behavior; treat a null/falsy ptr
// as the empty string. Centralized here so all WASM string reads are safe.
export function readWasmString(Module, ptr) {
  return ptr ? Module.UTF8ToString(ptr) : '';
}

// Read a render page into a fresh ImageData. Centralizes the
// "render -> copy HEAPU8 -> createImageData" sequence used by 5 sites.
// Returns { ok, imageData, w, h, error }. On failure imageData is null and
// error carries the C-side message (or thrown message).
export function renderPageIntoImageData(Module, pageIdx, width, height, dpi) {
  let ok = 0;
  try {
    ok = Module._nanopdf_render_page(pageIdx, width, height, dpi);
  } catch (e) {
    return { ok: false, imageData: null, w: 0, h: 0, error: e?.message || String(e) };
  }
  if (ok !== 1) {
    const error = Module._nanopdf_get_last_error
      ? Module.UTF8ToString(Module._nanopdf_get_last_error())
      : '';
    return { ok: false, imageData: null, w: 0, h: 0, error };
  }
  const ptr = Module._nanopdf_get_render_buffer();
  const size = Module._nanopdf_get_render_buffer_size();
  const w = Module._nanopdf_get_render_width();
  const h = Module._nanopdf_get_render_height();
  if (!ptr || size <= 0) {
    return { ok: false, imageData: null, w, h, error: 'empty render buffer' };
  }
  // Detach from the WASM heap so subsequent renders can't clobber us.
  const data = new Uint8ClampedArray(size);
  data.set(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, size));
  if (Module._nanopdf_release_render_buffer) {
    Module._nanopdf_release_render_buffer();
  }
  return { ok: true, imageData: { data, w, h }, w, h, error: '' };
}

// Validate a WASM operation's ok flag and throw with the C-side error
// message. Use before reading any *_get_buffer / *_get_size pair, since those
// output pointers are documented as valid only on success.
export function assertOkOrThrow(ok, opName, Module) {
  if (ok === 1 || ok === true) return;
  const detail = Module && Module._nanopdf_get_last_error
    ? Module.UTF8ToString(Module._nanopdf_get_last_error())
    : '';
  throw new Error(detail ? `${opName}: ${detail}` : `${opName} failed`);
}

// Escape a literal string for use inside a RegExp constructor.
export function escapeRegExp(s) {
  return String(s ?? '').replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Wrap occurrences of `term` (case-insensitive) in a string with <mark> tags.
// The input is HTML-escaped first so the output is safe to drop into innerHTML.
export function highlightContext(context, term) {
  const safe = escapeHtml(context || '');
  if (!term) return safe;
  const re = new RegExp(escapeRegExp(escapeHtml(term)), 'gi');
  return safe.replace(re, (m) => `<mark>${m}</mark>`);
}

// Human-friendly "X min ago" formatter for the auto-recovery banner. Inputs
// outside the expected range fall through to the closest unit.
export function formatRecoveryAge(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '';
  if (ms < 60_000) return 'just now';
  if (ms < 3_600_000) return `${Math.round(ms / 60_000)} min ago`;
  if (ms < 86_400_000) return `${Math.round(ms / 3_600_000)} h ago`;
  return `${Math.round(ms / 86_400_000)} d ago`;
}

// Returns true if any item in the outline branch (this item or any
// descendant) has a title containing `q` (case-insensitive). Used to keep
// ancestor entries visible when a descendant matches a filter.
export function outlineBranchMatches(items, q) {
  if (!Array.isArray(items) || !q) return true;
  const needle = String(q).toLowerCase();
  for (const it of items) {
    if (it && it.title && String(it.title).toLowerCase().includes(needle)) return true;
    if (it && Array.isArray(it.children) && it.children.length &&
        outlineBranchMatches(it.children, needle)) return true;
  }
  return false;
}

// Short, human-readable label for an annotation, used by the Marks list
// and elsewhere. Truncates long text annotation bodies to ~40 chars.
export function annotLabel(a) {
  if (!a || typeof a !== 'object') return '';
  switch (a.type) {
    case 'highlight': return 'Highlight';
    case 'text': {
      const t = a.text && a.text.trim();
      return t ? `\u201c${t.slice(0, 40)}${t.length > 40 ? '\u2026' : ''}\u201d` : 'Text';
    }
    case 'rect':      return 'Rectangle';
    case 'oval':      return 'Oval';
    case 'line':      return 'Line';
    case 'arrow':     return 'Arrow';
    case 'ink':       return 'Drawing';
    case 'redact':    return 'Redaction';
    case 'link':      return Number.isInteger(a.destPage) ? `Link \u2192 p.${a.destPage + 1}` : 'Link';
    case 'image':     return 'Image';
    default:          return a.type || 'Annotation';
  }
}

// Convert an annotation's `dash` value to the SVG `stroke-dasharray` string.
// Returns null for solid/no-dash so the attribute is omitted.
export function dashToStrokeDasharray(dash) {
  switch (dash) {
    case 'dashed': return '6 3';
    case 'dotted': return '2 4';
    case 'solid':  // fallthrough
    case null:
    case undefined: return null;
    default: return null;
  }
}

// Clamp a value into [lo, hi]. Returns lo for non-finite input.
export function clamp(value, lo, hi) {
  const n = Number(value);
  if (!Number.isFinite(n)) return lo;
  if (n < lo) return lo;
  if (n > hi) return hi;
  return n;
}
