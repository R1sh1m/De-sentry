# Run De-Sentry cluster via WSL Ubuntu
param(
    [int]$Nodes = 3
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$desentryDir = Join-Path $projectRoot "desentry"
$wslPath = ($desentryDir -replace '\\', '/').Replace('C:', '/mnt/c').Replace('c:', '/mnt/c')

Write-Host "Starting De-Sentry cluster ($Nodes nodes)..." -ForegroundColor Cyan
wsl -d Ubuntu -e bash -c "cd '$wslPath' && ./scripts/run_cluster.sh $Nodes"

Write-Host ""
Write-Host "Nodes are accessible on localhost:" -ForegroundColor Green
for ($i = 0; $i -lt $Nodes; $i++) {
    $apiPort = 7701 + $i
    Write-Host "  Node $i API: http://127.0.0.1:$apiPort" -ForegroundColor Gray
}

$dashPath = "file:///" + ((Join-Path $desentryDir "tools\dashboard.html") -replace '\\', '/')
Write-Host ""
Write-Host "Web Dashboard: $dashPath" -ForegroundColor Yellow
