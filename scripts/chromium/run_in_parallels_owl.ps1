param(
  [string]$ChromiumRoot = "Y:\Desktop\CodexWorkspace\chromium",
  [string]$WorkspaceRoot = "Y:\Desktop\CodexWorkspace",
  [ValidateSet('x64','arm64')][string]$Arch = 'x64',
  [ValidateSet('Debug','Release')][string]$Config = 'Release',
  [ValidateSet('true', 'false')][string]$RunSync = 'true',
  [switch]$NoFetch
)

$ErrorActionPreference = 'Stop'

$chromiumScripts = $PSScriptRoot
$parentScripts = Split-Path -Parent $chromiumScripts

function Find-Script([string]$Name, [string[]]$Candidates) {
  foreach ($candidate in $Candidates) {
    $candidatePath = Join-Path $candidate $Name
    if (Test-Path $candidatePath) {
      return $candidatePath
    }
  }
  return $null
}

$setup = Find-Script -Name 'setup_owl_workspace.ps1' -Candidates @($chromiumScripts, $parentScripts)
$build = Find-Script -Name 'build_owl.ps1' -Candidates @($chromiumScripts, $parentScripts)
$run   = Find-Script -Name 'run_owl.ps1' -Candidates @($chromiumScripts, $parentScripts)

if (-not (Test-Path $setup)) { throw "Missing script: $setup" }
if (-not (Test-Path $build)) { throw "Missing script: $build" }
if (-not (Test-Path $run)) { throw "Missing script: $run" }

& powershell -NoProfile -ExecutionPolicy Bypass -File $setup `
    -WorkspaceRoot $WorkspaceRoot `
    -ChromiumDir (Split-Path $ChromiumRoot -Leaf) `
    -SkipFetch:$NoFetch `
    -NoSync:($RunSync -eq 'false')

& powershell -NoProfile -ExecutionPolicy Bypass -File $build `
    -ChromiumRoot $ChromiumRoot `
    -Archs @($Arch) `
    -Config $Config

& powershell -NoProfile -ExecutionPolicy Bypass -File $run -ChromiumRoot $ChromiumRoot -Arch $Arch
