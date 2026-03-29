# PDF to PNG Rasterizer

A command-line tool that converts PDF pages to PNG images using nanopdf and the ThorVG rendering backend.

## Features

- Render single or all pages from a PDF document
- Customizable output resolution (width, height, scale, DPI)
- Maintains aspect ratio by default
- Batch processing for multi-page documents
- Verbose output mode for debugging
- Progress updates for dense pages while rendering (`--verbose`)

## Prerequisites

1. **Build nanopdf with ThorVG support:**
   ```bash
   cd ../../  # Go to nanopdf root
   mkdir build && cd build
   cmake .. -DNANOPDF_USE_THORVG=ON
   make
   ```

2. **Install ThorVG library:**
   ```bash
   cd ../../scripts  # From nanopdf root
   ./install-thorvg.sh
   ```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

### Basic Usage

Convert the first page of a PDF to PNG:
```bash
./rasterize input.pdf output.png
```

### Options

- `-p, --page <n>` : Page number to render (default: 1)
- `-w, --width <n>` : Output width in pixels (default: 800)
- `-h, --height <n>` : Output height in pixels (default: 600)
- `-s, --scale <f>` : Scale factor (overrides width/height)
- `--dpi <n>` : DPI for rendering (default: 72)
- `--all` : Render all pages (creates multiple PNG files)
- `--verbose` : Verbose output
- `--help` : Show help message

### Examples

**Render page 2 at specific resolution:**
```bash
./rasterize document.pdf output.png -p 2 -w 1024 -h 768
```

**Render all pages at 150 DPI:**
```bash
./rasterize document.pdf output.png --all --dpi 150
```
This creates: `output_page001.png`, `output_page002.png`, etc.

**Render with 2x scale factor:**
```bash
./rasterize document.pdf output.png -s 2.0
```

**Render with verbose output:**
```bash
./rasterize document.pdf output.png --verbose
```
When the page contains many render objects, verbose mode prints render progress in 1% steps.

## Output Format

- Single page: Creates the specified output file
- Multiple pages (`--all`): Creates numbered files with pattern `basename_pageNNN.png`

## Limitations

- Requires ThorVG library to be installed
- Text rendering is currently simplified (placeholder implementation)
- Some advanced PDF features may not be fully supported
- Performance depends on PDF complexity and output resolution

## Troubleshooting

**"ThorVG support not enabled" error:**
- Rebuild nanopdf with `-DNANOPDF_USE_THORVG=ON`
- Ensure ThorVG is installed on your system

**"Failed to initialize ThorVG backend" error:**
- Check ThorVG installation
- Verify ThorVG library is in your library path

**Poor rendering quality:**
- Increase DPI: `--dpi 150` or `--dpi 300`
- Use scale factor: `-s 2.0` for 2x resolution
- Specify larger dimensions: `-w 2048 -h 1536`

## Technical Details

The rasterizer uses:
- **nanopdf**: For parsing PDF structure and content
- **ThorVG**: For vector graphics rendering
- **stb_image_write**: For PNG output (embedded in ThorVG backend)

The rendering pipeline:
1. Parse PDF with nanopdf
2. Extract page content and resources
3. Convert PDF operations to ThorVG drawing commands
4. Render to bitmap buffer
5. Save as PNG file

## License

Apache 2.0 - See LICENSE file in the root directory
