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
    [string]$Out = "build\bin\act.png"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("Act" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class Act {
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

# Send, unless this is a look-only call.
if ($Btn -or $Lx -or $Ly -or $Rx -or $Ry -or $Lt -or $Rt) {
    $ackPath = "$Cmd.ack"
    $seq = 1
    if (Test-Path $ackPath) { $seq = [int]((Get-Content $ackPath -Raw).Trim()) + 1 }
    $line = "seq=$seq lx=$Lx ly=$Ly rx=$Rx ry=$Ry lt=$Lt rt=$Rt btn=$(if($Btn){$Btn}else{'-'}) ms=$Ms"
    Set-Content -Path "$Cmd.tmp" -Value $line -NoNewline
    Move-Item "$Cmd.tmp" $Cmd -Force
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
$bmp = New-Object System.Drawing.Bitmap $w, $h
$gr = [System.Drawing.Graphics]::FromImage($bmp)
$gr.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$gr.Dispose()
$sw = [int]($w / 2); $sh = [int]($h / 2)
$small = New-Object System.Drawing.Bitmap $sw, $sh
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($bmp, 0, 0, $sw, $sh)
$g2.Dispose(); $bmp.Dispose()
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose()
Write-Host "shot: $Out  (${sw}x${sh})  foreground=$([Act]::Fg()) game=$($g.Id)"
