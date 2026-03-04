param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [string[]]$Archs = @('x64', 'arm64'),
  [switch]$IncludeChrome,
  [switch]$SkipArm64IfMissingPrereqs
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

Assert-Command gn
Assert-Command autoninja

$srcRoot = Resolve-ChromiumSrcRoot -ChromiumRoot $ChromiumRoot
$isDebug = if ($Config -eq 'Debug') { 'true' } else { 'false' }

if (-not $env:DEPOT_TOOLS_WIN_TOOLCHAIN) {
  $env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
}

$effectiveArchs = @($Archs)
if ($effectiveArchs -contains 'arm64' -and -not (Test-Arm64MsvcRuntimePresent)) {
  if ($SkipArm64IfMissingPrereqs) {
    Write-Host 'ARM64 prerequisites missing; skipping arm64 OWL build because -SkipArm64IfMissingPrereqs was set.'
    $effectiveArchs = @($effectiveArchs | Where-Object { $_ -ne 'arm64' })
  } else {
    throw 'ARM64 build prerequisites missing. Re-run with -SkipArm64IfMissingPrereqs to continue with x64 only.'
  }
}

if (-not $effectiveArchs) {
  throw 'No target architectures selected after preflight checks.'
}

if ($IncludeChrome -and -not (Test-AtlHeaderPresent)) {
  if (Test-AtlWorkaroundActive -ChromiumRoot $ChromiumRoot) {
    Write-Host 'ATL headers are missing, but ATL workaround patch appears active; continuing chrome build.'
  } else {
    throw 'Chromium chrome build requires ATL/MFC headers (atldef.h). Install VS ATL/MFC components or apply an ATL workaround patch queue before building chrome.'
  }
}

& (Join-Path $PSScriptRoot 'build_owl.ps1') `
  -ChromiumRoot $ChromiumRoot `
  -Archs $effectiveArchs `
  -Config $Config

if ($IncludeChrome) {
  $outDir = Join-Path $srcRoot 'out/chrome_x64'
  New-Item -ItemType Directory -Path $outDir -Force | Out-Null

  $argsFile = Join-Path $outDir 'args.gn'
  @(
    'target_os="win"'
    'target_cpu="x64"'
    "is_debug=$isDebug"
    'is_component_build=false'
    'symbol_level=1'
    'dawn_use_built_dxc=false'
  ) | Set-Content -Path $argsFile -Encoding ascii

  Set-Location $srcRoot
  Write-Host "Configuring chrome out dir: $outDir"
  & gn gen $outDir
  if ($LASTEXITCODE -ne 0) { throw 'gn gen failed for chrome_x64' }

  Write-Host 'Building chrome target (x64)'
  & autoninja -C $outDir chrome
  if ($LASTEXITCODE -ne 0) { throw 'autoninja failed for chrome_x64 chrome target' }

  Write-Host "Chrome artifact: $srcRoot\out\chrome_x64\chrome.exe"
}
