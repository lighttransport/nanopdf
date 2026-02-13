import './style.css';
import createModule from 'nanopdf-wasm';
import wasmUrl from 'nanopdf-wasm-bin';

// State
let Module = null;
let currentPage = 0;
let totalPages = 0;
let hasRendering = false;
let fileName = '';
let fileSize = 0;
let outlineData = null;
let searchResults = [];
let currentSearchIndex = -1;
let currentSearchTerm = '';
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

// DOM refs
const loadingOverlay = document.getElementById('loading-overlay');
const loadingText = document.getElementById('loading-text');
const dropOverlay = document.getElementById('drop-overlay');
const pdfInput = document.getElementById('pdf-input');
const renderBtn = document.getElementById('render-btn');
const extractBtn = document.getElementById('extract-btn');
const prevBtn = document.getElementById('prev-btn');
const nextBtn = document.getElementById('next-btn');
const sidebarToggleBtn = document.getElementById('sidebar-toggle-btn');
const canvas = document.getElementById('pdf-canvas');
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
  }

  for (const item of info) {
    html += `<div class="doc-info-item"><div class="label">${item.label}</div><div class="value">${item.value}</div></div>`;
  }
  sidebarContent.innerHTML = html;
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

  // Adjust wrapper dimensions for 90/270 so scrolling works correctly
  if (rotation === 90 || rotation === 270) {
    canvasWrapper.style.width = canvas.height + 'px';
    canvasWrapper.style.height = canvas.width + 'px';
  } else {
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';
  }
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

  // Apply rotation
  applyRotation();

  const zoomPct = Math.round(zoomLevel * 100);
  setStatus(`Page ${currentPage + 1} / ${totalPages} | ${pageWidth.toFixed(0)} x ${pageHeight.toFixed(0)} pts | ${zoomPct}%${rotation !== 0 ? ' | ' + rotation + '°' : ''}`);
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
    return;
  }

  const termPtr = Module.stringToNewUTF8(term);
  const pagesPtr = Module.stringToNewUTF8('all');
  const resultPtr = Module._nanopdf_batch_find_text(termPtr, pagesPtr);
  const resultStr = Module.UTF8ToString(resultPtr);
  Module._free(termPtr);
  Module._free(pagesPtr);

  try {
    const data = JSON.parse(resultStr);
    if (data.results) {
      for (const pageResult of data.results) {
        for (const match of pageResult.matches) {
          searchResults.push({
            page: pageResult.page,
            start: match.start,
            end: match.end,
            context: match.context
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
  } else {
    searchInfo.textContent = 'No matches';
    searchPrevBtn.disabled = true;
    searchNextBtn.disabled = true;
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
}

function prevSearchResult() {
  if (searchResults.length === 0) return;
  currentSearchIndex = (currentSearchIndex - 1 + searchResults.length) % searchResults.length;
  const result = searchResults[currentSearchIndex];
  if (result.page !== currentPage) {
    goToPage(result.page);
  }
  updateSearchInfo();
}

function clearSearch() {
  // Don't clear input or results, just reset current match tracking
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
    currentPage = 0;

    // Reset view state
    zoomLevel = 1.0;
    rotation = 0;
    fitMode = 'width';
    thumbnailCache = {};
    thumbnailRenderQueue = [];
    isThumbnailRendering = false;
    selectedPages = new Set();
    lastClickedPage = 0;
    updateZoomDisplay();
    updateExportButton();
    fitModeBtn.textContent = 'Fit Width';
    canvas.style.transform = '';
    canvasWrapper.style.width = '';
    canvasWrapper.style.height = '';

    // Enable controls
    renderBtn.disabled = !hasRendering;
    extractBtn.disabled = false;
    prevBtn.disabled = false;
    nextBtn.disabled = false;
    sidebarToggleBtn.disabled = false;
    searchInput.disabled = false;
    zoomInBtn.disabled = !hasRendering;
    zoomOutBtn.disabled = !hasRendering;
    fitModeBtn.disabled = !hasRendering;
    rotateBtn.disabled = !hasRendering;

    updatePageDisplay();

    // Load outline
    loadOutline();

    // Auto-show sidebar if there are bookmarks
    if (outlineData && outlineData.outline && outlineData.outline.length > 0) {
      sidebar.classList.remove('hidden');
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

async function exportSelectedPages() {
  if (selectedPages.size === 0 || !hasRendering || !Module) return;

  const pages = [...selectedPages].sort((a, b) => a - b);
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

  // Trigger download
  const blob = new Blob([pdfBytes], { type: 'application/pdf' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  const baseName = fileName.replace(/\.pdf$/i, '');
  a.download = `${baseName}_pages_${pages.map(p => p + 1).join('-')}.pdf`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);

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
extractBtn.addEventListener('click', extractText);
exportBtn.addEventListener('click', exportSelectedPages);

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

    hideLoading();

    if (!hasRendering) {
      setStatus('Ready (rendering not available - build with NANOPDF_USE_THORVG or NANOPDF_USE_BLEND2D for rendering)');
      renderBtn.style.display = 'none';
    } else {
      setStatus('Ready - open a PDF file or drag and drop');
    }

    console.log('nanopdf WASM loaded. Rendering:', hasRendering);
  } catch (err) {
    hideLoading();
    setStatus('Failed to load WASM: ' + err.message, true);
    console.error(err);
  }
}

init();
