<#
.SYNOPSIS
  Drive a RUNNING capture session without restarting the game.

.DESCRIPTION
  Getting inZOI to a loaded save costs about six minutes of menu and world
  streaming. Every experiment that changed a capture decision used to pay that
  toll again, and a probe that picked wrong was terminal for the session --
  idRejected_ is permanent -- so the only retry was to destroy the world and
  rebuild it.

  segcap polls a handful of marker files next to the DLL every few frames, so
  these actions take effect in the running game:

    arm       start writing masks
    disarm    stop writing masks
    reprobe   throw away everything discovery learned and start it over
    status    what the DLL is doing right now, from the log
    watch     follow the interesting lines as they are written

  WHAT STILL NEEDS A RELAUNCH: a rebuilt segcap.dll. The DLL is mapped into the
  game and MinHook's detours point into its code, so it cannot be swapped out
  from under live hooks. Config iterates live; code does not.

.EXAMPLE
  tools\live.ps1 reprobe ; tools\live.ps1 watch
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("arm", "disarm", "reprobe", "status", "watch")]
    [string]$Action,
    [string]$Bin
)

$ErrorActionPreference = "Stop"
if (-not $Bin) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Bin = Join-Path $root "build\bin"
}
$log = Join-Path $Bin "segcap.log"

$game = Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game -and $Action -ne "status") {
    Write-Host "[live] no game running -- these actions only affect a live session"
    return
}

switch ($Action) {
    "arm" {
        Set-Content -Path (Join-Path $Bin "segcap.arm") -Value "1" -NoNewline
        Write-Host "[live] ARMED -- masks will be written from the next captured frame"
    }
    "disarm" {
        Remove-Item (Join-Path $Bin "segcap.arm") -ErrorAction SilentlyContinue
        Write-Host "[live] DISARMED (the DLL latches armed_ for the session; disarm takes"
        Write-Host "       effect on the next run, use reprobe to change what is captured now)"
    }
    "reprobe" {
        Set-Content -Path (Join-Path $Bin "segcap.reprobe") -Value "1" -NoNewline
        Write-Host "[live] RE-PROBE requested -- watch for 'RE-PROBE requested' in the log"
    }
    "status" {
        if (-not (Test-Path $log)) { Write-Host "[live] no log at $log"; return }
        $tail = Get-Content $log -Tail 4000
        function Last([string]$pat) {
            ($tail | Where-Object { $_ -match $pat } | Select-Object -Last 1)
        }
        Write-Host "[live] game        : $(if ($game) { "pid $($game.Id) responding=$($game.Responding)" } else { 'not running' })"
        Write-Host "[live] masks       : $(@(Get-ChildItem (Join-Path $Bin 'segcap_mask_*.pgm') -ErrorAction SilentlyContinue).Count) on disk"
        foreach ($p in @('CAPTURE ARMED', 'RE-PROBE requested', 'idbuf: probing candidate',
                         'holding off', 'FOUND OUR IDS', 'drew ZERO barriers',
                         'inject: ON', 'inject: OFF', 'inject: attempts',
                         'customdepth: marked')) {
            $l = Last $p
            if ($l) { Write-Host ("[live] " + $l.Trim().Substring(0, [Math]::Min(150, $l.Trim().Length))) }
        }
    }
    "watch" {
        Write-Host "[live] following $log (Ctrl-C to stop)"
        Get-Content $log -Tail 5 -Wait | Where-Object {
            $_ -match 'idbuf|inject: (ON|OFF)|CAPTURE ARMED|RE-PROBE|FOUND OUR IDS|mask source|ERROR|drew ZERO'
        }
    }
}
