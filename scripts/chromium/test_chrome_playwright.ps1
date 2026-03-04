param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [string]$ChromeBinary = '',
  [ValidateSet('chrome', 'msedge', 'firefox', 'webkit')][string]$Browser = 'msedge',
  [string]$Url = 'https://example.com',
  [string]$ExpectedTitleContains = 'Example Domain',
  [string]$Session = 'atlas-chrome-smoke',
  [string]$OutputDir = 'output/playwright'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

Assert-Command npx

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$outputPath = (Resolve-Path $OutputDir).Path

function Invoke-PwCli {
  param([string[]]$CliArgs)

  $output = & npx --yes --package @playwright/cli playwright-cli @CliArgs 2>&1
  $text = $output -join [Environment]::NewLine
  if ($text) {
    Write-Host $text
  }

  if ($LASTEXITCODE -ne 0) {
    throw "playwright-cli failed with exit code $LASTEXITCODE for args: $($CliArgs -join ' ')"
  }

  return $text
}

if (-not $ChromeBinary) {
  try {
    $ChromeBinary = Resolve-ChromeBinary -ChromiumRoot $ChromiumRoot
  } catch {
    # Optional binary launch check; Playwright smoke still runs with browser channel.
    $ChromeBinary = ''
  }
}

if ($ChromeBinary) {
  Write-Host "Launching browser binary sanity check: $ChromeBinary"
  $binaryProfile = Join-Path $env:TEMP ('chrome-smoke-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $binaryProfile -Force | Out-Null
  $binaryArgs = @(
    '--no-first-run',
    '--no-default-browser-check',
    "--user-data-dir=$binaryProfile",
    $Url
  )
  $browserProc = Start-Process -FilePath $ChromeBinary -ArgumentList $binaryArgs -PassThru
  Start-Sleep -Seconds 2
  if (-not (Get-Process -Id $browserProc.Id -ErrorAction SilentlyContinue)) {
    throw "Browser binary failed to launch: $ChromeBinary"
  }
  Stop-Process -Id $browserProc.Id -Force
  Remove-Item -Path $binaryProfile -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Running Playwright CLI smoke using browser channel: $Browser"

# Best-effort cleanup in case a previous session exists.
try {
  Invoke-PwCli -CliArgs @("-s=$Session", 'close') | Out-Null
} catch {
  # Ignore close errors when the session does not exist.
}

$openOutput = Invoke-PwCli -CliArgs @("-s=$Session", 'open', $Url, '--browser', $Browser)
$openOutput | Set-Content -Path (Join-Path $outputPath 'chrome_open.txt') -Encoding utf8

$titleOutput = Invoke-PwCli -CliArgs @("-s=$Session", 'eval', '() => document.title')
$titleOutput | Set-Content -Path (Join-Path $outputPath 'chrome_title.txt') -Encoding utf8

if ($ExpectedTitleContains -and ($titleOutput -notmatch [regex]::Escape($ExpectedTitleContains))) {
  throw "Title assertion failed. Expected substring '$ExpectedTitleContains'."
}

$snapshotFile = Join-Path $outputPath 'chrome_snapshot.yml'
$screenshotFile = Join-Path $outputPath 'chrome_smoke.png'
Invoke-PwCli -CliArgs @("-s=$Session", 'snapshot', '--filename', $snapshotFile) | Out-Null
Invoke-PwCli -CliArgs @("-s=$Session", 'screenshot', '--filename', $screenshotFile) | Out-Null

Invoke-PwCli -CliArgs @("-s=$Session", 'close') | Out-Null

Write-Host "Playwright smoke test passed. Artifacts:"
Write-Host " - $screenshotFile"
Write-Host " - $snapshotFile"
