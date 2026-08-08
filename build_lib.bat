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

REM Copy shaders to build_lib/lib/assets
echo Copying shaders to build_lib/lib/assets
if not exist build_lib\lib md build_lib\lib
if not exist build_lib\lib\assets md build_lib\lib\assets

dir assets\shaders\*.spv >nul 2>&1
if %errorlevel% equ 0 (
    copy /Y assets\shaders\*.spv build_lib\lib\assets\ >nul
) else (
    echo No .spv files to copy – skipping
)

echo Library build completed successfully!
endlocal
