@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  build_all.bat  -  Build ISP + CFU (bank0/bank1) and
rem                    collect output .bin files into .\out
rem
rem  Projects:
rem    1) SampleCode\ISP\ISP_HID_20          target: ISP_HID_20
rem    2) SampleCode\StdDriver\HSUSBD_HID_CFU target: HSUSBD_HID_CFU_AP0 (bank0)
rem    3) SampleCode\StdDriver\HSUSBD_HID_CFU target: HSUSBD_HID_CFU_AP1 (bank1)
rem    4) isp_app\HID_ISP_TOOL                PC host tool (CMake / MSVC)
rem    5) isp_app\m3331_cal_tool             PC host tool (CMake / MSVC)
rem
rem  UV4 build return codes: 0=OK, 1=warnings, >=2=errors
rem ============================================================

rem ---- Keil MDK UV4 path (edit if installed elsewhere) --------
set "UV4=C:\Keil_v5\UV4\UV4.exe"
if not exist "%UV4%" (
    echo [ERROR] UV4.exe not found at "%UV4%"
    echo         Please edit the UV4 variable in this script.
    exit /b 1
)

rem ---- Paths --------------------------------------------------
set "ROOT=%~dp0"
set "OUT_DIR=%ROOT%out"
set "ISP_PROJ=%ROOT%SampleCode\ISP\ISP_HID_20\Keil\ISP_HID_20.uvprojx"
set "ISP_OBJ=%ROOT%SampleCode\ISP\ISP_HID_20\Keil\obj"
set "CFU_PROJ=%ROOT%SampleCode\StdDriver\HSUSBD_HID_CFU\Keil\HSUSBD_HID_CFU.uvprojx"
set "CFU_OBJ=%ROOT%SampleCode\StdDriver\HSUSBD_HID_CFU\Keil\obj"
set "TOOL_DIR=%ROOT%isp_app\HID_ISP_TOOL"
set "TOOL_BUILD=%TOOL_DIR%\build"
set "TOOL_EXE=%TOOL_BUILD%\Release\HID_ISP_TOOL.exe"
set "CAL_DIR=%ROOT%isp_app\m3331_cal_tool"
set "CAL_BUILD=%CAL_DIR%\build"
set "CAL_EXE=%CAL_BUILD%\Release\m3331_cal_tool.exe"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo ============================================================
echo  Output folder: %OUT_DIR%
echo ============================================================

rem ---- Build 1: ISP_HID_20 ------------------------------------
call :build "%ISP_PROJ%" "ISP_HID_20"       || goto :fail

rem ---- Build 2: CFU bank0 (AP0) -------------------------------
call :build "%CFU_PROJ%" "HSUSBD_HID_CFU_AP0" || goto :fail

rem ---- Build 3: CFU bank1 (AP1) -------------------------------
call :build "%CFU_PROJ%" "HSUSBD_HID_CFU_AP1" || goto :fail

rem ---- Build 4: HID_ISP_TOOL (PC host tool, CMake/MSVC) -------
call :build_tool || goto :fail

rem ---- Build 5: m3331_cal_tool (PC host tool, CMake/MSVC) -----
call :build_cal_tool || goto :fail

rem ---- Collect .bin files into out ----------------------------
echo.
echo ------------------------------------------------------------
echo  Collecting .bin files into out ...
echo ------------------------------------------------------------
copy /Y "%ISP_OBJ%\ISP_HID_20.bin"                  "%OUT_DIR%\" >nul && echo   [OK] ISP_HID_20.bin
copy /Y "%CFU_OBJ%\HSUSBD_HID_CFU_AP0*.bin"         "%OUT_DIR%\" >nul && echo   [OK] HSUSBD_HID_CFU_AP0*.bin
copy /Y "%CFU_OBJ%\HSUSBD_HID_CFU_AP1*.bin"         "%OUT_DIR%\" >nul && echo   [OK] HSUSBD_HID_CFU_AP1*.bin
copy /Y "%TOOL_EXE%"                                "%OUT_DIR%\" >nul && echo   [OK] HID_ISP_TOOL.exe
copy /Y "%CAL_EXE%"                                 "%OUT_DIR%\" >nul && echo   [OK] m3331_cal_tool.exe

echo.
echo ============================================================
echo  BUILD SUCCESS. Output in: %OUT_DIR%
echo ============================================================
dir /b "%OUT_DIR%\*.bin"
dir /b "%OUT_DIR%\*.exe"
endlocal
exit /b 0

rem ============================================================
rem  :build  <uvprojx>  <target>
rem     Rebuild the given target; treat errorlevel>=2 as failure
rem ============================================================
:build
echo.
echo ------------------------------------------------------------
echo  Building: %~2
echo ------------------------------------------------------------
set "LOG=%TEMP%\uv4_%~2.log"
"%UV4%" -j0 -r "%~1" -t "%~2" -o "%LOG%"
set "RC=!errorlevel!"
if exist "%LOG%" type "%LOG%"
if !RC! GEQ 2 (
    echo [ERROR] Build failed for target %~2 ^(rc=!RC!^)
    exit /b 1
)
echo [OK] Build finished for target %~2 ^(rc=!RC!^)
exit /b 0

rem ============================================================
rem  :build_tool
rem     Configure + build the HID_ISP_TOOL PC host tool via CMake.
rem     Requires cmake on PATH and an MSVC toolchain (Visual Studio).
rem ============================================================
:build_tool
echo.
echo ------------------------------------------------------------
echo  Building: HID_ISP_TOOL ^(CMake / MSVC^)
echo ------------------------------------------------------------
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake not found on PATH. Install CMake or run from a
    echo         Visual Studio Developer Command Prompt.
    exit /b 1
)
cmake -S "%TOOL_DIR%" -B "%TOOL_BUILD%" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure failed for HID_ISP_TOOL
    exit /b 1
)
cmake --build "%TOOL_BUILD%" --config Release
if errorlevel 1 (
    echo [ERROR] CMake build failed for HID_ISP_TOOL
    exit /b 1
)
if not exist "%TOOL_EXE%" (
    echo [ERROR] HID_ISP_TOOL.exe not found after build
    exit /b 1
)
echo [OK] Build finished for HID_ISP_TOOL
exit /b 0

rem ============================================================
rem  :build_cal_tool
rem     Configure + build the m3331_cal_tool PC host tool via CMake.
rem     Requires cmake on PATH and an MSVC toolchain (Visual Studio).
rem ============================================================
:build_cal_tool
echo.
echo ------------------------------------------------------------
echo  Building: m3331_cal_tool ^(CMake / MSVC^)
echo ------------------------------------------------------------
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake not found on PATH. Install CMake or run from a
    echo         Visual Studio Developer Command Prompt.
    exit /b 1
)
cmake -S "%CAL_DIR%" -B "%CAL_BUILD%" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure failed for m3331_cal_tool
    exit /b 1
)
cmake --build "%CAL_BUILD%" --config Release
if errorlevel 1 (
    echo [ERROR] CMake build failed for m3331_cal_tool
    exit /b 1
)
if not exist "%CAL_EXE%" (
    echo [ERROR] m3331_cal_tool.exe not found after build
    exit /b 1
)
echo [OK] Build finished for m3331_cal_tool
exit /b 0

:fail
echo.
echo ============================================================
echo  BUILD FAILED.
echo ============================================================
endlocal
exit /b 1
