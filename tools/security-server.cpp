#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include "../RtspServer.h"
#include "../Screen2ONVIF.h"

int wmain(int argc, wchar_t* argv[])
{
    const bool open = argc > 1 && std::wstring(argv[1]) == L"--open";
    const bool probeTest = argc > 1 && std::wstring(argv[1]) == L"--probe-test";
    const bool traceTest = argc > 1 && std::wstring(argv[1]) == L"--trace-test";
    const bool clientsTest = argc > 1 && std::wstring(argv[1]) == L"--clients-test";
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    AppSettings settings;
    settings.cameraName = L"Касса №5 & <Вход> \"GetProfiles\"";
    settings.overlayTemplate = open ? L"{date} {time}" : L"{camera}\\n{date} {time}";
    settings.onvifPort = open ? 18001 : 18000;
    settings.rtspPort = open ? 18555 : 18554;
    settings.rtspPath = L"pc-screen"; // Keep regression coverage for existing installations.
    settings.authenticationEnabled = !open;
    settings.userName = L"test-user";
    settings.password = L"test-password";
    settings.allowedIpAddresses = open ? L"" : L"127.0.0.2";
    if (clientsTest) settings.allowedIpAddresses = L"127.0.0.2,127.0.0.3";
    try
    {
        StartEmbeddedOnvifServer(settings, {}, open ? 13703 : 13702);
        std::mutex logMutex;
        std::vector<std::string> diagnostics;
        RtspServer server([&](const std::string& message) {
            if (message.find("RTSP peer=") != 0) return;
            std::lock_guard<std::mutex> lock(logMutex);
            diagnostics.push_back(message);
        });
        server.Start(settings.rtspPort, "pc-screen", settings.authenticationEnabled,
                     "test-user", "test-password", WideToUtf8String(settings.allowedIpAddresses), clientsTest ? "pc-screen-sub" : "");
        const uint8_t frame[] = { 0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0xce,0,0,0,1,0x65,1 };
        server.PublishAccessUnit(frame, sizeof(frame), 0);
        if (clientsTest) server.PublishAccessUnit(frame, sizeof(frame), 0, 1);
        std::atomic_bool publishing{ probeTest || traceTest };
        std::thread publisher;
        if (publishing)
        {
            publisher = std::thread([&]
            {
                int64_t time = 0;
                while (publishing)
                {
                    server.PublishAccessUnit(frame, sizeof(frame), time);
                    time += 1'000'000;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        }
        const bool probeOk = !probeTest || server.Probe();
        std::cout << "READY" << std::endl;
        if (clientsTest)
        {
            std::string command;
            while (std::getline(std::cin, command) && !command.empty())
            {
                const auto clients = server.ClientStatistics();
                std::cout << "STATS " << clients.devices << ' ' << clients.mainSessions << ' ' << clients.subSessions << std::endl;
            }
        }
        else if (!probeTest) std::cin.get();
        publishing = false;
        if (publisher.joinable()) publisher.join();
        server.Stop();
        if (!probeOk) throw std::runtime_error("Authenticated RTSP watchdog probe failed");
        if (probeTest) std::cout << "PASS: authenticated watchdog DESCRIBE/SETUP/PLAY and complete RTP packet" << std::endl;
        if (diagnostics.size() > 48) throw std::runtime_error("Connection diagnostics exceeded rate limit");
        std::string combined;
        for (const auto& entry : diagnostics) combined += entry + "\n";
        for (const char* secret : { "test-password", "test-user", "Authorization:", "response=", "cnonce=", "nonce=", "rtsp://" })
            if (combined.find(secret) != std::string::npos) throw std::runtime_error("Diagnostics exposed credentials or raw URIs");
        if (traceTest)
        {
            for (const char* expected : { "DESCRIBE 401", "DESCRIBE 200", "SETUP 401", "Digest URI mismatch",
                                         "SETUP 200", "PLAY 200", "first RTP packet sent", "closed; RTP packets=" })
                if (combined.find(expected) == std::string::npos) throw std::runtime_error("Missing RTSP diagnostic stage");
            std::cout << "PASS: RTSP stage/error/RTP diagnostics are bounded and contain no credentials" << std::endl;
        }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    CoUninitialize();
    WSACleanup();
    return 0;
}
