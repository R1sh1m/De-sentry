<#
.SYNOPSIS
    Stops a cluster started by run_cluster.ps1.

.DESCRIPTION
    Reads the pid files run_cluster.ps1 wrote and stops those processes.

    Two safeguards, because "stop the cluster" must never mean "stop something
    else that happens to have that pid":

      * Each pid is checked to be a live `desentryd` process before it is
        stopped. Pids are reused, and a stale pid file from a previous boot
        could otherwise name somebody's editor.
      * A stale pid file is removed rather than reported as an error. Its
        process is already gone, which is what the caller wanted.
#>
$ErrorActionPreference = "Stop"

$runDir = Join-Path $env:LOCALAPPDATA "desentry_cluster"
if (-not (Test-Path $runDir)) {
    Write-Host "No cluster directory at $runDir -- nothing to stop." -ForegroundColor Gray
    return
}

$pidFiles = Get-ChildItem -Path $runDir -Filter *.pid -ErrorAction SilentlyContinue
if (-not $pidFiles) {
    Write-Host "No running cluster recorded in $runDir." -ForegroundColor Gray
    return
}

$stopped = 0
foreach ($file in $pidFiles) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    $processId = (Get-Content $file.FullName -ErrorAction SilentlyContinue | Select-Object -First 1)

    $target = $null
    if ($processId -match '^\d+$') {
        $target = Get-Process -Id ([int]$processId) -ErrorAction SilentlyContinue
    }

    if ($null -eq $target) {
        Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
        continue
    }

    # The name check is what stops a recycled pid from taking an unrelated
    # process with it.
    if ($target.ProcessName -ne "desentryd") {
        Write-Host "  skipping pid $processId ($($target.ProcessName)) -- not a desentryd" -ForegroundColor Yellow
        Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
        continue
    }

    Stop-Process -Id $target.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
    Write-Host "  stopped $name (pid $($target.Id))" -ForegroundColor Gray
    $stopped++
}

Write-Host "Stopped $stopped node(s). Data directories are left in $runDir." -ForegroundColor Green
