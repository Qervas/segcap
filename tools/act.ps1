<#
.SYNOPSIS
  Send ONE gamepad input to the game and screenshot the result.

.DESCRIPTION
  The interactive primitive: act, then look. This is what "don't hardcode the
  operation" actually needs -- a way to try one thing and see what it did,
  rather than writing a sequence and hoping.

  It focuses the game first, every time. Without that the input goes to
  whatever has the foreground and the screenshot shows that instead, which has
  already produced a full page of confident nonsense once in this project.

  Requires `vpad --serve <cmdfile>` to be running (tools/serve_pad.ps1).

.EXAMPLE
  .\tools\act.ps1 -Btn DD                 # tap d-pad down
  .\tools\act.ps1 -Btn A -Ms 150          # tap A
  .\tools\act.ps1 -Ly 26000 -Ms 2000      # hold left stick forward 2s
  .\tools\act.ps1 -Wait 3                 # no input, just look after 3s
#>
param(
    [string]$Process = "inZOI-Win64-Shipping",
    [int]$Lx = 0, [int]$Ly = 0, [int]$Rx = 0, [int]$Ry = 0,
    [int]$Lt = 0, [int]$Rt = 0,
    [string]$Btn = "",
    [int]$Ms = 250,
    [double]$Wait = 1.5,
    [string]$Cmd = "build\bin\vpad_cmd.txt",
    [string]$Out = "build\bin\act.png",
    [double]$Scale = 0.62,
    # Click at this point in the window, physical pixels from its top-left.
    # Send a keystroke to the game. inZOI's transport responds to the number
    # keys -- "1" resumes at normal speed -- which is far more reliable than
    # clicking a transport button, because a key needs no coordinate and cannot
    # land on the neighbouring control. Our click route was hitting pause instead
    # of play, so every "unpaused" capture was actually of a frozen sim.
    [string]$Key = "",
    [int]$KeyHoldMs = 90,
    [switch]$KeyLegacy,
    [int]$ClickX = -1,
    # Same click, as a fraction of the window. Prefer these: pixel coordinates
    # measured on one window size click empty space on any other.
    [double]$ClickFx = -1,
    [double]$ClickFy = -1,
    [int]$ClickY = -1
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("Act" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class Act {
  // DPI AWARENESS IS NOT OPTIONAL HERE.
  //
  // On a scaled display, a DPI-unaware process is handed VIRTUALISED
  // coordinates: GetWindowRect reports the window smaller than it really is,
  // and CopyFromScreen then captures a cropped region of it. The screenshot
  // still looks like a perfectly good screenshot -- it is simply missing the
  // edges.
  //
  // That cost real time. Every capture of inZOI was silently losing the bottom
  // strip of its HUD, which is exactly where the game-speed controls live, and
  // I concluded from those pictures that the game had no time controls. A
  // 1463x914 capture of a 1990x1244 window: the same 1.36x the display is
  // scaled by.
  //
  // Must be called before any window or graphics call in the process.
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a,uint b,IntPtr c,uint d);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,IntPtr e);

  // Click at a point inside the window, in PHYSICAL pixels relative to its
  // top-left. inZOI is a life sim with a mouse-first HUD -- several of its
  // controls (the game-speed transport bar among them) are not reachable from
  // the gamepad at all, because the d-pad enters a widget-navigation mode
  // instead of operating them.
  public static void ClickAt(IntPtr h, int rx, int ry){
    RECT r; if(!GetWindowRect(h,out r)) return;
    SetCursorPos(r.Left+rx, r.Top+ry);
    System.Threading.Thread.Sleep(120);
    mouse_event(0x0002,0,0,0,IntPtr.Zero);
    System.Threading.Thread.Sleep(50);
    mouse_event(0x0004,0,0,0,IntPtr.Zero);
  }

  public static uint Fg(){ IntPtr f=GetForegroundWindow(); if(f==IntPtr.Zero) return 0;
    uint p; GetWindowThreadProcessId(f,out p); return p; }
  public static bool Focus(IntPtr h,uint pid){ if(Fg()==pid) return true;
    SystemParametersInfoW(0x2001,0,IntPtr.Zero,0x02);
    uint ft=GetWindowThreadProcessId(GetForegroundWindow(),IntPtr.Zero), mt=GetCurrentThreadId();
    bool at=(ft!=0&&ft!=mt)&&AttachThreadInput(mt,ft,true);
    ShowWindow(h,9); BringWindowToTop(h); SetForegroundWindow(h);
    if(at) AttachThreadInput(mt,ft,false); return Fg()==pid; }
}
"@
}

[Act]::SetProcessDPIAware() | Out-Null

# Keystrokes go through keybd_event with KEYEVENTF_SCANCODE, not SendKeys.
# Games commonly read raw scan codes and ignore the synthesised virtual-key
# messages SendKeys produces, so SendKeys looks like it worked and does nothing.
if (-not ("Keys1" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit, Size=40)]
public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
public class Keys1 {
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint mapType);

  // SendInput, not keybd_event.
  //
  // keybd_event stamps its events LLKHF_INJECTED, and games reading through Raw
  // Input or DirectInput routinely discard those -- which is exactly what inZOI
  // did: the log said "key: sent '1'", focus had succeeded, and the simulation
  // stayed paused for 176 seconds (object delta +0 the whole time). SendInput
  // with a hardware scan code is the closest thing to a real keypress that user
  // mode can produce.
  public static void Tap(ushort vk, int holdMs){
    ushort sc = (ushort)MapVirtualKeyW(vk, 0);
    INPUT[] down = new INPUT[1];
    down[0].type = 1;
    down[0].ki.wVk = 0; down[0].ki.wScan = sc; down[0].ki.dwFlags = 0x0008; // SCANCODE
    INPUT[] up = new INPUT[1];
    up[0].type = 1;
    up[0].ki.wVk = 0; up[0].ki.wScan = sc; up[0].ki.dwFlags = 0x0008 | 0x0002; // +KEYUP
    SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
    System.Threading.Thread.Sleep(holdMs);
    SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
  }
  // Legacy path kept as a fallback: if a title filters SendInput instead, the
  // two APIs fail in opposite directions and trying both costs milliseconds.
  public static void TapLegacy(byte vk, int holdMs){
    byte sc = (byte)MapVirtualKeyW(vk, 0);
    keybd_event(vk, sc, 0x0008, IntPtr.Zero);
    System.Threading.Thread.Sleep(holdMs);
    keybd_event(vk, sc, 0x0008 | 0x0002, IntPtr.Zero);
  }
}
"@
}

$g = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $g) { throw "$Process is not running" }

$focused = $false
for ($i = 0; $i -lt 20; $i++) {
    if ([Act]::Focus($g.MainWindowHandle, [uint32]$g.Id)) { $focused = $true; break }
    Start-Sleep -Milliseconds 300
}
if (-not $focused) {
    # Say it rather than carry on. An input sent to an unfocused window goes
    # nowhere and the screenshot would show a state the input never reached.
    Write-Host "WARNING: could not focus $Process -- input will go elsewhere"
}

# Fractional click. Resolved here, inside the one process that is already
# DPI-aware and already holds the window handle.
#
# The first version of this lived in the caller and shelled out to a nested
# `powershell -Command` to fetch the window rect. The escaping broke, the query
# returned 0x0, and every click in the sequence landed on (0,0) -- while the
# script cheerfully printed a click for each step. Coordinates measured on one
# window size are not portable, but neither is a rect fetched by a subshell
# that can fail silently.
# Keys go after the focus loop above, so the game is foreground when they land.
if ($Key) {
    $vk = if ($Key -match '^[0-9]$') { [byte][char]$Key }
          elseif ($Key -match '^[a-zA-Z]$') { [byte][char]([string]$Key).ToUpper() }
          else { 0 }
    if ($vk -eq 0) {
        Write-Host "key: '$Key' not supported (use a single digit or letter)"
    } else {
        if ($KeyLegacy) { [Keys1]::TapLegacy([byte]$vk, $KeyHoldMs) }
        else { [Keys1]::Tap([uint16]$vk, $KeyHoldMs) }
        Write-Host "key: sent '$Key' (vk 0x$('{0:X2}' -f $vk)) hold=${KeyHoldMs}ms via $(if($KeyLegacy){'keybd_event'}else{'SendInput'}) to pid $($g.Id)"
    }
}

if ($ClickFx -ge 0 -and $ClickFy -ge 0) {
    $wr = New-Object Act+RECT
    if (-not [Act]::GetWindowRect($g.MainWindowHandle, [ref]$wr)) {
        throw "GetWindowRect failed; refusing to click at a guessed position"
    }
    $ww = $wr.Right - $wr.Left
    $wh = $wr.Bottom - $wr.Top
    if ($ww -le 0 -or $wh -le 0) {
        throw "window measured ${ww}x${wh}; refusing to click at a guessed position"
    }
    $ClickX = [int]($ww * $ClickFx)
    $ClickY = [int]($wh * $ClickFy)
    Write-Host "fractional click ($ClickFx, $ClickFy) of ${ww}x${wh}"
}

if ($ClickX -ge 0 -and $ClickY -ge 0) {
    [Act]::ClickAt($g.MainWindowHandle, $ClickX, $ClickY)
    Write-Host "clicked at window-relative ($ClickX, $ClickY)"
}

# Send, unless this is a look-only call.
if ($Btn -or $Lx -or $Ly -or $Rx -or $Ry -or $Lt -or $Rt) {
    $ackPath = "$Cmd.ack"
    $seq = 1
    if (Test-Path $ackPath) { $seq = [int]((Get-Content $ackPath -Raw).Trim()) + 1 }
    $line = "seq=$seq lx=$Lx ly=$Ly rx=$Rx ry=$Ry lt=$Lt rt=$Rt btn=$(if($Btn){$Btn}else{'-'}) ms=$Ms"
    # Write-then-rename so the server never reads a half-written line.
    # Move-Item -Force still throws if the destination exists on some hosts;
    # File.Move with overwrite:true is the one that actually replaces.
    Set-Content -Path "$Cmd.tmp" -Value $line -NoNewline
    [System.IO.File]::Move((Resolve-Path "$Cmd.tmp").Path, (Join-Path (Get-Location) $Cmd), $true)
    Write-Host "sent: $line"

    $acked = $false
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $ackPath) {
            $v = 0; [int]::TryParse((Get-Content $ackPath -Raw).Trim(), [ref]$v) | Out-Null
            if ($v -ge $seq) { $acked = $true; break }
        }
        Start-Sleep -Milliseconds 60
    }
    if (-not $acked) { Write-Host "WARNING: vpad did not ack -- is 'vpad --serve' running?" }
} else {
    Write-Host "(no input; observing only)"
}

Start-Sleep -Seconds $Wait

$r = New-Object Act+RECT
[Act]::GetWindowRect($g.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top

# A window can legitimately measure 0x0 for a moment -- inZOI's handle goes
# invalid while a save loads and the swapchain is rebuilt. Constructing a
# Bitmap with those dimensions throws "Parameter is not valid", which killed
# the whole driving script mid-sequence and left the game running unattended.
# The screenshot is diagnostic; failing to take one is not worth aborting for.
if ($w -le 0 -or $h -le 0) {
    Write-Host "shot: SKIPPED -- window measured ${w}x${h} (loading or minimised?)"
    return
}

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gr = [System.Drawing.Graphics]::FromImage($bmp)
$gr.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$gr.Dispose()
# Downscale only enough to keep the file small; -Scale 1 for full detail when
# reading small HUD text.
$sw = [int]($w * $Scale); $sh = [int]($h * $Scale)
$small = New-Object System.Drawing.Bitmap $sw, $sh
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($bmp, 0, 0, $sw, $sh)
$g2.Dispose(); $bmp.Dispose()
# Anchor $Out to the REPO, not to the working directory.
#
# Bitmap.Save is a .NET call, and .NET resolves a relative path against
# [Environment]::CurrentDirectory -- which PowerShell's Set-Location does NOT
# update. So `cd` into the repo, run this, and New-Item happily creates
# build\bin relative to $PWD while Save looks for it somewhere else entirely and
# reports "the directory build\bin does not exist" about a directory that plainly
# does. It worked for weeks because the shell happened to START in the repo root;
# it failed 150 seconds into an inZOI diagnosis run when that stopped being true,
# and took the run with it. Resolving against the script's own location makes the
# output path independent of who called us and from where.
if (-not [System.IO.Path]::IsPathRooted($Out)) {
    $repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Out  = Join-Path $repo $Out
}
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose()
Write-Host "shot: $Out  window=${w}x${h} saved=${sw}x${sh}  foreground=$([Act]::Fg()) game=$($g.Id)"
