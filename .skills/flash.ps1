# .skills/flash.ps1 -- one-click build + flash (+ monitor)
# Ported from embedspec/.skills/flash.sh (2026-10-19), rewritten for ESP-IDF.
#
# WHY PowerShell (not .sh/.bat):
#   1) Git Bash: tools/idf.py exits 0 immediately when MSYSTEM is set  -> FAKE PASS
#   2) cmd.exe  : blocked by security policy in this environment
#   3) export.ps1: activate.py detects shell as "powershell.EXE" (upper case) -> fails
#   => constructing the IDF env by hand in PowerShell is the ONLY working path here.
#   See CLAUDE.md "构建环境" for the evidence.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .skills\flash.ps1
#   powershell -ExecutionPolicy Bypass -File .skills\flash.ps1 -Port COM10 -Monitor
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI,
# so non-ASCII text here would be garbled (same class of bug as the .bat rule).

param(
    [string]$Port = "COM10",
    [switch]$Monitor,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Continue'

# ---- IDF environment (hand-built; see CLAUDE.md) ----
$env:IDF_PATH             = 'C:\Espressif\v5.4\esp-idf'
$env:IDF_TOOLS_PATH       = 'C:\Users\wumu2\.espressif'
$tc                       = "$env:IDF_TOOLS_PATH\tools"
$pyDir                    = "$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env\Scripts"
$env:IDF_PYTHON_ENV_PATH  = "$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env"
$env:CMAKE_BUILD_PARALLEL_LEVEL = '4'   # avoid cc1plus OOM; 'idf.py build -j 4' is invalid
$env:PATH = "$tc\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;" +
            "$tc\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;" +
            "$tc\cmake\3.30.2\bin;$tc\ninja\1.12.1;" +
            "$tc\ccache\4.10.2\ccache\bin;$tc\esp-rom-elfs\20241011;" +
            "$pyDir;$env:PATH"

$py  = "$pyDir\python.exe"
$idf = "$env:IDF_PATH\tools\idf.py"

$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj

Write-Host "=== ESP32-S3 gateway: build + flash ===" -ForegroundColor Cyan
Write-Host "  project : $proj"
Write-Host "  port    : $Port"
Write-Host "  idf.py  : $idf"

# ---- sanity: MSYSTEM must be absent ----
if ($env:MSYSTEM) {
    Write-Host "ABORT: MSYSTEM=$env:MSYSTEM is set -> idf.py would fake-pass. Run this from PowerShell." -ForegroundColor Red
    exit 1
}

# ---- 1) build ----
if (-not $SkipBuild) {
    Write-Host "`n[1/2] idf.py build ..." -ForegroundColor Yellow
    & $py $idf build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED (exit $LASTEXITCODE). Nothing was flashed." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "BUILD OK" -ForegroundColor Green
} else {
    Write-Host "`n[1/2] build skipped (-SkipBuild)" -ForegroundColor DarkGray
}

# ---- 2) flash ----
Write-Host "`n[2/2] idf.py -p $Port flash ..." -ForegroundColor Yellow
Write-Host "  NOTE: opening COM10 resets the ESP32 (native USB-Serial-JTAG)." -ForegroundColor DarkGray
& $py $idf -p $Port flash
if ($LASTEXITCODE -ne 0) {
    Write-Host "FLASH FAILED (exit $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "  hints: port in use? wrong port? run: pyserial comports()" -ForegroundColor DarkGray
    exit $LASTEXITCODE
}
Write-Host "FLASH OK" -ForegroundColor Green

# ---- optional monitor ----
if ($Monitor) {
    Write-Host "`n[monitor] Ctrl+] to quit" -ForegroundColor Yellow
    & $py $idf -p $Port monitor
}

Write-Host "`nDone." -ForegroundColor Cyan
