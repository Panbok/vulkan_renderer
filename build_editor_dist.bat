@echo off
setlocal

rem Builds the Release editor and installs the relocatable editor distribution
rem (docs/adr/078-project-build-and-packaging.md) into the given folder, by
rem default build_release\dist\VKR Editor. The folder is replaced.
set "PREFIX=%~1"
if "%PREFIX%"=="" set "PREFIX=%~dp0build_release\dist\VKR Editor"
call "%~dp0build_editor.bat" Release
if errorlevel 1 exit /b %errorlevel%
if exist "%PREFIX%" rmdir /s /q "%PREFIX%"
cmake --install "%~dp0build_release" --prefix "%PREFIX%" --component editor --config Release
if errorlevel 1 exit /b %errorlevel%
echo Installed the editor distribution in %PREFIX%
exit /b 0
