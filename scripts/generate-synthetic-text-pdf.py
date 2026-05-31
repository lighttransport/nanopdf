#!/usr/bin/env python3
"""Generate deterministic text extraction/search PDF fixtures.

The generated PDF intentionally avoids external font tooling. It contains:
  - horizontal WinAnsi text in Helvetica
  - vertical Japanese text using a Type0 Identity-V font and ToUnicode CMap
"""

from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parents[1]
OUTDIR = PROJECT_DIR / "tests" / "fixtures" / "visual"


def stream_object(dictionary: str, data: bytes) -> bytes:
    return (
        f"<< {dictionary} /Length {len(data)} >>\nstream\n".encode("ascii")
        + data
        + b"\nendstream"
    )


def build_pdf() -> bytes:
    horizontal_text = (
        b"BT\n"
        b"/F1 18 Tf\n"
        b"72 720 Td\n"
        b"(SEARCHABLE HORIZONTAL TEXT) Tj\n"
        b"0 -32 Td\n"
        b"(Select this horizontal line.) Tj\n"
        b"ET\n"
    )

    # CIDs 1..6 map to "縦書き日本語".
    vertical_text = (
        b"BT\n"
        b"/F2 24 Tf\n"
        b"430 690 Td\n"
        b"<000100020003000400050006> Tj\n"
        b"ET\n"
    )

    contents = horizontal_text + vertical_text
    tounicode = b"""/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /NanoPDFSyntheticTategaki def
/CMapType 2 def
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
6 beginbfchar
<0001> <7E26>
<0002> <66F8>
<0003> <304D>
<0004> <65E5>
<0005> <672C>
<0006> <8A9E>
endbfchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
"""

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> "
        b"/Contents 8 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>",
        b"<< /Type /Font /Subtype /Type0 /BaseFont /KozMinPr6N-Regular "
        b"/Encoding /Identity-V /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>",
        b"<< /Type /Font /Subtype /CIDFontType0 /BaseFont /KozMinPr6N-Regular "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) /Supplement 6 >> "
        b"/FontDescriptor 9 0 R /DW 1000 /DW2 [880 -1000] >>",
        stream_object(b"".decode("ascii"), tounicode),
        stream_object(b"".decode("ascii"), contents),
        b"<< /Type /FontDescriptor /FontName /KozMinPr6N-Regular "
        b"/Flags 4 /FontBBox [-120 -250 1000 1000] /ItalicAngle 0 "
        b"/Ascent 880 /Descent -120 /CapHeight 700 /StemV 80 >>",
    ]

    out = bytearray()
    out.extend(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objects, start=1):
        offsets.append(len(out))
        out.extend(f"{i} 0 obj\n".encode("ascii"))
        out.extend(obj)
        out.extend(b"\nendobj\n")

    xref_offset = len(out)
    out.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    out.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        out.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    out.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    return bytes(out)


def main() -> None:
    OUTDIR.mkdir(parents=True, exist_ok=True)
    path = OUTDIR / "synthetic_text_search_selection.pdf"
    path.write_bytes(build_pdf())
    print(f"Created {path}")


if __name__ == "__main__":
    main()
