# scripts/fetch-verify-deps.ps1
#
# Clone (or update) blend2d and thorvg into ref/ for canvas verification.
# Run from the repository root:
#
#   pwsh scripts/fetch-verify-deps.ps1
#
# To force a clean re-clone, delete the directories first:
#
#   Remove-Item -Recurse -Force ref/blend2d, ref/asmjit, ref/thorvg
#   pwsh scripts/fetch-verify-deps.ps1
#
# These libraries are used as reference renderers to verify lightui's
# shape & stroke primitives produce correct output.

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RefDir = Join-Path $RepoRoot "ref"

# ---------------------------------------------------------------------------
# Pinned revisions (update these when you want to pull in newer versions)
# ---------------------------------------------------------------------------

# blend2d requires asmjit as a sibling directory for JIT compilation
$AsmjitUrl = "https://github.com/asmjit/asmjit.git"
$AsmjitTag = "master"

$Blend2dUrl = "https://github.com/blend2d/blend2d.git"
$Blend2dTag = "master"

$ThorvgUrl = "https://github.com/thorvg/thorvg.git"
$ThorvgTag = "v1.0"

# ---------------------------------------------------------------------------

function Invoke-Git {
    # Run git without $ErrorActionPreference = Stop treating stderr as fatal
    param([Parameter(ValueFromRemainingArguments)]$Args)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & git @Args 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            # git progress/info on stderr — ignore
        } else {
            $_
        }
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
        Write-Host "[fetch-verify-deps] ${Name}: already cloned - fetching $Tag"
        Invoke-Git -C $dest fetch --depth=1 origin $Tag | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Invoke-Git -C $dest fetch --depth=1 origin "refs/tags/${Tag}:refs/tags/${Tag}" | Out-Null
        }
        Invoke-Git -C $dest checkout $Tag --quiet | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Invoke-Git -C $dest checkout "FETCH_HEAD" --quiet | Out-Null
        }
    } else {
        Write-Host "[fetch-verify-deps] ${Name}: cloning $Tag from $Url"
        Invoke-Git clone --depth=1 --branch $Tag $Url $dest | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Invoke-Git clone --depth=1 $Url $dest | Out-Null
        }
    }
    $desc = (Invoke-Git -C $dest describe --tags --always) | Where-Object { $_ -is [string] } | Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or -not $desc) {
        $desc = (Invoke-Git -C $dest rev-parse --short HEAD) | Where-Object { $_ -is [string] } | Select-Object -First 1
    }
    Write-Host "[fetch-verify-deps] ${Name}: $desc"
}

if (-not (Test-Path $RefDir)) {
    New-Item -ItemType Directory -Path $RefDir -Force | Out-Null
}

# blend2d needs asmjit as a sibling or inside its 3rdparty directory
Clone-OrUpdate -Name "asmjit"  -Url $AsmjitUrl  -Tag $AsmjitTag
Clone-OrUpdate -Name "blend2d" -Url $Blend2dUrl -Tag $Blend2dTag

# Link asmjit into blend2d's 3rdparty directory if not already there
$blend2d3rd = Join-Path (Join-Path $RefDir "blend2d") "3rdparty"
$asmjitLink = Join-Path $blend2d3rd "asmjit"
if (-not (Test-Path $asmjitLink)) {
    if (-not (Test-Path $blend2d3rd)) {
        New-Item -ItemType Directory -Path $blend2d3rd -Force | Out-Null
    }
    $asmjitSrc = Join-Path $RefDir "asmjit"
    # Use junction (no admin required) instead of symlink
    New-Item -ItemType Junction -Path $asmjitLink -Target $asmjitSrc | Out-Null
    Write-Host "[fetch-verify-deps] blend2d: linked asmjit into 3rdparty/"
}

Clone-OrUpdate -Name "thorvg" -Url $ThorvgUrl -Tag $ThorvgTag

Write-Host ""
Write-Host "[fetch-verify-deps] Done."
Write-Host ""
Write-Host "  ref/blend2d/   blend2d 2D rendering library"
Write-Host "  ref/asmjit/    asmjit JIT assembler (blend2d dependency)"
Write-Host "  ref/thorvg/    thorvg vector graphics library"
Write-Host ""
Write-Host "Build verification tests with:"
Write-Host "  cmake -B build -DLUI_BUILD_VERIFY=ON"
Write-Host "  cmake --build build --target verify_blend2d verify_thorvg"
