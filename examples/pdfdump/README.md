# pdfdump - PDF Structure Dump Tool

A CLI tool to dump PDF document structure information in YAML or JSON format.

## Features

- Document metadata (title, author, creator, producer, dates)
- Page layout information (dimensions, rotation)
- Font listing (name, type, encoding, embedded status)
- Image information (dimensions, color space, compression)
- Annotations summary
- Form fields summary
- **Digital signatures** with integrity verification
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

## Digital Signatures

pdfdump detects and reports digital signatures in PDF documents, including
MDP (Modification Detection and Prevention) certification signatures.

```yaml
signatures:
  total_fields: 2
  signed_count: 2
  list:
    - name: Signature1
      is_signed: true
      reason: "Document certified"
      location: "Tokyo, Japan"
      date: "D:20240101120000+09'00'"
      algorithm: SHA256
      filter: Adobe.PPKLite
      subfilter: adbe.pkcs7.detached
      type: certification
      mdp_permissions: 2
      allowed_changes: form_fill_and_sign
      byte_range: "[0, 1234, 5678, 9012]"
      integrity: intact
    - name: Signature2
      is_signed: true
      type: approval
      integrity: intact
```

**Signature types:**
- `certification` - MDP/DocMDP signature that certifies the document
- `approval` - Regular approval signature

**MDP permission levels (mdp_permissions):**
- `1` (no_changes) - No changes allowed after signing
- `2` (form_fill_and_sign) - Only form filling and additional signatures allowed
- `3` (form_fill_sign_annotate) - Form filling, signatures, and annotations allowed

**Integrity status values:**
- `intact` - Document unchanged since signing
- `modified_after_signing` - Content appended after signature
- `invalid_byte_range` - Signature byte range is malformed
- `no_byte_range` - No byte range specified
- `not_signed` - Signature field exists but is unsigned

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
- Signature page references and rectangles
- Bookmark counts
- Revision SHA256 hashes and xref offsets
- Previous xref pointers for revision chain
