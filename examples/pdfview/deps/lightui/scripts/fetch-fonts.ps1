# scripts/fetch-fonts.ps1
#
# Downloads open font equivalents of the PDF Standard 14 fonts,
# Noto Sans CJK for Japanese, Simplified Chinese, Traditional Chinese,
# and Korean, and Noto Emoji (monochrome) for emoji/symbol rendering.
#
# Run from the repository root:
#   pwsh scripts/fetch-fonts.ps1
#
# To skip large Noto CJK downloads, set NOTO_LANGS to a subset:
#   $env:NOTO_LANGS="jp"; pwsh scripts/fetch-fonts.ps1
#
# Output directory: fonts/
#   fonts/liberation/   - Liberation Sans / Serif / Mono  (Helvetica/Times/Courier)
#   fonts/freefont/     - GNU FreeFont                    (Symbol / ZapfDingbats)
#   fonts/noto/         - Noto Sans CJK                   (JP / SC / TC / KR)
#   fonts/emoji/        - Noto Emoji monochrome            (outline emoji glyphs)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$FontsDir = Join-Path $RepoRoot "fonts"

# -- Pinned versions --------------------------------------------------------
$LiberationVersion = "2.00.1"
$FreefontDate = "20120503"
$NotoCjkTag = "Sans2.004"
$NotoCjkRepo = "notofonts/noto-cjk"

$NotoLangs = if ($env:NOTO_LANGS) { $env:NOTO_LANGS -split '\s+' } else { @("jp", "sc", "tc", "kr") }
$NotoWeights = if ($env:NOTO_WEIGHTS) { $env:NOTO_WEIGHTS -split '\s+' } else { @("Regular", "Bold") }

$SourceCodeProVersion = "2.042R-u_1.062R-i"
$SourceCodeProTag = "2.042R-u%2F1.062R-i%2F1.026R-vf"

# -- Noto zip mapping -------------------------------------------------------
function Get-NotoZip {
    param([string]$Lang)
    switch ($Lang) {
        "jp" { "06_NotoSansCJKjp.zip" }
        "sc" { "08_NotoSansCJKsc.zip" }
        "tc" { "09_NotoSansCJKtc.zip" }
        "kr" { "07_NotoSansCJKkr.zip" }
        "hk" { "10_NotoSansCJKhk.zip" }
        default { "" }
    }
}

# -- Download helper ---------------------------------------------------------
function Fetch-File {
    param(
        [string]$Url,
        [string]$Dest,
        [string]$Label
    )
    if (-not $Label) { $Label = Split-Path -Leaf $Dest }
    if (Test-Path $Dest) {
        Write-Host "  skip   $Label  (already present)"
        return
    }
    Write-Host "  fetch  $Label"
    $ProgressPreference = 'SilentlyContinue'
    $retries = 3
    for ($i = 0; $i -lt $retries; $i++) {
        try {
            Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
            return
        } catch {
            if ($i -eq ($retries - 1)) { throw }
            Start-Sleep -Seconds 2
        }
    }
}

# -- 0. Source Code Pro ------------------------------------------------------
Write-Host ""
Write-Host "+- Source Code Pro"
$scpDir = Join-Path $FontsDir "sourcecodepro"
New-Item -ItemType Directory -Path $scpDir -Force | Out-Null

$scpArchive = Join-Path $scpDir "_SourceCodePro.zip"
$scpUrl = "https://github.com/adobe-fonts/source-code-pro/releases/download/${SourceCodeProTag}/OTF-source-code-pro-${SourceCodeProVersion}.zip"

$scpRegularOtf = Join-Path $scpDir "SourceCodePro-Regular.otf"
$scpRegularTtf = Join-Path $scpDir "SourceCodePro-Regular.ttf"

if (-not (Test-Path $scpRegularOtf) -and -not (Test-Path $scpRegularTtf)) {
    try {
        Fetch-File -Url $scpUrl -Dest $scpArchive -Label "SourceCodePro OTF"
    } catch {
        Write-Host "  warn   OTF download failed, trying TTF fallback"
    }
    if (Test-Path $scpArchive) {
        Write-Host "  extract..."
        Expand-Archive -Path $scpArchive -DestinationPath $scpDir -Force
        # Flatten nested .otf files
        Get-ChildItem -Path $scpDir -Recurse -Filter "*.otf" -File | Where-Object {
            $_.DirectoryName -ne $scpDir
        } | ForEach-Object {
            Move-Item -Path $_.FullName -Destination $scpDir -Force
        }
        # Remove empty subdirectories
        Get-ChildItem -Path $scpDir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { (Get-ChildItem $_.FullName -Force).Count -eq 0 } |
            ForEach-Object { Remove-Item $_.FullName -Force }
        Remove-Item -Path $scpArchive -Force -ErrorAction SilentlyContinue
        Write-Host "+- ok"
    } else {
        # TTF fallback
        $scpTtfUrl = "https://github.com/adobe-fonts/source-code-pro/raw/release/TTF/SourceCodePro-Regular.ttf"
        try { Fetch-File -Url $scpTtfUrl -Dest $scpRegularTtf -Label "SourceCodePro-Regular.ttf" } catch {}
        $scpTtfBoldUrl = "https://github.com/adobe-fonts/source-code-pro/raw/release/TTF/SourceCodePro-Bold.ttf"
        $scpBoldTtf = Join-Path $scpDir "SourceCodePro-Bold.ttf"
        try { Fetch-File -Url $scpTtfBoldUrl -Dest $scpBoldTtf -Label "SourceCodePro-Bold.ttf" } catch {}
        Write-Host "+- ok (TTF fallback)"
    }
} else {
    Write-Host "+- already installed"
}

# -- 1. Liberation Fonts ----------------------------------------------------
Write-Host ""
Write-Host "+- Liberation Fonts $LiberationVersion"
$libDir = Join-Path $FontsDir "liberation"
New-Item -ItemType Directory -Path $libDir -Force | Out-Null

$libArchive = Join-Path $libDir "_liberation-fonts-ttf-${LiberationVersion}.tar.gz"
$libUrl = "https://releases.pagure.org/liberation-fonts/liberation-fonts-ttf-${LiberationVersion}.tar.gz"
$libCheck = Join-Path $libDir "LiberationSans-Regular.ttf"

if (-not (Test-Path $libCheck)) {
    Fetch-File -Url $libUrl -Dest $libArchive -Label "liberation-fonts-ttf-${LiberationVersion}.tar.gz"
    Write-Host "  extract..."
    # Use Windows tar.exe (available on Windows 10+); avoid Git Bash tar
    & "$env:SystemRoot\System32\tar.exe" -xzf $libArchive --strip-components=1 -C $libDir
    # Remove non-ttf files
    Get-ChildItem -Path $libDir -File | Where-Object { $_.Extension -ne ".ttf" } |
        ForEach-Object { Remove-Item $_.FullName -Force }
    Remove-Item -Path $libArchive -Force -ErrorAction SilentlyContinue
    Write-Host "+- ok"
} else {
    Write-Host "+- already installed"
}

# -- 2. GNU FreeFont ---------------------------------------------------------
Write-Host ""
Write-Host "+- GNU FreeFont $FreefontDate"
$ffDir = Join-Path $FontsDir "freefont"
New-Item -ItemType Directory -Path $ffDir -Force | Out-Null

$ffArchive = Join-Path $ffDir "_freefont-ttf-${FreefontDate}.zip"
$ffUrl = "https://ftpmirror.gnu.org/gnu/freefont/freefont-ttf-${FreefontDate}.zip"
$ffCheck = Join-Path $ffDir "FreeSerif.ttf"

if (-not (Test-Path $ffCheck)) {
    Fetch-File -Url $ffUrl -Dest $ffArchive -Label "freefont-ttf-${FreefontDate}.zip"
    Write-Host "  extract..."
    Expand-Archive -Path $ffArchive -DestinationPath $ffDir -Force
    # Flatten nested .ttf files
    Get-ChildItem -Path $ffDir -Recurse -Filter "*.ttf" -File | Where-Object {
        $_.DirectoryName -ne $ffDir
    } | ForEach-Object {
        Move-Item -Path $_.FullName -Destination $ffDir -Force
    }
    # Remove empty subdirectories
    Get-ChildItem -Path $ffDir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { (Get-ChildItem $_.FullName -Force).Count -eq 0 } |
        ForEach-Object { Remove-Item $_.FullName -Force }
    Remove-Item -Path $ffArchive -Force -ErrorAction SilentlyContinue
    Write-Host "+- ok"
} else {
    Write-Host "+- already installed"
}

# -- 3. Noto Sans CJK -------------------------------------------------------
Write-Host ""
Write-Host "+- Noto Sans CJK $NotoCjkTag  (langs: $($NotoLangs -join ' ')  weights: $($NotoWeights -join ' '))"
$notoDir = Join-Path $FontsDir "noto"
New-Item -ItemType Directory -Path $notoDir -Force | Out-Null

$notoBase = "https://github.com/${NotoCjkRepo}/releases/download/${NotoCjkTag}"

foreach ($lang in $NotoLangs) {
    $zipAsset = Get-NotoZip -Lang $lang
    if (-not $zipAsset) {
        Write-Host "|  warn   unknown language '$lang' - skipping"
        continue
    }

    # Check if all requested weights are already present
    $allPresent = $true
    foreach ($weight in $NotoWeights) {
        $otf = Join-Path $notoDir "NotoSansCJK${lang}-${weight}.otf"
        if (-not (Test-Path $otf)) { $allPresent = $false; break }
    }
    if ($allPresent) {
        Write-Host "|  skip   $lang - already installed"
        continue
    }

    $zipDest = Join-Path $notoDir "_${zipAsset}"
    Fetch-File -Url "${notoBase}/${zipAsset}" -Dest $zipDest -Label "$zipAsset  (~90 MB)"

    Write-Host "|  extract $lang weights: $($NotoWeights -join ' ')"
    # Extract the full zip to a temp directory, then copy needed files
    $tempExtract = Join-Path $notoDir "_extract_${lang}"
    Expand-Archive -Path $zipDest -DestinationPath $tempExtract -Force

    foreach ($weight in $NotoWeights) {
        $otfName = "NotoSansCJK${lang}-${weight}.otf"
        $otfDest = Join-Path $notoDir $otfName
        if (Test-Path $otfDest) { continue }

        $found = Get-ChildItem -Path $tempExtract -Recurse -Filter $otfName -File | Select-Object -First 1
        if ($found) {
            Copy-Item -Path $found.FullName -Destination $otfDest -Force
            Write-Host "|    ok   $otfName"
        } else {
            Write-Host "|    warn $otfName not found in zip"
        }
    }
    Remove-Item -Path $tempExtract -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path $zipDest -Force -ErrorAction SilentlyContinue
}
Write-Host "+- done"

# -- 4. Noto Emoji (monochrome) ---------------------------------------------
Write-Host ""
Write-Host "+- Noto Emoji (monochrome)"
$emojiDir = Join-Path $FontsDir "emoji"
New-Item -ItemType Directory -Path $emojiDir -Force | Out-Null

$notoEmojiUrl = "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
$notoEmojiLicense = "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/OFL.txt"
$emojiCheck = Join-Path $emojiDir "NotoEmoji.ttf"

if (-not (Test-Path $emojiCheck)) {
    Fetch-File -Url $notoEmojiUrl -Dest $emojiCheck -Label "NotoEmoji.ttf"
    Fetch-File -Url $notoEmojiLicense -Dest (Join-Path $emojiDir "OFL.txt") -Label "OFL.txt (license)"
    Write-Host "+- ok"
} else {
    Write-Host "+- already installed"
}

# -- Summary -----------------------------------------------------------------
Write-Host ""
Write-Host "========================================"
Write-Host " Installed fonts"
Write-Host "========================================"
foreach ($subdir in @("liberation", "freefont", "noto", "emoji")) {
    $dir = Join-Path $FontsDir $subdir
    if (-not (Test-Path $dir)) { continue }
    $files = Get-ChildItem -Path $dir -File | Where-Object { $_.Extension -in @(".ttf", ".otf") }
    $count = ($files | Measure-Object).Count
    Write-Host ""
    Write-Host "  fonts/${subdir}/  ($count files)"
    $files | ForEach-Object { Write-Host "    $($_.Name)" }
}

Write-Host ""
Write-Host "========================================"
Write-Host " PDF Standard 14 mapping"
Write-Host "========================================"
Write-Host "  Helvetica*     -> fonts/liberation/LiberationSans-{Regular,Bold,Italic,BoldItalic}.ttf"
Write-Host "  Times*         -> fonts/liberation/LiberationSerif-{Regular,Bold,Italic,BoldItalic}.ttf"
Write-Host "  Courier*       -> fonts/liberation/LiberationMono-{Regular,Bold,Italic,BoldItalic}.ttf"
Write-Host "  Symbol         -> fonts/freefont/FreeSerif.ttf   (U+2200-22FF Mathematical Operators)"
Write-Host "  ZapfDingbats   -> fonts/freefont/FreeSans.ttf    (U+2700-27BF Dingbats)"
Write-Host ""
Write-Host "========================================"
Write-Host " CJK"
Write-Host "========================================"
Write-Host "  Japanese (jp)           -> fonts/noto/NotoSansCJKjp-Regular.otf"
Write-Host "  Simplified Chinese (sc) -> fonts/noto/NotoSansCJKsc-Regular.otf"
Write-Host "  Traditional Chinese (tc)-> fonts/noto/NotoSansCJKtc-Regular.otf"
Write-Host "  Korean (kr)             -> fonts/noto/NotoSansCJKkr-Regular.otf"
Write-Host ""
Write-Host "========================================"
Write-Host " Emoji"
Write-Host "========================================"
Write-Host "  Noto Emoji (mono) -> fonts/emoji/NotoEmoji.ttf  (SIL OFL 1.1)"
Write-Host ""
Write-Host "To test font rendering:"
Write-Host "  ./build/test_render tests/golden tests/output fonts/liberation/LiberationSans-Regular.ttf"
