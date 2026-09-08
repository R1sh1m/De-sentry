<#
.SYNOPSIS
    Starts a local De-Sentry cluster on Windows.

.DESCRIPTION
    The native counterpart of run_cluster.sh: N `desentryd.exe` processes, each
    with its own data directory, API port and P2P port, bootstrapped to know
    about each other.

    v1 shelled out to WSL. That worked while the engine was Linux-only; it does
    not now that the tree builds natively on Windows, and it left the Windows
    build path effectively untested. This drives desentryd.exe directly.

.PARAMETER Nodes
    Number of data nodes. Default 3.

.PARAMETER Supervisor
    Also start an app-local supervisor on 7700/7800. It holds no data and is
    never a replication hop; it exists for hardware discovery, quota checks and
    the quorum-gated checkpoint.

.PARAMETER Engines
    Comma-separated storage engines for every node. Default "kv". Names must be
    compiled into the binary -- ask it with GET /_engines.

.EXAMPLE
    .\scripts\run_cluster.ps1 -Nodes 3 -Supervisor
#>
param(
    [int]$Nodes = 3,
    [switch]$Supervisor,
    [string]$Engines = "kv"
)

$ErrorActionPreference = "Stop"

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

# CMake puts the binary in a per-configuration subdirectory with the Visual
# Studio generator and directly in build/ with Ninja, so both are checked
# rather than making the caller remember which generator they used.
$candidates = @(
    (Join-Path $projectRoot "build\desentryd.exe"),
    (Join-Path $projectRoot "build\RelWithDebInfo\desentryd.exe"),
    (Join-Path $projectRoot "build\Release\desentryd.exe"),
    (Join-Path $projectRoot "build\Debug\desentryd.exe")
)
$bin = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bin) {
    Write-Error @"
desentryd.exe not found. Build it first:
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build --config RelWithDebInfo
Looked in:
$($candidates -join "`n")
"@
}

$runDir = Join-Path $env:LOCALAPPDATA "desentry_cluster"

& (Join-Path $scriptDir "stop_cluster.ps1") 2>$null
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Get-ChildItem -Path $runDir -Filter *.pid -ErrorAction SilentlyContinue | Remove-Item -Force

$engineList = ($Engines -split ",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$defaultEngine = $engineList[0]

function Start-DesentryNode {
    param(
        [string]$Name,
        [int]$ApiPort,
        [int]$P2pPort,
        [bool]$IsSupervisor,
        [string[]]$BootstrapPeers
    )

    $dataDir = Join-Path $runDir "$Name\data"
    New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
    $configPath = Join-Path $runDir "$Name\node.json"

    # A supervisor binds everything to loopback and does not advertise itself.
    # NodeConfig::Validate() enforces the API side and refuses to start
    # otherwise, so writing anything else here would only produce a node that
    # will not run.
    $p2pBind   = if ($IsSupervisor) { "127.0.0.1" } else { "0.0.0.0" }
    $discovery = -not $IsSupervisor

    $config = [ordered]@{
        node_name     = $Name
        data_dir      = $dataDir
        api_bind_addr = "127.0.0.1"
        api_port      = $ApiPort
        p2p_bind_addr = $p2pBind
        p2p_port      = $P2pPort

        discovery_enabled     = $discovery
        discovery_port        = 7901
        discovery_interval_ms = 2000
        bootstrap_peers       = @($BootstrapPeers)
        gossip_interval_ms    = 2000
        buffer_pool_pages     = 1024

        supervisor  = [bool]$IsSupervisor
        quota_mb    = 0
        quota_split = [ordered]@{
            db            = 60
            transit_store = 15
            cache_hash    = 10
            ledger        = 10
            net_buffers   = 5
        }
        engines             = @($engineList)
        default_engine      = $defaultEngine
        replication_factor  = 3
        transit_ttl_seconds = 604800
        retention_days      = 0

        encrypt_at_rest = $false
        keychain_ref    = ""

        max_peer_threads        = 8
        peer_rate_limit_per_sec = 200
        peer_rate_burst         = 400
        mdns_enabled            = $discovery
        advertise_hostname      = ""
    }

    # -Depth 5 because quota_split is nested; the default of 2 would serialise
    # it as a type name and the node would silently fall back to defaults.
    $config | ConvertTo-Json -Depth 5 | Out-File -FilePath $configPath -Encoding utf8

    $process = Start-Process -FilePath $bin `
        -ArgumentList @("--config", $configPath) `
        -RedirectStandardOutput (Join-Path $runDir "$Name.log") `
        -RedirectStandardError  (Join-Path $runDir "$Name.err.log") `
        -WindowStyle Hidden -PassThru

    $process.Id | Out-File -FilePath (Join-Path $runDir "$Name.pid") -Encoding ascii
    Write-Host ("started {0,-12} api=http://127.0.0.1:{1}  p2p=127.0.0.1:{2}  pid={3}" -f `
        $Name, $ApiPort, $P2pPort, $process.Id) -ForegroundColor Gray
}

Write-Host "Starting De-Sentry cluster ($Nodes data nodes, engines: $Engines)..." -ForegroundColor Cyan

for ($i = 0; $i -lt $Nodes; $i++) {
    # Each node's bootstrap list is every other data node. The supervisor is
    # never in it: it holds no data and is not a replication hop.
    $peers = @()
    for ($j = 0; $j -lt $Nodes; $j++) {
        if ($j -ne $i) { $peers += "127.0.0.1:$(7801 + $j)" }
    }
    Start-DesentryNode -Name "node$i" -ApiPort (7701 + $i) -P2pPort (7801 + $i) `
        -IsSupervisor $false -BootstrapPeers $peers
}

if ($Supervisor) {
    # The supervisor still has to reach the data nodes to collect tips for a
    # checkpoint, so it gets them as bootstrap peers even though no data routes
    # through it.
    $allPeers = @()
    for ($j = 0; $j -lt $Nodes; $j++) { $allPeers += "127.0.0.1:$(7801 + $j)" }
    Start-DesentryNode -Name "supervisor" -ApiPort 7700 -P2pPort 7800 `
        -IsSupervisor $true -BootstrapPeers $allPeers
}

Write-Host ""
Write-Host "Cluster is up. Try:" -ForegroundColor Green
Write-Host "  Invoke-RestMethod http://127.0.0.1:7701/_brain" -ForegroundColor Gray
Write-Host "  Invoke-RestMethod http://127.0.0.1:7701/_engines" -ForegroundColor Gray
if ($Supervisor) {
    Write-Host "  Invoke-RestMethod -Method Post http://127.0.0.1:7700/_checkpoint" -ForegroundColor Gray
    Write-Host "  Invoke-RestMethod http://127.0.0.1:7700/_supervisor/topology" -ForegroundColor Gray
}

$dashPath = "file:///" + ((Join-Path $projectRoot "tools\dashboard.html") -replace '\\', '/')
Write-Host ""
Write-Host "Dashboard: $dashPath" -ForegroundColor Yellow
Write-Host "Logs:      $runDir\node<i>.log" -ForegroundColor Gray
Write-Host "Stop with: .\scripts\stop_cluster.ps1" -ForegroundColor Gray
