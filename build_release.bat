@echo off
setlocal
call "%~dp0build.bat" Release
exit /b %errorlevel%
