@echo off
setlocal EnableDelayedExpansion

rem Exit early if any commands fail
rem In Batch, 'exit /b 1' can be used after a command that might fail to achieve
rem a similar effect, but it needs to be checked explicitly.
rem For this script's purpose, we'll assume commands generally succeed or handle
rem errors as they occur within the command itself.

rem Ensure compile steps are run within the repository directory
pushd "%~dp0" || exit /b 1

set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1
if !errorlevel! EQU 0 (
    where clang++ >nul 2>&1
    if !errorlevel! EQU 0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

set "BASH_HINT="
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\usr\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\usr\bin\bash.exe" set "BASH_HINT=C:\msys64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\usr\bin\bash.exe" set "BASH_HINT=C:\mingw64\usr\bin\bash.exe"

set "BASH_ARG="
if not "!BASH_HINT!"=="" set "BASH_ARG=-DBASH_EXECUTABLE:FILEPATH=!BASH_HINT!"
set "VKR_BASH_ENV_FILE=%CD%\tools\vkr_bash_env.sh"
if not "!BASH_HINT!"=="" if exist "!VKR_BASH_ENV_FILE!" set "BASH_ENV=!VKR_BASH_ENV_FILE!"

echo !BASH_HINT! | findstr /I /C:"C:\msys64\" /C:"C:\mingw64\" >nul 2>&1
if !errorlevel! EQU 0 if exist "C:\msys64\usr\bin" set "PATH=C:\msys64\usr\bin;C:\msys64\bin;%PATH%"
if !errorlevel! EQU 0 if exist "C:\mingw64\usr\bin" set "PATH=C:\mingw64\usr\bin;C:\mingw64\bin;%PATH%"

rem The CPU suite owns a dedicated tree and deliberately refreshes its cache.
cmake --fresh -B build_test -S . -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Debug %GENERATOR% %COMPILERS% %BASH_ARG%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    popd
    exit /b 1
)

rem Build only the test target
cmake --build ./build_test --target vulkan_renderer_tester --config Debug
if %errorlevel% neq 0 (
    echo CMake build failed for vulkan_renderer_tester.
    popd
    exit /b 1
)

rem Return to the original directory
popd

rem Unit fixtures intentionally exercise source and legacy compatibility.
set "VKR_TEXTURE_VKT_STRICT=0"
set "VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK=1"
set "VKR_TEXTURE_VKT_ALLOW_LEGACY=1"

rem Execute the test runner (single-config first, then multi-config fallback).
set "TEST_EXE=%~dp0build_test\tests\vulkan_renderer_tester.exe"
if not exist "!TEST_EXE!" set "TEST_EXE=%~dp0build_test\tests\Debug\vulkan_renderer_tester.exe"
"!TEST_EXE!" %*

if %errorlevel% neq 0 (
    echo Test runner exited with an error.
    exit /b 1
)

endlocal
