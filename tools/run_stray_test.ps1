<#
.SYNOPSIS
  Launch Stray, inject segcap, get past the menu, idle in gameplay, collect the log.

.DESCRIPTION
  Removes the human from the test loop. Three of the four steps automate cleanly:

    launch      steam://rungameid/1332010 -- no clicking, Steam's own URL protocol
    inject      the injector's --watch mode already handles this
    collect     the log is shared-read, so it can be copied while the game runs

  Only menu navigation is uncertain. Games that read raw input usually still see
  SendInput, because it injects into the same queue raw input reads from -- but
  titles that filter LLMHF_INJECTED will ignore it. If that happens, the script
  says so plainly instead of silently timing out, and a human presses one key.

  What makes this viable at all: ProcessEvent fires constantly from the engine's
  own dispatch -- animation, AI, timers. The cat does not need to move. We only
  need to get past the menu and then idle.

.PARAMETER Seconds
  How long to stay in gameplay after the menu. Default 220 -- long enough for the
  t+120s sample plus the ~30s ProcessEvent candidate search.

.PARAMETER NoKill
  Leave the game running at the end instead of closing it.

.PARAMETER NoInput
  Skip menu navigation entirely; useful to check whether SendInput is the thing
  that is failing.
#>
param(
    [int]$Seconds = 220,
    [switch]$NoKill,
    [switch]$NoInput
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll = Join-Path $root "build\bin\segcap.dll"
$injector = Join-Path $root "build\bin\injector.exe"
$log = Join-Path $root "build\bin\segcap.log"

if (-not (Test-Path $dll)) { throw "segcap.dll not built: $dll" }

# --- SendInput P/Invoke ------------------------------------------------------
# SendKeys is not used: it posts messages, which games reading raw input ignore.
# SendInput injects at the system level, into the same queue raw input drains.
if (-not ("Win32Input" -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Input {
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags;
                               public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size=40)]
    public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint n, INPUT[] pInputs, int cbSize);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint id, uint to, bool attach);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, IntPtr v, uint winIni);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr extra);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    const uint MOUSEEVENTF_LEFTUP = 0x0004;

    [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
    const uint ES_CONTINUOUS = 0x80000000;
    const uint ES_SYSTEM_REQUIRED = 0x00000001;
    const uint ES_DISPLAY_REQUIRED = 0x00000002;

    // Keep the display and system awake for the duration of a capture run.
    //
    // This is a hard requirement, not a nicety. A run failed with
    // "foreground pid 0 = Idle" -- the System Idle Process, meaning NO window
    // held the foreground because the display had slept. Windows will not focus
    // a window on a sleeping or locked desktop and blocks injected input there,
    // so an unattended session simply dies once the machine idles. For a dataset
    // pipeline meant to run for hours unattended, that is the difference between
    // working and not.
    //
    // Note this cannot defeat a LOCKED session (Win+L or a lock-on-resume
    // policy) -- nothing in user space can. Preventing idle sleep is what is
    // available.
    public static void KeepAwake(bool on) {
        SetThreadExecutionState(on
            ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
            : ES_CONTINUOUS);
    }

    // Clicking a window focuses it at the OS level regardless of how the app
    // handles input. AttachThreadInput alone proved unreliable -- one run
    // reported focused=True and the next False with no change in between, and a
    // failed focus silently sinks the whole session: gamepad ignored, stuck at
    // the menu, ProcessEvent never confirms, no masks.
    //
    // The click lands in the top-left region rather than dead centre: Stray's
    // menus put interactive elements centrally, and a stray click there could
    // activate something. A corner is inert.
    public static void ClickToFocus(IntPtr hWnd) {
        RECT r;
        if (!GetWindowRect(hWnd, out r)) return;
        int x = r.Left + Math.Max(8, (r.Right - r.Left) / 12);
        int y = r.Top + Math.Max(8, (r.Bottom - r.Top) / 12);
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(80);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(200);
    }

    const uint SPI_SETFOREGROUNDLOCKTIMEOUT = 0x2001;
    const uint SPIF_SENDCHANGE = 0x02;

    // Windows refuses SetForegroundWindow from a process that does not own the
    // current foreground window -- it flashes the taskbar instead. Three
    // mechanisms together defeat that reliably:
    //
    //   1. drop the foreground lock timeout to zero
    //   2. attach our input queue to the current foreground thread, which makes
    //      Windows treat us as "the same" input context and permits the switch
    //   3. then ShowWindow + BringWindowToTop + SetForegroundWindow
    //
    // Needed because an unattended dataset run cannot rely on a human clicking
    // the window: the virtual gamepad delivers input correctly, but an unfocused
    // window ignores it.
    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    // Returns the PID owning the current foreground window.
    public static uint ForegroundPid() {
        uint pid;
        GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        return pid;
    }

    // Verification is by PROCESS, not window handle.
    //
    // An earlier version required GetForegroundWindow() == MainWindowHandle and
    // reported focused=False on runs where the game was plainly in front. A
    // fullscreen D3D12 title often has a different HWND in the foreground than
    // the one .NET reports as MainWindowHandle -- so the check was wrong, not
    // the focus. Comparing owning PIDs asks the question we actually care
    // about: is the game the active application?
    public static bool ForceForeground(IntPtr hWnd, uint targetPid) {
        if (hWnd == IntPtr.Zero) return false;
        if (ForegroundPid() == targetPid) return true;   // already there

        SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, IntPtr.Zero, SPIF_SENDCHANGE);

        uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        uint myThread = GetCurrentThreadId();
        bool attached = false;
        if (fgThread != 0 && fgThread != myThread) {
            attached = AttachThreadInput(myThread, fgThread, true);
        }

        ShowWindow(hWnd, 9);          // SW_RESTORE
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);

        if (attached) AttachThreadInput(myThread, fgThread, false);
        return ForegroundPid() == targetPid;
    }

    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_SCANCODE = 0x0008;

    // Scan codes, not virtual keys. Many games read scan codes directly from
    // raw input and never look at the VK field.
    public static void TapScan(ushort scan, int holdMs) {
        INPUT[] down = new INPUT[1];
        down[0].type = 1;
        down[0].ki.wScan = scan;
        down[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] up = new INPUT[1];
        up[0].type = 1;
        up[0].ki.wScan = scan;
        up[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
}

function Tap([ushort]$scan, [string]$name) {
    Write-Host "[auto]   tap $name"
    [Win32Input]::TapScan($scan, 60)
    Start-Sleep -Milliseconds 700
}

# --- 0. clean slate ----------------------------------------------------------
# Keep the machine awake first. A previous run died with "foreground pid 0 =
# Idle": the display had slept, so no window held the foreground and nothing
# could be focused or receive input.
[Win32Input]::KeepAwake($true)
Write-Host "[auto] display/system sleep suppressed for this run"

Get-Process -Name "Stray*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[auto] closing existing $($_.ProcessName) ($($_.Id))"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue

# --- 1. arm the injector BEFORE launching ------------------------------------
# Injecting at process start is what keeps descriptor misses at zero; views
# created before we arrive cannot be recovered.
Write-Host "[auto] arming injector"
$inj = Start-Process -FilePath $injector `
    -ArgumentList "--dll `"$dll`" --watch Stray-Win64-Shipping.exe --wait 300" `
    -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\auto_inject.txt"

# --- 2. launch via Steam's URL protocol --------------------------------------
Write-Host "[auto] launching steam://rungameid/1332010"
Start-Process "steam://rungameid/1332010"

# --- 3. wait for the renderer process ----------------------------------------
$game = $null
for ($i = 0; $i -lt 120; $i++) {
    $game = Get-Process -Name "Stray-Win64-Shipping" -ErrorAction SilentlyContinue
    if ($game) { break }
    Start-Sleep -Seconds 1
}
if (-not $game) { throw "Stray never started" }
Write-Host "[auto] game up as pid $($game.Id)"

$inj | Wait-Process -Timeout 60 -ErrorAction SilentlyContinue
Get-Content "$env:TEMP\auto_inject.txt" -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "[inject] $_" }

# --- 4. drive the game with a virtual gamepad --------------------------------
#
# SendInput was tried first and Stray ignored it -- proven, not assumed: after
# four scan-code Enter taps the engine's object array stayed at the menu's
# 173,598 slots instead of growing to the ~320,000 an in-level session shows.
#
# ViGEm presents a controller through a kernel bus driver, so the game cannot
# tell it from real hardware. The pad exists only while vpad.exe runs, so it is
# started as a background process alongside the game rather than before it.
if (-not $NoInput) {
    $vpad = Join-Path $root "build\bin\vpad.exe"
    if (-not (Test-Path $vpad)) { throw "vpad.exe not built: $vpad" }

    Write-Host "[auto] waiting for the game window"
    for ($i = 0; $i -lt 90; $i++) {
        $game.Refresh()
        if ($game.MainWindowHandle -ne 0) { break }
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Seconds 20   # let the menu finish animating in

    # Focus, verified rather than assumed. An unfocused window silently ignores
    # gamepad input, which previously required a human to click the window --
    # exactly what an unattended dataset run cannot depend on.
    # Focus, verified rather than assumed, escalating if the polite method fails.
    # A failed focus silently sinks the entire session -- gamepad ignored, stuck
    # at the menu, ProcessEvent never confirms, no masks -- so it is worth
    # several attempts and a fallback.
    $focused = $false
    for ($i = 0; $i -lt 25; $i++) {
        $game.Refresh()
        if ($game.MainWindowHandle -eq 0) { Start-Sleep -Seconds 1; continue }

        if ([Win32Input]::ForceForeground($game.MainWindowHandle, [uint32]$game.Id)) {
            $focused = $true; break
        }
        # Escalate: a real click focuses at the OS level whatever the app does.
        if ($i -ge 3) {
            [Win32Input]::ClickToFocus($game.MainWindowHandle)
            if ([Win32Input]::ForceForeground($game.MainWindowHandle, [uint32]$game.Id)) {
                $focused = $true; break
            }
        }
        Start-Sleep -Seconds 1
    }
    $fgPid = [Win32Input]::ForegroundPid()
    $fgName = (Get-Process -Id $fgPid -ErrorAction SilentlyContinue).ProcessName
    Write-Host "[auto] window focused: $focused  (foreground pid $fgPid = $fgName, game pid $($game.Id))"
    if (-not $focused) {
        Write-Host "[auto] WARNING: could not take focus; gamepad input will be ignored"
    }
    Start-Sleep -Seconds 2

    # Menu, then patrol for the remaining time. Patrol matters beyond getting
    # in-level: a static camera yields thousands of near-identical frames, which
    # is close to worthless as segmentation training data.
    $patrolFor = [Math]::Max(20, $Seconds - 20)
    Write-Host "[auto] handing control to vpad (menu, then $patrolFor s patrol)"
    $padProc = Start-Process -FilePath $vpad `
        -ArgumentList "--menu --patrol $patrolFor" `
        -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\vpad_out.txt"
}

# --- 5. idle in gameplay -----------------------------------------------------
Write-Host "[auto] idling $Seconds s (ProcessEvent fires without player input)"
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 10
    if (-not (Get-Process -Id $game.Id -ErrorAction SilentlyContinue)) {
        Write-Host "[auto] !! game exited early -- possible crash, check the log"
        break
    }
    # Only stop early on SUCCESS. An earlier version also broke on "not found in
    # slots", which meant a failed run terminated before the object samples ran
    # -- losing exactly the diagnostic data needed to understand the failure.
    # Stopping early on failure is precisely backwards.
    $done = Select-String -Path $log -Pattern "ProcessEvent CONFIRMED" -ErrorAction SilentlyContinue
    if ($done) { Write-Host "[auto] ProcessEvent confirmed; letting samples run"; }
}

# --- 6. collect --------------------------------------------------------------
if ($padProc -and -not $padProc.HasExited) {
    Stop-Process -Id $padProc.Id -Force -ErrorAction SilentlyContinue
}
Get-Content "$env:TEMP\vpad_out.txt" -ErrorAction SilentlyContinue |
    Select-Object -Last 6 | ForEach-Object { Write-Host "[vpad] $_" }

$snapshot = Join-Path $root "build\bin\segcap_auto.log"
Copy-Item $log $snapshot -Force -ErrorAction SilentlyContinue
Write-Host "[auto] log copied to $snapshot"

# The decisive check for whether we actually got in-level: the object array
# grows from ~173k (menu) to ~320k once the level streams in. This is the same
# signal that proved SendInput was failing.
$slots = Select-String -Path $snapshot -Pattern "array has (\d+) slots" -ErrorAction SilentlyContinue |
    ForEach-Object { [int]$_.Matches.Groups[1].Value } | Sort-Object -Descending | Select-Object -First 1
if ($slots) {
    $verdict = if ($slots -gt 250000) { "IN-LEVEL (gamepad worked)" } else { "still at menu" }
    Write-Host "[auto] peak object-array slots: $slots  -> $verdict"
}

if (-not $NoKill) {
    Get-Process -Name "Stray*" -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host "[auto] game closed"
}

# Release the sleep suppression, so we do not leave the machine unable to idle.
[Win32Input]::KeepAwake($false)

Write-Host ""
Write-Host "=== ProcessEvent result ==="
Select-String -Path $snapshot -Pattern "PE candidate|CONFIRMED|not found in slots|game thread" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line }
