@echo off
REM Build script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

REM Optional arg1: BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
if "%VKR_BUILD_TARGET%"=="" set "VKR_BUILD_TARGET=vulkan_renderer"
if "%VKR_BUILD_LABEL%"=="" set "VKR_BUILD_LABEL=VKR app"
set "BUILD_TARGETS=%VKR_BUILD_TARGET%"
if "%VKR_BUILD_TARGET%"=="vulkan_renderer" set "BUILD_TARGETS=%BUILD_TARGETS% vkr_harness"
if "%VKR_BUILD_TARGET%"=="vkr_editor" set "BUILD_TARGETS=%BUILD_TARGETS% vkr_harness"

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

if /I "%BUILD_TYPE%"=="Debug" set "BUILD_TYPE=Debug"
if /I "%BUILD_TYPE%"=="Release" set "BUILD_TYPE=Release"
if /I "%BUILD_TYPE%"=="RelWithDebInfo" set "BUILD_TYPE=RelWithDebInfo"
if /I "%BUILD_TYPE%"=="MinSizeRel" set "BUILD_TYPE=MinSizeRel"

set "SANITIZER_PROFILE="
if "%VKR_DEBUG_SANITIZER%"=="" set "VKR_DEBUG_SANITIZER=default"
for %%S in (default address thread memory leak none) do if /I "%VKR_DEBUG_SANITIZER%"=="%%S" set "SANITIZER_PROFILE=%%S"
if "%SANITIZER_PROFILE%"=="" (
    echo Error: VKR_DEBUG_SANITIZER must be default, address, thread, memory, leak, or none.
    exit /b 1
)
set "VKR_DEBUG_SANITIZER=%SANITIZER_PROFILE%"
if not "%VKR_DEBUG_SANITIZER%"=="default" (
    if not "%BUILD_TYPE%"=="Debug" (
        echo Error: explicit VKR_DEBUG_SANITIZER profiles require a Debug build.
        exit /b 1
    )
    set "BUILD_DIR=build_debug_%VKR_DEBUG_SANITIZER%"
)

if not "%VKR_BUILD_DIR%"=="" set "BUILD_DIR=%VKR_BUILD_DIR%"

cd /d "%~dp0"
set "REPO_ROOT=%CD%"

echo Configuring CMake (%BUILD_TYPE%)
echo Using build directory: %BUILD_DIR%
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "GEN_TOOLSET="
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
if not "!BASH_HINT!"=="" set BASH_ARG="-DBASH_EXECUTABLE:FILEPATH=!BASH_HINT!"

set "VKR_BASH_ENV_FILE=%REPO_ROOT%\tools\vkr_bash_env.sh"
if not "!BASH_HINT!"=="" if exist "!VKR_BASH_ENV_FILE!" set "BASH_ENV=!VKR_BASH_ENV_FILE!"

echo !BASH_HINT! | findstr /I /C:"C:\msys64\" /C:"C:\mingw64\" >nul 2>&1
if !errorlevel! EQU 0 if exist "C:\msys64\usr\bin" set "PATH=C:\msys64\usr\bin;C:\msys64\bin;%PATH%"
if !errorlevel! EQU 0 if exist "C:\mingw64\usr\bin" set "PATH=C:\mingw64\usr\bin;C:\mingw64\bin;%PATH%"

REM Preserve the generator, compiler and toolchain already selected in this tree.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    set "GENERATOR="
    set "COMPILERS="
) else if "%GENERATOR%"=="" (
    set "COMPILERS="
    where clang-cl >nul 2>&1 && set "GEN_TOOLSET=-T ClangCL"
)
set "LOGGING_ARG="
if not "%VKR_EDITOR_LOGGING%"=="" set "LOGGING_ARG=-DVKR_EDITOR_LOGGING:BOOL=%VKR_EDITOR_LOGGING%"
set "METRICS_ARG="
if not "%VKR_METRICS_ENABLED%"=="" (
    set "METRICS_VALUE="
    for %%V in (0 OFF FALSE) do if /I "%VKR_METRICS_ENABLED%"=="%%V" set "METRICS_VALUE=OFF"
    for %%V in (1 ON TRUE) do if /I "%VKR_METRICS_ENABLED%"=="%%V" set "METRICS_VALUE=ON"
    if "!METRICS_VALUE!"=="" (
        echo Error: VKR_METRICS_ENABLED must be ON/OFF or 1/0.
        exit /b 1
    )
    set "METRICS_ARG=-DVKR_METRICS_ENABLED:BOOL=!METRICS_VALUE!"
)
cmake -S . -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DVKR_DEBUG_SANITIZER:STRING=%VKR_DEBUG_SANITIZER% -DVKR_BUILD_RUNTIME=ON -DVKR_BUILD_TOOLS=ON -DVKR_BUILD_APP=ON -DVKR_BUILD_EDITOR=ON -DVKR_BUILD_HARNESS=ON -DVKR_BUILD_TESTS=ON -DVKR_BUILD_EXAMPLES=ON %LOGGING_ARG% %METRICS_ARG% %GENERATOR% %GEN_TOOLSET% %COMPILERS% %BASH_ARG%
if errorlevel 1 goto :vkr_cmake_configure_failed

echo Building %VKR_BUILD_LABEL% (%BUILD_TYPE%)
cmake --build "%BUILD_DIR%" --target %BUILD_TARGETS% --config %BUILD_TYPE%
if errorlevel 1 goto :vkr_build_failed
echo Build completed successfully!
endlocal
exit /b 0

:vkr_cmake_configure_failed
echo CMake configure failed.
exit /b 1

:vkr_build_failed
echo Build failed.
exit /b 1
