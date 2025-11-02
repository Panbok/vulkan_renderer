@echo off
REM Build script for vulkan_renderer (Windows)
setlocal EnableDelayedExpansion

REM Optional arg1: BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

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

REM Configure CMake
echo Configuring CMake (%BUILD_TYPE%)
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

cmake -S . -B build -DCMAKE_BUILD_TYPE:STRING=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE %GENERATOR% %COMPILERS% %TOOLCHAIN%
if %errorlevel% neq 0 (
    echo CMake configure failed.
    exit /b 1
)

REM Build target
echo Building vulkan_renderer
cmake --build .\build --target vulkan_renderer --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

REM Copy shaders from root assets into build output
echo Copying shaders to build/app directory
if not exist build\app md build\app
if not exist build\app\assets md build\app\assets

dir assets\shaders\*.spv >nul 2>&1
if %errorlevel% equ 0 (
    copy /Y assets\shaders\*.spv build\app\assets\ >nul
) else (
    echo No .spv files to copy to build/app - skipping
)

echo Build completed successfully!
endlocal