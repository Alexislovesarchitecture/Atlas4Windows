param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('x64','arm64')][string]$Arch = 'x64',
  [string]$ProfileRoot = '',
  [switch]$LaunchChrome,
  [string]$ChromeBinary = '',
  [int]$RemoteDebuggingPort = 9222,
  [string]$ChromeUrl = 'https://example.com',
  [switch]$PassThru
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

$candidates = @(
  (Join-Path $ChromiumRoot (Join-Path ('src/out/owl_' + $Arch) 'owl_client.exe')),
  (Join-Path $ChromiumRoot (Join-Path ('out/owl_' + $Arch) 'owl_client.exe'))
)
if (Test-Path (Join-Path $ChromiumRoot '.gn')) {
  $candidates = ,(Join-Path $ChromiumRoot (Join-Path ('out/owl_' + $Arch) 'owl_client.exe')) + $candidates
}

$exe = $null
foreach ($candidate in $candidates) {
  if (Test-Path $candidate) {
    $exe = (Resolve-Path $candidate).Path
    break
  }
}

if (-not $exe) {
  throw "owl_client not found in expected paths: $($candidates -join ', '). Run build_owl.ps1 first."
}

$clientDir = Split-Path $exe
Set-Location $clientDir
Write-Host "Launching Owl Client from $exe"

$chromeProc = $null
$chromeExe = $null

$arguments = @()

if ($LaunchChrome) {
  $chromeExe = Resolve-ChromeBinary -ChromiumRoot $ChromiumRoot -ExplicitChromeBinary $ChromeBinary

  if (-not $ProfileRoot) {
    $ProfileRoot = Join-Path $env:TEMP ('owl-profile-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $ProfileRoot -Force | Out-Null
  }

  $chromeProfile = Join-Path $ProfileRoot 'chrome-profile'
  New-Item -ItemType Directory -Path $chromeProfile -Force | Out-Null

  $chromeArgs = @(
    '--no-first-run',
    '--no-default-browser-check',
    "--remote-debugging-port=$RemoteDebuggingPort",
    "--user-data-dir=$chromeProfile",
    $ChromeUrl
  )

  Write-Host "Launching Chromium browser from $chromeExe"
  $chromeProc = Start-Process -FilePath $chromeExe -ArgumentList $chromeArgs -PassThru

  $arguments += "--chrome_binary=$chromeExe"
  $arguments += "--remote_debugging_port=$RemoteDebuggingPort"
}

if ($ProfileRoot) {
  $arguments += "--profile_root=$ProfileRoot"
}

$owlProc = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru

if ($PassThru) {
  [pscustomobject]@{
    owl_client_pid = $owlProc.Id
    owl_client_path = $exe
    chrome_pid = if ($chromeProc) { $chromeProc.Id } else { $null }
    chrome_path = if ($chromeExe) { $chromeExe } else { $null }
    profile_root = if ($ProfileRoot) { $ProfileRoot } else { $null }
    remote_debugging_port = if ($LaunchChrome) { $RemoteDebuggingPort } else { $null }
  }
}
