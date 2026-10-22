@echo off
REM ===========================================================================
REM  CloudVault - portable packaging for Windows
REM
REM  Output: dist\CloudVault-portable\      portable folder, double-click the exe
REM          dist\CloudVault-portable.zip   zip for USB drive
REM
REM  Why: bin\ is already install-free because Qt DLLs are bundled. What was
REM  missing is that config travels with the app: dropping portable.txt next to
REM  the exe makes the client keep config/store/logs/cache/downloads under
REM  exe\portable\, so server URL and token survive moving to another machine.
REM
REM  KEEP THIS FILE ASCII-ONLY and do not put parentheses inside REM lines:
REM  cmd.exe decodes .bat as GBK and parentheses inside an if-block break
REM  command parsing. Both were hit while writing this script.
REM ===========================================================================
setlocal
set ROOT=%~dp0..
set SRC=%ROOT%\bin
set DIST=%ROOT%\dist
set OUT=%DIST%\CloudVault-portable
set ZIP=%DIST%\CloudVault-portable.zip

if not exist "%SRC%\CloudVault.exe" (
    echo [ERROR] %SRC%\CloudVault.exe not found. Build the client first.
    exit /b 1
)

echo [1/4] cleaning old output
if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%ZIP%" del /q "%ZIP%"
mkdir "%OUT%" 2>nul

echo [2/4] copying bin
xcopy "%SRC%\*" "%OUT%\" /E /I /Q /Y >nul
if errorlevel 1 (
    echo [ERROR] copy failed.
    exit /b 1
)

echo [3/4] stripping leftovers and writing portable marker
del /q "%OUT%\*.prelink" 2>nul
del /q "%OUT%\*.pdb" 2>nul
del /q "%OUT%\CloudVault.exe.locked-*" 2>nul
del /q "%OUT%\*.log" 2>nul
> "%OUT%\portable.txt" echo CloudVault portable mode marker. Do not delete.

echo [4/4] zipping
REM Use zip.exe when present. bsdtar writes zip entries STORED so it saves nothing.
where zip >nul 2>nul
if errorlevel 1 goto nozip
pushd "%OUT%"
zip -q -r -9 "..\CloudVault-portable.zip" .
if errorlevel 1 goto nozip_pop
popd
goto report

:nozip_pop
popd
:nozip
echo [WARN] zip step skipped or failed - the folder is still fully usable.

:report
echo.
echo Done.
echo   folder: %OUT%
echo   zip   : %ZIP%
if exist "%ZIP%" for %%F in ("%ZIP%") do echo   zip size: %%~zF bytes
echo.
echo Usage: copy the folder or unzip the zip to a USB drive, then run the exe.
echo        Config will be created under portable\ next to the exe.
endlocal
