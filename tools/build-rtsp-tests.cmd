@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "x64\rtsp-tests" mkdir "x64\rtsp-tests"
cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    tools\rtsp-packetization-tests.cpp Security.cpp Settings.cpp ^
    /Fo"x64\rtsp-tests\\" /Fe"x64\rtsp-tests\RtspPacketizationTests.exe" ^
    ws2_32.lib bcrypt.lib crypt32.lib xmllite.lib ole32.lib shell32.lib advapi32.lib /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
x64\rtsp-tests\RtspPacketizationTests.exe
exit /b %errorlevel%
