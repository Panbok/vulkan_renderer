@echo off
REM Build script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

REM Optional arg1: BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
if "%VKR_BUILD_TARGET%"=="" set "VKR_BUILD_TARGET=vulkan_renderer"
if "%VKR_BUILD_LABEL%"=="" set "VKR_BUILD_LABEL=VKR app"

set "BUILD_DIR="
if /I "%BUILD_TYPE%"=="Debug" set "BUILD_DIR=build_debug"
if /I "%BUILD_TYPE%"=="Release" set "BUILD_DIR=build_release"
if /I "%BUILD_TYPE%"=="RelWithDebInfo" set "BUILD_DIR=build_release_info"
if /I "%BUILD_TYPE%"=="MinSizeRel" set "BUILD_DIR=build_min_size_rel"
if "%BUILD_DIR%"=="" (
    echo Error: unsupported build type "%BUILD_TYPE%".
    echo Expected Debug, Release, RelWithDebInfo, or MinSizeRel.
    exit /b 1
)

cd /d "%~dp0"
set "REPO_ROOT=%CD%"

echo Configuring CMake (%BUILD_TYPE%)
echo Using build directory: %BUILD_DIR%
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1
if !errorlevel! EQU 0 (
    where clang++ >nul 2>&1
    if !errorlevel! EQU 0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

REM KTX-Software needs a real bash on Windows to generate version.h.
REM Prefer Git for Windows bash over the WSL stub at System32\bash.exe.
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

cmake --fresh -S . -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 goto :vkr_cmake_configure_failed

echo Building %VKR_BUILD_LABEL% (%BUILD_TYPE%)
cmake --build ".\%BUILD_DIR%" --target %VKR_BUILD_TARGET% vkr_harness vkr_mesh_cooker vkr_font_cooker --config %BUILD_TYPE%
if errorlevel 1 goto :vkr_build_failed
set "FONT_COOKER_BIN=%BUILD_DIR%\tools\vkr_font_cooker.exe"
if not exist "!FONT_COOKER_BIN!" set "FONT_COOKER_BIN=%BUILD_DIR%\tools\%BUILD_TYPE%\vkr_font_cooker.exe"
set "VKR_FONT_COOKER_BIN=!FONT_COOKER_BIN!"
call "%REPO_ROOT%\tools\cook_vkr_fonts.bat"
if errorlevel 1 goto :vkr_font_cook_failed
call "%REPO_ROOT%\tools\pack_vkt_textures.bat"
if errorlevel 1 goto :vkr_texture_pack_failed

echo Build completed successfully!
endlocal
exit /b 0

:vkr_cmake_configure_failed
echo CMake configure failed.
exit /b 1

:vkr_build_failed
echo Build failed.
exit /b 1

:vkr_texture_pack_failed
echo Texture packing failed.
exit /b 1

:vkr_font_cook_failed
echo Font cooking failed.
exit /b 1
