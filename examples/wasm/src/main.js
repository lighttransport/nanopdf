import './style.css';
import createModule from 'nanopdf-wasm';
import wasmUrl from 'nanopdf-wasm-bin';
import { loadStandardFonts, loadCDNCJKFonts } from './font-loader.js';
import {
  annotLabel,
  assertOkOrThrow,
  chooseSelectedExportRoute,
  clamp,
  computeRenderSize,
  dashToStrokeDasharray,
  escapeHtml,
  escapeRegExp,
  formatFileSize,
  formatPdfDate,
  formatRecoveryAge,
  getWasmAccelerationInfo,
  getMdpPermissionLabel,
  highlightContext,
  outlineBranchMatches,
  readWasmString,
  renderPageIntoCanvas,
  renderPageIntoImageData,
  resolvePdfDeepLink,
  shouldRenderPageStatus,
} from './viewer-utils.js';

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
let pendingPageScrollPlacement = null;
let pendingSeamlessOffset = null;
let lastPromoteTime = 0;
let scrollSnapLocked = false;
let lastCanvasScrollTop = 0;
let renderDocumentVersion = 0;
let adjacentPreviewJobId = 0;
let adjacentPreviewTimer = 0;
let adjacentPreviewTimerKind = '';
const adjacentPreviewRenderKeys = { prev: '', next: '' };
let fullRenderJobId = 0;
const fullRenderKeys = new Set();
let fileName = '';
let fileSize = 0;
let currentPdfBytes = null;
let outlineData = null;
let outlineFilter = '';   // case-insensitive substring filter on outline title
// User-added bookmarks (per page). Cleared on document load.
let userBookmarks = [];   // [{ id, page, title }]
let signatureData = null;
let editHistoryData = null;
let searchResults = [];
let currentSearchIndex = -1;
let currentSearchTerm = '';
let searchCaseSensitive = false;
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

// Persisted view preferences (zoom, fit mode, backend) + per-file last page.
const VIEW_STATE_KEY = 'nanopdf-view-state';
function loadViewState() {
  try { return JSON.parse(localStorage.getItem(VIEW_STATE_KEY) || '{}') || {}; }
  catch (e) { return {}; }
}
function saveViewState() {
  try {
    const s = loadViewState();
    s.zoom = zoomLevel;
    s.fitMode = fitMode;
    s.backend = renderBackend;
    if (fileName) {
      s.pages = s.pages || {};
      s.pages[fileName] = currentPage;
      // Bound the per-file page map so it can't grow without limit.
      const keys = Object.keys(s.pages);
      if (keys.length > 50) delete s.pages[keys[0]];
    }
    localStorage.setItem(VIEW_STATE_KEY, JSON.stringify(s));
  } catch (e) { /* storage disabled/full: ignore */ }
}
// CSS-pixel display size of the page canvas (the bitmap is this * devicePixelRatio).
// Overlay/coordinate math uses these, NOT canvas.width/height (which are device px).
let pageDisplayWidth = 0;
let pageDisplayHeight = 0;
let thumbnailCache = {}; // pageIndex -> ImageData
let thumbnailRenderQueue = [];
let isThumbnailRendering = false;
// Monotonic counter incremented on every main-canvas render. The thumbnail
// queue records the counter at queue-start; if a user re-renders the main
// page mid-queue, the recorded value is stale and the main canvas no longer
// needs restoring when the queue drains.
let mainRenderCounter = 0;
let thumbnailQueueStartedBeforeMainRender = 0;
let thumbnailObserver = null;

// Page selection state
let selectedPages = new Set();
let lastClickedPage = 0;

// ---- Annotation state ----
// annotations[pageIndex] = array of annotation objects. Geometry is stored in
// PDF page points with a bottom-left origin so it survives zoom/rotation and
// maps 1:1 to the nanopdf_annot_* export API.
let annotations = {};
let annotTool = 'select';        // select | highlight | text | rect | oval | line | arrow | ink | redact | link | image
let annotColorHex = '#ffd54a';
let annotFillShapes = false;
let selectedAnnotId = null;
let hoveredAnnotId = null;   // annotation under the cursor in select mode
let annotIdCounter = 1;
let annotDraft = null;           // in-progress drawing
let annotDragState = null;       // moving an existing annotation in select mode
let pendingStampImage = null;    // { imageId, dataURL, aspect } awaiting placement
// formFields[pageIndex] = [{name, type, value, rect:{x,y,width,height}, multiline, ...}]
let formFieldsByPage = {};

// ---- Undo/redo ----
// Snapshot-based: every discrete annotation or form-field change captures the
// pre-change state and pushes it onto the undo stack. Coalesces high-frequency
// text input (form fields) by waiting for blur.
const MAX_UNDO = 50;
const undoStack = [];
const redoStack = [];
let formFieldEditSnapshot = null;  // held during a focused form-field edit

function snapshotState() {
  return {
    annotations: JSON.parse(JSON.stringify(annotations)),
    formFields: JSON.parse(JSON.stringify(formFieldsByPage)),
    selectedAnnotId,
    hoveredAnnotId,
  };
}
function pushUndo() {
  undoStack.push(snapshotState());
  if (undoStack.length > MAX_UNDO) undoStack.shift();
  redoStack.length = 0;
  updateUndoRedoButtons();
}
function applySnapshot(s) {
  annotations = s.annotations;
  formFieldsByPage = s.formFields;
  selectedAnnotId = s.selectedAnnotId;
  hoveredAnnotId = s.hoveredAnnotId;
  // Form-field cache is keyed by page+structure; force a rebuild.
  renderedFormFieldsPage = -1;
  renderedFormFieldsKey = '';
  renderAnnotations();
  renderFormFields();
  updateAnnotLayerMode();
  updateSaveAnnotState();
}
function undo() {
  if (!undoStack.length) return false;
  redoStack.push(snapshotState());
  if (redoStack.length > MAX_UNDO) redoStack.shift();
  const s = undoStack.pop();
  applySnapshot(s);
  updateUndoRedoButtons();
  return true;
}
function redo() {
  if (!redoStack.length) return false;
  undoStack.push(snapshotState());
  if (undoStack.length > MAX_UNDO) undoStack.shift();
  const s = redoStack.pop();
  applySnapshot(s);
  updateUndoRedoButtons();
  return true;
}
function clearUndoHistory() {
  undoStack.length = 0;
  redoStack.length = 0;
  formFieldEditSnapshot = null;
  updateUndoRedoButtons();
}
function updateUndoRedoButtons() {
  const ub = document.getElementById('undo-btn');
  const rb = document.getElementById('redo-btn');
  if (ub) {
    ub.disabled = undoStack.length === 0;
    ub.title = undoStack.length === 0
      ? 'Nothing to undo'
      : `Undo (${modKeyLabel('Z')}) — ${undoStack.length} step${undoStack.length === 1 ? '' : 's'}`;
  }
  if (rb) {
    rb.disabled = redoStack.length === 0;
    rb.title = redoStack.length === 0
      ? 'Nothing to redo'
      : `Redo (${modKeyLabel('Shift+Z')}) — ${redoStack.length} step${redoStack.length === 1 ? '' : 's'}`;
  }
}
function modKeyLabel(key) {
  const mod = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent) ? '⌘' : 'Ctrl';
  return `${mod}+${key}`;
}

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
const bookmarkBtn = document.getElementById('bookmark-btn');
const sidebarToggleBtn = document.getElementById('sidebar-toggle-btn');
const canvas = document.getElementById('pdf-canvas');
const textOverlay = document.getElementById('text-overlay');
const canvasWrapper = document.getElementById('canvas-wrapper');
const prevPageWrapper = document.getElementById('prev-page-wrapper');
const prevPageCanvas = document.getElementById('prev-page-canvas');
const nextPageWrapper = document.getElementById('next-page-wrapper');
const nextPageCanvas = document.getElementById('next-page-canvas');
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
const searchPages = document.getElementById('search-pages');
const zoomInBtn = document.getElementById('zoom-in-btn');
const zoomOutBtn = document.getElementById('zoom-out-btn');
const zoomDisplay = document.getElementById('zoom-display');
const zoomSlider = document.getElementById('zoom-slider');
const fitModeBtn = document.getElementById('fit-mode-btn');
const rotateBtn = document.getElementById('rotate-btn');
const canvasScroll = document.getElementById('canvas-scroll');
const exportBtn = document.getElementById('export-btn');
const protectExport = document.getElementById('protect-export');
const exportPasswordForm = document.getElementById('export-password-form');
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

// Tri-state status: kind ∈ 'info' (default) | 'success' | 'error'. `true` is
// kept as a back-compat shorthand for 'error' so existing call sites that pass
// `setStatus(msg, true)` still work.
function setStatus(msg, kind = 'info') {
  const k = kind === true ? 'error' : (kind || 'info');
  statusText.textContent = msg;
  statusBar.className = 'status-bar' + (k === 'info' ? '' : ' ' + k);
}

// Cached helper: pulls the most-recent C-side error message, treating a NULL
// pointer as the empty string. Reused across every WASM error site so we
// never call UTF8ToString(0) (which is undefined behavior on Emscripten).
function wasmLastError() {
  if (!Module || !Module._nanopdf_get_last_error) return '';
  return readWasmString(Module, Module._nanopdf_get_last_error());
}

let operationStatusHoldUntil = 0;
function setOperationStatus(msg, kind = 'success', holdMs = 2500) {
  // Cap the hold so rapid back-to-back operations don't keep the page-info
  // status hidden for stacked intervals. If a hold is already active, the new
  // message still shows up immediately, but the new hold is bounded by the
  // remaining time on the existing one.
  const now = Date.now();
  const remaining = Math.max(0, operationStatusHoldUntil - now);
  operationStatusHoldUntil = now + Math.min(holdMs, remaining > 0 ? remaining : holdMs);
  // `true` is a back-compat shorthand for 'error'.
  setStatus(msg, kind === true ? 'error' : kind);
}

function showLoading(msg) {
  loadingText.textContent = msg;
  loadingOverlay.classList.remove('hidden');
}

function hideLoading() {
  loadingOverlay.classList.add('hidden');
}

function afterNextPaint() {
  return new Promise((resolve) => {
    if (typeof window.requestAnimationFrame !== 'function') {
      setTimeout(resolve, 0);
      return;
    }
    window.requestAnimationFrame(() => {
      window.requestAnimationFrame(resolve);
    });
  });
}

function scheduleDocumentIdleWork(callback) {
  if (typeof window.requestIdleCallback === 'function') {
    window.requestIdleCallback(callback, { timeout: 250 });
  } else {
    setTimeout(callback, 32);
  }
}

function isCurrentDocumentVersion(version) {
  return version === renderDocumentVersion;
}

function updatePageDisplay() {
  if (isPageJumpMode) return;
  pageDisplay.textContent = `Page ${currentPage + 1} / ${totalPages}`;
  prevBtn.disabled = currentPage <= 0;
  nextBtn.disabled = currentPage >= totalPages - 1;
}

function updateZoomDisplay() {
  const pct = Math.round(zoomLevel * 100);
  zoomDisplay.textContent = pct + '%';
  if (zoomSlider && Number(zoomSlider.value) !== pct) {
    zoomSlider.value = String(pct);
  }
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
    const error = wasmLastError() || 'backend unavailable';
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
  saveViewState();
}

// ---- Sidebar ----

function renderOutlineTab() {
  const hasNative = outlineData && outlineData.outline && outlineData.outline.length > 0;
  const hasUser = userBookmarks.length > 0;
  if (!hasNative && !hasUser) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No bookmarks in this document.</div>';
    return;
  }

  const q = outlineFilter.trim().toLowerCase();
  const items = q
    ? (hasNative ? outlineData.outline.filter((it) => outlineBranchMatches([it], q)) : [])
    : (hasNative ? outlineData.outline : []);
  const userItems = q
    ? userBookmarks.filter((b) => b.title.toLowerCase().includes(q))
    : userBookmarks;
  const noMatch = q && items.length === 0 && userItems.length === 0;

  let html = '';
  // Filter input.
  html += '<div class="outline-filter">';
  html += '<input type="text" id="outline-filter-input" placeholder="Filter bookmarks…" ' +
          `value="${escapeHtml(outlineFilter)}" autocomplete="off" />`;
  if (q) {
    html += `<button type="button" id="outline-filter-clear" title="Clear filter">&times;</button>`;
  }
  html += '</div>';

  if (noMatch) {
    html += '<div class="outline-empty">No matches for &ldquo;' + escapeHtml(outlineFilter) + '&rdquo;.</div>';
    sidebarContent.innerHTML = html;
    wireOutlineFilter();
    return;
  }

  if (userItems.length) {
    html += '<div class="outline-section-header">My bookmarks</div>';
    for (const b of userItems) {
      const pageLabel = ` (p.${b.page + 1})`;
      const title = q ? highlightContext(b.title, outlineFilter) : escapeHtml(b.title);
      html += `<div class="outline-item outline-item-user" data-page="${b.page}" data-user-bookmark="1" style="padding-left:8px;" title="My bookmark${escapeHtml(pageLabel)}">${title}<span style="color:#999;font-size:11px;">${escapeHtml(pageLabel)}</span></div>`;
    }
  }

  if (hasNative && items.length) {
    if (userItems.length) html += '<div class="outline-section-header">Document outline</div>';
  }

  function renderItems(list, depth) {
    for (const item of list) {
      const indent = depth * 16;
      const page = Number.isInteger(item.page) ? item.page : null;
      const pageLabel = page !== null ? ` (p.${page + 1})` : '';
      const pageAttr = page !== null ? ` data-page="${page}"` : '';
      const style = `padding-left:${indent + 8}px`;
      const boldStyle = item.bold ? 'font-weight:600;' : '';
      const italicStyle = item.italic ? 'font-style:italic;' : '';
      const title = q
        ? highlightContext(item.title || '', outlineFilter)
        : escapeHtml(item.title || '');
      html += `<div class="outline-item"${pageAttr} style="${style};${boldStyle}${italicStyle}" title="${escapeHtml(item.title || '')}${escapeHtml(pageLabel)}">${title}<span style="color:#999;font-size:11px;">${escapeHtml(pageLabel)}</span></div>`;
      if (item.children && item.children.length > 0) {
        const kids = q
          ? item.children.filter((c) => outlineBranchMatches([c], q))
          : item.children;
        if (kids.length) renderItems(kids, depth + 1);
      }
    }
  }
  renderItems(items, 0);
  sidebarContent.innerHTML = html;
  wireOutlineFilter();
  sidebarContent.querySelectorAll('.outline-item[data-page]').forEach((item) => {
    item.addEventListener('click', () => goToPage(parseInt(item.dataset.page, 10)));
  });
}

function wireOutlineFilter() {
  const input = document.getElementById('outline-filter-input');
  if (input) {
    input.addEventListener('input', (e) => {
      outlineFilter = e.target.value;
      renderOutlineTab();
      // Restore focus + caret so the user can keep typing.
      const el = document.getElementById('outline-filter-input');
      if (el) {
        el.focus();
        const v = el.value;
        el.setSelectionRange(v.length, v.length);
      }
    });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { outlineFilter = ''; renderOutlineTab(); }
    });
  }
  const clear = document.getElementById('outline-filter-clear');
  if (clear) clear.addEventListener('click', () => { outlineFilter = ''; renderOutlineTab(); });
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
    html += `<div class="doc-info-item"><div class="label">${escapeHtml(item.label)}</div><div class="value">${escapeHtml(item.value)}</div></div>`;
  }
  sidebarContent.innerHTML = html;
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
    html += `<div class="history-title">Revision ${escapeHtml(rev.revision)}</div>`;
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

let diffTrigger = null;   // element focused before the diff overlay opened

const diffState = {
  open: false,
  revIndex: 0,
  page: 0,
  pageCount: 0,
  mode: 'diff',          // 'diff' | 'before' | 'after' | 'swipe' | 'side' | 'text'
  swipe: 0.5,
  threshold: 28,         // luma diff sensitivity (higher = ignore more AA noise)
  showBoxes: true,       // draw change-region bounding boxes in diff mode
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
  if (Module._nanopdf_release_render_buffer) Module._nanopdf_release_render_buffer();
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
  const T = diffState.threshold;   // luma threshold: ignore sub-threshold AA noise
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
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-modal', 'true');
  el.setAttribute('aria-label', 'Revision changes');
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
          <button data-mode="text">Text</button>
        </div>
        <button class="rev-diff-close" id="rev-diff-close" title="Close">&times;</button>
      </div>
      <div class="rev-diff-changeinfo" id="rev-diff-changeinfo"></div>
      <div class="rev-diff-body">
        <canvas id="rev-diff-canvas"></canvas>
        <div class="rev-diff-text hidden" id="rev-diff-text"></div>
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
        <label class="rev-diff-sens" id="rev-diff-sens" title="Diff threshold: lower catches faint changes, higher ignores anti-aliasing noise">
          Threshold <span id="rev-diff-sens-val">28</span>
          <input type="range" id="rev-diff-sens-range" min="5" max="80" value="28" />
        </label>
        <label class="rev-diff-boxes" id="rev-diff-boxes-label" title="Outline changed regions">
          <input type="checkbox" id="rev-diff-boxes" checked /> Boxes
        </label>
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
      el.querySelector('#rev-diff-sens').classList.toggle('hidden', diffState.mode !== 'diff');
      el.querySelector('#rev-diff-boxes-label').classList.toggle('hidden', diffState.mode !== 'diff');
      paintDiffCanvas();
    });
  });
  el.querySelector('#rev-diff-boxes').addEventListener('change', (e) => {
    diffState.showBoxes = e.target.checked;
    paintDiffCanvas();
  });
  el.querySelector('#rev-diff-prev').addEventListener('click', () => stepDiffPage(-1));
  el.querySelector('#rev-diff-next').addEventListener('click', () => stepDiffPage(1));
  el.querySelector('#rev-diff-swipe').addEventListener('input', (e) => {
    diffState.swipe = e.target.value / 100;
    paintDiffCanvas();
  });
  el.querySelector('#rev-diff-sens-range').addEventListener('input', (e) => {
    diffState.threshold = parseInt(e.target.value, 10) || 28;
    el.querySelector('#rev-diff-sens-val').textContent = diffState.threshold;
    // The threshold changes which pages count as changed -> recompute the strip
    // (keeping the current page), then repaint.
    rescanChangedPages();
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
    if (Math.abs(lum(a, i) - lum(b, i)) > diffState.threshold) {
      if (++changed > 40) return true;
    }
  }
  return false;
}

// Word-level diff between two strings -> array of {t:'same'|'add'|'del', w:word}.
// Classic LCS over whitespace-split tokens (page text is short enough for O(n*m)).
function wordDiff(beforeText, afterText) {
  const a = beforeText.split(/\s+/).filter(Boolean);
  const b = afterText.split(/\s+/).filter(Boolean);
  const n = a.length, m = b.length;
  const dp = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
  for (let i = n - 1; i >= 0; i--)
    for (let j = m - 1; j >= 0; j--)
      dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
  const out = [];
  let i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] === b[j]) { out.push({ t: 'same', w: a[i] }); i++; j++; }
    else if (dp[i + 1][j] >= dp[i][j + 1]) { out.push({ t: 'del', w: a[i] }); i++; }
    else { out.push({ t: 'add', w: b[j] }); j++; }
  }
  while (i < n) out.push({ t: 'del', w: a[i++] });
  while (j < m) out.push({ t: 'add', w: b[j++] });
  return out;
}

// Render the word-level text diff for the current diff page into #rev-diff-text.
function renderTextDiff() {
  const host = document.getElementById('rev-diff-text');
  if (!host) return;
  if (!Module._nanopdf_extract_revision_text) {
    host.innerHTML = '<div class="rev-diff-textmsg">Text diff unavailable in this build.</div>';
    return;
  }
  const before = readWasmString(Module, Module._nanopdf_extract_revision_text(diffState.byteBefore, diffState.page));
  const after = readWasmString(Module, Module._nanopdf_extract_revision_text(diffState.byteAfter, diffState.page));
  const parts = wordDiff(before, after);
  const added = parts.filter(p => p.t === 'add').length;
  const removed = parts.filter(p => p.t === 'del').length;
  if (added === 0 && removed === 0) {
    host.innerHTML = `<div class="rev-diff-textmsg">No text changes on this page${after.trim() ? '' : ' (no extractable text)'}.</div>`;
    return;
  }
  let html = `<div class="rev-diff-textmsg">+${added} word${added === 1 ? '' : 's'} added, −${removed} removed</div><div class="rev-diff-textbody">`;
  for (const p of parts) {
    const w = escapeHtml(p.w);
    if (p.t === 'same') html += w + ' ';
    else if (p.t === 'add') html += `<span class="td-add">${w}</span> `;
    else html += `<span class="td-del">${w}</span> `;
  }
  host.innerHTML = html + '</div>';
}

// Recompute which pages differ at the current threshold (capped for big docs),
// preserving the current page. Returns the first changed page index, or -1.
function rescanChangedPages() {
  const savedPage = diffState.page;
  const scanLimit = diffState.pageCount <= 60 ? diffState.pageCount : 0;
  diffState.changed = new Set();
  diffState.changedComputed = scanLimit > 0;
  let first = -1;
  for (let p = 0; p < scanLimit; p++) {
    diffState.page = p;
    loadDiffPage();
    if (diffPageChanged()) { diffState.changed.add(p); if (first < 0) first = p; }
  }
  diffState.page = savedPage;
  loadDiffPage();
  renderDiffStrip();
  return first;
}

// Cluster changed pixels into bounding boxes. Uses a coarse cell grid + 8-conn
// connected components so a paragraph of changes becomes one box rather than
// thousands of speckles. Each region is classified added (darker now) or
// removed (lighter now) by its dominant pixel kind.
function computeChangeRegions(before, after, threshold) {
  if (!after) return [];
  const w = after.w, h = after.h, bd = after.data;
  const A = before && before.w === w && before.h === h ? before.data : null;
  const cell = 8;
  const cols = Math.ceil(w / cell), rows = Math.ceil(h / cell);
  const addCnt = new Int32Array(cols * rows);
  const delCnt = new Int32Array(cols * rows);
  for (let y = 0; y < h; y++) {
    const row = (y / cell | 0) * cols;
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      const d = (A ? lum(A, i) : 255) - lum(bd, i);  // >0 added ink, <0 removed
      if (d > threshold) addCnt[row + (x / cell | 0)]++;
      else if (d < -threshold) delCnt[row + (x / cell | 0)]++;
    }
  }
  const minPix = Math.max(3, (cell * cell * 0.06) | 0);
  const kind = new Int8Array(cols * rows);   // 0 none, 1 add, 2 del
  for (let c = 0; c < kind.length; c++) {
    if (addCnt[c] + delCnt[c] >= minPix) kind[c] = addCnt[c] >= delCnt[c] ? 1 : 2;
  }
  const comp = new Int32Array(cols * rows).fill(-1);
  const regions = [];
  const stack = [];
  for (let c0 = 0; c0 < kind.length; c0++) {
    if (!kind[c0] || comp[c0] !== -1) continue;
    const id = regions.length;
    let minx = cols, miny = rows, maxx = 0, maxy = 0, addS = 0, delS = 0, cells = 0;
    stack.push(c0); comp[c0] = id;
    while (stack.length) {
      const c = stack.pop();
      const cx = c % cols, cy = (c / cols) | 0;
      if (cx < minx) minx = cx; if (cy < miny) miny = cy;
      if (cx > maxx) maxx = cx; if (cy > maxy) maxy = cy;
      addS += addCnt[c]; delS += delCnt[c]; cells++;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        if (!dx && !dy) continue;
        const nx = cx + dx, ny = cy + dy;
        if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
        const nc = ny * cols + nx;
        if (kind[nc] && comp[nc] === -1) { comp[nc] = id; stack.push(nc); }
      }
    }
    if (cells < 2) continue;  // drop tiny specks
    regions.push({
      x: minx * cell, y: miny * cell,
      w: Math.min(w, (maxx + 1) * cell) - minx * cell,
      h: Math.min(h, (maxy + 1) * cell) - miny * cell,
      kind: addS >= delS ? 'add' : 'del',
    });
  }
  return regions;
}

function paintDiffCanvas() {
  const overlay = document.getElementById('rev-diff-overlay');
  const canvas = document.getElementById('rev-diff-canvas');
  const empty = document.getElementById('rev-diff-empty');
  const textHost = document.getElementById('rev-diff-text');

  // Text mode: word-level diff instead of the pixel canvas.
  if (diffState.mode === 'text') {
    canvas.classList.add('hidden');
    empty.classList.add('hidden');
    textHost.classList.remove('hidden');
    renderTextDiff();
    document.getElementById('rev-diff-pageinfo').textContent =
      `Page ${diffState.page + 1} / ${diffState.pageCount}`;
    overlay.querySelector('#rev-diff-prev').disabled = diffState.page <= 0;
    overlay.querySelector('#rev-diff-next').disabled = diffState.page >= diffState.pageCount - 1;
    return;
  }
  if (textHost) textHost.classList.add('hidden');

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

  // Overlay change-region bounding boxes (Diff mode only).
  let regionCount = null;
  if (diffState.mode === 'diff' && diffState.showBoxes !== false && !noChange) {
    const regions = computeChangeRegions(diffState.before, diffState.after, diffState.threshold);
    regionCount = regions.length;
    ctx.lineWidth = 2;
    ctx.font = '11px sans-serif';
    for (const r of regions) {
      ctx.strokeStyle = r.kind === 'add' ? 'rgba(16,162,72,0.95)' : 'rgba(200,40,40,0.95)';
      ctx.strokeRect(r.x + 0.5, r.y + 0.5, Math.max(2, r.w), Math.max(2, r.h));
    }
  }
  const changedCount = diffState.changedComputed ? diffState.changed.size : null;
  document.getElementById('rev-diff-pageinfo').textContent =
    `Page ${diffState.page + 1} / ${diffState.pageCount}` +
    (changedCount != null ? ` — ${changedCount} changed` : '') +
    (regionCount != null ? ` — ${regionCount} region${regionCount === 1 ? '' : 's'}` : '');
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
  overlay.querySelector('#rev-diff-sens').classList.remove('hidden');
  overlay.querySelector('#rev-diff-boxes-label').classList.remove('hidden');
  const signer = rev.signerName || rev.associatedSignature;
  overlay.querySelector('#rev-diff-title').textContent =
    `Revision ${rev.revision} changes` + (signer ? ` — signed by ${signer}` : '');
  diffTrigger = document.activeElement;
  overlay.classList.remove('hidden');
  diffState.open = true;
  renderDiffChangeInfo(rev);

  // Precompute which pages changed (cap the scan for very large docs), and
  // land on the first changed page.
  const first = rescanChangedPages();
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
      (violations.length ? `: ${escapeHtml(violations[0])}` : '') + `</span>`;
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
  if (diffTrigger && typeof diffTrigger.focus === 'function') {
    diffTrigger.focus();
    diffTrigger = null;
  }
}

// ---- Print ----

let printing = false;

// Render every page to an image and print via the browser. A print stylesheet
// shows only the #print-container (one image per page, each on its own sheet);
// the container is removed when printing finishes.
async function printDocument() {
  if (!Module || totalPages <= 0 || !hasRendering || printing) return;
  printing = true;
  const printBtn = document.getElementById('print-btn');
  const printScope = document.getElementById('print-scope');
  if (printBtn) { printBtn.disabled = true; printBtn.textContent = 'Rendering…'; }
  if (printScope) printScope.disabled = true;

  // Tear down any stale container, then build a fresh one.
  const old = document.getElementById('print-container');
  if (old) old.remove();
  const container = document.createElement('div');
  container.id = 'print-container';

  // Build the page-index list based on the current scope selector.
  const onlyCurrent = printScope && printScope.value === 'current';
  const pageList = onlyCurrent ? [currentPage] : Array.from({ length: totalPages }, (_, i) => i);

  const tmp = document.createElement('canvas');
  const failedPages = [];
  try {
    for (const p of pageList) {
      const pw = Module._nanopdf_get_page_width(p) || 612;
      // ~150 DPI for crisp print output, bounded so huge pages stay reasonable.
      const maxW = Math.min(2200, Math.round(pw * (150 / 72)));
      if (!renderPageToCanvas(p, tmp, maxW)) {
        failedPages.push(p);
        continue;
      }
      const img = document.createElement('img');
      img.src = tmp.toDataURL('image/png');
      img.className = 'print-page';
      container.appendChild(img);
      if (printBtn) printBtn.textContent = `Rendering… ${p + 1}/${pageList.length}`;
      // Yield so the progress text paints and the UI doesn't lock up.
      await new Promise((r) => setTimeout(r, 0));
    }
    if (!container.childElementCount) { setStatus('Nothing to print', true); return; }
    document.body.appendChild(container);

    const cleanup = () => {
      const c = document.getElementById('print-container');
      if (c) c.remove();
      window.removeEventListener('afterprint', cleanup);
    };
    window.addEventListener('afterprint', cleanup);
    // Re-render the on-screen page (the shared render buffer was reused above).
    renderCurrentPage();
    if (failedPages.length) {
      setOperationStatus(
        `Printed ${container.childElementCount} / ${pageList.length} page(s) — ` +
        `failed: ${failedPages.map((p) => p + 1).join(', ')}`,
        true,
      );
    }
    window.print();
    // Fallback cleanup for browsers that don't fire afterprint reliably.
    setTimeout(cleanup, 60000);
  } finally {
    printing = false;
    if (printBtn) { printBtn.disabled = false; printBtn.textContent = 'Print'; }
    if (printScope && hasRendering && totalPages > 0) printScope.disabled = false;
  }
}

// ---- Keyboard shortcut help overlay ----

function ensureShortcutHelp() {
  let el = document.getElementById('shortcut-help-overlay');
  if (el) return el;
  const mod = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent) ? '⌘' : 'Ctrl';
  const rows = [
    ['←  /  →', 'Previous / next page'],
    ['Home  /  End', 'First / last page'],
    [`${mod} F`, 'Focus search'],
    [`${mod} +  /  ${mod} −`, 'Zoom in / out'],
    [`${mod} 0`, 'Reset zoom'],
    ['R', 'Rotate page'],
    ['P', 'Print'],
    ['B', 'Bookmark / un-bookmark current page'],
    ['V', 'Select / move tool'],
    ['Tab  /  ⇧ Tab', 'Next / previous annotation'],
    ['Delete', 'Delete selected annotation'],
    [`${mod} Z`, 'Undo'],
    [`${mod} ⇧ Z  /  ${mod} Y`, 'Redo'],
    ['Esc', 'Deselect / close overlay'],
    ['?', 'Show / hide this help'],
  ];
  const diffRows = [
    ['←  /  →', 'Previous / next page (in diff)'],
    ['Esc', 'Close diff'],
  ];
  const rowHtml = (list) => list.map(([k, d]) =>
    `<div class="kbd-row"><kbd>${escapeHtml(k)}</kbd><span>${escapeHtml(d)}</span></div>`).join('');
  el = document.createElement('div');
  el.id = 'shortcut-help-overlay';
  el.className = 'shortcut-help-overlay hidden';
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-label', 'Keyboard shortcuts');
  el.innerHTML = `
    <div class="shortcut-help-panel">
      <div class="shortcut-help-header">
        <span>Keyboard shortcuts</span>
        <button id="shortcut-help-close" title="Close (Esc)" aria-label="Close">&times;</button>
      </div>
      <div class="shortcut-help-body">
        <div class="kbd-group">${rowHtml(rows)}</div>
        <div class="kbd-grouptitle">Revision diff</div>
        <div class="kbd-group">${rowHtml(diffRows)}</div>
      </div>
    </div>`;
  document.body.appendChild(el);
  el.querySelector('#shortcut-help-close').addEventListener('click', hideShortcutHelp);
  el.addEventListener('click', (e) => { if (e.target === el) hideShortcutHelp(); });
  return el;
}

function toggleShortcutHelp() {
  const el = ensureShortcutHelp();
  el.classList.toggle('hidden');
}
function hideShortcutHelp() {
  const el = document.getElementById('shortcut-help-overlay');
  if (el) el.classList.add('hidden');
}
function shortcutHelpOpen() {
  const el = document.getElementById('shortcut-help-overlay');
  return el && !el.classList.contains('hidden');
}

// ---- Digital signing (PKCS#12, in-browser, OpenSSL-free) ----

let signP12Bytes = null;   // Uint8Array of the uploaded .p12/.pfx
let signCloseTimer = 0;    // setTimeout id for auto-close after a successful sign

function clearSignCloseTimer() {
  if (signCloseTimer) { clearTimeout(signCloseTimer); signCloseTimer = 0; }
}
function scheduleSignClose(ms) {
  clearSignCloseTimer();
  signCloseTimer = setTimeout(() => { signCloseTimer = 0; closeSign(); }, ms);
}

function ensureSignOverlay() {
  let el = document.getElementById('sign-overlay');
  if (el) return el;
  el = document.createElement('div');
  el.id = 'sign-overlay';
  el.className = 'organize-overlay hidden';
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-modal', 'true');
  el.setAttribute('aria-label', 'Sign PDF');
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
        <form id="sign-password-form" autocomplete="off" style="margin:0;">
          <input type="text" name="sign-username" autocomplete="username" tabindex="-1" aria-hidden="true" style="position:absolute;left:-9999px;width:1px;height:1px;opacity:0;" />
          <label style="display:flex;flex-direction:column;gap:4px;font-size:13px;color:#444;">
            Certificate password
            <input type="password" id="sign-pass" placeholder="PKCS#12 password" autocomplete="new-password" />
          </label>
        </form>
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
    if (signP12Bytes) signP12Bytes.fill(0);
    if (!f) { signP12Bytes = null; return; }
    signP12Bytes = new Uint8Array(await f.arrayBuffer());
    setSignStatus(`Loaded ${f.name} (${signP12Bytes.length} bytes)`, '#2a7');
  });
  el.querySelector('#sign-go').addEventListener('click', doSign);
  el.querySelector('#sign-password-form').addEventListener('submit', (e) => {
    e.preventDefault();
    doSign();
  });
  el.querySelector('#sign-tsa').addEventListener('change', (e) => {
    el.querySelector('#sign-tsa-url-row').style.display = e.target.checked ? 'flex' : 'none';
  });
  return el;
}

function setSignStatus(msg, color) {
  const s = document.getElementById('sign-status');
  if (s) { s.textContent = msg; s.style.color = color || '#666'; }
}

// Track the trigger element so we can restore focus when an overlay closes.
let signTrigger = null;

function openSign() {
  if (!Module || !currentPdfBytes) return;
  if (!Module._nanopdf_sign_pdf) {
    setStatus('Sign requires rebuilding the nanopdf WASM module', true);
    return;
  }
  clearSignCloseTimer();
  clearSignSensitiveFields();
  const el = ensureSignOverlay();
  el.querySelector('#sign-p12').value = '';
  el.querySelector('#sign-pass').value = '';
  setSignStatus('');
  el.classList.remove('hidden');
  signTrigger = document.activeElement;
  // Focus the first focusable control so keyboard users land inside the dialog.
  const first = el.querySelector('input, select, textarea, button');
  if (first) first.focus();
}

function closeSign() {
  clearSignCloseTimer();
  const el = document.getElementById('sign-overlay');
  if (el) el.classList.add('hidden');
  clearSignSensitiveFields();
  if (signTrigger && typeof signTrigger.focus === 'function') {
    signTrigger.focus();
    signTrigger = null;
  }
}

function finishSign(suffix) {
  const outPtr = Module._nanopdf_sign_get_buffer();
  const outLen = Module._nanopdf_sign_get_size();
  if (!outPtr || outLen <= 0) {
    throw new Error(wasmLastError() || 'Sign produced an empty buffer');
  }
  const signed = new Uint8Array(Module.HEAPU8.buffer, outPtr, outLen).slice();
  const baseName = fileName.replace(/\.pdf$/i, '');
  downloadNamedPdf(signed, `${baseName}_${suffix}.pdf`);
  return outLen;
}

function clearSignSensitiveFields() {
  if (signP12Bytes) signP12Bytes.fill(0);
  signP12Bytes = null;
  const p12 = document.getElementById('sign-p12');
  const pass = document.getElementById('sign-pass');
  if (p12) p12.value = '';
  if (pass) pass.value = '';
}

function secureRandomU32() {
  const a = new Uint32Array(1);
  globalThis.crypto.getRandomValues(a);
  return a[0] >>> 0;
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
  const lastErr = () => readWasmString(Module, Module._nanopdf_get_last_error());

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
      const nLo = secureRandomU32();
      const nHi = secureRandomU32();
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
        const allowFallback = window.confirm(
          `Timestamp request failed (${netErr.message}). Sign without a trusted timestamp?`);
        if (!allowFallback) {
          setSignStatus('Signing canceled after timestamp failure.', '#c80');
          return;
        }
        setSignStatus('Signing without timestamp…', '#c80');
        const okPlain = Module._nanopdf_sign_pdf(
          pdfPtr, currentPdfBytes.length, p12Ptr, signP12Bytes.length, passPtr,
          reasonPtr, locPtr, contactPtr, currentPage, x, y, w, h, visible);
        if (!okPlain) { setSignStatus('Sign failed: ' + lastErr(), '#c33'); return; }
        const n = finishSign('signed');
        setSignStatus(`Signed without timestamp (${n} bytes). Downloaded.`, '#c80');
        scheduleSignClose(1400);
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
      scheduleSignClose(900);
      return;
    }

    // No timestamp: one-shot signing.
    const ok = Module._nanopdf_sign_pdf(
      pdfPtr, currentPdfBytes.length, p12Ptr, signP12Bytes.length, passPtr,
      reasonPtr, locPtr, contactPtr, currentPage, x, y, w, h, visible);
    if (!ok) { setSignStatus('Sign failed: ' + lastErr(), '#c33'); return; }
    const n = finishSign('signed');
    setSignStatus(`Signed (${n} bytes). Downloaded.`, '#2a7');
    scheduleSignClose(900);
  } catch (e) {
    setSignStatus('Sign error: ' + e, '#c33');
  } finally {
    freeAll();
    clearSignSensitiveFields();
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
  const result = renderPageIntoImageData(Module, pageIdx, w, h, 72 * (w / pw));
  if (!result.ok || !result.imageData) return false;
  const rw = result.w;
  const rh = result.h;
  canvas.width = rw; canvas.height = rh;
  const ctx = canvas.getContext('2d');
  ctx.putImageData(result.imageData, 0, 0);
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
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-modal', 'true');
  el.setAttribute('aria-label', 'Organize pages');
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

let organizeTrigger = null;

function openOrganize() {
  if (!Module || totalPages <= 0 || !hasRendering) return;
  organizeModel = [];
  for (let i = 0; i < totalPages; i++) organizeModel.push({ src: i, rot: 0 });
  organizeThumbCache = {};
  const el = ensureOrganizeOverlay();
  organizeTrigger = document.activeElement;
  el.classList.remove('hidden');
  renderOrganizeGrid();
}

function closeOrganize() {
  const el = document.getElementById('organize-overlay');
  if (el) el.classList.add('hidden');
  if (organizeTrigger && typeof organizeTrigger.focus === 'function') {
    organizeTrigger.focus();
    organizeTrigger = null;
  }
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
  if (docId < 0) throw new Error(wasmLastError() || 'Failed to load doc');
  try {
    Module._nanopdf_work_clear();
    for (const e of model) {
      const wi = Module._nanopdf_work_add_page(docId, e.src);
      if (wi < 0) throw new Error(wasmLastError() || 'Failed to add page');
      if (e.rot % 360 !== 0) Module._nanopdf_work_rotate_page(wi, ((e.rot % 360) + 360) % 360);
    }
    if (Module._nanopdf_export_pdf() !== 1) {
      throw new Error(wasmLastError() || 'Export failed');
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
      if (ok !== 1) throw new Error(wasmLastError() || 'Reorder failed');
      out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    } else {
      // Rotation present: route through the work/export path (flattened).
      out = await exportOrganizedViaWork(organizeModel);
    }
    if (!out) throw new Error('Empty output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_organized.pdf');
    setOperationStatus(`Saved reorganized PDF (${organizeModel.length} page(s))`, 'success');
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

  html += '<div class="thumbnail-list" role="listbox" aria-label="Pages">';
  for (let i = 0; i < totalPages; i++) {
    const classes = [];
    if (i === currentPage) classes.push('active');
    if (selectedPages.has(i)) classes.push('selected');
    const cls = classes.length > 0 ? ' ' + classes.join(' ') : '';
    html += `<div class="thumbnail-item${cls}" data-page="${i}" role="option" tabindex="0" ` +
      `aria-selected="${i === currentPage}" aria-label="Page ${i + 1}">`;
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

  // Keyboard navigation: focus a thumbnail (Tab into the list) and use
  // Up/Down/Home/End to move; Enter/Space is a click-equivalent; Spacebar
  // toggles selection.
  const list = sidebarContent.querySelector('.thumbnail-list');
  if (list) {
    list.addEventListener('keydown', (e) => {
      const target = e.target.closest('.thumbnail-item');
      if (!target) return;
      const cur = parseInt(target.dataset.page, 10);
      let next = cur;
      if (e.key === 'ArrowDown' || e.key === 'ArrowRight') next = Math.min(totalPages - 1, cur + 1);
      else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') next = Math.max(0, cur - 1);
      else if (e.key === 'Home') next = 0;
      else if (e.key === 'End') next = totalPages - 1;
      else if (e.key === ' ' || e.key === 'Spacebar') {
        e.preventDefault();
        if (selectedPages.has(cur)) selectedPages.delete(cur);
        else selectedPages.add(cur);
        lastClickedPage = cur;
        updateSelectionUI();
        return;
      } else if (e.key === 'Enter') {
        e.preventDefault();
        selectedPages.clear();
        lastClickedPage = cur;
        updateSelectionUI();
        goToPage(cur);
        return;
      } else return;
      e.preventDefault();
      if (next !== cur) {
        goToPage(next);
        const items = list.querySelectorAll('.thumbnail-item');
        const item = items[next];
        if (item) item.focus();
      }
    });
  }

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
    // Extract keeps vector content but ignores annotations. If any selected
    // page has annotations/form-fill to bake, suggest Export instead.
    const hasBakeableOnSelection = hasSelection &&
      [...selectedPages].some(pageHasBakeableContent);
    extractPagesBtn.disabled = !(hasSelection && Module && Module._nanopdf_split_pages) || hasBakeableOnSelection;
    extractPagesBtn.textContent = selectedPages.size > 0 ? `Extract (${selectedPages.size})` : 'Extract';
    extractPagesBtn.title = hasBakeableOnSelection
      ? 'Extract keeps vector content but ignores annotations — use Export to bake them in'
      : 'Extract selected pages into a new PDF (vector content preserved, annotations not baked in)';
  }
  if (organizeBtn) organizeBtn.disabled = !docLoaded;
  if (signBtn) signBtn.disabled = !(docLoaded && Module && Module._nanopdf_sign_pdf);
  { const pb = document.getElementById('print-btn'); if (pb) pb.disabled = !(docLoaded && hasRendering); }
  { const ps = document.getElementById('print-scope'); if (ps) ps.disabled = !(docLoaded && hasRendering); }

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
  if (thumbnailRenderQueue.length === 0) {
    // First item in a fresh queue: snapshot the main-render counter so we
    // can tell at drain-time whether the main canvas was clobbered.
    thumbnailQueueStartedBeforeMainRender = mainRenderCounter;
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

    const result = renderPageIntoImageData(Module, pageIdx, width, height, dpi);
    if (result.ok && result.imageData) {
      const renderWidth = result.w;
      const renderHeight = result.h;
      // Draw at display size by scaling
      canvasEl.width = renderWidth;
      canvasEl.height = renderHeight;
      const ctx = canvasEl.getContext('2d');
      ctx.putImageData(result.imageData, 0, 0);
      thumbnailCache[pageIdx] = result.imageData;
    }
  } catch (e) {
    console.warn('Thumbnail render failed for page', pageIdx, e);
  }

  isThumbnailRendering = false;

  if (thumbnailRenderQueue.length > 0) {
    // Use rAF to let the browser paint between renders; the 50 ms backstop
    // gives WASM memory time to settle on the first iteration.
    setTimeout(processThumbnailQueue, 50);
  } else {
    // All visible thumbnails done; only re-render the main page if it was
    // rendered *before* the queue started (i.e. it was clobbered by our work).
    if (thumbnailQueueStartedBeforeMainRender) {
      setTimeout(renderCurrentPage, 100);
    }
  }
}

function updateThumbnailHighlight() {
  if (activeSidebarTab !== 'thumbs') return;
  const items = sidebarContent.querySelectorAll('.thumbnail-item');
  items.forEach(item => {
    const page = parseInt(item.dataset.page, 10);
    const isActive = page === currentPage;
    item.classList.toggle('active', isActive);
    item.classList.toggle('selected', selectedPages.has(page));
    item.setAttribute('aria-selected', isActive ? 'true' : 'false');
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
  } else if (activeSidebarTab === 'marks') {
    renderMarksTab();
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

// ---- Marks tab (annotation list / index) ----
let marksFilter = '';

function renderMarksTab() {
  if (!Module || totalPages <= 0) {
    sidebarContent.innerHTML = '<div style="padding:12px;color:#999;font-size:13px;">No document loaded.</div>';
    return;
  }
  const q = marksFilter.trim().toLowerCase();
  // Collect all annotations across pages, sorted by page then by id (creation order).
  const all = [];
  for (let p = 0; p < totalPages; p++) {
    for (const a of (annotations[p] || [])) {
      all.push({ a, page: p });
    }
  }
  all.sort((x, y) => x.page - y.page || x.a.id - y.a.id);

  let html = '<div class="outline-filter">';
  html += '<input type="text" id="marks-filter-input" placeholder="Filter annotations…" ' +
          `value="${escapeHtml(marksFilter)}" autocomplete="off" />`;
  if (q) {
    html += `<button type="button" id="marks-filter-clear" title="Clear filter">&times;</button>`;
  }
  html += '</div>';

  const filtered = q
    ? all.filter(({ a, page }) =>
        annotLabel(a).toLowerCase().includes(q) ||
        a.type.toLowerCase().includes(q) ||
        ('p.' + (page + 1)).includes(q))
    : all;

  if (!filtered.length) {
    html += '<div class="outline-empty">' +
      (all.length
        ? `No matches for &ldquo;${escapeHtml(marksFilter)}&rdquo;.`
        : 'No annotations yet — draw one with the toolbar tools.') +
      '</div>';
    sidebarContent.innerHTML = html;
    wireMarksFilter();
    return;
  }

  html += '<div class="marks-list">';
  let lastPage = -1;
  for (const { a, page } of filtered) {
    if (page !== lastPage) {
      if (lastPage !== -1) html += '</div>';
      html += `<div class="marks-page-group">`;
      html += `<div class="marks-page-header">Page ${page + 1}</div>`;
      lastPage = page;
    }
    const isSelected = a.id === selectedAnnotId && page === currentPage;
    const isCurrentPage = page === currentPage;
    html += `<div class="marks-row${isSelected ? ' selected' : ''}${isCurrentPage ? '' : ' offpage'}" data-annot-id="${a.id}" data-page="${page}">`;
    html += `<span class="marks-type marks-type-${escapeHtml(a.type)}">${escapeHtml(annotLabel(a))}</span>`;
    html += `<button class="marks-del" data-annot-id="${a.id}" data-page="${page}" title="Delete">&times;</button>`;
    html += '</div>';
  }
  html += '</div></div>';

  sidebarContent.innerHTML = html;
  wireMarksFilter();
  sidebarContent.querySelectorAll('.marks-row').forEach((row) => {
    row.addEventListener('click', (e) => {
      if (e.target.classList.contains('marks-del')) return;
      const pid = parseInt(row.dataset.page, 10);
      const aid = parseInt(row.dataset.annotId, 10);
      if (pid !== currentPage) goToPage(pid);
      // Selection in the list also selects the annotation so the user can
      // immediately move/resize/delete via the toolbar.
      selectedAnnotId = aid;
      hoveredAnnotId = null;
      setAnnotTool('select');
      renderAnnotations();
      renderMarksTab();
    });
  });
  sidebarContent.querySelectorAll('.marks-del').forEach((btn) => {
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      const pid = parseInt(btn.dataset.page, 10);
      const aid = parseInt(btn.dataset.annotId, 10);
      const list = annotations[pid] || [];
      const idx = list.findIndex((a) => a.id === aid);
      if (idx < 0) return;
      pushUndo();
      list.splice(idx, 1);
      if (aid === selectedAnnotId && pid === currentPage) {
        selectedAnnotId = null;
        hoveredAnnotId = null;
        renderAnnotations();
        updateAnnotLayerMode();
      }
      updateSaveAnnotState();
      renderMarksTab();
    });
  });
}

function wireMarksFilter() {
  const input = document.getElementById('marks-filter-input');
  if (input) {
    input.addEventListener('input', (e) => {
      marksFilter = e.target.value;
      renderMarksTab();
      const el = document.getElementById('marks-filter-input');
      if (el) {
        el.focus();
        const v = el.value;
        el.setSelectionRange(v.length, v.length);
      }
    });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { marksFilter = ''; renderMarksTab(); }
    });
  }
  const clear = document.getElementById('marks-filter-clear');
  if (clear) clear.addEventListener('click', () => { marksFilter = ''; renderMarksTab(); });
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
    const index = Number.isInteger(a.index) ? a.index : -1;
    html += `<div class="attach-item">
      <div class="attach-name" title="${escapeHtml(a.name)}">${escapeHtml(a.name || '(unnamed)')}</div>
      <div class="attach-meta">${escapeHtml(meta)}${kb}</div>
      ${a.description ? `<div class="attach-desc">${escapeHtml(a.description)}</div>` : ''}
      <button class="attach-dl" data-index="${index}" data-name="${escapeHtml(a.name || 'attachment')}">Download</button>
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
      wasmLastError(), true);
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
  scrollSnapLocked = true;
  lastCanvasScrollTop = canvasScroll.scrollTop || 0;
  setTimeout(() => { scrollSnapLocked = false; }, 220);
  currentPage = pageIndex;
  selectedAnnotId = null;
  hoveredAnnotId = null;
  annotDraft = null;
  hideMarkupPopover();
  updatePageDisplay();
  renderCurrentPage();
  clearSearch();
  updateThumbnailHighlight();
  updateBookmarkButton();
  saveViewState();
}

function canvasScrollSnapThreshold() {
  const viewHeight = canvasScroll.clientHeight || 1;
  const visibleDocHeight = pageDisplayHeight > 0 ? pageDisplayHeight : viewHeight;
  return Math.max(0, Math.round(Math.min(viewHeight, visibleDocHeight) * 0.5));
}

function canvasScrollSlack() {
  return canvasScrollSnapThreshold();
}

function canvasScrollPageJumpThreshold() {
  const viewHeight = canvasScroll.clientHeight || 1;
  const visibleDocHeight = pageDisplayHeight > 0 ? pageDisplayHeight : viewHeight;
  return Math.max(0, Math.round(Math.min(viewHeight, visibleDocHeight) * 0.15));
}

function updateCanvasScrollSlack() {
  canvasScroll.style.setProperty('--canvas-scroll-slack', `${canvasScrollSlack()}px`);
}

function scrollTopForElementTop(el) {
  const scrollRect = canvasScroll.getBoundingClientRect();
  const elRect = el.getBoundingClientRect();
  return canvasScroll.scrollTop + (elRect.top - scrollRect.top);
}

function placeCanvasScroll(mode) {
  if (!mode) return;
  requestAnimationFrame(() => {
    updateCanvasScrollSlack();
    const threshold = canvasScrollSlack();
    const currentTop = scrollTopForElementTop(canvasWrapper);
    const maxTop = Math.max(0, canvasScroll.scrollHeight - canvasScroll.clientHeight);
    const maxLeft = Math.max(0, canvasScroll.scrollWidth - canvasScroll.clientWidth);
    let top = currentTop;
    if (mode === 'top') {
      top = currentTop - threshold;
    } else if (mode === 'bottom') {
      top = currentTop + threshold;
    }
    top = Math.max(0, Math.min(top, maxTop));
    canvasScroll.scrollTo({
      top,
      left: Math.min(canvasScroll.scrollLeft, maxLeft),
      behavior: 'auto',
    });
    lastCanvasScrollTop = top;
  });
}

function goToPageWithScroll(pageIndex, placement = 'top') {
  pendingPageScrollPlacement = placement;
  goToPage(pageIndex);
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
      const raw = input.value.trim();
      const n = Number(raw);
      if (!raw || !Number.isFinite(n) || n < 1 || n > totalPages || Math.floor(n) !== n) {
        // Flash the input border red so the user knows the value was rejected.
        input.classList.add('invalid');
        setStatus(`Page must be a number between 1 and ${totalPages}`, true);
        return;
      }
      exitPageJump();
      goToPage(n - 1);
    } else if (e.key === 'Escape') {
      exitPageJump();
    }
    e.stopPropagation();
  });
  input.addEventListener('input', () => {
    if (input.classList.contains('invalid')) input.classList.remove('invalid');
  });
  input.addEventListener('blur', () => {
    // Defer the exit so the invalid-flash class survives long enough to be seen
    // if the user pressed Enter (which also blurs the input).
    setTimeout(() => {
      if (isPageJumpMode) exitPageJump();
    }, 0);
  });
  pageDisplay.textContent = '';
  pageDisplay.appendChild(input);
  input.select();
}

// ---- User bookmarks ----
function findBookmarkAt(page) {
  return userBookmarks.find((b) => b.page === page);
}
function addBookmarkAtCurrent() {
  if (!Module || totalPages <= 0) return;
  if (findBookmarkAt(currentPage)) {
    setStatus('Page already bookmarked', 'info');
    return;
  }
  userBookmarks.push({ id: Date.now() + Math.random(), page: currentPage, title: `Page ${currentPage + 1}` });
  userBookmarks.sort((a, b) => a.page - b.page);
  updateBookmarkButton();
  if (activeSidebarTab === 'outline') renderOutlineTab();
  setStatus(`Bookmarked page ${currentPage + 1}`, 'success');
}
function removeBookmarkAt(page) {
  const idx = userBookmarks.findIndex((b) => b.page === page);
  if (idx < 0) return;
  userBookmarks.splice(idx, 1);
  updateBookmarkButton();
  if (activeSidebarTab === 'outline') renderOutlineTab();
}
function updateBookmarkButton() {
  if (!bookmarkBtn) return;
  const here = findBookmarkAt(currentPage);
  bookmarkBtn.classList.toggle('active', !!here);
  bookmarkBtn.title = here
    ? `Remove bookmark from page ${currentPage + 1} (B)`
    : 'Bookmark this page (B)';
}

function exitPageJump() {
  isPageJumpMode = false;
  updatePageDisplay();
}

// ---- Zoom ----

// Clamp to the slider's range (25%–400%) and snap to integer percent so the
// slider position stays in sync with the actual zoom value.
function setZoom(level, opts = {}) {
  const clamped = Math.max(0.25, Math.min(4.0, level));
  zoomLevel = clamped;
  updateZoomDisplay();
  if (opts.render !== false) {
    renderCurrentPage();
  }
  if (opts.persist !== false) {
    saveViewState();
  }
}

function zoomIn() {
  const nextStep = ZOOM_STEPS.find(s => s > zoomLevel + 0.001);
  if (nextStep) {
    setZoom(nextStep);
  } else {
    setZoom(zoomLevel + 0.25);
  }
}

function zoomOut() {
  const prevSteps = ZOOM_STEPS.filter(s => s < zoomLevel - 0.001);
  if (prevSteps.length > 0) {
    setZoom(prevSteps[prevSteps.length - 1]);
  } else {
    setZoom(zoomLevel - 0.25);
  }
}

function resetZoom() {
  zoomLevel = 1.0;
  updateZoomDisplay();
  renderCurrentPage();
  saveViewState();
}

function setRenderBackendIfAvailable(backendId) {
  if (!Module || !hasRendering || !Module._nanopdf_set_render_backend) return false;
  const available = backendId === BACKEND_LIGHTVG
    ? renderBackends.lightvg
    : renderBackends.thorvg;
  if (!available) return false;
  if (Module._nanopdf_set_render_backend(backendId) !== 1) return false;
  renderBackend = backendId;
  return true;
}

// ---- Rotation ----

function rotateClockwise() {
  rotation = (rotation + 90) % 360;
  applyRotation();
}

function applyRotation() {
  canvas.style.transform = rotation === 0 ? '' : `rotate(${rotation}deg)`;
  textOverlay.style.transform = canvas.style.transform;
  if (prevPageCanvas) prevPageCanvas.style.transform = canvas.style.transform;
  if (nextPageCanvas) nextPageCanvas.style.transform = canvas.style.transform;

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
  saveViewState();
}

// ---- Rendering ----

function computeBaseScale(pageWidth, pageHeight) {
  const sidebarWidth = sidebar.classList.contains('hidden') ? 0 : 260;
  const availWidth = window.innerWidth - sidebarWidth - 80; // 80 for padding/scrollbar
  const availHeight = window.innerHeight - 120; // toolbar + statusbar + padding
  const deviceMemory = Number(navigator.deviceMemory || 0);
  const fitWidthLimit = deviceMemory > 0 && deviceMemory <= 2
    ? 1200
    : deviceMemory > 0 && deviceMemory <= 4
      ? 1800
      : 3000;

  if (fitMode === 'width') {
    return Math.min(availWidth, fitWidthLimit) / pageWidth;
  } else {
    // Fit page: entire page visible
    const scaleW = availWidth / pageWidth;
    const scaleH = availHeight / pageHeight;
    return Math.min(scaleW, scaleH);
  }
}

function maxViewerRenderDimension(preview = false) {
  if (preview) return 1536;
  const deviceMemory = Number(navigator.deviceMemory || 0);
  if (deviceMemory > 0 && deviceMemory <= 2) return 2560;
  if (deviceMemory > 0 && deviceMemory <= 4) return 3072;
  return 4096;
}

function computePageRenderLayout(pageIndex, options = {}) {
  const preview = options.preview === true;
  const pageWidth = Module._nanopdf_get_page_width(pageIndex);
  const pageHeight = Module._nanopdf_get_page_height(pageIndex);
  const baseScale = computeBaseScale(pageWidth, pageHeight);
  const cssScale = baseScale * zoomLevel;
  const renderScale = preview ? 0.5 : 1.0;
  const cssWidth = Math.floor(pageWidth * cssScale);
  const cssHeight = Math.floor(pageHeight * cssScale);
  const dpr = window.devicePixelRatio || 1;
  const maxDim = Number.isFinite(options.maxDim) && options.maxDim > 0
    ? options.maxDim
    : maxViewerRenderDimension(preview);
  const renderSize = computeRenderSize(pageWidth, pageHeight, cssScale * renderScale * dpr, maxDim);
  return { pageWidth, pageHeight, cssWidth, cssHeight, ...renderSize };
}

function renderViewerPageToCanvas(pageIndex, targetCanvas, options = {}) {
  if (!Module || pageIndex < 0 || pageIndex >= totalPages || !targetCanvas) {
    return null;
  }

  const layout = computePageRenderLayout(pageIndex, options);
  const dpi = 72 * (layout.width / layout.pageWidth);
  // options.budgetMs (>0): cap this render's wall-clock so a fast scroll doesn't
  // block on one heavy page. On a budget interruption keep the existing bitmap;
  // the idle scheduleFullResolutionRender re-renders the page without a budget.
  const budgetMs = Number.isFinite(options.budgetMs) && options.budgetMs > 0 ? options.budgetMs : 0;
  const result = renderPageIntoCanvas(Module, pageIndex, layout.width, layout.height, dpi, targetCanvas, budgetMs);
  if (!result.ok) {
    if (result.interrupted) return { ok: false, interrupted: true };
    return { ok: false, error: result.error || 'unknown' };
  }

  const renderWidth = result.w;
  const renderHeight = result.h;
  targetCanvas.style.width = `${layout.cssWidth}px`;
  targetCanvas.style.height = `${layout.cssHeight}px`;
  return {
    ok: true,
    ...layout,
    width: renderWidth,
    height: renderHeight,
    renderMs: result.renderMs,
    paintMs: result.paintMs,
    totalMs: result.totalMs,
  };
}

function mainRenderKey(pageIndex) {
  const sidebarState = sidebar.classList.contains('hidden') ? 'hidden' : 'open';
  const dpr = window.devicePixelRatio || 1;
  return [
    renderDocumentVersion,
    pageIndex,
    renderBackend,
    fitMode,
    Math.round(zoomLevel * 1000),
    Math.round(dpr * 100),
    window.innerWidth,
    window.innerHeight,
    sidebarState,
  ].join(':');
}

function scheduleFullResolutionRender(key, pageIndex) {
  const jobId = ++fullRenderJobId;
  setTimeout(() => {
    if (jobId !== fullRenderJobId) return;
    if (pageIndex !== currentPage || key !== mainRenderKey(currentPage)) return;
    renderCurrentPage({ forceFull: true, expectedKey: key });
  }, 32);
}

function cancelScheduledAdjacentPreviewRender() {
  adjacentPreviewJobId++;
  if (!adjacentPreviewTimer) return;
  if (adjacentPreviewTimerKind === 'idle' && typeof window.cancelIdleCallback === 'function') {
    window.cancelIdleCallback(adjacentPreviewTimer);
  } else {
    clearTimeout(adjacentPreviewTimer);
  }
  adjacentPreviewTimer = 0;
  adjacentPreviewTimerKind = '';
}

function schedulePreviewWork(callback) {
  if (typeof window.requestIdleCallback === 'function') {
    adjacentPreviewTimerKind = 'idle';
    adjacentPreviewTimer = window.requestIdleCallback(callback, { timeout: 120 });
  } else {
    adjacentPreviewTimerKind = 'timeout';
    adjacentPreviewTimer = setTimeout(callback, 16);
  }
}

function adjacentPreviewKey(pageIndex) {
  const sidebarState = sidebar.classList.contains('hidden') ? 'hidden' : 'open';
  const dpr = window.devicePixelRatio || 1;
  return [
    renderDocumentVersion,
    pageIndex,
    renderBackend,
    fitMode,
    Math.round(zoomLevel * 1000),
    Math.round(dpr * 100),
    window.innerWidth,
    window.innerHeight,
    sidebarState,
  ].join(':');
}

function setPreviewVisible(wrapper, visible) {
  if (!wrapper) return;
  wrapper.style.display = visible ? 'inline-flex' : 'none';
  if (!visible) {
    const canvas = wrapper.querySelector('canvas');
    if (canvas) {
      canvas.width = 1;
      canvas.height = 1;
    }
  }
}

function prepareAdjacentPreviewSlot(slot, pageIndex, targetCanvas, wrapper) {
  if (!targetCanvas || !wrapper || pageIndex < 0 || pageIndex >= totalPages) {
    adjacentPreviewRenderKeys[slot] = '';
    setPreviewVisible(wrapper, false);
    return null;
  }

  const key = adjacentPreviewKey(pageIndex);
  const layout = computePageRenderLayout(pageIndex, { preview: true });
  targetCanvas.style.width = `${layout.cssWidth}px`;
  targetCanvas.style.height = `${layout.cssHeight}px`;
  setPreviewVisible(wrapper, true);

  if (adjacentPreviewRenderKeys[slot] !== key) {
    targetCanvas.width = 1;
    targetCanvas.height = 1;
    adjacentPreviewRenderKeys[slot] = '';
  }
  return key;
}

function renderAdjacentPreviewSlot(slot, pageIndex, targetCanvas, wrapper, key, jobId) {
  if (jobId !== adjacentPreviewJobId || !key) return;
  if (adjacentPreviewRenderKeys[slot] === key && targetCanvas.width > 1 && targetCanvas.height > 1) {
    return;
  }

  const result = renderViewerPageToCanvas(pageIndex, targetCanvas, { preview: true, budgetMs: ADJACENT_PREVIEW_BUDGET_MS });
  if (jobId !== adjacentPreviewJobId) return;
  if (result && result.ok) {
    adjacentPreviewRenderKeys[slot] = key;
    setPreviewVisible(wrapper, true);
  } else {
    // Includes budget interruption: leave the slot unkeyed so it re-renders on
    // the next settle; keep the previous preview hidden rather than erroring.
    adjacentPreviewRenderKeys[slot] = '';
    setPreviewVisible(wrapper, false);
  }
}

function renderAdjacentPagePreviews() {
  cancelScheduledAdjacentPreviewRender();

  if (!hasRendering || !Module || totalPages <= 0) {
    adjacentPreviewRenderKeys.prev = '';
    adjacentPreviewRenderKeys.next = '';
    setPreviewVisible(prevPageWrapper, false);
    setPreviewVisible(nextPageWrapper, false);
    return;
  }

  const prevPage = currentPage - 1;
  const nextPage = currentPage + 1;
  const prevKey = prepareAdjacentPreviewSlot('prev', prevPage, prevPageCanvas, prevPageWrapper);
  const nextKey = prepareAdjacentPreviewSlot('next', nextPage, nextPageCanvas, nextPageWrapper);

  const transform = rotation === 0 ? '' : `rotate(${rotation}deg)`;
  if (prevPageCanvas) prevPageCanvas.style.transform = transform;
  if (nextPageCanvas) nextPageCanvas.style.transform = transform;

  const jobId = adjacentPreviewJobId;
  schedulePreviewWork(() => {
    adjacentPreviewTimer = 0;
    adjacentPreviewTimerKind = '';
    renderAdjacentPreviewSlot('prev', prevPage, prevPageCanvas, prevPageWrapper, prevKey, jobId);
    schedulePreviewWork(() => {
      adjacentPreviewTimer = 0;
      adjacentPreviewTimerKind = '';
      renderAdjacentPreviewSlot('next', nextPage, nextPageCanvas, nextPageWrapper, nextKey, jobId);
    });
  });
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

// True when (clientX, clientY) lies within the page-canvas rectangle (so a
// drag-and-release inside the canvas keeps the annotation; release outside
// the canvas is the "drag-out to delete" cue).
function isPointInPageCanvas(clientX, clientY) {
  if (!canvas) return false;
  const r = canvas.getBoundingClientRect();
  return clientX >= r.left && clientX <= r.right &&
         clientY >= r.top  && clientY <= r.bottom;
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
  const oldPanel = annotHtml.querySelector('.annot-props');
  if (oldPanel) oldPanel.remove();

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
    // Resize handles on the selected shape (in select mode only).
    if (annotTool === 'select' && annotEditingEnabled()) {
      drawResizeHandles(sel);
      drawPropsPanel(sel);
    }
  }
  // Hover outline (only in select mode, only when not the selected one).
  const hov = hoveredAnnotId != null ? findAnnot(hoveredAnnotId) : null;
  if (hov && (!sel || hov.id !== sel.id)) {
    const bb = annotScreenBBox(hov);
    const r = svgEl('rect');
    r.setAttribute('x', bb.left - 2); r.setAttribute('y', bb.top - 2);
    r.setAttribute('width', Math.max(0, bb.width) + 4);
    r.setAttribute('height', Math.max(0, bb.height) + 4);
    r.setAttribute('fill', 'none');
    r.setAttribute('stroke', '#007bff');
    r.setAttribute('stroke-width', '1');
    r.setAttribute('stroke-opacity', '0.45');
    annotSvg.appendChild(r);
  }
  updateAnnotLayerMode();
  // Keep the Marks tab in sync with selection / live edits.
  if (activeSidebarTab === 'marks' && sidebarContent) {
    const rows = sidebarContent.querySelectorAll('.marks-row');
    if (rows.length) {
      rows.forEach((r) => {
        const pid = parseInt(r.dataset.page, 10);
        const aid = parseInt(r.dataset.annotId, 10);
        r.classList.toggle('selected', aid === selectedAnnotId && pid === currentPage);
      });
    }
  }
}

function drawAnnot(a) {
  const scale = getPageScale();
  const col = rgbToHex(a.color);
  if (a.type === 'text') { drawTextBox(a); return; }
  if (a.type === 'image') { drawImageStamp(a); return; }

  let el;
  if (a.type === 'link') {
    // Translucent blue tint + dashed border; cursor hints it is clickable.
    el = svgEl('rect');
    const box = pdfBoxToScreen(a);
    el.setAttribute('x', box.left); el.setAttribute('y', box.top);
    el.setAttribute('width', Math.max(0, box.width));
    el.setAttribute('height', Math.max(0, box.height));
    el.setAttribute('fill', col);
    el.setAttribute('fill-opacity', a.alpha != null ? a.alpha : 0.18);
    el.setAttribute('stroke', col);
    el.setAttribute('stroke-width', '1.5');
    el.setAttribute('stroke-dasharray', '5 3');
    el.classList.add('annot-link');
    // Tooltip shows the destination; status bar surfaces it on hover.
    const dp = a.destPage;
    el.setAttribute('data-link-dest', String(dp));
    el.appendChild(svgEl('title')).textContent =
      dp != null ? `Link to page ${dp + 1}` : 'Link (no destination)';
  } else if (a.type === 'redact') {
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
    const da = dashToStrokeDasharray(a.dash);
    for (const s of segs) {
      const ln = svgEl('line');
      ln.setAttribute('x1', s.x1); ln.setAttribute('y1', s.y1);
      ln.setAttribute('x2', s.x2); ln.setAttribute('y2', s.y2);
      ln.setAttribute('stroke', col);
      ln.setAttribute('stroke-width', Math.max(1, a.lineWidth * scale));
      ln.setAttribute('stroke-linecap', 'round');
      if (da) ln.setAttribute('stroke-dasharray', da);
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
    const da = dashToStrokeDasharray(a.dash);
    if (da) el.setAttribute('stroke-dasharray', da);
  }
  if (!el) return;
  el.classList.add('annot-shape');
  el.dataset.annotId = a.id;
  annotSvg.appendChild(el);
}

// ---- Resize handles ----
// Handles consume pointer events before the underlying shape does. For bbox-
// based annotations we draw 8 handles (4 corners + 4 edges). For line/arrow
// we draw 2 endpoint handles (circles at x1,y1 and x2,y2). For ink we use the
// same 8-handle layout and scale all points proportionally to the new bbox.
const HANDLE = 8;        // half-size in CSS px
const HANDLE_CORNER = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];

function handleCursor(name) {
  // 8-direction cursor mapping.
  if (name === 'nw' || name === 'se') return 'nwse-resize';
  if (name === 'ne' || name === 'sw') return 'nesw-resize';
  if (name === 'n' || name === 's') return 'ns-resize';
  if (name === 'e' || name === 'w') return 'ew-resize';
  if (name === 'start' || name === 'end') return 'move';
  return 'default';
}

function drawResizeHandles(a) {
  const bb = annotScreenBBox(a);
  if (a.type === 'line' || a.type === 'arrow') {
    // Two endpoint handles, drawn as small circles.
    const p1 = pdfToScreen(a.x1, a.y1), p2 = pdfToScreen(a.x2, a.y2);
    drawHandle(a, p1.x, p1.y, 'start');
    drawHandle(a, p2.x, p2.y, 'end');
    return;
  }
  // Bbox-based: 8 handles around the selection bbox.
  const x = bb.left, y = bb.top, w = bb.width, h = bb.height;
  const midX = x + w / 2, midY = y + h / 2;
  const x2 = x + w, y2 = y + h;
  const positions = {
    nw: [x, y], n: [midX, y], ne: [x2, y],
    e: [x2, midY], se: [x2, y2], s: [midX, y2],
    sw: [x, y2], w: [x, midY],
  };
  for (const name of HANDLE_CORNER) {
    const [px, py] = positions[name];
    drawHandle(a, px, py, name);
  }
}

// ---- Properties panel ----
// A small floating panel anchored to the bottom-right of the selected
// annotation, exposing color / line-width / font-size / delete controls.
// Only relevant in select mode on a non-image annotation.
function drawPropsPanel(a) {
  if (!annotHtml) return;
  if (a.type === 'image') return;   // image annots have no editable style here
  const bb = annotScreenBBox(a);
  // The panel may have 1 or 2 rows; the height below is the *upper bound* used
  // to decide whether to flip up.
  const PANEL_W = 196, PANEL_H = 64;
  let left = Math.min(Math.max(0, bb.left + bb.width - PANEL_W), pageDisplayWidth - PANEL_W);
  let top = bb.top + bb.height + 6;
  if (top + PANEL_H > pageDisplayHeight) top = Math.max(0, bb.top - PANEL_H - 6);
  if (top < 0) top = 4;

  const panel = document.createElement('div');
  panel.className = 'annot-props';
  panel.style.left = left + 'px';
  panel.style.top = top + 'px';

  // First row: color, width / size, dash, dest.
  const row1 = document.createElement('div');
  row1.className = 'annot-props-row';

  // Color swatch.
  const colorWrap = document.createElement('label');
  colorWrap.className = 'annot-props-field';
  colorWrap.title = 'Color';
  const colorInput = document.createElement('input');
  colorInput.type = 'color';
  colorInput.value = rgbToHex(a.color || { r: 1, g: 1, b: 1 });
  colorInput.addEventListener('input', () => {
    pushUndo();
    a.color = hexToRgb(colorInput.value);
    renderAnnotations();
  });
  colorInput.addEventListener('change', () => updateSaveAnnotState());
  const colorSpan = document.createElement('span');
  colorSpan.textContent = 'Color';
  colorWrap.append(colorInput, colorSpan);
  row1.appendChild(colorWrap);

  // Line width (rect/oval/line/arrow/ink/highlight/redact).
  if (a.type !== 'text') {
    const lwWrap = document.createElement('label');
    lwWrap.className = 'annot-props-field';
    lwWrap.title = 'Stroke width';
    const lwInput = document.createElement('input');
    lwInput.type = 'number';
    lwInput.min = '0.5';
    lwInput.max = '20';
    lwInput.step = '0.5';
    lwInput.value = String(a.lineWidth != null ? a.lineWidth : 2);
    lwInput.addEventListener('change', () => {
      const n = Number(lwInput.value);
      if (!Number.isFinite(n) || n < 0.5 || n > 20) { lwInput.value = String(a.lineWidth || 2); return; }
      pushUndo();
      a.lineWidth = n;
      renderAnnotations();
      updateSaveAnnotState();
    });
    const lwSpan = document.createElement('span');
    lwSpan.textContent = 'Width';
    lwWrap.append(lwInput, lwSpan);
    row1.appendChild(lwWrap);
  }

  // Font size (text only).
  if (a.type === 'text') {
    const fsWrap = document.createElement('label');
    fsWrap.className = 'annot-props-field';
    fsWrap.title = 'Font size (pt)';
    const fsInput = document.createElement('input');
    fsInput.type = 'number';
    fsInput.min = '6';
    fsInput.max = '144';
    fsInput.step = '1';
    fsInput.value = String(a.fontSize || 14);
    fsInput.addEventListener('change', () => {
      const n = Number(fsInput.value);
      if (!Number.isFinite(n) || n < 6 || n > 144) { fsInput.value = String(a.fontSize || 14); return; }
      pushUndo();
      a.fontSize = n;
      renderAnnotations();
      updateSaveAnnotState();
    });
    const fsSpan = document.createElement('span');
    fsSpan.textContent = 'Size';
    fsWrap.append(fsInput, fsSpan);
    row1.appendChild(fsWrap);
  }

  // Dash style (rect/oval/line/arrow/ink — not text/image/redact/link).
  if (['rect', 'oval', 'line', 'arrow', 'ink'].includes(a.type)) {
    const dashWrap = document.createElement('label');
    dashWrap.className = 'annot-props-field';
    dashWrap.title = 'Stroke style';
    const dashSel = document.createElement('select');
    [['solid', '—  solid'], ['dashed', '- - dashed'], ['dotted', '· · dotted']]
      .forEach(([v, label]) => {
        const o = document.createElement('option');
        o.value = v; o.textContent = label;
        if ((a.dash || 'solid') === v) o.selected = true;
        dashSel.appendChild(o);
      });
    dashSel.addEventListener('change', () => {
      pushUndo();
      a.dash = dashSel.value;
      renderAnnotations();
      updateSaveAnnotState();
    });
    dashWrap.appendChild(dashSel);
    row1.appendChild(dashWrap);
  }

  // Link destination page.
  if (a.type === 'link') {
    const destWrap = document.createElement('label');
    destWrap.className = 'annot-props-field';
    destWrap.title = 'Destination page';
    const destInput = document.createElement('input');
    destInput.type = 'number';
    destInput.min = '1';
    destInput.max = String(Math.max(1, totalPages));
    destInput.step = '1';
    destInput.value = String((a.destPage != null ? a.destPage : 0) + 1);
    destInput.addEventListener('change', () => {
      const n = parseInt(destInput.value, 10);
      if (!Number.isFinite(n) || n < 1 || n > totalPages) {
        destInput.value = String((a.destPage != null ? a.destPage : 0) + 1);
        return;
      }
      pushUndo();
      a.destPage = n - 1;
      renderAnnotations();
      updateSaveAnnotState();
    });
    const destSpan = document.createElement('span');
    destSpan.textContent = 'Page';
    destWrap.append(destInput, destSpan);
    row1.appendChild(destWrap);
  }

  panel.appendChild(row1);

  // Second row: opacity slider, delete button.
  const row2 = document.createElement('div');
  row2.className = 'annot-props-row';

  // Opacity slider (highlight, link, or any annotation).
  if (a.type !== 'image' && a.type !== 'ink') {
    const opWrap = document.createElement('label');
    opWrap.className = 'annot-props-field annot-props-opacity';
    opWrap.title = 'Opacity';
    const opInput = document.createElement('input');
    opInput.type = 'range';
    opInput.min = '0';
    opInput.max = '100';
    opInput.step = '1';
    opInput.value = String(Math.round((a.alpha != null ? a.alpha : 1) * 100));
    opInput.style.flex = '1';
    opInput.addEventListener('input', () => {
      a.alpha = clamp(Number(opInput.value), 0, 100) / 100;
      renderAnnotations();
    });
    opInput.addEventListener('change', () => {
      pushUndo();
      updateSaveAnnotState();
    });
    const opSpan = document.createElement('span');
    opSpan.textContent = `${opInput.value}%`;
    opInput.addEventListener('input', () => { opSpan.textContent = `${opInput.value}%`; });
    opWrap.append(opInput, opSpan);
    row2.appendChild(opWrap);
  }

  // Delete button.
  const delBtn = document.createElement('button');
  delBtn.type = 'button';
  delBtn.className = 'annot-props-del';
  delBtn.textContent = '✕';
  delBtn.title = 'Delete (Del)';
  delBtn.addEventListener('click', (ev) => {
    ev.stopPropagation();
    deleteSelectedAnnot();
  });
  delBtn.addEventListener('pointerdown', (ev) => ev.stopPropagation());
  row2.appendChild(delBtn);

  panel.appendChild(row2);

  // Prevent panel clicks from deselecting the annotation.
  panel.addEventListener('pointerdown', (ev) => ev.stopPropagation());
  annotHtml.appendChild(panel);
}

function drawHandle(a, px, py, name) {
  const h = svgEl('rect');
  h.setAttribute('x', px - HANDLE);
  h.setAttribute('y', py - HANDLE);
  h.setAttribute('width', HANDLE * 2);
  h.setAttribute('height', HANDLE * 2);
  h.setAttribute('fill', '#fff');
  h.setAttribute('stroke', '#007bff');
  h.setAttribute('stroke-width', '1.5');
  h.classList.add('annot-handle');
  h.dataset.annotId = a.id;
  h.dataset.handle = name;
  h.style.cursor = handleCursor(name);
  annotSvg.appendChild(h);
}

// Snapshot of the annotation geometry needed to compute the post-resize state.
// Storing this in annotResizeState means we don't have to read live values
// during a drag (which would compound round-off errors).
function captureResizeState(a) {
  if (a.type === 'line' || a.type === 'arrow') {
    return { type: a.type, x1: a.x1, y1: a.y1, x2: a.x2, y2: a.y2 };
  }
  if (a.type === 'ink') {
    return { type: a.type, points: a.points.map((p) => ({ x: p.x, y: p.y })) };
  }
  return { type: a.type, x: a.x, y: a.y, w: a.w, h: a.h };
}

function applyResize(a, pre, handle, dx, dy) {
  // dx, dy are in PDF points.
  if (a.type === 'line' || a.type === 'arrow') {
    if (handle === 'start') {
      a.x1 = pre.x1 + dx; a.y1 = pre.y1 + dy;
    } else if (handle === 'end') {
      a.x2 = pre.x2 + dx; a.y2 = pre.y2 + dy;
    }
    return;
  }
  if (a.type === 'ink') {
    // Scale all points proportionally to the new bbox.
    let minx = Infinity, miny = Infinity, maxx = -Infinity, maxy = -Infinity;
    for (const p of pre.points) {
      if (p.x < minx) minx = p.x; if (p.x > maxx) maxx = p.x;
      if (p.y < miny) miny = p.y; if (p.y > maxy) maxy = p.y;
    }
    const preW = maxx - minx || 1;
    const preH = maxy - miny || 1;
    let nx = pre.x, ny = pre.y, nw = pre.w, nh = pre.h;
    if (handle.includes('e')) nw = pre.w + dx;
    if (handle.includes('w')) { nx = pre.x + dx; nw = pre.w - dx; }
    if (handle.includes('s')) nh = pre.h + dy;
    if (handle.includes('n')) { ny = pre.y + dy; nh = pre.h - dy; }
    const sx = nw / preW, sy = nh / preH;
    a.points = pre.points.map((p) => ({
      x: nx + (p.x - minx) * sx,
      y: ny + (p.y - miny) * sy,
    }));
    a.x = nx; a.y = ny; a.w = nw; a.h = nh;
    return;
  }
  // Bbox-based: standard 8-handle resize.
  let nx = pre.x, ny = pre.y, nw = pre.w, nh = pre.h;
  if (handle.includes('e')) nw = pre.w + dx;
  if (handle.includes('w')) { nx = pre.x + dx; nw = pre.w - dx; }
  if (handle.includes('s')) nh = pre.h + dy;
  if (handle.includes('n')) { ny = pre.y + dy; nh = pre.h - dy; }
  // Clamp to a minimum size so an annotation can't be resized below 2 pt.
  if (nw < 2) { nw = 2; if (handle.includes('w')) nx = pre.x + pre.w - 2; }
  if (nh < 2) { nh = 2; if (handle.includes('n')) ny = pre.y + pre.h - 2; }
  a.x = nx; a.y = ny; a.w = nw; a.h = nh;
}

function annotHandlePointerDown(e, a, handle) {
  if (!annotEditingEnabled() || annotTool !== 'select') return;
  e.stopPropagation();
  e.preventDefault();
  const pre = captureResizeState(a);
  let moved = false;
  let last = annotPointerPos(e);
  const onMove = (ev) => {
    const pos = annotPointerPos(ev);
    const dx = (pos.x - last.x) / getPageScale();
    const dy = -(pos.y - last.y) / getPageScale();
    if (dx !== 0 || dy !== 0) moved = true;
    applyResize(a, pre, handle, dx, dy);
    last = pos;
    renderAnnotations();
  };
  const onUp = () => {
    window.removeEventListener('pointermove', onMove);
    window.removeEventListener('pointerup', onUp);
    if (moved) {
      // Push the pre-resize snapshot to the undo stack manually (we already
      // mutated the annotation in place).
      undoStack.push(snapshotState());
      if (undoStack.length > MAX_UNDO) undoStack.shift();
      redoStack.length = 0;
      updateUndoRedoButtons();
    }
    updateSaveAnnotState();
  };
  window.addEventListener('pointermove', onMove);
  window.addEventListener('pointerup', onUp);
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
  const da = dashToStrokeDasharray(a.dash);
  if (da && !a.filled) el.setAttribute('stroke-dasharray', da);
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
    dash: 'solid',
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
  pushUndo();
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
        wasmLastError(), true);
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
    pushUndo();
    delete d._start;
    delete d._aspect;
    if (d.type === 'link') {
      // Prompt for the destination page. Default to the current page + 1.
      const defaultPage = Math.min(totalPages, currentPage + 2);
      const raw = window.prompt(
        `Link destination page (1–${totalPages}, or 0 to cancel):`,
        String(defaultPage),
      );
      if (raw === null) { renderAnnotations(); return; }
      const n = parseInt(raw, 10);
      if (!Number.isFinite(n) || n < 1 || n > totalPages) {
        setStatus('Link cancelled (invalid page)', 'info');
        renderAnnotations();
        return;
      }
      d.destPage = n - 1;
      d.alpha = 0.18;        // light tint so the underlying text shows through
    }
    pageAnnots().push(d);
    selectedAnnotId = d.id;
    if (d.type === 'link') setAnnotTool('select');
  }
  renderAnnotations();
  updateSaveAnnotState();
}

function annotShapePointerDown(e) {
  if (!annotEditingEnabled() || annotTool !== 'select') return;
  // Handles take priority over the underlying shape.
  const handle = e.target.closest('.annot-handle');
  if (handle) {
    const a = findAnnot(Number(handle.dataset.annotId));
    if (a) annotHandlePointerDown(e, a, handle.dataset.handle);
    return;
  }
  const host = e.target.closest('[data-annot-id]');
  if (!host) return;
  e.stopPropagation();
  e.preventDefault();
  // Single click on a link navigates to its destination (no drag, no modifier).
  // Drag to move; modifier to enter the resize handles flow.
  const hostAnnot = findAnnot(Number(host.dataset.annotId));
  if (hostAnnot && hostAnnot.type === 'link' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
    if (Number.isInteger(hostAnnot.destPage) && hostAnnot.destPage >= 0 &&
        hostAnnot.destPage < totalPages) {
      goToPage(hostAnnot.destPage);
      setStatus(`Link → page ${hostAnnot.destPage + 1}`, 'info');
      return;
    }
  }
  selectedAnnotId = Number(host.dataset.annotId);
  const a = findAnnot(selectedAnnotId);
  renderAnnotations();
  if (!a || a.type === 'text') return;  // text boxes move via their own element
  // Capture the pre-drag snapshot; we'll commit to the undo stack on pointerup
  // only if the annotation actually moved (so a no-op click doesn't pollute
  // the history).
  const preDrag = snapshotState();
  let moved = false;
  let last = annotPointerPos(e);
  const onMove = (ev) => {
    const pos = annotPointerPos(ev);
    const dx = (pos.x - last.x) / getPageScale();
    const dy = -(pos.y - last.y) / getPageScale();
    if (dx !== 0 || dy !== 0) moved = true;
    translateAnnot(a, dx, dy);
    last = pos;
    renderAnnotations();
  };
  const onUp = (ev) => {
    window.removeEventListener('pointermove', onMove);
    window.removeEventListener('pointerup', onUp);
    // Drag-out-to-delete: if the user released outside the page bounds and
    // we actually moved, delete the annotation. The pre-drag snapshot is
    // pushed so Undo restores it.
    const overCanvas = isPointInPageCanvas(ev.clientX, ev.clientY);
    if (moved && !overCanvas) {
      // Don't push the (preDrag) state — we'll add a fresh snapshot of the
      // current state minus this annotation, so Undo brings it back. We do
      // it now while the annotation is still in the list.
      undoStack.push(snapshotState());
      if (undoStack.length > MAX_UNDO) undoStack.shift();
      const list = pageAnnots();
      const idx = list.findIndex((x) => x.id === a.id);
      if (idx >= 0) list.splice(idx, 1);
      selectedAnnotId = null;
      hoveredAnnotId = null;
      renderAnnotations();
      updateAnnotLayerMode();
      updateSaveAnnotState();
      setStatus('Annotation deleted (drag-out)', 'info');
      return;
    }
    if (moved) {
      undoStack.push(preDrag);
      if (undoStack.length > MAX_UNDO) undoStack.shift();
      redoStack.length = 0;
      updateUndoRedoButtons();
    }
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
    pushUndo();
    list.splice(i, 1);
    selectedAnnotId = null;
    hoveredAnnotId = null;
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
        required: !!f.required,
        options: Array.isArray(f.options) ? f.options : [],
        fontSize: 12,
      });
    }
  }
}

// Rendered form-field controls are cached per page so that re-renders driven
// by zoom/rotation only reposition existing inputs (no DOM teardown). The
// cache is keyed by page index; on page change we rebuild.
let renderedFormFieldsPage = -1;
let renderedFormFieldsKey = '';

function buildFormFieldControl(fld) {
  let input;
  if (fld.kind === 'checkbox') {
    input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = fld.checked;
    // For checkboxes we can treat each change as one undo step.
    input.addEventListener('change', () => {
      pushUndo();
      fld.checked = input.checked;
      updateSaveAnnotState();
    });
  } else if (fld.kind === 'choice') {
    input = document.createElement('select');
    for (const opt of fld.options) {
      const o = document.createElement('option');
      o.value = opt; o.textContent = opt;
      if (opt === fld.value) o.selected = true;
      input.appendChild(o);
    }
    input.addEventListener('change', () => {
      pushUndo();
      fld.value = input.value;
      updateSaveAnnotState();
    });
  } else {
    input = document.createElement(fld.multiline ? 'textarea' : 'input');
    if (!fld.multiline) input.type = fld.password ? 'password' : 'text';
    input.value = fld.value;
    // For text inputs we capture one undo snapshot per focus session; typing
    // any number of characters collapses into a single undo step.
    input.addEventListener('focus', () => {
      formFieldEditSnapshot = snapshotState();
    });
    input.addEventListener('input', () => { fld.value = input.value; updateSaveAnnotState(); });
    input.addEventListener('blur', () => {
      // Only push if the value actually changed during this focus session.
      if (formFieldEditSnapshot) {
        const after = snapshotState();
        if (JSON.stringify(formFieldEditSnapshot.formFields) !== JSON.stringify(after.formFields)) {
          undoStack.push(formFieldEditSnapshot);
          if (undoStack.length > MAX_UNDO) undoStack.shift();
          redoStack.length = 0;
          updateUndoRedoButtons();
        }
        formFieldEditSnapshot = null;
      }
    });
  }
  input.className = 'annot-formfield';
  input.title = fld.name + (fld.required ? ' (required)' : '');
  if (fld.required) {
    input.classList.add('annot-formfield-required');
    input.setAttribute('aria-required', 'true');
    // Placeholder hint makes the requirement visible even before the user
    // focuses the field. Use a non-intrusive marker.
    if (input.tagName === 'INPUT' || input.tagName === 'TEXTAREA') {
      if (!input.placeholder) input.placeholder = '(required)';
      else if (!input.placeholder.includes('required')) input.placeholder += ' *';
    }
  }
  return input;
}

function positionFormField(input, fld) {
  const box = pdfBoxToScreen({ x: fld.rect.x, y: fld.rect.y, w: fld.rect.width, h: fld.rect.height });
  input.style.left = box.left + 'px';
  input.style.top = box.top + 'px';
  input.style.width = Math.max(8, box.width) + 'px';
  input.style.height = Math.max(12, box.height) + 'px';
  if (fld.kind === 'text' || fld.kind === 'choice') {
    input.style.fontSize = Math.max(8, Math.min(box.height * 0.7, 18)) + 'px';
  }
}

function clearFormFieldControls() {
  annotHtml.querySelectorAll('.annot-formfield').forEach((n) => n.remove());
  renderedFormFieldsPage = -1;
  renderedFormFieldsKey = '';
}

function renderFormFields() {
  if (!Module || totalPages <= 0 || canvas.style.display !== 'block') return;
  if (rotation !== 0) {
    // Rotated pages can't show overlay controls; just drop the cache.
    if (renderedFormFieldsPage !== -1) clearFormFieldControls();
    return;
  }
  const fields = formFieldsByPage[currentPage];
  if (!fields || !fields.length) {
    if (renderedFormFieldsPage !== -1) clearFormFieldControls();
    return;
  }
  // Build a stable key from field set (name + kind + rect) so a value-only
  // edit reuses the DOM but a structural change (different fields shown)
  // triggers a rebuild.
  const key = fields.map((f) =>
    `${f.name}|${f.kind}|${f.rect.x},${f.rect.y},${f.rect.width},${f.rect.height}|${f.multiline ? 1 : 0}|${f.password ? 1 : 0}|${(f.options || []).join('\u0001')}`
  ).join('\n');
  if (renderedFormFieldsPage !== currentPage || renderedFormFieldsKey !== key) {
    clearFormFieldControls();
    for (const fld of fields) {
      annotHtml.appendChild(buildFormFieldControl(fld));
    }
    renderedFormFieldsPage = currentPage;
    renderedFormFieldsKey = key;
  }
  // Reposition on every call (zoom / rotation 0).
  const inputs = annotHtml.querySelectorAll('.annot-formfield');
  for (let i = 0; i < fields.length; i++) positionFormField(inputs[i], fields[i]);
}

// Save filled form fields as a real editable PDF via incremental update.
async function saveEditableForm() {
  if (!Module || !Module._nanopdf_form_load || !currentPdfBytes || !currentPdfBytes.length) {
    setStatus('Editable form save not available', true);
    return;
  }
  showLoading('Saving filled form...');
  await new Promise((r) => setTimeout(r, 30));
  // Track every WASM allocation we make so a throw from any WASM call
  // (e.g. form_save failing) still frees them in the finally block.
  const alloced = [];
  const freeAll = () => { for (const p of alloced) if (p) Module._free(p); };
  try {
    const ptr = Module._nanopdf_malloc(currentPdfBytes.length);
    Module.HEAPU8.set(currentPdfBytes, ptr);
    const ok = Module._nanopdf_form_load(ptr, currentPdfBytes.length);
    Module._nanopdf_free(ptr);
    if (ok !== 1) {
      throw new Error(wasmLastError() || 'Failed to load form');
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
        alloced.push(namePtr);
        if (f.kind === 'text') {
          const vp = Module.stringToNewUTF8(f.value || '');
          alloced.push(vp);
          if (Module._nanopdf_form_set_text(namePtr, vp)) count++;
        } else if (f.kind === 'checkbox') {
          if (Module._nanopdf_form_set_checkbox(namePtr, f.checked ? 1 : 0)) count++;
        } else if (f.kind === 'choice') {
          const vp = Module.stringToNewUTF8(f.value || '');
          alloced.push(vp);
          if (Module._nanopdf_form_set_choice(namePtr, vp)) count++;
        }
      }
    }
    if (Module._nanopdf_form_save() !== 1) {
      throw new Error(wasmLastError() || 'Form save failed');
    }
    const out = copyWasmBuffer(Module._nanopdf_form_get_buffer, Module._nanopdf_form_get_size);
    if (!out) throw new Error('Empty form output');
    downloadNamedPdf(out, fileName.replace(/\.pdf$/i, '') + '_filled.pdf');
    setStatus(`Saved editable filled form (${count} field(s))`, 'success');
  } catch (err) {
    setStatus('Form save error: ' + err.message, true);
    console.error(err);
  } finally {
    freeAll();
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
  // Marks tab is the canonical list; rebuild it when the doc set changes.
  if (activeSidebarTab === 'marks' && sidebarContent) renderMarksTab();
}

function resetAnnotations() {
  annotations = {};
  formFieldsByPage = {};
  selectedAnnotId = null;
  hoveredAnnotId = null;
  annotDraft = null;
  pendingStampImage = null;
  annotIdCounter = 1;
  setAnnotTool('select');
  if (annotHtml) clearChildren(annotHtml);
  if (annotSvg) clearChildren(annotSvg);
  renderedFormFieldsPage = -1;
  renderedFormFieldsKey = '';
}

// Wall-clock budgets (ms) for the low-res renders that run on the scroll
// critical path. A heavy page is interrupted past its budget and finished by
// the idle full-resolution pass, so fast scrolling never blocks on one page.
const COLD_PREVIEW_BUDGET_MS = 80;
const ADJACENT_PREVIEW_BUDGET_MS = 60;

function renderCurrentPage(options = {}) {
  if (!Module || totalPages <= 0) return;
  if (!hasRendering) {
    setStatus(`Page ${currentPage + 1} / ${totalPages} (rendering not available)`);
    if (activeSidebarTab === 'info') renderInfoTab();
    return;
  }

  // Don't render main page while thumbnails are being rendered
  if (isThumbnailRendering) return;

  const fullKey = mainRenderKey(currentPage);
  if (options.expectedKey && options.expectedKey !== fullKey) return;
  const useColdPreview = options.forceFull !== true && !fullRenderKeys.has(fullKey);
  // The low-res -> full-res upgrade (forceFull) re-renders the same page in
  // place; it must not move the viewport. Capture the scroll position so a
  // canvas resize during the upgrade can't snap us back to the page top. Skip
  // this when a deliberate (re)placement is pending.
  const preserveScrollTop =
    (options.forceFull === true && pendingSeamlessOffset === null &&
     pendingPageScrollPlacement === null)
      ? canvasScroll.scrollTop
      : null;
  const renderOptions = useColdPreview
    ? { preview: true, maxDim: 960, budgetMs: COLD_PREVIEW_BUDGET_MS }
    : {};
  const result = renderViewerPageToCanvas(currentPage, canvas, renderOptions);
  if (result && result.interrupted) {
    // Heavy page hit its budget during a fast scroll: keep whatever is on the
    // canvas and finish the page off the critical path (the idle full-res pass
    // re-renders it unbudgeted once we settle here), so scrolling stays smooth.
    if (shouldRenderPageStatus(Date.now(), operationStatusHoldUntil)) {
      setStatus(`Page ${currentPage + 1} / ${totalPages} | rendering…`);
    }
    scheduleFullResolutionRender(fullKey, currentPage);
    return;
  }
  if (!result || !result.ok) {
    setStatus('Render error: ' + (result.error || 'unknown'), true);
    return;
  }
  if (!useColdPreview) {
    fullRenderKeys.add(fullKey);
  }

  const { pageWidth, pageHeight, cssWidth, cssHeight } = result;
  pageDisplayWidth = cssWidth;
  pageDisplayHeight = cssHeight;
  mainRenderCounter++;

  emptyState.style.display = 'none';
  canvas.style.display = 'block';
  updateCanvasScrollSlack();
  resizeTextOverlay();

  // Apply rotation
  applyRotation();
  renderTextOverlay();
  renderAnnotations();
  renderFormFields();
  if (useColdPreview) {
    cancelScheduledAdjacentPreviewRender();
  } else {
    renderAdjacentPagePreviews();
  }

  const zoomPct = Math.round(zoomLevel * 100);
  if (shouldRenderPageStatus(Date.now(), operationStatusHoldUntil)) {
    const rasterMs = Number.isFinite(result.renderMs) ? ` | raster ${result.renderMs.toFixed(1)} ms` : '';
    setStatus(`Page ${currentPage + 1} / ${totalPages} | ${backendLabel(renderBackend)} | ${pageWidth.toFixed(0)} x ${pageHeight.toFixed(0)} pts | ${zoomPct}%${rotation !== 0 ? ' | ' + rotation + '°' : ''}${rasterMs}`);
  }
  statusRight.textContent = fileName;

  if (activeSidebarTab === 'info') renderInfoTab();

  if (preserveScrollTop !== null) {
    // Hold the viewport still across the full-resolution upgrade.
    if (canvasScroll.scrollTop !== preserveScrollTop) {
      canvasScroll.scrollTop = preserveScrollTop;
    }
    lastCanvasScrollTop = canvasScroll.scrollTop;
    requestAnimationFrame(() => {
      if (canvasScroll.scrollTop !== preserveScrollTop) {
        canvasScroll.scrollTop = preserveScrollTop;
        lastCanvasScrollTop = preserveScrollTop;
      }
    });
  } else if (pendingSeamlessOffset !== null) {
    // Seamless wheel paging: position the just-promoted page at the same
    // viewport offset its preview occupied, so the content does not jump.
    const offset = pendingSeamlessOffset;
    pendingSeamlessOffset = null;
    // Two frames: the first lets the adjacent-page previews toggle visibility
    // (which changes the layout at the first/last page); the second measures the
    // settled position so the promoted page lands exactly where its preview was.
    requestAnimationFrame(() => requestAnimationFrame(() => {
      const docTop = scrollTopForElementTop(canvasWrapper);
      const maxTop = Math.max(0, canvasScroll.scrollHeight - canvasScroll.clientHeight);
      canvasScroll.scrollTop = Math.max(0, Math.min(docTop - offset, maxTop));
      // Re-anchor the scroll baseline so the next scroll delta is measured from
      // the settled position, then release the lock and start a short cooldown.
      // Without this the lock could expire mid-render and a stale delta would
      // fire a spurious opposite-direction promote, cascading back to page 1.
      lastCanvasScrollTop = canvasScroll.scrollTop;
      lastPromoteTime = Date.now();
      scrollSnapLocked = false;
    }));
  } else if (pendingPageScrollPlacement) {
    const placement = pendingPageScrollPlacement;
    pendingPageScrollPlacement = null;
    placeCanvasScroll(placement);
  }

  if (useColdPreview) {
    scheduleFullResolutionRender(fullKey, currentPage);
  }
}

// ---- Text Extraction ----

// Current content-export view: 'text' | 'md' | 'tables'.
let textPanelFormat = 'text';

function renderTextPanel() {
  if (!Module || totalPages <= 0) return;
  let body = '', title = 'Extracted Text';
  if (textPanelFormat === 'md') {
    title = 'Markdown';
    body = readWasmString(Module, Module._nanopdf_page_to_markdown(currentPage)) || '(No text extracted)';
  } else if (textPanelFormat === 'tables') {
    title = 'Tables (CSV)';
    body = Module._nanopdf_extract_tables
      ? readWasmString(Module, Module._nanopdf_extract_tables(currentPage, 0))
      : '';
    if (!body) body = '(No tables detected on this page)';
  } else {
    body = readWasmString(Module, Module._nanopdf_extract_text(currentPage)) || '(No text extracted)';
  }
  const titleEl = document.getElementById('text-panel-title');
  if (titleEl) titleEl.textContent = `${title} — page ${currentPage + 1}`;
  textContent.textContent = body;
  document.querySelectorAll('.text-fmt').forEach(b => b.classList.remove('active'));
  const active = { text: 'text-fmt-text', md: 'text-fmt-md', tables: 'text-fmt-tables' }[textPanelFormat];
  const ab = document.getElementById(active);
  if (ab) ab.classList.add('active');
  textPanel.style.display = 'block';
}

function extractText() {
  textPanelFormat = 'text';
  renderTextPanel();
}

function downloadTextPanel() {
  const ext = { text: 'txt', md: 'md', tables: 'csv' }[textPanelFormat] || 'txt';
  const mime = { text: 'text/plain', md: 'text/markdown', tables: 'text/csv' }[textPanelFormat] || 'text/plain';
  const baseName = fileName.replace(/\.pdf$/i, '');
  const blob = new Blob([textContent.textContent], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `${baseName}_p${currentPage + 1}.${ext}`;
  document.body.appendChild(a); a.click(); document.body.removeChild(a);
  URL.revokeObjectURL(url);
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
    ? Module._nanopdf_search_text(termPtr, pagesPtr, searchCaseSensitive ? 1 : 0, 0, -1)
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
  // The legacy _nanopdf_batch_find_text path is case-insensitive only;
  // filter on the client when the user opts into case-sensitive search so
  // the WASM-new and WASM-old build paths behave the same way.
  if (searchCaseSensitive && searchResults.length > 0) {
    const t = currentSearchTerm;
    searchResults = searchResults.filter((r) => {
      const text = (r.context || '').toLowerCase();
      // Cheap heuristic: if the lowercased term is the term itself the
      // filter is a no-op (we'd keep everything). Otherwise the term has
      // some uppercase letter, so check that *some* position in the page
      // text contains the exact-case term. We don't have page text here,
      // so fall back to dropping matches whose context doesn't contain
      // the term case-sensitively.
      if (t && (r.context || '').includes(t)) return true;
      return false;
    });
  }

  if (searchResults.length > 0) {
    currentSearchIndex = 0;
    const firstPage = searchResults[0].page;
    if (firstPage !== currentPage) {
      goToPage(firstPage);
    }
    updateSearchInfo();
    renderSearchPageChips();
    showSearchResultsDropdown();
    searchPrevBtn.disabled = false;
    searchNextBtn.disabled = false;
    renderTextOverlay();
  } else {
    searchInfo.textContent = 'No matches';
    renderSearchPageChips();
    hideSearchResultsDropdown();
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

// Compact page-jump strip under the search bar. One chip per page that
// has at least one match; the current page's chip is highlighted. Capped
// at 60 chips to keep the toolbar from overflowing on huge docs.
function renderSearchPageChips() {
  if (!searchPages) return;
  searchPages.replaceChildren();
  if (searchResults.length === 0) {
    searchPages.classList.add('hidden');
    return;
  }
  // Group results by page and count.
  const byPage = new Map();
  for (const r of searchResults) {
    byPage.set(r.page, (byPage.get(r.page) || 0) + 1);
  }
  const pages = [...byPage.keys()].sort((a, b) => a - b);
  const cap = 60;
  const visible = pages.length > cap ? pages.slice(0, cap) : pages;
  for (const p of visible) {
    const chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'search-page-chip';
    if (p === currentPage) chip.classList.add('current');
    chip.textContent = `${p + 1}${byPage.get(p) > 1 ? ` (${byPage.get(p)})` : ''}`;
    chip.title = `Page ${p + 1} — ${byPage.get(p)} match${byPage.get(p) === 1 ? '' : 'es'}`;
    chip.addEventListener('click', () => {
      // Jump to the first match on that page (preserve next/prev flow).
      const idx = searchResults.findIndex((r) => r.page === p);
      if (idx >= 0) {
        currentSearchIndex = idx;
        if (p !== currentPage) goToPage(p);
        updateSearchInfo();
        renderTextOverlay();
      }
    });
    searchPages.appendChild(chip);
  }
  if (pages.length > cap) {
    const more = document.createElement('span');
    more.className = 'search-page-more';
    more.textContent = `+${pages.length - cap} more`;
    searchPages.appendChild(more);
  }
  searchPages.classList.remove('hidden');
}

// Floating dropdown of the top N matches with surrounding context. Click a row
// to jump straight to that match. Hidden by default; only shown when the
// search input is focused or has results and the user is typing.
const searchResultsEl = document.getElementById('search-results');
const SEARCH_RESULTS_CAP = 12;

function showSearchResultsDropdown() {
  if (!searchResultsEl) return;
  if (searchResults.length === 0 || !currentSearchTerm) {
    searchResultsEl.classList.add('hidden');
    searchResultsEl.replaceChildren();
    return;
  }
  const cap = Math.min(SEARCH_RESULTS_CAP, searchResults.length);
  searchResultsEl.replaceChildren();
  for (let i = 0; i < cap; i++) {
    const r = searchResults[i];
    const row = document.createElement('div');
    row.className = 'search-result-row';
    row.setAttribute('role', 'option');
    row.dataset.idx = String(i);
    const pageLabel = document.createElement('span');
    pageLabel.className = 'search-result-page';
    pageLabel.textContent = `p.${r.page + 1}`;
    const ctx = document.createElement('span');
    ctx.className = 'search-result-ctx';
    ctx.innerHTML = highlightContext(r.context || '…', currentSearchTerm);
    row.append(pageLabel, ctx);
    row.addEventListener('click', () => {
      currentSearchIndex = i;
      if (r.page !== currentPage) goToPage(r.page);
      updateSearchInfo();
      renderSearchPageChips();
      renderTextOverlay();
      hideSearchResultsDropdown();
      searchInput.focus();
    });
    searchResultsEl.appendChild(row);
  }
  if (searchResults.length > cap) {
    const more = document.createElement('div');
    more.className = 'search-result-more';
    more.textContent = `+${searchResults.length - cap} more — use ↑ / ↓ / Enter`;
    searchResultsEl.appendChild(more);
  }
  searchResultsEl.classList.remove('hidden');
}
function hideSearchResultsDropdown() {
  if (!searchResultsEl) return;
  searchResultsEl.classList.add('hidden');
}

function nextSearchResult() {
  if (searchResults.length === 0) return;
  currentSearchIndex = (currentSearchIndex + 1) % searchResults.length;
  const result = searchResults[currentSearchIndex];
  if (result.page !== currentPage) {
    goToPage(result.page);
  }
  updateSearchInfo();
  renderSearchPageChips();
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
  renderSearchPageChips();
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
  pushUndo();
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

async function loadDeferredDocumentMetadata(docVersion, name) {
  if (!isCurrentDocumentVersion(docVersion)) return;

  clearRecovery().finally(() => {
    if (!isCurrentDocumentVersion(docVersion)) return;
    recoveryActiveName = name;
    scheduleRecoverySave();
  });

  loadFormFields();
  if (!isCurrentDocumentVersion(docVersion)) return;
  renderFormFields();
  updateSaveAnnotState();
  updateExportButton();

  loadOutline();
  if (!isCurrentDocumentVersion(docVersion)) return;

  loadSignatureInfo();
  if (!isCurrentDocumentVersion(docVersion)) return;

  const signatures = signatureData && Array.isArray(signatureData.signatures)
    ? signatureData.signatures
    : [];
  const history = await loadRevisionHistory(currentPdfBytes, signatures);
  if (!isCurrentDocumentVersion(docVersion)) return;
  editHistoryData = history;

  const hasOutline = outlineData && outlineData.outline && outlineData.outline.length > 0;
  const hasSignatures = signatures.length > 0;
  let sidebarOpened = false;

  if (hasOutline || hasSignatures) {
    sidebarOpened = sidebar.classList.contains('hidden');
    sidebar.classList.remove('hidden');
    if (hasSignatures && !hasOutline) {
      activeSidebarTab = 'signatures';
      document.querySelectorAll('.sidebar-tabs button').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === activeSidebarTab);
      });
    }
  }

  if (!sidebar.classList.contains('hidden')) {
    updateSidebar();
  }
  if (activeSidebarTab === 'info') {
    renderInfoTab();
  }
  if (sidebarOpened && hasRendering) {
    renderCurrentPage();
  }
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
      const error = wasmLastError();
      throw new Error(error || 'Failed to load PDF');
    }

    fileName = name;
    fileSize = bytes.length;
    currentPdfBytes = new Uint8Array(bytes);
    currentPage = 0;
    renderDocumentVersion++;
    const docVersion = renderDocumentVersion;
    cancelScheduledAdjacentPreviewRender();
    adjacentPreviewRenderKeys.prev = '';
    adjacentPreviewRenderKeys.next = '';
    setPreviewVisible(prevPageWrapper, false);
    setPreviewVisible(nextPageWrapper, false);
    fullRenderJobId++;
    fullRenderKeys.clear();
    outlineData = null;
    signatureData = null;
    editHistoryData = null;
    formFieldsByPage = {};
    clearFormFieldControls();
    if (sidebarContent) sidebarContent.textContent = '';

    // Reset view state, then apply persisted preferences.
    zoomLevel = 1.0;
    rotation = 0;
    fitMode = 'width';
    const savedView = loadViewState();
    if (typeof savedView.zoom === 'number' && savedView.zoom > 0.05 && savedView.zoom <= 8) {
      zoomLevel = savedView.zoom;
    }
    if (savedView.fitMode === 'width' || savedView.fitMode === 'page') {
      fitMode = savedView.fitMode;
    }
    // Restore the backend if the saved one is available.
    if (savedView.backend === BACKEND_LIGHTVG || savedView.backend === BACKEND_THORVG) {
      setRenderBackendIfAvailable(savedView.backend);
    }
    // Restore the last viewed page for this specific file.
    if (savedView.pages && Number.isInteger(savedView.pages[name])) {
      const p = savedView.pages[name];
      if (p >= 0 && p < totalPages) currentPage = p;
    }
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
    outlineFilter = '';
    marksFilter = '';
    userBookmarks = [];
    clearUndoHistory();
    currentSelection = null;
    selectionStartPoint = null;
    selectionDragBox = null;
    searchInput.value = '';
    searchInfo.textContent = '';
    updateZoomDisplay();
    updateExportButton();
    fitModeBtn.textContent = fitMode === 'width' ? 'Fit Width' : 'Fit Page';
    canvas.style.transform = '';
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';

    // Enable controls
    renderBtn.disabled = !hasRendering;
    updateBackendToggle();
    extractBtn.disabled = false;
    prevBtn.disabled = false;
    nextBtn.disabled = false;
    if (bookmarkBtn) bookmarkBtn.disabled = !(totalPages > 0);
    sidebarToggleBtn.disabled = false;
    searchInput.disabled = false;
    if (searchCaseToggle) searchCaseToggle.disabled = false;
    zoomInBtn.disabled = !hasRendering;
    zoomOutBtn.disabled = !hasRendering;
    if (zoomSlider) zoomSlider.disabled = !hasRendering;
    fitModeBtn.disabled = !hasRendering;
    rotateBtn.disabled = !hasRendering;
    updateExportButton();

    // Annotation tooling
    resetAnnotations();
    setAnnotControlsEnabled(hasRendering);
    updateSaveAnnotState();

    updatePageDisplay();

    // Render first page
    if (hasRendering) {
      pendingPageScrollPlacement = 'start';
      renderCurrentPage();
    } else {
      emptyState.style.display = 'none';
      canvas.style.display = 'none';
      setStatus(`Loaded ${name}: ${totalPages} page(s) (no rendering backend)`);
    }

    await afterNextPaint();
    if (!isCurrentDocumentVersion(docVersion)) return;
    hideLoading();
    setStatus(`Loaded ${name}: ${totalPages} page(s), ${formatFileSize(fileSize)}`);
    statusRight.textContent = name;
    scheduleDocumentIdleWork(() => {
      loadDeferredDocumentMetadata(docVersion, name).catch((err) => {
        if (!isCurrentDocumentVersion(docVersion)) return;
        console.error('Deferred document metadata failed:', err);
      });
    });

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
  // Links are viewer-only; they don't get baked into a flattened export.
  if (annotations[pageIndex] && annotations[pageIndex].some((a) => a.type !== 'link')) return true;
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
      throw new Error(wasmLastError() ||
        'Failed to load source document for export');
    }

    Module._nanopdf_work_clear();
    pages.forEach((pageIdx) => {
      const workIndex = Module._nanopdf_work_add_page(docId, pageIdx);
      if (workIndex < 0) {
        throw new Error(wasmLastError() ||
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
        throw new Error(wasmLastError() ||
          'Failed to configure password protection');
      }
    }

    const exportResult = Module._nanopdf_export_pdf();
    if (exportResult !== 1) {
      throw new Error(wasmLastError() || 'Export failed');
    }

    const outPtr = Module._nanopdf_export_get_buffer();
    const outSize = Module._nanopdf_export_get_size();
    const pdfBytes = new Uint8Array(Module.HEAPU8.buffer, outPtr, outSize).slice();
    downloadPdfBytes(pdfBytes, suffix || (protect ? 'protected' : 'annotated'), pages);
    setOperationStatus(`Exported ${pages.length} page(s)${protect ? ' (protected)' : ''}`, 'success');
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
      throw new Error(wasmLastError() || 'Merge failed');
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
      throw new Error(wasmLastError() || 'Extract failed');
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
      throw new Error(wasmLastError() || 'Failed to load for editing');
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
      throw new Error(wasmLastError() || 'Annotation save failed');
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
    await saveFlattenedPdf(
      pages,
      'redacted',
      protectExport.checked
        ? `Saved redacted PDF without password protection (${pages.length} page(s))`
        : null,
    );
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
  const { width, height } = computeRenderSize(pw, ph, scale, 4096);
  const dpi = 72 * (width / pw);
  const result = renderPageIntoImageData(Module, pageIdx, width, height, dpi);
  if (!result.ok || !result.imageData) return null;
  const rw = result.w;
  const rh = result.h;
  const cvs = document.createElement('canvas');
  cvs.width = rw; cvs.height = rh;
  const ctx = cvs.getContext('2d');
  ctx.putImageData(result.imageData, 0, 0);
  flattenAnnotationsToCanvas(ctx, pageIdx, rw / pw, ph, imgCache);  // burns redaction
  return buildPdf([{ jpegBytes: canvasToJpegBytes(cvs), imgWidth: rw, imgHeight: rh, pageWidth: pw, pageHeight: ph }]);
}

// Extract original pages as a vector PDF (Uint8Array), preserving requested order.
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
async function saveFlattenedPdf(pages, suffix, successMessage = null) {
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
      if (!b) throw new Error(`Could not extract vector pages ${vectorRun.map(p => p + 1).join(', ')}`);
      parts.push(b);
      vectorRun = [];
    };
    for (let idx = 0; idx < pages.length; idx++) {
      const p = pages[idx];
      loadingText.textContent = `Processing page ${idx + 1} / ${pages.length}...`;
      if (pageHasBakeableContent(p)) {
        flushVector();
        const img = flattenOnePageToImagePdf(p, imgCache);
        if (!img) throw new Error(`Could not render page ${p + 1}`);
        parts.push(img);
        flattened++;
      } else {
        vectorRun.push(p);
      }
      if (idx % 8 === 7) await new Promise((r) => setTimeout(r, 0));
    }
    flushVector();
    if (!parts.length) { setOperationStatus('Nothing to save', true); return false; }

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
        const ok = Module._nanopdf_merge_add_pdf(ptr, b.length);
        Module._nanopdf_free(ptr);
        if (ok !== 1) {
          throw new Error(wasmLastError() || 'Merge input failed');
        }
      }
      if (Module._nanopdf_merge_finish() !== 1) {
        throw new Error(wasmLastError() || 'Merge failed');
      }
      out = copyWasmBuffer(Module._nanopdf_merge_get_buffer, Module._nanopdf_merge_get_size);
    }
    if (!out) throw new Error('Empty output');
    downloadPdfBytes(out, suffix || 'flattened', pages);
    setOperationStatus(
      successMessage ||
        `Saved ${pages.length} page(s) — ${flattened} rasterized, ${pages.length - flattened} kept vector`,
      'success',
    );
    return true;
  } catch (err) {
    setOperationStatus('Save error: ' + err.message, true);
    console.error(err);
    return false;
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

  const hasRedaction = pages.some((p) => (annotations[p] || []).some((a) => a.type === 'redact'));
  const hasBakeable = pages.some(pageHasBakeableContent);
  const route = chooseSelectedExportRoute({
    hasRedaction,
    protect: protectExport.checked,
    hasBakeable,
    canSplit: !!Module._nanopdf_split_pages,
  });

  // Redaction on any selected page requires the secure client-side burn.
  if (route === 'burn-redaction') {
    await saveFlattenedPdf(
      pages,
      'redacted',
      protectExport.checked
        ? `Exported redacted PDF without password protection (${pages.length} page(s))`
        : null,
    );
    return;
  }

  // Route through the C++ flatten path when encryption is requested or any
  // selected page carries annotations / form-field fills, so they get baked in.
  if (route === 'wasm-flatten') {
    await exportViaWasm(pages, {
      protect: protectExport.checked,
      suffix: protectExport.checked ? 'protected_pages' : 'pages',
    });
    renderCurrentPage();
    return;
  }

  if (route === 'vector-split') {
    showLoading(`Exporting ${pages.length} page(s)...`);
    await new Promise(r => setTimeout(r, 30));
    try {
      const out = extractVectorRun(pages);
      if (!out) throw new Error(wasmLastError() || 'Vector export failed');
      downloadPdfBytes(out, 'pages', pages);
      setOperationStatus(`Exported ${pages.length} page(s) as vector PDF`, 'success');
    } catch (err) {
      setStatus('Export error: ' + err.message, true);
      console.error(err);
    } finally {
      hideLoading();
    }
    return;
  }

  setStatus('Export requires rebuilding the nanopdf WASM module', true);
  return;
}

// ---- Window Resize ----

let resizeDebounce = null;

function onResize() {
  clearTimeout(resizeDebounce);
  resizeDebounce = setTimeout(() => {
    updateCanvasScrollSlack();
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
const helpBtn = document.getElementById('help-btn');
if (helpBtn) helpBtn.addEventListener('click', toggleShortcutHelp);
const printBtn = document.getElementById('print-btn');
if (printBtn) printBtn.addEventListener('click', printDocument);
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
if (bookmarkBtn) {
  bookmarkBtn.addEventListener('click', () => {
    if (findBookmarkAt(currentPage)) removeBookmarkAt(currentPage);
    else addBookmarkAtCurrent();
  });
}

// Zoom buttons
zoomInBtn.addEventListener('click', zoomIn);
zoomOutBtn.addEventListener('click', zoomOut);
// Zoom slider: input updates the displayed percent as the user drags; the
// expensive re-render only fires on change (release), so dragging the
// thumb stays smooth even on huge pages.
if (zoomSlider) {
  zoomSlider.addEventListener('input', () => {
    zoomDisplay.textContent = zoomSlider.value + '%';
  });
  zoomSlider.addEventListener('change', () => {
    setZoom(Number(zoomSlider.value) / 100);
  });
  // Double-click resets to 100%.
  zoomSlider.addEventListener('dblclick', () => { setZoom(1.0); });
}

// Fit mode toggle
fitModeBtn.addEventListener('click', toggleFitMode);

// Rotate button
rotateBtn.addEventListener('click', rotateClockwise);

// Advance/retreat the current page by `dir` (+1/-1) without moving the content
// under the viewport. The adjacent-page preview that is about to become the main
// page is already on screen; we record its current viewport offset, promote it,
// and after the re-render restore the scroll so that same page sits at the same
// offset. Measuring the real positions (rather than assuming a fixed stride)
// keeps it seamless across the first/last page, where the prev/next preview
// appears or disappears and changes the layout. The net effect is wheel
// scrolling that flows continuously from page to page with no snap/jump.
function seamlessAdvancePage(dir) {
  if (dir > 0 && currentPage >= totalPages - 1) return;
  if (dir < 0 && currentPage <= 0) return;
  const incoming = dir > 0 ? nextPageWrapper : prevPageWrapper;
  if (!incoming) return;
  const scrollRect = canvasScroll.getBoundingClientRect();
  // Viewport offset of the page that is about to become the main page.
  const incomingOffset = incoming.getBoundingClientRect().top - scrollRect.top;
  scrollSnapLocked = true;
  pendingSeamlessOffset = incomingOffset;
  currentPage += dir;
  selectedAnnotId = null;
  hoveredAnnotId = null;
  annotDraft = null;
  hideMarkupPopover();
  updatePageDisplay();
  renderCurrentPage();
  clearSearch();
  updateThumbnailHighlight();
  updateBookmarkButton();
  saveViewState();
  // The lock is released only once the scroll has been re-anchored (in the
  // pendingSeamlessOffset handler below); this safety timeout just guarantees we
  // never get stuck locked if the render path bails before that point.
  setTimeout(() => { scrollSnapLocked = false; }, 1200);
}

function snapPageFromScroll() {
  if (!Module || totalPages <= 0 || !hasRendering || scrollSnapLocked) return;
  // Cooldown after a promotion: lets momentum settle before another page turn,
  // preventing a chain of promotes (which could otherwise run all the way to the
  // first/last page) during a single fast wheel gesture.
  if (Date.now() - lastPromoteTime < 250) {
    lastCanvasScrollTop = canvasScroll.scrollTop;
    return;
  }
  const pageTop = scrollTopForElementTop(canvasWrapper);
  const pageHeight =
    canvasWrapper.offsetHeight || pageDisplayHeight || canvasScroll.clientHeight || 1;
  const pageBottom = pageTop + pageHeight;
  const viewTop = canvasScroll.scrollTop;
  const viewHeight = canvasScroll.clientHeight || 1;
  const viewBottom = viewTop + viewHeight;
  const delta = viewTop - lastCanvasScrollTop;
  lastCanvasScrollTop = viewTop;
  if (Math.abs(delta) <= 2) return;  // ignore sub-pixel jitter

  // Hand off only after the current page is nearly out of view. Promoting at
  // the viewport centre feels like snapping to a page boundary with mouse-wheel
  // deltas because a large part of the old page is still visible.
  const handoffSlop = Math.max(24, Math.min(viewHeight, pageHeight) * 0.08);
  if (delta > 0 && currentPage < totalPages - 1 &&
      pageBottom <= viewTop + handoffSlop) {
    seamlessAdvancePage(+1);
  } else if (delta < 0 && currentPage > 0 &&
             pageTop >= viewBottom - handoffSlop) {
    seamlessAdvancePage(-1);
  }
}

function snapPageFromKeyboard(reverse = false) {
  if (!Module || totalPages <= 0 || !hasRendering) return;
  if (reverse) {
    if (currentPage > 0) goToPageWithScroll(currentPage - 1, 'bottom');
  } else if (currentPage < totalPages - 1) {
    goToPageWithScroll(currentPage + 1, 'top');
  }
}

// Ctrl+scroll wheel zoom. Plain wheel scrolling stays native (so adjacent page
// previews are visible), but once it reaches a page boundary the scroll handler
// below snaps to the next/previous page. Snapping is also explicit via
// Space / Shift+Space.
canvasScroll.addEventListener('wheel', (e) => {
  if (e.ctrlKey || e.metaKey) {
    e.preventDefault();
    if (e.deltaY < 0) {
      zoomIn();
    } else {
      zoomOut();
    }
    return;
  }
}, { passive: false });

// Advance to the next/previous page when native wheel/trackpad scrolling crosses
// a page boundary. Without this, scrolling past the end of a page never turns the
// page (snapPageFromScroll was defined but never wired to the scroll event).
canvasScroll.addEventListener('scroll', () => {
  snapPageFromScroll();
}, { passive: true });

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

// Pinch-to-zoom on touch devices. Two-finger gesture steps through the existing
// zoom levels (reusing zoomIn/zoomOut so clamping, re-render, and state-persist
// all apply). Single-touch is untouched, so pointer-based annotation drawing and
// text selection keep working.
let pinchActive = false;
let pinchBaseDist = 0;
function touchDistance(touches) {
  const dx = touches[0].clientX - touches[1].clientX;
  const dy = touches[0].clientY - touches[1].clientY;
  return Math.hypot(dx, dy);
}
canvasScroll.addEventListener('touchstart', (e) => {
  if (e.touches.length === 2) {
    pinchActive = true;
    pinchBaseDist = touchDistance(e.touches);
    e.preventDefault();  // suppress the browser's native page pinch-zoom
  }
}, { passive: false });
canvasScroll.addEventListener('touchmove', (e) => {
  if (!pinchActive || e.touches.length !== 2) return;
  e.preventDefault();
  const d = touchDistance(e.touches);
  if (pinchBaseDist <= 0) { pinchBaseDist = d; return; }
  const ratio = d / pinchBaseDist;
  if (ratio > 1.2) { zoomIn(); pinchBaseDist = d; }
  else if (ratio < 0.83) { zoomOut(); pinchBaseDist = d; }
}, { passive: false });
function endPinch(e) {
  if (pinchActive && e.touches.length < 2) { pinchActive = false; pinchBaseDist = 0; }
}
canvasScroll.addEventListener('touchend', endPinch);
canvasScroll.addEventListener('touchcancel', endPinch);

// Keyboard navigation
// Cycle to the next/previous annotation on the current page in select mode.
function cycleAnnotationSelection(delta) {
  const list = pageAnnots();
  if (!list.length) return false;
  const idx = list.findIndex((a) => a.id === selectedAnnotId);
  let next = idx + delta;
  if (idx < 0) next = delta > 0 ? 0 : list.length - 1;
  next = ((next % list.length) + list.length) % list.length;
  selectedAnnotId = list[next].id;
  hoveredAnnotId = null;
  renderAnnotations();
  return true;
}

document.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;

  // Shortcut-help overlay: "?" toggles it; Esc closes it (highest priority).
  if (shortcutHelpOpen()) {
    if (e.key === 'Escape' || e.key === '?') { e.preventDefault(); hideShortcutHelp(); }
    return;
  }
  if (e.key === '?') { e.preventDefault(); toggleShortcutHelp(); return; }

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
  } else if (e.key === ' ') {
    e.preventDefault();
    snapPageFromKeyboard(e.shiftKey);
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
  } else if ((e.key === 'p' || e.key === 'P') && !e.ctrlKey && !e.metaKey) {
    if (totalPages > 0 && hasRendering) { e.preventDefault(); printDocument(); }
  } else if ((e.key === 'b' || e.key === 'B') && !e.ctrlKey && !e.metaKey) {
    // B toggles a bookmark on the current page.
    e.preventDefault();
    if (findBookmarkAt(currentPage)) removeBookmarkAt(currentPage);
    else addBookmarkAtCurrent();
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
  } else if (e.key === 'Tab' && annotTool === 'select' && annotEditingEnabled()) {
    // Tab / Shift+Tab cycle through the annotations on the current page.
    e.preventDefault();
    cycleAnnotationSelection(e.shiftKey ? -1 : 1);
  } else if ((e.ctrlKey || e.metaKey) && (e.key === 'z' || e.key === 'Z') &&
             !e.shiftKey) {
    // Undo (no INPUT/TEXTAREA focus thanks to the early return above).
    e.preventDefault();
    if (undo()) setStatus('Undo', 'info');
  } else if ((e.ctrlKey || e.metaKey) && ((e.key === 'z' || e.key === 'Z') && e.shiftKey || e.key === 'y' || e.key === 'Y')) {
    // Redo (Cmd/Ctrl+Shift+Z or Cmd/Ctrl+Y).
    e.preventDefault();
    if (redo()) setStatus('Redo', 'info');
  }
});

// Render / Extract / Export buttons
renderBtn.addEventListener('click', renderCurrentPage);
backendToggleBtn.addEventListener('click', switchRenderBackend);
extractBtn.addEventListener('click', extractText);
exportBtn.addEventListener('click', exportSelectedPages);
protectExport.addEventListener('change', updateExportButton);
exportPasswordForm.addEventListener('submit', (e) => {
  e.preventDefault();
  if (!exportBtn.disabled) exportSelectedPages();
});

// Close text panel
document.getElementById('text-panel-close').addEventListener('click', () => {
  textPanel.style.display = 'none';
});

// Content-export format switches + actions
document.getElementById('text-fmt-text').addEventListener('click', () => { textPanelFormat = 'text'; renderTextPanel(); });
document.getElementById('text-fmt-md').addEventListener('click', () => { textPanelFormat = 'md'; renderTextPanel(); });
document.getElementById('text-fmt-tables').addEventListener('click', () => { textPanelFormat = 'tables'; renderTextPanel(); });
document.getElementById('text-download').addEventListener('click', downloadTextPanel);
document.getElementById('text-copy').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(textContent.textContent);
    const b = document.getElementById('text-copy');
    const prev = b.textContent; b.textContent = 'Copied'; setTimeout(() => { b.textContent = prev; }, 1000);
  } catch (e) { /* clipboard blocked; ignore */ }
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
  // If the user is typing, the dropdown is already open with the previous
  // results. We reopen it explicitly after the new search completes.
  searchDebounce = setTimeout(() => {
    performSearch(e.target.value.trim());
    showSearchResultsDropdown();
  }, 300);
});
searchInput.addEventListener('focus', () => {
  if (searchResults.length > 0) showSearchResultsDropdown();
});
searchInput.addEventListener('blur', () => {
  // Defer so a click on a dropdown row is processed before the dropdown hides.
  setTimeout(hideSearchResultsDropdown, 150);
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
    hideSearchResultsDropdown();
    searchInput.blur();
  } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    // Move the cursor in the input but don't propagate to the global handler
    // (which would navigate pages).
    e.stopPropagation();
  }
});
searchPrevBtn.addEventListener('click', prevSearchResult);
searchNextBtn.addEventListener('click', nextSearchResult);
const searchCaseToggle = document.getElementById('search-case');
if (searchCaseToggle) {
  searchCaseToggle.addEventListener('change', () => {
    searchCaseSensitive = searchCaseToggle.checked;
    if (currentSearchTerm) {
      performSearch(currentSearchTerm);
      showSearchResultsDropdown();
    }
  });
}

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
const undoBtn = document.getElementById('undo-btn');
const redoBtn = document.getElementById('redo-btn');
if (undoBtn) undoBtn.addEventListener('click', () => { if (undo()) setStatus('Undo'); });
if (redoBtn) redoBtn.addEventListener('click', () => { if (redo()) setStatus('Redo'); });
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

// Hover tracking for select-mode outlines. Re-rendering on every pointermove
// would be too expensive, so we use pointerover/pointerout (which fire only
// when the cursor crosses a child element boundary) and bail out if the
// hovered id hasn't changed.
function annotShapePointerOver(e) {
  if (annotTool !== 'select' || !annotEditingEnabled()) return;
  const host = e.target.closest('[data-annot-id]');
  if (!host) return;
  const id = Number(host.dataset.annotId);
  if (id === selectedAnnotId || id === hoveredAnnotId) return;
  hoveredAnnotId = id;
  renderAnnotations();
}
function annotShapePointerOut(e) {
  // Only clear if the cursor actually left the hovered shape (not into a child).
  if (hoveredAnnotId == null) return;
  const related = e.relatedTarget && e.relatedTarget.closest && e.relatedTarget.closest('[data-annot-id]');
  if (related) return;
  hoveredAnnotId = null;
  renderAnnotations();
}
annotSvg.addEventListener('pointerover', annotShapePointerOver);
annotSvg.addEventListener('pointerout', annotShapePointerOut);

// Window resize
window.addEventListener('resize', onResize);

// ---- Auto-recovery to IndexedDB ----
// Stash the most-recently-opened PDF so a page reload (or accidental tab
// close) can offer to restore it. The stash is keyed by file name + a session
// id; if the user opens a different document, the old one is discarded.
const RECOVERY_DB = 'nanopdf-recovery';
const RECOVERY_STORE = 'snapshots';
const RECOVERY_MAX_BYTES = 50 * 1024 * 1024;     // hard cap per stash
let recoveryDbPromise = null;
let recoverySaveTimer = 0;
let recoveryActiveName = null;

function openRecoveryDb() {
  if (recoveryDbPromise) return recoveryDbPromise;
  if (typeof indexedDB === 'undefined') return Promise.resolve(null);
  recoveryDbPromise = new Promise((resolve) => {
    const req = indexedDB.open(RECOVERY_DB, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(RECOVERY_STORE)) {
        db.createObjectStore(RECOVERY_STORE, { keyPath: 'name' });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => resolve(null);
  });
  return recoveryDbPromise;
}

async function stashRecovery(name, bytes) {
  if (!name || !(bytes instanceof Uint8Array) || bytes.length > RECOVERY_MAX_BYTES) return;
  const db = await openRecoveryDb();
  if (!db) return;
  try {
    const tx = db.transaction(RECOVERY_STORE, 'readwrite');
    tx.objectStore(RECOVERY_STORE).put({
      name,
      bytes,
      savedAt: Date.now(),
    });
    await new Promise((resolve, reject) => {
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
    });
  } catch (e) {
    console.warn('Recovery stash failed:', e);
  }
}

function scheduleRecoverySave() {
  if (!currentPdfBytes || !recoveryActiveName) return;
  clearTimeout(recoverySaveTimer);
  // Snapshot the bytes immediately (a Uint8Array reference is cheap to copy).
  // Debounce the actual write so a flurry of edits only writes once.
  const name = recoveryActiveName;
  const bytes = currentPdfBytes;
  recoverySaveTimer = setTimeout(() => stashRecovery(name, bytes), 1500);
}

async function clearRecovery() {
  const db = await openRecoveryDb();
  if (!db) return;
  try {
    const tx = db.transaction(RECOVERY_STORE, 'readwrite');
    tx.objectStore(RECOVERY_STORE).clear();
    await new Promise((resolve, reject) => {
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
    });
  } catch (e) {
    console.warn('Recovery clear failed:', e);
  }
}

async function readRecovery() {
  const db = await openRecoveryDb();
  if (!db) return null;
  return new Promise((resolve) => {
    const tx = db.transaction(RECOVERY_STORE, 'readonly');
    const req = tx.objectStore(RECOVERY_STORE).getAll();
    req.onsuccess = () => {
      const items = req.result || [];
      if (!items.length) return resolve(null);
      items.sort((a, b) => (b.savedAt || 0) - (a.savedAt || 0));
      resolve(items[0]);
    };
    req.onerror = () => resolve(null);
  });
}

function ensureRecoveryBanner() {
  let el = document.getElementById('recovery-banner');
  if (el) return el;
  el = document.createElement('div');
  el.id = 'recovery-banner';
  el.className = 'recovery-banner hidden';
  el.setAttribute('role', 'alert');
  document.body.appendChild(el);
  return el;
}

async function offerRecovery() {
  const item = await readRecovery();
  if (!item || !item.bytes || !item.name) return;
  const el = ensureRecoveryBanner();
  const ago = formatRecoveryAge(Date.now() - (item.savedAt || 0));
  el.innerHTML = '';
  const msg = document.createElement('span');
  msg.className = 'recovery-msg';
  msg.textContent = `Resume "${item.name}" from ${ago}?`;
  const restore = document.createElement('button');
  restore.type = 'button';
  restore.className = 'recovery-btn primary';
  restore.textContent = 'Restore';
  const dismiss = document.createElement('button');
  dismiss.type = 'button';
  dismiss.className = 'recovery-btn';
  dismiss.textContent = 'Discard';
  el.append(msg, restore, dismiss);
  el.classList.remove('hidden');
  restore.addEventListener('click', async () => {
    hideRecoveryBanner();
    try {
      // Copy into a fresh ArrayBuffer so the consumer can transfer it.
      const ab = new ArrayBuffer(item.bytes.byteLength);
      new Uint8Array(ab).set(item.bytes);
      await loadPDF(ab, item.name);
      setStatus(`Restored "${item.name}"`, 'success');
    } catch (e) {
      setStatus('Could not restore the previous document: ' + e.message, true);
    }
  });
  dismiss.addEventListener('click', async () => {
    hideRecoveryBanner();
    await clearRecovery();
  });
}

function hideRecoveryBanner() {
  const el = document.getElementById('recovery-banner');
  if (el) el.classList.add('hidden');
}

async function loadExternalStandardFontsForWasm() {
  showLoading('Loading fonts...');
  const stdCount = await loadStandardFonts(Module, FONTS_BASE, (ratio, name) => {
    loadingText.textContent = `Loading font: ${name} (${Math.round(ratio * 100)}%)`;
  });
  console.log(`Loaded ${stdCount} standard fonts from external files`);
  return stdCount;
}

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

    // Load/register embedded fonts when compiled into the module.
    const embeddedFontsAvailable = Module._nanopdf_fonts_available() === 1;
    let standardFontsReady = false;
    if (embeddedFontsAvailable) {
      try {
        showLoading('Registering embedded fonts...');
        if (typeof Module._nanopdf_register_embedded_fonts !== 'function') {
          throw new Error('Module._nanopdf_register_embedded_fonts is not a function');
        }
        const count = Module._nanopdf_register_embedded_fonts();
        standardFontsReady = count > 0;
        console.log(`Registered ${count} embedded fonts with FontProvider`);
      } catch (e) {
        console.log('Embedded fonts not available:', e.message);
      }
    }
    if (!standardFontsReady) {
      try {
        await loadExternalStandardFontsForWasm();
      } catch (e) {
        console.log('External fonts not available:', e.message);
      }
    }

    // CJK fonts are loaded in the background so PDFs with bold/emoji-like glyphs
    // can re-render with the right face once ready.
    loadCDNCJKFonts(Module, (ratio, name) => {
      console.log(`CJK font: ${name} (${Math.round(ratio * 100)}%)`);
    }).then(count => {
      if (count > 0 && totalPages > 0 && hasRendering) {
        console.log(`Loaded ${count} CJK fonts from CDN`);
        renderCurrentPage();
      }
      return count;
    }).catch(e => {
      console.log('CJK fonts not available:', e.message);
      return 0;
    });

    hideLoading();

    if (!hasRendering) {
      setStatus('Ready (rendering not available - build with NANOPDF_USE_LIGHTVG or NANOPDF_USE_THORVG for rendering)');
      renderBtn.style.display = 'none';
      updateBackendToggle();
    } else {
      setStatus(`Ready - ${backendLabel(renderBackend)} backend`);
    }

    const accel = getWasmAccelerationInfo(Module);
    console.log('nanopdf WASM loaded. Rendering:', hasRendering,
                'SIMD:', accel.simd, 'fpnge:', accel.fastPng ? accel.fpngeIsa : 'none');

    // Deep-link support: ?pdf=<pdf>, #pdf=<pdf>, or #url=<pdf>.
    // Avoid ?url= because Vite dev treats that as an import URL query.
    const deepLink = resolvePdfDeepLink(window.location.search, window.location.hash);
    if (deepLink) {
      loadPdfFromUrl(deepLink);
    } else {
      // No deep link: check for an auto-recovery stash from a previous visit
      // and offer to restore it.
      offerRecovery();
    }
  } catch (err) {
    hideLoading();
    setStatus('Failed to load WASM: ' + err.message, true);
    console.error(err);
  }
}

init();
