@echo off
setlocal
cd /d "%~dp0"
set "VKR_LIBRARY_SET=%~1"
if "%VKR_LIBRARY_SET%"=="" set "VKR_LIBRARY_SET=renderer"
if "%VKR_LIBRARY_SET%"=="renderer" (
  set "VKR_RUNTIME=OFF"
  set "VKR_TARGET=vkr_renderer_example"
) else if "%VKR_LIBRARY_SET%"=="runtime" (
  set "VKR_RUNTIME=ON"
  set "VKR_TARGET=vkr_host_example"
) else (
  echo Usage: %~nx0 [renderer^|runtime]
  exit /b 2
)
set "VKR_BUILD_DIR=build_lib_%VKR_LIBRARY_SET%"
set "VKR_GENERATOR="
where ninja >nul 2>&1 && set "VKR_GENERATOR=-G Ninja"
set "VKR_COMPILERS="
where clang >nul 2>&1
if not errorlevel 1 (
  where clang++ >nul 2>&1
  if not errorlevel 1 set "VKR_COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)
cmake --fresh -S . -B "%VKR_BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVKR_BUILD_RUNTIME=%VKR_RUNTIME% -DVKR_BUILD_EXAMPLES=ON -DVKR_BUILD_TOOLS=OFF -DVKR_BUILD_APP=OFF -DVKR_BUILD_EDITOR=OFF -DVKR_BUILD_HARNESS=OFF -DVKR_BUILD_TESTS=OFF %VKR_GENERATOR% %VKR_COMPILERS%
if errorlevel 1 exit /b %errorlevel%
cmake --build "%VKR_BUILD_DIR%" --target %VKR_TARGET% --config Release
exit /b %errorlevel%
