<#
.SYNOPSIS
    One-command developer bootstrap for De-Sentry on Windows.

.DESCRIPTION
    Checks prerequisites, builds the C++ engine, installs the app's
    dependencies, fetches the sizing model (once) and stages the desentryd
    sidecar -- everything after cloning that `README.md` used to list as
    five separate manual steps. Every step is idempotent: re-running this
    after a pull only rebuilds what changed.

    Node.js and Python 3 are auto-downloaded into .local/ when missing from
    PATH. A self-contained Python virtual environment is created in .venv/.

    Run from the repository root:
        .\scripts\setup.ps1
        .\scripts\setup.ps1 -Install   # also try to install missing tools via winget

    Afterwards either run a headless mesh:
        .\scripts\run_cluster.ps1 -Nodes 3
    or the desktop control room:
        cd app; npm run tauri:dev
#>
param(
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Test-Command($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

# MSYS2 installs its toolchain outside PATH sometimes; pick it up so the
# checks below (and the build after them) see one consistent environment.
# Same for rustup, which lands in %USERPROFILE%\.cargo\bin and is only on
# PATH for shells started after it was installed.
foreach ($dir in @("C:\msys64\ucrt64\bin", "C:\msys64\usr\bin", "$env:USERPROFILE\.cargo\bin")) {
    if (($dir -ne $null) -and (Test-Path $dir) -and ($env:Path -notlike "*$dir*")) {
        $env:Path = "$dir;" + $env:Path
    }
}

# ── Local tool directory (gitignored) ─────────────────────────────────────
$localDir     = Join-Path $projectRoot ".local"
$nodeVersion  = "22.11.0"

# ── Auto-download Node.js if not on PATH ──────────────────────────────────
function Ensure-Node {
    if (Test-Command "node") { return }
    Write-Host "[setup] Node.js not found on PATH -- downloading v$nodeVersion" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $localDir -Force | Out-Null

    $zipPath   = Join-Path $localDir "node.zip"
    $url       = "https://nodejs.org/dist/v${nodeVersion}/node-v${nodeVersion}-win-x64.zip"
    Write-Host "  Downloading from $url" -ForegroundColor Gray
    Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing

    Expand-Archive -Path $zipPath -DestinationPath $localDir -Force
    Remove-Item $zipPath

    $extracted = Get-ChildItem -Directory $localDir | Where-Object { $_.Name -like "node-v${nodeVersion}-*" } | Select-Object -First 1
    if ($null -eq $extracted) {
        Write-Error "[setup] Failed to extract Node.js archive"
        return
    }
    Rename-Item -Path $extracted.FullName -NewName "node"
    $env:Path = (Join-Path $localDir "node") + ";" + $env:Path
    Write-Host "  [ok] node $(node -v)" -ForegroundColor Gray
}

# ── Auto-download Python 3 if not on PATH ─────────────────────────────────
function Ensure-Python {
    if (Test-Command "python") { return }
    Write-Host "[setup] Python 3 not found on PATH -- downloading Miniconda" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $localDir -Force | Out-Null

    $exePath = Join-Path $localDir "miniconda.exe"
    $url     = "https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe"
    Write-Host "  Downloading from $url" -ForegroundColor Gray
    Invoke-WebRequest -Uri $url -OutFile $exePath -UseBasicParsing

    $condaDir = Join-Path $localDir "miniconda"
    & $exePath /InstallationType=JustMe /RegisterPython=0 /AddToPath=0 /S /D="$condaDir"
    Remove-Item $exePath
    $env:Path = "$condaDir;" + "$condaDir\Scripts;" + $env:Path
    Write-Host "  [ok] python $(python --version)" -ForegroundColor Gray
}

$missing = @()
function Require-Tool($name, $hint) {
    if (Test-Command $name) {
        Write-Host ("  [ok] {0,-10} {1}" -f $name, (& $name --version 2>$null | Select-Object -First 1)) -ForegroundColor Gray
    } else {
        Write-Host ("  [!!] {0,-10} missing -- {1}" -f $name, $hint) -ForegroundColor Yellow
        $script:missing += $name
    }
}

Write-Host "1/6 prerequisites" -ForegroundColor Cyan
Require-Tool "cmake" "winget install -e --id Kitware.CMake"
Require-Tool "ninja" "comes with the MSYS2 package mingw-w64-ucrt-x86_64-ninja"
Require-Tool "g++" "MSYS2 UCRT64 (https://www.msys2.org) or Visual Studio Build Tools"
Require-Tool "cargo" "winget install -e --id Rustlang.Rustup  (then the MSVC *and* GNU toolchains as needed)"
Require-Tool "node" "auto-downloads into .local\node"
Require-Tool "python" "auto-downloads Miniconda into .local\miniconda"
if (-not (Test-Path "C:\msys64\ucrt64\include\openssl\evp.h")) {
    Write-Host "  [!!] OpenSSL headers not found under MSYS2 UCRT64 -- pacman -S mingw-w64-ucrt-x86_64-openssl" -ForegroundColor Yellow
    $missing += "openssl-headers"
} else {
    Write-Host "  [ok] openssl-headers (MSYS2 UCRT64)" -ForegroundColor Gray
}

# ── Attempt auto-download for node and python ─────────────────────────────
if ($missing -contains "node")   { Ensure-Node }
if ($missing -contains "python") { Ensure-Python }

# Re-check: remove any tool that is now available
$newMissing = @()
foreach ($tool in $missing) {
    if (Test-Command $tool) {
        Write-Host ("  [ok] {0,-10} (installed by setup)" -f $tool) -ForegroundColor Gray
    } else {
        $newMissing += $tool
    }
}
$missing = $newMissing

if ($missing.Count -gt 0) {
    if (-not $Install) {
        Write-Error ("Missing tools: {0}. Re-run with -Install to try winget, or install them by hand." -f ($missing -join ", "))
    }
    Write-Host "Trying winget for the remaining missing tools..." -ForegroundColor Cyan
    $wingetMap = @{
        "cmake"   = "Kitware.CMake"
        "cargo"   = "Rustlang.Rustup"
    }
    foreach ($tool in $missing) {
        if ($wingetMap.ContainsKey($tool)) {
            winget install -e --id $wingetMap[$tool] --silent --accept-source-agreements --accept-package-agreements
        } else {
            Write-Warning "no winget package known for '$tool' -- install it by hand, then re-run setup."
        }
    }
    Write-Host "Re-run .\scripts\setup.ps1 now that the tools are installed." -ForegroundColor Green
    return
}

Write-Host "2/6 Python virtual environment" -ForegroundColor Cyan
$venvDir = Join-Path $projectRoot ".venv"
if (-not (Test-Path $venvDir)) {
    Write-Host "  Creating virtual environment at $venvDir" -ForegroundColor Gray
    & python -m venv $venvDir
}
$venvPython = Join-Path $venvDir "Scripts\python.exe"
Write-Host ("  venv: {0} ({1})" -f (Get-Command $venvPython | Select-Object -ExpandProperty Source),
                                   (& $venvPython --version)) -ForegroundColor Gray

# Install any declared Python requirements
foreach ($reqFile in @(
    Join-Path $projectRoot "clients\python\requirements.txt",
    Join-Path $projectRoot "app\scripts\requirements.txt"
)) {
    if (Test-Path $reqFile) {
        Write-Host ("  pip install -r {0}" -f (Split-Path $reqFile -Leaf)) -ForegroundColor Gray
        & $venvPython -m pip install -q -r $reqFile
    }
}

Write-Host "3/6 engine (CMake configure + build)" -ForegroundColor Cyan
$generatorArgs = @()
if (Test-Command "ninja") { $generatorArgs = @("-G", "Ninja") }
# Keep the compiler ABI aligned with the UCRT64 OpenSSL headers/libraries
# checked above. PATH also contains C:\msys64\usr\bin for utilities, so a
# stale CMake cache (or PATH lookup) can otherwise select the Cygwin compiler.
$toolchainArgs = @()
$ucrtGcc = "C:\msys64\ucrt64\bin\gcc.exe"
$ucrtGxx = "C:\msys64\ucrt64\bin\g++.exe"
if ((Test-Path $ucrtGcc) -and (Test-Path $ucrtGxx)) {
    $toolchainArgs = @("-DCMAKE_C_COMPILER=$ucrtGcc", "-DCMAKE_CXX_COMPILER=$ucrtGxx")
}
& cmake -S $projectRoot -B "$projectRoot\build" @generatorArgs @toolchainArgs -DCMAKE_BUILD_TYPE=RelWithDebInfo
if ($LASTEXITCODE -ne 0) {
    # A stale cache (e.g. configured once with a different generator or
    # toolchain) fails exactly like this; wiping it and retrying once is
    # safe because configure regenerates everything from CMakeLists.txt.
    Write-Host "  configure failed -- clearing a possibly stale CMake cache and retrying once" -ForegroundColor Yellow
    Remove-Item "$projectRoot\build\CMakeCache.txt" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "$projectRoot\build\CMakeFiles" -ErrorAction SilentlyContinue
    & cmake -S $projectRoot -B "$projectRoot\build" @generatorArgs @toolchainArgs -DCMAKE_BUILD_TYPE=RelWithDebInfo
}
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& cmake --build "$projectRoot\build" -j
if ($LASTEXITCODE -ne 0) { throw "engine build failed" }

$engineBin = Get-ChildItem "$projectRoot\build\desentryd.exe",
    "$projectRoot\build\RelWithDebInfo\desentryd.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $engineBin) { throw "build finished but desentryd.exe was not produced" }
Write-Host ("  engine ready: {0} ({1:N1} MiB)" -f $engineBin.FullName, ($engineBin.Length / 1MB)) -ForegroundColor Gray

Write-Host "4/6 app dependencies (npm install)" -ForegroundColor Cyan
Push-Location "$projectRoot\app"
try {
    & npm install --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm install failed" }

    Write-Host "5/6 sizing model (one-time download, skipped when present)" -ForegroundColor Cyan
    & node scripts/fetch-model.mjs
    if ($LASTEXITCODE -ne 0) { throw "fetch-model failed" }

    Write-Host "6/6 stage desentryd as the Tauri sidecar" -ForegroundColor Cyan
    & node scripts/stage-sidecar.mjs
    if ($LASTEXITCODE -ne 0) { throw "stage-sidecar failed" }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Setup complete. Next:" -ForegroundColor Green
Write-Host "  .\scripts\run_cluster.ps1 -Nodes 3   # headless 3-node mesh (API 7701-7703)"
Write-Host "  cd app; npm run tauri:dev            # desktop control room"
Write-Host "  cd app; npm run tauri:build          # installer (needs WiX: dotnet tool install -g wix)"
