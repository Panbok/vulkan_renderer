@echo off
REM Build and run script for vulkan_renderer
setlocal EnableDelayedExpansion

echo Starting build and run process...
echo.

set "SCRIPT_DIR=%~dp0"
REM Windows quoting gotcha: a quoted path ending with '\' can escape the closing quote.
REM Keep a version without the trailing slash for tools like Windows Terminal.
set "SCRIPT_CWD=%SCRIPT_DIR%"
if "%SCRIPT_CWD:~-1%"=="\" set "SCRIPT_CWD=%SCRIPT_CWD:~0,-1%"

REM Optional: accept build type
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

REM Call the build script
call "%SCRIPT_DIR%build.bat" %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Build failed! Exiting.
    exit /b 1
)

echo.
echo Build successful! Starting vulkan_renderer...
echo.

REM Execute the vulkan_renderer with working directory set to repo root
set "APP_EXE=%SCRIPT_DIR%build\app\vulkan_renderer.exe"
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
    wt new-tab -d "%SCRIPT_CWD%" --title "Vulkan Renderer" -- "%APP_EXE%" %*
) else (
    pushd "%SCRIPT_CWD%"
    "%APP_EXE%" %*
    popd
)

endlocal 