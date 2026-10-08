@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "x64\code-review" mkdir "x64\code-review"
cl.exe /nologo /std:c++17 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    /analyze /c ScreenCapture.cpp RtspServer.cpp Screen2ONVIF.cpp Settings.cpp Security.cpp TrayApp.cpp ^
    /Fo"x64\code-review\\"
exit /b %errorlevel%
