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
    [switch]$NoReadback,
    # Turn on the D3D12 validation layer. Slow, and for diagnosis only -- never
    # for a capture run. Needs the "Graphics Tools" optional Windows feature.
    [switch]$D3DDebug,
    # Drive the character during the capture hold. OFF by default: motion makes
    # UE stream, streaming churns the object graph, and that churn destroys the
    # marked slots the mask is built from. Static capture must work first.
    [switch]$Walk,
    # Probe scene-scale INTEGER render targets for per-object ids, testing each
    # against the stencil slots we actually leased.
    #
    # This is the Nanite route. On UE 5.6 / PC D3D12 the Nanite CustomDepth
    # export writes depth to CombinedCustomDepth and the stencil VALUE to a
    # separate CombinedCustomStencil of format PF_R16G16_UINT -- a COLOUR
    # target. On such a title the depth-stencil we elect never has its stencil
    # plane written, which is exactly what inZOI's masks showed.
    #
    # DEFAULT OFF. The probe copies from arbitrary integer render targets, and on
    # UE5 that is not yet safe: even with the "we have observed a transition"
    # guard satisfied, copying a 1280x800 R32_UINT killed inZOI at t=49s twice.
    # Observed-barriers turns out to be necessary but NOT sufficient, because the
    # state shadow updates when a barrier is RECORDED while the GPU executes it
    # later, and because a placed resource can inherit state records left by a
    # previous tenant of the same address.
    #
    # Copying safely needs the copy to be recorded into the GAME's own command
    # list at a point where its state is known exactly -- see HANDOFF.md. Until
    # then this stays opt-in for diagnosis, because it identifies the right buffer
    # and is the only thing that does.
    [switch]$IdBuf,
    # Observe render passes and barrier shapes around the id buffer and log what
    # WOULD be injected, recording nothing. Answers "does this title use
    # BeginRenderPass on this path" -- which matters because a CopyTextureRegion
    # recorded inside a render pass makes the runtime remove the command list.
    [switch]$InjectDry,
    # Record the copy into the game's own command list, taking StateBefore from
    # the game's own barrier instead of from our resource shadow. Implies -IdBuf,
    # since injection only ever targets the buffer the probe proved.
    [switch]$Inject
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
# One authoritative reset, then assert only what this run wants. See
# tools/reset_markers.ps1 for why the list does not live here.
& (Join-Path $root "tools\reset_markers.ps1") -Bin $bin
if ($NoMark) {
    Remove-Item (Join-Path $bin "segcap.mark") -ErrorAction SilentlyContinue
    Write-Host "[play] mark marker ABSENT -- read-only run"
} else {
    Set-Content -Path (Join-Path $bin "segcap.mark") -Value "1" -NoNewline
    Write-Host "[play] mark marker SET -- THIS RUN WILL WRITE bRenderCustomDepth"
}
$ddMarker = Join-Path $bin "segcap.d3ddebug"
if ($D3DDebug) {
    Set-Content -Path $ddMarker -Value "1" -NoNewline
    Write-Host "[play] D3D12 VALIDATION LAYER ON -- diagnosis run, expect it to be slow"
} else {
    Remove-Item $ddMarker -ErrorAction SilentlyContinue
}

if ($InjectDry) {
    Set-Content -Path (Join-Path $bin "segcap.injectdry") -Value "1" -NoNewline
    Write-Host "[play] INJECT DRY RUN -- observes render passes and barrier shapes only"
    $IdBuf = $true
} elseif ($Inject) {
    Set-Content -Path (Join-Path $bin "segcap.inject") -Value "1" -NoNewline
    Write-Host "[play] INJECT ARMED -- copies recorded into the game's own command lists"
    $IdBuf = $true
}

$idbMarker = Join-Path $bin "segcap.idbuf"
if ($IdBuf) {
    Set-Content -Path $idbMarker -Value "1" -NoNewline
    Write-Host "[play] ID-BUFFER PROBE on -- will walk scene-scale integer targets and test"
    Write-Host "       each against the slots we leased (the Nanite CombinedCustomStencil route)"
} else {
    Remove-Item $idbMarker -ErrorAction SilentlyContinue
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

# ---- arm the readback in GAMEPLAY, not on the first frame that qualifies ------
#
# The dump budget is a fixed number of frames, and without this it is spent on
# whatever renders first. A run that reached gameplay perfectly still produced 61
# masks of the MAIN MENU and zero of the world, because the menu satisfied the
# recording gate at t=49 and the budget was gone by t=65.
#
# So the readback is held disarmed until the script has actually clicked play,
# and armed from here rather than by hand. The script is the only thing that
# knows where in the route we are; asking a human to drop the file is exactly
# the manual step this harness exists to remove.
Remove-Item (Join-Path $bin "segcap.arm") -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $bin "segcap.requirearm") -Value "1" -NoNewline
Write-Host "[play] readback DISARMED until gameplay (budget reserved for the world)"

# -ErrorAction on the kill as well as the enumeration. A process can exit between
# Get-Process and Stop-Process -- most easily when the previous run's game is
# already on its way down -- and with $ErrorActionPreference = "Stop" the script
# then dies during CLEANUP, before it has launched anything. Losing a run to
# "cannot find a process with the process identifier" is absurd when the state we
# wanted (that process not running) is exactly what we got.
Get-Process -Name "inZOI*","vpad" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "[play] closing stale $($_.ProcessName) $($_.Id)"
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 3
# ARCHIVE FIRST, THEN DELETE. These two lines were the other way round, which
# meant the previous run's segcap.log was destroyed a moment before the archiver
# came to collect it -- so every crashed run left behind captures but no log, and
# the one artefact that explains a crash was the one artefact we always deleted.
# Found while diagnosing the t=254s inZOI crash, whose log survived only because
# it happened to still be sitting in build\bin when nothing had re-run since.
& (Join-Path $root "tools\archive_capture.ps1") -Title "inzoi" -Bin $bin
Remove-Item $log -ErrorAction SilentlyContinue

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

# --- the route, as fractions of the window ------------------------------------
# Measured on 2560x1600. act.ps1 is DPI-aware and takes physical pixels, so the
# fractions are resolved against the real window rect at click time.
# "The process exists" is NOT "the game is running".
#
# A -D3DDebug session hung inZOI during world streaming: the render thread stopped
# advancing while the process stayed alive at 12.9 GB resident. Every check here
# passed, so the script clicked transport-play on a frozen window and then drove a
# corpse with gamepad input for two more minutes before reporting 0 masks -- which
# reads exactly like a capture bug rather than the hang it was. Responding is the
# Win32 answer to "is this window's message loop still pumping".
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SegcapThread {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenThread(uint a, bool i, uint id);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint ResumeThread(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
}
'@ -ErrorAction SilentlyContinue

# Recover a WHOLE-PROCESS SUSPEND, which is not a hang and not a crash.
#
# Observed twice: the game stops dead mid-session, the log's last line is
# ordinary, and sampling shows every one of its ~147 threads in Wait/Suspended
# with a suspend count of exactly 1 while the process burns ZERO CPU. That is the
# signature of one NtSuspendProcess-style suspend, never resumed -- not a
# deadlock (which would hold locks, not suspend counts) and not a fatal error
# (no crash report, no CrashReportClient, no WER event).
#
# Ruled OUT as the suspender: MinHook (every MH_* call in segcap runs once at
# startup, none at the time of the freeze), the D3D12 debug layer (it reproduces
# with the layer off), Windows Error Reporting, and UE's own crash handler. WHAT
# suspends it is still unknown.
#
# Resuming by hand brought a suspended session back and it ran on for another 400
# seconds, so this is recoverable -- and a capture run that dies on it wastes
# twenty minutes of game time for a condition that costs milliseconds to undo.
# Recover, but say so LOUDLY: this is a workaround over an unexplained fault, and
# a silent one would let the fault disappear from the record.
function TryResumeSuspended($p) {
    $resumed = 0
    foreach ($t in $p.Threads) {
        $h = [SegcapThread]::OpenThread(0x0002, $false, [uint32]$t.Id)   # THREAD_SUSPEND_RESUME
        if ($h -eq [IntPtr]::Zero) { continue }
        $prev = [SegcapThread]::ResumeThread($h)
        if ($prev -ne [uint32]"0xFFFFFFFF" -and $prev -gt 0) { $resumed++ }
        [void][SegcapThread]::CloseHandle($h)
    }
    return $resumed
}

function GameLive([string]$what) {
    $p = Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw "game exited before: $what" }
    if ($p.Responding) { return $p }

    # Not responding. Distinguish a load hitch from a suspend by watching CPU:
    # a streaming stall burns cycles, a suspended process burns none.
    $cpu0 = $p.CPU
    Start-Sleep -Seconds 3
    $p.Refresh()
    if ($p.Responding) { return $p }
    $moved = ($p.CPU - $cpu0) -gt 0.5

    # BUSY IS NOT DEAD. A game streaming a 35 GB save stops pumping its message
    # loop for long stretches while burning CPU the whole time -- Responding is
    # false and nothing is wrong. This function exists to catch the case where the
    # process is NOT working, and the CPU delta is exactly that discriminator, so
    # a thread that is advancing gets left alone. The first version threw here
    # anyway and killed a run during world load, which is the one phase where a
    # multi-second stall is the expected behaviour rather than a fault.
    if ($moved) { return $p }

    $susp = @($p.Threads | Where-Object { $_.ThreadState -eq 'Wait' -and $_.WaitReason -eq 'Suspended' }).Count
    if ($susp -gt ($p.Threads.Count / 2)) {
        Write-Host "[play] !! GAME SUSPENDED before: $what -- $susp of $($p.Threads.Count) threads suspended, 0 CPU. UNEXPLAINED FAULT; resuming them."
        $n = TryResumeSuspended $p
        Start-Sleep -Seconds 5
        $p.Refresh()
        Write-Host "[play] !! resumed $n thread(s); Responding=$($p.Responding)"
        if ($p.Responding) { return $p }
        throw "game stayed frozen after resuming $n thread(s) before: $what"
    }

    # Flat CPU for one 3-second sample is not proof either: a game streaming from
    # disk blocks on I/O, which consumes no CPU and pumps no messages. Watch for
    # considerably longer before calling it dead, and let any sign of life win.
    for ($i = 0; $i -lt 6; $i++) {
        $cpuN = $p.CPU
        Start-Sleep -Seconds 5
        $p.Refresh()
        if ($p.Responding) { return $p }
        if (($p.CPU - $cpuN) -gt 0.5) {
            Write-Host "[play] (not responding but working -- CPU advancing during: $what)"
            return $p
        }
    }
    throw "game is FROZEN (not responding) before: $what -- pid $($p.Id) is alive but its message loop has stopped and it consumed no CPU for 30s, with $susp/$($p.Threads.Count) threads suspended. Check the log's LAST timestamp against the wall clock."
}

function Click([double]$fx, [double]$fy, [double]$wait, [string]$what) {
    GameLive $what | Out-Null
    Write-Host "[play] $what"
    & $act -ClickFx $fx -ClickFy $fy -Wait $wait | ForEach-Object { Write-Host "       $_" }
}

# WAIT FOR THE SIGNAL, NOT FOR THE CLOCK.
#
# MenuWait and LoadWait were fixed Start-Sleeps of 150s and 180s -- 5.5 minutes of
# blind waiting before a run could fail, on every iteration, chosen once to be
# safely longer than the worst case. The DLL already logs exactly when the engine
# is up and when the world settles, so the schedule can be driven by the game
# instead of guessed at. These keep the old fixed values as CEILINGS, so the worst
# case is unchanged and only the common case gets faster.
function Get-LogTail([int]$n = 400) {
    if (-not (Test-Path $log)) { return @() }
    try { return @(Get-Content $log -Tail $n -ErrorAction Stop) } catch { return @() }
}

# Wait until every one of $Patterns has appeared in the log, or $TimeoutSec.
# Returns $true if the signal arrived, $false if it timed out (caller decides
# whether that is fatal -- here it never is, we just fall through to the ceiling).
function Wait-ForLog([string[]]$Patterns, [int]$TimeoutSec, [string]$What, [int]$FloorSec = 0) {
    $t0 = Get-Date
    Write-Host "[play] waiting for $What (ceiling ${TimeoutSec}s)"
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        GameLive $What | Out-Null
        $tail = Get-LogTail 1200
        $all = $true
        foreach ($p in $Patterns) {
            if (-not ($tail | Where-Object { $_ -match $p })) { $all = $false; break }
        }
        if ($all) {
            $waited = [math]::Round(((Get-Date) - $t0).TotalSeconds)
            if ($waited -lt $FloorSec) {
                Start-Sleep -Seconds ($FloorSec - $waited)
                $waited = $FloorSec
            }
            Write-Host "[play] $What after ${waited}s (ceiling was ${TimeoutSec}s)"
            return $true
        }
        Start-Sleep -Seconds 2
    }
    Write-Host "[play] $What NOT observed within ${TimeoutSec}s -- proceeding on the ceiling"
    return $false
}

# The world is "settled" when the DLL stops reporting object-count churn. UE
# streams a save in over many seconds and segcap logs `world is changing (A -> B
# objects)` each time the count jumps; when that goes quiet, streaming is done.
# This is the signal the fixed 180s was standing in for.
function Wait-ForWorldSettled([int]$TimeoutSec, [int]$QuietSec = 8) {
    $t0 = Get-Date
    Write-Host "[play] waiting for the world to settle (quiet ${QuietSec}s, ceiling ${TimeoutSec}s)"
    $lastChange = Get-Date
    $sawAny = $false
    $seen = ""
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        GameLive "world settle" | Out-Null
        $tail = Get-LogTail 600
        $chg = @($tail | Where-Object { $_ -match 'world is changing' }) | Select-Object -Last 1
        if ($chg -and $chg -ne $seen) { $seen = $chg; $lastChange = Get-Date; $sawAny = $true }
        if ($sawAny -and ((Get-Date) - $lastChange).TotalSeconds -ge $QuietSec) {
            Write-Host "[play] world settled after $([math]::Round(((Get-Date)-$t0).TotalSeconds))s"
            return $true
        }
        Start-Sleep -Seconds 2
    }
    Write-Host "[play] world-settled signal not seen within ${TimeoutSec}s -- proceeding"
    return $false
}

# "The engine is up" is observable, not a duration. ProcessEvent CONFIRMED means
# the UObject world is reachable and validated; the first `customdepth: marked`
# means components are being enumerated and rendered. Both together mean the menu
# is live. On every run logged so far these land at t=40 and t=46 -- against a
# fixed wait of 150s. The floor keeps a margin for the menu's own fade-in.
#
# Placed HERE, below the function definitions, not up beside the process launch:
# PowerShell executes a script top-to-bottom, so a call above the `function`
# keyword that defines it fails with "not recognized as a name of a cmdlet" --
# which is exactly how the first version of this died, 20 seconds into a run.
# CLICK AS SOON AS THE GAME IS DRAWING, AND RETRY UNTIL IT TAKES.
#
# This used to wait for `ProcessEvent CONFIRMED` and the first `customdepth:
# marked`. Both are ENGINE-INTROSPECTION milestones -- they say our UObject
# plumbing is ready, which has nothing whatever to do with whether the main menu
# is on screen and clickable. Gating navigation on them meant every run paid for
# work it did not need yet, and each time that work got faster the gate was
# re-tuned instead of removed.
#
# The only precondition for clicking is that the game is rendering. The Present
# hook's own census line proves that and lands within a few seconds.
Wait-ForLog @('distinct targets observed') $MenuWait "the game to start rendering" -FloorSec 6 | Out-Null

# Then probe rather than assume. A click on a still-loading screen does nothing,
# so the cost of being early is one wasted click -- while the cost of being late
# is the minutes this script has been burning. Repeat the pair until the log
# shows the save actually loading. The loop is self-correcting: if a click lands
# on the wrong screen the next iteration starts from the main menu again.
$entered = $false
for ($try = 1; $try -le 6 -and -not $entered; $try++) {
    Click 0.0883 0.2169 2  "Continue (attempt $try)"
    Click 0.6926 0.3631 3  "first save slot (attempt $try)"
    $entered = Wait-ForLog @('world is changing') 25 "the save to begin loading"
}
if (-not $entered) {
    Write-Host "[play] no load signal after $($try - 1) attempts -- continuing anyway"
}

Wait-ForWorldSettled $LoadWait 8 | Out-Null

# The sim loads PAUSED. Clicking play is what starts time; the clock in the
# bottom-left advancing is how you know it worked.
Click 0.0660 0.9606 5  "transport play (unpause)"

# Let marking catch up before spending the dump budget. The mark loop needs the
# world to be stable and the field layout calibrated, and neither is true in the
# first seconds after a save load -- marking reported "stale 300 of 300" for a
# long stretch while the previous world's components were still being released.
# Arm on evidence that marking has caught up, not on a stopwatch. A run that
# arms early spends its capture budget on a world still being released.
Wait-ForWorldSettled 60 6 | Out-Null
Start-Sleep -Seconds 5
Set-Content -Path (Join-Path $bin "segcap.arm") -Value "1" -NoNewline
Write-Host "[play] ARMED -- every captured frame from here is gameplay"

Write-Host "[play] in gameplay; holding for ${Seconds}s while segcap marks"
$deadline = (Get-Date).AddSeconds($Seconds)
$step = 0
while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Name "inZOI-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) {
        Write-Host "[play] GAME EXITED at step $step -- see the log tail below"
        break
    }
    # Frozen counts as over. Continuing would spend the rest of the hold walking
    # a window that cannot move, and bank the result as a capture failure.
    if (-not $p.Responding) {
        Write-Host "[play] GAME FROZEN at step $step (pid $($p.Id) alive, not responding) -- stopping the hold"
        break
    }
    # STAND STILL unless -Walk is asked for.
    #
    # This used to drive the left stick full forward for 2.5s then full back for
    # 2.5s, forever, to "keep the scene changing". Three things wrong with that,
    # and the third is why it is now off by default:
    #
    #   1. Net displacement is about zero -- it shuffles between two viewpoints
    #      rather than touring anything, so it barely served its own purpose.
    #   2. It hardcodes what the left stick does in a life sim we never probed.
    #   3. It FIGHTS THE CAPTURE. Moving makes UE stream, streaming makes the
    #      object count churn, churn makes the marker pause and drop slots, and
    #      slot churn is what repeatedly pulled the id buffer out from under the
    #      probe. We were destabilising the exact state we were trying to read.
    #
    # Static capture has to work before dynamic capture means anything: if a
    # standing-still frame cannot be captured, a moving one certainly cannot.
    # Hold the world still, get masks, and only then reintroduce motion.
    if ($Walk) {
        $ly = if ($step % 2 -eq 0) { 30000 } else { -30000 }
        & $act -Ly $ly -Ms 2500 -Wait 0.5 | Out-Null
    } else {
        Start-Sleep -Seconds 3
    }
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
