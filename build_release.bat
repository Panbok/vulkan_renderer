@echo off
REM Build (Release) script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

cd /d "%~dp0"

REM Configure CMake (Release)
echo Configuring CMake (Release)
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"

set "COMPILERS="
set "GEN_TOOLSET="
if /I "%GENERATOR%"=="-G Ninja" (
    where clang >nul 2>&1
    if %errorlevel%==0 (
        where clang++ >nul 2>&1
        if %errorlevel%==0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
    )
) else (
    where clang-cl >nul 2>&1
    if %errorlevel%==0 set "GEN_TOOLSET=-T ClangCL"
)

cmake --fresh -S . -B build_release -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %GEN_TOOLSET% %COMPILERS%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer (Release)
cmake --build .\build_release --target vulkan_renderer vkr_harness vkr_mesh_cooker --config Release
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

echo Release build completed successfully!
endlocal
