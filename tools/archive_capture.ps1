<#
.SYNOPSIS
  Move the masks currently in build\bin into captures\<title>_<timestamp>\
  instead of deleting them.

.DESCRIPTION
  Both run scripts used to clear segcap_mask_* on startup so a run's output
  could not be confused with the previous run's. That is the right instinct and
  the wrong mechanism: a Stray regression run destroyed a verified inZOI
  gameplay session that had taken three attempts and about twenty minutes of
  game time to produce, and there was no copy anywhere.

  Deleting is only safe when the data is cheap to reproduce. A capture session
  needs the game launched, driven through a menu, a 35 GB save loaded and the
  world walked around for three minutes -- that is not cheap, and it is not
  reproducible on demand once the machine state has moved on.

  So: archive, never delete. The archive is keyed by title so two games cannot
  collide, and by timestamp so two runs of the same game cannot either.
#>
param(
    # Which game produced the masks about to be archived. Used only to name the
    # directory, so a wrong guess is cosmetic rather than destructive.
    [Parameter(Mandatory = $true)][string]$Title,
    [string]$Bin
)

$ErrorActionPreference = "Stop"
if (-not $Bin) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Bin = Join-Path $root "build\bin"
}
$root = Split-Path -Parent $Bin | Split-Path -Parent

$existing = @(Get-ChildItem (Join-Path $Bin "segcap_mask_*") -ErrorAction SilentlyContinue)
if ($existing.Count -eq 0) {
    Write-Host "[archive] nothing to archive"
    return
}

# Stamp from the newest file, not from "now". The masks may be days old; naming
# the directory after the moment we happened to archive them would misdate the
# session and make two archives sort in the wrong order.
$stamp = ($existing | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime.ToString("yyyyMMdd_HHmmss")
$dest  = Join-Path $root "captures\${Title}_$stamp"
if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }

Move-Item -Path (Join-Path $Bin "segcap_mask_*") -Destination $dest -Force
$pgm = @(Get-ChildItem (Join-Path $dest "*.pgm") -ErrorAction SilentlyContinue).Count
$json = @(Get-ChildItem (Join-Path $dest "*.json") -ErrorAction SilentlyContinue).Count

# The log is the other half of the evidence; a mask set with no log cannot be
# explained after the fact.
$log = Join-Path $Bin "segcap.log"
if (Test-Path $log) { Copy-Item $log (Join-Path $dest "segcap.log") -Force }

Write-Host "[archive] $pgm masks + $json sidecars -> $dest"
