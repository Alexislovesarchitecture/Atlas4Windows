param(
  [string]$ReferenceFile = (Join-Path $PSScriptRoot 'reference/chromium-reference.json')
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

Assert-Command git

if (-not (Test-Path $ReferenceFile)) {
  throw "Reference file not found: $ReferenceFile"
}

$reference = Get-Content $ReferenceFile -Raw | ConvertFrom-Json
$remote = [string]$reference.remote
$ref = [string]$reference.ref

$line = git ls-remote $remote $ref
if (-not $line) {
  throw "Failed to resolve $ref from $remote"
}

$commit = ($line -split '\s+')[0]
$reference.commit = $commit
$reference.pinned_at_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

$reference | ConvertTo-Json -Depth 5 | Set-Content -Path $ReferenceFile -Encoding utf8

Write-Host "Pinned $ref to $commit"
