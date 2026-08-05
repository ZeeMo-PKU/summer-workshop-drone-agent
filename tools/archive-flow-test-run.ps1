[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]{8}-[0-9]{6}$')]
    [string]$RunId,
    [string]$DroneHost = "root@10.8.82.81",
    [string]$RemoteRoot = "/opt/iking/match_agent_flow_test",
    [switch]$NoPush
)

$ErrorActionPreference = "Stop"

$repoRoot = (& git -C (Split-Path -Parent $PSScriptRoot) rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or -not $repoRoot) {
    throw "Unable to locate the Git repository."
}

$changes = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read Git status."
}
if ($changes.Count -gt 0) {
    throw "The Git worktree must be clean before archiving a flow test."
}

$relativeDestination = "test-data/runs/$RunId-flow-test"
$destination = Join-Path $repoRoot ($relativeDestination -replace '/', '\')
if (Test-Path -LiteralPath $destination) {
    throw "Destination already exists: $destination"
}

$remoteRun = "$RemoteRoot/runs/$RunId"
& ssh $DroneHost "test -d '$remoteRun' -a -f '$remoteRun/SHA256SUMS'"
if ($LASTEXITCODE -ne 0) {
    throw "Remote run is missing or incomplete: $remoteRun"
}

New-Item -ItemType Directory -Path $destination | Out-Null
& scp -r "${DroneHost}:$remoteRun/." $destination
if ($LASTEXITCODE -ne 0) {
    throw "Unable to download $remoteRun"
}

$expected = @{}
Get-Content -LiteralPath (Join-Path $destination "SHA256SUMS") -Encoding UTF8 |
    ForEach-Object {
        if ($_ -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
            throw "Invalid SHA256SUMS line: $_"
        }
        $expected[$Matches[2]] = $Matches[1].ToLowerInvariant()
    }

foreach ($entry in $expected.GetEnumerator()) {
    $localPath = Join-Path $destination ($entry.Key -replace '/', '\')
    if (-not (Test-Path -LiteralPath $localPath -PathType Leaf)) {
        throw "Manifest file is missing: $($entry.Key)"
    }
    $actual = (Get-FileHash -LiteralPath $localPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "SHA-256 mismatch: $($entry.Key)"
    }
}

$manifestFiles = @($expected.Keys | ForEach-Object { $_ -replace '\\', '/' } | Sort-Object)
$downloadedFiles = @(
    Get-ChildItem -LiteralPath $destination -Recurse -File |
        ForEach-Object {
            $_.FullName.Substring($destination.Length + 1).Replace('\', '/')
        } |
        Where-Object { $_ -ne 'SHA256SUMS' } |
        Sort-Object
)
if (Compare-Object -ReferenceObject $manifestFiles -DifferenceObject $downloadedFiles) {
    throw "Downloaded files do not exactly match SHA256SUMS."
}

$sensitive = @(
    & rg -n -i "DASHSCOPE_API_KEY|sk-[A-Za-z0-9_.-]{10,}|password|github_pat|ghp_" $destination
)
if ($LASTEXITCODE -eq 0 -and $sensitive.Count -gt 0) {
    throw "Potential credential material found in archived run."
}
if ($LASTEXITCODE -gt 1) {
    throw "Credential scan failed."
}

if ($NoPush) {
    Write-Host "Archived and verified locally: $destination"
    return
}

& git -C $repoRoot add -- $relativeDestination
if ($LASTEXITCODE -ne 0) { throw "Unable to stage flow-test data." }
& git -C $repoRoot commit -m "test-data: add isolated flow run $RunId"
if ($LASTEXITCODE -ne 0) { throw "Unable to commit flow-test data." }
& git -C $repoRoot push
if ($LASTEXITCODE -ne 0) { throw "Unable to push flow-test data." }

Write-Host "Archived, verified and pushed: $destination"
