@echo off
REM Build (RelWithDebInfo) script for vulkan_renderer (Windows)
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

set "TOOLCHAIN="
if defined VCPKG_ROOT set "TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

cmake -S . -B build_release_info -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %TOOLCHAIN%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer (RelWithDebInfo)
cmake --build .\build_release_info --target vulkan_renderer --config RelWithDebInfo
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

REM Copy shaders to build_release_info/app/assets
echo Copying shaders to build_release_info/app/assets
if not exist build_release_info\app md build_release_info\app
if not exist build_release_info\app\assets md build_release_info\app\assets

dir assets\*.spv >nul 2>&1
if %errorlevel% equ 0 (
    copy /Y assets\*.spv build_release_info\app\assets\ >nul
) else (
    echo No .spv files to copy – skipping
)

echo RelWithDebInfo build completed successfully!
endlocal


