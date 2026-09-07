@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  merge_aprom.bat  -  Merge AP0 + AP1 sum bins into one
rem                      248 KB APROM image.
rem
rem   AP0 (bank0) @ 0x00000 - 0x1EFFF  (124 KB)
rem   AP1 (bank1) @ 0x1F000 - 0x3DFFF  (124 KB)
rem   Total = 0x3E000 = 253952 bytes = 248 KB
rem
rem  Inputs  : out\HSUSBD_HID_CFU_AP0_sum.bin
rem            out\HSUSBD_HID_CFU_AP1_sum.bin
rem  Output  : out\HSUSBD_HID_CFU_APROM_248KB.BIN
rem ============================================================

set "ROOT=%~dp0"
set "OUT_DIR=%ROOT%out"
set "AP0=%OUT_DIR%\HSUSBD_HID_CFU_AP0_sum.bin"
set "AP1=%OUT_DIR%\HSUSBD_HID_CFU_AP1_sum.bin"
set "MERGED=%OUT_DIR%\HSUSBD_HID_CFU_APROM_248KB.BIN"
set "EXPECT=253952"

rem ---- Check inputs -------------------------------------------
if not exist "%AP0%" ( echo [ERROR] Missing "%AP0%" & exit /b 1 )
if not exist "%AP1%" ( echo [ERROR] Missing "%AP1%" & exit /b 1 )

for %%F in ("%AP0%") do set "SZ0=%%~zF"
for %%F in ("%AP1%") do set "SZ1=%%~zF"
echo [*] AP0 : %AP0%  (%SZ0% bytes)
echo [*] AP1 : %AP1%  (%SZ1% bytes)

rem ---- Concatenate AP0 + AP1 ----------------------------------
copy /b "%AP0%" + "%AP1%" "%MERGED%" >nul
if errorlevel 1 ( echo [ERROR] Merge failed & exit /b 1 )

for %%F in ("%MERGED%") do set "SZM=%%~zF"

echo.
echo ============================================================
echo  Merged : %MERGED%
echo  Size   : %SZM% bytes  (expected %EXPECT% = 248 KB)
echo ============================================================

if not "%SZM%"=="%EXPECT%" (
    echo [WARN] Output size %SZM% differs from expected %EXPECT% bytes.
    echo        Check that each _sum.bin is exactly 126976 bytes.
    exit /b 1
)

echo [OK] APROM 248 KB image created successfully.
endlocal
exit /b 0
