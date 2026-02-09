@echo off
REM Build script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

REM Optional arg1: BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

cd /d "%~dp0"
set "REPO_ROOT=%CD%"

REM Texture packing defaults (align with build.sh)
if "%VKR_VKT_PACK%"=="" set "VKR_VKT_PACK=1"

REM Compile shaders from root assets directory
echo Compiling shaders
pushd assets 2>nul
if errorlevel 1 (
    echo Error: assets directory not found.
    exit /b 1
)

where slangc >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: slangc compiler not found. Please install slangc.
    popd
    exit /b 1
)

pushd shaders 2>nul
if not errorlevel 1 (
    dir *.slang >nul 2>&1
    if %errorlevel% equ 0 (
        for %%f in (*.slang) do (
            echo Compiling %%f
            slangc -target spirv -o "%%~nf.spv" "%%f"
        )
    ) else (
        echo No .slang files found to compile
    )
    popd
)
popd

REM Configure CMake
echo Configuring CMake (%BUILD_TYPE%)
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1 >nul
if %errorlevel%==0 (
    where clang++ >nul 2>&1 >nul
    if %errorlevel%==0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

REM KTX-Software needs a real bash on Windows to generate version.h.
REM Prefer Git for Windows bash over the WSL stub at System32\bash.exe.
set "BASH_HINT="

REM Try common Git for Windows locations (avoid %ProgramFiles(x86)% parsing edge cases).
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\usr\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\usr\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\usr\bin\bash.exe"

REM Try MSYS2/MinGW bash locations (mingw64 environments).
if "!BASH_HINT!"=="" if exist "C:\msys64\usr\bin\bash.exe" set "BASH_HINT=C:\msys64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\bin\bash.exe" set "BASH_HINT=C:\msys64\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\usr\bin\bash.exe" set "BASH_HINT=C:\mingw64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\bin\bash.exe" set "BASH_HINT=C:\mingw64\bin\bash.exe"

set "BASH_ARG="
if not "!BASH_HINT!"=="" set "BASH_ARG=-DBASH_EXECUTABLE:FILEPATH=!BASH_HINT!"

REM Ensure non-interactive bash shells used by CMake/Ninja have a sane PATH.
REM This prevents KTX scripts using `#!/usr/bin/env bash` from accidentally
REM resolving `bash` to the WSL stub at C:\Windows\System32\bash.exe.
set "VKR_BASH_ENV_FILE=%REPO_ROOT%\tools\vkr_bash_env.sh"
if not "!BASH_HINT!"=="" if exist "!VKR_BASH_ENV_FILE!" set "BASH_ENV=!VKR_BASH_ENV_FILE!"

REM If we're using MSYS2/MinGW bash, make sure PATH is MSYS-first.
REM This prevents /usr/bin/env bash (used by ktx-software scripts/mkversion)
REM from accidentally resolving to the WSL stub at System32\bash.exe.
echo !BASH_HINT! | findstr /I /C:"C:\msys64\" /C:"C:\mingw64\" >nul 2>&1
if !errorlevel! EQU 0 if exist "C:\msys64\usr\bin" set "PATH=C:\msys64\usr\bin;C:\msys64\bin;%PATH%"
if !errorlevel! EQU 0 if exist "C:\mingw64\usr\bin" set "PATH=C:\mingw64\usr\bin;C:\mingw64\bin;%PATH%"

cmake --fresh -S . -B build -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 goto :vkr_cmake_configure_failed

REM Align with build.sh: Release builds require texture packing (enabled by default).
if /I "%BUILD_TYPE%"=="Release" call :vkr_release_checks
if errorlevel 1 exit /b !errorlevel!

REM Optional texture pack step (align with build.sh).
if /I "%VKR_VKT_PACK%"=="1" goto :vkr_do_pack_textures
echo Skipping texture pack step (set VKR_VKT_PACK=1 to enable)
goto :vkr_after_pack_textures
:vkr_do_pack_textures
call :vkr_pack_textures
if errorlevel 1 exit /b !errorlevel!
:vkr_after_pack_textures

REM Build target
echo Building vulkan_renderer (%BUILD_TYPE%)
cmake --build .\build --target vulkan_renderer --config %BUILD_TYPE%
if errorlevel 1 goto :vkr_build_failed

REM Copy shaders from root assets into build output
echo Copying shaders to build/app directory
if not exist build\app md build\app
if not exist build\app\assets md build\app\assets

dir assets\shaders\*.spv >nul 2>&1
if errorlevel 1 goto :vkr_no_spv_to_copy
copy /Y assets\shaders\*.spv build\app\assets\ >nul
goto :vkr_after_spv_copy
:vkr_no_spv_to_copy
echo No .spv files to copy to build/app - skipping
:vkr_after_spv_copy

echo Build completed successfully!
endlocal
exit /b 0

:vkr_cmake_configure_failed
echo CMake configure failed.
exit /b 1

:vkr_build_failed
echo Build failed.
exit /b 1

REM ============================================================================
REM Subroutines
REM ============================================================================

:vkr_release_checks
REM Release build requires packing enabled.
if /I "%VKR_VKT_PACK%"=="1" goto :vkr_release_checks_ok
echo Release build requires texture packing. Texture packing is enabled by default (VKR_VKT_PACK=1^); set VKR_VKT_PACK=0 to disable. Set VKR_VKT_PACK=1 to enable packing for Release builds.
exit /b 1
:vkr_release_checks_ok

REM Enable strict packing by default in Release builds unless explicitly set.
if defined VKR_VKT_PACK_STRICT goto :vkr_release_checks_done
set "VKR_VKT_PACK_STRICT=1"
echo Release build: enabling strict texture packing (VKR_VKT_PACK_STRICT=1^)
:vkr_release_checks_done
exit /b 0

:vkr_pack_textures
echo Building texture packer
cmake --build .\build --target vkr_vkt_packer --config %BUILD_TYPE%
if errorlevel 1 goto :vkr_pack_textures_build_failed

set "TEXTURE_ROOT=%VKR_TEXTURE_PACK_INPUT_DIR%"
if "%TEXTURE_ROOT%"=="" set "TEXTURE_ROOT=%REPO_ROOT%\assets\textures"

if not exist "%TEXTURE_ROOT%" goto :vkr_pack_textures_no_textures

set "PACKER_BIN=%VKR_VKT_PACKER_BIN%"
if not "%PACKER_BIN%"=="" goto :vkr_have_packer

REM Search common output locations (align with tools/pack_vkt_textures.sh).
if exist "%REPO_ROOT%\build\tools\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\tools\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\tools\%BUILD_TYPE%\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\tools\%BUILD_TYPE%\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\tools\Debug\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\tools\Debug\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\tools\Release\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\tools\Release\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\%BUILD_TYPE%\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\%BUILD_TYPE%\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\Debug\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\Debug\vkr_vkt_packer.exe"
if "%PACKER_BIN%"=="" if exist "%REPO_ROOT%\build\Release\vkr_vkt_packer.exe" set "PACKER_BIN=%REPO_ROOT%\build\Release\vkr_vkt_packer.exe"

:vkr_have_packer
if not "%PACKER_BIN%"=="" goto :vkr_run_packer
echo Texture pack step failed: programmatic packer binary was not found.
echo Build target "vkr_vkt_packer" first or set VKR_VKT_PACKER_BIN.
if /I "%VKR_VKT_PACK_STRICT%"=="1" exit /b 1
exit /b 2

:vkr_run_packer
set "STRICT_ARG="
set "FORCE_ARG="
set "VERBOSE_ARG="
if /I "%VKR_VKT_PACK_STRICT%"=="1" set "STRICT_ARG=--strict"
if /I "%VKR_VKT_PACK_FORCE%"=="1" set "FORCE_ARG=--force"
if /I "%VKR_VKT_PACK_VERBOSE%"=="1" set "VERBOSE_ARG=--verbose"

echo Packing .vkt textures with programmatic packer: %PACKER_BIN%
%PACKER_BIN% --input-dir "%TEXTURE_ROOT%" %STRICT_ARG% %FORCE_ARG% %VERBOSE_ARG%
set "PACK_RC=!errorlevel!"
if not "!PACK_RC!"=="0" goto :vkr_pack_textures_run_failed
exit /b 0

:vkr_pack_textures_build_failed
echo Failed to build vkr_vkt_packer.
exit /b 1

:vkr_pack_textures_no_textures
echo Texture pack step skipped: texture directory not found at %TEXTURE_ROOT%
exit /b 0

:vkr_pack_textures_run_failed
echo Texture pack step failed (exit code !PACK_RC!).
exit /b !PACK_RC!
