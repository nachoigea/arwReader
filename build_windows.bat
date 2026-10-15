@echo off
REM ===========================================================================
REM  Compila e instala arwReader para una versión de Nuke en Windows.
REM
REM  Uso:   build_windows.bat "C:\Program Files\Nuke14.0v5"
REM         build_windows.bat "C:\Program Files\Nuke15.1v3" "Visual Studio 17 2022"
REM         build_windows.bat "C:\Program Files\Nuke14.0v5" "Visual Studio 17 2022" v142
REM  (3er argumento opcional = toolset; v142 permite compilar para Nuke 14 con VS2022)
REM
REM  Generador recomendado: Nuke 14 -> "Visual Studio 16 2019" (toolset v142)
REM                         Nuke 15/16 -> "Visual Studio 17 2022"
REM  Se instala en %USERPROFILE%\.nuke\arwReader\<versión>\
REM ===========================================================================
setlocal
if "%~1"=="" (
  echo Uso: %~nx0 "C:\Program Files\Nuke14.0v5" ["Visual Studio 16 2019"]
  exit /b 1
)
set "NUKE_ROOT=%~1"
set "GEN=%~2"
if "%GEN%"=="" set "GEN=Visual Studio 16 2019"
set "TOOLSET_ARG="
if not "%~3"=="" set "TOOLSET_ARG=-T %~3"

for %%I in ("%NUKE_ROOT%") do set "NUKE_NAME=%%~nxI"
set "BUILD_DIR=%~dp0build-%NUKE_NAME%"
set "PREFIX=%USERPROFILE%\.nuke\arwReader"

cmake -S "%~dp0." -B "%BUILD_DIR%" -G "%GEN%" -A x64 %TOOLSET_ARG% -DNUKE_ROOT="%NUKE_ROOT%" || exit /b 1
cmake --build "%BUILD_DIR%" --config Release --parallel || exit /b 1
cmake --install "%BUILD_DIR%" --config Release --prefix "%PREFIX%" || exit /b 1

echo.
echo Instalado en %PREFIX%
echo Si aun no lo has hecho, anade esta linea a %USERPROFILE%\.nuke\init.py :
echo     nuke.pluginAddPath("arwReader")
endlocal
