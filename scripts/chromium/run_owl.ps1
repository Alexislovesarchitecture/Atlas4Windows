param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('x64','arm64')][string]$Arch = 'x64',
  [string]$ProfileRoot = ''
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $ChromiumRoot (Join-Path ('out/owl_' + $Arch) ("owl_client.exe"))
if (-not (Test-Path $exe)) {
  throw "owl_client not found: $exe. Run build_owl.ps1 first."
}

$clientDir = Split-Path $exe
Set-Location $clientDir
Write-Host "Launching Owl Client from $exe"

$arguments = @()
if ($ProfileRoot) {
  $arguments += "--profile_root=$ProfileRoot"
}

Start-Process -FilePath $exe -ArgumentList $arguments
