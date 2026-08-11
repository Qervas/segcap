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

# BOTH halves of a capture, not just the masks.
#
# This script archived segcap_mask_* only, and the colour frames it left behind
# were not harmless leftovers -- they were the other half of the evidence. A
# Stray run's 114 segcap_frame_*.ppm sat in build\bin through an entire inZOI
# session, so the next overlay would have paired inZOI masks against Stray's
# video and produced a demo that was wrong in the one way the brief cares about.
# Misalignment you can see is a bug; misalignment between two different GAMES
# looks like a capture that simply failed, and costs an afternoon to explain.
$patterns = @("segcap_mask_*", "segcap_frame_*")

$existing = @(foreach ($p in $patterns) {
    Get-ChildItem (Join-Path $Bin $p) -ErrorAction SilentlyContinue
})

# A run that produced no images still produced a log, and on a DIAGNOSIS run the
# log IS the result. This used to return here, before the log was copied, so the
# next run's Remove-Item took it: a debug-layer session that hung the game wrote
# 410 MB of validation evidence and would have left nothing behind, because it
# happened to write zero masks. Keep the log on its own.
$log = Join-Path $Bin "segcap.log"
if ($existing.Count -eq 0) {
    if (Test-Path $log) {
        $stamp = (Get-Item $log).LastWriteTime.ToString("yyyyMMdd_HHmmss")
        $dest  = Join-Path $root "captures\${Title}_${stamp}_logonly"
        if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }
        Move-Item $log (Join-Path $dest "segcap.log") -Force
        Write-Host "[archive] no images, but kept the log -> $dest"
    } else {
        Write-Host "[archive] nothing to archive"
    }
    return
}

# Stamp from the newest file, not from "now". The masks may be days old; naming
# the directory after the moment we happened to archive them would misdate the
# session and make two archives sort in the wrong order.
$stamp = ($existing | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime.ToString("yyyyMMdd_HHmmss")

# Name from the LOG, not from -Title. Every caller passes the title of the run it
# is ABOUT TO START, while the files being moved belong to the run that just
# ENDED -- so a Stray session archived from an inZOI launcher was filed under
# "inzoi", and hours were later spent reasoning about an "inZOI" capture that was
# Stray's. The log's first line records the actual host executable; that is
# evidence, and -Title is a guess. -Title remains the fallback for the case the
# bug cannot survive anyway: no log at all.
$name = $Title
if (Test-Path $log) {
    # Head of the file only. `host:` is written at t=0.000, and these logs reach
    # hundreds of megabytes -- Select-String over the whole file would scan 410 MB
    # to read line 1.
    $hostLine = Get-Content $log -TotalCount 40 |
                Where-Object { $_ -match 'host:\s*(.+)$' } | Select-Object -First 1
    if ($hostLine -and $hostLine -match 'host:\s*(.+)$') {
        $exe = [IO.Path]::GetFileNameWithoutExtension($matches[1].Trim())
        # inZOI-Win64-Shipping -> inzoi; Stray-Win64-Shipping -> stray
        $name = ($exe -replace '-Win64-Shipping$','' -replace '[^A-Za-z0-9_]','').ToLower()
        if (-not $name) { $name = $Title }
        if ($name -ne $Title) {
            Write-Host "[archive] host says '$name', caller said '$Title' -- filing under the log"
        }
    }
}
$dest  = Join-Path $root "captures\${name}_$stamp"
if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }

foreach ($p in $patterns) {
    Move-Item -Path (Join-Path $Bin $p) -Destination $dest -Force -ErrorAction SilentlyContinue
}
$pgm  = @(Get-ChildItem (Join-Path $dest "*.pgm") -ErrorAction SilentlyContinue).Count
$ppm  = @(Get-ChildItem (Join-Path $dest "*.ppm") -ErrorAction SilentlyContinue).Count
$json = @(Get-ChildItem (Join-Path $dest "*.json") -ErrorAction SilentlyContinue).Count

# The log is the other half of the evidence; a mask set with no log cannot be
# explained after the fact. ($log was resolved above, before the empty-set exit.)
if (Test-Path $log) { Copy-Item $log (Join-Path $dest "segcap.log") -Force }

Write-Host "[archive] $pgm masks + $ppm frames + $json sidecars -> $dest"

# Say so when a run archived masks without frames, or frames without masks. Both
# are legitimate (a census run writes no masks; -NoReadback writes neither), but
# an UNNOTICED imbalance is how stale frames survived into the next session.
if ($pgm -gt 0 -and $ppm -eq 0) {
    Write-Host "[archive] note: masks but NO colour frames -- overlays will have nothing to blend onto"
} elseif ($ppm -gt 0 -and $pgm -eq 0) {
    Write-Host "[archive] note: colour frames but NO masks"
}
