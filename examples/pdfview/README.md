# pdfview — native PDF viewer (lightui + nanopdf)

A native desktop PDF viewer built on **[lightui](https://github.com/lighttransport/lightui)**
for windowing/widgets and **nanopdf** for parsing and page rasterization. Linux/X11 is
the current focus; Windows and macOS are structured-for but not yet wired.

The goal is rough feature parity with the WASM viewer (`examples/wasm`): page
navigation, zoom/fit, an outline + thumbnail sidebar, text search & selection, a
document-info / signatures / forms panel, and revision-history visual diff.

## Architecture

```
pdfview executable
├── lightui (deps/lightui) ........ window (X11), widgets, layout, lighttype fonts
├── nanopdf (libnanopdf.a) ........ PDF parse + page rasterization -> RGBA
└── ONE shared lightvg (lvg_*) .... the vector rasterizer, provided by libnanopdf.a
```

nanopdf and lightui both use **lightvg**. To avoid two copies, the viewer compiles
lightui's UI/widget/font/X11 layer but **not** lightvg's `canvas.c`/`surface.c` — those
`lvg_*` symbols come from `libnanopdf.a` (which vendors lightui's current lightvg at
`src/third_party/lightvg`). Only `lightvg/src/dirty.c` (the `lvg_dirty_*` region tracker,
used by lightui's layout but not part of nanopdf) is compiled here.

A page is rendered by nanopdf into an RGBA buffer (`nanopdf::make_backend(LightVG)
->render_page`), wrapped as an `lvg_surface_t`, and blitted into the UI with
`lvg_canvas_draw_image`.

Fonts are **shared**: UI text uses nanopdf's `fonts/` (no separate UI font set), and PDF
rendering uses nanopdf's font machinery.

## Dependencies

Vendored under `deps/` (copied, not submodules):

- `deps/lightui/` — lightui UI + lighttype + X11 backend + lightvg headers/`dirty.c`.
- `deps/nativefiledialog-ng/` — native Open/Save dialogs *(added in Phase 2)*.

System: a C/C++17 toolchain, `zlib`, and X11 at runtime (lightui dlopens `libX11`, so
there is no link-time X11 dependency).

## Build

Easiest — `run.sh` builds a dedicated LightVG-only nanopdf library plus the viewer:

```bash
./run.sh [document.pdf]      # or: ./run.sh --mcp document.pdf
```

Manual build:

```bash
# 1) Build nanopdf WITHOUT ThorVG (the viewer links only the LightVG backend;
#    a ThorVG-enabled libnanopdf.a fails to link on tvg:: symbols). A dedicated
#    build dir avoids clobbering a ThorVG-enabled build/ you may already have.
cmake -S ../.. -B ../../build-pdfview -DCMAKE_BUILD_TYPE=Release -DNANOPDF_USE_THORVG=OFF
cmake --build ../../build-pdfview --target nanopdf -j

# 2) Build the viewer, pointing at that nanopdf build dir
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DNANOPDF_BUILD_DIR=$(cd ../.. && pwd)/build-pdfview
cmake --build build -j     # -> ./build/pdfview
```

> The viewer renders via nanopdf's LightVG backend (it does not link ThorVG). If
> you point `NANOPDF_BUILD_DIR` at a `libnanopdf.a` built with `NANOPDF_USE_THORVG=ON`,
> linking fails with undefined `tvg::…` symbols — rebuild nanopdf with
> `-DNANOPDF_USE_THORVG=OFF` (as above).

## Run

```bash
./build/pdfview [document.pdf]
```

`--selftest [out.ppm]` renders a reference frame to an in-memory surface and dumps a PPM
(no window) — used for headless verification of the render path.

With no document, the window shows an **Open PDF…** button (click it, press `o`, or
**drag-and-drop** a PDF file onto the window). Drag-and-drop works any time to open a
different file.

## Controls

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| `o` | Open file (native dialog) | `+` / `-` | Zoom in / out |
| `←` / `PgUp` / `k` | Previous page | `0` / `f` / `1` | Fit width / fit page / 100% |
| `→` / `PgDn` / `j` | Next page | `b` | Toggle sidebar |
| `Home` / `End` | First / last page | `t` | Sidebar → thumbnails (Pages) |
| `p` then digits, `Enter` | Go to page number | | |
| `↑` / `↓` / wheel | Scroll | `/` then type, `Enter` | Search; `n`/`N` next/prev |
| drag on page | Select text (`Ctrl+C` to copy) | `i` | Info / signatures / forms panel |
| `h` / `u` / `x` | Highlight / underline / strikethrough the selection | `w` | Save annotated copy (`*.annotated.pdf`) |
| `m` then click, type, `Enter` | Add a sticky note | | |
| click outline / thumbnail | Jump to page | `r` | Revisions panel; `d` cycles After/Before/Diff |
| `g` | Toggle **debug** mode | drag (debug) | Inspect region → PDF objects |
| `s` | Save screenshot (PNG) of the frame | `[` / `]` | Rotate view CCW / CW |
| | | `Esc` | Clear search/selection, else quit |

## Debug mode + MCP (LLM/VLM-assisted inspection)

**Debug mode** (`g`): drag a region on the page and the viewer reports the PDF objects
under it — text runs with their font + object number, image XObjects with placement +
object number, and annotations — highlighting them on the page. The right "Debug" panel
lists the hits; click one to highlight it and dump the underlying PDF object.

**MCP server** (`--mcp [port]`, default 3001): embeds lightui's MCP server so an LLM/agent
can drive and inspect the live viewer over HTTP (JSON-RPC). Tools: `viewer_state`,
`goto_page`, `set_zoom`, `open`, `search`, `pdf_inspect_region`, `pdf_dump_object`,
`pdf_highlight_object`, `pdf_clear_highlight`, plus lightui's built-in `screenshot` (returns
the live frame as a base64 PNG for VLMs) and canvas inspectors. `--mcp-serve <pdf> [port]`
runs the same server **headless** (offscreen render, no display) for LLM PDF inspection.

Headless self-test modes (no display): `--selftest`, `--renderpage <pdf> <page> <out.ppm>`,
`--compose <pdf> <out.ppm>`, `--search <pdf> <term> <out.ppm>`, `--info <pdf> <out.ppm>`,
`--rev <pdf> <out.ppm> [before|diff]`, `--inspect <pdf> <page> <x1> <y1> <x2> <y2>`,
`--debugview <pdf> <page> <x1> <y1> <x2> <y2> <out.ppm>`,
`--screenshot <pdf> <out.png|jpg> [page] [scale]` (export a page as PNG/JPG; format
chosen by the output extension).

Interactively, press `s` to save a PNG screenshot of the current frame to the working
directory (auto-named `<file>_p<NN>_<seq>.png`).

## Status

- [x] Phase 1: scaffold, vendored deps, build wiring, window + blank canvas, shared lvg.
- [x] Phase 2: open (CLI + native portal dialog), render/display, navigation, zoom/fit,
      scroll, bundled-font registration.
- [x] Phase 3: outline sidebar (bookmark tree, decoded titles, click-to-jump) + thumbnail
      strip (tabbed Outline/Pages).
- [x] Phase 4: text search (highlight, next/prev across pages) + drag text selection.
- [x] Phase 5: document info / signatures / forms panel.
- [x] Phase 6: revision-history list + Before/After/Diff visual diff.
- [x] Debug mode: region → PDF objects (text/font/obj, images, annotations),
      highlight + object dump.
- [x] MCP server: drive/inspect the viewer from an LLM/agent over HTTP, incl.
      VLM screenshots and the PDF debug tools (live `--mcp` and headless `--mcp-serve`).

### Digital signatures + RFC 3161 timestamps
pdfview can apply a real cryptographic signature (PKCS#7/CMS `adbe.pkcs7.detached`,
SHA-256) via OpenSSL, as an incremental update that preserves existing revisions:

```bash
# PKCS#12 bundle, with a DigiCert timestamp:
pdfview --sign in.pdf out.pdf --p12 id.p12 --pass secret --reason "Approved" --tsa digicert
# PEM cert + key:
pdfview --sign in.pdf out.pdf --cert cert.pem --key key.pem --tsa freetsa
```

When `--tsa` is given, an RFC 3161 signature-timestamp (`id-aa-timeStampToken`) is
embedded, producing a PAdES-T / CAdES-T style signature. TSA presets:
`digicert`, `globalsign`, `sectigo` (http), `freetsa` (https); or pass a full URL.
`opentimestamps` is reserved (it is not RFC 3161). Signatures verify with external
tools (e.g. poppler `pdfsig`). An agent can drive signing via the MCP `pdf_sign`
tool. Note: signing requires nanopdf's writer to load the input; PDFs that use
cross-reference streams are not yet loadable for incremental update.

The **Info panel** (`i`) cryptographically verifies each signature via OpenSSL —
showing VALID/INVALID, whether it covers the whole document, the signer, digest,
signing time, and any embedded RFC 3161 timestamp (time + TSA) — rather than the
digest-only integrity check.

### HiDPI / display scaling
The viewer renders to a physical-resolution surface and scales all chrome (metrics,
font, scroll/threshold deltas) by the display's DPI factor, honoring the desktop's
configured scale including fractional values (e.g. 1.25x) via `Xft.dpi`. The scale is
re-applied if the window moves to a monitor with a different scale.

### Text extraction / search / CJK
nanopdf text extraction resolves Unicode via the full mapping chain (ToUnicode CMap,
Adobe-Japan1 CID→Unicode, `/Encoding` `/Differences` glyph names, WinAnsi/MacRoman/Symbol),
plus a fallback that reverses an embedded TrueType cmap (GID→Unicode) for Identity-H CID
fonts without ToUnicode. So search/selection and CJK work for PDFs with standard encodings,
ToUnicode maps, Adobe-Japan1 CID fonts, or cmap-bearing embedded fonts.

Remaining limit (industry-wide, not viewer-specific): a PDF whose embedded subset fonts use
Identity-H **and** carry no ToUnicode, no cmap, and no glyph names (e.g. the A64FX manual
body in `data/`) has no Unicode anywhere — it renders but cannot be searched/extracted
without OCR. `pdftotext`/Acrobat fail these too.
