@echo off
setlocal
set "VKR_LIBRARY_SET=%~1"
if "%VKR_LIBRARY_SET%"=="" set "VKR_LIBRARY_SET=renderer"
if "%VKR_LIBRARY_SET%"=="renderer" (
    set "VKR_BUILD_TARGET=vkr_renderer_example"
) else if "%VKR_LIBRARY_SET%"=="runtime" (
    set "VKR_BUILD_TARGET=vkr_host_example"
) else (
    echo Usage: %~nx0 [renderer^|runtime]
    exit /b 2
)
set "VKR_BUILD_LABEL=VKR library example"
call "%~dp0build.bat" Release
exit /b %errorlevel%
