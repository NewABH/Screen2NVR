@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "x64\settings-tests" mkdir "x64\settings-tests"
cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    tools\settings-persistence-tests.cpp Settings.cpp ^
    /Fo"x64\settings-tests\\" /Fe"x64\settings-tests\SettingsPersistenceTests.exe" ^
    ole32.lib shell32.lib advapi32.lib /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
x64\settings-tests\SettingsPersistenceTests.exe
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    tools\settings-tests.cpp Settings.cpp Security.cpp TrayApp.cpp Screen2ONVIF.cpp ^
    /Fo"x64\settings-tests\\" /Fe"x64\settings-tests\SettingsTests.exe" ^
    ws2_32.lib dxgi.lib d2d1.lib dwrite.lib ole32.lib shell32.lib advapi32.lib user32.lib gdi32.lib ^
    comctl32.lib comdlg32.lib msimg32.lib bcrypt.lib crypt32.lib xmllite.lib ^
    /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
x64\settings-tests\SettingsTests.exe
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /O2 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE ^
    tools\security-server.cpp Settings.cpp Security.cpp RtspServer.cpp Screen2ONVIF.cpp ^
    /Fo"x64\settings-tests\\" /Fe"x64\settings-tests\SecurityServer.exe" ^
    ws2_32.lib ole32.lib shell32.lib advapi32.lib bcrypt.lib crypt32.lib xmllite.lib ^
    /link /INCREMENTAL:NO
exit /b %errorlevel%
