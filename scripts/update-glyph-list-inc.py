#!/usr/bin/env python3
"""Regenerate src/adobe_glyph_list.inc from data/adobe_glyph_list.txt."""

import argparse
from pathlib import Path

ENCODING = "ascii"


def load_glyph_list(path: Path):
    entries = []
    with path.open("r", encoding=ENCODING) as infile:
        for line in infile:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, values = line.split(";", 1)
            codepoints = [int(item, 16) for item in values.strip().split()]
            entries.append((name, codepoints))
    return entries


def codepoints_to_escaped_utf8(codepoints):
    utf8_bytes = b"".join(chr(cp).encode("utf-8") for cp in codepoints)
    return "".join(f"\\x{byte:02X}" for byte in utf8_bytes)


def write_header(path: Path, entries):
    with path.open("w", encoding=ENCODING, newline="\n") as outfile:
        outfile.write("// Auto-generated from data/adobe_glyph_list.txt; do not edit manually.\n")
        outfile.write("struct GlyphListEntry {\n")
        outfile.write("  const char* name;\n")
        outfile.write("  const char* utf8;\n")
        outfile.write("};\n\n")
        outfile.write("static const GlyphListEntry kAdobeGlyphList[] = {\n")
        for name, cps in entries:
            escaped = codepoints_to_escaped_utf8(cps)
            outfile.write(f'    {{"{name}", "{escaped}"}},\n')
        outfile.write("};\n")
        outfile.write(f"static const size_t kAdobeGlyphListCount = {len(entries)};\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/adobe_glyph_list.txt"))
    parser.add_argument("--output", type=Path, default=Path("src/adobe_glyph_list.inc"))
    args = parser.parse_args()

    entries = load_glyph_list(args.input)
    write_header(args.output, entries)


if __name__ == "__main__":
    main()
