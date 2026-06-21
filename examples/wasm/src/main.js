import './style.css';
import createModule from 'nanopdf-wasm';
import wasmUrl from 'nanopdf-wasm-bin';
import { loadStandardFonts, loadCDNCJKFonts } from './font-loader.js';

// External fonts live alongside the site under <base>/fonts. Derive the path
// from Vite's BASE_URL so it works both at the dev-server root ('/') and under
// a GitHub Pages project path (e.g. '/nanopdf/'). BASE_URL always ends in '/'.
const FONTS_BASE = `${import.meta.env.BASE_URL}fonts`;

// State
let Module = null;
let currentPage = 0;
let totalPages = 0;
let hasRendering = false;
let renderBackend = 1; // 0 = LightVG, 1 = ThorVG
let renderBackends = { lightvg: false, thorvg: false };
let fileName = '';
let fileSize = 0;
let currentPdfBytes = null;
let outlineData = null;
let signatureData = null;
let editHistoryData = null;
let searchResults = [];
let currentSearchIndex = -1;
let currentSearchTerm = '';
let currentSelection = null;
let isSelectingText = false;
let selectionStartPoint = null;
let selectionDragBox = null;
let isPageJumpMode = false;
let activeSidebarTab = 'outline';

// New state
let zoomLevel = 1.0;
let rotation = 0; // 0, 90, 180, 270
let fitMode = 'width'; // 'width' or 'page'
let thumbnailCache = {}; // pageIndex -> ImageData
let thumbnailRenderQueue = [];
let isThumbnailRendering = false;
let thumbnailObserver = null;

// Page selection state
let selectedPages = new Set();
let lastClickedPage = 0;

const ZOOM_STEPS = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0];
const BACKEND_LIGHTVG = 0;
const BACKEND_THORVG = 1;

// DOM refs
const loadingOverlay = document.getElementById('loading-overlay');
const loadingText = document.getElementById('loading-text');
const dropOverlay = document.getElementById('drop-overlay');
const pdfInput = document.getElementById('pdf-input');
const openPdfBtn = document.getElementById('open-pdf-btn');
const renderBtn = document.getElementById('render-btn');
const extractBtn = document.getElementById('extract-btn');
const backendToggleBtn = document.getElementById('backend-toggle-btn');
const prevBtn = document.getElementById('prev-btn');
const nextBtn = document.getElementById('next-btn');
const sidebarToggleBtn = document.getElementById('sidebar-toggle-btn');
const canvas = document.getElementById('pdf-canvas');
const textOverlay = document.getElementById('text-overlay');
const canvasWrapper = document.getElementById('canvas-wrapper');
const pageDisplay = document.getElementById('page-display');
const statusText = document.getElementById('status-text');
const statusRight = document.getElementById('status-right');
const statusBar = document.getElementById('status-bar');
const emptyState = document.getElementById('empty-state');
const sidebar = document.getElementById('sidebar');
const sidebarContent = document.getElementById('sidebar-content');
const textPanel = document.getElementById('text-panel');
const textContent = document.getElementById('text-content');
const searchInput = document.getElementById('search-input');
const searchPrevBtn = document.getElementById('search-prev-btn');
const searchNextBtn = document.getElementById('search-next-btn');
const searchInfo = document.getElementById('search-info');
const zoomInBtn = document.getElementById('zoom-in-btn');
const zoomOutBtn = document.getElementById('zoom-out-btn');
const zoomDisplay = document.getElementById('zoom-display');
const fitModeBtn = document.getElementById('fit-mode-btn');
const rotateBtn = document.getElementById('rotate-btn');
const canvasScroll = document.getElementById('canvas-scroll');
const exportBtn = document.getElementById('export-btn');
const protectExport = document.getElementById('protect-export');
const exportPassword = document.getElementById('export-password');
const exportOwnerPassword = document.getElementById('export-owner-password');

function setStatus(msg, isError = false) {
  statusText.textContent = msg;
  statusBar.className = 'status-bar' + (isError ? ' error' : '');
}

function showLoading(msg) {
  loadingText.textContent = msg;
  loadingOverlay.classList.remove('hidden');
}

function hideLoading() {
  loadingOverlay.classList.add('hidden');
}

function updatePageDisplay() {
  if (isPageJumpMode) return;
  pageDisplay.textContent = `Page ${currentPage + 1} / ${totalPages}`;
  prevBtn.disabled = currentPage <= 0;
  nextBtn.disabled = currentPage >= totalPages - 1;
}

function formatFileSize(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

function updateZoomDisplay() {
  zoomDisplay.textContent = Math.round(zoomLevel * 100) + '%';
}

function backendLabel(backendId) {
  return backendId === BACKEND_LIGHTVG ? 'LightVG' : 'ThorVG';
}

function syncRenderBackendFromModule() {
  if (!Module || !hasRendering) return;

  renderBackends = {
    lightvg: Module._nanopdf_render_backend_available
      ? Module._nanopdf_render_backend_available(BACKEND_LIGHTVG) === 1
      : false,
    thorvg: Module._nanopdf_render_backend_available
      ? Module._nanopdf_render_backend_available(BACKEND_THORVG) === 1
      : true,
  };

  if (Module._nanopdf_get_render_backend) {
    const current = Module._nanopdf_get_render_backend();
    if (current === BACKEND_LIGHTVG || current === BACKEND_THORVG) {
      renderBackend = current;
    }
  } else if (!renderBackends.thorvg && renderBackends.lightvg) {
    renderBackend = BACKEND_LIGHTVG;
  }

  updateBackendToggle();
}

function updateBackendToggle() {
  if (!backendToggleBtn) return;
  const availableCount = (renderBackends.lightvg ? 1 : 0) + (renderBackends.thorvg ? 1 : 0);
  backendToggleBtn.textContent = backendLabel(renderBackend);
  backendToggleBtn.disabled = !hasRendering || availableCount < 2;
  backendToggleBtn.hidden = !hasRendering || availableCount === 0;
  backendToggleBtn.title = availableCount > 1
    ? `Rendering backend: ${backendLabel(renderBackend)}`
    : `Rendering backend: ${backendLabel(renderBackend)} (only backend available)`;
}

function switchRenderBackend() {
  if (!Module || !hasRendering || !Module._nanopdf_set_render_backend) return;
  const nextBackend = renderBackend === BACKEND_THORVG ? BACKEND_LIGHTVG : BACKEND_THORVG;
  const nextAvailable = nextBackend === BACKEND_LIGHTVG
    ? renderBackends.lightvg
    : renderBackends.thorvg;
  if (!nextAvailable) return;

  const result = Module._nanopdf_set_render_backend(nextBackend);
  if (result !== 1) {
    const error = Module._nanopdf_get_last_error
      ? Module.UTF8ToString(Module._nanopdf_get_last_error())
      : 'backend unavailable';
    setStatus('Backend switch failed: ' + error, true);
    return;
  }

  renderBackend = nextBackend;
  thumbnailCache = {};
  thumbnailRenderQueue = [];
  isThumbnailRendering = false;
  updateBackendToggle();
  if (activeSidebarTab === 'info') renderInfoTab();
  if (activeSidebarTab === 'thumbs') updateSidebar();
  if (totalPages > 0) renderCurrentPage();
}

// ---- Sidebar ----

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

function renderOutlineTab() {
  if (!outlineData || !outlineData.outline || outlineData.outline.length === 0) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No bookmarks in this document.</div>';
    return;
  }

  let html = '';
  function renderItems(items, depth) {
    for (const item of items) {
      const indent = depth * 16;
      const pageLabel = item.page !== undefined ? ` (p.${item.page + 1})` : '';
      const clickAttr = item.page !== undefined
        ? `onclick="window._jumpToPage(${item.page})"` : '';
      const style = `padding-left:${indent + 8}px`;
      const boldStyle = item.bold ? 'font-weight:600;' : '';
      const italicStyle = item.italic ? 'font-style:italic;' : '';
      html += `<div class="outline-item" ${clickAttr} style="${style};${boldStyle}${italicStyle}" title="${escapeHtml(item.title)}${pageLabel}">${escapeHtml(item.title)}<span style="color:#999;font-size:11px;">${pageLabel}</span></div>`;
      if (item.children && item.children.length > 0) {
        renderItems(item.children, depth + 1);
      }
    }
  }
  renderItems(outlineData.outline, 0);
  sidebarContent.innerHTML = html;
}

function renderInfoTab() {
  let html = '';
  const info = [
    { label: 'File Name', value: fileName || 'N/A' },
    { label: 'File Size', value: fileSize ? formatFileSize(fileSize) : 'N/A' },
    { label: 'Pages', value: totalPages || 'N/A' },
    { label: 'Rendering', value: hasRendering ? 'Available' : 'Not available' },
    { label: 'Backend', value: hasRendering ? backendLabel(renderBackend) : 'N/A' },
  ];

  if (totalPages > 0 && Module) {
    const pw = Module._nanopdf_get_page_width(currentPage);
    const ph = Module._nanopdf_get_page_height(currentPage);
    info.push({ label: 'Current Page Size', value: `${pw.toFixed(1)} x ${ph.toFixed(1)} pts` });
    info.push({ label: 'Inches', value: `${(pw/72).toFixed(2)}" x ${(ph/72).toFixed(2)}"` });

    const hasOutline = Module._nanopdf_has_outline() === 1;
    info.push({ label: 'Bookmarks', value: hasOutline ? `${Module._nanopdf_get_outline_count()} items` : 'None' });

    const hasForms = Module._nanopdf_has_form_fields() === 1;
    info.push({ label: 'Form Fields', value: hasForms ? `${Module._nanopdf_get_form_field_count()} fields` : 'None' });

    const signatureCount = signatureData && Array.isArray(signatureData.signatures)
      ? signatureData.signatures.length
      : 0;
    info.push({ label: 'Signatures', value: signatureCount > 0 ? `${signatureCount} field${signatureCount === 1 ? '' : 's'}` : 'None' });

    const revisionCount = editHistoryData && Array.isArray(editHistoryData.revisions)
      ? editHistoryData.revisions.length
      : 0;
    info.push({ label: 'Revisions', value: revisionCount > 0 ? `${revisionCount}` : 'None' });
  }

  for (const item of info) {
    html += `<div class="doc-info-item"><div class="label">${item.label}</div><div class="value">${item.value}</div></div>`;
  }
  sidebarContent.innerHTML = html;
}

function getMdpPermissionLabel(value) {
  switch (value) {
    case 1: return 'No changes allowed';
    case 2: return 'Form fill and sign only';
    case 3: return 'Form fill, sign, annotate';
    default: return value ? `Unknown (${value})` : 'Not specified';
  }
}

function formatPdfDate(value) {
  if (!value) return '';
  const match = /^D:(\d{4})(\d{2})?(\d{2})?(\d{2})?(\d{2})?(\d{2})?/.exec(value);
  if (!match) return value;
  const [, year, month = '01', day = '01', hour = '00', minute = '00', second = '00'] = match;
  return `${year}-${month}-${day} ${hour}:${minute}:${second}`;
}

function signatureStatusText(sig) {
  if (!sig.signed && !sig.signaturePresent) return 'Unsigned field';
  if (!sig.byteRangeValid) return 'Signed, ByteRange missing or invalid';
  return sig.isCertification ? 'Certification signature' : 'Approval signature';
}

function renderSignatureDetail(label, value) {
  if (value === undefined || value === null || value === '') return '';
  return `<div class="signature-detail"><span>${label}</span><strong>${escapeHtml(String(value))}</strong></div>`;
}

function renderSignaturesTab() {
  if (!Module || totalPages <= 0) {
    sidebarContent.innerHTML = '<div class="empty-sidebar-message">No document loaded.</div>';
    return;
  }

  const signatures = signatureData && Array.isArray(signatureData.signatures)
    ? signatureData.signatures
    : [];

  if (signatures.length === 0) {
    sidebarContent.innerHTML = '<div class="empty-sidebar-message">No signature fields in this document.</div>';
    return;
  }

  let html = '<div class="signature-list">';
  signatures.forEach((sig, index) => {
    const name = sig.name || `Signature ${index + 1}`;
    const status = signatureStatusText(sig);
    const statusClass = sig.isCertification ? ' certification' : (sig.signed || sig.signaturePresent ? ' signed' : '');
    const rect = Array.isArray(sig.rect) && sig.rect.length === 4
      ? sig.rect.map(n => Number(n).toFixed(1)).join(', ')
      : '';
    const byteRange = Array.isArray(sig.byteRange)
      ? sig.byteRange.join(', ')
      : '';
    const signedDate = formatPdfDate(sig.date);

    html += `<section class="signature-card" data-signature-index="${index}">`;
    html += '<div class="signature-card-header">';
    html += `<div><div class="signature-name">${escapeHtml(name)}</div><div class="signature-status${statusClass}">${escapeHtml(status)}</div></div>`;
    html += `<button type="button" class="validate-signature-btn" data-signature-index="${index}" ${sig.signaturePresent ? '' : 'disabled'}>Validate</button>`;
    html += '</div>';
    html += '<div class="signature-details">';
    html += renderSignatureDetail('Type', sig.isCertification ? 'Certification (DocMDP)' : 'Approval');
    html += renderSignatureDetail('MDP', sig.isCertification ? getMdpPermissionLabel(sig.mdpPermissions) : '');
    html += renderSignatureDetail('Transform', sig.transformMethod);
    html += renderSignatureDetail('Filter', sig.filter);
    html += renderSignatureDetail('SubFilter', sig.subFilter);
    html += renderSignatureDetail('Reason', sig.reason);
    html += renderSignatureDetail('Location', sig.location);
    html += renderSignatureDetail('Contact', sig.contact);
    html += renderSignatureDetail('Date', signedDate);
    html += renderSignatureDetail('Timestamp', sig.hasTimestamp ? (sig.timestampType || 'embedded') : 'None');
    html += renderSignatureDetail('Timestamp Date', formatPdfDate(sig.timestampDate));
    html += renderSignatureDetail('TSA', sig.timestampAuthority);
    html += renderSignatureDetail('Timestamp Hash', sig.timestampHashAlgorithm);
    html += renderSignatureDetail('Timestamp Token', sig.timestampTokenLength ? `${sig.timestampTokenLength} bytes` : '');
    html += renderSignatureDetail('ByteRange', byteRange);
    html += renderSignatureDetail('Digest', sig.digestAlgorithm);
    html += renderSignatureDetail('Contents', sig.contentsLength ? `${sig.contentsLength} bytes` : '');
    html += renderSignatureDetail('Rect', rect);
    if (Array.isArray(sig.lockedFields) && sig.lockedFields.length > 0) {
      html += renderSignatureDetail('Locked Fields', sig.lockedFields.join(', '));
    }
    html += '</div>';
    html += `<div class="signature-validation" id="signature-validation-${index}"></div>`;
    html += '</section>';
  });
  html += '</div>';

  sidebarContent.innerHTML = html;
  sidebarContent.querySelectorAll('.validate-signature-btn').forEach(btn => {
    btn.addEventListener('click', () => validateSignature(parseInt(btn.dataset.signatureIndex, 10)));
  });
}

function renderHistoryTab() {
  if (!Module || totalPages <= 0) {
    sidebarContent.innerHTML = '<div class="empty-sidebar-message">No document loaded.</div>';
    return;
  }

  const revisions = editHistoryData && Array.isArray(editHistoryData.revisions)
    ? editHistoryData.revisions
    : [];

  if (revisions.length === 0) {
    sidebarContent.innerHTML = '<div class="empty-sidebar-message">No edit history detected.</div>';
    return;
  }

  let html = '<div class="history-summary">';
  html += `<div>${revisions.length} revision${revisions.length === 1 ? '' : 's'}</div>`;
  html += `<div>${formatFileSize(editHistoryData.fileSize || 0)}</div>`;
  html += '</div>';
  html += '<div class="history-list">';

  revisions.forEach((rev, idx) => {
    const associated = rev.associatedSignature || '';
    const docMDPClass = rev.hasDocMDP && rev.docMDPAllowed === false ? ' violation' : '';
    const className = (associated ? 'history-card signed' : 'history-card') + docMDPClass;
    const objectSummary = [
      rev.addedObjects && rev.addedObjects.length > 0 ? `+${rev.addedObjects.join(',')}` : '',
      rev.modifiedObjects && rev.modifiedObjects.length > 0 ? `~${rev.modifiedObjects.join(',')}` : '',
      rev.deletedObjects && rev.deletedObjects.length > 0 ? `-${rev.deletedObjects.join(',')}` : ''
    ].filter(Boolean).join(' ');
    html += `<section class="${className}">`;
    html += '<div class="history-card-header">';
    html += `<div class="history-title">Revision ${rev.revision}</div>`;
    html += `<div class="history-size">${formatFileSize(rev.sizeBytes)}</div>`;
    html += '</div>';
    html += '<div class="history-details">';
    html += renderSignatureDetail('Bytes', `${rev.startOffset} - ${rev.endOffset}`);
    html += renderSignatureDetail('startxref', rev.xrefOffset || 'Unknown');
    html += renderSignatureDetail('Prev', rev.prevXrefOffset || '');
    html += renderSignatureDetail('SHA-256', rev.sha256 ? rev.sha256.slice(0, 24) : '');
    html += renderSignatureDetail('Objects', objectSummary);
    html += renderSignatureDetail('Signed By', associated);
    html += renderSignatureDetail('Sign Time', formatPdfDate(rev.signingTime));
    if (associated) {
      html += renderSignatureDetail('After Signing', rev.modifiedAfterSignature ? 'Additional bytes appended' : 'No appended bytes');
    }
    if (rev.hasDocMDP) {
      html += renderSignatureDetail('DocMDP', rev.docMDPAllowed ? 'Allowed' : 'Disallowed');
      html += renderSignatureDetail('MDP', getMdpPermissionLabel(rev.mdpPermissions));
      if (Array.isArray(rev.docMDPViolations) && rev.docMDPViolations.length > 0) {
        html += renderSignatureDetail('Violation', rev.docMDPViolations.join(' | '));
      } else {
        html += renderSignatureDetail('Status', rev.docMDPStatus);
      }
    }
    html += '</div>';
    // Visual diff vs the previous revision. The first revision is the base
    // document (nothing to diff against), so only offer it from revision 2 on.
    if (idx > 0 && hasRendering) {
      html += `<button class="history-diff-btn" data-rev-index="${idx}" title="Visually highlight what this revision changed">Visualize changes</button>`;
    }
    html += '</section>';
  });

  html += '</div>';
  sidebarContent.innerHTML = html;

  sidebarContent.querySelectorAll('.history-diff-btn').forEach(btn => {
    btn.addEventListener('click', () => openRevisionDiff(parseInt(btn.dataset.revIndex, 10)));
  });
}

// ---- Revision visual diff ----
//
// Highlights what an incremental-update revision changed by rendering the page
// as it existed *before* the revision and *after* it, then compositing the two:
// content added by the revision is green, removed content is red, and unchanged
// content fades to light gray for context. Before/After/Swipe modes show the
// raw renders for comparison.

const diffState = {
  open: false,
  revIndex: 0,
  page: 0,
  pageCount: 0,
  mode: 'diff',          // 'diff' | 'before' | 'after' | 'swipe'
  swipe: 0.5,
  before: null,          // { data, w, h } or null (page absent in prev revision)
  after: null,
  byteAfter: 0,
  byteBefore: 0,
};

// Render one revision snapshot of a page into a detached pixel buffer.
function renderRevisionPixels(byteLen, pageIdx, width, height, dpi) {
  if (!Module._nanopdf_render_revision_page) return null;
  let ok = 0;
  try {
    ok = Module._nanopdf_render_revision_page(byteLen, pageIdx, width, height, dpi);
  } catch (e) {
    console.warn('revision render crashed', e);
    return null;
  }
  if (ok !== 1) return null;
  const ptr = Module._nanopdf_get_render_buffer();
  const size = Module._nanopdf_get_render_buffer_size();
  const w = Module._nanopdf_get_render_width();
  const h = Module._nanopdf_get_render_height();
  if (!ptr || size <= 0) return null;
  // Copy out — the buffer is reused by the next render call.
  const data = new Uint8ClampedArray(size);
  data.set(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, size));
  return { data, w, h };
}

function lum(d, i) {
  // Rec.601 luma of an RGBA pixel at byte offset i.
  return 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
}

// Build an ImageData for the current mode from cached before/after buffers.
function composeDiffImage(ctx) {
  const after = diffState.after;
  const before = diffState.before;
  if (!after) return null;
  const w = after.w, h = after.h;
  const out = ctx.createImageData(w, h);
  const o = out.data, A = before && before.w === w && before.h === h ? before.data : null;
  const B = after.data;

  if (diffState.mode === 'after' || diffState.mode === 'before') {
    const src = diffState.mode === 'before' ? (A || null) : B;
    if (!src) { o.fill(255); for (let i = 3; i < o.length; i += 4) o[i] = 255; }
    else o.set(src);
    return out;
  }

  if (diffState.mode === 'swipe') {
    const splitX = Math.round(w * diffState.swipe);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const src = (x < splitX && A) ? A : B;
        o[i] = src[i]; o[i + 1] = src[i + 1]; o[i + 2] = src[i + 2]; o[i + 3] = 255;
      }
    }
    // divider line
    for (let y = 0; y < h; y++) {
      const i = (y * w + splitX) * 4;
      if (splitX < w) { o[i] = 0; o[i + 1] = 123; o[i + 2] = 255; o[i + 3] = 255; }
    }
    return out;
  }

  // 'diff' mode — signed luminance difference.
  const T = 28;          // ignore sub-threshold AA noise
  for (let i = 0; i < B.length; i += 4) {
    const bL = lum(B, i);
    const aL = A ? lum(A, i) : 255;  // no prior page => everything is "added"
    const d = aL - bL;               // >0: darker now (added ink); <0: removed
    let rr, gg, bb;
    if (d > T) {
      const k = Math.min(1, d / 170);
      rr = Math.round(255 - 239 * k); gg = Math.round(255 - 95 * k); bb = Math.round(255 - 183 * k); // -> #10A248
    } else if (d < -T) {
      const k = Math.min(1, -d / 170);
      rr = Math.round(255 - 47 * k); gg = Math.round(255 - 215 * k); bb = Math.round(255 - 215 * k);  // -> #C82828
    } else {
      const ink = (255 - bL) / 255;             // unchanged: fade to light gray
      const g = Math.round(255 - ink * 105);
      rr = gg = bb = g;
    }
    o[i] = rr; o[i + 1] = gg; o[i + 2] = bb; o[i + 3] = 255;
  }
  return out;
}

function ensureDiffOverlay() {
  let el = document.getElementById('rev-diff-overlay');
  if (el) return el;
  el = document.createElement('div');
  el.id = 'rev-diff-overlay';
  el.className = 'rev-diff-overlay hidden';
  el.innerHTML = `
    <div class="rev-diff-panel">
      <div class="rev-diff-header">
        <div class="rev-diff-title" id="rev-diff-title">Revision changes</div>
        <div class="rev-diff-modes">
          <button data-mode="diff" class="active">Diff</button>
          <button data-mode="before">Before</button>
          <button data-mode="after">After</button>
          <button data-mode="swipe">Swipe</button>
        </div>
        <button class="rev-diff-close" id="rev-diff-close" title="Close">&times;</button>
      </div>
      <div class="rev-diff-body">
        <canvas id="rev-diff-canvas"></canvas>
        <div class="rev-diff-empty hidden" id="rev-diff-empty"></div>
      </div>
      <div class="rev-diff-footer">
        <div class="rev-diff-pagenav">
          <button id="rev-diff-prev" title="Previous page">&larr;</button>
          <span id="rev-diff-pageinfo">Page 1 / 1</span>
          <button id="rev-diff-next" title="Next page">&rarr;</button>
        </div>
        <input type="range" id="rev-diff-swipe" min="0" max="100" value="50" class="hidden" />
        <div class="rev-diff-legend">
          <span><i class="swatch added"></i>Added</span>
          <span><i class="swatch removed"></i>Removed</span>
          <span><i class="swatch same"></i>Unchanged</span>
        </div>
      </div>
    </div>`;
  document.body.appendChild(el);

  el.querySelector('#rev-diff-close').addEventListener('click', closeRevisionDiff);
  el.addEventListener('click', (e) => { if (e.target === el) closeRevisionDiff(); });
  el.querySelectorAll('.rev-diff-modes button').forEach(b => {
    b.addEventListener('click', () => {
      diffState.mode = b.dataset.mode;
      el.querySelectorAll('.rev-diff-modes button').forEach(x => x.classList.toggle('active', x === b));
      el.querySelector('#rev-diff-swipe').classList.toggle('hidden', diffState.mode !== 'swipe');
      el.querySelector('.rev-diff-legend').classList.toggle('hidden', diffState.mode !== 'diff');
      paintDiffCanvas();
    });
  });
  el.querySelector('#rev-diff-prev').addEventListener('click', () => stepDiffPage(-1));
  el.querySelector('#rev-diff-next').addEventListener('click', () => stepDiffPage(1));
  el.querySelector('#rev-diff-swipe').addEventListener('input', (e) => {
    diffState.swipe = e.target.value / 100;
    paintDiffCanvas();
  });
  return el;
}

function diffRenderSize(pageIdx) {
  const pw = Module._nanopdf_get_page_width(pageIdx) || 612;
  const ph = Module._nanopdf_get_page_height(pageIdx) || 792;
  const maxW = 1000;
  const scale = Math.min(maxW / pw, 2.0);
  const w = Math.max(1, Math.round(pw * scale));
  const h = Math.max(1, Math.round(ph * scale));
  return { w, h, dpi: 72 * (w / pw) };
}

// Load before/after buffers for the current diff page (cached on diffState).
function loadDiffPage() {
  const { w, h, dpi } = diffRenderSize(diffState.page);
  diffState.after = renderRevisionPixels(diffState.byteAfter, diffState.page, w, h, dpi);
  diffState.before = renderRevisionPixels(diffState.byteBefore, diffState.page, w, h, dpi);
}

// True if the page has any visible change between before/after.
function diffPageChanged() {
  const A = diffState.before, B = diffState.after;
  if (!B) return false;
  if (!A || A.w !== B.w || A.h !== B.h) return true;  // page added / size change
  const a = A.data, b = B.data;
  let changed = 0;
  for (let i = 0; i < b.length; i += 4) {
    if (Math.abs(lum(a, i) - lum(b, i)) > 28) {
      if (++changed > 40) return true;
    }
  }
  return false;
}

function paintDiffCanvas() {
  const overlay = document.getElementById('rev-diff-overlay');
  const canvas = document.getElementById('rev-diff-canvas');
  const empty = document.getElementById('rev-diff-empty');
  const ctx = canvas.getContext('2d');
  const img = composeDiffImage(ctx);
  if (!img) {
    canvas.classList.add('hidden');
    empty.classList.remove('hidden');
    empty.textContent = 'This page is not present in the selected revision.';
    return;
  }
  canvas.width = img.width;
  canvas.height = img.height;
  ctx.putImageData(img, 0, 0);
  const noChange = diffState.mode === 'diff' && !diffPageChanged();
  empty.classList.toggle('hidden', !noChange);
  if (noChange) empty.textContent = 'No visual changes on this page.';
  canvas.classList.toggle('dimmed', noChange);
  canvas.classList.remove('hidden');
  document.getElementById('rev-diff-pageinfo').textContent =
    `Page ${diffState.page + 1} / ${diffState.pageCount}`;
  overlay.querySelector('#rev-diff-prev').disabled = diffState.page <= 0;
  overlay.querySelector('#rev-diff-next').disabled = diffState.page >= diffState.pageCount - 1;
}

function stepDiffPage(delta) {
  const next = diffState.page + delta;
  if (next < 0 || next >= diffState.pageCount) return;
  diffState.page = next;
  loadDiffPage();
  paintDiffCanvas();
}

function openRevisionDiff(revIndex) {
  const revisions = (editHistoryData && editHistoryData.revisions) || [];
  if (revIndex <= 0 || revIndex >= revisions.length) return;
  const rev = revisions[revIndex];
  const prev = revisions[revIndex - 1];
  diffState.revIndex = revIndex;
  diffState.byteAfter = rev.endOffset;
  diffState.byteBefore = prev.endOffset;
  diffState.mode = 'diff';
  diffState.pageCount = Module._nanopdf_get_revision_page_count
    ? (Module._nanopdf_get_revision_page_count(rev.endOffset) || totalPages)
    : totalPages;

  const overlay = ensureDiffOverlay();
  overlay.querySelectorAll('.rev-diff-modes button').forEach(x => x.classList.toggle('active', x.dataset.mode === 'diff'));
  overlay.querySelector('#rev-diff-swipe').classList.add('hidden');
  overlay.querySelector('.rev-diff-legend').classList.remove('hidden');
  const signer = rev.signerName || rev.associatedSignature;
  overlay.querySelector('#rev-diff-title').textContent =
    `Revision ${rev.revision} changes` + (signer ? ` — signed by ${signer}` : '');
  overlay.classList.remove('hidden');
  diffState.open = true;

  // Jump to the first page that actually changed (cap the scan for big docs).
  diffState.page = Math.min(currentPage, diffState.pageCount - 1);
  const scanLimit = diffState.pageCount <= 30 ? diffState.pageCount : 0;
  let landed = false;
  for (let p = 0; p < scanLimit; p++) {
    diffState.page = p;
    loadDiffPage();
    if (diffPageChanged()) { landed = true; break; }
  }
  if (!landed) { diffState.page = Math.min(currentPage, diffState.pageCount - 1); loadDiffPage(); }
  paintDiffCanvas();
}

function closeRevisionDiff() {
  const overlay = document.getElementById('rev-diff-overlay');
  if (overlay) overlay.classList.add('hidden');
  diffState.open = false;
  diffState.before = diffState.after = null;
}

// ---- Thumbnails ----

function renderThumbnailsTab() {
  if (!Module || totalPages <= 0) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No document loaded.</div>';
    return;
  }

  if (!hasRendering) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">Rendering not available.</div>';
    return;
  }

  // Selection bar
  let html = '<div class="selection-bar" id="selection-bar">';
  html += `<span id="selection-count">${selectedPages.size > 0 ? selectedPages.size + ' selected' : 'Ctrl/Shift+click to select'}</span>`;
  html += '<button id="select-all-btn">All</button>';
  html += '<button id="clear-selection-btn">Clear</button>';
  html += '</div>';

  html += '<div class="thumbnail-list">';
  for (let i = 0; i < totalPages; i++) {
    const classes = [];
    if (i === currentPage) classes.push('active');
    if (selectedPages.has(i)) classes.push('selected');
    const cls = classes.length > 0 ? ' ' + classes.join(' ') : '';
    html += `<div class="thumbnail-item${cls}" data-page="${i}">`;
    html += '<div class="select-check">&#10003;</div>';
    html += `<canvas class="thumb-canvas" data-page="${i}" width="1" height="1"></canvas>`;
    html += `<div class="thumb-label">${i + 1}</div>`;
    html += '</div>';
  }
  html += '</div>';
  sidebarContent.innerHTML = html;

  // Selection bar handlers
  document.getElementById('select-all-btn').addEventListener('click', (e) => {
    e.stopPropagation();
    for (let i = 0; i < totalPages; i++) selectedPages.add(i);
    updateSelectionUI();
  });
  document.getElementById('clear-selection-btn').addEventListener('click', (e) => {
    e.stopPropagation();
    selectedPages.clear();
    updateSelectionUI();
  });

  // Click handlers with multi-select support
  sidebarContent.querySelectorAll('.thumbnail-item').forEach(item => {
    item.addEventListener('click', (e) => {
      const page = parseInt(item.dataset.page, 10);
      handleThumbnailClick(page, e);
    });
  });

  // Lazy load with IntersectionObserver
  setupThumbnailObserver();
}

function handleThumbnailClick(page, event) {
  if (event.ctrlKey || event.metaKey) {
    // Ctrl+Click: toggle page in selection
    if (selectedPages.has(page)) {
      selectedPages.delete(page);
    } else {
      selectedPages.add(page);
    }
    lastClickedPage = page;
    updateSelectionUI();
  } else if (event.shiftKey) {
    // Shift+Click: range select from lastClickedPage to page
    const start = Math.min(lastClickedPage, page);
    const end = Math.max(lastClickedPage, page);
    for (let i = start; i <= end; i++) {
      selectedPages.add(i);
    }
    updateSelectionUI();
  } else {
    // Plain click: navigate to page, clear selection
    selectedPages.clear();
    lastClickedPage = page;
    updateSelectionUI();
    goToPage(page);
  }
}

function updateSelectionUI() {
  // Update thumbnail classes
  if (activeSidebarTab === 'thumbs') {
    sidebarContent.querySelectorAll('.thumbnail-item').forEach(item => {
      const page = parseInt(item.dataset.page, 10);
      item.classList.toggle('selected', selectedPages.has(page));
      item.classList.toggle('active', page === currentPage);
    });
    // Update count
    const countEl = document.getElementById('selection-count');
    if (countEl) {
      countEl.textContent = selectedPages.size > 0
        ? selectedPages.size + ' selected'
        : 'Ctrl/Shift+click to select';
    }
  }
  // Update export button
  updateExportButton();
}

function updateExportButton() {
  const hasSelection = selectedPages.size > 0 && hasRendering;
  exportBtn.disabled = !hasSelection;
  protectExport.disabled = !hasSelection;
  const passwordEnabled = hasSelection && protectExport.checked;
  exportPassword.disabled = !passwordEnabled;
  exportOwnerPassword.disabled = !passwordEnabled;
  exportBtn.classList.toggle('has-selection', hasSelection);
  exportBtn.textContent = hasSelection
    ? `Export (${selectedPages.size})`
    : 'Export';
}

function setupThumbnailObserver() {
  if (thumbnailObserver) {
    thumbnailObserver.disconnect();
  }

  thumbnailObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting) {
        const canvasEl = entry.target;
        const pageIdx = parseInt(canvasEl.dataset.page, 10);
        queueThumbnailRender(pageIdx, canvasEl);
        thumbnailObserver.unobserve(canvasEl);
      }
    }
  }, {
    root: sidebarContent,
    rootMargin: '100px',
  });

  sidebarContent.querySelectorAll('.thumb-canvas').forEach(c => {
    const pageIdx = parseInt(c.dataset.page, 10);
    // Set placeholder size
    const pw = Module._nanopdf_get_page_width(pageIdx);
    const ph = Module._nanopdf_get_page_height(pageIdx);
    const thumbWidth = 120;
    const thumbScale = thumbWidth / pw;
    const thumbHeight = Math.floor(ph * thumbScale);
    c.width = thumbWidth;
    c.height = thumbHeight;
    c.style.width = thumbWidth + 'px';
    c.style.height = thumbHeight + 'px';

    // If cached, draw immediately
    if (thumbnailCache[pageIdx]) {
      const ctx = c.getContext('2d');
      ctx.putImageData(thumbnailCache[pageIdx], 0, 0);
    } else {
      thumbnailObserver.observe(c);
    }
  });
}

function queueThumbnailRender(pageIdx, canvasEl) {
  // Don't queue if already cached and drawn
  if (thumbnailCache[pageIdx]) {
    const ctx = canvasEl.getContext('2d');
    ctx.putImageData(thumbnailCache[pageIdx], 0, 0);
    return;
  }
  thumbnailRenderQueue.push({ pageIdx, canvasEl });
  processThumbnailQueue();
}

function processThumbnailQueue() {
  if (isThumbnailRendering || thumbnailRenderQueue.length === 0) return;
  isThumbnailRendering = true;

  const { pageIdx, canvasEl } = thumbnailRenderQueue.shift();

  try {
    // Render at small thumbnail size to minimize memory usage
    const pw = Module._nanopdf_get_page_width(pageIdx);
    const ph = Module._nanopdf_get_page_height(pageIdx);
    const thumbWidth = 120;
    const thumbScale = thumbWidth / pw;
    const width = thumbWidth;
    const height = Math.floor(ph * thumbScale);
    const dpi = 72 * thumbScale;

    const result = Module._nanopdf_render_page(pageIdx, width, height, dpi);

    if (result === 1) {
      const bufferPtr = Module._nanopdf_get_render_buffer();
      const bufferSize = Module._nanopdf_get_render_buffer_size();
      const renderWidth = Module._nanopdf_get_render_width();
      const renderHeight = Module._nanopdf_get_render_height();

      // Draw at display size by scaling
      canvasEl.width = renderWidth;
      canvasEl.height = renderHeight;
      const ctx = canvasEl.getContext('2d');
      const imageData = ctx.createImageData(renderWidth, renderHeight);
      const srcData = new Uint8ClampedArray(Module.HEAPU8.buffer, bufferPtr, bufferSize);
      imageData.data.set(srcData);
      ctx.putImageData(imageData, 0, 0);
      thumbnailCache[pageIdx] = imageData;
    }
  } catch (e) {
    console.warn('Thumbnail render failed for page', pageIdx, e);
  }

  isThumbnailRendering = false;

  if (thumbnailRenderQueue.length > 0) {
    // Use setTimeout to give WASM memory time to settle between renders
    setTimeout(processThumbnailQueue, 50);
  } else {
    // All visible thumbnails done, re-render current page to restore main canvas buffer
    setTimeout(renderCurrentPage, 100);
  }
}

function updateThumbnailHighlight() {
  if (activeSidebarTab !== 'thumbs') return;
  const items = sidebarContent.querySelectorAll('.thumbnail-item');
  items.forEach(item => {
    const page = parseInt(item.dataset.page, 10);
    item.classList.toggle('active', page === currentPage);
    item.classList.toggle('selected', selectedPages.has(page));
  });

  // Auto-scroll current thumbnail into view
  const activeItem = sidebarContent.querySelector('.thumbnail-item.active');
  if (activeItem) {
    activeItem.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }
}

function updateSidebar() {
  if (activeSidebarTab === 'outline') {
    renderOutlineTab();
  } else if (activeSidebarTab === 'thumbs') {
    renderThumbnailsTab();
  } else if (activeSidebarTab === 'signatures') {
    renderSignaturesTab();
  } else if (activeSidebarTab === 'history') {
    renderHistoryTab();
  } else {
    renderInfoTab();
  }
}

// ---- Page Navigation ----

function goToPage(pageIndex) {
  if (pageIndex < 0 || pageIndex >= totalPages) return;
  currentPage = pageIndex;
  updatePageDisplay();
  renderCurrentPage();
  clearSearch();
  updateThumbnailHighlight();
}

window._jumpToPage = function(pageIndex) {
  goToPage(pageIndex);
};

function enterPageJump() {
  if (totalPages <= 0 || isPageJumpMode) return;
  isPageJumpMode = true;
  const input = document.createElement('input');
  input.type = 'number';
  input.className = 'page-jump-input';
  input.min = 1;
  input.max = totalPages;
  input.value = currentPage + 1;
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      const page = parseInt(input.value, 10) - 1;
      exitPageJump();
      if (page >= 0 && page < totalPages) {
        goToPage(page);
      }
    } else if (e.key === 'Escape') {
      exitPageJump();
    }
    e.stopPropagation();
  });
  input.addEventListener('blur', () => {
    exitPageJump();
  });
  pageDisplay.textContent = '';
  pageDisplay.appendChild(input);
  input.select();
}

function exitPageJump() {
  isPageJumpMode = false;
  updatePageDisplay();
}

// ---- Zoom ----

function zoomIn() {
  const nextStep = ZOOM_STEPS.find(s => s > zoomLevel + 0.001);
  if (nextStep) {
    zoomLevel = nextStep;
  } else if (zoomLevel < 4.0) {
    zoomLevel = Math.min(4.0, zoomLevel + 0.25);
  }
  updateZoomDisplay();
  renderCurrentPage();
}

function zoomOut() {
  const prevSteps = ZOOM_STEPS.filter(s => s < zoomLevel - 0.001);
  if (prevSteps.length > 0) {
    zoomLevel = prevSteps[prevSteps.length - 1];
  }
  updateZoomDisplay();
  renderCurrentPage();
}

function resetZoom() {
  zoomLevel = 1.0;
  updateZoomDisplay();
  renderCurrentPage();
}

// ---- Rotation ----

function rotateClockwise() {
  rotation = (rotation + 90) % 360;
  applyRotation();
}

function applyRotation() {
  canvas.style.transform = rotation === 0 ? '' : `rotate(${rotation}deg)`;
  textOverlay.style.transform = canvas.style.transform;

  // Adjust wrapper dimensions for 90/270 so scrolling works correctly
  if (rotation === 90 || rotation === 270) {
    canvasWrapper.style.width = canvas.height + 'px';
    canvasWrapper.style.height = canvas.width + 'px';
  } else {
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';
  }
  renderTextOverlay();
}

// ---- Fit Mode ----

function toggleFitMode() {
  fitMode = fitMode === 'width' ? 'page' : 'width';
  fitModeBtn.textContent = fitMode === 'width' ? 'Fit Width' : 'Fit Page';
  renderCurrentPage();
}

// ---- Rendering ----

function computeBaseScale(pageWidth, pageHeight) {
  const sidebarWidth = sidebar.classList.contains('hidden') ? 0 : 260;
  const availWidth = window.innerWidth - sidebarWidth - 80; // 80 for padding/scrollbar
  const availHeight = window.innerHeight - 120; // toolbar + statusbar + padding

  if (fitMode === 'width') {
    return Math.min(availWidth, 800) / pageWidth;
  } else {
    // Fit page: entire page visible
    const scaleW = availWidth / pageWidth;
    const scaleH = availHeight / pageHeight;
    return Math.min(scaleW, scaleH);
  }
}

function getPageScale() {
  if (!Module || totalPages <= 0 || canvas.width === 0) return 1;
  const pageWidth = Module._nanopdf_get_page_width(currentPage);
  return canvas.width / pageWidth;
}

function pdfRectToScreen(rect) {
  const pageHeight = Module._nanopdf_get_page_height(currentPage);
  const scale = getPageScale();
  const x = rect.x * scale;
  const y = (pageHeight - rect.y - rect.height) * scale;
  return {
    x,
    y,
    width: rect.width * scale,
    height: rect.height * scale
  };
}

function overlayPointToPdf(clientX, clientY) {
  const rect = textOverlay.getBoundingClientRect();
  const scale = getPageScale();
  const pageHeight = Module._nanopdf_get_page_height(currentPage);
  const x = Math.max(0, Math.min(canvas.width, clientX - rect.left));
  const y = Math.max(0, Math.min(canvas.height, clientY - rect.top));
  return {
    screenX: x,
    screenY: y,
    pdfX: x / scale,
    pdfY: pageHeight - (y / scale)
  };
}

function appendOverlayRect(rect, className) {
  if (!rect || rect.width <= 0 || rect.height <= 0) return;
  const screen = pdfRectToScreen(rect);
  const el = document.createElement('div');
  el.className = className;
  el.style.left = `${screen.x}px`;
  el.style.top = `${screen.y}px`;
  el.style.width = `${Math.max(1, screen.width)}px`;
  el.style.height = `${Math.max(1, screen.height)}px`;
  textOverlay.appendChild(el);
}

function resizeTextOverlay() {
  textOverlay.style.width = `${canvas.width}px`;
  textOverlay.style.height = `${canvas.height}px`;
  textOverlay.style.display = canvas.style.display === 'block' ? 'block' : 'none';
  textOverlay.style.transform = canvas.style.transform || '';
}

function renderTextOverlay() {
  if (!textOverlay) return;
  textOverlay.innerHTML = '';
  resizeTextOverlay();

  if (!Module || totalPages <= 0 || canvas.style.display !== 'block') return;

  const pageMatches = searchResults.filter(result => result.page === currentPage);
  for (const result of pageMatches) {
    const isCurrent = searchResults[currentSearchIndex] === result;
    const quads = result.quads && result.quads.length > 0
      ? result.quads
      : [{ x: result.x, y: result.y, width: result.width, height: result.height }];
    for (const quad of quads) {
      appendOverlayRect(quad, `text-highlight${isCurrent ? ' current' : ''}`);
    }
  }

  if (currentSelection && currentSelection.page === currentPage) {
    for (const segment of currentSelection.segments || []) {
      appendOverlayRect(segment.quad, 'selection-highlight');
    }
  }

  if (selectionDragBox) {
    const el = document.createElement('div');
    el.className = 'drag-selection-box';
    el.style.left = `${selectionDragBox.x}px`;
    el.style.top = `${selectionDragBox.y}px`;
    el.style.width = `${selectionDragBox.width}px`;
    el.style.height = `${selectionDragBox.height}px`;
    textOverlay.appendChild(el);
  }
}

function renderCurrentPage() {
  if (!Module || totalPages <= 0) return;
  if (!hasRendering) {
    setStatus(`Page ${currentPage + 1} / ${totalPages} (rendering not available)`);
    if (activeSidebarTab === 'info') renderInfoTab();
    return;
  }

  // Don't render main page while thumbnails are being rendered
  if (isThumbnailRendering) return;

  const pageWidth = Module._nanopdf_get_page_width(currentPage);
  const pageHeight = Module._nanopdf_get_page_height(currentPage);

  const baseScale = computeBaseScale(pageWidth, pageHeight);
  const effectiveScale = baseScale * zoomLevel;
  // Cap maximum render dimensions to avoid WASM memory issues
  const maxDim = 4096;
  const width = Math.min(maxDim, Math.floor(pageWidth * effectiveScale));
  const height = Math.min(maxDim, Math.floor(pageHeight * effectiveScale));

  canvas.width = width;
  canvas.height = height;

  const dpi = 72 * (width / pageWidth);

  let result;
  try {
    result = Module._nanopdf_render_page(currentPage, width, height, dpi);
  } catch (e) {
    console.error('Render crashed:', e);
    setStatus('Render error: WASM memory issue', true);
    return;
  }

  if (result !== 1) {
    const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
    setStatus('Render error: ' + error, true);
    return;
  }

  const bufferPtr = Module._nanopdf_get_render_buffer();
  const bufferSize = Module._nanopdf_get_render_buffer_size();
  const renderWidth = Module._nanopdf_get_render_width();
  const renderHeight = Module._nanopdf_get_render_height();

  const ctx = canvas.getContext('2d');
  const imageData = ctx.createImageData(renderWidth, renderHeight);
  const srcData = new Uint8ClampedArray(Module.HEAPU8.buffer, bufferPtr, bufferSize);
  imageData.data.set(srcData);
  ctx.putImageData(imageData, 0, 0);

  emptyState.style.display = 'none';
  canvas.style.display = 'block';
  resizeTextOverlay();

  // Apply rotation
  applyRotation();
  renderTextOverlay();

  const zoomPct = Math.round(zoomLevel * 100);
  setStatus(`Page ${currentPage + 1} / ${totalPages} | ${backendLabel(renderBackend)} | ${pageWidth.toFixed(0)} x ${pageHeight.toFixed(0)} pts | ${zoomPct}%${rotation !== 0 ? ' | ' + rotation + '°' : ''}`);
  statusRight.textContent = fileName;

  if (activeSidebarTab === 'info') renderInfoTab();
}

// ---- Text Extraction ----

function extractText() {
  if (!Module || totalPages <= 0) return;

  const textPtr = Module._nanopdf_extract_text(currentPage);
  const text = Module.UTF8ToString(textPtr);

  textContent.textContent = text || '(No text extracted)';
  textPanel.style.display = 'block';
}

// ---- Search ----

let searchDebounce = null;

function performSearch(term) {
  searchResults = [];
  currentSearchIndex = -1;
  currentSearchTerm = term;

  if (!Module || totalPages <= 0 || !term) {
    searchInfo.textContent = '';
    searchPrevBtn.disabled = true;
    searchNextBtn.disabled = true;
    renderTextOverlay();
    return;
  }

  const termPtr = Module.stringToNewUTF8(term);
  const pagesPtr = Module.stringToNewUTF8('all');
  const resultPtr = Module._nanopdf_search_text
    ? Module._nanopdf_search_text(termPtr, pagesPtr, 0, 0, -1)
    : Module._nanopdf_batch_find_text(termPtr, pagesPtr);
  const resultStr = Module.UTF8ToString(resultPtr);
  Module._free(termPtr);
  Module._free(pagesPtr);

  try {
    const data = JSON.parse(resultStr);
    if (Module._nanopdf_search_text && data.results) {
      for (const match of data.results) {
        searchResults.push({
          page: match.page,
          start: match.start,
          end: match.end,
          context: match.context,
          x: match.x,
          y: match.y,
          width: match.width,
          height: match.height,
          quads: match.quads || [],
          writingMode: match.writingMode || 'horizontal'
        });
      }
    } else if (data.results) {
      for (const pageResult of data.results) {
        for (const match of pageResult.matches) {
          searchResults.push({
            page: pageResult.page,
            start: match.start,
            end: match.end,
            context: match.context,
            x: match.x,
            y: match.y,
            width: match.width,
            height: match.height,
            quads: match.quads || [],
            writingMode: match.writingMode || 'horizontal'
          });
        }
      }
    }
  } catch (e) {
    console.error('Search parse error:', e);
  }

  if (searchResults.length > 0) {
    currentSearchIndex = 0;
    const firstPage = searchResults[0].page;
    if (firstPage !== currentPage) {
      goToPage(firstPage);
    }
    updateSearchInfo();
    searchPrevBtn.disabled = false;
    searchNextBtn.disabled = false;
    renderTextOverlay();
  } else {
    searchInfo.textContent = 'No matches';
    searchPrevBtn.disabled = true;
    searchNextBtn.disabled = true;
    renderTextOverlay();
  }
}

function updateSearchInfo() {
  if (searchResults.length === 0) {
    searchInfo.textContent = '';
    return;
  }
  searchInfo.textContent = `${currentSearchIndex + 1} / ${searchResults.length}`;
}

function nextSearchResult() {
  if (searchResults.length === 0) return;
  currentSearchIndex = (currentSearchIndex + 1) % searchResults.length;
  const result = searchResults[currentSearchIndex];
  if (result.page !== currentPage) {
    goToPage(result.page);
  }
  updateSearchInfo();
  renderTextOverlay();
}

function prevSearchResult() {
  if (searchResults.length === 0) return;
  currentSearchIndex = (currentSearchIndex - 1 + searchResults.length) % searchResults.length;
  const result = searchResults[currentSearchIndex];
  if (result.page !== currentPage) {
    goToPage(result.page);
  }
  updateSearchInfo();
  renderTextOverlay();
}

function clearSearch() {
  // Don't clear input or results, just reset current match tracking
}

// ---- Text Selection ----

function beginTextSelection(e) {
  if (!Module || totalPages <= 0 || canvas.style.display !== 'block') return;
  if (e.button !== 0) return;
  isSelectingText = true;
  currentSelection = null;
  selectionStartPoint = overlayPointToPdf(e.clientX, e.clientY);
  selectionDragBox = { x: selectionStartPoint.screenX, y: selectionStartPoint.screenY, width: 0, height: 0 };
  textOverlay.setPointerCapture(e.pointerId);
  renderTextOverlay();
  e.preventDefault();
}

function updateTextSelection(e) {
  if (!isSelectingText || !selectionStartPoint) return;
  const point = overlayPointToPdf(e.clientX, e.clientY);
  const x = Math.min(selectionStartPoint.screenX, point.screenX);
  const y = Math.min(selectionStartPoint.screenY, point.screenY);
  selectionDragBox = {
    x,
    y,
    width: Math.abs(point.screenX - selectionStartPoint.screenX),
    height: Math.abs(point.screenY - selectionStartPoint.screenY)
  };
  renderTextOverlay();
  e.preventDefault();
}

function finishTextSelection(e) {
  if (!isSelectingText || !selectionStartPoint) return;
  const endPoint = overlayPointToPdf(e.clientX, e.clientY);
  isSelectingText = false;
  selectionDragBox = null;

  const x1 = Math.min(selectionStartPoint.pdfX, endPoint.pdfX);
  const x2 = Math.max(selectionStartPoint.pdfX, endPoint.pdfX);
  const y1 = Math.min(selectionStartPoint.pdfY, endPoint.pdfY);
  const y2 = Math.max(selectionStartPoint.pdfY, endPoint.pdfY);
  selectionStartPoint = null;

  if (Math.abs(x2 - x1) < 1 || Math.abs(y2 - y1) < 1) {
    currentSelection = null;
    renderTextOverlay();
    return;
  }

  if (Module._nanopdf_select_text_rect) {
    const ptr = Module._nanopdf_select_text_rect(currentPage, x1, y1, x2, y2);
    const resultStr = Module.UTF8ToString(ptr);
    try {
      const selection = JSON.parse(resultStr);
      currentSelection = selection.error ? null : selection;
    } catch (err) {
      console.error('Selection parse error:', err);
      currentSelection = null;
    }
  }

  renderTextOverlay();
  if (currentSelection && currentSelection.text) {
    setStatus(`Selected ${currentSelection.text.length} characters`);
  }
  e.preventDefault();
}

function copyCurrentSelection(e) {
  if (!currentSelection || !currentSelection.text) return;
  e.clipboardData.setData('text/plain', currentSelection.text);
  e.preventDefault();
  setStatus('Selection copied');
}

// ---- PDF Loading ----

async function loadPDF(arrayBuffer, name) {
  if (!Module) return;

  showLoading('Loading PDF...');
  const bytes = new Uint8Array(arrayBuffer);

  try {
    const ptr = Module._nanopdf_malloc(bytes.length);
    Module.HEAPU8.set(bytes, ptr);

    totalPages = Module._nanopdf_load_pdf(ptr, bytes.length);
    Module._nanopdf_free(ptr);

    if (totalPages <= 0) {
      const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
      throw new Error(error || 'Failed to load PDF');
    }

    fileName = name;
    fileSize = bytes.length;
    currentPdfBytes = new Uint8Array(bytes);
    currentPage = 0;
    signatureData = null;
    editHistoryData = null;

    // Reset view state
    zoomLevel = 1.0;
    rotation = 0;
    fitMode = 'width';
    thumbnailCache = {};
    thumbnailRenderQueue = [];
    isThumbnailRendering = false;
    selectedPages = new Set();
    lastClickedPage = 0;
    protectExport.checked = false;
    exportPassword.value = '';
    exportOwnerPassword.value = '';
    searchResults = [];
    currentSearchIndex = -1;
    currentSearchTerm = '';
    currentSelection = null;
    selectionStartPoint = null;
    selectionDragBox = null;
    searchInput.value = '';
    searchInfo.textContent = '';
    updateZoomDisplay();
    updateExportButton();
    fitModeBtn.textContent = 'Fit Width';
    canvas.style.transform = '';
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';

    // Enable controls
    renderBtn.disabled = !hasRendering;
    updateBackendToggle();
    extractBtn.disabled = false;
    prevBtn.disabled = false;
    nextBtn.disabled = false;
    sidebarToggleBtn.disabled = false;
    searchInput.disabled = false;
    zoomInBtn.disabled = !hasRendering;
    zoomOutBtn.disabled = !hasRendering;
    fitModeBtn.disabled = !hasRendering;
    rotateBtn.disabled = !hasRendering;
    updateExportButton();

    updatePageDisplay();

    // Load outline
    loadOutline();
    loadSignatureInfo();
    editHistoryData = await loadRevisionHistory(currentPdfBytes, signatureData?.signatures || []);

    const hasSignatures = signatureData && Array.isArray(signatureData.signatures) &&
      signatureData.signatures.length > 0;

    // Auto-show sidebar if there are bookmarks or signatures
    if ((outlineData && outlineData.outline && outlineData.outline.length > 0) || hasSignatures) {
      sidebar.classList.remove('hidden');
      if (hasSignatures && (!outlineData || !outlineData.outline || outlineData.outline.length === 0)) {
        activeSidebarTab = 'signatures';
        document.querySelectorAll('.sidebar-tabs button').forEach(b => {
          b.classList.toggle('active', b.dataset.tab === activeSidebarTab);
        });
      }
      updateSidebar();
    }

    // Render first page
    if (hasRendering) {
      renderCurrentPage();
    } else {
      emptyState.style.display = 'none';
      canvas.style.display = 'none';
      setStatus(`Loaded ${name}: ${totalPages} page(s) (no rendering backend)`);
    }

    hideLoading();
    setStatus(`Loaded ${name}: ${totalPages} page(s), ${formatFileSize(fileSize)}`);
    statusRight.textContent = name;

  } catch (err) {
    hideLoading();
    setStatus('Error: ' + err.message, true);
    console.error(err);
  }
}

function loadOutline() {
  if (!Module) return;
  try {
    const hasOutline = Module._nanopdf_has_outline();
    if (hasOutline === 1) {
      const outlinePtr = Module._nanopdf_get_outline();
      const outlineStr = Module.UTF8ToString(outlinePtr);
      outlineData = JSON.parse(outlineStr);
    } else {
      outlineData = null;
    }
  } catch (e) {
    console.error('Outline parse error:', e);
    outlineData = null;
  }
}

function loadSignatureInfo() {
  signatureData = null;
  if (!Module || !Module._nanopdf_get_signatures) return;
  try {
    const sigPtr = Module._nanopdf_get_signatures();
    const sigStr = Module.UTF8ToString(sigPtr);
    const data = JSON.parse(sigStr);
    signatureData = data.error ? null : data;
  } catch (e) {
    console.error('Signature parse error:', e);
    signatureData = null;
  }
}

async function loadRevisionHistory(bytes, signatures) {
  if (Module && Module._nanopdf_get_revision_history) {
    try {
      const historyPtr = Module._nanopdf_get_revision_history();
      const historyStr = Module.UTF8ToString(historyPtr);
      const data = JSON.parse(historyStr);
      if (!data.error) {
        data.fileSize = bytes.length;
        return data;
      }
    } catch (e) {
      console.error('Revision history parse error:', e);
    }
  }

  return buildEditHistory(bytes, signatures);
}

function parseStartXrefBefore(pdfText, eofIndex) {
  const marker = 'startxref';
  const pos = pdfText.lastIndexOf(marker, eofIndex);
  if (pos < 0) return 0;
  const match = /^\s*(\d+)/.exec(pdfText.slice(pos + marker.length, eofIndex));
  return match ? parseInt(match[1], 10) : 0;
}

function parsePrevXref(pdfText, xrefOffset, eofIndex) {
  if (!xrefOffset || xrefOffset >= eofIndex) return 0;
  const chunk = pdfText.slice(xrefOffset, eofIndex);
  const match = /\/Prev\s+(\d+)/.exec(chunk);
  return match ? parseInt(match[1], 10) : 0;
}

function parseTraditionalXrefObjects(pdfText, xrefOffset, eofIndex) {
  if (!xrefOffset || pdfText.slice(xrefOffset, xrefOffset + 4) !== 'xref') return [];
  const objects = [];
  const lines = pdfText.slice(xrefOffset + 4, eofIndex).split(/\r?\n/);

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line || line === 'trailer') break;
    const header = /^(\d+)\s+(\d+)$/.exec(line);
    if (!header) continue;

    const startObj = parseInt(header[1], 10);
    const count = parseInt(header[2], 10);
    for (let j = 0; j < count && i + 1 + j < lines.length; j++) {
      const entry = lines[i + 1 + j].trim();
      if (/^\d{10}\s+\d{5}\s+n/.test(entry)) {
        objects.push(startObj + j);
      }
    }
    i += count;
  }

  return objects.filter(obj => obj !== 0);
}

async function sha256Hex(bytes) {
  if (!globalThis.crypto || !globalThis.crypto.subtle) return '';
  const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest))
    .map(value => value.toString(16).padStart(2, '0'))
    .join('');
}

async function buildEditHistory(bytes, signatures) {
  const pdfText = new TextDecoder('latin1').decode(bytes);
  const eofOffsets = [];
  let searchFrom = 0;

  while (true) {
    const pos = pdfText.indexOf('%%EOF', searchFrom);
    if (pos < 0) break;
    let end = pos + 5;
    if (pdfText[end] === '\r') end++;
    if (pdfText[end] === '\n') end++;
    eofOffsets.push(end);
    searchFrom = end;
  }

  if (eofOffsets.length === 0) {
    return { fileSize: bytes.length, revisions: [] };
  }

  const revisions = [];
  let previousEnd = 0;
  for (let i = 0; i < eofOffsets.length; i++) {
    const endOffset = eofOffsets[i];
    const xrefOffset = parseStartXrefBefore(pdfText, endOffset);
    revisions.push({
      revision: i + 1,
      startOffset: previousEnd,
      endOffset,
      sizeBytes: endOffset - previousEnd,
      xrefOffset,
      prevXrefOffset: parsePrevXref(pdfText, xrefOffset, endOffset),
      addedObjects: parseTraditionalXrefObjects(pdfText, xrefOffset, endOffset),
      modifiedObjects: [],
      deletedObjects: [],
      sha256: await sha256Hex(bytes.slice(0, endOffset)),
      associatedSignature: '',
      signingTime: '',
      modifiedAfterSignature: false,
      hasDocMDP: false,
      docMDPAllowed: true,
      docMDPStatus: ''
    });
    previousEnd = endOffset;
  }

  for (const sig of signatures || []) {
    if (!Array.isArray(sig.byteRange) || sig.byteRange.length !== 4) continue;
    const coverageEnd = sig.byteRange[2] + sig.byteRange[3];
    let best = null;
    for (const rev of revisions) {
      if (rev.endOffset >= coverageEnd && (!best || rev.endOffset < best.endOffset)) {
        best = rev;
      }
    }
    if (best) {
      best.associatedSignature = sig.name || 'Signature';
      best.signingTime = sig.date || sig.timestampDate || '';
      best.modifiedAfterSignature = coverageEnd < bytes.length;
    }
  }

  return { fileSize: bytes.length, revisions };
}

function validateSignature(index) {
  if (!Module || !Module._nanopdf_validate_signature) return;
  const target = document.getElementById(`signature-validation-${index}`);
  if (target) {
    target.textContent = 'Validating...';
    target.className = 'signature-validation pending';
  }

  try {
    const resultPtr = Module._nanopdf_validate_signature(index);
    const resultStr = Module.UTF8ToString(resultPtr);
    const result = JSON.parse(resultStr);
    if (!target) return;

    if (result.error) {
      target.textContent = result.error;
      target.className = 'signature-validation error';
      return;
    }

    const parts = [
      result.integrityValid ? 'Integrity OK' : 'Integrity failed',
      result.signatureValid ? 'PKCS#7 parsed' : 'PKCS#7 not verified'
    ];
    if (result.signerName) parts.push(`Signer: ${result.signerName}`);
    if (result.signingTime) parts.push(`Time: ${result.signingTime}`);
    if (result.digestAlgorithm) parts.push(`Digest: ${result.digestAlgorithm}`);
    target.textContent = parts.join(' | ');
    target.className = `signature-validation ${result.success ? 'success' : 'error'}`;
  } catch (e) {
    if (target) {
      target.textContent = e.message;
      target.className = 'signature-validation error';
    }
  }
}

// ---- PDF Export ----

function canvasToJpegBytes(cvs) {
  const dataUrl = cvs.toDataURL('image/jpeg', 0.92);
  const base64 = dataUrl.split(',')[1];
  const binaryString = atob(base64);
  const bytes = new Uint8Array(binaryString.length);
  for (let i = 0; i < binaryString.length; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return bytes;
}

function buildPdf(pageImages) {
  const enc = new TextEncoder();
  const parts = [];
  let offset = 0;
  const offsets = {};
  let nextObj = 1;

  function write(str) {
    const bytes = enc.encode(str);
    parts.push(bytes);
    offset += bytes.length;
  }

  function writeRaw(bytes) {
    parts.push(bytes);
    offset += bytes.length;
  }

  // Header
  write('%PDF-1.4\n%\xc0\xc1\xc2\xc3\n');

  const catalogObj = nextObj++;
  const pagesObj = nextObj++;
  const pageObjs = [];
  const contentObjs = [];
  const imageObjs = [];

  for (let i = 0; i < pageImages.length; i++) {
    pageObjs.push(nextObj++);
    contentObjs.push(nextObj++);
    imageObjs.push(nextObj++);
  }

  // Catalog
  offsets[catalogObj] = offset;
  write(`${catalogObj} 0 obj\n<< /Type /Catalog /Pages ${pagesObj} 0 R >>\nendobj\n`);

  // Pages
  const kidsStr = pageObjs.map(n => `${n} 0 R`).join(' ');
  offsets[pagesObj] = offset;
  write(`${pagesObj} 0 obj\n<< /Type /Pages /Kids [${kidsStr}] /Count ${pageImages.length} >>\nendobj\n`);

  // Each page
  for (let i = 0; i < pageImages.length; i++) {
    const img = pageImages[i];
    const pw = img.pageWidth.toFixed(2);
    const ph = img.pageHeight.toFixed(2);

    // Page object
    offsets[pageObjs[i]] = offset;
    write(`${pageObjs[i]} 0 obj\n<< /Type /Page /Parent ${pagesObj} 0 R /MediaBox [0 0 ${pw} ${ph}] /Contents ${contentObjs[i]} 0 R /Resources << /XObject << /Im0 ${imageObjs[i]} 0 R >> >> >>\nendobj\n`);

    // Content stream
    const contentStr = `q\n${pw} 0 0 ${ph} 0 0 cm\n/Im0 Do\nQ\n`;
    offsets[contentObjs[i]] = offset;
    write(`${contentObjs[i]} 0 obj\n<< /Length ${contentStr.length} >>\nstream\n`);
    write(contentStr);
    write('endstream\nendobj\n');

    // Image XObject
    offsets[imageObjs[i]] = offset;
    write(`${imageObjs[i]} 0 obj\n<< /Type /XObject /Subtype /Image /Width ${img.imgWidth} /Height ${img.imgHeight} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length ${img.jpegBytes.length} >>\nstream\n`);
    writeRaw(img.jpegBytes);
    write('\nendstream\nendobj\n');
  }

  // XRef table
  const xrefOffset = offset;
  const totalObjs = nextObj;
  write(`xref\n0 ${totalObjs}\n`);
  write('0000000000 65535 f \r\n');
  for (let i = 1; i < totalObjs; i++) {
    const off = offsets[i] || 0;
    write(String(off).padStart(10, '0') + ' 00000 n \r\n');
  }

  // Trailer
  write(`trailer\n<< /Size ${totalObjs} /Root ${catalogObj} 0 R >>\n`);
  write(`startxref\n${xrefOffset}\n%%EOF\n`);

  // Combine
  const totalLength = parts.reduce((sum, p) => sum + p.length, 0);
  const result = new Uint8Array(totalLength);
  let pos = 0;
  for (const part of parts) {
    result.set(part, pos);
    pos += part.length;
  }
  return result;
}

function downloadPdfBytes(pdfBytes, suffix, pages) {
  const blob = new Blob([pdfBytes], { type: 'application/pdf' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  const baseName = fileName.replace(/\.pdf$/i, '');
  a.download = `${baseName}_${suffix}_${pages.map(p => p + 1).join('-')}.pdf`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

async function exportSelectedPagesProtected(pages) {
  const userPassword = exportPassword.value;
  const ownerPassword = exportOwnerPassword.value || userPassword;
  if (!userPassword) {
    setStatus('Protected export requires a user password', true);
    exportPassword.focus();
    return false;
  }
  if (!currentPdfBytes || currentPdfBytes.length === 0) {
    setStatus('Protected export failed: no source PDF loaded', true);
    return false;
  }
  if (!Module._nanopdf_export_set_passwords) {
    setStatus('Protected export requires rebuilding the nanopdf WASM module', true);
    return false;
  }

  showLoading(`Exporting protected PDF (${pages.length} page(s))...`);
  await new Promise(r => setTimeout(r, 50));

  let docId = -1;
  try {
    const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, ptr);
    docId = Module._nanopdf_doc_load(ptr, currentPdfBytes.length);
    Module._nanopdf_free(ptr);
    if (docId < 0) {
      const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
      throw new Error(error || 'Failed to load source document for export');
    }

    Module._nanopdf_work_clear();
    for (const pageIdx of pages) {
      const workIndex = Module._nanopdf_work_add_page(docId, pageIdx);
      if (workIndex < 0) {
        const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
        throw new Error(error || `Failed to add page ${pageIdx + 1}`);
      }
    }

    const userPtr = Module.stringToNewUTF8(userPassword);
    const ownerPtr = Module.stringToNewUTF8(ownerPassword);
    const passwordResult = Module._nanopdf_export_set_passwords(userPtr, ownerPtr, 3);
    Module._free(userPtr);
    Module._free(ownerPtr);
    if (passwordResult !== 1) {
      const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
      throw new Error(error || 'Failed to configure password protection');
    }

    const exportResult = Module._nanopdf_export_pdf();
    if (exportResult !== 1) {
      const error = Module.UTF8ToString(Module._nanopdf_get_last_error());
      throw new Error(error || 'Protected export failed');
    }

    const outPtr = Module._nanopdf_export_get_buffer();
    const outSize = Module._nanopdf_export_get_size();
    const pdfBytes = new Uint8Array(Module.HEAPU8.buffer, outPtr, outSize).slice();
    downloadPdfBytes(pdfBytes, 'protected_pages', pages);
    setStatus(`Exported ${pages.length} protected page(s) as PDF`);
    return true;
  } catch (err) {
    setStatus('Protected export error: ' + err.message, true);
    console.error(err);
    return false;
  } finally {
    const emptyPtr = Module.stringToNewUTF8('');
    Module._nanopdf_export_set_passwords(emptyPtr, emptyPtr, 3);
    Module._free(emptyPtr);
    if (docId >= 0) {
      Module._nanopdf_doc_close(docId);
    }
    Module._nanopdf_work_clear();
    hideLoading();
  }
}

async function exportSelectedPages() {
  if (selectedPages.size === 0 || !hasRendering || !Module) return;

  const pages = [...selectedPages].sort((a, b) => a - b);

  if (protectExport.checked) {
    await exportSelectedPagesProtected(pages);
    renderCurrentPage();
    return;
  }

  showLoading(`Exporting ${pages.length} page(s)...`);

  // Small delay so loading overlay can render
  await new Promise(r => setTimeout(r, 50));

  const pageImages = [];

  for (let idx = 0; idx < pages.length; idx++) {
    const pageIdx = pages[idx];
    loadingText.textContent = `Rendering page ${idx + 1} / ${pages.length}...`;

    const pw = Module._nanopdf_get_page_width(pageIdx);
    const ph = Module._nanopdf_get_page_height(pageIdx);

    // Render at 150 DPI (2x 72 DPI) for good quality
    const scale = 150 / 72;
    const width = Math.min(4096, Math.floor(pw * scale));
    const height = Math.min(4096, Math.floor(ph * scale));
    const dpi = 72 * (width / pw);

    let result;
    try {
      result = Module._nanopdf_render_page(pageIdx, width, height, dpi);
    } catch (e) {
      console.error('Export render failed for page', pageIdx, e);
      continue;
    }
    if (result !== 1) continue;

    const bufferPtr = Module._nanopdf_get_render_buffer();
    const bufferSize = Module._nanopdf_get_render_buffer_size();
    const renderWidth = Module._nanopdf_get_render_width();
    const renderHeight = Module._nanopdf_get_render_height();

    const tmpCanvas = document.createElement('canvas');
    tmpCanvas.width = renderWidth;
    tmpCanvas.height = renderHeight;
    const ctx = tmpCanvas.getContext('2d');
    const imageData = ctx.createImageData(renderWidth, renderHeight);
    const srcData = new Uint8ClampedArray(Module.HEAPU8.buffer, bufferPtr, bufferSize);
    imageData.data.set(srcData);
    ctx.putImageData(imageData, 0, 0);

    const jpegBytes = canvasToJpegBytes(tmpCanvas);
    pageImages.push({
      jpegBytes,
      imgWidth: renderWidth,
      imgHeight: renderHeight,
      pageWidth: pw,
      pageHeight: ph,
    });

    // Yield to UI between pages
    await new Promise(r => setTimeout(r, 10));
  }

  if (pageImages.length === 0) {
    hideLoading();
    setStatus('Export failed: no pages could be rendered', true);
    return;
  }

  loadingText.textContent = 'Building PDF...';
  await new Promise(r => setTimeout(r, 50));

  const pdfBytes = buildPdf(pageImages);

  downloadPdfBytes(pdfBytes, 'pages', pages);

  hideLoading();

  // Re-render current page to restore main canvas
  renderCurrentPage();
  setStatus(`Exported ${pageImages.length} page(s) as PDF`);
}

// ---- Window Resize ----

let resizeDebounce = null;

function onResize() {
  clearTimeout(resizeDebounce);
  resizeDebounce = setTimeout(() => {
    if (hasRendering && totalPages > 0) {
      renderCurrentPage();
    }
  }, 150);
}

// ---- Event Handlers ----

// File input
openPdfBtn.addEventListener('click', () => pdfInput.click());
pdfInput.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  const arrayBuffer = await file.arrayBuffer();
  loadPDF(arrayBuffer, file.name);
  // Reset so the same file can be re-selected
  pdfInput.value = '';
});

// Drag and drop
let dragCounter = 0;
document.addEventListener('dragenter', (e) => {
  e.preventDefault();
  dragCounter++;
  dropOverlay.classList.add('active');
});
document.addEventListener('dragleave', (e) => {
  e.preventDefault();
  dragCounter--;
  if (dragCounter <= 0) {
    dragCounter = 0;
    dropOverlay.classList.remove('active');
  }
});
document.addEventListener('dragover', (e) => {
  e.preventDefault();
});
document.addEventListener('drop', async (e) => {
  e.preventDefault();
  dragCounter = 0;
  dropOverlay.classList.remove('active');

  const files = e.dataTransfer.files;
  if (files.length > 0 && files[0].type === 'application/pdf') {
    const arrayBuffer = await files[0].arrayBuffer();
    loadPDF(arrayBuffer, files[0].name);
  } else if (files.length > 0 && files[0].name.toLowerCase().endsWith('.pdf')) {
    const arrayBuffer = await files[0].arrayBuffer();
    loadPDF(arrayBuffer, files[0].name);
  }
});

// Page navigation
prevBtn.addEventListener('click', () => {
  if (currentPage > 0) goToPage(currentPage - 1);
});
nextBtn.addEventListener('click', () => {
  if (currentPage < totalPages - 1) goToPage(currentPage + 1);
});
pageDisplay.addEventListener('click', enterPageJump);

// Zoom buttons
zoomInBtn.addEventListener('click', zoomIn);
zoomOutBtn.addEventListener('click', zoomOut);

// Fit mode toggle
fitModeBtn.addEventListener('click', toggleFitMode);

// Rotate button
rotateBtn.addEventListener('click', rotateClockwise);

// Ctrl+scroll wheel zoom
canvasScroll.addEventListener('wheel', (e) => {
  if (e.ctrlKey || e.metaKey) {
    e.preventDefault();
    if (e.deltaY < 0) {
      zoomIn();
    } else {
      zoomOut();
    }
  }
}, { passive: false });

// Keyboard navigation
document.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;

  // Revision diff overlay captures navigation keys while open.
  if (diffState.open) {
    if (e.key === 'Escape') { e.preventDefault(); closeRevisionDiff(); }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); stepDiffPage(-1); }
    else if (e.key === 'ArrowRight') { e.preventDefault(); stepDiffPage(1); }
    return;
  }

  if (e.key === 'ArrowLeft') {
    e.preventDefault();
    if (currentPage > 0) goToPage(currentPage - 1);
  } else if (e.key === 'ArrowRight') {
    e.preventDefault();
    if (currentPage < totalPages - 1) goToPage(currentPage + 1);
  } else if (e.key === 'Home') {
    e.preventDefault();
    if (totalPages > 0) goToPage(0);
  } else if (e.key === 'End') {
    e.preventDefault();
    if (totalPages > 0) goToPage(totalPages - 1);
  } else if ((e.ctrlKey || e.metaKey) && e.key === 'f') {
    e.preventDefault();
    searchInput.focus();
  } else if ((e.ctrlKey || e.metaKey) && (e.key === '=' || e.key === '+')) {
    e.preventDefault();
    zoomIn();
  } else if ((e.ctrlKey || e.metaKey) && e.key === '-') {
    e.preventDefault();
    zoomOut();
  } else if ((e.ctrlKey || e.metaKey) && e.key === '0') {
    e.preventDefault();
    resetZoom();
  } else if (e.key === 'r' || e.key === 'R') {
    if (!e.ctrlKey && !e.metaKey) {
      rotateClockwise();
    }
  }
});

// Render / Extract / Export buttons
renderBtn.addEventListener('click', renderCurrentPage);
backendToggleBtn.addEventListener('click', switchRenderBackend);
extractBtn.addEventListener('click', extractText);
exportBtn.addEventListener('click', exportSelectedPages);
protectExport.addEventListener('change', updateExportButton);

// Close text panel
document.getElementById('text-panel-close').addEventListener('click', () => {
  textPanel.style.display = 'none';
});

// Sidebar toggle
sidebarToggleBtn.addEventListener('click', () => {
  sidebar.classList.toggle('hidden');
  updateSidebar();
  if (hasRendering && totalPages > 0) {
    setTimeout(renderCurrentPage, 50);
  }
});

// Sidebar tabs
document.querySelectorAll('.sidebar-tabs button').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.sidebar-tabs button').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    activeSidebarTab = btn.dataset.tab;
    updateSidebar();
  });
});

// Search
searchInput.addEventListener('input', (e) => {
  clearTimeout(searchDebounce);
  searchDebounce = setTimeout(() => {
    performSearch(e.target.value.trim());
  }, 300);
});
searchInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    if (e.shiftKey) {
      prevSearchResult();
    } else {
      nextSearchResult();
    }
  } else if (e.key === 'Escape') {
    searchInput.blur();
  }
});
searchPrevBtn.addEventListener('click', prevSearchResult);
searchNextBtn.addEventListener('click', nextSearchResult);

// Text selection overlay
textOverlay.addEventListener('pointerdown', beginTextSelection);
textOverlay.addEventListener('pointermove', updateTextSelection);
textOverlay.addEventListener('pointerup', finishTextSelection);
textOverlay.addEventListener('pointercancel', finishTextSelection);
document.addEventListener('copy', copyCurrentSelection);

// Window resize
window.addEventListener('resize', onResize);

// ---- Initialize ----

async function init() {
  showLoading('Loading WASM module...');
  try {
    Module = await createModule({
      locateFile: (path) => {
        if (path.endsWith('.wasm')) return wasmUrl;
        return path;
      },
    });

    const result = Module._nanopdf_init();
    if (result !== 1) {
      throw new Error('Failed to initialize nanopdf');
    }

    hasRendering = Module._nanopdf_has_rendering() === 1;
    syncRenderBackendFromModule();

    // Load external fonts if embedded fonts are not available
    const embeddedFontsAvailable = Module._nanopdf_fonts_available() === 1;
    if (!embeddedFontsAvailable) {
      try {
        showLoading('Loading fonts...');
        const stdCount = await loadStandardFonts(Module, FONTS_BASE, (ratio, name) => {
          loadingText.textContent = `Loading font: ${name} (${Math.round(ratio * 100)}%)`;
        });
        console.log(`Loaded ${stdCount} standard fonts from external files`);

        // Load CJK fonts in the background (they're large, ~10MB). These are
        // not self-hosted — they stream from the jsDelivr CDN (@embedpdf/fonts-jp)
        // so the deployed site stays small. See loadCDNCJKFonts in font-loader.js.
        loadCDNCJKFonts(Module, (ratio, name) => {
          console.log(`CJK font: ${name} (${Math.round(ratio * 100)}%)`);
        }).then(cjkCount => {
          if (cjkCount > 0) {
            console.log(`Loaded ${cjkCount} CJK fonts from CDN`);
          }
        }).catch(e => {
          console.log('CJK fonts not available:', e.message);
        });
      } catch (e) {
        console.log('External fonts not available:', e.message);
      }
    }

    hideLoading();

    if (!hasRendering) {
      setStatus('Ready (rendering not available - build with NANOPDF_USE_LIGHTVG or NANOPDF_USE_THORVG for rendering)');
      renderBtn.style.display = 'none';
      updateBackendToggle();
    } else {
      setStatus(`Ready - ${backendLabel(renderBackend)} backend`);
    }

    console.log('nanopdf WASM loaded. Rendering:', hasRendering);
  } catch (err) {
    hideLoading();
    setStatus('Failed to load WASM: ' + err.message, true);
    console.error(err);
  }
}

init();
