param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
  [switch]$LaunchChrome,
  [string]$ChromeBinary = '',
  [int]$RemoteDebuggingPort = 9222
)

$ErrorActionPreference = 'Stop'

function Resolve-OwlExe([string]$ChromiumRootPath, [string]$ArchName, [string]$ExeName) {
  $candidates = @(
    (Join-Path $ChromiumRootPath (Join-Path ("src/out/owl_$ArchName") $ExeName)),
    (Join-Path $ChromiumRootPath (Join-Path ("out/owl_$ArchName") $ExeName))
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return (Resolve-Path $candidate).Path
    }
  }

  throw "$ExeName not found for arch $ArchName. Checked: $($candidates -join ', ')"
}

$owlHost = Resolve-OwlExe -ChromiumRootPath $ChromiumRoot -ArchName $Arch -ExeName 'owl_host.exe'
$owlClient = Resolve-OwlExe -ChromiumRootPath $ChromiumRoot -ArchName $Arch -ExeName 'owl_client.exe'

Write-Host "Running owl_host ping smoke test: $owlHost"
$hostOutput = & $owlHost --ping 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "owl_host exited with code $LASTEXITCODE"
}
if (-not ($hostOutput -match 'owl_host: pong')) {
  throw "owl_host ping output mismatch. Output: $hostOutput"
}

$profileRoot = Join-Path $env:TEMP ("owl-profile-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null

Write-Host "Running owl_client smoke test: $owlClient"
$clientOutput = & $owlClient --smoke-test "--profile_root=$profileRoot" 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "owl_client exited with code $LASTEXITCODE"
}
if (-not ($clientOutput -match 'owl_client smoke test OK')) {
  throw "owl_client smoke output mismatch. Output: $clientOutput"
}

Write-Host "Launching owl_client through run_owl.ps1"
$launchInfo = $null
if ($LaunchChrome) {
  if ($ChromeBinary) {
    $launchInfo = & (Join-Path $PSScriptRoot 'run_owl.ps1') `
      -ChromiumRoot $ChromiumRoot `
      -Arch $Arch `
      -ProfileRoot $profileRoot `
      -LaunchChrome `
      -ChromeBinary $ChromeBinary `
      -RemoteDebuggingPort $RemoteDebuggingPort `
      -PassThru
  } else {
    $launchInfo = & (Join-Path $PSScriptRoot 'run_owl.ps1') `
      -ChromiumRoot $ChromiumRoot `
      -Arch $Arch `
      -ProfileRoot $profileRoot `
      -LaunchChrome `
      -RemoteDebuggingPort $RemoteDebuggingPort `
      -PassThru
  }
} else {
  $launchInfo = & (Join-Path $PSScriptRoot 'run_owl.ps1') `
    -ChromiumRoot $ChromiumRoot `
    -Arch $Arch `
    -ProfileRoot $profileRoot `
    -PassThru
}
Start-Sleep -Seconds 2

$owlProc = Get-Process -Id $launchInfo.owl_client_pid -ErrorAction SilentlyContinue
if (-not $owlProc) {
  throw 'run_owl.ps1 did not start owl_client process'
}
Write-Host "Stopping launched owl_client process: $($owlProc.Id)"
$owlProc | Stop-Process -Force

if ($LaunchChrome) {
  if ($launchInfo.chrome_pid) {
    $chromeProc = Get-Process -Id $launchInfo.chrome_pid -ErrorAction SilentlyContinue
    if ($chromeProc) {
      Write-Host "Stopping launched chrome process: $($chromeProc.Id)"
      $chromeProc | Stop-Process -Force
    }
  }
}

Remove-Item -Path $profileRoot -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "OWL smoke tests passed for $Arch"
