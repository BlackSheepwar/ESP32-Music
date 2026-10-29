# =============================================================================
#  KS synth offline numeric test -- compile and run on the PC.
#
#  Usage (from the ESP32_Music_Device directory):
#      powershell -ExecutionPolicy Bypass -File .\tools\run_ks_test.ps1
#      powershell -ExecutionPolicy Bypass -File .\tools\run_ks_test.ps1 -Verbose
#      powershell -ExecutionPolicy Bypass -File .\tools\run_ks_test.ps1 -Rebuild
#
#  WHY THIS EXISTS
#    The speaker has not arrived yet, so the synth cannot be judged by ear.
#    Meanwhile the synth is the hottest and riskiest code we changed. This
#    script compiles ks_synth.c verbatim on the PC (same -O3 -ffast-math as the
#    firmware component) and asserts numeric properties instead of listening:
#      - all 6 instruments must produce sound
#        (used to fail: harp pick_position=0.50 silenced the WHOLE song)
#      - voices must be reclaimed
#        (used to fail: amp never decayed -> pool exhausted -> steal clicks)
#      - decay_seconds must scale voice lifetime proportionally
#      - strum=1 and strum=0 must produce different output
#        (used to fail: strum always parsed as 0, so strumming was dead code)
#      - pathological chord_note_counts must not go out of bounds
#        (used to be able to crash with a +/-2MB offset)
#
#  ENCODING: this file is ASCII-ONLY BY DESIGN.
#    Windows PowerShell 5.1 decodes a .ps1 without a BOM as ANSI, which turns
#    any non-ASCII byte (including an em-dash) into mojibake and breaks parsing.
#    Keep every byte in this file < 0x80.
# =============================================================================

param(
    [switch]$Verbose,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$TestSrc   = Join-Path $Root 'tools\ks_test.c'
$KsSrc     = Join-Path $Root 'User\Middlewares\KS\ks_synth.c'
$StubDir   = Join-Path $Root 'tools\stub'
$CommonDir = Join-Path $Root 'User\Common'
$KsDir     = Join-Path $Root 'User\Middlewares\KS'
$OutDir    = Join-Path $Root 'tools\build'
$Exe       = Join-Path $OutDir 'ks_test.exe'

Write-Host "[ks-test] root   = $Root"

# ---- locate gcc ----
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host "[ks-test] ERROR: gcc not found in PATH" -ForegroundColor Red
    Write-Host "[ks-test]        install MinGW-w64 or add it to PATH"
    exit 1
}
Write-Host "[ks-test] gcc    = $($gcc.Source)"

# ---- check sources ----
foreach ($f in @($TestSrc, $KsSrc)) {
    if (-not (Test-Path $f)) {
        Write-Host "[ks-test] ERROR: missing source: $f" -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

# ---- compile ----
# -O3 -ffast-math matches the Middlewares component_compile_options so the
# floating point behaviour here resembles the device.
$gccArgs = @(
    '-std=gnu11',
    '-O3', '-ffast-math', '-g',
    '-Wall', '-Wextra',
    "-I$StubDir", "-I$CommonDir", "-I$KsDir",
    $TestSrc, $KsSrc,
    '-o', $Exe,
    '-lm'
)

$needBuild = $Rebuild -or (-not (Test-Path $Exe))
if (-not $needBuild) {
    $exeTime = (Get-Item $Exe).LastWriteTime
    foreach ($f in @($TestSrc, $KsSrc)) {
        if ((Get-Item $f).LastWriteTime -gt $exeTime) { $needBuild = $true; break }
    }
}

if ($needBuild) {
    Write-Host "[ks-test] compiling ..."
    $compileOut = & gcc @gccArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ks-test] COMPILE FAILED" -ForegroundColor Red
        $compileOut | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    if ($compileOut) {
        Write-Host "[ks-test] compiler warnings:" -ForegroundColor Yellow
        $compileOut | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host "[ks-test] compile OK"
} else {
    Write-Host "[ks-test] up to date (use -Rebuild to force)"
}

# ---- run ----
Write-Host ""
if ($Verbose) { & $Exe -v } else { & $Exe }
$code = $LASTEXITCODE

Write-Host ""
if ($code -eq 0) {
    Write-Host "[ks-test] ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "[ks-test] TESTS FAILED (exit $code)" -ForegroundColor Red
}
exit $code
