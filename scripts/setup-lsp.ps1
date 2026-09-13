<#
.SYNOPSIS
    Regenerate per-machine LSP artifacts for De-Sentry on Windows.

.DESCRIPTION
    Writes only gitignored files (see .gitignore "Local LSP overrides"):
      1. <root>\compile_commands.json -- symlink to the newest
         build*\compile_commands.json, so clangd auto-discovers it in every
         editor (VSCode, Neovim, Zed, Emacs). Falls back to a copy when
         symlinks need privileges this shell does not have.
      2. .vscode\lsp.local.json -- seeded from lsp.local.json.example on
         first run, never overwritten afterwards.

    Idempotent: re-run after `cmake -S . -B build`, after switching build
    dirs, or after pulling new shared LSP configs.

    Run from the repository root:
        .\scripts\setup-lsp.ps1
#>

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $projectRoot

# MSYS2 installs its toolchain outside PATH sometimes; pick it up so the
# probe below (and clangd, if MSYS2-provided) sees a consistent environment.
foreach ($dir in @("C:\msys64\ucrt64\bin", "C:\msys64\usr\bin", "$env:USERPROFILE\.cargo\bin")) {
    if (($dir -ne $null) -and (Test-Path $dir) -and ($env:Path -notlike "*$dir*")) {
        $env:Path = "$dir;" + $env:Path
    }
}

# Prefer the documented default build dir; otherwise take the newest
# compile_commands.json under any build* directory (Visual Studio generator
# nests outputs, e.g. build\RelWithDebInfo).
$candidate = $null
if (Test-Path "build\compile_commands.json") {
    $candidate = "build\compile_commands.json"
} else {
    $candidate = Get-ChildItem -Recurse -Depth 3 -Filter "compile_commands.json" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\build*" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

if ($null -eq $candidate) {
    Write-Output "[lsp] no compile_commands.json found under build*\."
    Write-Output "[lsp] configure first: cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo"
    Write-Output "[lsp] (CMAKE_EXPORT_COMPILE_COMMANDS is already ON in CMakeLists.txt)"
} else {
    $link = Join-Path $projectRoot "compile_commands.json"
    if (Test-Path $link) { Remove-Item -Force $link }
    try {
        New-Item -ItemType SymbolicLink -Path $link -Target $candidate | Out-Null
        Write-Output "[lsp] linked compile_commands.json -> $candidate"
    } catch {
        Copy-Item -Force $candidate $link
        Write-Output "[lsp] copied compile_commands.json from $candidate (symlink unavailable;"
        Write-Output "[lsp] re-run this script after reconfiguring so it does not go stale)"
    }
}

if (-not (Test-Path ".vscode")) { New-Item -ItemType Directory -Path ".vscode" | Out-Null }
if (Test-Path ".vscode\lsp.local.json") {
    Write-Output "[lsp] .vscode\lsp.local.json already exists -- leaving your overrides alone"
} else {
    Copy-Item ".vscode\lsp.local.json.example" ".vscode\lsp.local.json"
    Write-Output "[lsp] seeded .vscode\lsp.local.json from example (gitignored)"
}

Write-Output "[lsp] toolchain probe (informational only):"
foreach ($tool in @("clangd", "rust-analyzer", "python", "node", "cargo", "cmake")) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) {
        try { $ver = (& $tool --version 2>$null | Select-Object -First 1) } catch { $ver = "" }
        Write-Output "  [ok] $tool $ver"
    } else {
        Write-Output "  [--] $tool not on PATH"
    }
}
Write-Output "[lsp] done. Shared configs (.clangd, pyrightconfig.json,"
Write-Output "[lsp] .vscode\settings.json) are committed; local files stay gitignored."
