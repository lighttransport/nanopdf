#!/usr/bin/env bash
# download-corpora.sh — Download PDF test corpora for validation testing
#
# Usage:
#   ./scripts/download-corpora.sh <subcommand> [options]
#
# Subcommands:
#   arlington   Download Arlington PDF model TSV files
#   safedocs    Download SafeDocs hand-coded edge-case PDFs
#   pdf-diff    Download pdf-differences interop test PDFs
#   cc-main     Download CC-MAIN PDF subset (use --count N)
#   govdocs     Download GovDocs1 subset (use --count N)
#   verapdf     Download veraPDF PDF/A & PDF/UA corpus
#   pdfjs       Download Mozilla pdf.js test PDFs
#   pdfium      Download Chromium pdfium test PDFs
#   tika        Download Apache Tika issue tracker PDFs
#   unsafe      Download DARPA SafeDocs malformed PDFs
#   all         Download all corpora
#   status      Report what's downloaded
#
# Options:
#   --count N   Number of files/ZIPs for large corpora (default: 1)
#   --force     Re-download even if already present
#   --dry-run   Preview actions without downloading

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CORPORA_DIR="${PROJECT_DIR}/tests/data/corpora"

# Options
FORCE=false
DRY_RUN=false
COUNT=1

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# Utility Functions
# ============================================================================

log_info()  { echo -e "${BLUE}[corpora]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[corpora]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[corpora]${NC} $*"; }
log_error() { echo -e "${RED}[corpora]${NC} $*" >&2; }

check_deps() {
    local missing=()
    for cmd in "$@"; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required tools: ${missing[*]}"
        log_error "Install them and try again."
        exit 1
    fi
}

ensure_dir() {
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would create: $1"
    else
        mkdir -p "$1"
    fi
}

is_downloaded() {
    local dir="$1"
    [[ -d "$dir" ]] && [[ -n "$(ls -A "$dir" 2>/dev/null)" ]]
}

skip_if_exists() {
    local dir="$1"
    local name="$2"
    if [[ "$FORCE" != "true" ]] && is_downloaded "$dir"; then
        log_ok "$name already downloaded. Use --force to re-download."
        return 0
    fi
    return 1
}

cleanup_temp() {
    if [[ -n "${TEMP_DIR:-}" ]] && [[ -d "$TEMP_DIR" ]]; then
        rm -rf "$TEMP_DIR"
    fi
}
trap cleanup_temp EXIT

# ============================================================================
# Download Functions
# ============================================================================

download_arlington() {
    local dest="${CORPORA_DIR}/arlington"
    if skip_if_exists "${dest}/tsv/latest" "Arlington"; then return 0; fi
    check_deps git

    log_info "Downloading Arlington PDF model..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone pdf-association/arlington-pdf-model"
        return 0
    fi

    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 https://github.com/pdf-association/arlington-pdf-model.git \
        "${TEMP_DIR}/arlington-pdf-model"
    ensure_dir "$dest"
    cp -r "${TEMP_DIR}/arlington-pdf-model/tsv" "$dest/"
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "Arlington downloaded to ${dest}"
}

download_safedocs() {
    local dest="${CORPORA_DIR}/safedocs"
    if skip_if_exists "$dest" "SafeDocs"; then return 0; fi
    check_deps git

    log_info "Downloading SafeDocs corpus..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone pdf-association/safedocs"
        return 0
    fi

    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 https://github.com/pdf-association/safedocs.git \
        "${TEMP_DIR}/safedocs"
    ensure_dir "$dest"
    # Copy the PDF files, preserving directory structure
    if [[ -d "${TEMP_DIR}/safedocs/pdfs" ]]; then
        cp -r "${TEMP_DIR}/safedocs/pdfs/"* "$dest/"
    else
        cp -r "${TEMP_DIR}/safedocs/"* "$dest/"
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "SafeDocs downloaded to ${dest}"
}

download_pdf_diff() {
    local dest="${CORPORA_DIR}/pdf-differences"
    if skip_if_exists "$dest" "pdf-differences"; then return 0; fi
    check_deps git

    log_info "Downloading pdf-differences corpus..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone pdf-association/pdf-differences"
        return 0
    fi

    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 https://github.com/pdf-association/pdf-differences.git \
        "${TEMP_DIR}/pdf-differences"
    ensure_dir "$dest"
    cp -r "${TEMP_DIR}/pdf-differences/"* "$dest/"
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "pdf-differences downloaded to ${dest}"
}

download_cc_main() {
    local dest="${CORPORA_DIR}/cc-main-2021"
    if skip_if_exists "$dest" "CC-MAIN"; then return 0; fi
    check_deps wget unzip

    log_info "Downloading CC-MAIN-2021 PDF subset (${COUNT} ZIP(s))..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would download ${COUNT} ZIP(s) from digitalcorpora.org"
        return 0
    fi

    ensure_dir "$dest"
    local base_url="https://downloads.digitalcorpora.org/corpora/files/CC-MAIN-2021-31-PDF-UNTRUNCATED"

    # Get listing and download first N ZIPs
    TEMP_DIR="$(mktemp -d)"
    wget -q -O "${TEMP_DIR}/listing.html" "${base_url}/" 2>/dev/null || true
    # Extract ZIP filenames from the listing
    local zips
    zips=$(grep -oP 'href="\K[^"]+\.zip' "${TEMP_DIR}/listing.html" 2>/dev/null | head -n "$COUNT" || true)

    if [[ -z "$zips" ]]; then
        # Fallback: try direct numbered ZIPs
        for i in $(seq 0 $((COUNT - 1))); do
            local zip_name
            zip_name=$(printf "CC-MAIN-2021-31-PDF-UNTRUNCATED-%04d.zip" "$i")
            log_info "Downloading ${zip_name}..."
            wget -q -P "$dest" "${base_url}/${zip_name}" 2>/dev/null || {
                log_warn "Could not download ${zip_name}; skipping."
                continue
            }
            unzip -q -o "${dest}/${zip_name}" -d "$dest" || true
            rm -f "${dest}/${zip_name}"
        done
    else
        for zip_name in $zips; do
            log_info "Downloading ${zip_name}..."
            wget -q -P "${TEMP_DIR}" "${base_url}/${zip_name}" 2>/dev/null || {
                log_warn "Could not download ${zip_name}; skipping."
                continue
            }
            unzip -q -o "${TEMP_DIR}/${zip_name}" -d "$dest" || true
        done
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "CC-MAIN downloaded to ${dest}"
}

download_govdocs() {
    local dest="${CORPORA_DIR}/govdocs1"
    if skip_if_exists "$dest" "GovDocs1"; then return 0; fi
    check_deps wget

    log_info "Downloading GovDocs1 subset (${COUNT} thread(s))..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would download ${COUNT} GovDocs1 thread(s)"
        return 0
    fi

    ensure_dir "$dest"
    local base_url="https://downloads.digitalcorpora.org/corpora/files/govdocs1"

    for i in $(seq 0 $((COUNT - 1))); do
        local thread_dir
        thread_dir=$(printf "%03d" "$i")
        log_info "Downloading thread ${thread_dir}..."
        ensure_dir "${dest}/${thread_dir}"
        wget -q -r -np -nH --cut-dirs=4 -P "${dest}/${thread_dir}" \
            -A "*.pdf" "${base_url}/${thread_dir}/" 2>/dev/null || {
            log_warn "Could not download thread ${thread_dir}; skipping."
        }
    done
    log_ok "GovDocs1 downloaded to ${dest}"
}

download_verapdf() {
    local dest="${CORPORA_DIR}/verapdf-corpus"
    if skip_if_exists "$dest" "veraPDF"; then return 0; fi
    check_deps git

    log_info "Downloading veraPDF corpus..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone veraPDF/veraPDF-corpus"
        return 0
    fi

    git clone --depth 1 https://github.com/veraPDF/veraPDF-corpus.git "$dest"
    log_ok "veraPDF corpus downloaded to ${dest}"
}

download_pdfjs() {
    local dest="${CORPORA_DIR}/pdfjs-tests"
    if skip_if_exists "$dest" "pdf.js"; then return 0; fi
    check_deps git

    log_info "Downloading pdf.js test PDFs..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone niceDev0908/pdfium (pdf.js test PDFs)"
        return 0
    fi

    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 https://github.com/niceDev0908/pdfium.git \
        "${TEMP_DIR}/pdfium"
    ensure_dir "$dest"
    if [[ -d "${TEMP_DIR}/pdfium/test/pdfs" ]]; then
        cp -r "${TEMP_DIR}/pdfium/test/pdfs/"* "$dest/"
    elif [[ -d "${TEMP_DIR}/pdfium/testing" ]]; then
        cp -r "${TEMP_DIR}/pdfium/testing/"* "$dest/"
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "pdf.js tests downloaded to ${dest}"
}

download_pdfium() {
    local dest="${CORPORA_DIR}/pdfium-tests"
    if skip_if_exists "$dest" "pdfium"; then return 0; fi
    check_deps git

    log_info "Downloading pdfium test PDFs..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would git clone niceDev0908/pdfium (pdfium testing)"
        return 0
    fi

    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 https://github.com/niceDev0908/pdfium.git \
        "${TEMP_DIR}/pdfium"
    ensure_dir "$dest"
    if [[ -d "${TEMP_DIR}/pdfium/testing" ]]; then
        cp -r "${TEMP_DIR}/pdfium/testing/"* "$dest/"
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "pdfium tests downloaded to ${dest}"
}

download_tika() {
    local dest="${CORPORA_DIR}/tika-corpus"
    if skip_if_exists "$dest" "Tika"; then return 0; fi
    check_deps wget

    log_info "Downloading Tika PDF corpus (${COUNT} file(s))..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would download ${COUNT} PDF(s) from corpora.tika.apache.org"
        return 0
    fi

    ensure_dir "$dest"
    local base_url="https://corpora.tika.apache.org/base/packaged/pdfs"

    TEMP_DIR="$(mktemp -d)"
    wget -q -O "${TEMP_DIR}/listing.html" "${base_url}/" 2>/dev/null || true
    local pdfs
    pdfs=$(grep -oiP 'href="\K[^"]+\.pdf' "${TEMP_DIR}/listing.html" 2>/dev/null | head -n "$COUNT" || true)

    if [[ -n "$pdfs" ]]; then
        for pdf_name in $pdfs; do
            log_info "Downloading ${pdf_name}..."
            wget -q -P "$dest" "${base_url}/${pdf_name}" 2>/dev/null || {
                log_warn "Could not download ${pdf_name}; skipping."
            }
        done
    else
        log_warn "Could not retrieve Tika PDF listing."
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "Tika corpus downloaded to ${dest}"
}

download_unsafe() {
    local dest="${CORPORA_DIR}/unsafe-docs"
    if skip_if_exists "$dest" "unsafe-docs"; then return 0; fi
    check_deps wget

    log_info "Downloading DARPA SafeDocs unsafe PDFs (${COUNT} file(s))..."
    if [[ "$DRY_RUN" == "true" ]]; then
        log_info "[dry-run] Would download ${COUNT} unsafe PDF(s)"
        return 0
    fi

    ensure_dir "$dest"
    local base_url="https://downloads.digitalcorpora.org/corpora/files/CC-MAIN-2021-31-UNSAFE"

    TEMP_DIR="$(mktemp -d)"
    wget -q -O "${TEMP_DIR}/listing.html" "${base_url}/" 2>/dev/null || true
    local zips
    zips=$(grep -oP 'href="\K[^"]+\.zip' "${TEMP_DIR}/listing.html" 2>/dev/null | head -n "$COUNT" || true)

    if [[ -n "$zips" ]]; then
        for zip_name in $zips; do
            log_info "Downloading ${zip_name}..."
            wget -q -P "${TEMP_DIR}" "${base_url}/${zip_name}" 2>/dev/null || {
                log_warn "Could not download ${zip_name}; skipping."
                continue
            }
            unzip -q -o "${TEMP_DIR}/${zip_name}" -d "$dest" || true
        done
    else
        log_warn "Could not retrieve unsafe-docs listing."
    fi
    rm -rf "$TEMP_DIR"
    TEMP_DIR=""
    log_ok "unsafe-docs downloaded to ${dest}"
}

download_all() {
    download_arlington
    download_safedocs
    download_pdf_diff
    download_cc_main
    download_govdocs
    download_verapdf
    download_pdfjs
    download_pdfium
    download_tika
    download_unsafe
}

show_status() {
    echo ""
    echo "=== PDF Test Corpora Status ==="
    echo ""
    local corpora=(
        "arlington:Arlington PDF Model"
        "safedocs:SafeDocs Edge-Case PDFs"
        "pdf-differences:pdf-differences Interop Tests"
        "cc-main-2021:CC-MAIN-2021 PDF Subset"
        "govdocs1:GovDocs1 Subset"
        "verapdf-corpus:veraPDF PDF/A & PDF/UA"
        "pdfjs-tests:Mozilla pdf.js Test PDFs"
        "pdfium-tests:Chromium pdfium Test PDFs"
        "tika-corpus:Apache Tika PDFs"
        "unsafe-docs:DARPA SafeDocs Malformed PDFs"
    )

    for item in "${corpora[@]}"; do
        local dir="${item%%:*}"
        local name="${item#*:}"
        local path="${CORPORA_DIR}/${dir}"

        if is_downloaded "$path"; then
            local count
            count=$(find "$path" -type f 2>/dev/null | wc -l)
            echo -e "  ${GREEN}[OK]${NC}    ${name} (${count} files)"
        else
            echo -e "  ${YELLOW}[--]${NC}    ${name} (not downloaded)"
        fi
    done
    echo ""
    echo "Corpora directory: ${CORPORA_DIR}"
    echo ""
}

# ============================================================================
# Main
# ============================================================================

usage() {
    echo "Usage: $0 <subcommand> [options]"
    echo ""
    echo "Subcommands:"
    echo "  arlington   Download Arlington PDF model TSV files"
    echo "  safedocs    Download SafeDocs edge-case PDFs"
    echo "  pdf-diff    Download pdf-differences interop tests"
    echo "  cc-main     Download CC-MAIN PDF subset (use --count N)"
    echo "  govdocs     Download GovDocs1 subset (use --count N)"
    echo "  verapdf     Download veraPDF corpus"
    echo "  pdfjs       Download pdf.js test PDFs"
    echo "  pdfium      Download pdfium test PDFs"
    echo "  tika        Download Tika PDFs (use --count N)"
    echo "  unsafe      Download unsafe-docs (use --count N)"
    echo "  all         Download all corpora"
    echo "  status      Show download status"
    echo ""
    echo "Options:"
    echo "  --count N   Number of files/ZIPs for large corpora (default: 1)"
    echo "  --force     Re-download even if present"
    echo "  --dry-run   Preview actions without downloading"
    exit 1
}

if [[ $# -lt 1 ]]; then
    usage
fi

# Collect subcommands first, then parse options
SUBCOMMANDS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --count)
            COUNT="${2:-1}"
            shift 2
            ;;
        --force)
            FORCE=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --help|-h)
            usage
            ;;
        -*)
            log_error "Unknown option: $1"
            usage
            ;;
        *)
            SUBCOMMANDS+=("$1")
            shift
            ;;
    esac
done

if [[ ${#SUBCOMMANDS[@]} -eq 0 ]]; then
    usage
fi

ensure_dir "$CORPORA_DIR"

for subcmd in "${SUBCOMMANDS[@]}"; do
    case "$subcmd" in
        arlington)  download_arlington ;;
        safedocs)   download_safedocs ;;
        pdf-diff)   download_pdf_diff ;;
        cc-main)    download_cc_main ;;
        govdocs)    download_govdocs ;;
        verapdf)    download_verapdf ;;
        pdfjs)      download_pdfjs ;;
        pdfium)     download_pdfium ;;
        tika)       download_tika ;;
        unsafe)     download_unsafe ;;
        all)        download_all ;;
        status)     show_status ;;
        *)
            log_error "Unknown subcommand: $subcmd"
            usage
            ;;
    esac
done
