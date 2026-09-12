@echo off
setlocal EnableExtensions EnableDelayedExpansion

pushd "%~dp0.." || exit /b 1
set "REPO_ROOT=%CD%"
set "COOKER_BIN=%VKR_FONT_COOKER_BIN%"
if not "%COOKER_BIN%"=="" goto :cooker_ready

set "BUILD_DIR=%VKR_FONT_COOKER_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%REPO_ROOT%\build_release"
set "VKR_BUILD_TARGET=vkr_font_cooker"
set "VKR_BUILD_LABEL=VKR font cooker"
set "VKR_BUILD_DIR=%BUILD_DIR%"
call "%REPO_ROOT%\build.bat" Release
if errorlevel 1 goto :build_failed
if exist "%BUILD_DIR%\tools\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\tools\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\tools\Release\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\tools\Release\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\Release\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\Release\vkr_font_cooker.exe"

:cooker_ready
if not exist "%COOKER_BIN%" (
    echo Font cook step failed: vkr_font_cooker was not found. 1>&2
    popd
    exit /b 2
)
if not "%~1"=="" goto :cook_arguments
"%COOKER_BIN%" --config "assets\fonts\UbuntuMono-cooked.fontcfg"
if errorlevel 1 goto :cook_failed
"%COOKER_BIN%" --config "assets\fonts\UbuntuMono-Bold-cooked.fontcfg"
if errorlevel 1 goto :cook_failed
goto :done

:cook_arguments
if "%~1"=="" goto :done
"%COOKER_BIN%" --config "%~1"
if errorlevel 1 goto :cook_failed
shift
goto :cook_arguments

:build_failed
echo vkr_font_cooker build failed. 1>&2
popd
exit /b 1
:cook_failed
echo Font cook step failed. 1>&2
popd
exit /b 1
:done
popd
exit /b 0
