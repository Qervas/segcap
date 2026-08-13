<#
.SYNOPSIS
  First contact with inZOI (UE5, D3D12) in CENSUS MODE. Read-only.

.DESCRIPTION
  Stage 1 of the generalization attempt. This run cannot mutate the game and
  cannot issue GPU work:

    segcap.mark    ABSENT  -> no CustomDepth writes, no UObject writes at all
    segcap.census  PRESENT -> no copy, no barriers, nothing on the game's queue

  Census mode exists precisely for this: observing an unfamiliar title before
  acting on it. A wrong shadowed resource state on a new engine would show up as
  a GPU hang rather than an error message, so the first run does not transition
  anything.

  What it is trying to answer, in order:

    1. Does injection work at all on a 287 MB UE5 binary?
    2. Does the D3D12 layer generalize -- queue sniff, swapchain, descriptor
       mapping? Nothing in it is UE-specific, so this should pass.
    3. Are there scene-resolution depth-stencil targets, and does the election
       find one? Watch the `CreateDSV #N:` lines: they are logged upstream of
       the election filter, so they distinguish "no depth target exists" from
       "my filter ate it" -- a distinction that cost a run on Stray.
    4. Does the UE4 structural search find UE5's GUObjectArray? This is the
       likely breaking point. UE5 changed FUObjectArray, the FName pool and the
       FProperty chain.

  A clean failure at 4 is a legitimate result and is worth writing up: it says
  exactly which structural assumption is UE4-specific.

.PARAMETER Seconds
  How long to observe after the process appears. inZOI is a 35 GB title; give it
  room to reach a level.

.PARAMETER Direct
  Launch the shipping exe directly (needs steam_appid.txt beside it) instead of
  watching for a Steam-launched process. Direct launch injects before the first
  D3D call and eliminates descriptor misses; --watch modifies nothing about the
  install. Start without this, and only use it if late injection turns out to be
  what is hiding the render targets.
#>
param(
    [int]$Seconds = 240,
    [switch]$Direct,
    # Drive the game with the virtual pad: A through the main menu into the
    # first save slot, then walk. Without this the census only ever observes
    # the menu, whose object array is 90% UI widgets and whose depth targets
    # are not a real scene.
    [switch]$Play,
    [int]$PreDelay = 80,        # inZOI is a 35 GB title; do not touch it while loading
    [int]$MenuPresses = 3,      # Continue -> first slot -> confirm
    [int]$MenuGapMs = 9000,     # a save load sits between confirmations
    [int]$LoadWait = 90,        # world streaming after the last confirmation
    # Attempt ProcessEvent discovery. NOT read-only -- it installs a trampoline
    # on a UObject virtual. Safe enough to try now only because the search was
    # rebuilt: read-only triage picks ~3 candidate slots instead of sweeping 21
    # blind, and the validator stops after 2000 samples instead of running on
    # every one of UE5's ~300k dispatches per second. The earlier version of
    # this froze inZOI in under two seconds.
    [switch]$PeTriage
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll      = Join-Path $root "build\bin\segcap.dll"
$injector = Join-Path $root "build\bin\injector.exe"
$log      = Join-Path $root "build\bin\segcap.log"

$gameDir = "C:\Program Files (x86)\Steam\steamapps\common\inZOI\BlueClient\Binaries\Win64"
$gameExe = Join-Path $gameDir "inZOI-Win64-Shipping.exe"
$procName = "inZOI-Win64-Shipping"
$appId = 2456740

foreach ($f in @($dll, $injector, $gameExe)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}

# --- dismiss the screensaver BEFORE anything else -----------------------------
#
# Turning the setting off does not dismiss one that is already running, and an
# active screensaver switches the INPUT DESKTOP from "Default" to "Screen-saver".
# From Default, GetForegroundWindow() then correctly returns NULL and no window
# can be focused, so every synthetic input is discarded.
#
# This cost four runs on Stray and was written up in DEBUGGING.md 7.1 and listed
# as a known trap in RESUME.md -- and then this script walked into it anyway,
# because it ported the "disable the setting" half and not the "kill the running
# one" half. Doing it first, before the game launches, rather than inside the
# focus loop where it is already too late.
if (-not ("SS" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class SS {
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
[SS]::Off()
if ([SS]::Desktop() -ne "Default") {
    Write-Host "[inzoi] screensaver active ($([SS]::Desktop())) -- dismissing"
    Get-Process -Name "scrnsave", "*.scr" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
Write-Host "[inzoi] input desktop: $([SS]::Desktop())"

# --- safety: prove the run cannot write ---------------------------------------
Remove-Item (Join-Path $root "build\bin\segcap.mark") -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $root "build\bin\segcap.census") -Value "1" -NoNewline
Remove-Item (Join-Path $root "build\bin\segcap.abtest")   -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root "build\bin\segcap.captures") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root "build\bin\segcap.stride")   -ErrorAction SilentlyContinue
$peMarker = Join-Path $root "build\bin\segcap.petriage"
if ($PeTriage) {
    Set-Content -Path $peMarker -Value "1" -NoNewline
    Write-Host "[inzoi] NOT READ-ONLY: pe-triage marker set -- this run will hook a vtable slot"
} else {
    Remove-Item $peMarker -ErrorAction SilentlyContinue
    Write-Host "[inzoi] READ-ONLY: mark marker removed, census marker set"
}

Get-Process -Name "$procName*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[inzoi] closing stale $($_.Id)"; Stop-Process -Id $_.Id -Force
}
Start-Sleep -Seconds 2
Remove-Item $log -ErrorAction SilentlyContinue

# --- launch + inject ----------------------------------------------------------
$injOut = Join-Path $env:TEMP "inzoi_inject.txt"
if ($Direct) {
    $appidFile = Join-Path $gameDir "steam_appid.txt"
    if (-not (Test-Path $appidFile)) {
        Set-Content -Path $appidFile -Value "$appId" -NoNewline
        Write-Host "[inzoi] wrote steam_appid.txt (reversible; delete to undo)"
    }
    Write-Host "[inzoi] launching suspended and injecting"
    $inj = Start-Process -FilePath $injector -NoNewWindow -PassThru -RedirectStandardOutput $injOut `
        -ArgumentList @("--launch", "`"$gameExe`"", "--args", "`"-dx12`"",
                        "--workdir", "`"$gameDir`"", "--dll", "`"$dll`"")
} else {
    Write-Host "[inzoi] arming injector (--watch), then launching via Steam"
    $inj = Start-Process -FilePath $injector -NoNewWindow -PassThru -RedirectStandardOutput $injOut `
        -ArgumentList @("--dll", "`"$dll`"", "--watch", "$procName.exe", "--wait", "300")
    Start-Sleep -Seconds 1
    Start-Process "steam://rungameid/$appId"
}

$inj | Wait-Process -Timeout 300 -ErrorAction SilentlyContinue
Get-Content $injOut -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "[inject] $_" }

$game = $null
for ($i = 0; $i -lt 300; $i++) {
    $game = Get-Process -Name $procName -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game) { break }
    Start-Sleep -Seconds 1
}
if (-not $game) { throw "inZOI never started -- check $injOut" }
Write-Host "[inzoi] game pid $($game.Id); observing for ${Seconds}s"

# --- optionally drive it into a save ------------------------------------------
$pad = $null
if ($Play) {
    # Same two environmental hazards as Stray: an unfocused window discards
    # gamepad input, and the screensaver switches the input desktop after 60s
    # idle so nothing can be focused at all. Both cost multiple runs there; do
    # not re-pay for them here.
    if (-not ("W32i" -as [type])) {
        Add-Type @"
using System; using System.Runtime.InteropServices;
public class W32i {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a, uint b, IntPtr c, uint d);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
  public static void KeepAwake(bool on){ SetThreadExecutionState(on?(0x80000000u|1u|2u):0x80000000u); }
  public static void NoScreenSaver(){ SystemParametersInfoW(0x0011, 0, IntPtr.Zero, 0x02); }
  public static uint FgPid(){ IntPtr f=GetForegroundWindow(); if(f==IntPtr.Zero) return 0;
    uint p; GetWindowThreadProcessId(f, out p); return p; }
  public static bool Focus(IntPtr h, uint pid){
    if(h==IntPtr.Zero) return false; if(FgPid()==pid) return true;
    SystemParametersInfoW(0x2001,0,IntPtr.Zero,0x02);
    uint ft=GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), mt=GetCurrentThreadId();
    bool at=(ft!=0&&ft!=mt)&&AttachThreadInput(mt,ft,true);
    ShowWindow(h,9); BringWindowToTop(h); SetForegroundWindow(h);
    if(at) AttachThreadInput(mt,ft,false);
    return FgPid()==pid; }

  // The escalation that actually worked on Stray. A click goes to whatever is
  // TOPMOST at those coordinates, so raising the window first is not optional --
  // without it the click lands on the terminal driving the automation and the
  // run reports a bare "no focus" with no hint that it clicked the wrong thing.
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x,int y,int cx,int cy, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,IntPtr e);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr OpenInputDesktop(uint f, bool inh, uint acc);
  [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool GetUserObjectInformationW(IntPtr h,int i,IntPtr b,int n,out int need);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }

  public static void RaiseAbove(IntPtr h){
    SetWindowPos(h,new IntPtr(-1),0,0,0,0,0x0002|0x0001|0x0040);
    System.Threading.Thread.Sleep(60);
    SetWindowPos(h,new IntPtr(-2),0,0,0,0,0x0002|0x0001|0x0040); }

  public static void ClickToFocus(IntPtr h){
    RECT r; if(!GetWindowRect(h,out r)) return;
    int x=r.Left+Math.Max(8,(r.Right-r.Left)/12), y=r.Top+Math.Max(8,(r.Bottom-r.Top)/12);
    SetCursorPos(x,y); System.Threading.Thread.Sleep(80);
    mouse_event(0x0002,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(40);
    mouse_event(0x0004,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(200); }

  // Distinguishes "nothing has focus", "the terminal stole it" and "the
  // screensaver switched the input desktop" -- three identical symptoms with
  // three different fixes.
  public static string InputDesktop(){
    IntPtr d=OpenInputDesktop(0,false,0x0001);
    if(d==IntPtr.Zero) return "<denied: locked/secure>";
    IntPtr b=Marshal.AllocHGlobal(512);
    try{ int n; if(!GetUserObjectInformationW(d,2,b,512,out n)) return "<unknown>";
         return Marshal.PtrToStringUni(b); }
    finally{ Marshal.FreeHGlobal(b); CloseDesktop(d); } }

  public static string FgDesc(){
    IntPtr f=GetForegroundWindow(); if(f==IntPtr.Zero) return "<none>";
    uint p; GetWindowThreadProcessId(f,out p);
    var sb=new System.Text.StringBuilder(256); GetWindowTextW(f,sb,256);
    return "pid "+p+" '"+sb.ToString()+"'"; }
}
"@
    }
    [W32i]::KeepAwake($true)
    [W32i]::NoScreenSaver()
    Write-Host "[inzoi] input desktop: $([W32i]::InputDesktop())"

    $focused = $false
    for ($i = 0; $i -lt 120; $i++) {
        $game.Refresh()
        if ($game.HasExited) { break }
        if ($game.MainWindowHandle -ne 0) {
            if ([W32i]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
                $focused = $true; Write-Host "[inzoi] window focused at t=${i}s"; break
            }
            if ($i -ge 4) {
                [W32i]::RaiseAbove($game.MainWindowHandle)
                if ([W32i]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
                    $focused = $true; Write-Host "[inzoi] focused at t=${i}s (raise)"; break
                }
                [W32i]::ClickToFocus($game.MainWindowHandle)
                if ([W32i]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
                    $focused = $true; Write-Host "[inzoi] focused at t=${i}s (raise+click)"; break
                }
            }
        }
        if ($i % 15 -eq 14) {
            Write-Host ("[inzoi]   t={0}s unfocused; hwnd={1} fg={2} desktop={3}" -f `
                        $i, $game.MainWindowHandle, [W32i]::FgDesc(), [W32i]::InputDesktop())
        }
        Start-Sleep -Seconds 1
    }
    if (-not $focused) {
        Write-Host "[inzoi] WARNING: no focus; gamepad input will be discarded"
        Write-Host "[inzoi]   fg=$([W32i]::FgDesc())  desktop=$([W32i]::InputDesktop())"
    }

    $vpad = Join-Path $root "build\bin\vpad.exe"
    $padOut = Join-Path $env:TEMP "inzoi_vpad.txt"
    $inputLog = Join-Path $root "build\bin\segcap_input.jsonl"
    Remove-Item $inputLog -ErrorAction SilentlyContinue

    # Total pad lifetime must outlast the observation window, or the pad is
    # removed mid-run and the game stops receiving input.
    # NO menu presses.
    #
    # inZOI auto-resumes into the last save -- a screenshot taken 30s after a
    # cold launch, with no input at all, showed live gameplay. The A x3 written
    # for Stray's menus was therefore being delivered into a running world,
    # where A is a context action, and the game exited around the third press.
    #
    # Movement uses inZOI's own scheme, read off its on-screen legend: left
    # stick to walk, LB+right stick for camera, RB to run, B to cancel.
    $patrolFor = [Math]::Max(60, $Seconds - $PreDelay)
    Write-Host "[inzoi] vpad: wait ${PreDelay}s, then patrol ${patrolFor}s (inzoi profile, no A presses)"
    $pad = Start-Process -FilePath $vpad -NoNewWindow -PassThru -RedirectStandardOutput $padOut `
        -ArgumentList @("--pre-delay", "$PreDelay",
                        "--profile", "inzoi",
                        "--patrol", "$patrolFor", "--ui-escape", "45",
                        "--input-log", "`"$inputLog`"")
}

# --- observe ------------------------------------------------------------------
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 15
    if (-not (Get-Process -Id $game.Id -ErrorAction SilentlyContinue)) {
        Write-Host "[inzoi] !! game exited early -- check $log"
        break
    }
    # Re-assert focus. A game that opens a launcher, changes resolution, or
    # shows an overlay can take the foreground away mid-run, and from then on
    # every synthetic input is silently discarded.
    if ($Play -and [W32i]::FgPid() -ne [uint32]$game.Id) {
        $game.Refresh()
        if ($game.MainWindowHandle -ne 0) {
            [W32i]::Focus($game.MainWindowHandle, [uint32]$game.Id) | Out-Null
        }
    }
    $marks = Select-String -Path $log -Pattern "backbuffer|CreateDSV #|FOUND object array|ELECTED|no FChunkedFixed" `
                -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($marks) { Write-Host "[inzoi]   $($marks.Line)" }
}

if ($pad -and -not $pad.HasExited) { Stop-Process -Id $pad.Id -Force -ErrorAction SilentlyContinue }
if ($Play) {
    Get-Content (Join-Path $env:TEMP "inzoi_vpad.txt") -ErrorAction SilentlyContinue |
        Select-Object -Last 6 | ForEach-Object { Write-Host "[vpad] $_" }
    [W32i]::KeepAwake($false)
}

$snapshot = Join-Path $root "build\bin\segcap_inzoi.log"
Copy-Item $log $snapshot -Force -ErrorAction SilentlyContinue
Get-Process -Name "$procName*" -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "[inzoi] closed; log -> $snapshot"

# --- verdict ------------------------------------------------------------------
Write-Host ""
Write-Host "=============== STAGE 1: D3D12 LAYER ==============="
foreach ($p in @("segcap attached", "host:", "captured DIRECT command queue", "resolved device",
                 "backbuffer", "hooks installed", "descriptor misses")) {
    $m = Select-String -Path $snapshot -Pattern $p -ErrorAction SilentlyContinue | Select-Object -First 1
    Write-Host ("  {0,-32} {1}" -f $p, $(if ($m) { $m.Line.Trim() } else { "-- not seen --" }))
}
Write-Host ""
Write-Host "  depth-stencil resources the game created:"
$dsv = Select-String -Path $snapshot -Pattern "CreateDSV #" -ErrorAction SilentlyContinue
if ($dsv) { $dsv | Select-Object -First 10 | ForEach-Object { Write-Host "    $($_.Line.Trim())" } }
else { Write-Host "    -- none logged --" }
Write-Host ""
Write-Host "  election:"
$el = Select-String -Path $snapshot -Pattern "ELECTED|no viable CustomDepth" -ErrorAction SilentlyContinue |
      Select-Object -Last 3
if ($el) { $el | ForEach-Object { Write-Host "    $($_.Line.Trim())" } } else { Write-Host "    -- no election ran --" }

Write-Host ""
Write-Host "=============== STAGE 2: ENGINE LAYER ==============="
$eng = Select-String -Path $snapshot -Pattern "FOUND object array|no FChunkedFixedUObjectArray|NumElements|FNamePool|name pool" `
           -ErrorAction SilentlyContinue | Select-Object -First 8
if ($eng) { $eng | ForEach-Object { Write-Host "  $($_.Line.Trim())" } }
else { Write-Host "  -- engine discovery produced no output --" }
