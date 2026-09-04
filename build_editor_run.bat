@echo off
setlocal

set "VKR_BUILD_TARGET=vkr_editor"
set "VKR_BUILD_LABEL=VKR editor"
set "VKR_RUN_SUBDIR=editor"
set "VKR_RUN_BINARY=vkr_editor"
set "VKR_RUN_LABEL=VKR Editor"
call "%~dp0build_run.bat" %*
exit /b %errorlevel%
