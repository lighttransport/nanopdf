# PDF Rasterizer

A command-line tool that converts PDF pages to raster images using nanopdf.
LightVG is the default backend and does not require an external graphics
library. ThorVG can be enabled as an optional backend for comparison.

## Building

Build nanopdf first, then build the example against that build directory:

```bash
cd ../..
cmake -S . -B build -DNANOPDF_USE_LIGHTVG=ON
cmake --build build

cd examples/rasterize
cmake -S . -B build -DNANOPDF_BUILD_DIR=../../build
cmake --build build
```

To enable the optional ThorVG backend, build nanopdf with
`-DNANOPDF_USE_THORVG=ON` and configure this example with `-DUSE_THORVG=ON`.

## Usage

```bash
./rasterize input.pdf output.png [options]
```

Options:

- `--backend <name>`: Renderer, `lightvg` by default; `thorvg` if compiled in; `list` prints compiled backend availability
- `-p, --page <n>`: Page number to render, default `1`
- `--pages <spec>`: Page selection, for example `1-3,7,10-12`
- `--all`: Render every page
- `--dpi <n>`: DPI for rendering, default `150`
- `-w, --width <n>` / `-h, --height <n>`: Explicit output dimensions
- `-s, --scale <f>`: Scale factor; overrides width/height/DPI sizing
- `-r, --rotate <n>`: Rotate output by `0`, `90`, `180`, or `270`
- `-g, --grayscale`: Convert output to grayscale
- `--format <name>`: `png`, `jpg`, `bmp`, or `tga`; default `png`
- `--jpeg-quality <n>`: JPEG quality `1-100`, default `90`
- `--png-compression <n>`: PNG compression `0-9`, default `6`
- `--verbose`: Print page details and render progress for dense pages
- `--log-level <n>`: `0=none`, `1=error`, `2=warn`, `3=info`, `4=debug`, `5=trace`

`--all`, `--page`, and `--pages` are mutually exclusive.

For multi-page output, page numbers use a four-digit suffix:

- `output.png` becomes `output-0001.png`, `output-0002.png`, ...
- If the output stem contains `0000`, that placeholder is replaced.

## Examples

```bash
./rasterize document.pdf output.png
./rasterize document.pdf output.png -p 2 --dpi 300
./rasterize document.pdf output.png --pages 1-3,7,10-12
./rasterize document.pdf output.jpg --format jpg --jpeg-quality 85
./rasterize document.pdf output.png --all --dpi 150
```

## Notes

Sizing priority is `--scale`, then explicit width/height, then `--dpi`.
When only width or height is provided, the page aspect ratio is preserved.
When neither is provided, the default 150 DPI output matches common
`pdftoppm` visual-regression baselines.
