@echo off
setlocal

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: Visual Studio 2022 Build Tools with C++ workload not found.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

if not exist "x64\Release" mkdir "x64\Release"

rc.exe /nologo /fo"x64\Release\Screen2NVR.res" Screen2NVR.rc
if errorlevel 1 exit /b 1

cl.exe /nologo /std:c++17 /O2 /Ob2 /EHsc /MT /DNDEBUG /W4 /permissive- /utf-8 ^
    /DUNICODE /D_UNICODE ^
    ScreenCapture.cpp RtspServer.cpp Screen2ONVIF.cpp Settings.cpp Security.cpp TrayApp.cpp ^
    /Fo"x64\Release\\" /Fd"x64\Release\Screen2NVR.pdb" ^
    /Fe"x64\Release\Screen2NVR.exe" ^
    ws2_32.lib d3d11.lib d3dcompiler.lib dxgi.lib d2d1.lib dwrite.lib ^
    mfplat.lib mf.lib mfuuid.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib user32.lib gdi32.lib ^
    comctl32.lib comdlg32.lib msimg32.lib bcrypt.lib crypt32.lib xmllite.lib ^
    "x64\Release\Screen2NVR.res" ^
    /link /INCREMENTAL:NO /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
if errorlevel 1 exit /b 1

echo Release x64 build completed successfully.
endlocal
