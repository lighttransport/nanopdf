#!/usr/bin/env python3
# SPDX-License-Identifier: Apache 2.0
# Copyright 2024 - Present, Light Transport Entertainment Inc.
#
# Prepare external fonts for WASM builds:
# - Scans fonts/ directory for .ttf/.otf files
# - Generates fonts-manifest.json with metadata
# - Copies font files to a target directory

import json
import os
import shutil
import sys

# Font category mapping
# 0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif
FONT_CATEGORIES = {
    "arimo": 0,       # Sans (Helvetica alternative)
    "cousine": 1,     # Mono (Courier alternative)
    "tinos": 2,       # Serif (Times alternative)
    "stix": 3,        # Symbol
    "noto-symbols": 3, # Symbol (ZapfDingbats alternative)
    "noto-sans-jp": 4, # CJK Sans
    "noto-serif-jp": 5, # CJK Serif
}

# PDF Standard 14 font mapping
PDF14_MAPPING = {
    "Helvetica": "arimo/Arimo-Regular.ttf",
    "Helvetica-Bold": "arimo/Arimo-Bold.ttf",
    "Helvetica-Oblique": "arimo/Arimo-Italic.ttf",
    "Helvetica-BoldOblique": "arimo/Arimo-BoldItalic.ttf",
    "Times-Roman": "tinos/Tinos-Regular.ttf",
    "Times-Bold": "tinos/Tinos-Bold.ttf",
    "Times-Italic": "tinos/Tinos-Italic.ttf",
    "Times-BoldItalic": "tinos/Tinos-BoldItalic.ttf",
    "Courier": "cousine/Cousine-Regular.ttf",
    "Courier-Bold": "cousine/Cousine-Bold.ttf",
    "Courier-Oblique": "cousine/Cousine-Italic.ttf",
    "Courier-BoldOblique": "cousine/Cousine-BoldItalic.ttf",
    "Symbol": "stix/STIXTwoMath-Regular.otf",
    "ZapfDingbats": "noto-symbols/NotoSansSymbols-Regular.ttf",
}

# Only include Regular weight for CJK to keep sizes reasonable
CJK_REGULAR_ONLY = True


def get_category(rel_path):
    """Determine font category from its relative path."""
    parts = rel_path.replace("\\", "/").split("/")
    if len(parts) >= 1:
        folder = parts[0]
        if folder in FONT_CATEGORIES:
            return FONT_CATEGORIES[folder]
    return 0  # Default to sans


def should_include(rel_path):
    """Filter which fonts to include."""
    basename = os.path.basename(rel_path)
    parts = rel_path.replace("\\", "/").split("/")
    if len(parts) >= 1:
        folder = parts[0]
        if folder in ("noto-sans-jp", "noto-serif-jp"):
            if CJK_REGULAR_ONLY:
                # Only include Regular weight for CJK
                if "Regular" not in basename:
                    return False
            # Prefer smaller SubsetOTF (NotoSansJP/NotoSerifJP) over full
            # collection (NotoSansCJKjp/NotoSerifCJKjp)
            if "CJK" in basename:
                return False
    return True


def main():
    # Determine paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    fonts_dir = os.path.join(project_root, "fonts")

    # Default target: examples/wasm/public/fonts/
    if len(sys.argv) > 1:
        target_dir = sys.argv[1]
    else:
        target_dir = os.path.join(project_root, "examples", "wasm", "public", "fonts")

    if not os.path.isdir(fonts_dir):
        print(f"Error: fonts directory not found: {fonts_dir}", file=sys.stderr)
        sys.exit(1)

    # Collect font files
    fonts = []
    for root, dirs, files in os.walk(fonts_dir):
        for filename in sorted(files):
            if not (filename.endswith(".ttf") or filename.endswith(".otf")):
                continue
            abs_path = os.path.join(root, filename)
            rel_path = os.path.relpath(abs_path, fonts_dir)
            rel_path = rel_path.replace("\\", "/")

            if not should_include(rel_path):
                continue

            size = os.path.getsize(abs_path)
            name = os.path.splitext(filename)[0]
            category = get_category(rel_path)

            fonts.append({
                "name": name,
                "category": category,
                "file": rel_path,
                "size": size,
            })

    if not fonts:
        print("Warning: no font files found", file=sys.stderr)

    # Build manifest
    manifest = {
        "fonts": fonts,
        "pdf14_mapping": PDF14_MAPPING,
    }

    # Create target directory
    os.makedirs(target_dir, exist_ok=True)

    # Copy font files
    copied = 0
    total_size = 0
    for font in fonts:
        src = os.path.join(fonts_dir, font["file"])
        dst = os.path.join(target_dir, font["file"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1
        total_size += font["size"]

    # Write manifest
    manifest_path = os.path.join(target_dir, "fonts-manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    # Summary
    standard = [f for f in fonts if f["category"] <= 3]
    cjk = [f for f in fonts if f["category"] >= 4]
    std_size = sum(f["size"] for f in standard)
    cjk_size = sum(f["size"] for f in cjk)

    print(f"Prepared {copied} font files -> {target_dir}")
    print(f"  Standard fonts: {len(standard)} files ({std_size / 1024 / 1024:.1f} MB)")
    print(f"  CJK fonts:      {len(cjk)} files ({cjk_size / 1024 / 1024:.1f} MB)")
    print(f"  Total:          {total_size / 1024 / 1024:.1f} MB")
    print(f"  Manifest:       {manifest_path}")


if __name__ == "__main__":
    main()
