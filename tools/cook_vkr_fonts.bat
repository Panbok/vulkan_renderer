@echo off
setlocal EnableExtensions EnableDelayedExpansion

pushd "%~dp0.." || exit /b 1
set "REPO_ROOT=%CD%"
set "COOKER_BIN=%VKR_FONT_COOKER_BIN%"
if not "%COOKER_BIN%"=="" goto :cooker_ready

set "BUILD_DIR=%VKR_FONT_COOKER_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%REPO_ROOT%\build_font_cooker"
set "GENERATOR="
where ninja >nul 2>&1 && set "GENERATOR=-G Ninja"
set "COMPILERS="
where clang >nul 2>&1
if !errorlevel! EQU 0 (
    where clang++ >nul 2>&1
    if !errorlevel! EQU 0 set "COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
)
set "BASH_HINT="
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%ProgramFiles%\Git\usr\bin\bash.exe" set "BASH_HINT=%ProgramFiles%\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "%LocalAppData%\Programs\Git\usr\bin\bash.exe" set "BASH_HINT=%LocalAppData%\Programs\Git\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\usr\bin\bash.exe" set "BASH_HINT=C:\msys64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\msys64\bin\bash.exe" set "BASH_HINT=C:\msys64\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\usr\bin\bash.exe" set "BASH_HINT=C:\mingw64\usr\bin\bash.exe"
if "!BASH_HINT!"=="" if exist "C:\mingw64\bin\bash.exe" set "BASH_HINT=C:\mingw64\bin\bash.exe"
set "BASH_ARG="
if not "!BASH_HINT!"=="" set "BASH_ARG=-DBASH_EXECUTABLE:FILEPATH=!BASH_HINT!"
set "VKR_BASH_ENV_FILE=%REPO_ROOT%\tools\vkr_bash_env.sh"
if not "!BASH_HINT!"=="" if exist "!VKR_BASH_ENV_FILE!" set "BASH_ENV=!VKR_BASH_ENV_FILE!"
echo !BASH_HINT! | findstr /I /C:"C:\msys64\" /C:"C:\mingw64\" >nul 2>&1
if !errorlevel! EQU 0 if exist "C:\msys64\usr\bin" set "PATH=C:\msys64\usr\bin;C:\msys64\bin;%PATH%"
if !errorlevel! EQU 0 if exist "C:\mingw64\usr\bin" set "PATH=C:\mingw64\usr\bin;C:\mingw64\bin;%PATH%"
echo Building vkr_font_cooker in %BUILD_DIR%
cmake -S "%REPO_ROOT%" -B "%BUILD_DIR%" -U CMAKE_TOOLCHAIN_FILE -DCMAKE_BUILD_TYPE:STRING=Release %GENERATOR% %COMPILERS% %BASH_ARG%
if errorlevel 1 goto :configure_failed
cmake --build "%BUILD_DIR%" --target vkr_font_cooker --config Release
if errorlevel 1 goto :build_failed
if exist "%BUILD_DIR%\tools\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\tools\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\tools\Release\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\tools\Release\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\vkr_font_cooker.exe"
if "%COOKER_BIN%"=="" if exist "%BUILD_DIR%\Release\vkr_font_cooker.exe" set "COOKER_BIN=%BUILD_DIR%\Release\vkr_font_cooker.exe"

:cooker_ready
if not exist "%COOKER_BIN%" (
    echo Font cook step failed: vkr_font_cooker was not found. 1>&2
    popd
    exit /b 2
)
if not "%~1"=="" goto :cook_arguments
"%COOKER_BIN%" --config "assets\fonts\UbuntuMono-cooked.fontcfg"
if errorlevel 1 goto :cook_failed
goto :done

:cook_arguments
if "%~1"=="" goto :done
"%COOKER_BIN%" --config "%~1"
if errorlevel 1 goto :cook_failed
shift
goto :cook_arguments

:configure_failed
echo CMake configure failed. 1>&2
popd
exit /b 1
:build_failed
echo vkr_font_cooker build failed. 1>&2
popd
exit /b 1
:cook_failed
echo Font cook step failed. 1>&2
popd
exit /b 1
:done
popd
exit /b 0
