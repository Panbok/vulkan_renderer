@echo off
REM Build script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

REM Optional arg1: BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

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

cmake -S . -B "%BUILD_DIR%" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 goto :vkr_cmake_configure_failed

echo Building vulkan_renderer (%BUILD_TYPE%)
cmake --build ".\%BUILD_DIR%" --target vulkan_renderer vkr_harness --config %BUILD_TYPE%
if errorlevel 1 goto :vkr_build_failed

echo Copying shaders to %BUILD_DIR%/app directory
if not exist "%BUILD_DIR%\app\assets" md "%BUILD_DIR%\app\assets"
dir assets\shaders\*.spv >nul 2>&1
if errorlevel 1 goto :vkr_no_spv_to_copy
copy /Y assets\shaders\*.spv "%BUILD_DIR%\app\assets" >nul
goto :vkr_after_spv_copy
:vkr_no_spv_to_copy
echo No .spv files to copy to %BUILD_DIR%/app - skipping
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
