# =============================================================================
#  LED pattern engine offline test -- compile and run on the PC.
#
#  Usage (from the ESP32_Music_Device directory):
#      powershell -ExecutionPolicy Bypass -File .\tools\run_led_test.ps1
#      powershell -ExecutionPolicy Bypass -File .\tools\run_led_test.ps1 -Rebuild
#
#  WHY THIS EXISTS
#    There is no way to tell, from the ESP32 alone, whether "the LED does not
#    blink" is caused by the caller, the pattern engine, or the GPIO layer.
#    This harness links the REAL app_led.c and stubs only the BSP/FreeRTOS
#    layer, so the pattern engine runs verbatim and every GPIO write is
#    recorded. Toggle counts are then asserted.
#
#  WHAT IT CAUGHT (do not delete this test)
#    app_led_advance_locked() used to compute the target level AND store it
#    into inst->level, after which App_LED_Service called app_led_apply_locked()
#    which compared "on == inst->level" and therefore NEVER wrote the GPIO.
#    Result: PAT_ON and PAT_OFF worked, but EVERY blinking pattern was frozen.
#    On hardware that showed up as "WiFi drops, state machine switches to
#    RETRY_WAIT, but the LED just stays solid".
#
#  ENCODING: ASCII-ONLY BY DESIGN (see run_ks_test.ps1 header for why).
# =============================================================================

param(
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$TestSrc   = Join-Path $Root 'tools\led_test.c'
$LedSrc    = Join-Path $Root 'User\APP\APP_LED\app_led.c'
$StubDir   = Join-Path $Root 'tools\stub'
$CommonDir = Join-Path $Root 'User\Common'
$LedDir    = Join-Path $Root 'User\APP\APP_LED'
$GpioDir   = Join-Path $Root 'User\BSP\BSP_GPIO'
$OutDir    = Join-Path $Root 'tools\build'
$Exe       = Join-Path $OutDir 'led_test.exe'

Write-Host "[led-test] root = $Root"

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host "[led-test] ERROR: gcc not found in PATH" -ForegroundColor Red
    exit 1
}

foreach ($f in @($TestSrc, $LedSrc)) {
    if (-not (Test-Path $f)) {
        Write-Host "[led-test] ERROR: missing source: $f" -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

$gccArgs = @(
    '-std=gnu11',
    '-O2', '-g', '-Wall', '-Wextra',
    "-I$StubDir", "-I$CommonDir", "-I$LedDir", "-I$GpioDir",
    $TestSrc, $LedSrc,
    '-o', $Exe,
    '-lm'
)

$needBuild = $Rebuild -or (-not (Test-Path $Exe))
if (-not $needBuild) {
    $exeTime = (Get-Item $Exe).LastWriteTime
    foreach ($f in @($TestSrc, $LedSrc)) {
        if ((Get-Item $f).LastWriteTime -gt $exeTime) { $needBuild = $true; break }
    }
}

if ($needBuild) {
    Write-Host "[led-test] compiling ..."
    $compileOut = & gcc @gccArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[led-test] COMPILE FAILED" -ForegroundColor Red
        $compileOut | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    if ($compileOut) {
        Write-Host "[led-test] compiler warnings:" -ForegroundColor Yellow
        $compileOut | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host "[led-test] compile OK"
} else {
    Write-Host "[led-test] up to date (use -Rebuild to force)"
}

Write-Host ""
& $Exe
$code = $LASTEXITCODE

Write-Host ""
if ($code -eq 0) {
    Write-Host "[led-test] ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "[led-test] TESTS FAILED (exit $code)" -ForegroundColor Red
}
exit $code
