<#
.SYNOPSIS
  Fully unattended Stray capture run: launch, inject, navigate to gameplay,
  patrol, capture, collect. No human in the loop at any point.

.DESCRIPTION
  This supersedes run_stray_test.ps1, which launched through Steam's URL
  protocol. That path worked but left two problems unsolved, and both of them
  cost real time before the cause was understood:

  1. INJECTION WAS ALWAYS LATE.
     Steam's launcher starts the game itself, so the injector could only watch
     for the process and attach after it existed. Every descriptor view created
     before we arrived was unrecoverable, so CreateRTV/CreateDSV never saw them
     and the resource behind those handles stayed unknown.

  2. FOCUS FAILED FOUR RUNS IN A ROW.
     Stray was configured for FullscreenMode=1 (borderless). During the mode
     switch that follows its menu, GetForegroundWindow() returns NULL -- there
     is genuinely no foreground window for a few seconds. Every attempt to take
     focus during that window fails, the virtual gamepad's input is discarded by
     an unfocused window, and the run sits at the main menu producing menu
     frames. Twice I misdiagnosed this, once blaming a locked session.

  Both are fixed by the same change: launch the shipping executable DIRECTLY.

  What made that impossible before was SteamAPI_RestartAppIfNecessary. Launched
  outside Steam, the game asks Steam to relaunch it and exits -- so the process
  we injected into suspended was never the process that rendered anything.
  Writing steam_appid.txt next to the executable is the documented way to tell
  the Steamworks API "this process IS the app", and it makes that call a no-op.
  Steam still has to be running; this is not a DRM bypass, it is the same
  mechanism Steamworks ships for developers running their own build.

  With direct launch we get, in order:

    --launch      CREATE_SUSPENDED, inject, resume. Injection happens before the
                  first D3D call, so descriptor coverage is complete.
    -windowed     no exclusive/borderless mode switch, so no NULL-foreground
                  window, so focus is stable from the moment the window exists.
    -ResX/-ResY   1920x1080. Pixel count is not cosmetic here: every mask is a
                  full-frame GPU->CPU readback plus a file write on the present
                  thread, so it is bandwidth on the game's critical path. 720p
                  was used while the pipeline was being debugged (a quarter of
                  the 2560x1600 the game defaults to); 1080p is for the demo,
                  and doubles the per-frame cost again. If a run starts
                  hitching, this is the first thing to lower.

  Measured: window up at t=6s and foreground held continuously, versus 20s+ and
  four consecutive focus failures through Steam.

.PARAMETER Seconds
  Total gameplay time after the menu. Default 240.

.PARAMETER NoKill
  Leave the game running at the end.

.PARAMETER NoMark
  Delete build/bin/segcap.mark before launching, which makes segcap capture
  without enabling CustomDepth marking. This is the "A" side of the A/B test
  that proves we did not change what the player sees.
#>
param(
    [int]$Seconds = 240,
    [switch]$NoKill,
    [switch]$NoMark,
    # A/B run: hold the camera still and capture colour frames unconditionally,
    # so frames from before marking begins can be compared against frames from
    # after it. Done as ONE run rather than two: across two sessions the cat's
    # idle animation and the NPCs are at different phases, which shows up as a
    # large pixel difference that has nothing to do with CustomDepth.
    [switch]$AbTest,
    # Capture profile. Demo runs want a small stride (consecutive gameplay,
    # smooth playback); identity-analysis runs want a large one (minutes of
    # coverage, so objects actually leave and re-enter the working set). One
    # run cannot serve both well.
    [int]$Captures = 0,
    [int]$Stride = 0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll      = Join-Path $root "build\bin\segcap.dll"
$injector = Join-Path $root "build\bin\injector.exe"
$vpad     = Join-Path $root "build\bin\vpad.exe"
$log      = Join-Path $root "build\bin\segcap.log"
$markFile = Join-Path $root "build\bin\segcap.mark"

$gameDir = "C:\Program Files (x86)\Steam\steamapps\common\Stray\Hk_project\Binaries\Win64"
$gameExe = Join-Path $gameDir "Stray-Win64-Shipping.exe"

foreach ($f in @($dll, $injector, $vpad, $gameExe)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}

# Assert the FULL marker state, not just the markers this script sets.
#
# These files are the DLL's only configuration channel, they live next to the
# DLL, and they outlive the run that created them. A run of the inZOI census
# left segcap.census and segcap.petriage behind, and the next Stray run picked
# them up and quietly executed as a census: it launched, injected, drove the
# game, reported "IN-LEVEL confirmed" and exited 0, having marked nothing and
# captured nothing. Nothing in that output said "census" -- the mode only
# appeared 10 seconds into the DLL log.
#
# Same shape as every other bug in this project that cost real time: a stale
# piece of state read at the wrong moment, reporting the answer that matched
# the hypothesis. So every runner now states every marker it depends on.
foreach ($stale in @("segcap.census", "segcap.petriage")) {
    $p = Join-Path $root "build\bin\$stale"
    if (Test-Path $p) {
        Write-Host "[auto] clearing stale marker: $stale"
        Remove-Item $p -Force
    }
}

# steam_appid.txt is what makes direct launch possible at all. Check rather than
# assume -- without it the game silently relaunches through Steam and every
# symptom looks like an injection bug instead of a launch bug.
$appidFile = Join-Path $gameDir "steam_appid.txt"
if (-not (Test-Path $appidFile)) {
    throw "steam_appid.txt missing from $gameDir -- direct launch will be defeated by SteamAPI_RestartAppIfNecessary"
}
if (-not (Get-Process -Name "steam" -ErrorAction SilentlyContinue)) {
    throw "Steam is not running; the Steamworks API will fail to initialise"
}

if (-not ("W32" -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32 {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool c);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, IntPtr c, uint d);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after,
                                                                    int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);

    // Raise the game above every other window before trying to focus or click.
    //
    // Without this, ClickToFocus was clicking whatever window happened to be
    // topmost at those screen coordinates -- which, in an automated run, is the
    // terminal driving the automation. The click "succeeded" (mouse_event
    // reports nothing), focus stayed where it was, and the run reported a bare
    // "no focus" with no indication that the click had landed on the wrong
    // window entirely. Making the target topmost first removes the ambiguity.
    //
    // TOPMOST is then dropped back to plain top: leaving it permanently
    // above-everything is hostile if the user is present, and unnecessary once
    // the game holds focus.
    public static void RaiseAbove(IntPtr hWnd) {
        const uint SWP_NOMOVE = 0x0002, SWP_NOSIZE = 0x0001, SWP_SHOWWINDOW = 0x0040;
        IntPtr HWND_TOPMOST = new IntPtr(-1), HWND_NOTOPMOST = new IntPtr(-2);
        SetWindowPos(hWnd, HWND_TOPMOST,   0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
        System.Threading.Thread.Sleep(60);
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    }

    // Who actually holds the foreground, by name and title. Reporting only
    // "focused: False" gives no way to tell "nothing has focus" from "the
    // terminal stole it" from "a Steam overlay is in front" -- three different
    // bugs with three different fixes.
    public static string ForegroundDesc() {
        IntPtr fg = GetForegroundWindow();
        if (fg == IntPtr.Zero) return "<no foreground window>";
        uint pid; GetWindowThreadProcessId(fg, out pid);
        var sb = new System.Text.StringBuilder(256);
        GetWindowTextW(fg, sb, 256);
        return "pid " + pid + " '" + sb.ToString() + "'";
    }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    // Clicking focuses a window at the OS level regardless of how the app
    // handles input, and it works when SetForegroundWindow is refused.
    //
    // This was in the previous harness and was dropped when this one was
    // written; the very next run became the first in three to fail focus
    // outright, stranding the session at the menu. Restored, and kept as an
    // escalation rather than the first move because the polite path usually
    // works and a click is a real event the game will see.
    //
    // The click lands in the top-left twelfth, not the centre: Stray's menus
    // put interactive elements centrally and a stray click there could select
    // something. A corner is inert.
    public static void ClickToFocus(IntPtr hWnd) {
        RECT r;
        if (!GetWindowRect(hWnd, out r)) return;
        int x = r.Left + Math.Max(8, (r.Right - r.Left) / 12);
        int y = r.Top  + Math.Max(8, (r.Bottom - r.Top) / 12);
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(80);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);   // LEFTDOWN
        System.Threading.Thread.Sleep(40);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);   // LEFTUP
        System.Threading.Thread.Sleep(200);
    }

    // Windows will not focus a window on a sleeping desktop, and will not
    // deliver injected input there either. An unattended run that outlives the
    // idle timeout dies silently without this.
    //
    // NOTE: this is necessary but NOT sufficient. ES_DISPLAY_REQUIRED prevents
    // the display powering down; it does not prevent the SCREENSAVER. See
    // SetScreenSaver below -- they are two separate mechanisms and blocking
    // only one of them was the cause of four consecutive failed runs.
    public static void KeepAwake(bool on) {
        SetThreadExecutionState(on ? (0x80000000u | 1u | 2u) : 0x80000000u);
    }

    [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a, uint p, IntPtr v, uint w);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr OpenInputDesktop(uint f, bool inherit, uint access);
    [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetUserObjectInformationW(IntPtr h, int index, IntPtr buf, int len, out int need);

    // Turn the screensaver off for the duration of an unattended run.
    //
    // This is THE fix for the failure that cost the most runs in this project.
    // With the user away, no physical input arrives; after the 60-second idle
    // timeout Windows activates the screensaver, which switches the INPUT
    // DESKTOP from "Default" to "Screen-saver". From the Default desktop,
    // GetForegroundWindow() then correctly returns NULL -- there genuinely is
    // no foreground window there any more -- so focus can never be taken and
    // every synthetic input goes nowhere. The game sits at the menu.
    //
    // What made this expensive to find is that the symptom is indistinguishable
    // from a locked session, and I twice diagnosed it as one, once blaming the
    // user's machine. The distinguishing measurement is the input desktop NAME
    // plus ScreenSaverIsSecure: a locked session denies OpenInputDesktop
    // outright, whereas here it opens and reports "Screen-saver". Guessing
    // between two hypotheses that produce the same symptom is what the probe
    // below exists to stop.
    //
    // SPI_SETSCREENSAVEACTIVE is a user setting, so it is saved and restored
    // rather than simply forced off.
    const uint SPI_GETSCREENSAVEACTIVE = 0x0010;
    const uint SPI_SETSCREENSAVEACTIVE = 0x0011;
    const uint SPIF_SENDCHANGE = 0x0002;

    public static bool GetScreenSaverEnabled() {
        IntPtr b = Marshal.AllocHGlobal(4);
        try {
            Marshal.WriteInt32(b, 0);
            SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, b, 0);
            return Marshal.ReadInt32(b) != 0;
        } finally { Marshal.FreeHGlobal(b); }
    }

    public static void SetScreenSaver(bool on) {
        SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, on ? 1u : 0u, IntPtr.Zero, SPIF_SENDCHANGE);
    }

    // Name of the desktop currently receiving input: "Default" normally,
    // "Screen-saver" while the screensaver runs, and unopenable when locked.
    public static string InputDesktop() {
        IntPtr d = OpenInputDesktop(0, false, 0x0001 /*DESKTOP_READOBJECTS*/);
        if (d == IntPtr.Zero) return "<denied: locked or secure desktop>";
        IntPtr b = Marshal.AllocHGlobal(512);
        try {
            int need;
            if (!GetUserObjectInformationW(d, 2 /*UOI_NAME*/, b, 512, out need)) return "<unknown>";
            return Marshal.PtrToStringUni(b);
        } finally { Marshal.FreeHGlobal(b); CloseDesktop(d); }
    }

    // 0 means "no foreground window at all", which is NOT the same as "some
    // process with pid 0". Conflating them produced a confident, wrong
    // diagnosis of a locked session on a machine that was not locked.
    public static uint ForegroundPid() {
        IntPtr fg = GetForegroundWindow();
        if (fg == IntPtr.Zero) return 0;
        uint pid; GetWindowThreadProcessId(fg, out pid); return pid;
    }

    // Compare by PROCESS, not by HWND: a D3D12 title often has a different
    // window in the foreground than the one .NET reports as MainWindowHandle,
    // so an HWND comparison reports "not focused" for a game plainly in front.
    public static bool Focus(IntPtr hWnd, uint targetPid) {
        if (hWnd == IntPtr.Zero) return false;
        if (ForegroundPid() == targetPid) return true;
        SystemParametersInfo(0x2001, 0, IntPtr.Zero, 0x02);   // SPI_SETFOREGROUNDLOCKTIMEOUT
        uint fgT = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        uint myT = GetCurrentThreadId();
        bool att = (fgT != 0 && fgT != myT) && AttachThreadInput(myT, fgT, true);
        ShowWindow(hWnd, 9); BringWindowToTop(hWnd); SetForegroundWindow(hWnd);
        if (att) AttachThreadInput(myT, fgT, false);
        return ForegroundPid() == targetPid;
    }
}
"@
}

# --- 0. clean slate ----------------------------------------------------------
[W32]::KeepAwake($true)
$saverWasOn = [W32]::GetScreenSaverEnabled()
if ($saverWasOn) { [W32]::SetScreenSaver($false) }
Write-Host "[auto] sleep suppressed; screensaver was $(if($saverWasOn){'ON -> disabled for this run'}else{'already off'})"
Write-Host "[auto] input desktop: $([W32]::InputDesktop())"

# If the screensaver is ALREADY running we are on the "Screen-saver" desktop
# and nothing we do can focus a window on "Default" until it exits. Disabling
# the setting does not dismiss an active one, so it is closed explicitly.
if ([W32]::InputDesktop() -ne "Default") {
    Write-Host "[auto] screensaver is active -- dismissing it"
    Get-Process -Name "scrnsave", "*.scr" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Write-Host "[auto] input desktop now: $([W32]::InputDesktop())"
}

Get-Process -Name "Stray*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[auto] closing stale $($_.ProcessName) ($($_.Id))"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Seconds 3

Remove-Item $log -ErrorAction SilentlyContinue
# Archive rather than delete. This line used to be a Remove-Item, and running
# the Stray regression check destroyed a verified inZOI gameplay session that
# had cost three attempts and twenty minutes of game time to produce.
& (Join-Path $root "tools\archive_capture.ps1") -Title "prev" -Bin (Join-Path $root "build\bin")
Get-ChildItem (Join-Path $root "build\bin") -Filter "segcap_frame_*" -ErrorAction SilentlyContinue | Remove-Item -Force

if ($NoMark) {
    Remove-Item $markFile -ErrorAction SilentlyContinue
    Write-Host "[auto] marking DISABLED (A-side baseline: no CustomDepth writes)"
} else {
    Set-Content -Path $markFile -Value "1" -NoNewline
    Write-Host "[auto] marking ENABLED"
}

$capFile = Join-Path $root "build\bin\segcap.captures"
$strFile = Join-Path $root "build\bin\segcap.stride"
if ($Captures -gt 0) { Set-Content $capFile -Value "$Captures" -NoNewline } else { Remove-Item $capFile -ErrorAction SilentlyContinue }
if ($Stride -gt 0)   { Set-Content $strFile -Value "$Stride"   -NoNewline } else { Remove-Item $strFile -ErrorAction SilentlyContinue }
if ($Captures -gt 0 -or $Stride -gt 0) {
    Write-Host "[auto] capture profile: captures=$Captures stride=$Stride"
}

$abFile = Join-Path $root "build\bin\segcap.abtest"
if ($AbTest) {
    Set-Content -Path $abFile -Value "1" -NoNewline
    Write-Host "[auto] A/B mode: colour frames captured unconditionally on a stride"
} else {
    Remove-Item $abFile -ErrorAction SilentlyContinue
}

# --- 1. suspended launch + inject + resume -----------------------------------
# One call does all three. Injection lands before the process executes a single
# instruction of its own, which is the only way descriptor coverage is complete.
$gameArgs = "-dx12 -windowed -ResX=1920 -ResY=1080 -nosplash"
Write-Host "[auto] launching suspended and injecting"
Write-Host "[auto]   $gameExe $gameArgs"

$injOut = Join-Path $env:TEMP "auto_inject.txt"
$inj = Start-Process -FilePath $injector -NoNewWindow -PassThru -RedirectStandardOutput $injOut `
    -ArgumentList @(
        "--launch",  "`"$gameExe`"",
        "--args",    "`"$gameArgs`"",
        "--workdir", "`"$gameDir`"",
        "--dll",     "`"$dll`""
    )
$inj | Wait-Process -Timeout 120 -ErrorAction SilentlyContinue
Get-Content $injOut -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "[inject] $_" }

$game = $null
for ($i = 0; $i -lt 60; $i++) {
    $game = Get-Process -Name "Stray-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game) { break }
    Start-Sleep -Seconds 1
}
if (-not $game) { throw "Stray never started -- check $injOut" }
Write-Host "[auto] game pid $($game.Id)"

# --- 2. window + focus -------------------------------------------------------
# In windowed mode the window appears in ~6s and takes focus on its own, so this
# loop normally exits on its first iteration. It is kept because a silent focus
# failure sinks the whole run, and verifying is cheap.
$focused = $false
for ($i = 0; $i -lt 60; $i++) {
    $game.Refresh()
    if ($game.HasExited) { throw "game exited during startup -- check $log" }
    if ($game.MainWindowHandle -ne 0) {
        if ([W32]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
            $focused = $true
            Write-Host "[auto] window up and focused at t=${i}s"
            break
        }
        # Escalate once the polite path has had a few tries: raise the window
        # above everything else FIRST, then click it. Clicking without raising
        # hits whatever is on top at those coordinates, which during an
        # automated run is the terminal running this script.
        if ($i -ge 4) {
            [W32]::RaiseAbove($game.MainWindowHandle)
            if ([W32]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
                $focused = $true
                Write-Host "[auto] window focused at t=${i}s (via raise)"
                break
            }
            [W32]::ClickToFocus($game.MainWindowHandle)
            if ([W32]::Focus($game.MainWindowHandle, [uint32]$game.Id)) {
                $focused = $true
                Write-Host "[auto] window focused at t=${i}s (via raise+click)"
                break
            }
        }
        if ($i % 10 -eq 9) {
            # Report the input desktop alongside the foreground window. "No
            # foreground window" alone is ambiguous between a mode switch, a
            # screensaver and a lock; the desktop name separates them.
            Write-Host ("[auto]   t={0}s unfocused; foreground={1} desktop={2}" -f `
                        $i, [W32]::ForegroundDesc(), [W32]::InputDesktop())
        }
    }
    Start-Sleep -Seconds 1
}
if (-not $focused) {
    Write-Host "[auto] WARNING: no focus; gamepad input will be discarded"
    Write-Host "[auto]   foreground: $([W32]::ForegroundDesc())  desktop: $([W32]::InputDesktop())"
}
Start-Sleep -Seconds 5

# --- 3. drive the game -------------------------------------------------------
# SendInput was tried and Stray ignored it -- proven by the object array staying
# at the menu's slot count after four scan-code taps. ViGEm presents the pad
# through a kernel bus driver, so the game cannot tell it from real hardware.
$padOut = Join-Path $env:TEMP "vpad_out.txt"
# The input log lands beside the captures so a session is self-contained:
# frames, masks, sidecars and the actions that produced them in one directory.
$inputLog = Join-Path $root "build\bin\segcap_input.jsonl"
Remove-Item $inputLog -ErrorAction SilentlyContinue
if ($AbTest) {
    # Menu only, then nothing. A moving camera would swamp the measurement:
    # the question is whether MARKING changed the image, so everything else
    # must be held as still as the engine allows.
    Write-Host "[auto] vpad: menu only (A/B run holds the camera still)"
    $pad = Start-Process -FilePath $vpad -NoNewWindow -PassThru -RedirectStandardOutput $padOut `
        -ArgumentList "--menu --menu-presses 8 --input-log `"$inputLog`""
} else {
    $patrolFor = [Math]::Max(30, $Seconds - 40)
    Write-Host "[auto] vpad: menu, then ${patrolFor}s patrol"
    $pad = Start-Process -FilePath $vpad -NoNewWindow -PassThru -RedirectStandardOutput $padOut `
        -ArgumentList "--menu --menu-presses 8 --patrol $patrolFor --input-log `"$inputLog`""
}

# --- 4. run, watching for the in-level transition ----------------------------
# The object array grows from ~175k slots at the menu to ~320k+ once a level
# streams in. That is the signal that we are actually in gameplay, and it is the
# same one that proved SendInput was being ignored.
Write-Host "[auto] running for ${Seconds}s"
$deadline = (Get-Date).AddSeconds($Seconds)
$announcedInLevel = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 10
    if (-not (Get-Process -Id $game.Id -ErrorAction SilentlyContinue)) {
        Write-Host "[auto] !! game exited early -- check $log"
        break
    }
    if (-not $announcedInLevel) {
        $peak = Select-String -Path $log -Pattern "array has (\d+) slots" -ErrorAction SilentlyContinue |
                ForEach-Object { [int]$_.Matches.Groups[1].Value } |
                Sort-Object -Descending | Select-Object -First 1
        if ($peak -and $peak -gt 250000) {
            Write-Host "[auto] IN-LEVEL confirmed ($peak slots)"
            $announcedInLevel = $true
        }
    }
    if ([W32]::ForegroundPid() -ne [uint32]$game.Id) {
        $game.Refresh()
        if ($game.MainWindowHandle -ne 0) {
            [W32]::Focus($game.MainWindowHandle, [uint32]$game.Id) | Out-Null
        }
    }
}

# --- 5. collect --------------------------------------------------------------
if ($pad -and -not $pad.HasExited) { Stop-Process -Id $pad.Id -Force -ErrorAction SilentlyContinue }
Get-Content $padOut -ErrorAction SilentlyContinue | Select-Object -Last 4 |
    ForEach-Object { Write-Host "[vpad] $_" }

$suffix = if ($NoMark) { "_nomark" } else { "" }
$snapshot = Join-Path $root "build\bin\segcap_auto$suffix.log"
Copy-Item $log $snapshot -Force -ErrorAction SilentlyContinue

if (-not $NoKill) {
    Get-Process -Name "Stray*" -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host "[auto] game closed"
}
[W32]::KeepAwake($false)
# Restore the user's screensaver preference rather than leaving it off.
if ($saverWasOn) { [W32]::SetScreenSaver($true); Write-Host "[auto] screensaver setting restored" }

# --- 6. verdict --------------------------------------------------------------
$bin = Join-Path $root "build\bin"
$masks  = @(Get-ChildItem $bin -Filter "segcap_mask_*.pgm"  -ErrorAction SilentlyContinue)
$frames = @(Get-ChildItem $bin -Filter "segcap_frame_*.*"   -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in ".ppm", ".png" })
$cars   = @(Get-ChildItem $bin -Filter "segcap_mask_*.json" -ErrorAction SilentlyContinue)

$peak = Select-String -Path $snapshot -Pattern "array has (\d+) slots" -ErrorAction SilentlyContinue |
        ForEach-Object { [int]$_.Matches.Groups[1].Value } | Sort-Object -Descending | Select-Object -First 1

Write-Host ""
Write-Host "=============== RESULT ==============="
Write-Host ("  peak object slots : {0}  -> {1}" -f $peak,
            $(if ($peak -gt 250000) { "IN-LEVEL" } else { "STILL AT MENU" }))
Write-Host ("  masks / frames / sidecars : {0} / {1} / {2}" -f $masks.Count, $frames.Count, $cars.Count)
Write-Host ("  log : {0}" -f $snapshot)

# A run that was asked to mark and produced nothing is a FAILED run, and it
# needs to say so here rather than in the twelfth line of a log nobody reads.
# The census-marker leak was invisible precisely because this block reported
# "IN-LEVEL" and stopped talking.
if (-not $NoMark) {
    $censusMode = Select-String -Path $snapshot -Pattern "skipping ProcessEvent discovery" -ErrorAction SilentlyContinue
    $peFound    = Select-String -Path $snapshot -Pattern "ProcessEvent CONFIRMED" -ErrorAction SilentlyContinue
    if ($censusMode) {
        Write-Host "  VERDICT: FAILED -- ran in CENSUS mode despite marking being requested."
        Write-Host "           A stale segcap.census marker was present at launch."
    } elseif (-not $peFound) {
        Write-Host "  VERDICT: FAILED -- ProcessEvent was never confirmed, so nothing could be marked."
    } elseif ($masks.Count -eq 0) {
        Write-Host "  VERDICT: FAILED -- ProcessEvent found but no masks were written."
    } else {
        Write-Host "  VERDICT: ok"
    }
}
Write-Host "======================================"
Select-String -Path $snapshot -Pattern "CONFIRMED|marked \d+|elected|distinct stencil" -ErrorAction SilentlyContinue |
    Select-Object -Last 12 | ForEach-Object { Write-Host "  $($_.Line)" }
