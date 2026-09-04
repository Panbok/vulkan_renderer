@echo off
REM Build and run script for vulkan_renderer
setlocal EnableDelayedExpansion

echo Starting build and run process...
echo.

set "SCRIPT_DIR=%~dp0"
if "%VKR_RUN_SUBDIR%"=="" set "VKR_RUN_SUBDIR=app"
if "%VKR_RUN_BINARY%"=="" set "VKR_RUN_BINARY=vulkan_renderer"
if "%VKR_RUN_LABEL%"=="" set "VKR_RUN_LABEL=Vulkan Renderer"
REM Windows quoting gotcha: a quoted path ending with '\' can escape the closing quote.
REM Keep a version without the trailing slash for tools like Windows Terminal.
set "SCRIPT_CWD=%SCRIPT_DIR%"
if "%SCRIPT_CWD:~-1%"=="\" set "SCRIPT_CWD=%SCRIPT_CWD:~0,-1%"

REM Optional: accept build type
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

set "BUILD_DIR="
if /I "%BUILD_TYPE%"=="Debug" set "BUILD_DIR=build_debug"
if /I "%BUILD_TYPE%"=="Release" set "BUILD_DIR=build_release"
if /I "%BUILD_TYPE%"=="RelWithDebInfo" set "BUILD_DIR=build_release_info"
if /I "%BUILD_TYPE%"=="MinSizeRel" set "BUILD_DIR=build_min_size_rel"
if "%BUILD_DIR%"=="" (
    echo Error: unsupported build type "%BUILD_TYPE%".
    exit /b 1
)

REM Call the build script
call "%SCRIPT_DIR%build.bat" %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Build failed! Exiting.
    exit /b 1
)

echo.
echo Build successful! Starting %VKR_RUN_LABEL%...
echo.

REM Execute the vulkan_renderer with working directory set to repo root
set "APP_EXE=%SCRIPT_DIR%%BUILD_DIR%\%VKR_RUN_SUBDIR%\%VKR_RUN_BINARY%.exe"
if not exist "%APP_EXE%" set "APP_EXE=%SCRIPT_DIR%%BUILD_DIR%\%VKR_RUN_SUBDIR%\%BUILD_TYPE%\%VKR_RUN_BINARY%.exe"
if not exist "%APP_EXE%" (
    echo Error: executable not found at "%APP_EXE%"
    exit /b 1
)

REM Configure AddressSanitizer symbolization if available (Clang/LLVM).
if "%ASAN_SYMBOLIZER_PATH%"=="" (
    if exist "C:\Program Files\LLVM\bin\llvm-symbolizer.exe" (
        set "ASAN_SYMBOLIZER_PATH=C:\Program Files\LLVM\bin\llvm-symbolizer.exe"
    )
)
if "%ASAN_OPTIONS%"=="" (
    set "ASAN_OPTIONS=symbolize=1:malloc_context_size=50:abort_on_error=1"
)

REM Launch in Windows Terminal if available; otherwise run in-place.
where wt >nul 2>&1
if %errorlevel% equ 0 (
    REM Run the exe in a new Windows Terminal tab.
    REM Use `--` to stop wt option parsing before the command.
    wt new-tab -d "%SCRIPT_CWD%" --title "%VKR_RUN_LABEL%" -- "%APP_EXE%" %2 %3 %4 %5 %6 %7 %8 %9
) else (
    pushd "%SCRIPT_CWD%"
    "%APP_EXE%" %2 %3 %4 %5 %6 %7 %8 %9
    popd
)

endlocal
