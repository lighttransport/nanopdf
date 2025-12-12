# pdfdump - PDF Structure Dump Tool

A CLI tool to dump PDF document structure information in YAML or JSON format.

## Features

- Document metadata (title, author, creator, producer, dates)
- Page layout information (dimensions, rotation)
- Font listing (name, type, encoding, embedded status)
- Image information (dimensions, color space, compression)
- Annotations summary
- Form fields summary
- Bookmarks/outlines summary
- **Revision history** with hashes and diff information

## Building

First, build the nanopdf library:

```bash
cd ../..
mkdir -p build && cd build
cmake .. && make
```

Then build pdfdump:

```bash
cd examples/pdfdump
./build.sh
# or manually:
mkdir -p build && cd build
cmake .. && make
```

## Usage

```
pdfdump <input.pdf> [options]

Options:
  -f, --format <yaml|json>  Output format (default: yaml)
  -o, --output <file>       Output to file instead of stdout
  -p, --page <n>            Dump specific page only (1-based)
  --no-fonts                Skip font information
  --no-images               Skip image information
  --verbose                 Include additional details
  --help                    Show help message
```

## Examples

```bash
# Basic usage (YAML output)
./build/pdfdump document.pdf

# JSON output
./build/pdfdump document.pdf -f json

# Save to file with verbose details
./build/pdfdump document.pdf -o info.yaml --verbose

# Dump only page 1
./build/pdfdump document.pdf -p 1

# Skip fonts and images
./build/pdfdump document.pdf --no-fonts --no-images
```

## Sample Output (YAML)

```yaml
document:
  pdf_version: 1.6
  title: Sample Document
  author: John Doe
  creator: Word
  producer: Adobe PDF Library
  creation_date: "D:20240101120000"
  page_count: 10
  encrypted: false
layout:
  pages:
    - number: 1
      width_pts: 612.00
      height_pts: 792.00
fonts:
  count: 2
  list:
    - name: F1
      base_font: Helvetica
      type: Type1
      embedded: false
    - name: F2
      base_font: TimesNewRoman
      type: TrueType
      embedded: true
      embedding_type: TrueType
images:
  count: 1
  list:
    - name: Im1
      page: 1
      width: 800
      height: 600
      bits_per_component: 8
      color_space: DeviceRGB
      filter: DCTDecode
annotations:
  count: 5
forms:
  field_count: 3
  signature_field_count: 0
outlines:
  has_bookmarks: true
  bookmark_count: 15
revisions:
  count: 2
  current_md5: abc123def456...
  current_sha256: abc123def456...
  history:
    - revision: 1
      size_bytes: 4500
      cumulative_size: 4500
      md5: def789abc012...
      added_objects: "1,2,3,4,5,6,7"
    - revision: 2
      size_bytes: 800
      cumulative_size: 5300
      md5: abc123def456...
      modified_objects: "3,5"
      added_objects: "8,9"
```

## Sample Output (JSON)

```json
{
  "document": {
    "pdf_version": "1.6",
    "title": "Sample Document",
    "page_count": 10,
    "encrypted": false
  },
  "layout": {
    "pages": [
      {"number": 1, "width_pts": 612.00, "height_pts": 792.00}
    ]
  },
  "fonts": {
    "count": 2,
    "list": [
      {"name": "F1", "base_font": "Helvetica", "type": "Type1", "embedded": false}
    ]
  },
  "images": {
    "count": 1,
    "list": [
      {"name": "Im1", "page": 1, "width": 800, "height": 600, "color_space": "DeviceRGB"}
    ]
  },
  "annotations": {
    "count": 5
  },
  "forms": {
    "field_count": 3,
    "signature_field_count": 0
  },
  "outlines": {
    "has_bookmarks": true
  },
  "revisions": {
    "count": 1,
    "current_md5": "abc123def456...",
    "current_sha256": "abc123def456...",
    "history": [
      {"revision": 1, "size_bytes": 4911, "md5": "abc123def456..."}
    ]
  }
}
```

## Revision History

PDFs can contain incremental updates (revisions). Each revision appends new data
to the file. pdfdump detects these revisions and reports:

- **Revision count**: Number of incremental updates
- **Cumulative hashes**: Hash of document data up to each revision
- **Diff info**: Objects added, modified, or deleted in each revision

This is useful for:
- Document forensics and version tracking
- Detecting modifications to signed PDFs
- Understanding document editing history

## Verbose Mode

With `--verbose`, additional details are included:

- Page dimensions in millimeters
- Font metrics (ascent, descent, family)
- Image raw and decoded sizes
- Full annotation and form field details
- Bookmark counts
- Revision SHA256 hashes and xref offsets
- Previous xref pointers for revision chain
