#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "../TrayApp.h"
#include "../Security.h"
#include "../CaptureGeometry.h"

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        Require(Security::Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5 known-answer test failed");
        Require(Security::Base64("user:pass") == "dXNlcjpwYXNz", "Base64 known-answer test failed");
        Require(Security::Basic("bAsIc dXNlcjpwYXNz", "user", "pass"), "Case-insensitive Basic failed");
        Require(!Security::Basic("Basic dXNlcjpwYXNzjunk", "user", "pass"), "Basic accepted a prefix match");
        Require(Security::Header("POST / HTTP/1.1\r\nauthorization: Basic yes\r\n\r\n", "Authorization") == "Basic yes",
                "Case-insensitive header failed");
        Require(Security::Header("POST / HTTP/1.1\r\n\r\nAuthorization: Basic yes", "authorization").empty(),
                "Body content treated as HTTP authorization");
        Require(Security::Header("POST / HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n", "authorization").empty(),
                "Duplicate authorization accepted");
        Require(Security::Header("DESCRIBE rtsp://camera/ RTSP/1.0\r\nAuthorization: Digest username=\"test\",\r\n\trealm=\"Screen2NVR\"\r\n\r\n", "authorization") ==
                "Digest username=\"test\", realm=\"Screen2NVR\"", "Folded RTSP authorization was not unfolded");
        Require(Security::Header("DESCRIBE rtsp://camera/ RTSP/1.0\r\nX-Other: something\r\n Authorization: Basic yes\r\n\r\n", "authorization").empty(),
                "Continuation of another header was treated as Authorization");
        Security::AuthenticationLogLimiter logLimit(60'000, 3);
        Require(logLimit.ShouldLog() && logLimit.ShouldLog() && logLimit.ShouldLog() && !logLimit.ShouldLog(),
                "Bounded connection diagnostics exceeded burst allowance");
        const AppSettings defaults;
        Require(!defaults.loggingEnabled, "Logging must be off on a fresh or legacy configuration");
        for (uint32_t profile = 0; profile <= 3; ++profile)
        {
            AppSettings preset;
            preset.outputWidth = 1366; preset.outputHeight = 768;
            preset.subWidth = 480; preset.subHeight = 270;
            ApplyVideoProfile(preset, profile);
            Require(preset.outputWidth == 1366 && preset.outputHeight == 768 && preset.subWidth == 480 && preset.subHeight == 270,
                    "Quality preset changed a stream resolution");
            Require(preset.frameRate == (profile == 1 ? 8U : profile == 3 ? 25U : 12U) &&
                    preset.bitrateKbps == (profile == 1 ? 1000U : profile == 3 ? 4000U : 2000U) &&
                    preset.gopSize == (profile == 1 ? 24U : profile == 3 ? 50U : 25U), "Quality preset parameters incorrect");
            Require(preset.subFrameRate == 8 && preset.subBitrateKbps == 512 && preset.subGopSize == 16 &&
                    preset.minimumFrameRate == 4 && preset.adaptiveFrameRate && preset.adaptiveLoad,
                    "Quality preset changed unrelated options");
        }
        for (const VideoResolution source : { VideoResolution{1920,1080}, {2560,1440}, {3440,1440}, {5120,1440},
                                               {1280,1024}, {1600,1200}, {1080,1920}, {1000,800}, {1000,1000}, {1365,767} })
        {
            for (bool secondary : { false, true })
            {
                const auto choices = BuildStreamResolutions(source, secondary);
                Require(!choices.empty(), "Missing resolution list");
                Require(choices.front().width == (secondary ? 720U : source.width & ~1U), "Incorrect maximum resolution");
                Require(secondary ? choices.size() == 4 && choices.back().width == 320 : choices.back().width == 720,
                        "Resolution range/count incorrect");
                for (size_t i = 0; i < choices.size(); ++i)
                {
                    const auto size = choices[i];
                    const double idealHeight = static_cast<double>(size.width) * source.height / source.width;
                    Require(size.width <= source.width && size.height <= source.height && !(size.width & 1U) && !(size.height & 1U) &&
                            std::abs(size.height - idealHeight) < 2.0, "Resolution not even or has wrong crop aspect");
                    Require(i == 0 || choices[i - 1].width > size.width, "Duplicate/unsorted resolutions");
                }
            }
        }
        Require(BuildStreamResolutions({1920,1080}, true) == std::vector<VideoResolution>{{720,406},{640,360},{480,270},{320,180}},
                "16:9 secondary resolution ladder incorrect");
        Require(BuildStreamResolutions({1000,800}, true) == std::vector<VideoResolution>{{720,576},{640,512},{480,384},{320,256}},
                "Crop proportions were replaced by the monitor proportions");
        Require(BuildStreamResolutions({640,480}, false) == std::vector<VideoResolution>{{640,480}} &&
                BuildStreamResolutions({640,480}, true).size() == 4, "Small crop upscaled or missing smaller sub sizes");
        Require(BuildStreamResolutions({}, false).empty() && BuildStreamResolutions({1,1080}, false).empty() &&
                BuildStreamResolutions({1000,0}, true).empty(), "Invalid source dimensions accepted");
        AppSettings cropSettings;
        const RECT desktop{-1920,100,0,1180};
        cropSettings.captureMode = 1;
        cropSettings.captureRegionX = 200; cropSettings.captureRegionY = 300;
        cropSettings.captureRegionWidth = 1000; cropSettings.captureRegionHeight = 800;
        auto crop = CaptureGeometry::Rectangle(cropSettings, desktop, nullptr);
        Require(crop.left == -1720 && crop.top == 400 && crop.right == -720 && crop.bottom == 1180,
                "Capture bounds not clipped identically on a displaced monitor");
        cropSettings.captureRegionX = -200; cropSettings.captureRegionY = -100;
        crop = CaptureGeometry::Rectangle(cropSettings, desktop, nullptr);
        Require(crop.right - crop.left == 800 && crop.bottom - crop.top == 700, "Negative crop coordinates have wrong proportions");
        cropSettings.captureMode = 3;
        crop = CaptureGeometry::Rectangle(cropSettings, desktop, nullptr);
        Require(crop.right - crop.left == 1920 && crop.bottom - crop.top == 1080, "Missing-window fallback changed");
        HWND cropWindow = CreateWindowExW(0, L"STATIC", L"Screen2NVR geometry fixture", WS_POPUP,
            50, 40, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(cropWindow != nullptr, "Cannot create hidden crop fixture");
        const auto windowCrop = CaptureGeometry::Rectangle(cropSettings, {0,0,1920,1080}, cropWindow);
        const auto clippedWindow = CaptureGeometry::Rectangle(cropSettings, {0,0,900,700}, cropWindow);
        DestroyWindow(cropWindow);
        Require(windowCrop.right - windowCrop.left == 1000 && windowCrop.bottom - windowCrop.top == 800 &&
                clippedWindow.right - clippedWindow.left == 850 && clippedWindow.bottom - clippedWindow.top == 660,
                "Window aspect or monitor-edge clipping incorrect");
        AppSettings ultrawide;
        ultrawide.outputWidth = 720; ultrawide.outputHeight = 202;
        ultrawide.subWidth = 320; ultrawide.subHeight = 90;
        NormalizeSubStreamSettings(ultrawide);
        Require(ultrawide.subHeight == 90 && MakeSubStreamSettings(ultrawide).outputHeight == 90,
                "Legacy height minimum distorts ultrawide resolutions");
        std::cout << "PASS: independent quality profiles, source-aspect resolution lists, clipping, portrait/ultrawide/small/invalid areas\n";
        Require(defaults.onvifPort == 80 && defaults.rtspPort == 554 &&
                defaults.rtspPath == L"Streaming/Channels/101" && defaults.subRtspPath == L"Streaming/Channels/102",
                "Requested camera endpoint defaults are incorrect");
        Require(IsValidRtspPath(defaults.rtspPath) && IsValidRtspPath(defaults.subRtspPath) && IsValidRtspPath(L"pc-screen"),
                "Hierarchical/legacy RTSP paths should both remain valid");
        for (const auto bad : { L"", L"/Streaming/Channels/101", L"stream/", L"a//b", L"a/../b", L"a/./b", L"a b", L"a\\b", L"a?b", L"a#b", L"a\r\nb" })
            Require(!IsValidRtspPath(bad), "Invalid RTSP path accepted");
        std::cout << "PASS: ONVIF/RTSP defaults, hierarchical paths, folded headers and bounded diagnostics\n";
        std::wstring addresses = L"192.168.1.2; 192.168.1.3";
        Require(Security::NormalizeIpList(addresses, addresses) && addresses == L"192.168.1.2, 192.168.1.3",
                "Aliased IP-list normalization failed");
        Require(!Security::NormalizeIpList(L"999.1.1.1", addresses), "Invalid IPv4 accepted");
        Require(!Security::NormalizeIpList(L", ; ,", addresses), "Separators-only allowlist accepted");
        Require(Security::IpAllowed("192.168.1.2", "192.168.1.2, 192.168.1.3") &&
                !Security::IpAllowed("192.168.1.20", "192.168.1.2") &&
                Security::IpAllowed("127.0.0.1", "192.168.1.2"), "IP access control failed");
        const auto boardRecord = [](const std::vector<std::string>& strings, uint8_t flags = 1)
        {
            std::vector<uint8_t> record{ 2,15,0,0,1,2,0,3,0,flags,0,0,0,10,0 };
            for (const auto& text : strings) { record.insert(record.end(), text.begin(), text.end()); record.push_back(0); }
            record.push_back(0);
            return record;
        };
        auto board = boardRecord({ " Maker & Co ", "Model <A>", "SN-123" });
        auto identity = ParseMotherboardSmbios(board.data(), board.size());
        Require(identity.manufacturer == L"Maker & Co" && identity.model == L"Model <A>" && identity.serial == L"SN-123",
                "SMBIOS Type 2 strings or trimming incorrect");
        auto daughter = boardRecord({ "Daughter", "Module", "Child" }, 0); daughter[13] = 3;
        daughter.insert(daughter.end(), board.begin(), board.end());
        Require(ParseMotherboardSmbios(daughter.data(), daughter.size()).serial == L"SN-123", "Hosting board not preferred");
        for (size_t length = 0; length < board.size(); ++length)
            Require(ParseMotherboardSmbios(board.data(), length).serial == L"Unknown", "Truncated SMBIOS accepted");
        Require(ParseMotherboardSmbios(nullptr, 123).serial == L"Unknown", "Null SMBIOS not handled");
        board[4] = 0; board[7] = 99;
        identity = ParseMotherboardSmbios(board.data(), board.size());
        Require(identity.manufacturer == L"Unknown" && identity.serial == L"Unknown" && identity.model == L"Model <A>",
                "Missing/invalid SMBIOS string index not handled per field");
        board = boardRecord({ "Default string", "Not Specified", "To Be Filled By O.E.M." });
        identity = ParseMotherboardSmbios(board.data(), board.size());
        Require(identity.manufacturer == L"Unknown" && identity.model == L"Unknown" && identity.serial == L"Unknown",
                "Firmware placeholders advertised as real identity");
        board = boardRecord({ "Производитель", "Плата", "123" });
        Require(ParseMotherboardSmbios(board.data(), board.size()).model == L"Плата", "UTF-8 SMBIOS text corrupted");
        board[1] = 0;
        Require(ParseMotherboardSmbios(board.data(), board.size()).model == L"Unknown", "Invalid record length not rejected");
        board = { 127,4,0,0,0,0 }; board.insert(board.end(), daughter.begin(), daughter.end());
        Require(ParseMotherboardSmbios(board.data(), board.size()).serial == L"Unknown", "SMBIOS end marker ignored");
        const auto& actualBoard = GetMotherboardIdentity();
        Require(!actualBoard.manufacturer.empty() && !actualBoard.model.empty() && !actualBoard.serial.empty(), "Identity fallback is empty");
        std::cout << "PASS: motherboard SMBIOS identity, hosting board, missing/invalid/truncated data and UTF-8\n";
        auto masks = ParsePrivacyMasks(L"10,20,200,100; -4,5,8,9; bad; 1,2,-4,0; 2147483647,1,100,100");
        Require(masks.size() == 2 && masks[1].x == -4 && masks[0].width == 200,
                "Legacy mask parsing or overflow rejection failed");
        Require(SerializePrivacyMasks(masks) == L"10,20,200,100;-4,5,8,9", "Mask serialization failed");
        Require(ParsePrivacyMasks(L"").empty(), "Empty masks failed");
        auto scaled = ParsePrivacyMasks(L"600,300,900,600");
        RescalePrivacyMasks(scaled, 1920, 1080, 1280, 720);
        Require(SerializePrivacyMasks(scaled) == L"400,200,600,400", "Privacy mask scaling failed");
        AppSettings settings;
        settings.cameraName = L"Test camera";
        settings.overlayTemplate = L"{camera}";
        Require(BuildOverlayText(settings) == L"Test camera", "Camera token expansion failed");
        settings.overlayTemplate.clear();
        Require(BuildOverlayText(settings).empty(), "Empty template should disable all overlay text");
        settings.overlayTemplate = L"Literal text";
        Require(BuildOverlayText(settings) == L"Literal text", "Literal text needs no hidden switches");
        settings.overlayTemplate = L"{date} {time}";
        Require(BuildOverlayText(settings).size() == 19, "Date/time format failed");
        Require(MigrateOverlayTemplate(L"{camera}\\n{date} {time}", true, false) == L"{date} {time}",
                "Legacy default migration changed visible text");
        Require(MigrateOverlayTemplate(L"{date} {time}\\n{camera}", false, true) == L"{camera}",
                "Legacy camera-only migration failed");
        Require(MigrateOverlayTemplate(L"Anything", false, false).empty(), "Disabled legacy text became visible");
        Require(MigrateOverlayTemplate(L"{camera}\\n{date} {time}", true, true) == L"{camera}\\n{date} {time}",
                "Enabled legacy template changed");
        AppSettings active = settings, saved = settings;
        saved.overlayTemplate = L"{camera}"; saved.timestampFontName = L"Arial";
        saved.timestampFontSize = 24; saved.timestampBackgroundOpacity = 22;
        saved.privacyMasks = L"20,30,100,80"; saved.showCursor = false;
        saved.autoStart = !active.autoStart; saved.adaptiveFrameRate = false;
        saved.loggingEnabled = true;
        saved.uiLanguage = 1;
        Require(!SettingsRequireRestart(active, saved), "Live settings incorrectly require restart");
        TrayRuntimeStatus mailbox;
        uint64_t revision = 0;
        mailbox.QueueLiveSettings(saved);
        Require(mailbox.ConsumeLiveSettings(active, revision) && active.overlayTemplate == L"{camera}" &&
                active.timestampFontName == L"Arial" && active.timestampFontSize == 24 &&
                active.privacyMasks == saved.privacyMasks && !active.showCursor && !active.adaptiveFrameRate && active.loggingEnabled && active.uiLanguage == 1,
                "Saved live settings were not applied");
        Require(!mailbox.ConsumeLiveSettings(active, revision), "Same revision was applied more than once");
        saved.loggingEnabled = false;
        mailbox.QueueLiveSettings(saved); mailbox.ConsumeLiveSettings(active, revision);
        Require(!active.loggingEnabled && !SettingsRequireRestart(active, saved), "Disabling logging requires a restart or was not applied");
        saved.rtspPort++;
        Require(SettingsRequireRestart(active, saved), "Changed listening port does not request restart");
        mailbox.QueueLiveSettings(saved); mailbox.ConsumeLiveSettings(active, revision);
        Require(SettingsRequireRestart(active, saved), "Postponed restart was forgotten after a live save");
        saved.outputWidth = 1280; saved.privacyMasks.clear(); saved.overlayTemplate.clear();
        mailbox.QueueLiveSettings(saved); mailbox.ConsumeLiveSettings(active, revision);
        Require(active.outputWidth == 1920 && !active.privacyMasks.empty() && !active.overlayTemplate.empty(),
                "Pending resolution change applied masks/coordinates to the wrong-sized stream");
        std::cout << "PASS: template-only overlay, legacy migration, live settings mailbox and restart boundaries\n";
        settings.overlayTemplate = L"{camera}\\n{date} {time}";
        if (argc < 2 || std::wstring(argv[1]) != L"--ui")
        {
            std::cout << "PASS: motherboard identity, legacy masks, bounds, mask roundtrip/scaling, overlay text\n";
            return 0;
        }

        // UI-only fixture: never starts a listener or the capture pipeline, and never writes configuration.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Require(SUCCEEDED(com), "COM initialization failed");
        WSADATA wsa{};
        Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock initialization failed");
        TrayRuntimeStatus status;
        bool savedLogging = settings.loggingEnabled;
        status.settingsSaver = [&](const AppSettings& value, std::wstring&) { savedLogging = value.loggingEnabled; return true; };
        status.configureLogging = [&](bool enabled, std::wstring&)
        {
            Require(enabled == savedLogging, "Logging toggle applied before persistence");
            return true;
        };
        std::atomic_bool running{ true };
        status.startTickMs = GetTickCount64();
        status.phase = static_cast<uint32_t>(PipelinePhase::streaming);
        status.publishedFrames = 1;
        std::thread frames([&]
        {
            while (running)
            {
                const uint64_t now = GetTickCount64();
                status.captureHeartbeatMs = now; status.publishedHeartbeatMs = now; status.rtspProbeHeartbeatMs = now;
                if (status.previewRequested)
                {
                    std::lock_guard<std::mutex> lock(status.previewMutex);
                    status.previewWidth = 640; status.previewHeight = 360;
                    status.previewPixels.resize(640 * 360 * 4);
                    for (unsigned y = 0; y < 360; ++y)
                        for (unsigned x = 0; x < 640; ++x)
                        {
                            auto* pixel = status.previewPixels.data() + (y * 640 + x) * 4;
                            pixel[0] = static_cast<uint8_t>(100 + y / 4);
                            pixel[1] = static_cast<uint8_t>(110 + x / 6);
                            pixel[2] = static_cast<uint8_t>((x / 32 + y / 32) % 2 == 0 ? 210 : 130);
                            pixel[3] = 255;
                        }
                    status.previewTickMs = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        const int result = RunTrayApplication(settings, running, status);
        running = false;
        frames.join();
        WSACleanup(); CoUninitialize();
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
