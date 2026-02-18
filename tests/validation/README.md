# PDF Validation Test Harness

Tests nanopdf against the PDF specification using the Arlington PDF model and
real-world PDF corpora.

## Quick Start

```bash
# Build everything
mkdir -p build && cd build && cmake .. && make

# Run validation tests (all will SKIP gracefully without corpora)
ctest -L validation --output-on-failure

# Download Arlington model + SafeDocs + pdf-differences
../scripts/download-corpora.sh arlington safedocs pdf-diff

# Run again — now tests exercise real data
ctest -L validation --output-on-failure

# Check what's downloaded
../scripts/download-corpora.sh status
```

## Corpora

| Corpus | Command | Description |
|--------|---------|-------------|
| Arlington | `arlington` | PDF DOM definition as TSV files (~613 files) |
| SafeDocs | `safedocs` | Hand-coded edge-case PDFs |
| pdf-differences | `pdf-diff` | 17 categories of interop test PDFs |
| CC-MAIN-2021 | `cc-main --count N` | Common Crawl PDF subset |
| GovDocs1 | `govdocs --count N` | US government document corpus |
| veraPDF | `verapdf` | PDF/A and PDF/UA validation corpus |
| pdf.js | `pdfjs` | Mozilla pdf.js test PDFs |
| pdfium | `pdfium` | Chromium pdfium test PDFs |
| Tika | `tika --count N` | Apache Tika issue tracker PDFs |
| unsafe-docs | `unsafe --count N` | DARPA SafeDocs malformed PDFs |

Download all: `./scripts/download-corpora.sh all`

## Test Structure

- **test_arlington_parser** — Unit tests for TSV parsing
- **test_arlington_validate** — Validate parsed PDFs against Arlington model
- **test_safedocs** — Parse SafeDocs corpus by category
- **test_pdf_differences** — Parse pdf-differences corpus by category
- **test_corpus_parsing** — Parse CC-MAIN, GovDocs, veraPDF, pdf.js, pdfium, Tika, unsafe-docs

## CMake

All validation tests are labeled `"validation"` in CTest:

```bash
ctest -L validation           # Run all validation tests
ctest -R test_arlington       # Run Arlington tests only
ctest -R test_corpus_parsing  # Run corpus parsing tests only
```

Build without validation tests:

```cmake
cmake .. -DNANOPDF_BUILD_VALIDATION_TESTS=OFF
```
