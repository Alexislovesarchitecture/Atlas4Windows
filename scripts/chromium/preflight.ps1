param(
  [switch]$RequireAtl,
  [switch]$RequireArm64Runtime
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

$results = [ordered]@{
  atl_header = Get-AtlHeaderPath
  arm64_msvc_runtime = Get-Arm64MsvcRuntimePath
}

if ($results.atl_header) {
  Write-Host "ATL header found: $($results.atl_header)"
} else {
  Write-Host 'ATL header missing (expected atldef.h under VS atlmfc\include).'
}

if ($results.arm64_msvc_runtime) {
  Write-Host "ARM64 MSVC runtime found: $($results.arm64_msvc_runtime)"
} else {
  Write-Host 'ARM64 MSVC runtime missing (expected msvcp140.dll under VS Redist\MSVC\*\arm64\...).'
}

if ($RequireAtl -and -not $results.atl_header) {
  throw 'Chromium preflight failed: ATL/MFC headers are missing. Install Visual Studio Build Tools ATL/MFC components.'
}

if ($RequireArm64Runtime -and -not $results.arm64_msvc_runtime) {
  throw 'Chromium preflight failed: ARM64 MSVC runtime files are missing. Install VS C++ ARM64 runtime/redist components.'
}

