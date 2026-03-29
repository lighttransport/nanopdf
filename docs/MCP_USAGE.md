# nanopdf MCP Server

Model Context Protocol (MCP) server for nanopdf - enables AI assistants like Claude Desktop to interact with PDF documents through a standardized protocol.

## Overview

The nanopdf MCP server provides AI assistants with tools to:
- Load and parse PDF documents
- Extract text content from pages
- Search within PDFs
- Access document metadata and structure
- Inspect fonts, images, and other PDF elements
- Analyze text layout with position information

## Installation

### Build from Source

```bash
# Standard build
mkdir build && cd build
cmake ..
make nanopdf-mcp

# The executable will be at: build/nanopdf-mcp
```

### Install

```bash
# From build directory
sudo make install

# Or copy manually
sudo cp nanopdf-mcp /usr/local/bin/
```

## Configuration

### Claude Desktop

Add the MCP server to Claude Desktop's configuration file:

**Location:**
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
- Linux: `~/.config/Claude/claude_desktop_config.json`
- Windows: `%APPDATA%\Claude\claude_desktop_config.json`

**Configuration:**

```json
{
  "mcpServers": {
    "nanopdf": {
      "command": "/path/to/nanopdf-mcp"
    }
  }
}
```

Replace `/path/to/nanopdf-mcp` with the actual path to your executable.

### Restart Claude Desktop

After adding the configuration, restart Claude Desktop for the changes to take effect.

## Available Tools

The nanopdf MCP server provides 14 tools for PDF interaction:

### 1. load_pdf

Load a PDF document from file path or base64 data.

**Parameters:**
- `path` (string, optional): Path to PDF file
- `data` (string, optional): Base64-encoded PDF data

**Note:** Provide either `path` or `data`, not both.

**Example:**
```
Load the PDF file at /home/user/document.pdf
```

**Returns:**
- `pageCount`: Total number of pages
- `filename`: Name of the loaded file
- `metadata`: Document metadata (title, author, etc.)

### 2. get_page_count

Get the total number of pages in the loaded PDF.

**Parameters:** None

**Example:**
```
How many pages does this PDF have?
```

**Returns:**
- `count`: Number of pages

### 3. extract_text

Extract text content from one or more pages.

**Parameters:**
- `page` (number, optional): Single page to extract (0-indexed)
- `start_page` (number, optional): Start page for range extraction
- `end_page` (number, optional): End page for range extraction

**Example:**
```
Extract text from page 0
Extract text from pages 0 to 5
```

**Returns:**
- `text`: Extracted text content
- `start_page`: Starting page number
- `end_page`: Ending page number

### 4. get_page_info

Get page dimensions and metadata.

**Parameters:**
- `page` (number, required): Page number (0-indexed)

**Example:**
```
What are the dimensions of page 0?
```

**Returns:**
- `page`: Page number
- `width`: Page width in points
- `height`: Page height in points
- `rotation`: Rotation angle (0, 90, 180, 270)

### 5. get_metadata

Get PDF document metadata (title, author, creation date, etc.).

**Parameters:** None

**Example:**
```
What metadata does this PDF contain?
```

**Returns:**
- `Title`: Document title
- `Author`: Document author
- `Subject`: Document subject
- `Keywords`: Document keywords
- `Creator`: Application that created the document
- `Producer`: PDF producer
- `CreationDate`: Creation date
- `ModDate`: Modification date
- Additional custom metadata fields

### 6. extract_text_layout

Extract text with position and layout information.

**Parameters:**
- `page` (number, required): Page number (0-indexed)

**Example:**
```
Extract text with layout information from page 0
```

**Returns:**
- `page`: Page number
- `pageWidth`: Page width
- `pageHeight`: Page height
- `chars`: Array of character objects with:
  - `c`: Character
  - `x`, `y`: Position
  - `w`, `h`: Dimensions
  - `fontSize`: Font size
  - `fontName`: Font name

### 7. find_text

Search for text within a specific page.

**Parameters:**
- `page` (number, required): Page number to search
- `query` (string, required): Text to search for

**Example:**
```
Find the word "introduction" in page 0
```

**Returns:**
- `page`: Page number
- `query`: Search query
- `matchCount`: Number of matches found
- `matches`: Array of match objects with:
  - `position`: Character position in text
  - `context`: Surrounding text context

### 8. get_fonts

List fonts used in the document or a specific page.

**Parameters:**
- `page` (number, optional): Page number (0-indexed). If omitted, returns all fonts in document

**Example:**
```
What fonts are used in this PDF?
What fonts are used on page 0?
```

**Returns:**
- `fonts`: Array of font objects with:
  - `name`: Font resource name
  - `subtype`: Font type (Type1, TrueType, Type0, etc.)
  - `baseFont`: PostScript font name
  - `embedded`: Whether font is embedded in PDF

### 9. get_images

List images in a specific page.

**Parameters:**
- `page` (number, required): Page number (0-indexed)

**Example:**
```
What images are on page 0?
```

**Returns:**
- `page`: Page number
- `images`: Array of image objects with:
  - `name`: Image resource name
  - `width`: Image width in pixels
  - `height`: Image height in pixels
  - `colorSpace`: Color space (DeviceRGB, DeviceGray, etc.)

### 10. close_pdf

Close the currently loaded PDF and free resources.

**Parameters:** None

**Example:**
```
Close the current PDF
```

**Returns:**
- `success`: true

### 11. query_region

Extract PDF object info for a specified coordinate region. Returns text spans with bounding boxes, font metadata, text direction/rotation, and line grouping. Useful for correlating image recognition results with PDF text data.

**Parameters:**
- `page` (required): Page number (0-indexed)
- `x1` (required): Left X coordinate in PDF points
- `y1` (required): Bottom Y coordinate in PDF points
- `x2` (required): Right X coordinate in PDF points
- `y2` (required): Top Y coordinate in PDF points

**Example:**
```
Query region (100, 700, 400, 750) on page 0
```

**Returns:**
- `page`: Page number
- `queryRect`: The queried rectangle `{x1, y1, x2, y2}`
- `pageWidth`, `pageHeight`: Page dimensions in PDF points
- `textSpans`: Array of text spans found in the region, each containing:
  - `text`: The text content
  - `fontName`: Font name
  - `fontSize`: Font size in points
  - `rotation`: Rotation angle in degrees
  - `direction`: Text direction string (`ltr-horizontal`, `vertical-up`, `vertical-down`, `rtl-horizontal`, or `rotated-N`)
  - `bbox`: Bounding box `{x, y, width, height}` in PDF points
  - `lineIndex`: Line group index (if detected)
- `textSpanCount`: Number of text spans
- `charCount`: Number of individual characters in the region
- `pageImages`: Array of image XObjects on the page (name, width, height, colorSpace)

**Note:** Each text span also includes a `writingMode` field (`"horizontal"` or `"vertical"`) based on whether the font has vertical metrics (Type0 CJK fonts with vertical writing).

### 12. get_page_structure

Get structured text layout (lines and words) for a page. Lighter than `extract_text_layout` which returns per-character data.

**Parameters:**
- `page` (number, required): Page number (0-indexed)

**Returns:**
- `page`: Page number
- `pageWidth`, `pageHeight`: Page dimensions
- `numColumns`: Detected number of text columns
- `lines`: Array of line objects with:
  - `text`: Line text content
  - `bbox`: Bounding box `{x, y, width, height}`
  - `readingOrder`: Sequence in reading order
  - `rotation`: Rotation angle in degrees
  - `isRtl`: Whether the line is right-to-left
  - `baseline`: Y-coordinate of the text baseline
- `words`: Array of word objects with:
  - `text`: Word text content
  - `bbox`: Bounding box `{x, y, width, height}`
  - `lineIndex`: Index of the line this word belongs to

### 13. query_annotations

List annotations on a page, optionally filtered by a coordinate region.

**Parameters:**
- `page` (number, required): Page number (0-indexed)
- `x1`, `y1`, `x2`, `y2` (number, optional): Region to filter annotations by overlap

**Returns:**
- `page`: Page number
- `annotationCount`: Number of annotations returned
- `annotations`: Array of annotation objects with:
  - `type`: Annotation type (Text, Link, FreeText, Highlight, Widget, etc.)
  - `rect`: Bounding rectangle `{x1, y1, x2, y2}`
  - `contents`: Text content (if any)
  - `name`: Annotation name (if any)
  - For Link annotations: `actionType`, `uri`
  - For Widget annotations: `fieldType`, `fieldName`, `fieldValue`
  - For FreeText annotations: `defaultAppearance`

### 14. get_image_placements

Get placement information for all images on a page, including position, display size, and the full CTM (current transformation matrix).

**Parameters:**
- `page` (number, required): Page number (0-indexed)

**Returns:**
- `page`: Page number
- `count`: Number of image placements
- `imagePlacements`: Array of placement objects with:
  - `name`: Image XObject resource name
  - `imageWidth`, `imageHeight`: Native image dimensions in pixels
  - `ctm`: 6-element array `[a, b, c, d, e, f]` (current transformation matrix)
  - `x`, `y`: Display position in PDF points (from CTM translation)
  - `displayWidth`, `displayHeight`: Display dimensions in PDF points (from CTM scaling)

## Usage Examples

### Basic Workflow

```
1. Load PDF: "Load the PDF file at /home/user/report.pdf"
2. Get info: "How many pages does it have?"
3. Extract text: "Extract text from page 0"
4. Search: "Search for the word 'summary' in page 0"
5. Close: "Close the PDF"
```

### Analyzing Document Structure

```
1. "Load /home/user/technical_doc.pdf"
2. "What metadata does this document have?"
3. "What fonts are used in this PDF?"
4. "What are the dimensions of page 0?"
```

### Text Extraction with Layout

```
1. "Load /home/user/invoice.pdf"
2. "Extract text with layout information from page 0"
3. "Find all occurrences of 'Total' in page 0"
```

## Troubleshooting

### Server Not Appearing in Claude Desktop

1. Check that the path in `claude_desktop_config.json` is correct and absolute
2. Verify the executable has execute permissions: `chmod +x nanopdf-mcp`
3. Restart Claude Desktop completely
4. Check Claude Desktop's developer console for errors

### PDF Loading Errors

- **"Failed to parse PDF"**: The PDF file may be corrupted or encrypted
- **"Failed to load document structure"**: The PDF structure is malformed
- **"File open error"**: Check file path and permissions

### Tool Errors

- **"No PDF loaded"**: You must load a PDF first using `load_pdf`
- **"Page index out of range"**: Check the page count and use 0-indexed page numbers
- **"Missing required parameter"**: Ensure all required parameters are provided

## Protocol Details

The nanopdf MCP server implements:
- **Protocol**: MCP (Model Context Protocol) 2024-11-05
- **Transport**: stdio (newline-delimited JSON)
- **Capabilities**: tools
- **JSON-RPC**: 2.0

## Performance Notes

- PDFs are cached in memory while loaded
- Use `close_pdf` to free memory when done
- Large PDFs (>100MB) may take longer to load
- Text extraction is performed on-demand per page

## Security Considerations

- The server can only access files the process has permission to read
- No network access required
- No data is sent to external services
- PDFs are processed locally

## Development

### Building with Debug Info

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make nanopdf-mcp
```

### Running Manually

```bash
# The server reads JSON-RPC from stdin and writes to stdout
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | ./nanopdf-mcp
```

### Logging

Server logs are written to stderr with timestamps:
```
[2025-02-15 12:34:56] [INFO] nanopdf MCP server starting...
[2025-02-15 12:34:56] [INFO] Server initialized, waiting for requests...
```

## License

MIT License - See LICENSE file for details

## Support

- Issues: https://github.com/syoyo/nanopdf/issues
- Documentation: https://github.com/syoyo/nanopdf
- MCP Specification: https://modelcontextprotocol.io

## Version

nanopdf MCP server version 0.1.0
