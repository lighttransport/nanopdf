# scripts/fetch-deps.ps1
#
# Clone (or update) FreeType and HarfBuzz into ref/ at pinned commits.
# Run from the repository root:
#
#   pwsh scripts/fetch-deps.ps1
#
# To force a clean re-clone, delete the ref/ directory first:
#
#   Remove-Item -Recurse -Force ref; pwsh scripts/fetch-deps.ps1

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RefDir = Join-Path $RepoRoot "ref"

# ---------------------------------------------------------------------------
# Pinned revisions (update these when you want to pull in newer versions)
# ---------------------------------------------------------------------------
$FreetypeUrl = "https://github.com/freetype/freetype.git"
$FreetypeTag = "VER-2-13-3"

$HarfbuzzUrl = "https://github.com/harfbuzz/harfbuzz.git"
$HarfbuzzTag = "10.4.0"

# ---------------------------------------------------------------------------

function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments)]$Args)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & git @Args 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { } else { $_ }
    }
    $ErrorActionPreference = $prev
    return $LASTEXITCODE
}

function Clone-OrUpdate {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Tag
    )
    $dest = Join-Path $RefDir $Name

    if (Test-Path (Join-Path $dest ".git")) {
        Write-Host "[fetch-deps] ${Name}: already cloned - fetching tag $Tag"
        Invoke-Git -C $dest fetch --depth=1 origin "refs/tags/${Tag}:refs/tags/${Tag}" | Out-Null
        Invoke-Git -C $dest checkout $Tag --quiet | Out-Null
    } else {
        Write-Host "[fetch-deps] ${Name}: cloning tag $Tag from $Url"
        Invoke-Git clone --depth=1 --branch $Tag $Url $dest | Out-Null
    }
    $desc = (Invoke-Git -C $dest describe --tags --always) | Where-Object { $_ -is [string] } | Select-Object -First 1
    Write-Host "[fetch-deps] ${Name}: $desc"
}

if (-not (Test-Path $RefDir)) {
    New-Item -ItemType Directory -Path $RefDir -Force | Out-Null
}

Clone-OrUpdate -Name "freetype" -Url $FreetypeUrl -Tag $FreetypeTag
Clone-OrUpdate -Name "harfbuzz" -Url $HarfbuzzUrl -Tag $HarfbuzzTag

Write-Host ""
Write-Host "[fetch-deps] Done.  Build with:"
Write-Host "  cmake -B build -DLUI_BUILD_FONTS=ON"
Write-Host "  cmake --build build"
