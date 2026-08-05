[CmdletBinding()]
param(
    [string]$DroneHost = "root@10.8.82.81",
    [string]$RemoteRoot = "/opt/iking/match_agent",
    [datetime]$StartedAt = (Get-Date).AddMinutes(-30),
    [string]$RunId = (Get-Date -Format "yyyyMMdd-HHmmss"),
    [string]$Notes = "",
    [string]$LogPath = "",
    [switch]$NoPush
)

$ErrorActionPreference = "Stop"

$repoRoot = (& git -C (Split-Path -Parent $PSScriptRoot) rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or -not $repoRoot) {
    throw "Unable to locate the Git repository."
}

$existingChanges = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read Git status."
}
if ($existingChanges.Count -gt 0) {
    throw "The Git worktree must be clean before archiving a test run."
}

if ($RunId -notmatch '^[0-9]{8}-[0-9]{6}$') {
    throw "RunId must use the format YYYYMMDD-HHMMSS."
}

$relativeRunPath = "test-data/runs/$RunId"
$runDirectory = Join-Path $repoRoot ($relativeRunPath -replace '/', '\')
$captureDirectory = Join-Path $runDirectory "captures"
if (Test-Path -LiteralPath $runDirectory) {
    throw "Run directory already exists: $runDirectory"
}
New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null

$remoteSince = $StartedAt.ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss 'UTC'")
$remoteCaptureRoot = "$RemoteRoot/captures"

$metadataCommand = @(
    "date --iso-8601=seconds",
    "drone-status || true",
    "ps -eo pid,ppid,user,tty,stat,lstart,args | grep '[m]atch' || true",
    "sha256sum '$RemoteRoot/match.cpp' '$RemoteRoot/match' 2>/dev/null || true",
    "find '$remoteCaptureRoot' -maxdepth 1 -type f -newermt '$remoteSince' -printf '%TY-%Tm-%TdT%TH:%TM:%TS%Tz %s %p\\n' | sort"
) -join "; "

$metadata = @(& ssh $DroneHost $metadataCommand)
if ($LASTEXITCODE -ne 0) {
    throw "Unable to collect remote metadata over SSH."
}
$metadata | Set-Content -LiteralPath (Join-Path $runDirectory "metadata.txt") -Encoding UTF8

$listCommand = "find '$remoteCaptureRoot' -maxdepth 1 -type f -newermt '$remoteSince' " +
               "\( -name '*.jpg' -o -name '*.jpeg' -o -name '*.png' \) -print | sort"
$remoteFiles = @(& ssh $DroneHost $listCommand | Where-Object { $_ -and $_.Trim() })
if ($LASTEXITCODE -ne 0) {
    throw "Unable to list test captures over SSH."
}

foreach ($remoteFile in $remoteFiles) {
    & scp "${DroneHost}:$remoteFile" $captureDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to download $remoteFile"
    }
}

if ($LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        throw "LogPath does not exist: $LogPath"
    }
    Copy-Item -LiteralPath $LogPath -Destination (Join-Path $runDirectory "run.log")
}

$runInfo = [ordered]@{
    run_id = $RunId
    started_at = $StartedAt.ToString("o")
    archived_at = (Get-Date).ToString("o")
    drone_host = $DroneHost
    remote_root = $RemoteRoot
    capture_count = $remoteFiles.Count
}
$runInfo | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory "run.json") -Encoding UTF8

if ($Notes) {
    $Notes | Set-Content -LiteralPath (Join-Path $runDirectory "notes.md") -Encoding UTF8
} else {
    "No notes provided." | Set-Content -LiteralPath (Join-Path $runDirectory "notes.md") -Encoding UTF8
}

$manifest = Get-ChildItem -LiteralPath $runDirectory -Recurse -File |
    Where-Object { $_.Name -ne "manifest.json" } |
    ForEach-Object {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
        [ordered]@{
            path = $_.FullName.Substring($runDirectory.Length + 1).Replace('\', '/')
            bytes = $_.Length
            sha256 = $hash.Hash.ToLowerInvariant()
        }
    }
$manifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $runDirectory "manifest.json") -Encoding UTF8

if ($NoPush) {
    Write-Host "Archived test run locally: $runDirectory"
    return
}

& git -C $repoRoot add -- $relativeRunPath
if ($LASTEXITCODE -ne 0) {
    throw "Unable to stage test data."
}
& git -C $repoRoot commit -m "test-data: add run $RunId"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to commit test data."
}
& git -C $repoRoot push
if ($LASTEXITCODE -ne 0) {
    throw "Unable to push test data."
}

Write-Host "Archived and pushed test run: $runDirectory"
