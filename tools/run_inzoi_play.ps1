<#
.SYNOPSIS
  Drive inZOI from the desktop into loaded gameplay, then hold it there while
  segcap marks and captures. THIS RUN MUTATES THE GAME.

.DESCRIPTION
  Everything before this was read-only. This is the first run that sets
  bRenderCustomDepth on inZOI's UObjects, so it is a separate script rather
  than a flag on the census runner -- the census script's whole contract is
  that it cannot write, and it should stay that way.

  The route into gameplay was established by probing, not by guessing, and two
  findings in it are not obvious:

    1. THE MAIN MENU DOES NOT TAKE GAMEPAD FOCUS. Pressing d-pad Down on the
       main menu highlights nothing. The pad drives the news carousel in the
       bottom-right corner instead (note its LB/RB hints). Continue, and the
       save-slot list behind it, need real mouse clicks. An earlier automated
       attempt pressed A three times into a menu that was never listening and
       reported success because the process was still alive.

    2. THE SIMULATION LOADS PAUSED. The save loads with the transport bar in
       its paused state, so the Zoi will not move no matter what the stick
       does -- which reads exactly like broken input. Clicking the play button
       is what starts time; the clock advancing is the confirmation.

  Coordinates are held as FRACTIONS of the window and resolved at runtime.
  They were measured on a 2560x1600 window and hardcoding those pixels would
  silently click empty space on any other size.

.PARAMETER Seconds
  How long to stay in gameplay, marking and capturing, after the world loads.
#>
param(
    [int]$Seconds = 240,
    [int]$MenuWait = 85,        # inZOI is 35 GB; the menu takes its time
    [int]$LoadWait = 90,        # save load + world streaming
    [int]$Captures = 60,
    [int]$Stride = 20,
    # Skip marking -- same route into gameplay, but read-only. Use this to
    # re-check the route without touching the game's objects.
    [switch]$NoMark,
    # Isolation run: mark normally, but issue no GPU work at all. Note that
    # -Captures 0 does NOT do this -- it only zeroes the dump budget, while the
    # copy still runs every frame.
    [switch]$NoReadback
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll      = Join-Path $root "build\bin\segcap.dll"
$injector = Join-Path $root "build\bin\injector.exe"
$vpad     = Join-Path $root "build\bin\vpad.exe"
$log      = Join-Path $root "build\bin\segcap.log"
$act      = Join-Path $root "tools\act.ps1"

$gameDir = "C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64"
$gameExe = Join-Path $gameDir "inZOI-Win64-Shipping.exe"
$appId   = 2456740

foreach ($f in @($dll, $injector, $vpad, $gameExe, $act)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}

# --- screensaver, before anything else ----------------------------------------
# An active screensaver switches the input desktop away from "Default", and from
# there no window can be focused and every synthetic input is discarded. This
# cost four runs on Stray. Print the desktop rather than theorising if focus
# ever fails.
if (-not ("SS2" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class SS2 {
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a,uint b,IntPtr c,uint d);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr OpenInputDesktop(uint f,bool i,uint a);
  [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool GetUserObjectInformationW(IntPtr h,int i,IntPtr b,int n,out int need);
  public static void Off(){ SystemParametersInfoW(0x0011,0,IntPtr.Zero,0x02); }
  public static string Desktop(){
    IntPtr d=OpenInputDesktop(0,false,0x0001);
    if(d==IntPtr.Zero) return "<denied: locked>";
    IntPtr b=Marshal.AllocHGlobal(512);
    try{ int n; if(!GetUserObjectInformationW(d,2,b,512,out n)) return "<unknown>";
         return Marshal.PtrToStringUni(b); }
    finally{ Marshal.FreeHGlobal(b); CloseDesktop(d); } }
}
"@
}
[SS2]::Off()
Write-Host "[play] input desktop: $([SS2]::Desktop())"

# --- markers ------------------------------------------------------------------
$bin = Join-Path $root "build\bin"
Remove-Item (Join-Path $bin "segcap.census")   -ErrorAction SilentlyContinue
Remove-Item (Join-Path $bin "segcap.petriage") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $bin "segcap.abtest")   -ErrorAction SilentlyContinue
if ($NoMark) {
    Remove-Item (Join-Path $bin "segcap.mark") -ErrorAction SilentlyContinue
    Write-Host "[play] mark marker ABSENT -- read-only run"
} else {
    Set-Content -Path (Join-Path $bin "segcap.mark") -Value "1" -NoNewline
    Write-Host "[play] mark marker SET -- THIS RUN WILL WRITE bRenderCustomDepth"
}
$nrMarker = Join-Path $bin "segcap.noreadback"
if ($NoReadback) {
    Set-Content -Path $nrMarker -Value "1" -NoNewline
    Write-Host "[play] NO-READBACK: marking runs, zero GPU work -- isolation run"
} else {
    Remove-Item $nrMarker -ErrorAction SilentlyContinue
}
Set-Content -Path (Join-Path $bin "segcap.captures") -Value "$Captures" -NoNewline
Set-Content -Path (Join-Path $bin "segcap.stride")   -Value "$Stride"   -NoNewline

Get-Process -Name "inZOI*","vpad" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[play] closing stale $($_.ProcessName) $($_.Id)"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue
Get-ChildItem (Join-Path $bin "segcap_mask_*") -ErrorAction SilentlyContinue | Remove-Item -Force

# --- launch suspended + inject ------------------------------------------------
$appidFile = Join-Path $gameDir "steam_appid.txt"
if (-not (Test-Path $appidFile)) {
    Set-Content -Path $appidFile -Value "$appId" -NoNewline
    Write-Host "[play] wrote steam_appid.txt (reversible; delete to undo)"
}
$injOut = Join-Path $env:TEMP "inzoi_play_inject.txt"
Write-Host "[play] launching suspended and injecting"
Start-Process -FilePath $injector -NoNewWindow -RedirectStandardOutput $injOut `
    -ArgumentList @("--launch", "`"$gameExe`"", "--args", "`"-dx12`"",
                    "--workdir", "`"$gameDir`"", "--dll", "`"$dll`"") | Out-Null

$game = $null
for ($i = 0; $i -lt 60; $i++) {
    $game = Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game) { break }
    Start-Sleep -Seconds 1
}
if (-not $game) { Get-Content $injOut -ErrorAction SilentlyContinue; throw "game never appeared" }
Write-Host "[play] game pid $($game.Id)"

Start-Process -FilePath $vpad -ArgumentList @("--serve", "build\bin\vpad_cmd.txt") -WindowStyle Hidden | Out-Null

Write-Host "[play] waiting ${MenuWait}s for the main menu"
Start-Sleep -Seconds $MenuWait

# --- the route, as fractions of the window ------------------------------------
# Measured on 2560x1600. act.ps1 is DPI-aware and takes physical pixels, so the
# fractions are resolved against the real window rect at click time.
function Click([double]$fx, [double]$fy, [double]$wait, [string]$what) {
    if (-not (Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue)) {
        throw "game exited before: $what"
    }
    Write-Host "[play] $what"
    & $act -ClickFx $fx -ClickFy $fy -Wait $wait | ForEach-Object { Write-Host "       $_" }
}

Click 0.0883 0.2169 4  "Continue"
Click 0.6926 0.3631 8  "first save slot (play)"

Write-Host "[play] waiting ${LoadWait}s for the world to stream in"
Start-Sleep -Seconds $LoadWait

# The sim loads PAUSED. Clicking play is what starts time; the clock in the
# bottom-left advancing is how you know it worked.
Click 0.0660 0.9606 5  "transport play (unpause)"

Write-Host "[play] in gameplay; holding for ${Seconds}s while segcap marks"
$deadline = (Get-Date).AddSeconds($Seconds)
$step = 0
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue)) {
        Write-Host "[play] GAME EXITED at step $step -- see the log tail below"
        break
    }
    # Walk, so the scene changes and the capture is not 60 copies of one frame.
    $ly = if ($step % 2 -eq 0) { 30000 } else { -30000 }
    & $act -Ly $ly -Ms 2500 -Wait 0.5 | Out-Null
    $step++
}

Write-Host ""
Write-Host "=============== RESULT ==============="
$masks = (Get-ChildItem (Join-Path $bin "segcap_mask_*.pgm")  -ErrorAction SilentlyContinue).Count
$cars  = (Get-ChildItem (Join-Path $bin "segcap_mask_*.json") -ErrorAction SilentlyContinue).Count
Write-Host "  masks / sidecars : $masks / $cars"
if (Test-Path $log) {
    foreach ($pat in @("ProcessEvent CONFIRMED","triage shortlisted","ELECTED",
                       "customdepth: marked","REJECTED as non-renderable","no viable")) {
        $hit = Select-String -Path $log -Pattern $pat -ErrorAction SilentlyContinue | Select-Object -Last 3
        foreach ($h in $hit) { Write-Host "  $($h.Line.Trim())" }
    }
}
Write-Host "======================================"
Write-Host "  log: $log"
