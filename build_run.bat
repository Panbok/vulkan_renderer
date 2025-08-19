@echo off
REM Build and run script for vulkan_renderer
setlocal EnableDelayedExpansion

echo Starting build and run process...
echo.

REM Optional: accept build type
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

REM Call the build script
call "%~dp0build.bat" %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Build failed! Exiting.
    exit /b 1
)

echo.
echo Build successful! Starting vulkan_renderer...
echo.

REM Execute the vulkan_renderer with working directory set to repo root
set "APP_EXE=%~dp0build\app\vulkan_renderer.exe"
if not exist "%APP_EXE%" (
    echo Error: executable not found at "%APP_EXE%"
    exit /b 1
)

wt new-tab --title "Vulkan Renderer" --startingDirectory "%SCRIPT_DIR%" cmd.exe /k ""%APP_EXE%" %*"

endlocal 