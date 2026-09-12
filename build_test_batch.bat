@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if errorlevel 1 exit /b 1

REM Build and run tests 50 times to check for intermittent failures

echo === Building project ===
set "VKR_BUILD_TARGET=vulkan_renderer_tester"
call "%~dp0build.bat" Debug
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)


echo.
echo === Running tests 50 times ===

set VKR_TEXTURE_VKT_STRICT=0
set VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1
set VKR_TEXTURE_VKT_ALLOW_LEGACY=1

set passed=0
set failed=0
set tmpfile=%TEMP%\test_output_%RANDOM%.txt
set "BUILD_DIR=build_debug"
for %%S in (address thread memory leak none) do if /I "%VKR_DEBUG_SANITIZER%"=="%%S" set "BUILD_DIR=build_debug_%%S"
if not "%VKR_BUILD_DIR%"=="" set "BUILD_DIR=%VKR_BUILD_DIR%"
for %%D in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fD"
set "TEST_EXE=%BUILD_DIR%\tests\vulkan_renderer_tester.exe"
if not exist "!TEST_EXE!" set "TEST_EXE=%BUILD_DIR%\tests\Debug\vulkan_renderer_tester.exe"

for /L %%i in (1,1,50) do (
    "!TEST_EXE!" > "!tmpfile!" 2>&1
    set exitcode=!errorlevel!
    if !exitcode! neq 0 (
        set /a failed+=1
        echo Run %%i: FAILED ^(exit code: !exitcode!^)
        echo --- Output ---
        type "!tmpfile!" | more /E +0
        echo --------------
        echo.
    ) else (
        set /a passed+=1
        echo Run %%i: PASSED
    )
)

del /q "!tmpfile!" 2>nul

echo.
echo === Summary ===
echo Passed: !passed!/50
echo Failed: !failed!/50

if !failed! gtr 0 (
    exit /b 1
)

endlocal

