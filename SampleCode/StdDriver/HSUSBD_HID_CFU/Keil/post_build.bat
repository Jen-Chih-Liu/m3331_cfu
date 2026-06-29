@echo off
rem ============================================================
rem  post_build.bat  -  HSUSBD_HID_CFU After-Build Script
rem  Called by Keil AfterMake UserProg2 as: post_build.bat @L
rem  %1 = output name, e.g. HSUSBD_HID_CFU_AP0 or HSUSBD_HID_CFU_AP1
rem
rem  Steps:
rem   1. fromelf: generate disassembly .txt  (was UserProg2)
rem   2. gen_checksum_bin.py: compute byte-sum checksum,
rem         write to ROM_SIZE-8, save as <name>_sum.bin
rem   3. gen_offer_bin.py: read FW_VERSION from _sum.bin,
rem         generate CFU offer.bin
rem   4. gen_content_bin.py: package _sum.bin into CFU
rem         content payload (chunk 48), save as <name>_content.bin
rem ============================================================

rem ---- ARM toolchain path (Keil MDK AC6) -----------------------
rem  fromelf.exe lives here; Keil does NOT add this to PATH when
rem  running a batch file via UserProg, so we set it explicitly.
set ARMCLANG_BIN=C:\Keil_v5\ARM\ARMCLANG\bin
set PATH=%ARMCLANG_BIN%;%PATH%

if "%1"=="" (
    echo [ERROR] post_build.bat requires the output name as %%1
    exit /b 1
)

set BIN_NAME=%1
set OBJ_DIR=.\obj
set BIN_FILE=%OBJ_DIR%\%BIN_NAME%.bin
set SUM_FILE=%OBJ_DIR%\%BIN_NAME%_sum.bin
set CONTENT_FILE=%OBJ_DIR%\%BIN_NAME%_content.bin
set AXF_FILE=%OBJ_DIR%\%BIN_NAME%.axf
set SCR_DIR=..

rem Auto-detect component ID for the OFFER:
rem   The offer must carry the componentId that the TARGET DEVICE has registered.
rem   Device running AP0 (ACTIVE_BANK=0) registers COMPONENT_30  -> offer needs componentId=0x30
rem   Device running AP1 (ACTIVE_BANK=1) registers COMPONENT_31  -> offer needs componentId=0x31
rem
rem   AP1 build = new binary to install into AP1 bank
rem     -> target device is running AP0 -> offer needs componentId=0x30
rem   AP0 build = new binary to install into AP0 bank
rem     -> target device is running AP1 -> offer needs componentId=0x31
set COMP=0x31
echo %BIN_NAME% | findstr /i "AP1" >nul && set COMP=0x30

echo.
echo ============================================================
echo  Post-Build: %BIN_NAME%
echo ============================================================

rem ---- Step 1: disassembly text --------------------------------
echo [1/4] fromelf: generating disassembly ...
fromelf --text -c "%AXF_FILE%" --output "%OBJ_DIR%\%BIN_NAME%.txt"
if errorlevel 1 ( echo [ERROR] fromelf failed & exit /b 1 )

rem ---- Step 2: checksum ----------------------------------------
echo [2/4] gen_checksum_bin.py: computing checksum ...
python "%SCR_DIR%\gen_checksum_bin.py" "%BIN_FILE%" --output "%SUM_FILE%"
if errorlevel 1 ( echo [ERROR] gen_checksum_bin.py failed & exit /b 1 )

rem ---- Step 3: offer.bin ---------------------------------------
echo [3/4] gen_offer_bin.py: generating offer.bin ...
python "%SCR_DIR%\gen_offer_bin.py" ^
    --component %COMP% ^
    --fw-bin "%SUM_FILE%" ^
    --version-offset 0x1FFF0 ^
    --no-bump ^
    --output "%OBJ_DIR%\%BIN_NAME%.offer.bin"
if errorlevel 1 ( echo [ERROR] gen_offer_bin.py failed & exit /b 1 )

rem ---- Step 4: content.bin -------------------------------------
echo [4/4] gen_content_bin.py: packaging content payload ...
python "%SCR_DIR%\gen_content_bin.py" "%SUM_FILE%" ^
    --output "%CONTENT_FILE%" ^
    --chunk 48
if errorlevel 1 ( echo [ERROR] gen_content_bin.py failed & exit /b 1 )

echo.
echo ============================================================
echo  Post-Build complete.
echo    Checksum bin : %SUM_FILE%
echo    Offer bin    : %OBJ_DIR%\%BIN_NAME%.offer.bin
echo    Content bin  : %CONTENT_FILE%
echo ============================================================
