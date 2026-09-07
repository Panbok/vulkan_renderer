@echo off
setlocal EnableExtensions EnableDelayedExpansion

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
if "%~1"=="" goto cook_complete
call :cook "%~1"
if not "!errorlevel!"=="0" exit /b 1
shift
goto cook_arguments

:cook_defaults
call :cook "tests\fixtures\rendering\specgloss_factor_parity.gltf" || exit /b 1
call :cook "tests\fixtures\rendering\editor_nodes.gltf" || exit /b 1
call :cook "tests\fixtures\rendering\editor_lights.gltf" || exit /b 1
call :cook "assets\models\falcon.obj" || exit /b 1
call :cook "assets\models\sponza.obj" || exit /b 1
call :cook "assets\models\New_Sponza_001.gltf" || exit /b 1
call :cook "assets\models\NewSponza_Curtains_glTF.gltf" || exit /b 1
call :cook "assets\models\bistro-lights.gltf" || exit /b 1
call :cook "assets\models\bistrox.gltf" || exit /b 1
call :cook "assets\models\bistro.gltf" || exit /b 1
call :cook "assets\models\san-miguel-low-poly.obj" || exit /b 1
if exist "assets\models\bistro-lights.gltf" (
  "%VKR_MESH_COOKER_BIN%" --input "assets/models/bistro-lights.gltf" ^
    --output "assets/models/bistro-lights-main.vkb" ^
    --light-range "LMBR_000019c_Paris_StringLights_01_Yellow_Color=5.0" ^
    --light-range "LMBR_000019a_Paris_StringLights_01_Pink_Color=5.0" ^
    --light-range "LMBR_0000197_Paris_StringLights_01_Red_Color=5.0" ^
    --light-range "LMBR_0000199_Paris_StringLights_01_Green_Color=5.0" ^
    --light-range "LMBR_000019b_Paris_StringLights_01_Orange_Color=5.0" ^
    --light-range "LMBR_0000198_Paris_StringLights_01_Blue_Color=5.0"
  if errorlevel 1 exit /b 1
)
goto cook_complete

:cook_complete
exit /b 0

:cook
set "SOURCE=%~1"
if not exist "%SOURCE%" (
    REM Model sources are local and optional; strictness starts once one exists.
    echo Mesh cook step skipped missing source: %SOURCE% 1>&2
    exit /b 0
)
if "%VKR_MESH_COOK_STRICT_INPUTS%"=="1" if not exist "%SOURCE%.vkr.json" (
    echo Mesh cook step failed: required import sidecar is missing: %SOURCE%.vkr.json 1>&2
    exit /b 1
)
for %%F in ("%SOURCE%") do set "SOURCE_ABSOLUTE=%%~fF"
set "REPOSITORY_SOURCE=!SOURCE_ABSOLUTE:%REPO_ROOT%\=!"
if /i not "!REPOSITORY_SOURCE!"=="!SOURCE_ABSOLUTE!" set "SOURCE=!REPOSITORY_SOURCE!"
for %%F in ("%SOURCE%") do set "OUTPUT=%%~dpnF.vkb"
echo Cooking %SOURCE% ^> %OUTPUT%
"%VKR_MESH_COOKER_BIN%" --input "%SOURCE%" --output "%OUTPUT%"
exit /b %ERRORLEVEL%
