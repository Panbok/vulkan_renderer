@echo off
REM Build (RelWithDebInfo) script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

cd /d "%~dp0"

REM Configure CMake (RelWithDebInfo)
echo Configuring CMake (RelWithDebInfo)
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
where clang >nul 2>&1 >nul
if %errorlevel%==0 (
    where clang++ >nul 2>&1 >nul
    if %errorlevel%==0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)

cmake --fresh -S . -B build_release_info -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer (RelWithDebInfo)
cmake --build .\build_release_info --target vulkan_renderer vkr_harness --config RelWithDebInfo
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

REM Copy shaders to build_release_info/app/assets
echo Copying shaders to build_release_info/app/assets
if not exist build_release_info\app md build_release_info\app
if not exist build_release_info\app\assets md build_release_info\app\assets

dir assets\shaders\*.spv >nul 2>&1
if %errorlevel% equ 0 (
    copy /Y assets\shaders\*.spv build_release_info\app\assets\ >nul
) else (
    echo No .spv files to copy – skipping
)

echo RelWithDebInfo build completed successfully!
endlocal
