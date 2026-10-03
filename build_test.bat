@echo off
setlocal EnableDelayedExpansion

rem Ensure compile steps are run within the repository directory
pushd "%~dp0" || exit /b 1

python tools/checks/check_format.py
if errorlevel 1 (
    popd
    exit /b 1
)
python tools/checks/check_path_boundaries.py
if errorlevel 1 (
    popd
    exit /b 1
)

rem Tests consume fixtures cooked on this host by vkr_bakery build assets/bakery.json.
set "VKR_BUILD_TARGET=vulkan_renderer_tester"
set "VKR_BUILD_LABEL=VKR CPU tests"
call "%~dp0build.bat" Debug
if errorlevel 1 (
    popd
    exit /b 1
)

set "BUILD_DIR=build_debug"
for %%S in (address thread memory leak none) do if /I "%VKR_DEBUG_SANITIZER%"=="%%S" set "BUILD_DIR=build_debug_%%S"
if not "%VKR_BUILD_DIR%"=="" set "BUILD_DIR=%VKR_BUILD_DIR%"
for %%D in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fD"

rem The managed path grammar runs through the project runner this build made.
set "BAKERY_EXE=%BUILD_DIR%\tools\bakery\vkr_bakery.exe"
if not exist "!BAKERY_EXE!" set "BAKERY_EXE=%BUILD_DIR%\tools\bakery\Debug\vkr_bakery.exe"
python tools/checks/check_path_contract.py --bakery "!BAKERY_EXE!"
if errorlevel 1 (
    popd
    exit /b 1
)

rem Return to the original directory
popd


rem Execute the test runner (single-config first, then multi-config fallback).
set "TEST_EXE=%BUILD_DIR%\tests\vulkan_renderer_tester.exe"
if not exist "!TEST_EXE!" set "TEST_EXE=%BUILD_DIR%\tests\Debug\vulkan_renderer_tester.exe"
"!TEST_EXE!" %*

if %errorlevel% neq 0 (
    echo Test runner exited with an error.
    exit /b 1
)

endlocal
