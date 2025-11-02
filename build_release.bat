@echo off
REM Build (Release) script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

cd /d "%~dp0"

REM Compile shaders from root assets directory
echo Compiling shaders
pushd assets 2>nul
if errorlevel 1 (
    echo Error: assets directory not found.
    exit /b 1
)

where slangc >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: slangc compiler not found. Please install slangc.
    popd
    exit /b 1
)

pushd shaders 2>nul
if not errorlevel 1 (
    dir *.slang >nul 2>&1
    if %errorlevel% equ 0 (
        for %%f in (*.slang) do (
            echo Compiling %%f
            slangc -target spirv -o "%%~nf.spv" "%%f"
        )
    ) else (
        echo No .slang files found to compile
    )
    popd
)
popd

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

set "TOOLCHAIN="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" set "TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=""%VCPKG_ROOT%\scripts\builds

cmake -S . -B build_release -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %GEN_TOOLSET% %COMPILERS% %TOOLCHAIN%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer (Release)
cmake --build .\build_release --target vulkan_renderer --config Release
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

REM Copy shaders to build_release/app/assets
echo Copying shaders to build_release/app/assets
if not exist build_release\app md build_release\app
if not exist build_release\app\assets md build_release\app\assets

dir assets\shaders\*.spv >nul 2>&1
if %errorlevel% equ 0 (
    copy /Y assets\shaders\*.spv build_release\app\assets\ >nul
) else (
    echo No .spv files to copy – skipping
)

echo Release build completed successfully!
endlocal


