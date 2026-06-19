#!/bin/sh
# scripts/fetch-deps.sh
#
# Clone (or update) FreeType and HarfBuzz into ref/ at pinned commits.
# Run from the repository root:
#
#   sh scripts/fetch-deps.sh
#
# To force a clean re-clone, delete the ref/ directory first:
#
#   rm -rf ref/ && sh scripts/fetch-deps.sh

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF_DIR="$REPO_ROOT/ref"

# ---------------------------------------------------------------------------
# Pinned revisions (update these when you want to pull in newer versions)
# ---------------------------------------------------------------------------
FREETYPE_URL="https://github.com/freetype/freetype.git"
FREETYPE_TAG="VER-2-13-3"

HARFBUZZ_URL="https://github.com/harfbuzz/harfbuzz.git"
HARFBUZZ_TAG="10.4.0"

# ---------------------------------------------------------------------------

clone_or_update() {
    local name="$1"
    local url="$2"
    local tag="$3"
    local dest="$REF_DIR/$name"

    if [ -d "$dest/.git" ]; then
        echo "[fetch-deps] $name: already cloned — fetching tag $tag"
        git -C "$dest" fetch --depth=1 origin "refs/tags/$tag:refs/tags/$tag" 2>/dev/null || true
        git -C "$dest" checkout "$tag" --quiet
    else
        echo "[fetch-deps] $name: cloning tag $tag from $url"
        git clone --depth=1 --branch "$tag" "$url" "$dest"
    fi
    echo "[fetch-deps] $name: $(git -C "$dest" describe --tags --always)"
}

mkdir -p "$REF_DIR"

clone_or_update freetype  "$FREETYPE_URL" "$FREETYPE_TAG"
clone_or_update harfbuzz  "$HARFBUZZ_URL" "$HARFBUZZ_TAG"

echo ""
echo "[fetch-deps] Done.  Build with:"
echo "  cmake -B build -DLUI_BUILD_FONTS=ON"
echo "  cmake --build build"
