# LightVG — Portable 2D Vector Graphics Rasterizer

Pure-C99 software rasterizer for ARGB32 surfaces.

**Zero dependencies by default** — no libm, no stdio, no heap required for
the core rasterizer. Feature-comparable to blend2d / thorvg for the
implemented subset.

## Features

- Path fill / stroke with non-zero and even-odd winding rules
- Cubic and quadratic Bezier flattening (de Casteljau subdivision)
- Anti-aliased lines (Xiaolin Wu) and coverage-based scanline AA
- Thick polylines with butt / round / miter / bevel caps and joins
- Dashed / dotted stroke patterns
- Linear and radial gradients (up to 16 color stops)
- Porter-Duff SRC_OVER and 8 additional blend modes (multiply, screen,
  overlay, darken, lighten, difference, exclusion, plus)
- Image blit / scale with bilinear and Lanczos filters
- Layered compositing with dirty-region tracking (optional, `scene.h`)

## Portability

### libm-free math
The rasterizer never calls libm. It uses the header-only `math.h`, which
provides libm-free replacements for `sqrtf / sinf / cosf / floorf / ceilf
/ roundf / fabsf` (and `double` variants for the Lanczos filter). Accuracy
is sufficient for sub-pixel rasterization (sqrt is near-IEEE; trig max error
~3e-6 in float).

Override any single routine with your own before the module is compiled:
```c
-DLVG_SQRTF=my_sqrt   -DLVG_SINF=my_sin   ...
```

Verification: `nm surface.c.o canvas.c.o` shows no undefined libm symbols
when PNG is disabled.

### Allocator callbacks / macros
The core rasterizer does not allocate. Only the convenience constructors
`lvg_surface_create/resize/destroy` call malloc-style functions.

Three ways to plug in your own allocator:
1. **Macro override** (compile-time, global):
   ```
   -DLVG_MALLOC=my_alloc -DLVG_FREE=my_free
   ```
2. **Callback form** (runtime, per-surface):
   ```c
   lvg_surface_t *s = lvg_surface_create_ex(w, h, my_alloc, my_free, ud);
   ...
   lvg_surface_destroy_ex(s, my_free, ud);
   ```
3. **Wrap caller-owned memory** (zero allocation):
   ```c
   uint32_t pix[W*H];
   lvg_surface_t s = lvg_surface_wrap(pix, W, H, W);
   ```

### Writer callbacks
Encoders are factored as stream-writers, so they have no stdio dependency:
```c
size_t my_write(void *ud, const void *buf, size_t n) { ... }
lvg_surface_write_ppm(surf, my_write, userdata);
lvg_surface_write_png(surf, my_write, userdata);         /* LVG_ENABLE_PNG  */
lvg_surface_write_jpeg(surf, my_write, userdata, 90);    /* LVG_ENABLE_JPEG */
```
JPEG `quality` is clamped to `[1, 100]` (75 ≈ web default, 90 ≈ visually
lossless). Alpha is discarded for JPEG.

The `lvg_surface_save_*` helpers (which take a file path) are thin stdio
wrappers and are compiled only when `LVG_NO_STDIO` is *not* defined.
`lvg_surface_save(surf, path)` dispatches on extension: `.png`, `.ppm`,
`.jpg`/`.jpeg` (JPEG uses default quality 90; call `lvg_surface_save_jpeg`
to control it).

### PNG / JPEG are opt-in
PNG and JPEG encoding both use the vendored `stb_image_write.h`, which
itself calls libm. Each format is compiled only when its flag is set:

| Flag                    | Enables                                         |
|-------------------------|-------------------------------------------------|
| (none, default)         | PPM only — libm-free, no extra deps             |
| `LVG_ENABLE_PNG`     | + `lvg_surface_write_png` / `lvg_surface_save_png` |
| `LVG_ENABLE_JPEG`    | + `lvg_surface_write_jpeg` / `lvg_surface_save_jpeg` |

Without any flag, the disabled writers return `-1` and the module stays
libm-free. PPM is always available and has no heavy dependencies.

## Files

```
lightvg/src/
  surface.c   — ARGB32 pixel buffer + PPM writer (+ PNG/JPEG if enabled)
  canvas.c    — path rasterizer, gradients, stroking, blending
  backend_registry.c — optional runtime backend registry used by canvas.c
  dirty.c     — dirty-rectangle tracking
  layer.c     — cached off-screen surfaces
  scene.c     — layered compositor (uses layer/dirty)

lightvg/include/lightvg/
  types.h     — color, point, size, rect primitives
  math.h      — libm-free sqrtf/sinf/cosf/... (header-only)
  surface.h
  canvas.h
  canvas_backend.h
  dirty.h
  layer.h
  scene.h
  lightvg.h   — umbrella header
```

## Dependencies

| Component       | Default | With `LVG_ENABLE_PNG` |
|-----------------|---------|--------------------------|
| `types.h`    | none    | none                     |
| `math.h`     | none    | none                     |
| `canvas.c`      | none    | none                     |
| `surface.c`     | none    | `stb_image_write.h`, libm (via stb) — also activated by `LVG_ENABLE_JPEG` |
| `scene.c`       | `layer.h`, `dirty.h`; drop this file if you only need the rasterizer |

## Minimal build (another project)

Copy `lightvg/src/` and `lightvg/include/lightvg/` into the target project.

```sh
cc -std=c99 -I./lightvg/include -c \
  lightvg/src/surface.c \
  lightvg/src/backend_registry.c \
  lightvg/src/canvas.c
# No -lm, no other dependencies.
```

## License

Apache-2.0 (same as lightui).
