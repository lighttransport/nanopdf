#!/bin/bash
#
# fetch-external-tests.sh
#
# Downloads curated PDF test files from external sources for visual regression testing.
# This script is idempotent and will skip files that already exist.
#
# Sources:
#   - Poppler test suite (gitlab.freedesktop.org/poppler/test)
#   - pdf.js test fixtures (mozilla/pdf.js)
#   - PDF 2.0 examples (pdf-association/pdf20examples)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
EXTERNAL_DIR="$PROJECT_ROOT/data/external"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Create external directory
mkdir -p "$EXTERNAL_DIR"
log_info "Using external directory: $EXTERNAL_DIR"

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        log_error "$1 is not installed. Please install it to continue."
        return 1
    fi
    return 0
}

# Download a single file from a URL
download_file() {
    local url="$1"
    local output="$2"
    local description="$3"

    if [[ -f "$output" ]]; then
        log_skip "$description (already exists)"
        return 0
    fi

    log_info "Downloading $description..."
    if wget -q --show-progress -O "$output" "$url" 2>&1; then
        log_success "Downloaded $description"
        return 0
    else
        log_error "Failed to download $description from $url"
        rm -f "$output"
        return 1
    fi
}

# Initialize git sparse-checkout and fetch specific files
sparse_checkout_files() {
    local repo_url="$1"
    local branch="$2"
    local repo_name="$3"
    shift 3
    local files=("$@")

    local temp_dir="/tmp/pdf_test_checkout_$$"
    local repo_dir="$temp_dir/$repo_name"

    log_info "Setting up sparse checkout for $repo_name..."

    mkdir -p "$temp_dir"
    cd "$temp_dir"

    # Initialize repo with sparse-checkout
    git clone --depth 1 --filter=blob:none --sparse "$repo_url" "$repo_name" 2>&1 | grep -v "^hint:" || true

    cd "$repo_dir"
    git sparse-checkout set "${files[@]}" 2>&1 | grep -v "^hint:" || true

    # Copy files to external directory
    for file in "${files[@]}"; do
        if [[ -f "$file" ]]; then
            local dest="$EXTERNAL_DIR/$(basename "$file")"
            if [[ ! -f "$dest" ]]; then
                log_info "Copying $(basename "$file")..."
                cp "$file" "$dest"
                log_success "Copied $(basename "$file")"
            else
                log_skip "$(basename "$file") (already exists)"
            fi
        else
            log_error "File not found in repo: $file"
        fi
    done

    cd - > /dev/null
    rm -rf "$temp_dir"
}

# ============================================================================
# Download Poppler test files (graphics-heavy PDFs)
# ============================================================================

download_poppler_tests() {
    log_info ""
    log_info "=========================================="
    log_info "Downloading Poppler test files..."
    log_info "=========================================="

    # These are specific test files from the Poppler test suite
    # that are good for visual regression testing
    local poppler_files=(
        "pdfs/cs3.pdf"
        "pdfs/cs4.pdf"
        "pdfs/image-base64.pdf"
        "pdfs/image-rgb.pdf"
        "pdfs/moire.pdf"
        "pdfs/pattern.pdf"
        "pdfs/radial-shading.pdf"
        "pdfs/type3.pdf"
        "pdfs/type3-font.pdf"
    )

    sparse_checkout_files \
        "https://gitlab.freedesktop.org/poppler/test.git" \
        "master" \
        "poppler-test" \
        "${poppler_files[@]}" || log_error "Failed to fetch some Poppler test files"
}

# ============================================================================
# Download pdf.js test fixtures
# ============================================================================

download_pdfjs_tests() {
    log_info ""
    log_info "=========================================="
    log_info "Downloading pdf.js test PDFs..."
    log_info "=========================================="

    # Direct downloads of specific test PDFs from pdf.js
    # Using raw.githubusercontent.com for direct file access
    local pdfjs_base="https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs"

    local pdfjs_files=(
        "annotation-popup.pdf"
        "color-rgb.pdf"
        "graphics.pdf"
        "font-with-space.pdf"
        "linearization.pdf"
        "pattern.pdf"
    )

    for file in "${pdfjs_files[@]}"; do
        download_file \
            "$pdfjs_base/$file" \
            "$EXTERNAL_DIR/$file" \
            "pdf.js test file: $file" || true
    done
}

# ============================================================================
# Download PDF 2.0 examples
# ============================================================================

download_pdf20_examples() {
    log_info ""
    log_info "=========================================="
    log_info "Downloading PDF 2.0 examples..."
    log_info "=========================================="

    # PDF 2.0 examples from the PDF Association
    local pdf20_files=(
        "PDF20/Examples/006_08-ObjStm-ObjStmDict.pdf"
        "PDF20/Examples/013-LaunchAction.pdf"
        "PDF20/Examples/055-FileSpec-EFStream.pdf"
    )

    sparse_checkout_files \
        "https://github.com/pdf-association/pdf20examples.git" \
        "master" \
        "pdf20examples" \
        "${pdf20_files[@]}" || log_error "Failed to fetch some PDF 2.0 examples"
}

# ============================================================================
# Main execution
# ============================================================================

main() {
    log_info "PDF Test File Downloader"
    log_info "========================="
    log_info ""

    # Check for required tools
    if ! check_tool wget; then
        log_error "wget is required. Install with: apt-get install wget (Ubuntu/Debian) or brew install wget (macOS)"
        exit 1
    fi

    if ! check_tool git; then
        log_error "git is required. Install with: apt-get install git (Ubuntu/Debian) or brew install git (macOS)"
        exit 1
    fi

    # Download from different sources
    download_poppler_tests || log_error "Poppler download had issues"
    download_pdfjs_tests || log_error "pdf.js download had issues"
    download_pdf20_examples || log_error "PDF 2.0 download had issues"

    log_info ""
    log_success "Test file download complete!"
    log_info "Files downloaded to: $EXTERNAL_DIR"
    log_info "Total files: $(find "$EXTERNAL_DIR" -type f -name '*.pdf' 2>/dev/null | wc -l)"
    log_info ""
}

main "$@"
