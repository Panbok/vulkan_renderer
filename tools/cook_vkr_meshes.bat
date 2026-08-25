@echo off
setlocal EnableExtensions

cd /d "%~dp0\.."
set "REPO_ROOT=%CD%"
if not "%VKR_MESH_COOKER_BIN%"=="" goto cooker_ready
call "%REPO_ROOT%\build_release.bat"
set "VKR_MESH_COOKER_BIN=%REPO_ROOT%\build_release\tools\Release\vkr_mesh_cooker.exe"
if not exist "%VKR_MESH_COOKER_BIN%" set "VKR_MESH_COOKER_BIN=%REPO_ROOT%\build_release\tools\vkr_mesh_cooker.exe"

:cooker_ready
if not exist "%VKR_MESH_COOKER_BIN%" (
    echo Mesh cook step failed: vkr_mesh_cooker was not found. 1>&2
    exit /b 2
)

if "%~1"=="" goto cook_defaults

:cook_arguments
if "%~1"=="" goto pack_textures
call :cook "%~1"
if errorlevel 1 exit /b 1
shift
goto cook_arguments

:cook_defaults
call :cook "assets\models\falcon.obj" || exit /b 1
call :cook "assets\models\sponza.obj" || exit /b 1
call :cook "assets\models\New_Sponza_001.gltf" || exit /b 1
call :cook "assets\models\NewSponza_Curtains_glTF.gltf" || exit /b 1
call :cook "assets\models\bistro-lights.gltf" || exit /b 1
call :cook "assets\models\bistrox.gltf" || exit /b 1
call :cook "assets\models\bistro.gltf" || exit /b 1
call :cook "assets\models\san-miguel-low-poly.obj" || exit /b 1
goto pack_textures

:pack_textures
call "%REPO_ROOT%\tools\pack_vkt_textures.bat"
if errorlevel 1 exit /b 1
exit /b 0

:cook
set "SOURCE=%~1"
if not exist "%SOURCE%" (
    echo Mesh cook step skipped missing source: %SOURCE% 1>&2
    exit /b 0
)
for %%F in ("%SOURCE%") do set "OUTPUT=%%~dpnF.vkb"
echo Cooking %SOURCE% ^> %OUTPUT%
"%VKR_MESH_COOKER_BIN%" --input "%SOURCE%" --output "%OUTPUT%"
exit /b %ERRORLEVEL%
