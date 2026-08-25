@echo off
REM Build library target script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

cd /d "%~dp0"

REM Configure CMake
echo Configuring CMake for renderer_lib
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1 >nul
if %errorlevel%==0 (
    where clang++ >nul 2>&1 >nul
    if %errorlevel%==0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

cmake -S . -B build_lib -U CMAKE_TOOLCHAIN_FILE -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build the library target
echo Building renderer_lib
cmake --build .\build_lib --target renderer_lib
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

call "%~dp0tools\pack_vkt_textures.bat"
if %errorlevel% neq 0 (
    echo Texture packing failed.
    exit /b 1
)

echo Library build completed successfully!
endlocal
