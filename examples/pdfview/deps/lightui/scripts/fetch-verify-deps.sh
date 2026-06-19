#!/usr/bin/env bash
# scripts/fetch-verify-deps.sh
#
# Clone (or update) blend2d and thorvg into ref/ for canvas verification.
# Run from the repository root:
#
#   bash scripts/fetch-verify-deps.sh
#
# To force a clean re-clone, delete the directories first:
#
#   rm -rf ref/blend2d ref/asmjit ref/thorvg && bash scripts/fetch-verify-deps.sh
#
# These libraries are used as reference renderers to verify lightui's
# shape & stroke primitives produce correct output.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF_DIR="$REPO_ROOT/ref"

# ---------------------------------------------------------------------------
# Pinned revisions (update these when you want to pull in newer versions)
# ---------------------------------------------------------------------------

# blend2d requires asmjit as a sibling directory for JIT compilation
ASMJIT_URL="https://github.com/asmjit/asmjit.git"
ASMJIT_TAG="master"

BLEND2D_URL="https://github.com/blend2d/blend2d.git"
BLEND2D_TAG="master"

THORVG_URL="https://github.com/thorvg/thorvg.git"
THORVG_TAG="v1.0"

# ---------------------------------------------------------------------------

clone_or_update() {
    local name="$1"
    local url="$2"
    local tag="$3"
    local dest="$REF_DIR/$name"

    if [ -d "$dest/.git" ]; then
        echo "[fetch-verify-deps] $name: already cloned — fetching $tag"
        git -C "$dest" fetch --depth=1 origin "$tag" 2>/dev/null || \
        git -C "$dest" fetch --depth=1 origin "refs/tags/$tag:refs/tags/$tag" 2>/dev/null || true
        git -C "$dest" checkout "$tag" --quiet 2>/dev/null || \
        git -C "$dest" checkout "FETCH_HEAD" --quiet 2>/dev/null || true
    else
        echo "[fetch-verify-deps] $name: cloning $tag from $url"
        git clone --depth=1 --branch "$tag" "$url" "$dest" 2>/dev/null || \
        git clone --depth=1 "$url" "$dest"
    fi
    echo "[fetch-verify-deps] $name: $(git -C "$dest" describe --tags --always 2>/dev/null || git -C "$dest" rev-parse --short HEAD)"
}

mkdir -p "$REF_DIR"

# blend2d needs asmjit as a sibling or inside its 3rdparty directory
clone_or_update asmjit  "$ASMJIT_URL"  "$ASMJIT_TAG"
clone_or_update blend2d "$BLEND2D_URL" "$BLEND2D_TAG"

# Link asmjit into blend2d's 3rdparty directory if not already there
BLEND2D_3RD="$REF_DIR/blend2d/3rdparty"
if [ ! -d "$BLEND2D_3RD/asmjit" ] && [ ! -L "$BLEND2D_3RD/asmjit" ]; then
    mkdir -p "$BLEND2D_3RD"
    ln -sf "$REF_DIR/asmjit" "$BLEND2D_3RD/asmjit"
    echo "[fetch-verify-deps] blend2d: linked asmjit into 3rdparty/"
fi

clone_or_update thorvg  "$THORVG_URL"  "$THORVG_TAG"

echo ""
echo "[fetch-verify-deps] Done."
echo ""
echo "  ref/blend2d/   blend2d 2D rendering library"
echo "  ref/asmjit/    asmjit JIT assembler (blend2d dependency)"
echo "  ref/thorvg/    thorvg vector graphics library"
echo ""
echo "Build verification tests with:"
echo "  cmake -B build -DLUI_BUILD_VERIFY=ON"
echo "  cmake --build build --target verify_blend2d verify_thorvg"
