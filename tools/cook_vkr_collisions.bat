@echo off
setlocal
set "COLLISION_CALLER_DIR=%CD%"
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
pushd "%REPO_ROOT%" || exit /b 1
if not defined VKR_BUILD_DIR set "VKR_BUILD_DIR=build_release"
for %%I in ("%VKR_BUILD_DIR%") do set "VKR_BUILD_DIR=%%~fI"
popd
set "VKR_BUILD_TARGET=vkr_collision_cooker"
set "VKR_BUILD_LABEL=VKR collision cooker"
call "%REPO_ROOT%\build.bat" Release
set "COLLISION_BUILD_RESULT=%errorlevel%"
cd /d "%COLLISION_CALLER_DIR%"
if not "%COLLISION_BUILD_RESULT%"=="0" exit /b %COLLISION_BUILD_RESULT%
set "COOKER_BIN=%VKR_BUILD_DIR%\tools\vkr_collision_cooker.exe"
if not exist "%COOKER_BIN%" set "COOKER_BIN=%VKR_BUILD_DIR%\tools\Release\vkr_collision_cooker.exe"
"%COOKER_BIN%" %*
exit /b %errorlevel%
