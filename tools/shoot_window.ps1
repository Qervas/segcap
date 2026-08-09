<#
.SYNOPSIS
  Screenshot a game window at intervals. No input, no injection.

.DESCRIPTION
  Exists because I was driving inZOI's menus blind -- A three times, then B on a
  timer -- and the game kept exiting around the third B press. That is not a bug
  to fix, it is a thing to LOOK AT: I do not know what those buttons do in this
  title, and pressing them harder will not find out.

  So: run the game, take pictures, read the menu. Every other blind-guess loop
  in this project cost multiple runs before someone pointed an instrument at it.

  Screen-region capture rather than PrintWindow: a D3D12 swapchain usually
  gives back a black frame to PrintWindow, whereas the desktop compositor has
  the real pixels for a borderless window.
#>
param(
    [string]$ProcessName = "inZOI-Win64-Shipping",
    [int]$EveryMs = 15000,
    [int]$Count = 14,
    [string]$OutDir = "build\bin\shots"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Shot {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
}
"@

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

for ($i = 0; $i -lt $Count; $i++) {
    $p = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { Write-Host "[shot] $ProcessName not running"; Start-Sleep -Milliseconds $EveryMs; continue }
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq 0) { Write-Host "[shot] no window yet"; Start-Sleep -Milliseconds $EveryMs; continue }

    $r = New-Object Shot+RECT
    [Shot]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -le 0 -or $ht -le 0) { Start-Sleep -Milliseconds $EveryMs; continue }

    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $g.Dispose()

    # Downscale: these are read to understand a menu, not to inspect pixels, and
    # a 2560x1600 PNG per shot is a lot of bytes for that.
    $sw = [int]($w / 2); $sh = [int]($ht / 2)
    $small = New-Object System.Drawing.Bitmap $sw, $sh
    $g2 = [System.Drawing.Graphics]::FromImage($small)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($bmp, 0, 0, $sw, $sh)
    $g2.Dispose(); $bmp.Dispose()

    $path = Join-Path $OutDir ("shot_{0:d3}.png" -f $i)
    $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose()
    Write-Host ("[shot] {0}  {1}x{2}" -f $path, $sw, $sh)
    Start-Sleep -Milliseconds $EveryMs
}
