# .skills/debug.ps1 -- OpenOCD + GDB session for ESP32-S3 (built-in USB-JTAG)
# Ported from embedspec/.skills/debug.sh (2026-10-19), rewritten for ESP-IDF / xtensa.
#
# Differences vs the STM32 original:
#   - arm-none-eabi-gdb   -> xtensa-esp32s3-elf-gdb
#   - openocd -f openocd.cfg (ST-Link SWD) -> idf.py openocd (on-chip USB-JTAG)
#   - `make` build        -> idf.py build
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .skills\debug.ps1
#   powershell -ExecutionPolicy Bypass -File .skills\debug.ps1 -Break main
#
# ASCII-only on purpose (PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [string]$BreakAt = "app_main"
)

$ErrorActionPreference = 'Continue'

$env:IDF_PATH             = 'C:\Espressif\v5.4\esp-idf'
$env:IDF_TOOLS_PATH       = 'C:\Users\wumu2\.espressif'
$tc                       = "$env:IDF_TOOLS_PATH\tools"
$pyDir                    = "$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env\Scripts"
$env:IDF_PYTHON_ENV_PATH  = "$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env"
$env:CMAKE_BUILD_PARALLEL_LEVEL = '4'
$env:PATH = "$tc\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;" +
            "$tc\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;" +
            "$tc\cmake\3.30.2\bin;$tc\ninja\1.12.1;" +
            "$tc\ccache\4.10.2\ccache\bin;$tc\esp-rom-elfs\20241011;" +
            "$pyDir;$env:PATH"

$py   = "$pyDir\python.exe"
$idf  = "$env:IDF_PATH\tools\idf.py"
$gdb  = "$tc\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gdb.exe"
$elf  = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\esp32_modbus_gateway.elf'
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj

Write-Host "=== ESP32-S3 gateway: OpenOCD + GDB ===" -ForegroundColor Cyan
Write-Host "  gdb  : $gdb"
Write-Host "  elf  : $elf"
Write-Host "  break: $BreakAt"

if ($env:MSYSTEM) {
    Write-Host "ABORT: MSYSTEM=$env:MSYSTEM is set -> idf.py would fake-pass." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $gdb)) { Write-Host "ABORT: xtensa gdb not found." -ForegroundColor Red; exit 1 }
if (-not (Test-Path $elf)) { Write-Host "ABORT: elf not built. Run .skills\flash.ps1 first." -ForegroundColor Red; exit 1 }

# ---- 1) start OpenOCD in background (idf.py openocd) ----
Write-Host "`n[1/3] starting OpenOCD (localhost:3333) ..." -ForegroundColor Yellow
$ocdLog = Join-Path $env:TEMP 'gw_openocd.log'
Remove-Item $ocdLog -ErrorAction SilentlyContinue
$ocd = Start-Process -FilePath $py -ArgumentList @($idf, 'openocd') -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $ocdLog -RedirectStandardError "$ocdLog.err"
Start-Sleep -Seconds 4

if ($ocd.HasExited) {
    Write-Host "OpenOCD exited early; see $ocdLog / $ocdLog.err" -ForegroundColor Red
    Get-Content $ocdLog -ErrorAction SilentlyContinue | Select-Object -Last 15
    Get-Content "$ocdLog.err" -ErrorAction SilentlyContinue | Select-Object -Last 15
    exit 1
}

# ---- 2) attach GDB ----
Write-Host "[2/3] attaching GDB ..." -ForegroundColor Yellow
try {
    & $gdb $elf `
        -ex "target remote :3333" `
        -ex "monitor reset halt" `
        -ex "thb $BreakAt" `
        -ex "echo === GDB ready. 'continue' to run, 'bt' for backtrace. ===\n" `
        -ex "continue"
}
finally {
    # ---- 3) cleanup: ALWAYS kill OpenOCD, otherwise the port stays held
    #      (same lesson as T62: leftover openocd blocks the next program/flash)
    Write-Host "`n[3/3] stopping OpenOCD ..." -ForegroundColor Yellow
    if (-not $ocd.HasExited) { Stop-Process -Id $ocd.Id -Force -ErrorAction SilentlyContinue }
    Get-Process openocd -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "session ended." -ForegroundColor Cyan
}
