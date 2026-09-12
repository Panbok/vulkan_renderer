@echo off
REM Explicit, configuration-independent KTX2/UASTC texture packing.
setlocal EnableDelayedExpansion

cd /d "%~dp0.."
set "REPO_ROOT=%CD%"
set "BUILD_DIR=%REPO_ROOT%\build_release"
set "TEXTURE_ROOT=%VKR_TEXTURE_PACK_INPUT_DIR%"
if "%TEXTURE_ROOT%"=="" set "TEXTURE_ROOT=%REPO_ROOT%\assets\textures"

if not exist "%TEXTURE_ROOT%" (
    echo Texture pack step skipped: texture directory not found at %TEXTURE_ROOT%
    exit /b 0
)

set "PACKER_BIN=%VKR_VKT_PACKER_BIN%"
if not "%PACKER_BIN%"=="" goto :vkr_have_packer

set "VKR_BUILD_TARGET=vkr_vkt_packer"
set "VKR_BUILD_LABEL=VKR texture packer"
set "VKR_BUILD_DIR=%BUILD_DIR%"
call "%REPO_ROOT%\build.bat" Release
if errorlevel 1 exit /b 1

if exist "%BUILD_DIR%\tools\vkr_vkt_packer.exe" set "PACKER_BIN=%BUILD_DIR%\tools\vkr_vkt_packer.exe"
if "!PACKER_BIN!"=="" if exist "%BUILD_DIR%\tools\Release\vkr_vkt_packer.exe" set "PACKER_BIN=%BUILD_DIR%\tools\Release\vkr_vkt_packer.exe"
if "!PACKER_BIN!"=="" if exist "%BUILD_DIR%\vkr_vkt_packer.exe" set "PACKER_BIN=%BUILD_DIR%\vkr_vkt_packer.exe"
if "!PACKER_BIN!"=="" if exist "%BUILD_DIR%\Release\vkr_vkt_packer.exe" set "PACKER_BIN=%BUILD_DIR%\Release\vkr_vkt_packer.exe"

:vkr_have_packer
if not exist "%PACKER_BIN%" (
    echo Texture pack step failed: programmatic packer binary was not found.
    echo Set VKR_VKT_PACKER_BIN to use an existing packer binary.
    exit /b 2
)

set "STRICT_ARG="
set "FORCE_ARG="
set "VERBOSE_ARG="
if /I "%VKR_VKT_PACK_STRICT%"=="1" set "STRICT_ARG=--strict"
if /I "%VKR_VKT_PACK_FORCE%"=="1" set "FORCE_ARG=--force"
if /I "%VKR_VKT_PACK_VERBOSE%"=="1" set "VERBOSE_ARG=--verbose"

echo Packing .vkt textures with programmatic packer: %PACKER_BIN%
"%PACKER_BIN%" --input-dir "%TEXTURE_ROOT%" %STRICT_ARG% %FORCE_ARG% %VERBOSE_ARG%
exit /b !errorlevel!
