param(
  [string]$WorkspaceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
  [string]$ChromiumDir = "chromium",
  [switch]$NoSync,
  [switch]$SkipFetch
)

$ErrorActionPreference = 'Stop'

function Assert-Command {
  param([string]$Name)
  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if (-not $cmd) {
    throw "Required command not found in PATH: $Name"
  }
}

$repoRoot = Resolve-Path $WorkspaceRoot
$root = Join-Path $repoRoot $ChromiumDir

Write-Host "Workspace root: $repoRoot"
Write-Host "Chromium directory: $root"

$required = @('python','git','python3')
foreach ($c in $required) {
  if (Get-Command $c -ErrorAction SilentlyContinue) { break }
}
if (-not (Get-Command python -ErrorAction SilentlyContinue) -and -not (Get-Command python3 -ErrorAction SilentlyContinue)) {
  throw 'Python is required for Chromium bootstrap. Install Python and rerun.'
}

Assert-Command gclient
Assert-Command fetch
Assert-Command autoninja
Assert-Command gn

if (-not (Test-Path $root)) {
  New-Item -ItemType Directory -Path $root | Out-Null
}

Set-Location $root

if (-not (Test-Path (Join-Path $root '.gclient'))) {
  if ($SkipFetch) {
    Write-Host '.gclient missing and -SkipFetch set; create chromium tree manually before proceeding.'
    exit 0
  }

  Write-Host 'Fetching Chromium solution for the first time...'
  & fetch --nohooks chromium
  if ($LASTEXITCODE -ne 0) { throw 'fetch --nohooks chromium failed.' }
} else {
  Write-Host '.gclient already present; skipping fetch step.'
}

if (-not $NoSync) {
  Write-Host 'Running gclient sync ...'
  Set-Location $root
  & gclient sync --nohooks --no-history
  if ($LASTEXITCODE -ne 0) { throw 'gclient sync failed.' }
}

# Seed Owl integration directory skeleton if missing.
$owlRoot = Join-Path $root 'src/owl'
if (-not (Test-Path $owlRoot)) {
  Write-Host 'Creating //owl scaffold under Chromium tree.'
  New-Item -ItemType Directory -Path (Join-Path $owlRoot 'public/mojom') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $owlRoot 'host') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $owlRoot 'client') -Force | Out-Null
}

Copy-Item -Path (Join-Path $PSScriptRoot 'templates/owl/public/mojom/owl_host.mojom') -Destination (Join-Path $owlRoot 'public/mojom/owl_host.mojom') -Force
Copy-Item -Path (Join-Path $PSScriptRoot 'templates/owl/public/mojom/agent_gate.mojom') -Destination (Join-Path $owlRoot 'public/mojom/agent_gate.mojom') -Force
Copy-Item -Path (Join-Path $PSScriptRoot 'templates/owl/public/mojom/BUILD.gn') -Destination (Join-Path $owlRoot 'public/mojom/BUILD.gn') -Force

Write-Host "Chromium OWL workspace ready at: $root"
Write-Host 'Next steps:'
Write-Host '1) Add owl/BUILD.gn and chrome target wiring in the Chromium tree.'
Write-Host '2) Wire owl.mojom target outputs to host/client targets.'
Write-Host '3) Run scripts\build_owl.ps1'
