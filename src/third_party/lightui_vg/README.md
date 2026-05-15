# lightui_vg — vendored snapshot

Snapshot of the lightui vector-graphics engine, vendored from
`/home/syoyo/work/lightui/src/vg/` + `/home/syoyo/work/lightui/include/lightui/vg/`
+ `/home/syoyo/work/lightui/src/internal/`.

Used by `src/lightvg-backend.{hh,cc}` (the default `RenderBackend` in nanopdf)
to rasterize PDF pages with the lui_canvas software rasterizer. No external
graphics dependencies.

## Layout

```
include/lightui/
├── canvas.h, surface.h, scene.h, log.h        ← compatibility shims for callers using the old flat paths
└── vg/
    ├── vg.h, vg_types.h, vg_math.h
    ├── surface.h, canvas.h, canvas_backend.h
    └── scene.h

src/
├── canvas.c              (~4055 lines, software rasterizer)
├── surface.c             (allocate/destroy + PNG export gated behind LUI_VG_ENABLE_PNG)
├── scene.c, backend_registry.c
├── canvas_ops.h          (backend vtable — internal)
└── internal/
    ├── pixel_blend.h     (Porter-Duff SRC_OVER, optional SSE2/NEON)
    ├── lui_log.{c,h}
    └── lui_grow.h
```

The runtime-pluggable `blend2d` and `thorvg` sub-backends from upstream
(`src/vg/backends/{blend2d,thorvg}/`) are **not** vendored — nanopdf uses the
software rasterizer only.

## Sync-back policy

Per the project owner, whenever a fix is applied to one of these vendored
files, mirror the same fix into upstream lightui:

* VG fixes: `/home/syoyo/work/lightui/src/vg/`
* Font fixes (`ttf_parse.c`, `rasterize.c`, etc.): `/home/syoyo/work/lightui/src/fonts/`

Already mirrored:

* `src/third_party/ttf_parse.c` — cmap is now optional (CID-keyed PDF subsets
  often omit it). Same patch is applied to upstream `src/fonts/ttf_parse.c`.

## Not yet integrated

The following files are vendored at `src/third_party/` as forward references
but are **not** compiled into the library:

* `lui_font.c`, `lui_font_internal.h`     — glyph cache + atlas + sRGB LUT
* `lui_text_layout.c`, `lui_text_layout.h` — span-based text wrapping
* `utf8_util.h`                            — UTF-8 helpers

They depend transitively on `<lightui/font.h>` which wraps LightType
(`ltt_font_t`). Wiring them into nanopdf without LightType requires either
porting LightType's font handle out or refactoring `font_internal.h` to drop
the typedef dependency. Tracked as follow-up.

## LightVGBackend status

`src/lightvg-backend.{hh,cc}` carries the full PDF content-stream
implementation from `thorvg-backend.cc` — text, paths, images, gradients,
graphics state, soft-mask alpha, etc. The ThorVG-specific leaf operations
are intercepted by an `lvg::` shim (`src/lvg-compat.{hh,cc}`) that emits to
`lui_canvas`.

### Feature coverage

| Feature                          | LightVG path                                              |
| -------------------------------- | --------------------------------------------------------- |
| Solid path fills (NZ/EO)         | `lui_canvas_fill_polygonf_ex` (analytic AA)               |
| Solid axis-aligned rect fills    | `lui_canvas_fill_rect`                                    |
| Stroked paths (cap, join, miter) | `lui_canvas_draw_styled_polyline`                         |
| Gradient fills on rects          | `lui_canvas_fill_rect_gradient` (linear + radial)         |
| Gradient fills on polygons       | Mid-stop sample (degraded — no polygon-gradient prim)     |
| Gradient strokes                 | Mid-stop sample (degraded)                                |
| Image blits (scale + translate)  | `lui_canvas_draw_image` with bilinear filter              |
| Image rotation / shear           | Per-pixel inverse-transform sample with bilinear + SRC_OVER |
| Glyph bitmaps                    | ARGB blit through Picture → lui_canvas_draw_image         |
| Rectangle clipping (W on rect)   | `lui_canvas_set_clip` (exact)                             |
| Vector clipping (W on path)      | Alpha-mask compositing: rasterize clip path to mask, snapshot dest, draw, lerp back |
| Blend mode on axis-aligned rect  | `lui_canvas_fill_rect_blended` (Multiply, Screen, Overlay, Darken, Lighten, Difference, Exclusion) |
| Blend mode on non-rect shapes    | Falls back to SRC_OVER                                    |

### Known gaps

1. **Polygon gradient fills** — lui_canvas has no polygon-gradient primitive.
   PDF axial/radial shadings on rectangular regions render correctly; on
   arbitrary shapes the gradient degrades to its mid-stop colour.
2. **Blend modes on non-rect shapes** — `lui_canvas_fill_rect_blended` is
   rectangle-only. Non-Normal blend modes on circles / arbitrary paths fall
   back to SRC_OVER.
3. **PDF-specific blend modes** — ColorDodge, ColorBurn, HardLight,
   SoftLight, Hue, Saturation, Color, Luminosity have no lui_canvas
   counterpart; they degrade to SRC_OVER.
4. **Per-pixel soft masks** — opacity is multiplied through correctly, but
   the per-pixel mask sampling that varies opacity across a paint isn't
   wired through `lui_canvas`.
