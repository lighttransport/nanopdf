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

function updateSidebar() {
  if (activeSidebarTab === 'outline') {
    renderOutlineTab();
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

// ---- Rendering ----

function renderCurrentPage() {
  if (!Module || totalPages <= 0) return;
  if (!hasRendering) {
    setStatus(`Page ${currentPage + 1} / ${totalPages} (rendering not available)`);
    if (activeSidebarTab === 'info') renderInfoTab();
    return;
  }

  const pageWidth = Module._nanopdf_get_page_width(currentPage);
  const pageHeight = Module._nanopdf_get_page_height(currentPage);

  const maxWidth = Math.min(800, window.innerWidth - (sidebar.classList.contains('hidden') ? 80 : 340));
  const scale = maxWidth / pageWidth;
  const width = Math.floor(pageWidth * scale);
  const height = Math.floor(pageHeight * scale);

  canvas.width = width;
  canvas.height = height;

  const dpi = 72 * scale;
  const result = Module._nanopdf_render_page(currentPage, width, height, dpi);

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

  setStatus(`Page ${currentPage + 1} / ${totalPages} | ${pageWidth.toFixed(0)} x ${pageHeight.toFixed(0)} pts`);
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

    // Enable controls
    renderBtn.disabled = !hasRendering;
    extractBtn.disabled = false;
    prevBtn.disabled = false;
    nextBtn.disabled = false;
    sidebarToggleBtn.disabled = false;
    searchInput.disabled = false;

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

// ---- Event Handlers ----

// File input
pdfInput.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  const arrayBuffer = await file.arrayBuffer();
  loadPDF(arrayBuffer, file.name);
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
  }
});

// Render / Extract buttons
renderBtn.addEventListener('click', renderCurrentPage);
extractBtn.addEventListener('click', extractText);

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
