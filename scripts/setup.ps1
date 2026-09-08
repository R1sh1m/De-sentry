<#
.SYNOPSIS
    One-command developer bootstrap for De-Sentry on Windows.

.DESCRIPTION
    Checks prerequisites, builds the C++ engine, installs the app's
    dependencies, fetches the sizing model (once) and stages the desentryd
    sidecar -- everything after cloning that `README.md` used to list as
    five separate manual steps. Every step is idempotent: re-running this
    after a pull only rebuilds what changed.

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

$missing = @()
function Require-Tool($name, $hint) {
    if (Test-Command $name) {
        Write-Host ("  [ok] {0,-10} {1}" -f $name, (& $name --version 2>$null | Select-Object -First 1)) -ForegroundColor Gray
    } else {
        Write-Host ("  [!!] {0,-10} missing -- {1}" -f $name, $hint) -ForegroundColor Yellow
        $script:missing += $name
    }
}

Write-Host "1/5 prerequisites" -ForegroundColor Cyan
Require-Tool "cmake" "winget install -e --id Kitware.CMake"
Require-Tool "ninja" "comes with the MSYS2 package mingw-w64-ucrt-x86_64-ninja"
Require-Tool "g++" "MSYS2 UCRT64 (https://www.msys2.org) or Visual Studio Build Tools"
Require-Tool "cargo" "winget install -e --id Rustlang.Rustup  (then the MSVC *and* GNU toolchains as needed)"
Require-Tool "node" "winget install -e --id OpenJS.NodeJS.LTS"
Require-Tool "python3" "winget install -e --id Python.Python.3"
if (-not (Test-Path "C:\msys64\ucrt64\include\openssl\evp.h")) {
    Write-Host "  [!!] OpenSSL headers not found under MSYS2 UCRT64 -- pacman -S mingw-w64-ucrt-x86_64-openssl" -ForegroundColor Yellow
    $missing += "openssl-headers"
} else {
    Write-Host "  [ok] openssl-headers (MSYS2 UCRT64)" -ForegroundColor Gray
}

if ($missing.Count -gt 0) {
    if (-not $Install) {
        Write-Error ("Missing tools: {0}. Re-run with -Install to try winget, or install them by hand." -f ($missing -join ", "))
    }
    Write-Host "Trying winget for the missing tools..." -ForegroundColor Cyan
    $wingetMap = @{
        "cmake"   = "Kitware.CMake"
        "cargo"   = "Rustlang.Rustup"
        "node"    = "OpenJS.NodeJS.LTS"
        "python3" = "Python.Python.3"
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

Write-Host "2/5 engine (CMake configure + build)" -ForegroundColor Cyan
$generatorArgs = @()
if (Test-Command "ninja") { $generatorArgs = @("-G", "Ninja") }
& cmake -S $projectRoot -B "$projectRoot\build" @generatorArgs -DCMAKE_BUILD_TYPE=RelWithDebInfo
if ($LASTEXITCODE -ne 0) {
    # A stale cache (e.g. configured once with a different generator or
    # toolchain) fails exactly like this; wiping it and retrying once is
    # safe because configure regenerates everything from CMakeLists.txt.
    Write-Host "  configure failed -- clearing a possibly stale CMake cache and retrying once" -ForegroundColor Yellow
    Remove-Item "$projectRoot\build\CMakeCache.txt" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "$projectRoot\build\CMakeFiles" -ErrorAction SilentlyContinue
    & cmake -S $projectRoot -B "$projectRoot\build" @generatorArgs -DCMAKE_BUILD_TYPE=RelWithDebInfo
}
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& cmake --build "$projectRoot\build" -j
if ($LASTEXITCODE -ne 0) { throw "engine build failed" }

$engineBin = Get-ChildItem "$projectRoot\build\desentryd.exe",
    "$projectRoot\build\RelWithDebInfo\desentryd.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $engineBin) { throw "build finished but desentryd.exe was not produced" }
Write-Host ("  engine ready: {0} ({1:N1} MiB)" -f $engineBin.FullName, ($engineBin.Length / 1MB)) -ForegroundColor Gray

Write-Host "3/5 app dependencies (npm install)" -ForegroundColor Cyan
Push-Location "$projectRoot\app"
try {
    & npm install --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm install failed" }

    Write-Host "4/5 sizing model (one-time download, skipped when present)" -ForegroundColor Cyan
    & node scripts/fetch-model.mjs
    if ($LASTEXITCODE -ne 0) { throw "fetch-model failed" }

    Write-Host "5/5 stage desentryd as the Tauri sidecar" -ForegroundColor Cyan
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
