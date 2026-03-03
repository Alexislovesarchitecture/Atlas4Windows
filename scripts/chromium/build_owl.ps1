param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [string[]]$Archs = @('x64', 'arm64'),
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [ValidateSet('true', 'false')][string]$IsDebug,
  [switch]$OnlyArgs
)

$ErrorActionPreference = 'Stop'

function Assert-Command {
  param([string]$Name)
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command missing: $Name"
  }
}

if (-not $IsDebug) {
  $IsDebug = if ($Config -eq 'Debug') { 'true' } else { 'false' }
}

Assert-Command gn
Assert-Command autoninja

if (-not (Test-Path $ChromiumRoot)) {
  throw "Chromium root not found: $ChromiumRoot"
}

Set-Location $ChromiumRoot

$targets = @('owl_client', 'owl_host')
$argValues = "target_os=win is_debug=$IsDebug is_component_build=false symbol_level=1"

foreach ($arch in $Archs) {
  if ($arch -ne 'x64' -and $arch -ne 'arm64') {
    throw "Unsupported architecture '$arch'. Use x64 and/or arm64."
  }

  $outDir = Join-Path (Get-Location) ("out/owl_$arch")
  Write-Host "Configuring out dir: $outDir"
  & gn gen $outDir --args "$($argValues) target_cpu=$arch"
  if ($LASTEXITCODE -ne 0) { throw "gn gen failed for $arch" }

  if ($OnlyArgs) { continue }

  if ($arch -eq 'arm64') {
    Write-Host 'Note: ARM64 build requires x64 host tooling (not ARM-host) in this pass.'
  }

  Write-Host "Building owl_client/owl_host for $arch"
  & autoninja -C $outDir @targets
  if ($LASTEXITCODE -ne 0) { throw "autoninja failed for $arch" }
}

Write-Host "Build completed for: $($Archs -join ', ')"
Write-Host 'Artifacts (expected):'
foreach ($arch in $Archs) {
  Write-Host " - $ChromiumRoot\out\owl_$arch\owl_client.exe"
  Write-Host " - $ChromiumRoot\out\owl_$arch\owl_host.exe"
}
