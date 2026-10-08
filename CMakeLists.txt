cmake_minimum_required(VERSION 3.21)
project(Screen2NVR LANGUAGES CXX RC)

add_executable(Screen2NVR ScreenCapture.cpp RtspServer.cpp Screen2ONVIF.cpp Settings.cpp Security.cpp TrayApp.cpp Screen2NVR.rc)
target_compile_features(Screen2NVR PRIVATE cxx_std_17)
target_compile_definitions(Screen2NVR PRIVATE UNICODE _UNICODE)
target_compile_options(Screen2NVR PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(Screen2NVR PRIVATE
    ws2_32 d3d11 d3dcompiler dxgi d2d1 dwrite mfplat mf mfuuid ole32 oleaut32
    shell32 advapi32 user32 gdi32 comctl32 comdlg32 msimg32 bcrypt crypt32 xmllite)
target_link_options(Screen2NVR PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)

if(MSVC)
    set_property(TARGET Screen2NVR PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
