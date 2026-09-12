@echo off
setlocal
call "%~dp0build.bat" RelWithDebInfo
exit /b %errorlevel%
