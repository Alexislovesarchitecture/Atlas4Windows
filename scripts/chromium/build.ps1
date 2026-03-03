param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build_owl.ps1') `
  -ChromiumRoot $ChromiumRoot `
  -Archs @('x64', 'arm64') `
  -Config $Config
