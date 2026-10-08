@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "x64\encoder-tests" mkdir "x64\encoder-tests"
cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    tools\encoder-tests.cpp Settings.cpp Security.cpp TrayApp.cpp RtspServer.cpp Screen2ONVIF.cpp ^
    /Fo"x64\encoder-tests\\" /Fe"x64\encoder-tests\EncoderTests.exe" ^
    ws2_32.lib d3d11.lib d3dcompiler.lib dxgi.lib d2d1.lib dwrite.lib mfplat.lib mf.lib mfuuid.lib ^
    ole32.lib oleaut32.lib shell32.lib advapi32.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
    msimg32.lib bcrypt.lib crypt32.lib xmllite.lib /link /INCREMENTAL:NO
exit /b %errorlevel%
