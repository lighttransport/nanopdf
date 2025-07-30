# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

nanopdf is a lightweight, minimal PDF parsing library written in C++11. It focuses on parsing PDF document structure, extracting text content, and decoding various stream filters. The library is designed to be embedded in other applications with minimal dependencies.

## Build System

This project uses CMake as its build system with C++11 standard requirement.

### Build Commands

```bash
# Standard CMake build
mkdir build
cd build
cmake ..
make

# Using the bootstrap script with Clang
./scripts/bootstrap-clang.sh
cd build
make

# Build specific targets
make nanopdf          # Build library
make test_nanopdf     # Build test executable
```

### CMake Options

- `NANOPDF_USE_CCACHE`: Use ccache for faster recompilation (default: ON)
- `NANOPDF_USE_NANOSTL`: Use nanostl instead of standard library (default: OFF)  
- `NANOPDF_USE_STB_TRUETYPE`: Use stb_truetype for font loading (default: ON)
- `SANITIZE_ADDRESS`: Enable address sanitizer for debugging

### Testing

Run the test executable with a PDF file:
```bash
./test_nanopdf path/to/file.pdf
```

## Code Architecture

### Core Components

**nanopdf.hh/nanopdf.cc**: Main parsing engine containing:
- `Pdf` struct: Main document container with xref tables, trailer, and document catalog
- `Value` class: Polymorphic type system for PDF objects (boolean, number, string, name, array, dictionary, stream, reference)
- `Parser` class: Low-level PDF syntax parser
- `DocumentCatalog`: High-level document structure with pages
- `Page` struct: Individual page with resources, content streams, and font information

**stream-reader.hh**: Low-level byte stream reader with endianness handling for binary data parsing.

**jbig2-decoder.hh/cc**: JBIG2 monochrome bitmap decoder for compressed image streams.

**ttf-loader.cc**: TrueType font file loader and parser.

### Key Data Structures

- `XRef`/`XRefSection`: Cross-reference table entries for object location tracking
- `BaseFont`/`FontDescriptor`: Font resource management with metrics and embedded font data
- `DecodedStream`: Result container for stream filter decoding operations
- `ResolvedObject`: Container for dereferenced PDF objects with error handling

### Stream Filters

The `filters` namespace provides decoders for:
- FlateDecode (zlib/deflate compression)
- ASCII85Decode (Base85 encoding)
- LZWDecode (Lempel-Ziv-Welch compression)
- JBIG2Decode (monochrome bitmap compression)

### Dependencies

- **Required**: zlib for FlateDecode stream decompression
- **Embedded**: miniz.c/h, stb_truetype.h, stb_image.h, tiny_dng_loader.h, fpng for various format support
- **Optional**: nanostl for reduced standard library footprint

## Development Patterns

### Error Handling
Functions return boolean success/failure or use result structs with `success` flag and `error` message fields.

### Memory Management  
Uses RAII with smart pointers (`std::unique_ptr`) for font resources and manual resource cleanup in destructors.

### PDF Object Resolution
Objects are resolved through the `resolve_reference()` function using object number and generation number from xref tables.

### Stream Processing
Stream data is decoded through a pipeline: raw bytes → filter decoding → final decoded content using the `decode_stream()` function.