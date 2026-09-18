@echo off
REM Build CloudVault client (cmd; Qt6 + MinGW). ASCII-only to avoid codepage issues.
setlocal
set QT=D:\Qt\6.9.3\mingw_64
set MINGW=D:\Qt\Tools\mingw1310_64
set BD=%~dp0..\build_official

if not exist "%QT%\bin\qmake.exe"  echo [ERROR] qmake not found: %QT%\bin\qmake.exe & exit /b 2
if not exist "%MINGW%\bin\g++.exe" echo [ERROR] g++ not found: %MINGW%\bin\g++.exe & exit /b 2

if not exist "%BD%" mkdir "%BD%"
set PATH=%QT%\bin;%MINGW%\bin;%PATH%
cd /d "%BD%"
echo ==^> qmake
"%QT%\bin\qmake.exe" ..\File.pro || exit /b 1
echo ==^> mingw32-make -j4
"%MINGW%\bin\mingw32-make.exe" -j4 || exit /b 1
echo.
echo [OK] built: %~dp0..\bin\CloudVault.exe
echo First run / new machine: deploy Qt runtime via  bash scripts/deploy_windows.sh
endlocal
