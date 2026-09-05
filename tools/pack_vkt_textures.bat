@echo off
REM Explicit, configuration-independent KTX2/UASTC texture packing.
setlocal EnableDelayedExpansion

cd /d "%~dp0.."
set "REPO_ROOT=%CD%"
set "BUILD_DIR=%REPO_ROOT%\build_vkt_packer"
set "TEXTURE_ROOT=%VKR_TEXTURE_PACK_INPUT_DIR%"
if "%TEXTURE_ROOT%"=="" set "TEXTURE_ROOT=%REPO_ROOT%\assets\textures"

if not exist "%TEXTURE_ROOT%" (
    echo Texture pack step skipped: texture directory not found at %TEXTURE_ROOT%
    exit /b 0
)

set "PACKER_BIN=%VKR_VKT_PACKER_BIN%"
if not "%PACKER_BIN%"=="" goto :vkr_have_packer

set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1
if !errorlevel! EQU 0 (
    where clang++ >nul 2>&1
    if !errorlevel! EQU 0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

REM KTX-Software needs a real bash on Windows to generate version.h.
set "BASH_HINT="
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\usr\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\usr\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\usr\bin\bash.exe" set "BASH_HINT=C:\msys64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\bin\bash.exe" set "BASH_HINT=C:\msys64\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\usr\bin\bash.exe" set "BASH_HINT=C:\mingw64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\bin\bash.exe" set "BASH_HINT=C:\mingw64\bin\bash.exe"

set "BASH_ARG="
if not "!BASH_HINT!"=="" set "BASH_ARG=-DBASH_EXECUTABLE:FILEPATH=!BASH_HINT!"
set "VKR_BASH_ENV_FILE=%REPO_ROOT%\tools\vkr_bash_env.sh"
if not "!BASH_HINT!"=="" if exist "!VKR_BASH_ENV_FILE!" set "BASH_ENV=!VKR_BASH_ENV_FILE!"

echo !BASH_HINT! | findstr /I /C:"C:\msys64\" /C:"C:\mingw64\" >nul 2>&1
if !errorlevel! EQU 0 if exist "C:\msys64\usr\bin" set "PATH=C:\msys64\usr\bin;C:\msys64\bin;%PATH%"
if !errorlevel! EQU 0 if exist "C:\mingw64\usr\bin" set "PATH=C:\mingw64\usr\bin;C:\mingw64\bin;%PATH%"

echo Building the configuration-independent texture packer
cmake --fresh -S . -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE:STRING=Release %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --target vkr_vkt_packer --config Release
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
