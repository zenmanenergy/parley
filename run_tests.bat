@echo off
REM Parley Firmware Test Runner
REM Runs unit tests on native platform (Windows/Linux/macOS) without ESP32

setlocal enabledelayedexpansion

echo.
echo ========================================
echo  PARLEY FIRMWARE TEST RUNNER
echo ========================================
echo.

REM Check if platformio is installed
where pio >nul 2>nul
if errorlevel 1 (
    echo ERROR: PlatformIO is not installed or not in PATH
    echo Install it with: pip install platformio
    exit /b 1
)

REM Run tests for nodes firmware
echo [1/2] Running nodes firmware tests...
echo.
cd /d "firmware\nodes"
call pio test -e test
if errorlevel 1 (
    echo FAILED: nodes firmware tests
    cd /d ..\..
    exit /b 1
)
cd /d ..\..

echo.
echo [2/2] Running gateway firmware tests...
echo.
cd /d "firmware\gateway"
call pio test -e test
if errorlevel 1 (
    echo FAILED: gateway firmware tests
    cd /d ..\..
    exit /b 1
)
cd /d ..\..

echo.
echo ========================================
echo  ALL TESTS PASSED
echo ========================================
echo.
