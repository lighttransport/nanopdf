#!/usr/bin/env python3
"""
embed_fonts_binary.py - Generate ELF-linked embedded fonts (much faster to compile
than giant C hex arrays).

Writes:
  - <out_dir>/<bin_subdir>/<font_id>.bin       compressed font data
  - <out_dir>/<base>.hh                        declarations (small)
  - <out_dir>/<base>.S                         assembler with .incbin directives
  - <out_dir>/<base>.cc                        registry + helper functions

Usage:
  embed_fonts_binary.py <fonts_dir> <out_dir> <base_name> [options]

Options:
  --exclude-dirs=a,b
  --include-only-dirs=a,b
  --header-guard=NAME
  --namespace=NAME            default: embedded_fonts
  --symbol-prefix=PREFIX      default: nanopdf_font_ (per-font symbol prefix)
  --skip-pdf-mapping
"""

import sys
import os
import glob
import zlib


def c_identifier(name):
    return name.replace('-', '_').replace(' ', '_').replace('.', '_').lower()


def parse_options(args):
    opts = {
        'exclude_dirs': [],
        'include_only_dirs': [],
        'header_guard': 'NANOPDF_EMBEDDED_FONTS_HH',
        'namespace': 'embedded_fonts',
        'symbol_prefix': 'nanopdf_font_',
        'skip_pdf_mapping': False,
    }
    for arg in args:
        if arg.startswith('--exclude-dirs='):
            opts['exclude_dirs'] = [d.strip() for d in arg.split('=', 1)[1].split(',') if d.strip()]
        elif arg.startswith('--include-only-dirs='):
            opts['include_only_dirs'] = [d.strip() for d in arg.split('=', 1)[1].split(',') if d.strip()]
        elif arg.startswith('--header-guard='):
            opts['header_guard'] = arg.split('=', 1)[1]
        elif arg.startswith('--namespace='):
            opts['namespace'] = arg.split('=', 1)[1]
        elif arg.startswith('--symbol-prefix='):
            opts['symbol_prefix'] = arg.split('=', 1)[1]
        elif arg == '--skip-pdf-mapping':
            opts['skip_pdf_mapping'] = True
        else:
            print(f"Warning: Unknown option: {arg}", file=sys.stderr)
    return opts


def filter_font_files(font_files, fonts_dir, opts):
    if not opts['exclude_dirs'] and not opts['include_only_dirs']:
        return font_files
    filtered = []
    for f in font_files:
        rel = os.path.relpath(f, fonts_dir)
        parts = rel.replace('\\', '/').split('/')
        subdir = parts[0] if len(parts) > 1 else ''
        if opts['include_only_dirs']:
            if subdir in opts['include_only_dirs']:
                filtered.append(f)
        elif opts['exclude_dirs']:
            if subdir not in opts['exclude_dirs']:
                filtered.append(f)
        else:
            filtered.append(f)
    return filtered


def write_if_changed(path, content_bytes):
    """Write only if content changed — avoids triggering unnecessary rebuilds."""
    if os.path.exists(path):
        with open(path, 'rb') as f:
            if f.read() == content_bytes:
                return False
    with open(path, 'wb') as f:
        f.write(content_bytes)
    return True


def generate(fonts_dir, out_dir, base_name, opts):
    hh_path = os.path.join(out_dir, base_name + '.hh')
    s_path = os.path.join(out_dir, base_name + '.S')
    cc_path = os.path.join(out_dir, base_name + '.cc')
    bin_dir = os.path.join(out_dir, base_name + '_bin')
    os.makedirs(bin_dir, exist_ok=True)

    font_files = []
    for ext in ('*.ttf', '*.otf'):
        font_files.extend(glob.glob(os.path.join(fonts_dir, '**', ext), recursive=True))
    font_files = filter_font_files(font_files, fonts_dir, opts)
    if not font_files:
        print(f"Error: no font files in {fonts_dir} (after filtering)", file=sys.stderr)
        sys.exit(1)
    font_files.sort()

    header_guard = opts['header_guard']
    namespace = opts['namespace']
    sym_prefix = opts['symbol_prefix']

    entries = []  # (font_name, font_id, sym, compressed_size, original_size, filename, bin_path)
    total_orig = 0
    total_comp = 0

    for f in font_files:
        fname = os.path.splitext(os.path.basename(f))[0]
        fext = os.path.splitext(f)[1]
        fid = c_identifier(fname)
        sym = sym_prefix + fid
        with open(f, 'rb') as bf:
            raw = bf.read()
        comp = zlib.compress(raw, level=9)
        bin_path = os.path.join(bin_dir, fid + '.bin')
        write_if_changed(bin_path, comp)
        entries.append((fname, fid, sym, len(comp), len(raw), fname + fext, bin_path))
        total_orig += len(raw)
        total_comp += len(comp)
        print(f"Embedded: {fname}{fext} -> {fid} ({len(raw):,} -> {len(comp):,})")

    # ---- Header (declarations only) ----
    hh = []
    hh.append("// Auto-generated - do not edit\n")
    hh.append(f"#ifndef {header_guard}\n#define {header_guard}\n\n")
    hh.append("#include <cstddef>\n#include <cstdint>\n#include <vector>\n\n")
    hh.append("namespace nanopdf {\n")
    hh.append(f"namespace {namespace} {{\n\n")
    hh.append("struct FontEntry {\n"
              "  const char* base_name;\n"
              "  const unsigned char* compressed_data;\n"
              "  std::size_t compressed_size;\n"
              "  std::size_t original_size;\n"
              "  const char* filename;\n"
              "};\n\n")
    hh.append("extern const FontEntry font_registry[];\n")
    hh.append("extern const std::size_t font_count;\n\n")
    if not opts['skip_pdf_mapping']:
        hh.append("struct FontMapping { const char* pdf_name; const char* substitute_name; };\n")
        hh.append("extern const FontMapping pdf_standard_14_mapping[];\n")
        hh.append("extern const std::size_t pdf_mapping_count;\n\n")
    hh.append("bool decompress_font(const FontEntry* entry, std::vector<std::uint8_t>& output);\n")
    hh.append("bool decompress_font_to_buffer(const FontEntry* entry, unsigned char* buffer, std::size_t buffer_size);\n")
    hh.append("const FontEntry* find_font(const char* name);\n")
    if not opts['skip_pdf_mapping']:
        hh.append("const FontEntry* get_pdf_standard_font(const char* pdf_name);\n")
    hh.append(f"\n}}  // namespace {namespace}\n")
    hh.append("}  // namespace nanopdf\n\n")
    hh.append(f"#endif  // {header_guard}\n")
    write_if_changed(hh_path, ''.join(hh).encode())

    # ---- Assembler with .incbin ----
    s = []
    s.append("// Auto-generated - do not edit\n")
    s.append("#if defined(__linux__) && (defined(__ELF__) || defined(__linux))\n")
    s.append("    .section .note.GNU-stack,\"\",@progbits\n")
    s.append("#endif\n")
    s.append("#if defined(__APPLE__)\n")
    s.append("    .section __TEXT,__const\n")
    s.append("#else\n")
    s.append("    .section .rodata\n")
    s.append("#endif\n\n")
    for _, fid, sym, comp_sz, _, _, bin_path in entries:
        # Use forward slashes for portability in .incbin paths
        inc_path = bin_path.replace('\\', '/')
        # Underscore-prefixed symbol on macOS, plain on ELF
        s.append("#if defined(__APPLE__)\n")
        s.append(f"    .globl _{sym}_start\n")
        s.append(f"_{sym}_start:\n")
        s.append(f'    .incbin "{inc_path}"\n')
        s.append(f"    .globl _{sym}_end\n")
        s.append(f"_{sym}_end:\n")
        s.append("#else\n")
        s.append(f"    .globl {sym}_start\n")
        s.append(f"    .type  {sym}_start, @object\n")
        s.append(f"    .align 16\n")
        s.append(f"{sym}_start:\n")
        s.append(f'    .incbin "{inc_path}"\n')
        s.append(f"    .globl {sym}_end\n")
        s.append(f"{sym}_end:\n")
        s.append(f"    .size {sym}_start, {comp_sz}\n")
        s.append("#endif\n\n")
    write_if_changed(s_path, ''.join(s).encode())

    # ---- Registry + helpers ----
    cc = []
    cc.append("// Auto-generated - do not edit\n")
    cc.append(f"#include \"{os.path.basename(hh_path)}\"\n")
    cc.append("#include <cstring>\n\n")
    cc.append("#ifdef NANOPDF_USE_MINIZ\n#include \"miniz.h\"\n#else\n#include <zlib.h>\n#endif\n\n")

    cc.append('extern "C" {\n')
    for _, _, sym, _, _, _, _ in entries:
        cc.append(f"extern const unsigned char {sym}_start[];\n")
    cc.append("}\n\n")

    cc.append("namespace nanopdf {\n")
    cc.append(f"namespace {namespace} {{\n\n")

    cc.append("const FontEntry font_registry[] = {\n")
    for fname, _, sym, comp_sz, orig_sz, filename, _ in entries:
        cc.append(f'  {{ "{fname}", {sym}_start, {comp_sz}u, {orig_sz}u, "{filename}" }},\n')
    cc.append("};\n")
    cc.append("const std::size_t font_count = sizeof(font_registry) / sizeof(font_registry[0]);\n\n")

    if not opts['skip_pdf_mapping']:
        cc.append("const FontMapping pdf_standard_14_mapping[] = {\n")
        cc.append('  { "Helvetica",             "Arimo-Regular" },\n')
        cc.append('  { "Helvetica-Bold",        "Arimo-Bold" },\n')
        cc.append('  { "Helvetica-Oblique",     "Arimo-Italic" },\n')
        cc.append('  { "Helvetica-BoldOblique", "Arimo-BoldItalic" },\n')
        cc.append('  { "Times-Roman",           "Tinos-Regular" },\n')
        cc.append('  { "Times-Bold",            "Tinos-Bold" },\n')
        cc.append('  { "Times-Italic",          "Tinos-Italic" },\n')
        cc.append('  { "Times-BoldItalic",      "Tinos-BoldItalic" },\n')
        cc.append('  { "Courier",               "Cousine-Regular" },\n')
        cc.append('  { "Courier-Bold",          "Cousine-Bold" },\n')
        cc.append('  { "Courier-Oblique",       "Cousine-Italic" },\n')
        cc.append('  { "Courier-BoldOblique",   "Cousine-BoldItalic" },\n')
        cc.append('  { "Symbol",                "STIXTwoMath-Regular" },\n')
        cc.append('  { "ZapfDingbats",          "NotoSansSymbols-Regular" },\n')
        cc.append("};\n")
        cc.append("const std::size_t pdf_mapping_count = sizeof(pdf_standard_14_mapping) / sizeof(pdf_standard_14_mapping[0]);\n\n")

    cc.append("bool decompress_font(const FontEntry* entry, std::vector<std::uint8_t>& output) {\n"
              "  if (!entry || !entry->compressed_data) return false;\n"
              "  output.resize(entry->original_size);\n"
              "#ifdef NANOPDF_USE_MINIZ\n"
              "  mz_ulong dest_len = entry->original_size;\n"
              "  int r = mz_uncompress(output.data(), &dest_len, entry->compressed_data, entry->compressed_size);\n"
              "  if (r != MZ_OK) { output.clear(); return false; }\n"
              "#else\n"
              "  uLongf dest_len = entry->original_size;\n"
              "  int r = uncompress(output.data(), &dest_len, entry->compressed_data, entry->compressed_size);\n"
              "  if (r != Z_OK) { output.clear(); return false; }\n"
              "#endif\n"
              "  return true;\n"
              "}\n\n")

    cc.append("bool decompress_font_to_buffer(const FontEntry* entry, unsigned char* buffer, std::size_t buffer_size) {\n"
              "  if (!entry || !entry->compressed_data || !buffer) return false;\n"
              "  if (buffer_size < entry->original_size) return false;\n"
              "#ifdef NANOPDF_USE_MINIZ\n"
              "  mz_ulong dest_len = entry->original_size;\n"
              "  int r = mz_uncompress(buffer, &dest_len, entry->compressed_data, entry->compressed_size);\n"
              "  return r == MZ_OK;\n"
              "#else\n"
              "  uLongf dest_len = entry->original_size;\n"
              "  int r = uncompress(buffer, &dest_len, entry->compressed_data, entry->compressed_size);\n"
              "  return r == Z_OK;\n"
              "#endif\n"
              "}\n\n")

    cc.append("const FontEntry* find_font(const char* name) {\n"
              "  if (!name) return nullptr;\n"
              "  for (std::size_t i = 0; i < font_count; ++i) {\n"
              "    const char* a = name; const char* b = font_registry[i].base_name;\n"
              "    while (*a && *b) {\n"
              "      char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;\n"
              "      char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;\n"
              "      if (ca != cb) break;\n"
              "      ++a; ++b;\n"
              "    }\n"
              "    if (*a == *b) return &font_registry[i];\n"
              "  }\n"
              "  return nullptr;\n"
              "}\n\n")

    if not opts['skip_pdf_mapping']:
        cc.append("const FontEntry* get_pdf_standard_font(const char* pdf_name) {\n"
                  "  if (!pdf_name) return nullptr;\n"
                  "  for (std::size_t i = 0; i < pdf_mapping_count; ++i) {\n"
                  "    if (std::strcmp(pdf_name, pdf_standard_14_mapping[i].pdf_name) == 0) {\n"
                  "      return find_font(pdf_standard_14_mapping[i].substitute_name);\n"
                  "    }\n"
                  "  }\n"
                  "  return nullptr;\n"
                  "}\n\n")

    cc.append(f"}}  // namespace {namespace}\n")
    cc.append("}  // namespace nanopdf\n")
    write_if_changed(cc_path, ''.join(cc).encode())

    ratio = (1 - total_comp / total_orig) * 100
    print(f"Total: {total_orig:,} -> {total_comp:,} ({ratio:.1f}% reduction), {len(entries)} fonts")
    print(f"Generated: {hh_path}")
    print(f"Generated: {s_path}")
    print(f"Generated: {cc_path}")


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    fonts_dir = sys.argv[1]
    out_dir = sys.argv[2]
    base = sys.argv[3]
    opts = parse_options(sys.argv[4:])
    if not os.path.isdir(fonts_dir):
        print(f"Error: directory not found: {fonts_dir}", file=sys.stderr)
        sys.exit(1)
    os.makedirs(out_dir, exist_ok=True)
    generate(fonts_dir, out_dir, base, opts)
