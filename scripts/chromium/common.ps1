Set-StrictMode -Version Latest

function Assert-Command {
  param([Parameter(Mandatory = $true)][string]$Name)

  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command missing: $Name"
  }
}

function Resolve-ChromiumSrcRoot {
  param([Parameter(Mandatory = $true)][string]$ChromiumRoot)

  if (-not (Test-Path $ChromiumRoot)) {
    throw "Chromium root not found: $ChromiumRoot"
  }

  if (Test-Path (Join-Path $ChromiumRoot '.gn')) {
    return (Resolve-Path $ChromiumRoot).Path
  }

  $srcCandidate = Join-Path $ChromiumRoot 'src'
  if (Test-Path (Join-Path $srcCandidate '.gn')) {
    return (Resolve-Path $srcCandidate).Path
  }

  throw "Could not find Chromium source root (.gn) under: $ChromiumRoot"
}

function Get-MsvcRoots {
  $roots = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC')
  )

  $existing = @()
  foreach ($root in $roots) {
    if (Test-Path $root) {
      $existing += (Resolve-Path $root).Path
    }
  }
  return $existing
}

function Get-MsvcRedistRoots {
  $roots = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2019\BuildTools\VC\Redist\MSVC'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC')
  )

  $existing = @()
  foreach ($root in $roots) {
    if (Test-Path $root) {
      $existing += (Resolve-Path $root).Path
    }
  }
  return $existing
}

function Get-AtlHeaderPath {
  foreach ($msvcRoot in Get-MsvcRoots) {
    $path = Get-ChildItem -Path $msvcRoot -Filter 'atldef.h' -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\atlmfc\\include\\atldef\.h$' } |
      Select-Object -First 1 -ExpandProperty FullName
    if ($path) {
      return $path
    }
  }
  return $null
}

function Test-AtlHeaderPresent {
  return [bool](Get-AtlHeaderPath)
}

function Get-Arm64MsvcRuntimePath {
  foreach ($redistRoot in Get-MsvcRedistRoots) {
    $path = Get-ChildItem -Path $redistRoot -Filter 'msvcp140.dll' -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\arm64\\' } |
      Select-Object -First 1 -ExpandProperty FullName
    if ($path) {
      return $path
    }
  }
  return $null
}

function Test-Arm64MsvcRuntimePresent {
  return [bool](Get-Arm64MsvcRuntimePath)
}

function Resolve-ChromeBinary {
  param(
    [Parameter(Mandatory = $true)][string]$ChromiumRoot,
    [string]$OutDirName = 'chrome_x64',
    [string]$ExplicitChromeBinary = ''
  )

  if ($ExplicitChromeBinary) {
    if (-not (Test-Path $ExplicitChromeBinary)) {
      throw "Chrome binary not found: $ExplicitChromeBinary"
    }
    return (Resolve-Path $ExplicitChromeBinary).Path
  }

  $srcRoot = Resolve-ChromiumSrcRoot -ChromiumRoot $ChromiumRoot
  $candidate = Join-Path $srcRoot (Join-Path "out/$OutDirName" 'chrome.exe')
  if (-not (Test-Path $candidate)) {
    throw "Chrome binary not found: $candidate"
  }

  return (Resolve-Path $candidate).Path
}

function Test-AtlWorkaroundActive {
  param([Parameter(Mandatory = $true)][string]$ChromiumRoot)

  $srcRoot = Resolve-ChromiumSrcRoot -ChromiumRoot $ChromiumRoot
  $baseBuild = Join-Path $srcRoot 'base/BUILD.gn'
  $shellCc = Join-Path $srcRoot 'ui/base/win/shell.cc'

  if (-not (Test-Path $baseBuild) -or -not (Test-Path $shellCc)) {
    return $false
  }

  $baseText = Get-Content $baseBuild -Raw
  $shellText = Get-Content $shellCc -Raw

  $atlThrowRemoved = ($baseText -notmatch 'win/atl_throw\.cc') -and ($baseText -notmatch 'win/atl_throw\.h')
  $shellUsesScopedCoMem = ($shellText -match 'base/win/scoped_co_mem\.h') -and ($shellText -match 'path_id_list\.get\(\)')

  return ($atlThrowRemoved -and $shellUsesScopedCoMem)
}
