@echo off
REM Build (Release) script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

cd /d "%~dp0"
set "REPO_ROOT=%CD%"

REM Configure CMake (Release)
echo Configuring CMake (Release)
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
set "GEN_TOOLSET="
if /I "%GENERATOR%"=="-G Ninja" (
    where clang >nul 2>&1
    if !errorlevel! EQU 0 (
        where clang++ >nul 2>&1
        if !errorlevel! EQU 0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
    )
) else (
    where clang-cl >nul 2>&1
    if !errorlevel! EQU 0 set "GEN_TOOLSET=-T ClangCL"
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

cmake --fresh -S . -B build_release -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %GEN_TOOLSET% %COMPILERS% %BASH_ARG%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer (Release)
cmake --build .\build_release --target vulkan_renderer vkr_harness vkr_mesh_cooker vkr_font_cooker --config Release
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

set "FONT_COOKER_BIN=%CD%\build_release\tools\vkr_font_cooker.exe"
if not exist "%FONT_COOKER_BIN%" set "FONT_COOKER_BIN=%CD%\build_release\tools\Release\vkr_font_cooker.exe"
set "VKR_FONT_COOKER_BIN=%FONT_COOKER_BIN%"
call "%~dp0tools\cook_vkr_fonts.bat"
if %errorlevel% neq 0 (
    echo Font cooking failed.
    exit /b 1
)

call "%~dp0tools\pack_vkt_textures.bat"
if %errorlevel% neq 0 (
    echo Texture packing failed.
    exit /b 1
)

echo Release build completed successfully!
endlocal
