@echo off
setlocal EnableDelayedExpansion

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
set "BUILD_DIR=build_bindless_vulkan_v0_%BUILD_TYPE%"

cd /d "%~dp0"
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

cmake --fresh -S . -B "%BUILD_DIR%" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --target vkr_bindless_vulkan_v0 --config %BUILD_TYPE%
if errorlevel 1 exit /b 1

set "V0_EXE=%CD%\%BUILD_DIR%\tools\vkr_bindless_vulkan_v0.exe"
if not exist "!V0_EXE!" set "V0_EXE=%CD%\%BUILD_DIR%\tools\%BUILD_TYPE%\vkr_bindless_vulkan_v0.exe"
echo Built !V0_EXE!
endlocal
