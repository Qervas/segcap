<#
.SYNOPSIS
  Clear every segcap mode marker. The single authoritative list.

.DESCRIPTION
  Marker files next to the DLL are its only configuration channel, and they
  OUTLIVE the run that created them. This has corrupted three runs so far:

    1. An inZOI census left segcap.census + segcap.petriage behind; the next
       Stray run executed as a census, reported "IN-LEVEL confirmed", exited 0,
       and had marked nothing. Nothing in its output said "census".
    2. run_inzoi_play always created segcap.requirearm, which run_auto never
       cleared, so a later Stray run would hold its readback disarmed forever.
    3. segcap.idbuf, added later still, was left behind and made a Stray
       regression run enable the id-buffer probe -- which copies from arbitrary
       integer render targets and killed the game at t=68.6s. That run reported
       "VERDICT: FAILED -- no masks were written", and the cause was in neither
       the code under test nor the game.

  Each time the fix was "add the new marker to that script's list", and each
  time the NEXT marker was forgotten, because the list lived beside the script
  that happened to set it. A per-script list cannot stay complete.

  So there is now exactly one list, here, and every runner calls this before
  asserting the markers it actually wants. Adding a marker to the DLL means
  adding it here, once.

  This file must stay in sync with the markers read in dllmain.cpp.
#>
param([string]$Bin)

$ErrorActionPreference = "Stop"
if (-not $Bin) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Bin = Join-Path $root "build\bin"
}

# Every mode marker dllmain.cpp looks for. Deliberately NOT a wildcard over
# segcap.*: that directory also holds segcap.dll, segcap.log and the capture
# profile, and a wildcard here would be a footgun aimed at the build output.
$markers = @(
    "segcap.census",      # read-only census, suppresses all GPU work
    "segcap.petriage",    # opt in to ProcessEvent discovery during census
    "segcap.mark",        # WRITE bRenderCustomDepth -- mutates the game
    "segcap.abtest",      # A/B capture comparison
    "segcap.noreadback",  # mark, but issue no GPU work at all
    "segcap.d3ddebug",    # D3D12 validation layer
    "segcap.requirearm",  # hold the readback until segcap.arm appears
    "segcap.arm",         # the arm trigger itself
    "segcap.idbuf",       # id-buffer probe (copies from integer targets)
    "segcap.groundtruth", # unmark one slot mid-run and check its pixels vanish
    "segcap.radius",      # only mark objects within N units of the character
    "segcap.plateau",     # object count that means "engine up", per title
    "segcap.nocolour",    # skip backbuffer colour capture entirely
    "segcap.injectdry",   # observe render passes / barrier shapes, record nothing
    "segcap.inject",      # record copies into the game's own command lists
    "segcap.introspect"   # reflection dump
)

$cleared = @()
foreach ($m in $markers) {
    $p = Join-Path $Bin $m
    if (Test-Path $p) {
        Remove-Item $p -Force
        $cleared += $m
    }
}

if ($cleared.Count -gt 0) {
    Write-Host "[markers] cleared stale: $($cleared -join ', ')"
} else {
    Write-Host "[markers] none stale"
}
