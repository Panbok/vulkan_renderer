@echo off
setlocal enabledelayedexpansion

REM Build and run tests 50 times to check for intermittent failures

echo === Building project ===
cmake --build build_test --target vulkan_renderer_tester --config Debug
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo === Running tests 50 times ===

set passed=0
set failed=0
set tmpfile=%TEMP%\test_output_%RANDOM%.txt
set "TEST_EXE=build_test\tests\vulkan_renderer_tester.exe"
if not exist "!TEST_EXE!" set "TEST_EXE=build_test\tests\Debug\vulkan_renderer_tester.exe"

for /L %%i in (1,1,50) do (
    "!TEST_EXE!" > "!tmpfile!" 2>&1
    set exitcode=!errorlevel!
    if !exitcode! neq 0 (
        set /a failed+=1
        cho Run %%i: FAILED ^(exit code: !exitcode!^)
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

