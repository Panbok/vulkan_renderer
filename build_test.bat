@echo off
setlocal EnableDelayedExpansion

rem Ensure compile steps are run within the repository directory
pushd "%~dp0" || exit /b 1

rem Tests consume the checked-in cooked fixtures. Bakery owns regeneration.
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

rem Return to the original directory
popd

rem Unit fixtures intentionally exercise source and legacy compatibility.
set "VKR_TEXTURE_VKT_STRICT=0"
set "VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1"
set "VKR_TEXTURE_VKT_ALLOW_LEGACY=1"

rem Execute the test runner (single-config first, then multi-config fallback).
set "TEST_EXE=%BUILD_DIR%\tests\vulkan_renderer_tester.exe"
if not exist "!TEST_EXE!" set "TEST_EXE=%BUILD_DIR%\tests\Debug\vulkan_renderer_tester.exe"
"!TEST_EXE!" %*

if %errorlevel% neq 0 (
    echo Test runner exited with an error.
    exit /b 1
)

endlocal
