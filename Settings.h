#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct MotherboardIdentity
{
    std::wstring manufacturer = L"Unknown", model = L"Unknown", serial = L"Unknown";
};
MotherboardIdentity ParseMotherboardSmbios(const uint8_t* table, size_t size);
const MotherboardIdentity& GetMotherboardIdentity();
inline constexpr uint16_t kDefaultOnvifPort = 80;
inline constexpr wchar_t kDefaultMainRtspPath[] = L"Streaming/Channels/101";
inline constexpr wchar_t kDefaultSubRtspPath[] = L"Streaming/Channels/102";

struct PrivacyMask
{
    int32_t x = 0, y = 0, width = 0, height = 0;
};

struct VideoResolution
{
    uint32_t width = 0, height = 0;
    bool operator==(const VideoResolution& other) const { return width == other.width && height == other.height; }
};
// The source is the actual crop, not the monitor or the already scaled output frame.
std::vector<VideoResolution> BuildStreamResolutions(VideoResolution source, bool secondary);

struct AppSettings
{
    std::wstring cameraName = L"PC Screen Camera";
    uint16_t onvifPort = kDefaultOnvifPort;
    uint16_t rtspPort = 554;
    std::wstring rtspPath = kDefaultMainRtspPath;
    uint32_t outputWidth = 1920;
    uint32_t outputHeight = 1080;
    uint32_t frameRate = 12;
    uint32_t bitrateKbps = 2000;
    uint32_t gopSize = 25;
    uint32_t videoProfile = 2;
    bool subStreamEnabled = false;
    std::wstring subRtspPath = kDefaultSubRtspPath;
    uint32_t subWidth = 640, subHeight = 360;
    uint32_t subFrameRate = 8, subBitrateKbps = 512, subGopSize = 16;
    bool adaptiveFrameRate = true;
    bool adaptiveLoad = true;
    uint32_t minimumFrameRate = 4;
    uint32_t encoderPreference = 0;
    bool allowSoftwareEncoder = true;
    uint32_t captureMode = 0;
    uint32_t monitorIndex = 0;
    int32_t captureRegionX = 0;
    int32_t captureRegionY = 0;
    uint32_t captureRegionWidth = 1280;
    uint32_t captureRegionHeight = 720;
    std::wstring captureWindowTitle;
    bool showCursor = true;
    bool highlightMouseClicks = false;
    uint32_t mouseClickHighlightSize = 96;
    uint32_t mouseClickHighlightDurationMs = 800;
    bool standbyEnabled = true;
    std::wstring standbyText = L"Экран временно недоступен";
    // The template is the only text switch; an empty template disables the overlay.
    std::wstring overlayTemplate = L"{date} {time}";
    uint32_t timestampPosition = 0;
    std::wstring timestampFontName = L"Segoe UI";
    uint32_t timestampFontSize = 16;
    uint32_t timestampFontWeight = 400;
    bool timestampFontItalic = false;
    uint32_t timestampMarginX = 10;
    uint32_t timestampMarginY = 10;
    uint32_t timestampPaddingX = 5;
    uint32_t timestampPaddingY = 2;
    uint32_t timestampBackgroundOpacity = 65;
    std::wstring privacyMasks;
    bool authenticationEnabled = false;
    std::wstring userName = L"admin";
    std::wstring password;
    std::wstring allowedIpAddresses;
    bool autoStart = false;
    bool loggingEnabled = false;
};

std::wstring GetScreen2NvrDataDirectory();
std::wstring GetScreen2NvrLogDirectory();
std::wstring GetScreen2NvrConfigPath();
bool EnsureScreen2NvrDataDirectories();
AppSettings LoadAppSettings();
bool SaveAppSettings(const AppSettings& settings, std::wstring& error);
// File-only persistence, also used by regression tests without touching HKCU or live settings.
bool SaveAppSettingsFile(const AppSettings& settings, const std::wstring& path, std::wstring& error);
std::vector<PrivacyMask> ParsePrivacyMasks(const std::wstring& text);
std::wstring SerializePrivacyMasks(const std::vector<PrivacyMask>& masks);
void RescalePrivacyMasks(std::vector<PrivacyMask>& masks, uint32_t oldWidth, uint32_t oldHeight,
                         uint32_t newWidth, uint32_t newHeight);
std::wstring BuildOverlayText(const AppSettings& settings);
std::wstring MigrateOverlayTemplate(std::wstring text, bool dateTime, bool cameraName);
void CopyLiveSettings(AppSettings& running, const AppSettings& saved);
bool SettingsRequireRestart(const AppSettings& running, const AppSettings& saved);
void ApplyVideoProfile(AppSettings& settings, uint32_t profile);
void NormalizeSubStreamSettings(AppSettings& settings);
bool IsValidRtspPath(const std::wstring& path);
AppSettings MakeSubStreamSettings(const AppSettings& settings);
bool IsAutoStartEnabled();
bool SetAutoStartEnabled(bool enabled, std::wstring& error);
std::string WideToUtf8String(const std::wstring& value);
