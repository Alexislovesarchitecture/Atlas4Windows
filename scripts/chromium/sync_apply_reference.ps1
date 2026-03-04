param(
  [string]$ChromiumRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'chromium'),
  [string]$ReferenceFile = (Join-Path $PSScriptRoot 'reference/chromium-reference.json'),
  [string]$PatchManifest = (Join-Path $PSScriptRoot 'reference/patch-manifest.json'),
  [switch]$NoSync,
  [switch]$SkipCheckout,
  [switch]$AllowDirty,
  [switch]$ForceAtlWorkaround
)

$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
  $PSNativeCommandUseErrorActionPreference = $false
}

. (Join-Path $PSScriptRoot 'common.ps1')

Assert-Command git
Assert-Command gclient

if (-not (Test-Path $ReferenceFile)) {
  throw "Reference file not found: $ReferenceFile"
}
if (-not (Test-Path $PatchManifest)) {
  throw "Patch manifest not found: $PatchManifest"
}

$reference = Get-Content $ReferenceFile -Raw | ConvertFrom-Json
$manifest = Get-Content $PatchManifest -Raw | ConvertFrom-Json

$srcRoot = Resolve-ChromiumSrcRoot -ChromiumRoot $ChromiumRoot
Set-Location $srcRoot

function Assert-CleanWorktree {
  param([string]$Message)

  $status = git status --porcelain
  if ($status) {
    throw "$Message`n$status"
  }
}

function Apply-GitPatchFile {
  param([Parameter(Mandatory = $true)][string]$PatchPath)

  function Invoke-GitQuiet {
    param([string[]]$GitArgs)

    $prevErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git @GitArgs 2>$null | Out-Null
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevErrorActionPreference
    return $exitCode
  }

  if (-not (Test-Path $PatchPath)) {
    throw "Patch file missing: $PatchPath"
  }

  if ((Invoke-GitQuiet -GitArgs @('apply', '--check', $PatchPath)) -eq 0) {
    Write-Host "Applying patch: $PatchPath"
    & git apply --3way --whitespace=nowarn $PatchPath
    if ($LASTEXITCODE -ne 0) {
      throw "Failed to apply patch: $PatchPath"
    }
    return
  }

  if ((Invoke-GitQuiet -GitArgs @('apply', '--reverse', '--check', $PatchPath)) -eq 0) {
    Write-Host "Patch already applied, skipping: $PatchPath"
    return
  }

  if ($AllowDirty) {
    $numstat = & git apply --numstat $PatchPath 2>$null
    $paths = @()
    foreach ($line in $numstat) {
      $parts = ($line -split "`t")
      if ($parts.Count -ge 3) {
        $paths += $parts[2]
      }
    }

    if ($paths.Count -gt 0) {
      $allPresent = $true
      foreach ($relPath in $paths) {
        if (-not (Test-Path (Join-Path $srcRoot $relPath))) {
          $allPresent = $false
          break
        }
      }
      if ($allPresent) {
        Write-Host "Patch targets already present in dirty worktree, skipping: $PatchPath"
        return
      }
    }
  }

  throw "Patch does not apply cleanly and is not already applied: $PatchPath"
}

if (-not $AllowDirty) {
  Assert-CleanWorktree -Message 'Chromium worktree must be clean before syncing/applying reference patches.'
}

$remoteUrl = git remote get-url origin
if ($remoteUrl -ne $reference.remote) {
  Write-Host "Updating origin remote to $($reference.remote)"
  & git remote set-url origin $reference.remote
  if ($LASTEXITCODE -ne 0) { throw 'Failed to update origin remote URL.' }
}

Write-Host "Fetching reference ref: $($reference.ref)"
& git fetch origin $reference.ref
if ($LASTEXITCODE -ne 0) { throw "git fetch failed for $($reference.ref)" }

if (-not $SkipCheckout) {
  Write-Host "Checking out pinned commit: $($reference.commit)"
  & git checkout --detach $reference.commit
  if ($LASTEXITCODE -ne 0) { throw "git checkout failed for $($reference.commit)" }
}

if (-not $NoSync) {
  $checkoutRoot = Split-Path -Parent $srcRoot
  Set-Location $checkoutRoot
  Write-Host "Running gclient sync in $checkoutRoot"
  & gclient sync --nohooks --no-history
  if ($LASTEXITCODE -ne 0) { throw 'gclient sync failed.' }
  Set-Location $srcRoot
}

if (-not $AllowDirty) {
  Assert-CleanWorktree -Message 'Chromium worktree is not clean after checkout/sync; refusing to apply patch queue.'
}

$atlMissing = -not (Test-AtlHeaderPresent)
foreach ($patch in $manifest.patches) {
  $patchPath = Join-Path $PSScriptRoot $patch.path
  $required = $false
  if ($patch.PSObject.Properties.Name -contains 'required') {
    $required = [bool]$patch.required
  }
  $applyWhen = ''
  if ($patch.PSObject.Properties.Name -contains 'apply_when') {
    $applyWhen = [string]$patch.apply_when
  }
  $patchId = [string]$patch.id

  $apply = $required
  if (-not $apply -and $applyWhen -eq 'missing_atl_headers' -and ($atlMissing -or $ForceAtlWorkaround)) {
    $apply = $true
  }

  if ($apply) {
    Apply-GitPatchFile -PatchPath $patchPath
  } else {
    Write-Host "Skipping optional patch: $patchId"
  }
}

Write-Host 'Reference sync + patch queue complete.'
Write-Host "Pinned commit: $($reference.commit)"
