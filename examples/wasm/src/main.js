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
// CSS-pixel display size of the page canvas (the bitmap is this * devicePixelRatio).
// Overlay/coordinate math uses these, NOT canvas.width/height (which are device px).
let pageDisplayWidth = 0;
let pageDisplayHeight = 0;
let thumbnailCache = {}; // pageIndex -> ImageData
let thumbnailRenderQueue = [];
let isThumbnailRendering = false;
let thumbnailObserver = null;

// Page selection state
let selectedPages = new Set();
let lastClickedPage = 0;

// ---- Annotation state ----
// annotations[pageIndex] = array of annotation objects. Geometry is stored in
// PDF page points with a bottom-left origin so it survives zoom/rotation and
// maps 1:1 to the nanopdf_annot_* export API.
let annotations = {};
let annotTool = 'select';        // select | highlight | text | rect | oval | line | arrow | ink
let annotColorHex = '#ffd54a';
let annotFillShapes = false;
let selectedAnnotId = null;
let annotIdCounter = 1;
let annotDraft = null;           // in-progress drawing
let annotDragState = null;       // moving an existing annotation in select mode
let pendingStampImage = null;    // { imageId, dataURL, aspect } awaiting placement
// formFields[pageIndex] = [{name, type, value, rect:{x,y,width,height}, multiline, ...}]
let formFieldsByPage = {};
const SVG_NS = 'http://www.w3.org/2000/svg';

const ZOOM_STEPS = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0];
const BACKEND_LIGHTVG = 0;
const BACKEND_THORVG = 1;

// DOM refs
const loadingOverlay = document.getElementById('loading-overlay');
const loadingText = document.getElementById('loading-text');
const dropOverlay = document.getElementById('drop-overlay');
const pdfInput = document.getElementById('pdf-input');
const openPdfBtn = document.getElementById('open-pdf-btn');
const openUrlBtn = document.getElementById('open-url-btn');
const combineBtn = document.getElementById('combine-btn');
const combineInput = document.getElementById('combine-input');
const extractPagesBtn = document.getElementById('extract-pages-btn');
const organizeBtn = document.getElementById('organize-btn');
const signBtn = document.getElementById('sign-btn');
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
// Annotation tooling
const annotTools = document.getElementById('annot-tools');
const annotLayer = document.getElementById('annot-layer');
const annotSvg = document.getElementById('annot-svg');
const annotHtml = document.getElementById('annot-html');
const annotColor = document.getElementById('annot-color');
const annotFill = document.getElementById('annot-fill');
const annotDeleteBtn = document.getElementById('annot-delete');
const saveAnnotBtn = document.getElementById('save-annot-btn');
const saveEditableBtn = document.getElementById('save-editable-btn');
const saveFormBtn = document.getElementById('save-form-btn');
const markupPopover = document.getElementById('markup-popover');
const stampInput = document.getElementById('stamp-input');

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
    html += `<div style="display:flex;gap:6px;">`;
    html += `<button type="button" class="validate-signature-btn" data-signature-index="${index}" ${sig.signaturePresent ? '' : 'disabled'}>Validate</button>`;
    html += `<button type="button" class="trust-signature-btn" data-signature-index="${index}" title="Check the signer chain against a CA bundle (PEM)" ${sig.signaturePresent ? '' : 'disabled'}>Check trust…</button>`;
    html += `</div>`;
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
    html += `<div class="signature-validation" id="signature-trust-${index}"></div>`;
    html += '</section>';
  });
  html += '</div>';

  sidebarContent.innerHTML = html;
  sidebarContent.querySelectorAll('.validate-signature-btn').forEach(btn => {
    btn.addEventListener('click', () => validateSignature(parseInt(btn.dataset.signatureIndex, 10)));
  });
  sidebarContent.querySelectorAll('.trust-signature-btn').forEach(btn => {
    btn.addEventListener('click', () => checkSignatureTrust(parseInt(btn.dataset.signatureIndex, 10)));
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

  if (diffState.mode === 'side') {
    // Before | After in two synchronized panes (one image => scroll/zoom synced).
    const gap = 14;
    const W = w * 2 + gap;
    const out2 = ctx.createImageData(W, h);
    const o2 = out2.data;
    o2.fill(255);
    for (let i = 3; i < o2.length; i += 4) o2[i] = 255;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const si = (y * w + x) * 4;
        if (A) {
          const dl = (y * W + x) * 4;
          o2[dl] = A[si]; o2[dl + 1] = A[si + 1]; o2[dl + 2] = A[si + 2]; o2[dl + 3] = 255;
        }
        const dr = (y * W + (w + gap) + x) * 4;
        o2[dr] = B[si]; o2[dr + 1] = B[si + 1]; o2[dr + 2] = B[si + 2]; o2[dr + 3] = 255;
      }
    }
    return out2;
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
          <button data-mode="side">Side-by-side</button>
          <button data-mode="before">Before</button>
          <button data-mode="after">After</button>
          <button data-mode="swipe">Swipe</button>
        </div>
        <button class="rev-diff-close" id="rev-diff-close" title="Close">&times;</button>
      </div>
      <div class="rev-diff-changeinfo" id="rev-diff-changeinfo"></div>
      <div class="rev-diff-body">
        <canvas id="rev-diff-canvas"></canvas>
        <div class="rev-diff-empty hidden" id="rev-diff-empty"></div>
      </div>
      <div class="rev-diff-strip" id="rev-diff-strip"></div>
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
  const changedCount = diffState.changedComputed ? diffState.changed.size : null;
  document.getElementById('rev-diff-pageinfo').textContent =
    `Page ${diffState.page + 1} / ${diffState.pageCount}` +
    (changedCount != null ? ` — ${changedCount} changed` : '');
  overlay.querySelector('#rev-diff-prev').disabled = diffState.page <= 0;
  overlay.querySelector('#rev-diff-next').disabled = diffState.page >= diffState.pageCount - 1;
  // Keep the strip's "current" highlight in sync.
  const strip = document.getElementById('rev-diff-strip');
  if (strip) {
    strip.querySelectorAll('.rev-diff-chip').forEach((c, i) => {
      c.classList.toggle('current', i === diffState.page);
    });
  }
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
  renderDiffChangeInfo(rev);

  // Precompute which pages changed (cap the scan for very large docs), and
  // land on the first changed page.
  diffState.changed = new Set();
  const scanLimit = diffState.pageCount <= 60 ? diffState.pageCount : 0;
  diffState.changedComputed = scanLimit > 0;
  let first = -1;
  for (let p = 0; p < scanLimit; p++) {
    diffState.page = p;
    loadDiffPage();
    if (diffPageChanged()) { diffState.changed.add(p); if (first < 0) first = p; }
  }
  diffState.page = first >= 0 ? first : Math.min(currentPage, diffState.pageCount - 1);
  loadDiffPage();
  renderDiffStrip();
  paintDiffCanvas();
}

// Structural-change summary + DocMDP/tamper banner for the diff overlay.
function renderDiffChangeInfo(rev) {
  const el = document.getElementById('rev-diff-changeinfo');
  if (!el) return;
  const added = (rev.addedObjects || []).length;
  const modified = (rev.modifiedObjects || []).length;
  const deleted = (rev.deletedObjects || []).length;
  const parts = [];
  if (added) parts.push(`<span class="chg add">+${added} added</span>`);
  if (modified) parts.push(`<span class="chg mod">~${modified} changed</span>`);
  if (deleted) parts.push(`<span class="chg del">-${deleted} removed</span>`);
  const objLine = parts.length
    ? `<span class="chg-objs">Objects: ${parts.join(' ')}</span>`
    : '';

  let banner = '';
  const violations = rev.docMDPViolations || [];
  if (rev.hasDocMDP && rev.docMDPStatus === 'disallowed') {
    banner = `<span class="chg-banner danger">⚠ Changes after signature are DISALLOWED by DocMDP` +
      (violations.length ? `: ${violations[0]}` : '') + `</span>`;
  } else if (rev.modifiedAfterSignature) {
    banner = `<span class="chg-banner warn">Modified after signing` +
      (rev.hasDocMDP ? ' (allowed by DocMDP)' : '') + `</span>`;
  }

  el.innerHTML = banner + objLine;
  el.classList.toggle('hidden', !banner && !objLine);
}

// Footer strip of page chips; changed pages highlighted, click to jump.
function renderDiffStrip() {
  const strip = document.getElementById('rev-diff-strip');
  if (!strip) return;
  strip.innerHTML = '';
  if (!diffState.changedComputed || diffState.pageCount <= 1) {
    strip.classList.add('hidden');
    return;
  }
  strip.classList.remove('hidden');
  for (let p = 0; p < diffState.pageCount; p++) {
    const chip = document.createElement('button');
    chip.className = 'rev-diff-chip';
    if (diffState.changed.has(p)) chip.classList.add('changed');
    if (p === diffState.page) chip.classList.add('current');
    chip.textContent = String(p + 1);
    chip.title = `Page ${p + 1}${diffState.changed.has(p) ? ' (changed)' : ''}`;
    chip.addEventListener('click', () => {
      diffState.page = p;
      loadDiffPage();
      renderDiffStrip();
      paintDiffCanvas();
    });
    strip.appendChild(chip);
  }
}

function closeRevisionDiff() {
  const overlay = document.getElementById('rev-diff-overlay');
  if (overlay) overlay.classList.add('hidden');
  diffState.open = false;
  diffState.before = diffState.after = null;
}

// ---- Digital signing (PKCS#12, in-browser, OpenSSL-free) ----

let signP12Bytes = null;   // Uint8Array of the uploaded .p12/.pfx

function ensureSignOverlay() {
  let el = document.getElementById('sign-overlay');
  if (el) return el;
  el = document.createElement('div');
  el.id = 'sign-overlay';
  el.className = 'organize-overlay hidden';
  el.innerHTML = `
    <div class="organize-panel" style="width:460px;max-width:92vw;">
      <div class="organize-header">
        <div class="organize-title">Sign PDF</div>
        <button id="sign-cancel">Cancel</button>
      </div>
      <div style="padding:18px 20px;display:flex;flex-direction:column;gap:12px;">
        <label style="display:flex;flex-direction:column;gap:4px;font-size:13px;color:#444;">
          Certificate (.p12 / .pfx)
          <input type="file" id="sign-p12" accept=".p12,.pfx,application/x-pkcs12" />
        </label>
        <label style="display:flex;flex-direction:column;gap:4px;font-size:13px;color:#444;">
          Certificate password
          <input type="password" id="sign-pass" placeholder="PKCS#12 password" />
        </label>
        <label style="display:flex;flex-direction:column;gap:4px;font-size:13px;color:#444;">
          Reason (optional)
          <input type="text" id="sign-reason" placeholder="e.g. I approve this document" />
        </label>
        <label style="display:flex;flex-direction:column;gap:4px;font-size:13px;color:#444;">
          Location (optional)
          <input type="text" id="sign-location" placeholder="e.g. Tokyo, JP" />
        </label>
        <label style="display:flex;align-items:center;gap:8px;font-size:13px;color:#444;">
          <input type="checkbox" id="sign-visible" checked />
          Visible signature on current page (bottom-left)
        </label>
        <label style="display:flex;align-items:center;gap:8px;font-size:13px;color:#444;">
          <input type="checkbox" id="sign-tsa" />
          Add trusted timestamp (RFC 3161 / PAdES-T)
        </label>
        <label id="sign-tsa-url-row" style="display:none;flex-direction:column;gap:4px;font-size:13px;color:#444;">
          Timestamp authority URL
          <input type="text" id="sign-tsa-url" value="https://freetsa.org/tsr" />
          <span style="font-size:11px;color:#999;">The TSA must allow browser (CORS) requests; many don't. If it fails, signing falls back to no timestamp.</span>
        </label>
        <div id="sign-status" style="font-size:13px;color:#666;min-height:18px;"></div>
      </div>
      <div class="organize-footer">
        <button class="primary" id="sign-go">Sign &amp; download</button>
      </div>
    </div>`;
  document.body.appendChild(el);
  el.querySelector('#sign-cancel').addEventListener('click', closeSign);
  el.addEventListener('click', (e) => { if (e.target === el) closeSign(); });
  el.querySelector('#sign-p12').addEventListener('change', async (e) => {
    const f = e.target.files[0];
    if (!f) { signP12Bytes = null; return; }
    signP12Bytes = new Uint8Array(await f.arrayBuffer());
    setSignStatus(`Loaded ${f.name} (${signP12Bytes.length} bytes)`, '#2a7');
  });
  el.querySelector('#sign-go').addEventListener('click', doSign);
  el.querySelector('#sign-tsa').addEventListener('change', (e) => {
    el.querySelector('#sign-tsa-url-row').style.display = e.target.checked ? 'flex' : 'none';
  });
  return el;
}

function setSignStatus(msg, color) {
  const s = document.getElementById('sign-status');
  if (s) { s.textContent = msg; s.style.color = color || '#666'; }
}

function openSign() {
  if (!Module || !Module._nanopdf_sign_pdf || !currentPdfBytes) return;
  signP12Bytes = null;
  const el = ensureSignOverlay();
  el.querySelector('#sign-p12').value = '';
  el.querySelector('#sign-pass').value = '';
  setSignStatus('');
  el.classList.remove('hidden');
}

function closeSign() {
  const el = document.getElementById('sign-overlay');
  if (el) el.classList.add('hidden');
}

function finishSign(suffix) {
  const outPtr = Module._nanopdf_sign_get_buffer();
  const outLen = Module._nanopdf_sign_get_size();
  const signed = new Uint8Array(Module.HEAPU8.buffer, outPtr, outLen).slice();
  const baseName = fileName.replace(/\.pdf$/i, '');
  downloadNamedPdf(signed, `${baseName}_${suffix}.pdf`);
  return outLen;
}

async function doSign() {
  if (!signP12Bytes) { setSignStatus('Choose a .p12/.pfx certificate first.', '#c33'); return; }
  const pass = document.getElementById('sign-pass').value || '';
  const reason = document.getElementById('sign-reason').value || '';
  const location = document.getElementById('sign-location').value || '';
  const visible = document.getElementById('sign-visible').checked ? 1 : 0;
  const wantTsa = document.getElementById('sign-tsa').checked;
  const tsaUrl = (document.getElementById('sign-tsa-url').value || '').trim();
  setSignStatus('Signing…');

  // Signature rectangle: bottom-left of the current page (page points).
  const x = 36, y = 36, w = 200, h = 48;

  let pdfPtr = 0, p12Ptr = 0, passPtr = 0, reasonPtr = 0, locPtr = 0, contactPtr = 0;
  const freeAll = () => {
    if (pdfPtr) Module._nanopdf_free(pdfPtr);
    if (p12Ptr) Module._nanopdf_free(p12Ptr);
    if (passPtr) Module._free(passPtr);
    if (reasonPtr) Module._free(reasonPtr);
    if (locPtr) Module._free(locPtr);
    if (contactPtr) Module._free(contactPtr);
    pdfPtr = p12Ptr = passPtr = reasonPtr = locPtr = contactPtr = 0;
  };
  const lastErr = () => Module.UTF8ToString(Module._nanopdf_get_last_error());

  try {
    pdfPtr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, pdfPtr);
    p12Ptr = Module._nanopdf_malloc(signP12Bytes.length);
    Module.HEAPU8.set(signP12Bytes, p12Ptr);
    passPtr = Module.stringToNewUTF8(pass);
    reasonPtr = Module.stringToNewUTF8(reason);
    locPtr = Module.stringToNewUTF8(location);
    contactPtr = Module.stringToNewUTF8('');

    if (wantTsa && tsaUrl && Module._nanopdf_sign_prepare) {
      // Phase A: build the deterministic signature + RFC 3161 request.
      const nLo = (Math.random() * 0xffffffff) >>> 0;
      const nHi = (Math.random() * 0xffffffff) >>> 0;
      const okPrep = Module._nanopdf_sign_prepare(
        pdfPtr, currentPdfBytes.length, p12Ptr, signP12Bytes.length, passPtr,
        reasonPtr, locPtr, contactPtr, currentPage, x, y, w, h, visible, nLo, nHi);
      if (!okPrep) { setSignStatus('Sign failed: ' + lastErr(), '#c33'); return; }

      const reqPtr = Module._nanopdf_sign_tsreq_buffer();
      const reqLen = Module._nanopdf_sign_tsreq_size();
      const tsReq = new Uint8Array(Module.HEAPU8.buffer, reqPtr, reqLen).slice();

      setSignStatus('Requesting timestamp from TSA…');
      let respBytes = null;
      try {
        const resp = await fetch(tsaUrl, {
          method: 'POST',
          headers: { 'Content-Type': 'application/timestamp-query' },
          body: tsReq,
        });
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        respBytes = new Uint8Array(await resp.arrayBuffer());
      } catch (netErr) {
        // CORS / network failure: fall back to a non-timestamped signature.
        setSignStatus('TSA unreachable (' + netErr.message + '); signing without timestamp…', '#c80');
        const okPlain = Module._nanopdf_sign_pdf(
          pdfPtr, currentPdfBytes.length, p12Ptr, signP12Bytes.length, passPtr,
          reasonPtr, locPtr, contactPtr, currentPage, x, y, w, h, visible);
        if (!okPlain) { setSignStatus('Sign failed: ' + lastErr(), '#c33'); return; }
        const n = finishSign('signed');
        setSignStatus(`Signed without timestamp (${n} bytes). Downloaded.`, '#c80');
        setTimeout(closeSign, 1400);
        return;
      }

      // Phase B: embed the TSA token.
      const respPtr = Module._nanopdf_malloc(respBytes.length);
      Module.HEAPU8.set(respBytes, respPtr);
      const okFin = Module._nanopdf_sign_finalize(respPtr, respBytes.length);
      Module._nanopdf_free(respPtr);
      if (!okFin) { setSignStatus('Timestamp failed: ' + lastErr(), '#c33'); return; }
      const n = finishSign('signed_ts');
      setSignStatus(`Signed + timestamped (${n} bytes). Downloaded.`, '#2a7');
      setTimeout(closeSign, 900);
      return;
    }

    // No timestamp: one-shot signing.
    const ok = Module._nanopdf_sign_pdf(
      pdfPtr, currentPdfBytes.length, p12Ptr, signP12Bytes.length, passPtr,
      reasonPtr, locPtr, contactPtr, currentPage, x, y, w, h, visible);
    if (!ok) { setSignStatus('Sign failed: ' + lastErr(), '#c33'); return; }
    const n = finishSign('signed');
    setSignStatus(`Signed (${n} bytes). Downloaded.`, '#2a7');
    setTimeout(closeSign, 900);
  } catch (e) {
    setSignStatus('Sign error: ' + e, '#c33');
  } finally {
    freeAll();
  }
}

// ---- Page organizer (reorder / rotate / delete) ----

// organizeModel: ordered array of { src: originalPageIndex, rot: 0|90|180|270 }
let organizeModel = [];
let organizeThumbCache = {};
let organizeDragIndex = -1;

function renderPageToCanvas(pageIdx, canvas, maxW) {
  const pw = Module._nanopdf_get_page_width(pageIdx) || 612;
  const ph = Module._nanopdf_get_page_height(pageIdx) || 792;
  const scale = maxW / pw;
  const w = Math.max(1, Math.round(pw * scale));
  const h = Math.max(1, Math.round(ph * scale));
  let ok = 0;
  try { ok = Module._nanopdf_render_page(pageIdx, w, h, 72 * (w / pw)); } catch (e) { return false; }
  if (ok !== 1) return false;
  const ptr = Module._nanopdf_get_render_buffer();
  const size = Module._nanopdf_get_render_buffer_size();
  const rw = Module._nanopdf_get_render_width();
  const rh = Module._nanopdf_get_render_height();
  canvas.width = rw; canvas.height = rh;
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(rw, rh);
  img.data.set(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, size));
  ctx.putImageData(img, 0, 0);
  return true;
}

function organizeThumb(src) {
  if (organizeThumbCache[src]) return organizeThumbCache[src];
  const c = document.createElement('canvas');
  const url = renderPageToCanvas(src, c, 120) ? c.toDataURL('image/png') : '';
  organizeThumbCache[src] = url;
  return url;
}

function ensureOrganizeOverlay() {
  let el = document.getElementById('organize-overlay');
  if (el) return el;
  el = document.createElement('div');
  el.id = 'organize-overlay';
  el.className = 'organize-overlay hidden';
  el.innerHTML = `
    <div class="organize-panel">
      <div class="organize-header">
        <div class="organize-title">Organize pages</div>
        <button id="organize-reset">Reset</button>
        <button id="organize-cancel">Cancel</button>
      </div>
      <div class="organize-grid" id="organize-grid"></div>
      <div class="organize-footer">
        <span id="organize-info" style="margin-right:auto;color:#666;font-size:13px;"></span>
        <button class="primary" id="organize-save">Save reorganized PDF</button>
      </div>
    </div>`;
  document.body.appendChild(el);
  el.querySelector('#organize-cancel').addEventListener('click', closeOrganize);
  el.querySelector('#organize-reset').addEventListener('click', () => { openOrganize(); });
  el.querySelector('#organize-save').addEventListener('click', saveOrganized);
  el.addEventListener('click', (e) => { if (e.target === el) closeOrganize(); });
  return el;
}

function openOrganize() {
  if (!Module || totalPages <= 0 || !hasRendering) return;
  organizeModel = [];
  for (let i = 0; i < totalPages; i++) organizeModel.push({ src: i, rot: 0 });
  organizeThumbCache = {};
  const el = ensureOrganizeOverlay();
  el.classList.remove('hidden');
  renderOrganizeGrid();
}

function closeOrganize() {
  const el = document.getElementById('organize-overlay');
  if (el) el.classList.add('hidden');
}

function renderOrganizeGrid() {
  const grid = document.getElementById('organize-grid');
  if (!grid) return;
  grid.innerHTML = '';
  organizeModel.forEach((entry, idx) => {
    const card = document.createElement('div');
    card.className = 'org-card';
    card.draggable = true;
    card.dataset.idx = idx;
    const img = document.createElement('img');
    img.src = organizeThumb(entry.src);
    img.style.transform = entry.rot ? `rotate(${entry.rot}deg)` : '';
    const label = document.createElement('div');
    label.className = 'org-card-label';
    label.textContent = `p.${entry.src + 1}`;
    const actions = document.createElement('div');
    actions.className = 'org-card-actions';
    const mk = (txt, title, fn, cls) => {
      const b = document.createElement('button');
      b.textContent = txt; b.title = title; if (cls) b.className = cls;
      b.addEventListener('click', (e) => { e.stopPropagation(); fn(); });
      return b;
    };
    actions.append(
      mk('⟲', 'Rotate left', () => { entry.rot = ((entry.rot - 90) % 360 + 360) % 360; renderOrganizeGrid(); }),
      mk('⟳', 'Rotate right', () => { entry.rot = (entry.rot + 90) % 360; renderOrganizeGrid(); }),
      mk('🗑', 'Delete page', () => { organizeModel.splice(idx, 1); renderOrganizeGrid(); }, 'del'),
    );
    card.append(img, label, actions);

    card.addEventListener('dragstart', () => { organizeDragIndex = idx; card.classList.add('dragging'); });
    card.addEventListener('dragend', () => { organizeDragIndex = -1; card.classList.remove('dragging'); });
    card.addEventListener('dragover', (e) => { e.preventDefault(); card.classList.add('drag-over'); });
    card.addEventListener('dragleave', () => card.classList.remove('drag-over'));
    card.addEventListener('drop', (e) => {
      e.preventDefault();
      card.classList.remove('drag-over');
      const from = organizeDragIndex, to = idx;
      if (from < 0 || from === to) return;
      const [moved] = organizeModel.splice(from, 1);
      organizeModel.splice(to, 0, moved);
      renderOrganizeGrid();
    });
    grid.appendChild(card);
  });
  const info = document.getElementById('organize-info');
  if (info) {
    const anyRot = organizeModel.some((e) => e.rot % 360 !== 0);
    info.textContent = `${organizeModel.length} page(s)` +
      (anyRot ? ' — rotated pages are flattened on save' : ' — vector preserved');
  }
  const saveBtn = document.getElementById('organize-save');
  if (saveBtn) saveBtn.disabled = organizeModel.length === 0;
}

async function exportOrganizedViaWork(model) {
  const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
  Module.HEAPU8.set(currentPdfBytes, ptr);
  const docId = Module._nanopdf_doc_load(ptr, currentPdfBytes.length);
  Module._nanopdf_free(ptr);
  if (docId < 0) throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Failed to load doc');
  try {
    Module._nanopdf_work_clear();
    for (const e of model) {
      const wi = Module._nanopdf_work_add_page(docId, e.src);
      if (wi < 0) throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Failed to add page');
      if (e.rot % 360 !== 0) Module._nanopdf_work_rotate_page(wi, ((e.rot % 360) + 360) % 360);
    }
    if (Module._nanopdf_export_pdf() !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Export failed');
    }
    return copyWasmBuffer(Module._nanopdf_export_get_buffer, Module._nanopdf_export_get_size);
  } finally {
    Module._nanopdf_doc_close(docId);
    Module._nanopdf_work_clear();
  }
}

async function saveOrganized() {
  if (!organizeModel.length) { setStatus('No pages to save', true); return; }
  const order = organizeModel.map((e) => e.src);
  const anyRot = organizeModel.some((e) => e.rot % 360 !== 0);
  showLoading('Saving reorganized PDF...');
  await new Promise((r) => setTimeout(r, 30));
  try {
    let out;
    if (!anyRot && Module._nanopdf_split_pages) {
      // Pure reorder/delete: vector-preserving via split.
      const jp = Module.stringToNewUTF8('[' + order.join(',') + ']');
      const ok = Module._nanopdf_split_pages(jp);
      Module._free(jp);
      if (ok !== 1) throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Reorder failed');
      out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    } else {
      // Rotation present: route through the work/export path (flattened).
      out = await exportOrganizedViaWork(organizeModel);
    }
    if (!out) throw new Error('Empty output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_organized.pdf');
    setStatus(`Saved reorganized PDF (${organizeModel.length} page(s))`);
    closeOrganize();
  } catch (err) {
    setStatus('Organize error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
  }
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
  const docLoaded = totalPages > 0 && hasRendering;
  const hasSelection = selectedPages.size > 0 && hasRendering;

  // Protect + password fields are usable as soon as a document is loaded,
  // independent of page selection (the Export button still needs a selection).
  protectExport.disabled = !docLoaded;
  const passwordEnabled = docLoaded && protectExport.checked;
  exportPassword.disabled = !passwordEnabled;
  exportOwnerPassword.disabled = !passwordEnabled;

  exportBtn.disabled = !hasSelection;
  exportBtn.classList.toggle('has-selection', hasSelection);
  exportBtn.textContent = hasSelection
    ? `Export (${selectedPages.size})`
    : 'Export';

  // Combine works on the whole document; Extract needs a page selection.
  if (combineBtn) combineBtn.disabled = !(totalPages > 0 && Module && Module._nanopdf_merge_start);
  if (extractPagesBtn) {
    extractPagesBtn.disabled = !(hasSelection && Module && Module._nanopdf_split_pages);
    extractPagesBtn.textContent = selectedPages.size > 0 ? `Extract (${selectedPages.size})` : 'Extract';
  }
  if (organizeBtn) organizeBtn.disabled = !docLoaded;
  if (signBtn) signBtn.disabled = !(docLoaded && Module && Module._nanopdf_sign_pdf);

  // Guide the user when Protect is on but nothing is selected to export.
  if (protectExport.checked && !hasSelection && docLoaded) {
    exportBtn.title = 'Select pages in the Thumbnails sidebar to export';
  } else {
    exportBtn.title = 'Export selected pages as PDF';
  }
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
  } else if (activeSidebarTab === 'files') {
    renderAttachmentsTab();
  } else {
    renderInfoTab();
  }
}

// ---- Attachments (embedded files) tab ----

function renderAttachmentsTab() {
  if (!Module || totalPages <= 0) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No document loaded.</div>';
    return;
  }
  if (!Module._nanopdf_attachments_list) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">Attachment support needs a rebuilt WASM module.</div>';
    return;
  }
  let data = { attachments: [], count: 0 };
  try { data = JSON.parse(Module.UTF8ToString(Module._nanopdf_attachments_list())); } catch (e) {}
  if (!data.count) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No embedded files.</div>';
    return;
  }
  let html = '<div class="attach-list">';
  for (const a of data.attachments) {
    const kb = a.size ? ` · ${(a.size / 1024).toFixed(1)} KB` : '';
    const meta = [a.mimeType, a.modDate || a.creationDate].filter(Boolean).join(' · ');
    html += `<div class="attach-item">
      <div class="attach-name" title="${escapeHtml(a.name)}">${escapeHtml(a.name || '(unnamed)')}</div>
      <div class="attach-meta">${escapeHtml(meta)}${kb}</div>
      ${a.description ? `<div class="attach-desc">${escapeHtml(a.description)}</div>` : ''}
      <button class="attach-dl" data-index="${a.index}" data-name="${escapeHtml(a.name || 'attachment')}">Download</button>
    </div>`;
  }
  html += '</div>';
  sidebarContent.innerHTML = html;
  sidebarContent.querySelectorAll('.attach-dl').forEach((btn) => {
    btn.addEventListener('click', () => downloadAttachment(parseInt(btn.dataset.index, 10), btn.dataset.name));
  });
}

function downloadAttachment(index, name) {
  if (!Module._nanopdf_attachment_extract) return;
  if (Module._nanopdf_attachment_extract(index) !== 1) {
    setStatus('Failed to extract attachment: ' +
      Module.UTF8ToString(Module._nanopdf_get_last_error()), true);
    return;
  }
  const bytes = copyWasmBuffer(Module._nanopdf_attachment_get_buffer, Module._nanopdf_attachment_get_size);
  if (!bytes) { setStatus('Attachment is empty', true); return; }
  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = name || 'attachment';
  document.body.appendChild(a); a.click(); document.body.removeChild(a);
  URL.revokeObjectURL(url);
  setStatus(`Downloaded "${name}" (${(bytes.length / 1024).toFixed(1)} KB)`);
}

// ---- Page Navigation ----

function goToPage(pageIndex) {
  if (pageIndex < 0 || pageIndex >= totalPages) return;
  currentPage = pageIndex;
  selectedAnnotId = null;
  annotDraft = null;
  hideMarkupPopover();
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
    canvasWrapper.style.width = pageDisplayHeight + 'px';
    canvasWrapper.style.height = pageDisplayWidth + 'px';
  } else {
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';
  }
  renderTextOverlay();
  renderAnnotations();
  renderFormFields();
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
  if (!Module || totalPages <= 0 || pageDisplayWidth === 0) return 1;
  const pageWidth = Module._nanopdf_get_page_width(currentPage);
  return pageDisplayWidth / pageWidth;
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
  const x = Math.max(0, Math.min(pageDisplayWidth, clientX - rect.left));
  const y = Math.max(0, Math.min(pageDisplayHeight, clientY - rect.top));
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
  textOverlay.style.width = `${pageDisplayWidth}px`;
  textOverlay.style.height = `${pageDisplayHeight}px`;
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

// ===================== Annotation overlay =====================

function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
  if (!m) return { r: 1, g: 0.835, b: 0.29 };
  return {
    r: parseInt(m[1], 16) / 255,
    g: parseInt(m[2], 16) / 255,
    b: parseInt(m[3], 16) / 255,
  };
}
function rgbToHex(c) {
  const h = (v) => Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16).padStart(2, '0');
  return `#${h(c.r)}${h(c.g)}${h(c.b)}`;
}

function currentPageHeightPts() {
  if (!Module || totalPages <= 0) return 0;
  return Module._nanopdf_get_page_height(currentPage);
}

// Screen CSS px (top-left, within overlay) -> PDF points (bottom-left origin).
function screenToPdf(sx, sy) {
  const scale = getPageScale();
  return { x: sx / scale, y: currentPageHeightPts() - sy / scale };
}
// PDF points -> screen CSS px within the overlay.
function pdfToScreen(px, py) {
  const scale = getPageScale();
  return { x: px * scale, y: (currentPageHeightPts() - py) * scale };
}
// PDF box {x,y(lower-left),w,h} -> screen {left,top,width,height}.
function pdfBoxToScreen(b) {
  const scale = getPageScale();
  const tl = pdfToScreen(b.x, b.y + b.h);
  return { left: tl.x, top: tl.y, width: b.w * scale, height: b.h * scale };
}
function annotPointerPos(e) {
  const rect = annotLayer.getBoundingClientRect();
  return {
    x: Math.max(0, Math.min(pageDisplayWidth, e.clientX - rect.left)),
    y: Math.max(0, Math.min(pageDisplayHeight, e.clientY - rect.top)),
  };
}

function pageAnnots(page = currentPage) {
  if (!annotations[page]) annotations[page] = [];
  return annotations[page];
}
function findAnnot(id, page = currentPage) {
  return pageAnnots(page).find((a) => a.id === id);
}
function annotEditingEnabled() {
  // Drawing math assumes an un-rotated page; allow editing only at 0deg.
  return hasRendering && totalPages > 0 && rotation === 0 &&
         canvas.style.display === 'block';
}

function setAnnotTool(tool) {
  annotTool = tool;
  if (annotTools) {
    annotTools.querySelectorAll('.annot-tool').forEach((b) => {
      b.classList.toggle('active', b.dataset.tool === tool);
    });
  }
  updateAnnotLayerMode();
}

function updateAnnotLayerMode() {
  const editing = annotEditingEnabled();
  const drawing = editing && annotTool !== 'select';
  annotLayer.classList.toggle('drawing', drawing);
  annotLayer.classList.toggle('selecting', editing && annotTool === 'select');
  if (annotDeleteBtn) annotDeleteBtn.disabled = !(editing && selectedAnnotId != null);
}

function setAnnotControlsEnabled(enabled) {
  if (!annotTools) return;
  annotTools.querySelectorAll('button, input').forEach((el) => {
    el.disabled = !enabled;
  });
  if (enabled) {
    annotDeleteBtn.disabled = selectedAnnotId == null;
  }
}

function translateAnnot(a, dx, dy) {
  if (a.type === 'line' || a.type === 'arrow') {
    a.x1 += dx; a.y1 += dy; a.x2 += dx; a.y2 += dy;
  } else if (a.type === 'ink') {
    a.points = a.points.map((p) => ({ x: p.x + dx, y: p.y + dy }));
  } else {
    a.x += dx; a.y += dy;
  }
}

function annotScreenBBox(a) {
  if (a.type === 'line' || a.type === 'arrow') {
    const p1 = pdfToScreen(a.x1, a.y1), p2 = pdfToScreen(a.x2, a.y2);
    return { left: Math.min(p1.x, p2.x), top: Math.min(p1.y, p2.y),
             width: Math.abs(p2.x - p1.x), height: Math.abs(p2.y - p1.y) };
  }
  if (a.type === 'ink') {
    let minx = Infinity, miny = Infinity, maxx = -Infinity, maxy = -Infinity;
    for (const p of a.points) {
      const s = pdfToScreen(p.x, p.y);
      minx = Math.min(minx, s.x); miny = Math.min(miny, s.y);
      maxx = Math.max(maxx, s.x); maxy = Math.max(maxy, s.y);
    }
    return { left: minx, top: miny, width: maxx - minx, height: maxy - miny };
  }
  return pdfBoxToScreen(a);
}

function svgEl(name) { return document.createElementNS(SVG_NS, name); }

function arrowHeadSegments(p1, p2) {
  const ang = Math.atan2(p2.y - p1.y, p2.x - p1.x);
  const len = Math.min(16, Math.hypot(p2.x - p1.x, p2.y - p1.y) * 0.4);
  const a1 = ang + Math.PI - 0.5;
  const a2 = ang + Math.PI + 0.5;
  return [
    { x1: p2.x, y1: p2.y, x2: p2.x + len * Math.cos(a1), y2: p2.y + len * Math.sin(a1) },
    { x1: p2.x, y1: p2.y, x2: p2.x + len * Math.cos(a2), y2: p2.y + len * Math.sin(a2) },
  ];
}

function clearChildren(el) { while (el.firstChild) el.removeChild(el.firstChild); }

function renderAnnotations() {
  if (!annotSvg) return;
  annotLayer.style.width = pageDisplayWidth + 'px';
  annotLayer.style.height = pageDisplayHeight + 'px';
  annotLayer.style.transform = canvas.style.transform || '';
  annotSvg.setAttribute('viewBox', `0 0 ${pageDisplayWidth} ${pageDisplayHeight}`);
  annotSvg.setAttribute('width', pageDisplayWidth);
  annotSvg.setAttribute('height', pageDisplayHeight);

  clearChildren(annotSvg);
  annotHtml.querySelectorAll('.annot-textbox').forEach((n) => n.remove());

  if (!Module || totalPages <= 0 || canvas.style.display !== 'block') return;

  const list = pageAnnots();
  const all = annotDraft ? list.concat([annotDraft]) : list;
  for (const a of all) drawAnnot(a);

  // Selection outline.
  const sel = selectedAnnotId != null ? findAnnot(selectedAnnotId) : null;
  if (sel) {
    const bb = annotScreenBBox(sel);
    const r = svgEl('rect');
    r.setAttribute('x', bb.left - 3); r.setAttribute('y', bb.top - 3);
    r.setAttribute('width', Math.max(0, bb.width) + 6);
    r.setAttribute('height', Math.max(0, bb.height) + 6);
    r.setAttribute('fill', 'none');
    r.setAttribute('stroke', '#007bff');
    r.setAttribute('stroke-width', '1');
    r.setAttribute('stroke-dasharray', '4 3');
    annotSvg.appendChild(r);
  }
  updateAnnotLayerMode();
}

function drawAnnot(a) {
  const scale = getPageScale();
  const col = rgbToHex(a.color);
  if (a.type === 'text') { drawTextBox(a); return; }
  if (a.type === 'image') { drawImageStamp(a); return; }

  let el;
  if (a.type === 'redact') {
    el = svgEl('rect');
    const box = pdfBoxToScreen(a);
    el.setAttribute('x', box.left); el.setAttribute('y', box.top);
    el.setAttribute('width', Math.max(0, box.width));
    el.setAttribute('height', Math.max(0, box.height));
    el.setAttribute('fill', '#000');
    el.setAttribute('stroke', '#e53935');     // red marker so users see redaction marks
    el.setAttribute('stroke-width', '1.5');
    el.setAttribute('stroke-dasharray', '4 2');
  } else if (a.type === 'highlight') {
    el = svgEl('rect');
    const box = pdfBoxToScreen(a);
    el.setAttribute('x', box.left); el.setAttribute('y', box.top);
    el.setAttribute('width', Math.max(0, box.width));
    el.setAttribute('height', Math.max(0, box.height));
    el.setAttribute('fill', col);
    el.setAttribute('fill-opacity', a.alpha != null ? a.alpha : 0.4);
  } else if (a.type === 'rect') {
    el = svgEl('rect');
    const box = pdfBoxToScreen(a);
    el.setAttribute('x', box.left); el.setAttribute('y', box.top);
    el.setAttribute('width', Math.max(0, box.width));
    el.setAttribute('height', Math.max(0, box.height));
    applyShapeStyle(el, a, col, scale);
  } else if (a.type === 'oval') {
    el = svgEl('ellipse');
    const box = pdfBoxToScreen(a);
    el.setAttribute('cx', box.left + box.width / 2);
    el.setAttribute('cy', box.top + box.height / 2);
    el.setAttribute('rx', Math.max(0, box.width / 2));
    el.setAttribute('ry', Math.max(0, box.height / 2));
    applyShapeStyle(el, a, col, scale);
  } else if (a.type === 'line' || a.type === 'arrow') {
    el = svgEl('g');
    const p1 = pdfToScreen(a.x1, a.y1), p2 = pdfToScreen(a.x2, a.y2);
    const segs = [{ x1: p1.x, y1: p1.y, x2: p2.x, y2: p2.y }];
    if (a.type === 'arrow') segs.push(...arrowHeadSegments(p1, p2));
    for (const s of segs) {
      const ln = svgEl('line');
      ln.setAttribute('x1', s.x1); ln.setAttribute('y1', s.y1);
      ln.setAttribute('x2', s.x2); ln.setAttribute('y2', s.y2);
      ln.setAttribute('stroke', col);
      ln.setAttribute('stroke-width', Math.max(1, a.lineWidth * scale));
      ln.setAttribute('stroke-linecap', 'round');
      el.appendChild(ln);
    }
  } else if (a.type === 'ink') {
    el = svgEl('polyline');
    el.setAttribute('points', a.points.map((p) => {
      const s = pdfToScreen(p.x, p.y); return `${s.x},${s.y}`;
    }).join(' '));
    el.setAttribute('fill', 'none');
    el.setAttribute('stroke', col);
    el.setAttribute('stroke-width', Math.max(1, a.lineWidth * scale));
    el.setAttribute('stroke-linejoin', 'round');
    el.setAttribute('stroke-linecap', 'round');
  }
  if (!el) return;
  el.classList.add('annot-shape');
  el.dataset.annotId = a.id;
  annotSvg.appendChild(el);
}

function applyShapeStyle(el, a, col, scale) {
  if (a.filled) {
    el.setAttribute('fill', col);
    el.setAttribute('fill-opacity', a.alpha != null ? a.alpha : 1);
    el.setAttribute('stroke', 'none');
  } else {
    el.setAttribute('fill', 'none');
    el.setAttribute('stroke', col);
    el.setAttribute('stroke-width', Math.max(1, a.lineWidth * scale));
  }
}

function drawTextBox(a) {
  const box = pdfBoxToScreen(a);
  const scale = getPageScale();
  const ta = document.createElement('textarea');
  ta.className = 'annot-textbox' + (a.id === selectedAnnotId ? ' selected' : '');
  ta.dataset.annotId = a.id;
  ta.style.left = box.left + 'px';
  ta.style.top = box.top + 'px';
  ta.style.width = Math.max(40, box.width) + 'px';
  ta.style.fontSize = (a.fontSize * scale) + 'px';
  ta.style.color = rgbToHex(a.color);
  ta.placeholder = 'Text…';
  ta.value = a.text || '';
  ta.addEventListener('input', () => { a.text = ta.value; autoGrowTextbox(ta, a); });
  ta.addEventListener('focus', () => {
    selectedAnnotId = a.id; updateAnnotLayerMode();
  });
  ta.addEventListener('pointerdown', (e) => e.stopPropagation());
  annotHtml.appendChild(ta);
  autoGrowTextbox(ta, a);
}
function autoGrowTextbox(ta, a) {
  ta.style.height = 'auto';
  ta.style.height = ta.scrollHeight + 'px';
  a.h = ta.offsetHeight / getPageScale();
  a.w = ta.offsetWidth / getPageScale();
}

// ---- Drawing / selecting interaction ----

function startDraft(e) {
  const pos = annotPointerPos(e);
  const pdf = screenToPdf(pos.x, pos.y);
  const color = hexToRgb(annotColorHex);
  const isRedact = annotTool === 'redact';
  annotDraft = {
    id: annotIdCounter++,
    type: annotTool,
    color: isRedact ? { r: 0, g: 0, b: 0 } : color,
    alpha: annotTool === 'highlight' ? 0.4 : 1,
    lineWidth: 2,
    filled: isRedact || (annotFillShapes && (annotTool === 'rect' || annotTool === 'oval')),
    _start: pdf,
  };
  if (annotTool === 'ink') annotDraft.points = [pdf];
  else if (annotTool === 'line' || annotTool === 'arrow') {
    annotDraft.x1 = pdf.x; annotDraft.y1 = pdf.y; annotDraft.x2 = pdf.x; annotDraft.y2 = pdf.y;
  } else { annotDraft.x = pdf.x; annotDraft.y = pdf.y; annotDraft.w = 0; annotDraft.h = 0; }
  if (annotTool === 'image' && pendingStampImage) {
    annotDraft.imageId = pendingStampImage.imageId;
    annotDraft.dataURL = pendingStampImage.dataURL;
    annotDraft._aspect = pendingStampImage.aspect || 1;
  }
  renderAnnotations();
}

function startTextBox(e) {
  const pos = annotPointerPos(e);
  const pdf = screenToPdf(pos.x, pos.y);
  const scale = getPageScale();
  const h = 20 / scale, w = 140 / scale;
  const a = {
    id: annotIdCounter++, type: 'text',
    x: pdf.x, y: pdf.y - h, w, h,
    text: '', fontSize: 14, color: hexToRgb(annotColorHex),
  };
  pageAnnots().push(a);
  selectedAnnotId = a.id;
  setAnnotTool('select');
  renderAnnotations();
  const ta = annotHtml.querySelector(`.annot-textbox[data-annot-id="${a.id}"]`);
  if (ta) ta.focus();
  updateSaveAnnotState();
}

// ---- Image / signature stamp ----

function pickStampImage() {
  if (stampInput) stampInput.click();
}

async function loadStampImage(file) {
  if (!Module || !Module._nanopdf_register_stamp_image) {
    setStatus('Image stamps need a rebuilt WASM module', true);
    return;
  }
  try {
    const buf = new Uint8Array(await file.arrayBuffer());
    const dataURL = await new Promise((resolve, reject) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result);
      r.onerror = reject;
      r.readAsDataURL(file);
    });
    // Natural size (for aspect ratio).
    const aspect = await new Promise((resolve) => {
      const img = new Image();
      img.onload = () => resolve(img.naturalWidth / Math.max(1, img.naturalHeight));
      img.onerror = () => resolve(1);
      img.src = dataURL;
    });
    // Register the encoded bytes with the WASM module.
    const ptr = Module._nanopdf_malloc(buf.length);
    Module.HEAPU8.set(buf, ptr);
    const id = Module._nanopdf_register_stamp_image(ptr, buf.length);
    Module._nanopdf_free(ptr);
    if (id < 0) {
      setStatus('Could not load image: ' +
        Module.UTF8ToString(Module._nanopdf_get_last_error()), true);
      return;
    }
    pendingStampImage = { imageId: id, dataURL, aspect };
    setAnnotTool('image');
    setStatus('Click or drag on the page to place the image');
  } catch (err) {
    setStatus('Image load error: ' + err.message, true);
  }
}

function drawImageStamp(a) {
  const box = pdfBoxToScreen(a);
  const el = svgEl('image');
  el.setAttributeNS('http://www.w3.org/1999/xlink', 'href', a.dataURL);
  el.setAttribute('href', a.dataURL);
  el.setAttribute('x', box.left);
  el.setAttribute('y', box.top);
  el.setAttribute('width', Math.max(1, box.width));
  el.setAttribute('height', Math.max(1, box.height));
  el.setAttribute('preserveAspectRatio', 'none');
  el.classList.add('annot-shape');
  el.dataset.annotId = a.id;
  annotSvg.appendChild(el);
}

function annotLayerPointerDown(e) {
  if (e.button !== 0) return;
  if (!annotEditingEnabled() || annotTool === 'select') return;
  e.preventDefault();
  if (annotTool === 'text') { startTextBox(e); return; }
  if (annotTool === 'image' && !pendingStampImage) { pickStampImage(); return; }
  annotLayer.setPointerCapture?.(e.pointerId);
  startDraft(e);
}

function annotLayerPointerMove(e) {
  if (!annotDraft) return;
  const pos = annotPointerPos(e);
  const pdf = screenToPdf(pos.x, pos.y);
  const s = annotDraft._start;
  if (annotDraft.type === 'ink') {
    annotDraft.points.push(pdf);
  } else if (annotDraft.type === 'line' || annotDraft.type === 'arrow') {
    annotDraft.x2 = pdf.x; annotDraft.y2 = pdf.y;
  } else {
    annotDraft.x = Math.min(s.x, pdf.x);
    annotDraft.y = Math.min(s.y, pdf.y);
    annotDraft.w = Math.abs(pdf.x - s.x);
    annotDraft.h = Math.abs(pdf.y - s.y);
  }
  renderAnnotations();
}

function annotLayerPointerUp(e) {
  if (!annotDraft) return;
  const d = annotDraft;
  annotDraft = null;
  let keep;
  if (d.type === 'ink') keep = d.points.length > 1;
  else if (d.type === 'line' || d.type === 'arrow') keep = Math.hypot(d.x2 - d.x1, d.y2 - d.y1) > 2;
  else if (d.type === 'image') {
    // A click (no real drag) places the image at a default size keeping aspect.
    if (d.w <= 2 || d.h <= 2) {
      const w = 160 / getPageScale();
      d.w = w; d.h = w / (d._aspect || 1);
      d.y = d._start.y - d.h;  // grow downward from the click point
      d.x = d._start.x;
    }
    keep = true;
    pendingStampImage = null;   // consume the pending image
    setAnnotTool('select');
  } else keep = d.w > 2 && d.h > 2;
  if (keep) {
    delete d._start;
    delete d._aspect;
    pageAnnots().push(d);
    selectedAnnotId = d.id;
  }
  renderAnnotations();
  updateSaveAnnotState();
}

function annotShapePointerDown(e) {
  if (!annotEditingEnabled() || annotTool !== 'select') return;
  const host = e.target.closest('[data-annot-id]');
  if (!host) return;
  e.stopPropagation();
  e.preventDefault();
  selectedAnnotId = Number(host.dataset.annotId);
  const a = findAnnot(selectedAnnotId);
  renderAnnotations();
  if (!a || a.type === 'text') return;  // text boxes move via their own element
  let last = annotPointerPos(e);
  const onMove = (ev) => {
    const pos = annotPointerPos(ev);
    const dx = (pos.x - last.x) / getPageScale();
    const dy = -(pos.y - last.y) / getPageScale();
    translateAnnot(a, dx, dy);
    last = pos;
    renderAnnotations();
  };
  const onUp = () => {
    window.removeEventListener('pointermove', onMove);
    window.removeEventListener('pointerup', onUp);
    updateSaveAnnotState();
  };
  window.addEventListener('pointermove', onMove);
  window.addEventListener('pointerup', onUp);
}

function deleteSelectedAnnot() {
  if (selectedAnnotId == null) return;
  const list = pageAnnots();
  const i = list.findIndex((a) => a.id === selectedAnnotId);
  if (i >= 0) {
    list.splice(i, 1);
    selectedAnnotId = null;
    renderAnnotations();
    updateAnnotLayerMode();
    updateSaveAnnotState();
  }
}

// ---- Form-field visual fill ----

function isCheckboxOn(value) {
  if (!value) return false;
  const v = String(value).replace(/^\//, '').toLowerCase();
  return v !== '' && v !== 'off';
}

function loadFormFields() {
  formFieldsByPage = {};
  if (!Module || !Module._nanopdf_get_form_fields) return;
  if (Module._nanopdf_has_form_fields && Module._nanopdf_has_form_fields() !== 1) return;
  let json;
  try {
    json = JSON.parse(Module.UTF8ToString(Module._nanopdf_get_form_fields()));
  } catch (e) { return; }
  if (!json || !json.fields) return;
  for (const f of json.fields) {
    if (f.readOnly) continue;
    // Map the field type to an editable control kind.
    let kind = null;
    if (f.type === 'text') kind = 'text';
    else if (f.type === 'button' && f.buttonType === 'checkbox') kind = 'checkbox';
    else if (f.type === 'choice') kind = 'choice';
    if (!kind) continue;  // radios/pushbuttons/signatures not yet editable
    for (const w of (f.widgets || [])) {
      if (w.page == null || w.page < 0 || !w.rect) continue;
      if (!formFieldsByPage[w.page]) formFieldsByPage[w.page] = [];
      formFieldsByPage[w.page].push({
        name: f.fullName || f.name || '',
        kind,
        rect: w.rect,
        value: f.value || '',
        checked: kind === 'checkbox' ? isCheckboxOn(f.value) : false,
        multiline: !!f.multiline,
        password: !!f.password,
        options: Array.isArray(f.options) ? f.options : [],
        fontSize: 12,
      });
    }
  }
}

function renderFormFields() {
  annotHtml.querySelectorAll('.annot-formfield').forEach((n) => n.remove());
  if (!Module || totalPages <= 0 || canvas.style.display !== 'block') return;
  if (rotation !== 0) return;
  const fields = formFieldsByPage[currentPage];
  if (!fields) return;
  for (const fld of fields) {
    const box = pdfBoxToScreen({ x: fld.rect.x, y: fld.rect.y, w: fld.rect.width, h: fld.rect.height });
    let input;
    if (fld.kind === 'checkbox') {
      input = document.createElement('input');
      input.type = 'checkbox';
      input.checked = fld.checked;
      input.addEventListener('change', () => { fld.checked = input.checked; updateSaveAnnotState(); });
    } else if (fld.kind === 'choice') {
      input = document.createElement('select');
      for (const opt of fld.options) {
        const o = document.createElement('option');
        o.value = opt; o.textContent = opt;
        if (opt === fld.value) o.selected = true;
        input.appendChild(o);
      }
      input.addEventListener('change', () => { fld.value = input.value; updateSaveAnnotState(); });
    } else {
      input = document.createElement(fld.multiline ? 'textarea' : 'input');
      if (!fld.multiline) input.type = fld.password ? 'password' : 'text';
      input.value = fld.value;
      input.addEventListener('input', () => { fld.value = input.value; updateSaveAnnotState(); });
    }
    input.className = 'annot-formfield';
    input.style.left = box.left + 'px';
    input.style.top = box.top + 'px';
    input.style.width = Math.max(8, box.width) + 'px';
    input.style.height = Math.max(12, box.height) + 'px';
    if (fld.kind === 'text' || fld.kind === 'choice') {
      input.style.fontSize = Math.max(8, Math.min(box.height * 0.7, 18)) + 'px';
    }
    input.title = fld.name;
    annotHtml.appendChild(input);
  }
}

// Save filled form fields as a real editable PDF via incremental update.
async function saveEditableForm() {
  if (!Module || !Module._nanopdf_form_load || !currentPdfBytes || !currentPdfBytes.length) {
    setStatus('Editable form save not available', true);
    return;
  }
  showLoading('Saving filled form...');
  await new Promise((r) => setTimeout(r, 30));
  try {
    const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, ptr);
    const ok = Module._nanopdf_form_load(ptr, currentPdfBytes.length);
    Module._nanopdf_free(ptr);
    if (ok !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Failed to load form');
    }
    // A field can have widgets on multiple pages; set each unique field once.
    const done = new Set();
    let count = 0;
    for (const page in formFieldsByPage) {
      for (const f of formFieldsByPage[page]) {
        const key = f.kind + ':' + f.name;
        if (done.has(key)) continue;
        done.add(key);
        const namePtr = Module.stringToNewUTF8(f.name);
        if (f.kind === 'text') {
          const vp = Module.stringToNewUTF8(f.value || '');
          if (Module._nanopdf_form_set_text(namePtr, vp)) count++;
          Module._free(vp);
        } else if (f.kind === 'checkbox') {
          if (Module._nanopdf_form_set_checkbox(namePtr, f.checked ? 1 : 0)) count++;
        } else if (f.kind === 'choice') {
          const vp = Module.stringToNewUTF8(f.value || '');
          if (Module._nanopdf_form_set_choice(namePtr, vp)) count++;
          Module._free(vp);
        }
        Module._free(namePtr);
      }
    }
    if (Module._nanopdf_form_save() !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Form save failed');
    }
    const out = copyWasmBuffer(Module._nanopdf_form_get_buffer, Module._nanopdf_form_get_size);
    if (!out) throw new Error('Empty form output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_filled.pdf');
    setStatus(`Saved editable filled form (${count} field(s))`);
  } catch (err) {
    setStatus('Form save error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
  }
}

function hasFillableFields() {
  for (const k in formFieldsByPage) {
    if (formFieldsByPage[k] && formFieldsByPage[k].length) return true;
  }
  return false;
}

function hasAnyAnnotations() {
  for (const k in annotations) if (annotations[k] && annotations[k].length) return true;
  for (const k in formFieldsByPage) {
    for (const f of formFieldsByPage[k]) if (formFieldBaked(f)) return true;
  }
  return false;
}
function updateSaveAnnotState() {
  const docLoaded = hasRendering && totalPages > 0;
  if (saveAnnotBtn) saveAnnotBtn.disabled = !docLoaded;
  if (saveFormBtn) saveFormBtn.disabled = !(docLoaded && hasFillableFields() && Module && Module._nanopdf_form_load);
  if (saveEditableBtn) {
    const ed = docLoaded && Module && Module._nanopdf_edit_load && editableMarkupCount().supported > 0;
    saveEditableBtn.disabled = !ed;
  }
}

function resetAnnotations() {
  annotations = {};
  formFieldsByPage = {};
  selectedAnnotId = null;
  annotDraft = null;
  pendingStampImage = null;
  annotIdCounter = 1;
  setAnnotTool('select');
  if (annotHtml) clearChildren(annotHtml);
  if (annotSvg) clearChildren(annotSvg);
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
  // CSS-pixel layout size of the page.
  const cssWidth = Math.floor(pageWidth * effectiveScale);
  const cssHeight = Math.floor(pageHeight * effectiveScale);

  // Render at device resolution so text stays crisp on HiDPI/Retina displays.
  // The canvas bitmap is cssSize * devicePixelRatio; CSS keeps it at the layout size.
  const dpr = window.devicePixelRatio || 1;
  // Cap maximum render dimensions (device px) to avoid WASM memory issues.
  const maxDim = 4096;
  const width = Math.min(maxDim, Math.round(cssWidth * dpr));
  const height = Math.min(maxDim, Math.round(cssHeight * dpr));

  canvas.width = width;
  canvas.height = height;
  canvas.style.width = `${cssWidth}px`;
  canvas.style.height = `${cssHeight}px`;
  pageDisplayWidth = cssWidth;
  pageDisplayHeight = cssHeight;

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
  renderAnnotations();
  renderFormFields();

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
  hideMarkupPopover();
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
    showMarkupPopover();
  } else {
    hideMarkupPopover();
  }
  e.preventDefault();
}

// ---- Text markup (Highlight/Underline/Strike/Squiggly on a text selection) ----

function selectionQuads() {
  if (!currentSelection || !Array.isArray(currentSelection.segments)) return [];
  return currentSelection.segments
    .map((s) => s.quad)
    .filter((q) => q && q.width > 0 && q.height > 0);
}

function showMarkupPopover() {
  if (!markupPopover) return;
  const quads = selectionQuads();
  if (!quads.length || rotation !== 0) { hideMarkupPopover(); return; }
  // Anchor above the top-center of the selection's bounding box (CSS px).
  let minScreenY = Infinity, cx = 0, n = 0;
  for (const q of quads) {
    const s = pdfRectToScreen(q);
    minScreenY = Math.min(minScreenY, s.y);
    cx += s.x + s.width / 2; n++;
  }
  markupPopover.style.left = (cx / n) + 'px';
  markupPopover.style.top = Math.max(8, minScreenY - 6) + 'px';
  markupPopover.classList.remove('hidden');
}

function hideMarkupPopover() {
  if (markupPopover) markupPopover.classList.add('hidden');
}

function markupSelection(kind) {
  const quads = selectionQuads();
  if (!quads.length) return;
  const page = currentSelection.page != null ? currentSelection.page : currentPage;
  const color = hexToRgb(annotColorHex);
  const list = (annotations[page] = annotations[page] || []);
  // markupKind + quad tag the annotation so "Save Editable" can write it as a
  // real text-markup PDF annotation; the visual shape (line/ink) is unchanged.
  for (const q of quads) {
    const quad = { x: q.x, y: q.y, w: q.width, h: q.height };
    if (kind === 'highlight') {
      list.push({ id: annotIdCounter++, type: 'highlight', markupKind: 'highlight',
        x: q.x, y: q.y, w: q.width, h: q.height, quad, color, alpha: 0.4 });
    } else if (kind === 'underline' || kind === 'strike') {
      const y = kind === 'strike' ? q.y + q.height * 0.5
                                  : q.y + Math.max(0.5, q.height * 0.06);
      list.push({ id: annotIdCounter++, type: 'line', markupKind: kind, quad,
        x1: q.x, y1: y, x2: q.x + q.width, y2: y, color, lineWidth: 1.5, alpha: 1 });
    } else if (kind === 'squiggly') {
      const base = q.y + Math.max(0.5, q.height * 0.06);
      const amp = Math.max(1, q.height * 0.09);
      const step = Math.max(2, q.height * 0.3);
      const pts = [];
      let up = true;
      for (let x = q.x; x <= q.x + q.width; x += step) {
        pts.push({ x, y: base + (up ? amp : 0) });
        up = !up;
      }
      pts.push({ x: q.x + q.width, y: base });
      list.push({ id: annotIdCounter++, type: 'ink', markupKind: 'squiggly', quad,
        points: pts, color, lineWidth: 1.2, alpha: 1 });
    }
  }
  currentSelection = null;
  hideMarkupPopover();
  renderTextOverlay();
  if (page === currentPage) renderAnnotations();
  updateSaveAnnotState();
  setStatus(`Added ${kind} markup`);
}

function copyCurrentSelection(e) {
  if (!currentSelection || !currentSelection.text) return;
  e.clipboardData.setData('text/plain', currentSelection.text);
  e.preventDefault();
  setStatus('Selection copied');
}

// ---- PDF Loading ----

// Derive a reasonable file name from a URL's path (falls back to document.pdf).
function fileNameFromUrl(url) {
  try {
    const path = new URL(url, window.location.href).pathname;
    const last = decodeURIComponent(path.split('/').pop() || '');
    if (last) return last.toLowerCase().endsWith('.pdf') ? last : last + '.pdf';
  } catch (e) { /* ignore */ }
  return 'document.pdf';
}

// Fetch a PDF from a URL and load it. Reports network/CORS/HTTP errors clearly.
async function loadPdfFromUrl(url) {
  if (!Module || !url) return;
  url = url.trim();
  if (!url) return;
  showLoading('Fetching PDF...');
  try {
    const res = await fetch(url, { redirect: 'follow' });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status} ${res.statusText}`);
    }
    const arrayBuffer = await res.arrayBuffer();
    if (!arrayBuffer || arrayBuffer.byteLength === 0) {
      throw new Error('Empty response');
    }
    await loadPDF(arrayBuffer, fileNameFromUrl(url));
  } catch (err) {
    hideLoading();
    const msg = (err && err.message) || String(err);
    // fetch() rejects with a generic "Failed to fetch" on CORS / network errors.
    const hint = /failed to fetch|load failed|networkerror/i.test(msg)
      ? ' (network or CORS error — the server must allow cross-origin requests)'
      : '';
    setStatus('Could not load URL: ' + msg + hint, true);
    console.error('loadPdfFromUrl failed:', err);
  }
}

function promptOpenUrl() {
  if (!Module) return;
  const url = window.prompt('Enter the URL of a PDF to open:', '');
  if (url) loadPdfFromUrl(url);
}

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

    // Annotation tooling
    resetAnnotations();
    loadFormFields();
    setAnnotControlsEnabled(hasRendering);
    updateSaveAnnotState();

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

// Prompt for a CA bundle (PEM) and validate the signer chain against it. With no
// bundle the bridge reports "trust not checked" — integrity is separate.
function checkSignatureTrust(index) {
  if (!Module || !Module._nanopdf_verify_trust) return;
  const target = document.getElementById(`signature-trust-${index}`);
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.pem,.crt,.cer,.ca-bundle,application/x-pem-file,application/x-x509-ca-cert';
  input.addEventListener('change', async () => {
    const file = input.files[0];
    if (!file) return;
    if (target) { target.textContent = 'Checking trust…'; target.className = 'signature-validation pending'; }
    let pemPtr = 0;
    try {
      const pem = new Uint8Array(await file.arrayBuffer());
      pemPtr = Module._nanopdf_malloc(pem.length);
      Module.HEAPU8.set(pem, pemPtr);
      const resStr = Module.UTF8ToString(Module._nanopdf_verify_trust(index, pemPtr, pem.length));
      const r = JSON.parse(resStr);
      if (!target) return;
      if (r.error && !r.trustChecked) {
        target.textContent = 'Trust: ' + r.error;
        target.className = 'signature-validation error';
        return;
      }
      const parts = [];
      if (r.trusted) {
        parts.push('Trust: chain valid');
        if (r.anchorCN) parts.push(`Anchor: ${r.anchorCN}`);
      } else {
        parts.push('Trust: NOT trusted');
        if (r.error) parts.push(r.error);
      }
      if (typeof r.certCount === 'number') parts.push(`${r.certCount} cert(s) in signature`);
      target.textContent = parts.join(' | ');
      target.className = `signature-validation ${r.trusted ? 'success' : 'error'}`;
    } catch (e) {
      if (target) { target.textContent = 'Trust check error: ' + e.message; target.className = 'signature-validation error'; }
    } finally {
      if (pemPtr) Module._nanopdf_free(pemPtr);
    }
  });
  input.click();
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
  const baseName = fileName.replace(/\.pdf$/i, '');
  downloadNamedPdf(pdfBytes, `${baseName}_${suffix}_${pages.map(p => p + 1).join('-')}.pdf`);
}

// Download a PDF byte buffer under an explicit file name.
function downloadNamedPdf(pdfBytes, filename) {
  const blob = new Blob([pdfBytes], { type: 'application/pdf' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// Copy a WASM output buffer (ptr,size getters) into a detached Uint8Array.
function copyWasmBuffer(getPtr, getSize) {
  const ptr = getPtr();
  const size = getSize();
  if (!ptr || size <= 0) return null;
  return new Uint8Array(Module.HEAPU8.buffer, ptr, size).slice();
}

// ---- Annotation baking (PDF points -> nanopdf_annot_* export API) ----

function arrowHeadPdf(x1, y1, x2, y2) {
  const ang = Math.atan2(y2 - y1, x2 - x1);
  const len = Math.min(12, Math.hypot(x2 - x1, y2 - y1) * 0.4);
  const a1 = ang + Math.PI - 0.5, a2 = ang + Math.PI + 0.5;
  return [
    { x1: x2, y1: y2, x2: x2 + len * Math.cos(a1), y2: y2 + len * Math.sin(a1) },
    { x1: x2, y1: y2, x2: x2 + len * Math.cos(a2), y2: y2 + len * Math.sin(a2) },
  ];
}

function bakeTextLines(workIndex, text, x, topY, fontSize, c, alpha) {
  const lines = String(text).split('\n');
  const fontPtr = Module.stringToNewUTF8('Helvetica');
  let baseline = topY - fontSize;
  for (const line of lines) {
    const txtPtr = Module.stringToNewUTF8(line);
    Module._nanopdf_annot_add_text(workIndex, x, baseline, txtPtr, fontPtr,
      fontSize, c.r, c.g, c.b, alpha);
    Module._free(txtPtr);
    baseline -= fontSize * 1.25;
  }
  Module._free(fontPtr);
}

function bakeAnnotationForWorkPage(workIndex, a) {
  const c = a.color || { r: 0, g: 0, b: 0 };
  const lw = a.lineWidth || 2;
  const alpha = a.alpha != null ? a.alpha : 1;
  switch (a.type) {
    case 'highlight':
      Module._nanopdf_annot_add_highlight(workIndex, a.x, a.y, a.w, a.h, c.r, c.g, c.b, alpha);
      break;
    case 'rect':
      Module._nanopdf_annot_add_rect(workIndex, a.x, a.y, a.w, a.h, lw, c.r, c.g, c.b, alpha, a.filled ? 1 : 0);
      break;
    case 'oval':
      Module._nanopdf_annot_add_oval(workIndex, a.x + a.w / 2, a.y + a.h / 2, a.w / 2, a.h / 2, lw, c.r, c.g, c.b, alpha, a.filled ? 1 : 0);
      break;
    case 'line':
      Module._nanopdf_annot_add_line(workIndex, a.x1, a.y1, a.x2, a.y2, lw, c.r, c.g, c.b, alpha);
      break;
    case 'arrow':
      Module._nanopdf_annot_add_line(workIndex, a.x1, a.y1, a.x2, a.y2, lw, c.r, c.g, c.b, alpha);
      for (const s of arrowHeadPdf(a.x1, a.y1, a.x2, a.y2)) {
        Module._nanopdf_annot_add_line(workIndex, s.x1, s.y1, s.x2, s.y2, lw, c.r, c.g, c.b, alpha);
      }
      break;
    case 'ink':
      for (let i = 1; i < a.points.length; i++) {
        const p = a.points[i - 1], q = a.points[i];
        Module._nanopdf_annot_add_line(workIndex, p.x, p.y, q.x, q.y, lw, c.r, c.g, c.b, alpha);
      }
      break;
    case 'text':
      if (a.text && a.text.trim()) {
        bakeTextLines(workIndex, a.text, a.x, a.y + a.h, a.fontSize || 14, c, alpha);
      }
      break;
    case 'redact':
      // Opaque black box. Because export flattens the page to a raster image
      // first, the text underneath is gone (not just visually covered).
      Module._nanopdf_annot_add_rect(workIndex, a.x, a.y, a.w, a.h, 0, 0, 0, 0, 1, 1);
      break;
    case 'image':
      if (a.imageId != null && Module._nanopdf_annot_add_image) {
        Module._nanopdf_annot_add_image(workIndex, a.x, a.y, a.w, a.h, a.imageId);
      }
      break;
  }
}

function formFieldBaked(f) {
  if (f.kind === 'checkbox') return f.checked ? 'X' : '';
  return (f.value || '').trim();
}

function bakeFormFieldsForWorkPage(workIndex, pageIndex) {
  const fields = formFieldsByPage[pageIndex];
  if (!fields) return;
  const black = { r: 0, g: 0, b: 0 };
  for (const f of fields) {
    const text = formFieldBaked(f);
    if (!text) continue;
    const fs = Math.max(8, Math.min(f.rect.height * 0.7, 12));
    // Start the text near the top of the field box so multi-line content flows down.
    bakeTextLines(workIndex, text, f.rect.x + 2, f.rect.y + f.rect.height - 2, fs, black, 1);
  }
}

function pageHasBakeableContent(pageIndex) {
  if (annotations[pageIndex] && annotations[pageIndex].length) return true;
  const fields = formFieldsByPage[pageIndex];
  return !!(fields && fields.some((f) => formFieldBaked(f)));
}

// Unified export through the C++ flatten path: rasterizes each page, draws
// annotations + form-field text on top, optionally encrypts, and downloads.
async function exportViaWasm(pages, { protect, suffix }) {
  if (!currentPdfBytes || currentPdfBytes.length === 0) {
    setStatus('Export failed: no source PDF loaded', true);
    return false;
  }
  if (!Module._nanopdf_doc_load || !Module._nanopdf_export_pdf) {
    setStatus('Export requires rebuilding the nanopdf WASM module', true);
    return false;
  }
  let userPassword = '', ownerPassword = '';
  if (protect) {
    userPassword = exportPassword.value;
    ownerPassword = exportOwnerPassword.value || userPassword;
    if (!userPassword) {
      setStatus('Protected export requires a user password', true);
      exportPassword.focus();
      return false;
    }
    if (!Module._nanopdf_export_set_passwords) {
      setStatus('Protected export requires rebuilding the nanopdf WASM module', true);
      return false;
    }
  }

  showLoading(`Exporting ${pages.length} page(s)...`);
  await new Promise(r => setTimeout(r, 50));

  let docId = -1;
  try {
    const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, ptr);
    docId = Module._nanopdf_doc_load(ptr, currentPdfBytes.length);
    Module._nanopdf_free(ptr);
    if (docId < 0) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) ||
        'Failed to load source document for export');
    }

    Module._nanopdf_work_clear();
    pages.forEach((pageIdx) => {
      const workIndex = Module._nanopdf_work_add_page(docId, pageIdx);
      if (workIndex < 0) {
        throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) ||
          `Failed to add page ${pageIdx + 1}`);
      }
      Module._nanopdf_annot_clear_page(workIndex);
      for (const a of (annotations[pageIdx] || [])) {
        bakeAnnotationForWorkPage(workIndex, a);
      }
      bakeFormFieldsForWorkPage(workIndex, pageIdx);
    });

    if (protect) {
      const userPtr = Module.stringToNewUTF8(userPassword);
      const ownerPtr = Module.stringToNewUTF8(ownerPassword);
      const pr = Module._nanopdf_export_set_passwords(userPtr, ownerPtr, 3);
      Module._free(userPtr);
      Module._free(ownerPtr);
      if (pr !== 1) {
        throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) ||
          'Failed to configure password protection');
      }
    }

    const exportResult = Module._nanopdf_export_pdf();
    if (exportResult !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Export failed');
    }

    const outPtr = Module._nanopdf_export_get_buffer();
    const outSize = Module._nanopdf_export_get_size();
    const pdfBytes = new Uint8Array(Module.HEAPU8.buffer, outPtr, outSize).slice();
    downloadPdfBytes(pdfBytes, suffix || (protect ? 'protected' : 'annotated'), pages);
    setStatus(`Exported ${pages.length} page(s)${protect ? ' (protected)' : ''}`);
    return true;
  } catch (err) {
    setStatus('Export error: ' + err.message, true);
    console.error(err);
    return false;
  } finally {
    if (protect && Module._nanopdf_export_set_passwords) {
      const emptyPtr = Module.stringToNewUTF8('');
      Module._nanopdf_export_set_passwords(emptyPtr, emptyPtr, 3);
      Module._free(emptyPtr);
    }
    if (docId >= 0) Module._nanopdf_doc_close(docId);
    Module._nanopdf_work_clear();
    hideLoading();
  }
}

// ---- Combine (merge) & Extract (split) ----

async function combinePdfs(files) {
  if (!Module || !Module._nanopdf_merge_start || !currentPdfBytes) {
    setStatus('Combine not available', true);
    return;
  }
  if (!files || !files.length) return;
  showLoading(`Combining ${files.length + 1} PDF(s)...`);
  await new Promise((r) => setTimeout(r, 30));
  const addBytes = (bytes) => {
    const ptr = Module._nanopdf_malloc(bytes.length);
    Module.HEAPU8.set(bytes, ptr);
    const r = Module._nanopdf_merge_add_pdf(ptr, bytes.length);
    Module._nanopdf_free(ptr);
    return r;
  };
  try {
    Module._nanopdf_merge_start();
    addBytes(currentPdfBytes);  // this document first
    for (const f of files) {
      addBytes(new Uint8Array(await f.arrayBuffer()));
    }
    if (Module._nanopdf_merge_finish() !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Merge failed');
    }
    const out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    if (!out) throw new Error('Empty merge output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_combined.pdf');
    setStatus(`Combined ${files.length + 1} PDF(s)`);
  } catch (err) {
    setStatus('Combine error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
  }
}

async function extractSelectedPages() {
  if (!Module || !Module._nanopdf_split_pages) {
    setStatus('Extract not available', true);
    return;
  }
  if (selectedPages.size === 0) {
    setStatus('Select pages in the Thumbnails sidebar to extract', true);
    return;
  }
  const pages = [...selectedPages].sort((a, b) => a - b);
  showLoading(`Extracting ${pages.length} page(s)...`);
  await new Promise((r) => setTimeout(r, 30));
  try {
    const jsonPtr = Module.stringToNewUTF8('[' + pages.join(',') + ']');
    const ok = Module._nanopdf_split_pages(jsonPtr);
    Module._free(jsonPtr);
    if (ok !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Extract failed');
    }
    const out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    if (!out) throw new Error('Empty extract output');
    downloadPdfBytes(out, 'extracted', pages);
    setStatus(`Extracted ${pages.length} page(s)`);
  } catch (err) {
    setStatus('Extract error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
  }
}

// Which annotation kinds can be written as real (re-editable) PDF annotations.
function editableMarkupCount() {
  let supported = 0, unsupported = 0;
  for (const k in annotations) {
    for (const a of (annotations[k] || [])) {
      if (a.markupKind || a.type === 'highlight' ||
          (a.type === 'text' && a.text && a.text.trim())) supported++;
      else unsupported++;
    }
  }
  return { supported, unsupported };
}

// Map a markup annotation to nanopdf_edit_add_markup's type code.
function markupTypeCode(a) {
  const k = a.markupKind || (a.type === 'highlight' ? 'highlight' : null);
  return { highlight: 0, underline: 1, squiggly: 2, strike: 3 }[k];
}

// Save highlights / text-markup / notes as REAL, re-editable PDF annotation
// objects via an incremental update (vs the flattened "Save").
async function saveEditableAnnotations() {
  if (!Module || !Module._nanopdf_edit_load || !currentPdfBytes || !currentPdfBytes.length) {
    setStatus('Editable annotation save not available', true);
    return;
  }
  const counts = editableMarkupCount();
  if (!counts.supported) {
    setStatus('No highlights/markup/notes to save editably (use Save to flatten shapes)', true);
    return;
  }
  showLoading('Saving editable annotations...');
  await new Promise((r) => setTimeout(r, 30));
  try {
    const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, ptr);
    const ok = Module._nanopdf_edit_load(ptr, currentPdfBytes.length);
    Module._nanopdf_free(ptr);
    if (ok !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Failed to load for editing');
    }
    let written = 0;
    for (const k in annotations) {
      const page = parseInt(k, 10);
      for (const a of (annotations[k] || [])) {
        const c = a.color || { r: 1, g: 1, b: 0 };
        const code = markupTypeCode(a);
        if (code != null) {
          const q = a.quad || { x: a.x, y: a.y, w: a.w, h: a.h };
          const alpha = code === 0 ? (a.alpha != null ? a.alpha : 0.4) : 1;
          if (Module._nanopdf_edit_add_markup(page, code, q.x, q.y, q.w, q.h, c.r, c.g, c.b, alpha)) written++;
        } else if (a.type === 'text' && a.text && a.text.trim()) {
          const tp = Module.stringToNewUTF8(a.text);
          if (Module._nanopdf_edit_add_note(page, a.x, a.y, a.w, a.h, tp)) written++;
          Module._free(tp);
        }
      }
    }
    if (Module._nanopdf_edit_save() !== 1) {
      throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Annotation save failed');
    }
    const out = copyWasmBuffer(Module._nanopdf_edit_get_buffer, Module._nanopdf_edit_get_size);
    if (!out) throw new Error('Empty output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_annotated.pdf');
    setStatus(`Saved ${written} editable annotation(s)` +
      (counts.unsupported ? ` — ${counts.unsupported} shape/ink/image need flattened "Save"` : ''));
  } catch (err) {
    setStatus('Editable save error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
  }
}

// Save the whole document (all pages) with annotations + form fills baked in.
async function saveAnnotatedPdf() {
  if (!Module || totalPages <= 0 || !hasRendering) return;
  const pages = [];
  for (let i = 0; i < totalPages; i++) pages.push(i);
  // Redaction must burn pixels client-side to truly remove text. Password
  // protection isn't applied on that path, so prefer security when both are set.
  if (docHasRedaction()) {
    if (protectExport.checked) {
      setStatus('Redaction is applied without password protection (security takes priority)', false);
    }
    await saveFlattenedPdf(pages, 'redacted');
    return;
  }
  await exportViaWasm(pages, { protect: protectExport.checked, suffix: 'annotated' });
  renderCurrentPage();
}

// ---- Client-side flatten: burn annotations into the page raster ----
// Used so redaction truly removes the underlying pixels (a vector box over a
// page image would leave the text recoverable in the image).

function rgbCss(c) {
  c = c || { r: 0, g: 0, b: 0 };
  return `rgb(${Math.round(c.r * 255)},${Math.round(c.g * 255)},${Math.round(c.b * 255)})`;
}

async function preloadStampImages() {
  const byId = {};
  for (const k in annotations) {
    for (const a of annotations[k]) {
      if (a.type === 'image' && a.dataURL && byId[a.imageId] === undefined) byId[a.imageId] = a.dataURL;
    }
  }
  const cache = {};
  await Promise.all(Object.keys(byId).map((id) => new Promise((res) => {
    const img = new Image();
    img.onload = () => { cache[id] = img; res(); };
    img.onerror = () => res();
    img.src = byId[id];
  })));
  return cache;
}

// Draw a page's annotations + form-field values onto a canvas. sc = px/pt.
function flattenAnnotationsToCanvas(ctx, pageIdx, sc, phPts, imgCache) {
  const PX = (x) => x * sc;
  const PY = (y) => (phPts - y) * sc;
  const list = annotations[pageIdx] || [];
  for (const a of list) {
    ctx.save();
    const css = rgbCss(a.color);
    if (a.type === 'redact') {
      ctx.fillStyle = '#000';
      ctx.fillRect(PX(a.x), PY(a.y + a.h), a.w * sc, a.h * sc);
    } else if (a.type === 'highlight') {
      ctx.globalAlpha = a.alpha != null ? a.alpha : 0.4;
      ctx.fillStyle = css;
      ctx.fillRect(PX(a.x), PY(a.y + a.h), a.w * sc, a.h * sc);
    } else if (a.type === 'rect') {
      ctx.lineWidth = Math.max(1, (a.lineWidth || 2) * sc);
      if (a.filled) { ctx.fillStyle = css; ctx.fillRect(PX(a.x), PY(a.y + a.h), a.w * sc, a.h * sc); }
      else { ctx.strokeStyle = css; ctx.strokeRect(PX(a.x), PY(a.y + a.h), a.w * sc, a.h * sc); }
    } else if (a.type === 'oval') {
      ctx.lineWidth = Math.max(1, (a.lineWidth || 2) * sc);
      ctx.beginPath();
      ctx.ellipse(PX(a.x + a.w / 2), PY(a.y + a.h / 2), (a.w / 2) * sc, (a.h / 2) * sc, 0, 0, Math.PI * 2);
      if (a.filled) { ctx.fillStyle = css; ctx.fill(); } else { ctx.strokeStyle = css; ctx.stroke(); }
    } else if (a.type === 'line' || a.type === 'arrow') {
      ctx.strokeStyle = css; ctx.lineWidth = Math.max(1, (a.lineWidth || 2) * sc); ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(PX(a.x1), PY(a.y1)); ctx.lineTo(PX(a.x2), PY(a.y2)); ctx.stroke();
      if (a.type === 'arrow') {
        for (const s of arrowHeadPdf(a.x1, a.y1, a.x2, a.y2)) {
          ctx.beginPath(); ctx.moveTo(PX(s.x1), PY(s.y1)); ctx.lineTo(PX(s.x2), PY(s.y2)); ctx.stroke();
        }
      }
    } else if (a.type === 'ink') {
      ctx.strokeStyle = css; ctx.lineWidth = Math.max(1, (a.lineWidth || 2) * sc);
      ctx.lineJoin = 'round'; ctx.lineCap = 'round';
      ctx.beginPath();
      a.points.forEach((p, i) => { const x = PX(p.x), y = PY(p.y); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
      ctx.stroke();
    } else if (a.type === 'text') {
      if (a.text && a.text.trim()) {
        const fs = (a.fontSize || 14) * sc;
        ctx.fillStyle = css; ctx.font = `${fs}px Helvetica, Arial, sans-serif`; ctx.textBaseline = 'top';
        let ty = PY(a.y + a.h);
        for (const ln of a.text.split('\n')) { ctx.fillText(ln, PX(a.x), ty); ty += fs * 1.25; }
      }
    } else if (a.type === 'image') {
      const img = imgCache[a.imageId];
      if (img) ctx.drawImage(img, PX(a.x), PY(a.y + a.h), a.w * sc, a.h * sc);
    }
    ctx.restore();
  }
  // Form-field values (text/checkbox/dropdown) flattened as text.
  for (const f of (formFieldsByPage[pageIdx] || [])) {
    const txt = formFieldBaked(f);
    if (!txt) continue;
    const fs = Math.max(8, Math.min(f.rect.height * 0.7, 12)) * sc;
    ctx.fillStyle = '#000'; ctx.font = `${fs}px Helvetica, Arial, sans-serif`; ctx.textBaseline = 'top';
    let ty = PY(f.rect.y + f.rect.height) + 1;
    for (const ln of String(txt).split('\n')) { ctx.fillText(ln, PX(f.rect.x) + 2, ty); ty += fs * 1.25; }
  }
}

// Render one page (with its annotations + redaction burned in) to a 1-page
// image PDF (Uint8Array), or null on failure.
function flattenOnePageToImagePdf(pageIdx, imgCache) {
  const pw = Module._nanopdf_get_page_width(pageIdx);
  const ph = Module._nanopdf_get_page_height(pageIdx);
  const scale = 150 / 72;
  const width = Math.min(4096, Math.floor(pw * scale));
  const height = Math.min(4096, Math.floor(ph * scale));
  const dpi = 72 * (width / pw);
  let ok = 0;
  try { ok = Module._nanopdf_render_page(pageIdx, width, height, dpi); } catch (e) { return null; }
  if (ok !== 1) return null;
  const bufPtr = Module._nanopdf_get_render_buffer();
  const bufSize = Module._nanopdf_get_render_buffer_size();
  const rw = Module._nanopdf_get_render_width();
  const rh = Module._nanopdf_get_render_height();
  const cvs = document.createElement('canvas');
  cvs.width = rw; cvs.height = rh;
  const ctx = cvs.getContext('2d');
  const imageData = ctx.createImageData(rw, rh);
  imageData.data.set(new Uint8ClampedArray(Module.HEAPU8.buffer, bufPtr, bufSize));
  ctx.putImageData(imageData, 0, 0);
  flattenAnnotationsToCanvas(ctx, pageIdx, rw / pw, ph, imgCache);  // burns redaction
  return buildPdf([{ jpegBytes: canvasToJpegBytes(cvs), imgWidth: rw, imgHeight: rh, pageWidth: pw, pageHeight: ph }]);
}

// Extract a contiguous run of original pages as a vector PDF (Uint8Array).
function extractVectorRun(indices) {
  if (!indices.length) return null;
  const jp = Module.stringToNewUTF8('[' + indices.join(',') + ']');
  const ok = Module._nanopdf_split_pages(jp);
  Module._free(jp);
  if (ok !== 1) return null;
  return copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
}

// Save with annotations baked in, rasterizing ONLY the pages that actually have
// annotations / redaction; untouched pages are kept as vector. The result is
// assembled by merging vector runs with single rasterized pages in order.
async function saveFlattenedPdf(pages, suffix) {
  if (!Module || !hasRendering) return;
  showLoading(`Saving ${pages.length} page(s)...`);
  await new Promise((r) => setTimeout(r, 30));
  try {
    const imgCache = await preloadStampImages();
    const parts = [];          // ordered PDF byte buffers
    let vectorRun = [];        // accumulating consecutive vector page indices
    let flattened = 0;
    const flushVector = () => {
      if (!vectorRun.length) return;
      const b = extractVectorRun(vectorRun);   // uses + copies the merge buffer
      if (b) parts.push(b);
      vectorRun = [];
    };
    for (let idx = 0; idx < pages.length; idx++) {
      const p = pages[idx];
      loadingText.textContent = `Processing page ${idx + 1} / ${pages.length}...`;
      if (pageHasBakeableContent(p)) {
        flushVector();
        const img = flattenOnePageToImagePdf(p, imgCache);
        if (img) { parts.push(img); flattened++; }
      } else {
        vectorRun.push(p);
      }
      if (idx % 8 === 7) await new Promise((r) => setTimeout(r, 0));
    }
    flushVector();
    if (!parts.length) { setStatus('Nothing to save', true); return; }

    loadingText.textContent = 'Assembling PDF...';
    await new Promise((r) => setTimeout(r, 30));
    let out;
    if (parts.length === 1) {
      out = parts[0];
    } else {
      // Merge the ordered parts (vector runs + rasterized pages) into one PDF.
      Module._nanopdf_merge_start();
      for (const b of parts) {
        const ptr = Module._nanopdf_malloc(b.length);
        Module.HEAPU8.set(b, ptr);
        Module._nanopdf_merge_add_pdf(ptr, b.length);
        Module._nanopdf_free(ptr);
      }
      if (Module._nanopdf_merge_finish() !== 1) {
        throw new Error(Module.UTF8ToString(Module._nanopdf_get_last_error()) || 'Merge failed');
      }
      out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    }
    if (!out) throw new Error('Empty output');
    downloadPdfBytes(out, suffix || 'flattened', pages);
    setStatus(`Saved ${pages.length} page(s) — ${flattened} rasterized, ${pages.length - flattened} kept vector`);
  } catch (err) {
    setStatus('Save error: ' + err.message, true);
    console.error(err);
  } finally {
    hideLoading();
    renderCurrentPage();
  }
}

function docHasRedaction() {
  for (const k in annotations) {
    if ((annotations[k] || []).some((a) => a.type === 'redact')) return true;
  }
  return false;
}

async function exportSelectedPages() {
  if (selectedPages.size === 0 || !hasRendering || !Module) return;

  const pages = [...selectedPages].sort((a, b) => a - b);

  // Redaction on any selected page requires the secure client-side burn.
  const hasRedaction = pages.some((p) => (annotations[p] || []).some((a) => a.type === 'redact'));
  if (hasRedaction && !protectExport.checked) {
    await saveFlattenedPdf(pages, 'redacted');
    return;
  }
  // Route through the C++ flatten path when encryption is requested or any
  // selected page carries annotations / form-field fills, so they get baked in.
  const hasBakeable = pages.some(pageHasBakeableContent);
  if (protectExport.checked || hasBakeable) {
    await exportViaWasm(pages, {
      protect: protectExport.checked,
      suffix: protectExport.checked ? 'protected_pages' : 'pages',
    });
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
openUrlBtn.addEventListener('click', promptOpenUrl);
combineBtn.addEventListener('click', () => combineInput.click());
combineInput.addEventListener('change', async (e) => {
  const files = [...e.target.files];
  combineInput.value = '';
  if (files.length) await combinePdfs(files);
});
extractPagesBtn.addEventListener('click', extractSelectedPages);
organizeBtn.addEventListener('click', openOrganize);
if (signBtn) signBtn.addEventListener('click', openSign);
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

// Pan the page by dragging with the middle mouse (wheel) button.
let isPanning = false;
let panStartX = 0;
let panStartY = 0;
let panScrollLeft = 0;
let panScrollTop = 0;

function onPanMove(e) {
  if (!isPanning) return;
  canvasScroll.scrollLeft = panScrollLeft - (e.clientX - panStartX);
  canvasScroll.scrollTop = panScrollTop - (e.clientY - panStartY);
}

function endPan() {
  if (!isPanning) return;
  isPanning = false;
  canvasScroll.classList.remove('panning');
  window.removeEventListener('mousemove', onPanMove);
  window.removeEventListener('mouseup', endPan);
}

canvasScroll.addEventListener('mousedown', (e) => {
  if (e.button !== 1) return; // middle (wheel) button only
  e.preventDefault();         // suppress the browser's middle-click autoscroll
  isPanning = true;
  panStartX = e.clientX;
  panStartY = e.clientY;
  panScrollLeft = canvasScroll.scrollLeft;
  panScrollTop = canvasScroll.scrollTop;
  canvasScroll.classList.add('panning');
  window.addEventListener('mousemove', onPanMove);
  window.addEventListener('mouseup', endPan);
});

// Stop the middle-click default action (autoscroll / paste-on-*nix) on release.
canvasScroll.addEventListener('auxclick', (e) => {
  if (e.button === 1) e.preventDefault();
});

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
  } else if ((e.key === 'Delete' || e.key === 'Backspace') &&
             selectedAnnotId != null && annotEditingEnabled()) {
    e.preventDefault();
    deleteSelectedAnnot();
  } else if (e.key === 'Escape' && selectedAnnotId != null) {
    selectedAnnotId = null;
    renderAnnotations();
    updateAnnotLayerMode();
  } else if ((e.key === 'v' || e.key === 'V') && !e.ctrlKey && !e.metaKey &&
             annotEditingEnabled()) {
    setAnnotTool('select');
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

// Annotation tools
annotTools.querySelectorAll('.annot-tool').forEach((btn) => {
  btn.addEventListener('click', () => {
    if (!annotEditingEnabled()) {
      setStatus(rotation !== 0
        ? 'Reset rotation to 0° to edit annotations'
        : 'Open a PDF to annotate', true);
      return;
    }
    if (btn.dataset.tool === 'image') { pickStampImage(); return; }
    setAnnotTool(btn.dataset.tool);
  });
});
if (stampInput) {
  stampInput.addEventListener('change', async (e) => {
    const file = e.target.files[0];
    stampInput.value = '';
    if (file) await loadStampImage(file);
  });
}
annotColor.addEventListener('input', () => {
  annotColorHex = annotColor.value;
  // Recolor the currently selected annotation, if any.
  if (selectedAnnotId != null) {
    const a = findAnnot(selectedAnnotId);
    if (a) { a.color = hexToRgb(annotColorHex); renderAnnotations(); }
  }
});
annotFill.addEventListener('change', () => {
  annotFillShapes = annotFill.checked;
  if (selectedAnnotId != null) {
    const a = findAnnot(selectedAnnotId);
    if (a && (a.type === 'rect' || a.type === 'oval')) {
      a.filled = annotFillShapes; renderAnnotations();
    }
  }
});
annotDeleteBtn.addEventListener('click', deleteSelectedAnnot);
saveAnnotBtn.addEventListener('click', saveAnnotatedPdf);
saveEditableBtn.addEventListener('click', saveEditableAnnotations);
saveFormBtn.addEventListener('click', saveEditableForm);

if (markupPopover) {
  markupPopover.querySelectorAll('button').forEach((btn) => {
    btn.addEventListener('pointerdown', (e) => e.preventDefault());  // keep selection
    btn.addEventListener('click', () => markupSelection(btn.dataset.markup));
  });
}

annotLayer.addEventListener('pointerdown', annotLayerPointerDown);
annotLayer.addEventListener('pointermove', annotLayerPointerMove);
annotLayer.addEventListener('pointerup', annotLayerPointerUp);
annotLayer.addEventListener('pointercancel', annotLayerPointerUp);
annotSvg.addEventListener('pointerdown', annotShapePointerDown);

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

    // Deep-link support: ?url=<pdf> (or ?pdf=<pdf>) auto-opens a remote PDF.
    const params = new URLSearchParams(window.location.search);
    const deepLink = params.get('url') || params.get('pdf');
    if (deepLink) {
      loadPdfFromUrl(deepLink);
    }
  } catch (err) {
    hideLoading();
    setStatus('Failed to load WASM: ' + err.message, true);
    console.error(err);
  }
}

init();
