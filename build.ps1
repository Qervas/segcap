<#
.SYNOPSIS
  Configure and build segcap.

.DESCRIPTION
  Uses the Ninja generator with the MSVC environment imported from vcvars64.bat.

  Why not a Visual Studio generator: only VS 18 is installed on this machine, and
  CMake 4.1 does not know that generator (it stops at "Visual Studio 17 2022").
  vswhere.exe is also absent, which is how CMake normally locates VS at all.
  Importing vcvars64.bat and driving Ninja sidesteps both problems and does not
  depend on CMake catching up to the installed toolset.

.PARAMETER Config
  Debug or Release. Default Release.

.PARAMETER Clean
  Delete the build directory first.
#>
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"

# --- locate vcvars64.bat -----------------------------------------------------
$candidates = @()
foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not $pf) { continue }
    $vsRoot = Join-Path $pf "Microsoft Visual Studio"
    if (-not (Test-Path $vsRoot)) { continue }
    $candidates += Get-ChildItem $vsRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
        ForEach-Object { Join-Path $_.FullName "VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ }
}

if (-not $candidates) {
    throw "vcvars64.bat not found. Is the MSVC C++ workload installed?"
}
# Newest install wins.
$vcvars = $candidates | Sort-Object | Select-Object -Last 1
Write-Host "[build] toolchain: $vcvars" -ForegroundColor Cyan

# --- import the MSVC environment --------------------------------------------
# vcvars64.bat only mutates the environment of the cmd that runs it, so dump the
# resulting environment and replay it into this session.
$imported = 0
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
        $imported++
    }
}
if ($imported -eq 0) { throw "failed to import environment from vcvars64.bat" }

$cl = (Get-Command cl.exe -ErrorAction SilentlyContinue)
if (-not $cl) { throw "cl.exe still not on PATH after importing vcvars" }
Write-Host "[build] cl.exe: $($cl.Source)" -ForegroundColor Cyan

# --- configure + build -------------------------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "[build] cleaning $buildDir" -ForegroundColor Yellow
    Remove-Item $buildDir -Recurse -Force
}

# CMake refuses to reconfigure a build directory with a different generator, and
# reports it as a bare "configure failed" that looks like a toolchain problem.
# Detect the mismatch and clear the directory rather than making the next person
# rediscover this.
$cache = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path $cache) {
    $cachedGen = (Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' |
        Select-Object -First 1).Matches.Groups[1].Value
    if ($cachedGen -and $cachedGen -ne "Ninja") {
        Write-Host "[build] cached generator '$cachedGen' != 'Ninja' -- clearing $buildDir" -ForegroundColor Yellow
        Remove-Item $buildDir -Recurse -Force
    }
}

# The quotes around -DCMAKE_BUILD_TYPE are load-bearing. PowerShell parses an
# unquoted native-command token that starts with '-' in parameter mode and does
# NOT interpolate variables inside it, so -DCMAKE_BUILD_TYPE=$Config would reach
# cmake as the literal string '$Config'. Ninja then fails much later with
# "expected newline, got lexing error", which points nowhere near the cause.
cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

Write-Host "[build] OK -> $buildDir\bin" -ForegroundColor Green
Get-ChildItem "$buildDir\bin" -ErrorAction SilentlyContinue |
    Select-Object Name, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB) } }
