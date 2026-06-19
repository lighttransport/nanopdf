#!/bin/sh
# scripts/fetch-fonts.sh
#
# Downloads open font equivalents of the PDF Standard 14 fonts,
# Noto Sans CJK for Japanese, Simplified Chinese, Traditional Chinese,
# and Korean, and Noto Emoji (monochrome) for emoji/symbol rendering.
#
# Run from the repository root:
#   sh scripts/fetch-fonts.sh
#
# To skip large Noto CJK downloads, set NOTO_LANGS to a subset:
#   NOTO_LANGS="jp" sh scripts/fetch-fonts.sh
#
# Output directory: fonts/
#   fonts/liberation/   – Liberation Sans / Serif / Mono  (Helvetica/Times/Courier)
#   fonts/freefont/     – GNU FreeFont                    (Symbol / ZapfDingbats)
#   fonts/noto/         – Noto Sans CJK                   (JP / SC / TC / KR)
#   fonts/emoji/        – Noto Emoji monochrome            (outline emoji glyphs)
#
# ─── PDF Standard 14 → open font mapping ────────────────────────────────────
#
#  PDF font               Open alternative              File
#  ─────────────────────  ────────────────────────────  ──────────────────────────────
#  Helvetica              Liberation Sans               LiberationSans-Regular.ttf
#  Helvetica-Bold         Liberation Sans Bold          LiberationSans-Bold.ttf
#  Helvetica-Oblique      Liberation Sans Italic        LiberationSans-Italic.ttf
#  Helvetica-BoldOblique  Liberation Sans Bold Italic   LiberationSans-BoldItalic.ttf
#  Times-Roman            Liberation Serif              LiberationSerif-Regular.ttf
#  Times-Bold             Liberation Serif Bold         LiberationSerif-Bold.ttf
#  Times-Italic           Liberation Serif Italic       LiberationSerif-Italic.ttf
#  Times-BoldItalic       Liberation Serif Bold Italic  LiberationSerif-BoldItalic.ttf
#  Courier                Liberation Mono               LiberationMono-Regular.ttf
#  Courier-Bold           Liberation Mono Bold          LiberationMono-Bold.ttf
#  Courier-Oblique        Liberation Mono Italic        LiberationMono-Italic.ttf
#  Courier-BoldOblique    Liberation Mono Bold Italic   LiberationMono-BoldItalic.ttf
#  Symbol                 GNU FreeSerif                 FreeSerif.ttf  (U+2200–22FF)
#  ZapfDingbats           GNU FreeSans                  FreeSans.ttf   (U+2700–27BF)
#
# ─── Noto Sans CJK weights ───────────────────────────────────────────────────
#
#  Each language zip (~90 MB) contains: Regular, Bold, DemiLight, Light, Medium,
#  Thin, Black.  This script extracts only the weights listed in NOTO_WEIGHTS.
#
# ─── Licenses ────────────────────────────────────────────────────────────────
#  Liberation Fonts  SIL Open Font License 1.1
#  GNU FreeFont      GNU GPL v3 with font exception
#  Noto Fonts        SIL Open Font License 1.1
# ─────────────────────────────────────────────────────────────────────────────

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONTS_DIR="$REPO_ROOT/fonts"

# ── Pinned versions ──────────────────────────────────────────────────────────
LIBERATION_VERSION="2.00.1"
FREEFONT_DATE="20120503"
NOTO_CJK_TAG="Sans2.004"
NOTO_CJK_REPO="notofonts/noto-cjk"

# Languages: jp  sc  tc  kr  hk  (space-separated)
NOTO_LANGS="${NOTO_LANGS:-jp sc tc kr}"

# Weights to extract from each language zip (space-separated).
NOTO_WEIGHTS="${NOTO_WEIGHTS:-Regular Bold}"

# ── Zip index for each language in the Sans2.004 release ────────────────────
# These are the asset filenames in the GitHub release.
# Uses a function instead of associative array for sh/zsh/bash-3 compatibility.
noto_zip_for() {
    case "$1" in
        jp) echo "06_NotoSansCJKjp.zip" ;;
        sc) echo "08_NotoSansCJKsc.zip" ;;
        tc) echo "09_NotoSansCJKtc.zip" ;;
        kr) echo "07_NotoSansCJKkr.zip" ;;
        hk) echo "10_NotoSansCJKhk.zip" ;;
        *)  echo "" ;;
    esac
}

# ── Download helper ──────────────────────────────────────────────────────────

fetch() {
    local url="$1"
    local dest="$2"
    local label="${3:-$(basename "$dest")}"
    if [ -f "$dest" ]; then
        echo "  skip   $label  (already present)"
        return 0
    fi
    echo "  fetch  $label"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --retry-delay 2 -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --tries=3 --waitretry=2 -O "$dest" "$url"
    else
        echo "ERROR: neither curl nor wget found." >&2
        exit 1
    fi
}

need_unzip() {
    command -v unzip >/dev/null 2>&1 || { echo "ERROR: unzip not found." >&2; exit 1; }
}

# ── 0. Source Code Pro ───────────────────────────────────────────────────────
# Source: https://github.com/adobe-fonts/source-code-pro
# Monospaced font designed for coding.  Used by the terminal demo.

SOURCECODEPRO_VERSION="2.042R-u_1.062R-i"
SOURCECODEPRO_TAG="2.042R-u%2F1.062R-i%2F1.026R-vf"

echo ""
echo "┌─ Source Code Pro"
mkdir -p "$FONTS_DIR/sourcecodepro"

SCP_ARCHIVE="$FONTS_DIR/sourcecodepro/_SourceCodePro.zip"
SCP_URL="https://github.com/adobe-fonts/source-code-pro/releases/download/${SOURCECODEPRO_TAG}/OTF-source-code-pro-${SOURCECODEPRO_VERSION}.zip"

if [ ! -f "$FONTS_DIR/sourcecodepro/SourceCodePro-Regular.otf" ] && \
   [ ! -f "$FONTS_DIR/sourcecodepro/SourceCodePro-Regular.ttf" ]; then
    need_unzip
    # Try OTF release first
    fetch "$SCP_URL" "$SCP_ARCHIVE" "SourceCodePro OTF" || true
    if [ -f "$SCP_ARCHIVE" ]; then
        echo "  extract..."
        unzip -q -o "$SCP_ARCHIVE" "*.otf" -d "$FONTS_DIR/sourcecodepro" 2>/dev/null || true
        # Flatten if nested
        find "$FONTS_DIR/sourcecodepro" -mindepth 2 -name "*.otf" \
             -exec mv -n {} "$FONTS_DIR/sourcecodepro/" \; 2>/dev/null || true
        find "$FONTS_DIR/sourcecodepro" -mindepth 1 -type d -empty -delete 2>/dev/null || true
        rm -f "$SCP_ARCHIVE"
        echo "└─ ok"
    else
        # Fallback: try TTF static from GitHub
        SCP_TTF_URL="https://github.com/adobe-fonts/source-code-pro/raw/release/TTF/SourceCodePro-Regular.ttf"
        fetch "$SCP_TTF_URL" "$FONTS_DIR/sourcecodepro/SourceCodePro-Regular.ttf" "SourceCodePro-Regular.ttf" || true
        SCP_TTF_BOLD="https://github.com/adobe-fonts/source-code-pro/raw/release/TTF/SourceCodePro-Bold.ttf"
        fetch "$SCP_TTF_BOLD" "$FONTS_DIR/sourcecodepro/SourceCodePro-Bold.ttf" "SourceCodePro-Bold.ttf" || true
        echo "└─ ok (TTF fallback)"
    fi
else
    echo "└─ already installed"
fi

# ── 1. Liberation Fonts ──────────────────────────────────────────────────────
# Source: https://releases.pagure.org/liberation-fonts/
# Metric-compatible with Helvetica, Times New Roman, Courier New.

echo ""
echo "┌─ Liberation Fonts ${LIBERATION_VERSION}"
mkdir -p "$FONTS_DIR/liberation"

LIB_ARCHIVE="$FONTS_DIR/liberation/_liberation-fonts-ttf-${LIBERATION_VERSION}.tar.gz"
LIB_URL="https://releases.pagure.org/liberation-fonts/liberation-fonts-ttf-${LIBERATION_VERSION}.tar.gz"

if [ ! -f "$FONTS_DIR/liberation/LiberationSans-Regular.ttf" ]; then
    fetch "$LIB_URL" "$LIB_ARCHIVE" "liberation-fonts-ttf-${LIBERATION_VERSION}.tar.gz"
    echo "  extract..."
    # Extract .ttf files: try GNU tar (--wildcards) first, fall back to BSD tar (macOS)
    if tar --wildcards -tf /dev/null 2>/dev/null || tar --wildcards --version >/dev/null 2>&1; then
        tar -xzf "$LIB_ARCHIVE" --strip-components=1 -C "$FONTS_DIR/liberation" \
            --wildcards '*.ttf'
    else
        # BSD tar (macOS): extract everything, then remove non-ttf files
        tar -xzf "$LIB_ARCHIVE" --strip-components=1 -C "$FONTS_DIR/liberation"
        find "$FONTS_DIR/liberation" -maxdepth 1 -type f ! -name '*.ttf' -delete 2>/dev/null || true
    fi
    rm -f "$LIB_ARCHIVE"
    echo "└─ ok"
else
    echo "└─ already installed"
fi

# ── 2. GNU FreeFont ──────────────────────────────────────────────────────────
# Source: https://www.gnu.org/software/freefont/
# FreeSerif covers the Symbol block (U+2200–22FF, mathematical operators).
# FreeSans  covers the Dingbats block (U+2700–27BF).

echo ""
echo "┌─ GNU FreeFont ${FREEFONT_DATE}"
mkdir -p "$FONTS_DIR/freefont"

FF_ARCHIVE="$FONTS_DIR/freefont/_freefont-ttf-${FREEFONT_DATE}.zip"
FF_URL="https://ftpmirror.gnu.org/gnu/freefont/freefont-ttf-${FREEFONT_DATE}.zip"

if [ ! -f "$FONTS_DIR/freefont/FreeSerif.ttf" ]; then
    need_unzip
    fetch "$FF_URL" "$FF_ARCHIVE" "freefont-ttf-${FREEFONT_DATE}.zip"
    echo "  extract..."
    unzip -q -o "$FF_ARCHIVE" "*.ttf" -d "$FONTS_DIR/freefont"
    # Flatten nested directories (unzip may create a subdirectory)
    find "$FONTS_DIR/freefont" -mindepth 2 -name "*.ttf" \
         -exec mv -n {} "$FONTS_DIR/freefont/" \; 2>/dev/null || true
    find "$FONTS_DIR/freefont" -mindepth 1 -type d -empty -delete 2>/dev/null || true
    rm -f "$FF_ARCHIVE"
    echo "└─ ok"
else
    echo "└─ already installed"
fi

# ── 3. Noto Sans CJK ────────────────────────────────────────────────────────
# Source: https://github.com/notofonts/noto-cjk  tag: Sans2.004
#
# Each per-language zip (~90 MB) contains all weights for one language.
# We download the zip, extract only the requested weights, then delete the zip.
#
#  Language  Zip asset                  OTF name pattern
#  ────────  ─────────────────────────  ──────────────────────────────────────
#  jp        06_NotoSansCJKjp.zip       NotoSansCJKjp-{Weight}.otf
#  kr        07_NotoSansCJKkr.zip       NotoSansCJKkr-{Weight}.otf
#  sc        08_NotoSansCJKsc.zip       NotoSansCJKsc-{Weight}.otf
#  tc        09_NotoSansCJKtc.zip       NotoSansCJKtc-{Weight}.otf
#  hk        10_NotoSansCJKhk.zip       NotoSansCJKhk-{Weight}.otf

echo ""
echo "┌─ Noto Sans CJK ${NOTO_CJK_TAG}  (langs: ${NOTO_LANGS}  weights: ${NOTO_WEIGHTS})"
mkdir -p "$FONTS_DIR/noto"

NOTO_BASE="https://github.com/${NOTO_CJK_REPO}/releases/download/${NOTO_CJK_TAG}"

for lang in $NOTO_LANGS; do
    zip_asset="$(noto_zip_for "$lang")"
    if [ -z "$zip_asset" ]; then
        echo "│  warn   unknown language '$lang' — skipping"
        continue
    fi

    # Check if all requested weights are already present
    all_present=1
    for weight in $NOTO_WEIGHTS; do
        otf="$FONTS_DIR/noto/NotoSansCJK${lang}-${weight}.otf"
        [ -f "$otf" ] || { all_present=0; break; }
    done
    if [ "$all_present" = "1" ]; then
        echo "│  skip   $lang — already installed"
        continue
    fi

    zip_dest="$FONTS_DIR/noto/_${zip_asset}"
    fetch "${NOTO_BASE}/${zip_asset}" "$zip_dest" "$zip_asset  (~90 MB)"

    need_unzip
    echo "│  extract $lang weights: $NOTO_WEIGHTS"
    for weight in $NOTO_WEIGHTS; do
        otf_name="NotoSansCJK${lang}-${weight}.otf"
        otf_dest="$FONTS_DIR/noto/$otf_name"
        if [ -f "$otf_dest" ]; then continue; fi
        # Try extracting the specific file; fall back to full extract
        unzip -q -o "$zip_dest" "$otf_name" -d "$FONTS_DIR/noto" 2>/dev/null || true
        if [ ! -f "$otf_dest" ]; then
            # Some zips nest files in a subdirectory
            unzip -q -o "$zip_dest" "*/$otf_name" -d "$FONTS_DIR/noto" 2>/dev/null || true
            find "$FONTS_DIR/noto" -name "$otf_name" ! -path "$otf_dest" \
                 -exec mv {} "$otf_dest" \; 2>/dev/null || true
        fi
        [ -f "$otf_dest" ] && echo "│    ok   $otf_name" \
                           || echo "│    warn $otf_name not found in zip"
    done
    rm -f "$zip_dest"
done
echo "└─ done"

# ── 4. Noto Emoji (monochrome) ──────────────────────────────────────────────
# Source: https://github.com/google/fonts/tree/main/ofl/notoemoji
# Monochrome outline emoji glyphs (standard glyf TrueType outlines).
# Works with any TrueType renderer — no color table support required.
# License: SIL Open Font License 1.1

echo ""
echo "┌─ Noto Emoji (monochrome)"
mkdir -p "$FONTS_DIR/emoji"

NOTO_EMOJI_URL="https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
NOTO_EMOJI_LICENSE="https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/OFL.txt"

if [ ! -f "$FONTS_DIR/emoji/NotoEmoji.ttf" ]; then
    fetch "$NOTO_EMOJI_URL" "$FONTS_DIR/emoji/NotoEmoji.ttf" "NotoEmoji.ttf"
    fetch "$NOTO_EMOJI_LICENSE" "$FONTS_DIR/emoji/OFL.txt" "OFL.txt (license)"
    echo "└─ ok"
else
    echo "└─ already installed"
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "════════════════════════════════════════"
echo " Installed fonts"
echo "════════════════════════════════════════"
for subdir in liberation freefont noto emoji; do
    dir="$FONTS_DIR/$subdir"
    [ -d "$dir" ] || continue
    count=$(find "$dir" -maxdepth 1 \( -name "*.ttf" -o -name "*.otf" \) | wc -l | tr -d ' ')
    echo ""
    echo "  fonts/$subdir/  ($count files)"
    find "$dir" -maxdepth 1 \( -name "*.ttf" -o -name "*.otf" \) \
         -exec basename {} \; | sed 's/^/    /'
done | sort

echo ""
echo "════════════════════════════════════════"
echo " PDF Standard 14 mapping"
echo "════════════════════════════════════════"
echo "  Helvetica*     → fonts/liberation/LiberationSans-{Regular,Bold,Italic,BoldItalic}.ttf"
echo "  Times*         → fonts/liberation/LiberationSerif-{Regular,Bold,Italic,BoldItalic}.ttf"
echo "  Courier*       → fonts/liberation/LiberationMono-{Regular,Bold,Italic,BoldItalic}.ttf"
echo "  Symbol         → fonts/freefont/FreeSerif.ttf   (U+2200–22FF Mathematical Operators)"
echo "  ZapfDingbats   → fonts/freefont/FreeSans.ttf    (U+2700–27BF Dingbats)"
echo ""
echo "════════════════════════════════════════"
echo " CJK"
echo "════════════════════════════════════════"
echo "  Japanese (jp)           → fonts/noto/NotoSansCJKjp-Regular.otf"
echo "  Simplified Chinese (sc) → fonts/noto/NotoSansCJKsc-Regular.otf"
echo "  Traditional Chinese (tc)→ fonts/noto/NotoSansCJKtc-Regular.otf"
echo "  Korean (kr)             → fonts/noto/NotoSansCJKkr-Regular.otf"
echo ""
echo "════════════════════════════════════════"
echo " Emoji"
echo "════════════════════════════════════════"
echo "  Noto Emoji (mono) → fonts/emoji/NotoEmoji.ttf  (SIL OFL 1.1)"
echo ""
echo "To test font rendering:"
echo "  ./build/test_render tests/golden tests/output fonts/liberation/LiberationSans-Regular.ttf"
