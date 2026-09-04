@echo off
setlocal

set "VKR_BUILD_TARGET=vkr_editor"
set "VKR_BUILD_LABEL=VKR editor"
call "%~dp0build.bat" %*
exit /b %errorlevel%
