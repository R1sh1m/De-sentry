# Stop De-Sentry cluster via WSL Ubuntu
$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$desentryDir = Join-Path $projectRoot "desentry"
$wslPath = ($desentryDir -replace '\\', '/').Replace('C:', '/mnt/c').Replace('c:', '/mnt/c')

Write-Host "Stopping De-Sentry cluster..." -ForegroundColor Cyan
wsl -d Ubuntu -e bash -c "cd '$wslPath' && ./scripts/stop_cluster.sh"
Write-Host "Cluster stopped." -ForegroundColor Green
