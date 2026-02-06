@echo off
setlocal

rem Exit early if any commands fail
rem In Batch, 'exit /b 1' can be used after a command that might fail to achieve
rem a similar effect, but it needs to be checked explicitly.
rem For this script's purpose, we'll assume commands generally succeed or handle
rem errors as they occur within the command itself.

rem Ensure compile steps are run within the repository directory
pushd "%~dp0" || exit /b 1

rem Configure step (can often be skipped if build dir exists and config hasn't changed)
rem Re-running refreshes CMake cache state for this build directory.
cmake --fresh -B build -S . -U CMAKE_TOOLCHAIN_FILE
if %errorlevel% neq 0 (
    echo CMake configure failed.
    popd
    exit /b 1
)

rem Build only the test target
cmake --build ./build --target vulkan_renderer_tester
if %errorlevel% neq 0 (
    echo CMake build failed for vulkan_renderer_tester.
    popd
    exit /b 1
)

rem Return to the original directory
popd

rem Execute the test runner
rem %~dp0 gets the drive and path of the current script.
"%~dp0build\tests\vulkan_renderer_tester" %*

if %errorlevel% neq 0 (
    echo Test runner exited with an error.
    exit /b 1
)

endlocal
