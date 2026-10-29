# build.ps1 -- build this project without idf.py (same result as `idf.py build`)
#
# WHY THIS SCRIPT EXISTS
#   idf.py is a Python program that captures subprocess output via asyncio, and on
#   Windows asyncio implements that with *named pipes*. Some restricted/sandboxed
#   environments forbid creating named pipes, so idf.py dies with
#   PermissionError: [WinError 5] at create_subprocess_exec.
#   Calling CMake + Ninja directly is exactly what idf.py does internally and
#   produces identical binaries.
#
# NOTE: this file is ASCII-only on purpose. Windows PowerShell 5.1 decodes .ps1
#   files as ANSI (GBK on a Chinese system) unless they carry a UTF-8 BOM, which
#   corrupts non-ASCII comments and can break the script. Keep it ASCII.
#
# USAGE (Windows PowerShell blocks unsigned scripts, hence the bypass flag):
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean    # full rebuild
#
# OUTPUT: build\ESP32_Music_Device.elf / .bin / .map
# FLASH (from an ESP-IDF terminal):
#   idf.py -p COMx flash monitor

param(
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'

# ---- Tool paths: edit this block only when moving to another machine ----
$IdfPath      = 'D:\a2\Espressif\frameworks\esp-idf-v5.4.4'
$IdfTools     = 'D:\a2\Espressif'
$IdfPyEnv     = 'D:\a2\Espressif\python_env\idf5.4_py3.11_env'
$CmakeBin     = 'D:\a2\Espressif\tools\cmake\3.30.2\bin'
$NinjaBin     = 'D:\a2\Espressif\tools\ninja\1.12.1'
$ToolchainBin = 'D:\a2\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
$RomElfBin    = 'D:\a2\Espressif\tools\esp-rom-elfs\20241011'
$Target       = 'esp32s3'

# Use absolute paths so a stale PATH cannot pick the wrong tool.
$CmakeExe = Join-Path $CmakeBin 'cmake.exe'
$NinjaExe = Join-Path $NinjaBin 'ninja.exe'

foreach ($tool in @($CmakeExe, $NinjaExe)) {
    if (-not (Test-Path $tool)) {
        Write-Host "[build] tool not found: $tool" -ForegroundColor Red
        Write-Host "[build] edit the tool path block at the top of build.ps1" -ForegroundColor Red
        exit 1
    }
}

$ProjRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjRoot 'build'
$ToolchainFile = "$IdfPath/tools/cmake/toolchain-$Target.cmake"

# ---- Environment ----
$env:IDF_PATH            = $IdfPath
$env:IDF_TOOLS_PATH      = $IdfTools
$env:IDF_PYTHON_ENV_PATH = $IdfPyEnv
$env:IDF_TARGET          = $Target
$env:IDF_CCACHE_ENABLE   = '0'   # ccache off: its cache dir may be unwritable here
if (Test-Path $RomElfBin) { $env:ESP_ROM_ELF_DIR = $RomElfBin }

$env:PATH = "$CmakeBin;$NinjaBin;$ToolchainBin;$IdfPyEnv\Scripts;$env:PATH"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "[build] removing $BuildDir" -ForegroundColor Yellow
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

# ---- 1. Configure ----
Write-Host "[build] configuring (target=$Target, toolchain=$ToolchainFile) ..." -ForegroundColor Cyan
$cfgLog = Join-Path $ProjRoot 'configure.log'

# Build the argument list explicitly instead of relying on how the parser
# expands -DNAME=$var inside a native command invocation.
$cmakeArgs = @(
    '-G', 'Ninja',
    '-S', $ProjRoot,
    '-B', $BuildDir,
    ('-DIDF_TARGET={0}' -f $Target),
    ('-DCMAKE_TOOLCHAIN_FILE={0}' -f $ToolchainFile)
)

& $CmakeExe @cmakeArgs > $cfgLog 2>&1
$cfgCode = $LASTEXITCODE
if ($null -eq $cfgCode) { $cfgCode = 0 }

if ($cfgCode -ne 0) {
    Write-Host "[build] CONFIGURE FAILED (exit $cfgCode)" -ForegroundColor Red
    Select-String -Path $cfgLog -Pattern 'CMake Error|error:|Error while' |
        Select-Object -Last 40 | ForEach-Object { $_.Line }
    Write-Host "(full log: $cfgLog)"
    exit 1
}

# ---- 2. Compile ----
Write-Host "[build] compiling ..." -ForegroundColor Cyan
$buildLog = Join-Path $ProjRoot 'build.log'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $NinjaExe -C $BuildDir > $buildLog 2>&1
$code = $LASTEXITCODE
$sw.Stop()
if ($null -eq $code) { $code = 0 }

if ($code -ne 0) {
    Write-Host "[build] BUILD FAILED (exit $code, $([int]$sw.Elapsed.TotalSeconds)s)" -ForegroundColor Red
    Write-Host "---- errors ----" -ForegroundColor Red
    Select-String -Path $buildLog -Pattern '^FAILED:|error:|undefined reference' |
        ForEach-Object { $_.Line } | Sort-Object -Unique | Select-Object -First 60
    Write-Host "(full log: $buildLog)"
    exit $code
}

Write-Host "[build] OK in $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor Green

# ---- 3. Report artifacts and flash usage ----
$elf = Join-Path $BuildDir 'ESP32_Music_Device.elf'
$bin = Join-Path $BuildDir 'ESP32_Music_Device.bin'

if (Test-Path $bin) {
    $binSize = (Get-Item $bin).Length
    Write-Host ("[build] app binary : {0:N0} bytes ({1:N2} MB)" -f $binSize, ($binSize / 1MB))
}
if (Test-Path $elf) {
    Write-Host "[build] image size:"
    & (Join-Path $ToolchainBin 'xtensa-esp32s3-elf-size.exe') $elf
}

# ---- 4. Warnings from our own code only ----
$warn = Select-String -Path $buildLog -Pattern 'ESP32_Music_Device/User.*warning:|ESP32_Music_Device\\User.*warning:'
if ($warn) {
    Write-Host "---- warnings in project code ----" -ForegroundColor Yellow
    $warn | ForEach-Object { $_.Line } | Sort-Object -Unique | Select-Object -First 40
} else {
    Write-Host "[build] no warnings in project code" -ForegroundColor Green
}
