param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [switch]$SkipSync,
  [switch]$SkipCheckout,
  [switch]$AllowDirty,
  [switch]$SkipChromeBuild,
  [bool]$SkipArm64IfMissingPrereqs = $true
)

$ErrorActionPreference = 'Stop'

$includeChrome = -not $SkipChromeBuild
$archs = @('x64', 'arm64')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

$resolvedChromiumRoot = $ChromiumRoot
if (Test-Path $ChromiumRoot) {
  $resolvedChromiumRoot = (Resolve-Path $ChromiumRoot).Path
}

Write-Host 'Step 1/4: Syncing Chromium to pinned reference and applying patch queue'
& (Join-Path $PSScriptRoot 'sync_apply_reference.ps1') `
  -ChromiumRoot $resolvedChromiumRoot `
  -NoSync:$SkipSync `
  -SkipCheckout:$SkipCheckout `
  -AllowDirty:$AllowDirty

Set-Location $repoRoot
Write-Host 'Step 2/4: Building OWL and Chromium targets'
& (Join-Path $PSScriptRoot 'build.ps1') `
  -ChromiumRoot $resolvedChromiumRoot `
  -Config $Config `
  -Archs $archs `
  -IncludeChrome:$includeChrome `
  -SkipArm64IfMissingPrereqs:$SkipArm64IfMissingPrereqs

Set-Location $repoRoot
Write-Host 'Step 3/4: Running OWL smoke tests'
& (Join-Path $PSScriptRoot 'test_owl.ps1') -ChromiumRoot $resolvedChromiumRoot -Arch x64 -LaunchChrome:$includeChrome

if ($includeChrome) {
  Write-Host 'Step 4/4: Running Playwright smoke test against built chrome'
  & (Join-Path $PSScriptRoot 'test_chrome_playwright.ps1') -ChromiumRoot $resolvedChromiumRoot
} else {
  Write-Host 'Step 4/4: Skipped Playwright chrome smoke (chrome build disabled).'
}

Write-Host 'Bring-up flow complete.'
