import assert from 'node:assert/strict';
import test from 'node:test';

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
  getMdpPermissionLabel,
  highlightContext,
  outlineBranchMatches,
  readWasmString,
  renderPageIntoImageData,
  resolvePdfDeepLink,
  shouldRenderPageStatus,
} from '../src/viewer-utils.js';

test('escapeHtml escapes text and attribute delimiters', () => {
  assert.equal(
    escapeHtml(`"><img src=x onerror=alert(1)>&'`),
    '&quot;&gt;&lt;img src=x onerror=alert(1)&gt;&amp;&#39;',
  );
});

test('computeRenderSize caps dimensions without changing aspect ratio', () => {
  const size = computeRenderSize(10000, 5000, 1, 4096);
  assert.equal(size.width, 4096);
  assert.equal(size.height, 2048);
});

test('computeRenderSize normalizes invalid page inputs', () => {
  assert.deepEqual(computeRenderSize(NaN, -10, 0, 4096), { width: 1, height: 1 });
  assert.deepEqual(computeRenderSize(100, 50, 1, NaN), { width: 100, height: 50 });
});

test('computeRenderSize handles scale=0 and very large scales', () => {
  // scale=0 is treated as identity (1x) so the page returns at its
  // declared size; this matches the helper's "scale must be > 0" contract.
  assert.deepEqual(computeRenderSize(800, 600, 0, 4096), { width: 800, height: 600 });
  // Very large scale gets capped to maxDim
  const size = computeRenderSize(1000, 500, 1000, 4096);
  assert.equal(size.width, 4096);
  assert.equal(size.height, 2048);
  // A small page at typical thumb size
  const thumb = computeRenderSize(612, 792, 120 / 612, 256);
  assert.equal(thumb.width, 120);
  assert.equal(thumb.height, 155);
});

test('formatFileSize handles normal and invalid sizes', () => {
  assert.equal(formatFileSize(512), '512 B');
  assert.equal(formatFileSize(1536), '1.5 KB');
  assert.equal(formatFileSize(2 * 1024 * 1024), '2.0 MB');
  assert.equal(formatFileSize(Number.NaN), '0 B');
  assert.equal(formatFileSize(-5), '0 B');
  assert.equal(formatFileSize(Infinity), '0 B');
  assert.equal(formatFileSize(null), '0 B');
  assert.equal(formatFileSize(undefined), '0 B');
});

test('formatPdfDate formats PDF D dates with sane defaults', () => {
  assert.equal(formatPdfDate('D:20240102030405'), '2024-01-02 03:04:05');
  assert.equal(formatPdfDate('D:2024'), '2024-01-01 00:00:00');
  assert.equal(formatPdfDate('not-a-date'), 'not-a-date');
  assert.equal(formatPdfDate(null), '');
});

test('formatPdfDate falls back to the raw string for partial-mangled input', () => {
  // A "D:" prefix with non-digit month: we still match the year group,
  // but month/day default to 01 since the regex only captures \d{2}?
  assert.equal(formatPdfDate('D:2024ab'), '2024-01-01 00:00:00');
  // No "D:" prefix at all -> returned as-is
  assert.equal(formatPdfDate('2024-01-02'), '2024-01-02');
});

test('getMdpPermissionLabel normalizes permission labels', () => {
  assert.equal(getMdpPermissionLabel(1), 'No changes allowed');
  assert.equal(getMdpPermissionLabel('2'), 'Form fill and sign only');
  assert.equal(getMdpPermissionLabel(3), 'Form fill, sign, annotate');
  assert.equal(getMdpPermissionLabel('7'), 'Unknown (7)');
  assert.equal(getMdpPermissionLabel('<script>'), 'Not specified');
});

test('chooseSelectedExportRoute prioritizes secure redaction burn-in', () => {
  assert.equal(
    chooseSelectedExportRoute({ hasRedaction: true, protect: true, hasBakeable: true, canSplit: true }),
    'burn-redaction',
  );
  assert.equal(
    chooseSelectedExportRoute({ hasRedaction: false, protect: true, hasBakeable: false, canSplit: true }),
    'wasm-flatten',
  );
  assert.equal(
    chooseSelectedExportRoute({ hasRedaction: false, protect: false, hasBakeable: false, canSplit: true }),
    'vector-split',
  );
  assert.equal(
    chooseSelectedExportRoute({ hasRedaction: false, protect: false, hasBakeable: false, canSplit: false }),
    'unavailable',
  );
});

test('chooseSelectedExportRoute redaction wins over protect and bakeable', () => {
  // Even with no protect and nothing to bake, a redaction must burn pixels.
  assert.equal(
    chooseSelectedExportRoute({ hasRedaction: true, protect: false, hasBakeable: false, canSplit: true }),
    'burn-redaction',
  );
});

test('resolvePdfDeepLink avoids the legacy ?url query and accepts pdf/hash forms', () => {
  assert.equal(resolvePdfDeepLink('?pdf=/sample.pdf', ''), '/sample.pdf');
  assert.equal(resolvePdfDeepLink('', '#url=https%3A%2F%2Fexample.test%2Fa.pdf'), 'https://example.test/a.pdf');
  assert.equal(resolvePdfDeepLink('?url=/blocked.pdf', ''), '');
});

test('resolvePdfDeepLink handles empty inputs and conflict between search and hash', () => {
  assert.equal(resolvePdfDeepLink('', ''), '');
  // Search wins over hash for the same key (consistent with the helper's order).
  assert.equal(resolvePdfDeepLink('?pdf=/a.pdf', '#pdf=/b.pdf'), '/a.pdf');
  // Only hash set
  assert.equal(resolvePdfDeepLink('', '#pdf=/c.pdf'), '/c.pdf');
});

test('shouldRenderPageStatus respects operation status hold window', () => {
  assert.equal(shouldRenderPageStatus(1000, 1500), false);
  assert.equal(shouldRenderPageStatus(1500, 1500), true);
  assert.equal(shouldRenderPageStatus(2000, 1500), true);
  assert.equal(shouldRenderPageStatus(2000, NaN), true);
});

test('readWasmString guards against a NULL WASM pointer', () => {
  // A module stub that throws if UTF8ToString is called on a NULL ptr —
  // confirms the helper short-circuits before invoking the bridge.
  let calls = 0;
  const Module = { UTF8ToString: () => { calls++; return 'should-not-be-called'; } };
  assert.equal(readWasmString(Module, 0), '');
  assert.equal(readWasmString(Module, null), '');
  assert.equal(readWasmString(Module, undefined), '');
  assert.equal(calls, 0);
  assert.equal(readWasmString(Module, 1), 'should-not-be-called');
  assert.equal(calls, 1);
});

test('renderPageIntoImageData copies the WASM render buffer into a detached ImageData', () => {
  const w = 4, h = 4;
  const size = w * h * 4;
  // Stub Module: render returns 1, returns a buffer of 0xFF RGBA pixels.
  const buf = new Uint8Array(size);
  buf.fill(0xFF);
  const calls = { render: 0, buffer: 0, size: 0, w: 0, h: 0, release: 0 };
  const Module = {
    _nanopdf_render_page: () => { calls.render++; return 1; },
    _nanopdf_get_render_buffer: () => { calls.buffer++; return 0x1000; },
    _nanopdf_get_render_buffer_size: () => { calls.size++; return size; },
    _nanopdf_get_render_width: () => { calls.w++; return w; },
    _nanopdf_get_render_height: () => { calls.h++; return h; },
    _nanopdf_release_render_buffer: () => { calls.release++; },
    HEAPU8: { buffer: new ArrayBuffer(0x10000) },
  };
  // Patch a real backing store at 0x1000 so the data.copy works.
  new Uint8Array(Module.HEAPU8.buffer, 0x1000, size).set(buf);
  const result = renderPageIntoImageData(Module, 0, w, h, 72);
  assert.equal(result.ok, true);
  assert.equal(result.w, w);
  assert.equal(result.h, h);
  assert.equal(result.imageData.data.length, size);
  assert.equal(result.imageData.data[0], 0xFF);
  assert.equal(typeof result.renderMs, 'number');
  assert.equal(typeof result.copyMs, 'number');
  assert.equal(typeof result.totalMs, 'number');
  assert.equal(calls.render, 1);
  assert.equal(calls.release, 1);
});

test('renderPageIntoImageData surfaces the C-side error on failure', () => {
  const Module = {
    _nanopdf_render_page: () => 0,
    _nanopdf_get_last_error: () => 0x2000,
    UTF8ToString: (p) => p === 0x2000 ? 'C says no' : '',
  };
  const result = renderPageIntoImageData(Module, 0, 1, 1, 72);
  assert.equal(result.ok, false);
  assert.equal(result.imageData, null);
  assert.equal(result.error, 'C says no');
});

test('renderPageIntoImageData returns an empty-buffer error if the bridge returns 0', () => {
  const Module = {
    _nanopdf_render_page: () => 1,
    _nanopdf_get_render_buffer: () => 0,
    _nanopdf_get_render_buffer_size: () => 0,
    _nanopdf_get_render_width: () => 0,
    _nanopdf_get_render_height: () => 0,
  };
  const result = renderPageIntoImageData(Module, 0, 1, 1, 72);
  assert.equal(result.ok, false);
  assert.equal(result.error, 'empty render buffer');
});

test('assertOkOrThrow passes through success and throws on failure', () => {
  // success: no throw
  assertOkOrThrow(1, 'op', null);
  assertOkOrThrow(true, 'op', null);
  // failure: throws with the C-side error
  const Module = {
    _nanopdf_get_last_error: () => 0x3000,
    UTF8ToString: (p) => p === 0x3000 ? 'C error' : '',
  };
  assert.throws(() => assertOkOrThrow(0, 'myOp', Module), /myOp.*C error/);
  // failure with no last-error bridge: throws with a generic message
  assert.throws(() => assertOkOrThrow(0, 'myOp', null), /myOp failed/);
});

test('escapeRegExp escapes regex metacharacters', () => {
  assert.equal(escapeRegExp('a.b+c*'), 'a\\.b\\+c\\*');
  assert.equal(escapeRegExp('(foo) [bar] {baz}'), '\\(foo\\) \\[bar\\] \\{baz\\}');
  assert.equal(escapeRegExp(''), '');
  assert.equal(escapeRegExp(null), '');
  assert.equal(escapeRegExp(undefined), '');
});

test('highlightContext wraps matches in <mark> and HTML-escapes the rest', () => {
  assert.equal(
    highlightContext('Hello world, welcome!', 'world'),
    'Hello <mark>world</mark>, welcome!',
  );
  // Case-insensitive.
  assert.equal(
    highlightContext('Foo FOO foo', 'foo'),
    '<mark>Foo</mark> <mark>FOO</mark> <mark>foo</mark>',
  );
  // HTML in the context is escaped (the output is safe for innerHTML).
  assert.equal(
    highlightContext('A <script>alert(1)</script> B', 'script'),
    'A &lt;<mark>script</mark>&gt;alert(1)&lt;/<mark>script</mark>&gt; B',
  );
  // No term -> return the HTML-escaped context, no <mark>.
  assert.equal(highlightContext('a & b', ''), 'a &amp; b');
  // Null / undefined context -> empty string.
  assert.equal(highlightContext(null, 'x'), '');
  assert.equal(highlightContext(undefined, 'x'), '');
  // Regex metacharacters in the term don't break the match.
  assert.equal(
    highlightContext('user.name = 42', 'user.name'),
    '<mark>user.name</mark> = 42',
  );
});

test('formatRecoveryAge covers the full range of inputs', () => {
  assert.equal(formatRecoveryAge(0), 'just now');
  assert.equal(formatRecoveryAge(500), 'just now');
  assert.equal(formatRecoveryAge(60_000 - 1), 'just now');
  assert.equal(formatRecoveryAge(60_000), '1 min ago');
  assert.equal(formatRecoveryAge(5 * 60_000), '5 min ago');
  assert.equal(formatRecoveryAge(60 * 60_000), '1 h ago');
  assert.equal(formatRecoveryAge(2 * 60 * 60_000), '2 h ago');
  assert.equal(formatRecoveryAge(24 * 60 * 60_000), '1 d ago');
  assert.equal(formatRecoveryAge(3 * 24 * 60 * 60_000), '3 d ago');
  // Defensive: negative or non-finite -> empty.
  assert.equal(formatRecoveryAge(-1), '');
  assert.equal(formatRecoveryAge(NaN), '');
  assert.equal(formatRecoveryAge(Infinity), '');
});

test('outlineBranchMatches walks the tree and matches case-insensitively', () => {
  const tree = [
    { title: 'Introduction', children: [
      { title: 'Overview' },
      { title: 'Goals', children: [
        { title: 'Long-term goals' },
      ] },
    ] },
    { title: 'References' },
  ];
  // Empty / falsy query is treated as "no filter" -> always true.
  assert.equal(outlineBranchMatches(tree, ''), true);
  assert.equal(outlineBranchMatches(tree, null), true);
  // Empty items: no match (no items to check).
  assert.equal(outlineBranchMatches([], 'anything'), false);
  // Direct match.
  assert.equal(outlineBranchMatches(tree, 'intro'), true);
  assert.equal(outlineBranchMatches(tree, 'REFER'), true);
  // Descendant match.
  assert.equal(outlineBranchMatches(tree, 'long-term'), true);
  assert.equal(outlineBranchMatches(tree, 'goals'), true);
  // No match anywhere.
  assert.equal(outlineBranchMatches(tree, 'nope'), false);
  // Defensive: non-array input.
  assert.equal(outlineBranchMatches(null, 'x'), true);
  // Items without a title don't crash.
  const sparse = [{ children: [{ title: 'real' }] }];
  assert.equal(outlineBranchMatches(sparse, 'real'), true);
  assert.equal(outlineBranchMatches(sparse, 'absent'), false);
});

test('annotLabel returns a human description for every annotation type', () => {
  assert.equal(annotLabel({ type: 'highlight' }), 'Highlight');
  assert.equal(annotLabel({ type: 'rect' }), 'Rectangle');
  assert.equal(annotLabel({ type: 'oval' }), 'Oval');
  assert.equal(annotLabel({ type: 'line' }), 'Line');
  assert.equal(annotLabel({ type: 'arrow' }), 'Arrow');
  assert.equal(annotLabel({ type: 'ink' }), 'Drawing');
  assert.equal(annotLabel({ type: 'redact' }), 'Redaction');
  assert.equal(annotLabel({ type: 'image' }), 'Image');
  // Link labels surface the destination page.
  assert.equal(annotLabel({ type: 'link', destPage: 4 }), 'Link → p.5');
  assert.equal(annotLabel({ type: 'link' }), 'Link');
  // Text body is quoted and truncated.
  assert.equal(
    annotLabel({ type: 'text', text: 'Hello world' }),
    '\u201cHello world\u201d',
  );
  assert.equal(
    annotLabel({ type: 'text', text: 'a'.repeat(80) }),
    '\u201c' + 'a'.repeat(40) + '\u2026\u201d',
  );
  // Empty / whitespace text falls back to "Text".
  assert.equal(annotLabel({ type: 'text' }), 'Text');
  assert.equal(annotLabel({ type: 'text', text: '   ' }), 'Text');
  // Unknown / defensive inputs.
  assert.equal(annotLabel({ type: 'stamp' }), 'stamp');
  assert.equal(annotLabel(null), '');
  assert.equal(annotLabel({}), 'Annotation');
  assert.equal(annotLabel({ type: '' }), 'Annotation');
});

test('dashToStrokeDasharray maps names to SVG dasharray strings', () => {
  assert.equal(dashToStrokeDasharray('solid'), null);
  assert.equal(dashToStrokeDasharray('dashed'), '6 3');
  assert.equal(dashToStrokeDasharray('dotted'), '2 4');
  // Null / undefined / unknown -> no dasharray.
  assert.equal(dashToStrokeDasharray(null), null);
  assert.equal(dashToStrokeDasharray(undefined), null);
  assert.equal(dashToStrokeDasharray(''), null);
  assert.equal(dashToStrokeDasharray('wiggly'), null);
});

test('clamp keeps numbers inside the [lo, hi] window', () => {
  assert.equal(clamp(5, 0, 10), 5);
  assert.equal(clamp(-1, 0, 10), 0);
  assert.equal(clamp(11, 0, 10), 10);
  assert.equal(clamp(0, 0, 10), 0);
  assert.equal(clamp(10, 0, 10), 10);
  // Non-finite input -> lo (defensive: treat as "not a number").
  assert.equal(clamp(NaN, 0, 10), 0);
  assert.equal(clamp(Infinity, 0, 10), 0);
  assert.equal(clamp(-Infinity, 0, 10), 0);
  assert.equal(clamp('5', 0, 10), 5);   // numeric coercion
  assert.equal(clamp(undefined, 0, 10), 0);
});
