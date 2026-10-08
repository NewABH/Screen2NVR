@echo off
setlocal

call build-release.cmd
if errorlevel 1 exit /b 1

set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo ERROR: Inno Setup 6 is not installed.
    echo Install it with: winget install --id JRSoftware.InnoSetup --exact
    exit /b 1
)

"%ISCC%" "installer\Screen2NVR.iss"
if errorlevel 1 exit /b 1

echo Installer build completed successfully: dist\Screen2NVR-Setup-x64.exe
endlocal
