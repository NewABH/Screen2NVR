#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <aclapi.h>

#include "Settings.h"
#include "Localization.h"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

namespace
{
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Screen2NVR";

std::wstring ReadText(const wchar_t* section, const wchar_t* key, const wchar_t* fallback,
                      const std::wstring& path)
{
    std::vector<wchar_t> buffer(4096);
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

uint32_t ReadNumber(const wchar_t* section, const wchar_t* key, uint32_t fallback,
                    uint32_t minimum, uint32_t maximum, const std::wstring& path)
{
    const UINT value = GetPrivateProfileIntW(section, key, fallback, path.c_str());
    return std::clamp<uint32_t>(value, minimum, maximum);
}

int32_t ReadSignedNumber(const wchar_t* section, const wchar_t* key, int32_t fallback,
                         int32_t minimum, int32_t maximum, const std::wstring& path)
{
    const std::wstring text = ReadText(section, key, std::to_wstring(fallback).c_str(), path);
    try { return std::clamp<int32_t>(std::stoi(text), minimum, maximum); }
    catch (...) { return fallback; }
}

std::mutex settingsFileMutex;

bool IsTransientFileError(DWORD code)
{
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

template<class Operation>
bool RetryFileOperation(Operation operation)
{
    for (unsigned attempt = 0; ; ++attempt)
    {
        if (operation()) return true;
        const DWORD code = GetLastError();
        if (!IsTransientFileError(code) || attempt == 10)
        {
            SetLastError(code);
            return false;
        }
        Sleep(50);
    }
}

bool SettingsFileError(const wchar_t* operation, const std::wstring& path,
                       DWORD code, std::wstring& error)
{
    wchar_t message[1024]{};
    const DWORD language = MAKELANGID(IsEnglishUi() ? LANG_ENGLISH : LANG_RUSSIAN, SUBLANG_DEFAULT);
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, language, message, ARRAYSIZE(message), nullptr);
    error = std::wstring(operation) + L"\n" + path + UiText(L"\n\nКод Windows: ", L"\n\nWindows error code: ") +
            std::to_wstring(code) + L". " + message;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_WRITE_PROTECT)
        error += UiText(L"\nПроверьте атрибут «Только чтение» и права на папку и файл. "
                        L"Для восстановления прав установки повторно запустите установщик Screen2NVR. "
                        L"Саму программу не требуется постоянно запускать от администратора.",
                        L"\nCheck the Read-only attribute and the folder and file permissions. "
                        L"Run the Screen2NVR installer again to restore installation permissions. "
                        L"The application itself does not need to run permanently as administrator.");
    else if (IsTransientFileError(code))
        error += UiText(L"\nФайл занят другой программой. Закройте редактор настроек и повторите сохранение.",
                        L"\nThe file is in use by another program. Close the settings editor and save again.");
    error += UiText(L"\nНовые настройки не применены. Предыдущие настройки не удаляются.",
                    L"\nThe new settings were not applied. The previous settings have been preserved.");
    return false;
}

struct TemporarySettingsFile
{
    std::wstring path;
    ~TemporarySettingsFile()
    {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};

struct SettingsSecurity
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ~SettingsSecurity() { if (descriptor) LocalFree(descriptor); }
};

// Create only a NEW sibling file. Never truncate the user's live INI, even if a read failed.
bool CreateSettingsTemporary(const std::wstring& destination, const std::vector<BYTE>& contents,
                             const SettingsSecurity& security, TemporarySettingsFile& temporary, std::wstring& error)
{
    GUID id{};
    wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, suffix, ARRAYSIZE(suffix)) == 0)
        return SettingsFileError(UiText(L"Не удалось подготовить временный файл настроек:", L"Could not prepare the temporary settings file:"), destination,
                                 ERROR_GEN_FAILURE, error);
    const std::wstring path = destination + L"." + suffix + L".tmp";
    SECURITY_ATTRIBUTES attributes{ sizeof(attributes), security.descriptor, FALSE };
    HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                security.descriptor ? &attributes : nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return SettingsFileError(UiText(L"Не удалось создать временный файл рядом с настройками:", L"Could not create a temporary file next to the settings:"),
                                 destination, GetLastError(), error);
    temporary.path = path;
    DWORD written = 0;
    const bool ok = WriteFile(output, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) != FALSE;
    const DWORD code = ok ? ERROR_WRITE_FAULT : GetLastError();
    CloseHandle(output);
    if (!ok || written != contents.size())
        return SettingsFileError(UiText(L"Не удалось записать временный файл настроек:", L"Could not write the temporary settings file:"), destination, code, error);
    return true;
}

bool ReadSettingsBytes(const std::wstring& path, std::vector<BYTE>& contents, bool& exists,
                        SettingsSecurity& security, std::wstring& error)
{
    HANDLE input = INVALID_HANDLE_VALUE;
    if (!RetryFileOperation([&]
        {
            input = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            return input != INVALID_HANDLE_VALUE;
        }))
    {
        const DWORD code = GetLastError();
        exists = code != ERROR_FILE_NOT_FOUND;
        if (!exists) return true;
        return SettingsFileError(UiText(L"Не удалось прочитать текущие настройки:", L"Could not read the current settings:"), path, code, error);
    }
    exists = true;
    // The staging copy contains credentials too: it must not gain broader permissions
    // than an existing INI with an explicitly restricted DACL.
    const DWORD securityError = GetSecurityInfo(input, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                                nullptr, nullptr, nullptr, nullptr, &security.descriptor);
    if (securityError != ERROR_SUCCESS)
    {
        CloseHandle(input);
        return SettingsFileError(UiText(L"Не удалось прочитать права файла настроек:", L"Could not read the settings file permissions:"), path, securityError, error);
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(input, &size) != FALSE;
    DWORD code = ok ? ERROR_FILE_TOO_LARGE : GetLastError();
    if (ok && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024)
    {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(input, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE;
        code = ok ? ERROR_READ_FAULT : GetLastError();
        ok = ok && read == contents.size();
    }
    else ok = false;
    CloseHandle(input);
    return ok || SettingsFileError(UiText(L"Не удалось прочитать текущие настройки:", L"Could not read the current settings:"), path, code, error);
}

std::wstring CurrentExecutablePath()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length > 0 && length < path.size() ? std::wstring(path.data(), length) : std::wstring();
}

bool IsConfigUsable(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    wchar_t marker[32]{};
    GetPrivateProfileStringW(L"Camera", L"Name", L"__missing__", marker, ARRAYSIZE(marker), path.c_str());
    return wcscmp(marker, L"__missing__") != 0 && marker[0] != L'\0';
}
}

std::wstring GetScreen2NvrDataDirectory()
{
    PWSTR knownPath = nullptr;
    std::wstring result = L"C:\\ProgramData\\Screen2NVR";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &knownPath)))
    {
        result = std::wstring(knownPath) + L"\\Screen2NVR";
        CoTaskMemFree(knownPath);
    }
    return result;
}

std::wstring GetScreen2NvrLogDirectory() { return GetScreen2NvrDataDirectory() + L"\\logs"; }
std::wstring GetScreen2NvrConfigPath() { return GetScreen2NvrDataDirectory() + L"\\config.ini"; }

void ApplyVideoProfile(AppSettings& settings, uint32_t profile)
{
    settings.videoProfile = std::min<uint32_t>(profile, 3);
    if (profile == 1)
    {
        settings.frameRate = 8; settings.bitrateKbps = 1000; settings.gopSize = 24;
    }
    else if (profile == 2)
    {
        settings.frameRate = 12; settings.bitrateKbps = 2000; settings.gopSize = 25;
    }
    else if (profile == 3)
    {
        settings.frameRate = 25; settings.bitrateKbps = 4000; settings.gopSize = 50;
    }
}

std::vector<VideoResolution> BuildStreamResolutions(VideoResolution source, bool secondary)
{
    std::vector<VideoResolution> result;
    if (!source.width || !source.height) return result;
    const auto add = [&](uint32_t requestedWidth)
    {
        const uint32_t width = requestedWidth & ~1U;
        if (width < 16 || width > source.width || width > 7680) return;
        // NV12/H.264 needs even dimensions. Round the height to the nearest even pixel.
        const uint64_t height = std::min<uint64_t>(source.height & ~1U,
            ((static_cast<uint64_t>(width) * source.height + source.width) / (2ULL * source.width)) * 2);
        if (height < 16 || height > 4320) return;
        const VideoResolution size{ width, static_cast<uint32_t>(height) };
        if (std::find(result.begin(), result.end(), size) == result.end()) result.push_back(size);
    };
    if (secondary)
    {
        const uint32_t maximum = std::min(720U, source.width);
        for (uint32_t width : { 720U, 640U, 480U, 320U })
            add(maximum * width / 720);
    }
    else
    {
        add(source.width);
        for (uint32_t width : { 7680U, 5120U, 3840U, 2560U, 1920U, 1600U, 1280U, 1024U, 960U, 720U }) add(width);
    }
    return result;
}

bool EnsureScreen2NvrDataDirectories()
{
    std::error_code error;
    std::filesystem::create_directories(GetScreen2NvrLogDirectory(), error);
    return !error;
}

MotherboardIdentity ParseMotherboardSmbios(const uint8_t* table, size_t size)
{
    MotherboardIdentity result;
    if (!table) return result;
    int bestRank = -1;
    for (size_t offset = 0; size - offset >= 4;)
    {
        const uint8_t type = table[offset], length = table[offset + 1];
        if (length < 4 || length > size - offset || type == 127) break;
        const size_t strings = offset + length;
        size_t end = strings;
        while (end + 1 < size && (table[end] != 0 || table[end + 1] != 0)) ++end;
        if (end + 1 >= size) break; // Truncated string-set: never read beyond firmware data.
        if (type == 2 && length >= 8)
        {
            // SMBIOS Type 2: prefer the hosting motherboard over daughterboards.
            const int rank = (length > 9 && (table[offset + 9] & 1) ? 2 : 0) +
                             (length > 13 && table[offset + 13] == 10 ? 1 : 0);
            if (rank > bestRank)
            {
                const auto readString = [&](uint8_t index) -> std::wstring
                {
                    if (!index) return L"Unknown";
                    size_t start = strings;
                    for (unsigned current = 1; start < end; ++current)
                    {
                        size_t finish = start;
                        while (finish < end && table[finish]) ++finish;
                        if (current == index)
                        {
                            if (finish == start || finish - start > 1024) return L"Unknown";
                            const auto* bytes = reinterpret_cast<const char*>(table + start);
                            const int count = static_cast<int>(finish - start);
                            int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, count, nullptr, 0);
                            const UINT codePage = chars ? CP_UTF8 : CP_ACP;
                            if (!chars) chars = MultiByteToWideChar(codePage, 0, bytes, count, nullptr, 0);
                            if (!chars) return L"Unknown";
                            std::wstring text(chars, L'\0');
                            MultiByteToWideChar(codePage, 0, bytes, count, text.data(), chars);
                            for (auto& ch : text) if (ch < 32 || ch == 127 || ch == 0xFFFE || ch == 0xFFFF) ch = L' ';
                            const size_t first = text.find_first_not_of(L" \t\r\n");
                            if (first == std::wstring::npos) return L"Unknown";
                            text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
                            std::wstring lower = text;
                            std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return std::towlower(ch); });
                            for (const auto placeholder : { L"unknown", L"none", L"not specified", L"not applicable",
                                    L"default string", L"to be filled by o.e.m.", L"to be filled by oem", L"system serial number" })
                                if (lower == placeholder) return L"Unknown";
                            return text;
                        }
                        start = finish + 1;
                    }
                    return L"Unknown";
                };
                result = { readString(table[offset + 4]), readString(table[offset + 5]), readString(table[offset + 7]) };
                bestRank = rank;
            }
        }
        offset = end + 2;
    }
    return result;
}

const MotherboardIdentity& GetMotherboardIdentity()
{
    static const MotherboardIdentity identity = []
    {
        constexpr DWORD provider = 0x52534D42; // 'RSMB', RawSMBIOSData (8-byte header).
        constexpr UINT maximumSize = 16 * 1024 * 1024;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const UINT size = GetSystemFirmwareTable(provider, 0, nullptr, 0);
            if (size < 8 || size > maximumSize) break;
            std::vector<uint8_t> raw(size);
            const UINT read = GetSystemFirmwareTable(provider, 0, raw.data(), size);
            if (read > size) continue;
            if (read < 8) break;
            uint32_t tableSize = 0;
            std::memcpy(&tableSize, raw.data() + 4, sizeof(tableSize));
            if (tableSize > read - 8) break;
            return ParseMotherboardSmbios(raw.data() + 8, tableSize);
        }
        return MotherboardIdentity{};
    }();
    return identity;
}

bool IsValidRtspPath(const std::wstring& path)
{
    if (path.empty() || path.size() > 512 || path.front() == L'/' || path.back() == L'/') return false;
    size_t segment = 0;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == L'/')
        {
            const auto part = path.substr(segment, i - segment);
            if (part.empty() || part == L"." || part == L"..") return false;
            segment = i + 1;
        }
        else if (path[i] <= L' ' || path[i] == 127 || std::wstring(L"\\?#\"<>").find(path[i]) != std::wstring::npos)
            return false;
    }
    return true;
}

void NormalizeSubStreamSettings(AppSettings& settings)
{
    settings.subWidth = std::clamp(settings.subWidth, 16U, settings.outputWidth) & ~1U;
    settings.subHeight = std::clamp(settings.subHeight, 16U, settings.outputHeight) & ~1U;
    settings.subFrameRate = std::clamp(settings.subFrameRate, 1U, settings.frameRate);
    settings.subBitrateKbps = std::clamp(settings.subBitrateKbps, 64U, 50000U);
    settings.subGopSize = std::clamp(settings.subGopSize, 1U, 600U);
    if (settings.subRtspPath.empty() || settings.subRtspPath == settings.rtspPath)
        settings.subRtspPath = settings.rtspPath + L"-sub";
}

AppSettings MakeSubStreamSettings(const AppSettings& settings)
{
    AppSettings sub = settings;
    NormalizeSubStreamSettings(sub);
    sub.outputWidth = sub.subWidth; sub.outputHeight = sub.subHeight;
    sub.frameRate = sub.subFrameRate; sub.bitrateKbps = sub.subBitrateKbps; sub.gopSize = sub.subGopSize;
    sub.rtspPath = sub.subRtspPath;
    // The source is the already composed main frame: never duplicate/crop overlays or masks.
    sub.captureMode = 0; sub.showCursor = false; sub.highlightMouseClicks = false;
    sub.overlayTemplate.clear(); sub.privacyMasks.clear();
    return sub;
}

bool IsAutoStartEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    std::vector<wchar_t> value(32768);
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                            reinterpret_cast<BYTE*>(value.data()), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && value[0] != L'\0';
}

bool SetAutoStartEnabled(bool enabled, std::wstring& error)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        error = UiText(L"Не удалось открыть раздел автозагрузки Windows.", L"Could not open the Windows startup registry key.");
        return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (enabled)
    {
        const std::wstring command = L"\"" + CurrentExecutablePath() + L"\"";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) error = UiText(L"Не удалось изменить автозагрузку Windows.", L"Could not change Windows startup settings.");
    return status == ERROR_SUCCESS;
}

AppSettings LoadAppSettings()
{
    EnsureScreen2NvrDataDirectories();
    std::wstring path = GetScreen2NvrConfigPath();
    const bool missing = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
                          GetLastError() == ERROR_FILE_NOT_FOUND;
    const std::wstring backupPath = path + L".bak";
    // Recovery is read-only. A temporarily locked INI must never be replaced with defaults.
    if (!IsConfigUsable(path) && IsConfigUsable(backupPath))
        path = backupPath;
    AppSettings settings;
    settings.uiLanguage = ReadNumber(L"General", L"Language", DefaultUiLanguage(), 0, 1, path);
    SetUiLanguage(settings.uiLanguage);
    settings.standbyText = UiText(L"Экран временно недоступен", L"Screen temporarily unavailable");
    settings.cameraName = ReadText(L"Camera", L"Name", settings.cameraName.c_str(), path);
    settings.onvifPort = static_cast<uint16_t>(ReadNumber(L"Network", L"OnvifPort", settings.onvifPort, 1, 65535, path));
    settings.rtspPort = static_cast<uint16_t>(ReadNumber(L"Network", L"RtspPort", settings.rtspPort, 1, 65535, path));
    settings.rtspPath = ReadText(L"Network", L"RtspPath", settings.rtspPath.c_str(), path);
    settings.outputWidth = ReadNumber(L"Video", L"Width", settings.outputWidth, 16, 7680, path);
    settings.outputHeight = ReadNumber(L"Video", L"Height", settings.outputHeight, 16, 4320, path);
    settings.frameRate = ReadNumber(L"Video", L"Fps", settings.frameRate, 1, 60, path);
    settings.bitrateKbps = ReadNumber(L"Video", L"BitrateKbps", settings.bitrateKbps, 128, 50000, path);
    settings.gopSize = ReadNumber(L"Video", L"Gop", settings.gopSize, 1, 600, path);
    settings.videoProfile = ReadNumber(L"Video", L"Profile", settings.videoProfile, 0, 3, path);
    settings.subStreamEnabled = ReadNumber(L"SubStream", L"Enabled", 0, 0, 1, path) != 0;
    settings.subRtspPath = ReadText(L"SubStream", L"RtspPath", settings.subRtspPath.c_str(), path);
    settings.subWidth = ReadNumber(L"SubStream", L"Width", 640, 16, 7680, path);
    settings.subHeight = ReadNumber(L"SubStream", L"Height", 360, 16, 4320, path);
    settings.subFrameRate = ReadNumber(L"SubStream", L"Fps", 8, 1, 60, path);
    settings.subBitrateKbps = ReadNumber(L"SubStream", L"BitrateKbps", 512, 64, 50000, path);
    settings.subGopSize = ReadNumber(L"SubStream", L"Gop", 16, 1, 600, path);
    settings.adaptiveFrameRate = ReadNumber(L"Video", L"AdaptiveFrameRate", 1, 0, 1, path) != 0;
    settings.adaptiveLoad = ReadNumber(L"Video", L"AdaptiveLoad", 1, 0, 1, path) != 0;
    settings.minimumFrameRate = ReadNumber(L"Video", L"MinimumFps", settings.minimumFrameRate, 1, 30, path);
    settings.encoderPreference = ReadNumber(L"Video", L"EncoderPreference", 0, 0, 2, path);
    settings.allowSoftwareEncoder = ReadNumber(L"Video", L"AllowSoftwareEncoder", 1, 0, 1, path) != 0;
    settings.captureMode = ReadNumber(L"Capture", L"Mode", 0, 0, 3, path);
    settings.monitorIndex = ReadNumber(L"Capture", L"Monitor", 0, 0, 31, path);
    settings.captureRegionX = ReadSignedNumber(L"Capture", L"RegionX", 0, -32768, 32768, path);
    settings.captureRegionY = ReadSignedNumber(L"Capture", L"RegionY", 0, -32768, 32768, path);
    settings.captureRegionWidth = ReadNumber(L"Capture", L"RegionWidth", 1280, 16, 16384, path);
    settings.captureRegionHeight = ReadNumber(L"Capture", L"RegionHeight", 720, 16, 16384, path);
    settings.captureWindowTitle = ReadText(L"Capture", L"WindowTitle", L"", path);
    settings.showCursor = ReadNumber(L"Capture", L"ShowCursor", 1, 0, 1, path) != 0;
    settings.highlightMouseClicks = ReadNumber(L"Capture", L"HighlightMouseClicks", 0, 0, 1, path) != 0;
    settings.mouseClickHighlightSize = ReadNumber(L"Capture", L"MouseClickHighlightSize", 96, 32, 320, path);
    settings.mouseClickHighlightDurationMs = ReadNumber(L"Capture", L"MouseClickHighlightDurationMs", 800, 200, 3000, path);
    settings.standbyEnabled = ReadNumber(L"Capture", L"StandbyEnabled", 1, 0, 1, path) != 0;
    settings.standbyText = ReadText(L"Capture", L"StandbyText", settings.standbyText.c_str(), path);
    const bool templateOnly = ReadNumber(L"Timestamp", L"TemplateVersion", 0, 0, 2, path) >= 2;
    settings.overlayTemplate = ReadText(L"Timestamp", L"Template",
        templateOnly ? settings.overlayTemplate.c_str() : L"{camera}\\n{date} {time}", path);
    if (!templateOnly)
        settings.overlayTemplate = MigrateOverlayTemplate(settings.overlayTemplate,
            ReadNumber(L"Video", L"Timestamp", 1, 0, 1, path) != 0,
            ReadNumber(L"Timestamp", L"ShowCameraName", 0, 0, 1, path) != 0);
    settings.timestampPosition = ReadNumber(L"Timestamp", L"Position", 0, 0, 3, path);
    settings.timestampFontName = ReadText(L"Timestamp", L"FontName", settings.timestampFontName.c_str(), path);
    settings.timestampFontSize = ReadNumber(L"Timestamp", L"FontSize", settings.timestampFontSize, 8, 128, path);
    const uint32_t legacyWeight = ReadNumber(L"Timestamp", L"Bold", 0, 0, 1, path) != 0 ? 700U : 400U;
    settings.timestampFontWeight = ReadNumber(L"Timestamp", L"FontWeight", legacyWeight, 100, 900, path);
    settings.timestampFontItalic = ReadNumber(L"Timestamp", L"Italic", 0, 0, 1, path) != 0;
    settings.timestampMarginX = ReadNumber(L"Timestamp", L"MarginX", settings.timestampMarginX, 0, 7680, path);
    settings.timestampMarginY = ReadNumber(L"Timestamp", L"MarginY", settings.timestampMarginY, 0, 4320, path);
    settings.timestampPaddingX = ReadNumber(L"Timestamp", L"PaddingX", settings.timestampPaddingX, 0, 100, path);
    settings.timestampPaddingY = ReadNumber(L"Timestamp", L"PaddingY", settings.timestampPaddingY, 0, 100, path);
    settings.timestampBackgroundOpacity = ReadNumber(L"Timestamp", L"BackgroundOpacity",
                                                      settings.timestampBackgroundOpacity, 0, 100, path);
    settings.privacyMasks = ReadText(L"Privacy", L"Masks", L"", path);
    settings.authenticationEnabled = ReadNumber(L"Security", L"Authentication", 0, 0, 1, path) != 0;
    settings.userName = ReadText(L"Security", L"UserName", settings.userName.c_str(), path);
    settings.password = ReadText(L"Security", L"Password", L"", path);
    settings.allowedIpAddresses = ReadText(L"Security", L"AllowedIPs", L"", path);
    settings.autoStart = IsAutoStartEnabled();
    settings.loggingEnabled = ReadNumber(L"Logging", L"Enabled", 0, 0, 1, path) != 0;

    if ((settings.outputWidth & 1U) != 0) --settings.outputWidth;
    if ((settings.outputHeight & 1U) != 0) --settings.outputHeight;
    if (settings.rtspPath.empty()) settings.rtspPath = kDefaultMainRtspPath;
    if (settings.timestampFontName.empty()) settings.timestampFontName = L"Segoe UI";
    if (settings.userName.empty()) settings.userName = L"admin";
    settings.minimumFrameRate = std::min(settings.minimumFrameRate, settings.frameRate);
    NormalizeSubStreamSettings(settings);

    if (missing && path != backupPath)
    {
        std::wstring ignored;
        SaveAppSettings(settings, ignored);
    }
    return settings;
}

bool SaveAppSettings(const AppSettings& settings, std::wstring& error)
{
    if (!SaveAppSettingsFile(settings, GetScreen2NvrConfigPath(), error)) return false;
    if (!SetAutoStartEnabled(settings.autoStart, error))
    {
        error += UiText(L"\nФайл настроек сохранён, но изменить автозагрузку не удалось.",
                        L"\nThe settings file was saved, but Windows startup could not be changed.");
        return false;
    }
    return true;
}

bool SaveAppSettingsFile(const AppSettings& settings, const std::wstring& destination, std::wstring& error)
{
    // The adaptive capture worker and the settings window can save at the same time.
    std::lock_guard<std::mutex> lock(settingsFileMutex);
    error.clear();
    std::error_code directoryError;
    std::filesystem::create_directories(std::filesystem::path(destination).parent_path(), directoryError);
    if (directoryError)
        return SettingsFileError(UiText(L"Не удалось создать папку настроек:", L"Could not create the settings folder:"), destination,
                                 static_cast<DWORD>(directoryError.value()), error);
    std::vector<BYTE> original;
    SettingsSecurity security;
    bool exists = false;
    if (!ReadSettingsBytes(destination, original, exists, security, error)) return false;
    // Preserve unknown keys in an existing UTF-16 INI. Legacy encodings are rewritten
    // using all current settings, with the original bytes retained in the backup.
    std::vector<BYTE> contents = { 0xFF, 0xFE };
    if (original.size() >= 2 && original[0] == 0xFF && original[1] == 0xFE)
        contents = original;
    TemporarySettingsFile temporary;
    if (!CreateSettingsTemporary(destination, contents, security, temporary, error)) return false;
    const std::wstring& path = temporary.path;
    DWORD writeError = ERROR_SUCCESS;
    const auto WriteValue = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value,
                                 const std::wstring& file)
    {
        if (writeError != ERROR_SUCCESS) return false;
        if (value.find_first_of(L"\r\n") != std::wstring::npos || value.find(L'\0') != std::wstring::npos)
        {
            writeError = ERROR_INVALID_DATA;
            return false;
        }
        const bool success = RetryFileOperation([&]
        {
            SetLastError(ERROR_SUCCESS);
            return WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()) != FALSE;
        });
        if (!success)
        {
            writeError = GetLastError();
            if (writeError == ERROR_SUCCESS) writeError = ERROR_WRITE_FAULT;
        }
        return success;
    };
    const auto number = [](uint32_t value) { return std::to_wstring(value); };
    bool ok = true;
    ok &= WriteValue(L"General", L"Language", number(settings.uiLanguage), path);
    ok &= WriteValue(L"Camera", L"Name", settings.cameraName, path);
    // Hardware identity is read on this PC, never copied from an INI to another PC.
    ok &= WriteValue(L"Network", L"OnvifPort", number(settings.onvifPort), path);
    ok &= WriteValue(L"Network", L"RtspPort", number(settings.rtspPort), path);
    ok &= WriteValue(L"Network", L"RtspPath", settings.rtspPath, path);
    ok &= WriteValue(L"Video", L"Width", number(settings.outputWidth), path);
    ok &= WriteValue(L"Video", L"Height", number(settings.outputHeight), path);
    ok &= WriteValue(L"Video", L"Fps", number(settings.frameRate), path);
    ok &= WriteValue(L"Video", L"BitrateKbps", number(settings.bitrateKbps), path);
    ok &= WriteValue(L"Video", L"Gop", number(settings.gopSize), path);
    ok &= WriteValue(L"Video", L"Profile", number(settings.videoProfile), path);
    ok &= WriteValue(L"SubStream", L"Enabled", settings.subStreamEnabled ? L"1" : L"0", path);
    ok &= WriteValue(L"SubStream", L"RtspPath", settings.subRtspPath, path);
    ok &= WriteValue(L"SubStream", L"Width", number(settings.subWidth), path);
    ok &= WriteValue(L"SubStream", L"Height", number(settings.subHeight), path);
    ok &= WriteValue(L"SubStream", L"Fps", number(settings.subFrameRate), path);
    ok &= WriteValue(L"SubStream", L"BitrateKbps", number(settings.subBitrateKbps), path);
    ok &= WriteValue(L"SubStream", L"Gop", number(settings.subGopSize), path);
    ok &= WriteValue(L"Video", L"AdaptiveFrameRate", settings.adaptiveFrameRate ? L"1" : L"0", path);
    ok &= WriteValue(L"Video", L"AdaptiveLoad", settings.adaptiveLoad ? L"1" : L"0", path);
    ok &= WriteValue(L"Video", L"MinimumFps", number(settings.minimumFrameRate), path);
    ok &= WriteValue(L"Video", L"EncoderPreference", number(settings.encoderPreference), path);
    ok &= WriteValue(L"Video", L"AllowSoftwareEncoder", settings.allowSoftwareEncoder ? L"1" : L"0", path);
    ok &= WriteValue(L"Capture", L"Mode", number(settings.captureMode), path);
    ok &= WriteValue(L"Capture", L"Monitor", number(settings.monitorIndex), path);
    ok &= WriteValue(L"Capture", L"RegionX", std::to_wstring(settings.captureRegionX), path);
    ok &= WriteValue(L"Capture", L"RegionY", std::to_wstring(settings.captureRegionY), path);
    ok &= WriteValue(L"Capture", L"RegionWidth", number(settings.captureRegionWidth), path);
    ok &= WriteValue(L"Capture", L"RegionHeight", number(settings.captureRegionHeight), path);
    ok &= WriteValue(L"Capture", L"WindowTitle", settings.captureWindowTitle, path);
    ok &= WriteValue(L"Capture", L"ShowCursor", settings.showCursor ? L"1" : L"0", path);
    ok &= WriteValue(L"Capture", L"HighlightMouseClicks", settings.highlightMouseClicks ? L"1" : L"0", path);
    ok &= WriteValue(L"Capture", L"MouseClickHighlightSize", number(settings.mouseClickHighlightSize), path);
    ok &= WriteValue(L"Capture", L"MouseClickHighlightDurationMs", number(settings.mouseClickHighlightDurationMs), path);
    if (!WritePrivateProfileStringW(L"Capture", L"HighlightCursor", nullptr, path.c_str()) ||
        !WritePrivateProfileStringW(L"Capture", L"CursorHighlightSize", nullptr, path.c_str()))
    {
        ok = false;
        if (writeError == ERROR_SUCCESS) writeError = GetLastError();
    }
    ok &= WriteValue(L"Capture", L"StandbyEnabled", settings.standbyEnabled ? L"1" : L"0", path);
    ok &= WriteValue(L"Capture", L"StandbyText", settings.standbyText, path);
    // Legacy readers can still interpret the file, but these flags are no longer settings.
    ok &= WriteValue(L"Video", L"Timestamp", settings.overlayTemplate.empty() ? L"0" : L"1", path);
    ok &= WriteValue(L"Timestamp", L"ShowCameraName", settings.overlayTemplate.find(L"{camera}") != std::wstring::npos ? L"1" : L"0", path);
    ok &= WriteValue(L"Timestamp", L"TemplateVersion", L"2", path);
    ok &= WriteValue(L"Timestamp", L"Template", settings.overlayTemplate, path);
    ok &= WriteValue(L"Timestamp", L"Position", number(settings.timestampPosition), path);
    ok &= WriteValue(L"Timestamp", L"FontName", settings.timestampFontName, path);
    ok &= WriteValue(L"Timestamp", L"FontSize", number(settings.timestampFontSize), path);
    ok &= WriteValue(L"Timestamp", L"FontWeight", number(settings.timestampFontWeight), path);
    ok &= WriteValue(L"Timestamp", L"Italic", settings.timestampFontItalic ? L"1" : L"0", path);
    ok &= WriteValue(L"Timestamp", L"Bold", settings.timestampFontWeight >= 600 ? L"1" : L"0", path);
    ok &= WriteValue(L"Timestamp", L"MarginX", number(settings.timestampMarginX), path);
    ok &= WriteValue(L"Timestamp", L"MarginY", number(settings.timestampMarginY), path);
    ok &= WriteValue(L"Timestamp", L"PaddingX", number(settings.timestampPaddingX), path);
    ok &= WriteValue(L"Timestamp", L"PaddingY", number(settings.timestampPaddingY), path);
    ok &= WriteValue(L"Timestamp", L"BackgroundOpacity", number(settings.timestampBackgroundOpacity), path);
    ok &= WriteValue(L"Privacy", L"Masks", settings.privacyMasks, path);
    ok &= WriteValue(L"Security", L"Authentication", settings.authenticationEnabled ? L"1" : L"0", path);
    ok &= WriteValue(L"Security", L"UserName", settings.userName, path);
    ok &= WriteValue(L"Security", L"Password", settings.password, path);
    ok &= WriteValue(L"Security", L"AllowedIPs", settings.allowedIpAddresses, path);
    ok &= WriteValue(L"Logging", L"Enabled", settings.loggingEnabled ? L"1" : L"0", path);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (!ok)
        return SettingsFileError(UiText(L"Не удалось записать новые настройки:", L"Could not write the new settings:"), destination,
                                 writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError, error);

    HANDLE staged = INVALID_HANDLE_VALUE;
    if (!RetryFileOperation([&]
        {
            staged = CreateFileW(path.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            return staged != INVALID_HANDLE_VALUE;
        }))
        return SettingsFileError(UiText(L"Не удалось проверить запись настроек:", L"Could not verify the settings write:"), destination, GetLastError(), error);
    const bool flushed = FlushFileBuffers(staged) != FALSE;
    const DWORD flushError = GetLastError();
    CloseHandle(staged);
    if (!flushed)
        return SettingsFileError(UiText(L"Не удалось завершить запись настроек на диск:", L"Could not finish writing the settings to disk:"), destination, flushError, error);

    // ReplaceFile preserves the destination ACL and keeps its previous bytes as a backup.
    // Never fall back to CREATE_ALWAYS/CopyFile-overwrite if replacement is denied.
    const std::wstring backup = destination + L".bak";
    TemporarySettingsFile damagedPrevious;
    if (exists && !IsConfigUsable(destination) && IsConfigUsable(backup))
        damagedPrevious.path = path + L".previous"; // Keep the known-good .bak during recovery.
    const std::wstring replacedPath = damagedPrevious.path.empty() ? backup : damagedPrevious.path;
    const bool committed = RetryFileOperation([&]
    {
        return exists ? ReplaceFileW(destination.c_str(), path.c_str(), replacedPath.c_str(), 0, nullptr, nullptr) != FALSE :
                        MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    });
    if (!committed)
    {
        const DWORD code = GetLastError();
        // Documented partial ReplaceFile failure: the old file may have already moved
        // to the backup. Restore it only if the destination is absent, never overwrite.
        if (exists && GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            if (!MoveFileExW(replacedPath.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                damagedPrevious.path.clear(); // Leave the recovery file intact if restoration is blocked.
        }
        SettingsFileError(UiText(L"Не удалось заменить файл настроек:", L"Could not replace the settings file:"), destination, code, error);
        if (exists && GetFileAttributesW(replacedPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            error += UiText(L"\nРезервная копия: ", L"\nBackup: ") + replacedPath;
        damagedPrevious.path.clear(); // Do not remove recovery data after a failed replacement.
        return false;
    }
    temporary.path.clear();
    // Discard the Win32 INI cache after replacing the file.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, destination.c_str());
    return true;
}

std::vector<PrivacyMask> ParsePrivacyMasks(const std::wstring& text)
{
    std::vector<PrivacyMask> result;
    std::wistringstream stream(text);
    std::wstring item;
    while (std::getline(stream, item, L';'))
    {
        std::replace(item.begin(), item.end(), L',', L' ');
        std::wistringstream values(item);
        PrivacyMask mask;
        if (values >> mask.x >> mask.y >> mask.width >> mask.height &&
            mask.x >= -32768 && mask.x <= 32768 && mask.y >= -32768 && mask.y <= 32768 &&
            mask.width > 0 && mask.width <= 32768 && mask.height > 0 && mask.height <= 32768)
            result.push_back(mask);
    }
    return result;
}

std::wstring SerializePrivacyMasks(const std::vector<PrivacyMask>& masks)
{
    std::wstring result;
    for (const auto& mask : masks)
    {
        if (!result.empty()) result += L";";
        result += std::to_wstring(mask.x) + L"," + std::to_wstring(mask.y) + L"," +
                  std::to_wstring(mask.width) + L"," + std::to_wstring(mask.height);
    }
    return result;
}

std::wstring MigrateOverlayTemplate(std::wstring text, bool dateTime, bool cameraName)
{
    if (!dateTime && !cameraName) return {};
    const auto remove = [&text](const wchar_t* token)
    {
        const size_t length = wcslen(token);
        size_t position;
        while ((position = text.find(token)) != std::wstring::npos) text.erase(position, length);
    };
    if (!dateTime) { remove(L"{date}"); remove(L"{time}"); }
    if (!cameraName) remove(L"{camera}");
    // Remove now-empty boundary lines without changing user text in the middle.
    for (;;)
    {
        if (!text.empty() && iswspace(text.front())) text.erase(0, 1);
        else if (text.compare(0, 2, L"\\n") == 0) text.erase(0, 2);
        else break;
    }
    for (;;)
    {
        if (!text.empty() && iswspace(text.back())) text.pop_back();
        else if (text.size() >= 2 && text.compare(text.size() - 2, 2, L"\\n") == 0) text.resize(text.size() - 2);
        else break;
    }
    return text;
}

void CopyLiveSettings(AppSettings& running, const AppSettings& saved)
{
    running.uiLanguage = saved.uiLanguage;
    // Never apply coordinates for a pending resolution change to the old-sized stream.
    if (running.outputWidth == saved.outputWidth && running.outputHeight == saved.outputHeight)
    {
        running.overlayTemplate = saved.overlayTemplate;
        running.timestampPosition = saved.timestampPosition;
        running.timestampFontName = saved.timestampFontName;
        running.timestampFontSize = saved.timestampFontSize;
        running.timestampFontWeight = saved.timestampFontWeight;
        running.timestampFontItalic = saved.timestampFontItalic;
        running.timestampMarginX = saved.timestampMarginX;
        running.timestampMarginY = saved.timestampMarginY;
        running.timestampPaddingX = saved.timestampPaddingX;
        running.timestampPaddingY = saved.timestampPaddingY;
        running.timestampBackgroundOpacity = saved.timestampBackgroundOpacity;
        running.privacyMasks = saved.privacyMasks;
    }
    running.showCursor = saved.showCursor;
    running.highlightMouseClicks = saved.highlightMouseClicks;
    running.mouseClickHighlightSize = saved.mouseClickHighlightSize;
    running.mouseClickHighlightDurationMs = saved.mouseClickHighlightDurationMs;
    running.standbyEnabled = saved.standbyEnabled;
    running.standbyText = saved.standbyText;
    running.adaptiveFrameRate = saved.adaptiveFrameRate;
    running.adaptiveLoad = saved.adaptiveLoad;
    running.minimumFrameRate = std::min(saved.minimumFrameRate, running.frameRate);
    running.autoStart = saved.autoStart;
    running.loggingEnabled = saved.loggingEnabled;
}

bool SettingsRequireRestart(const AppSettings& a, const AppSettings& b)
{
    // Compare with the active pipeline, not the last save (a restart may have been postponed).
    const auto structural = [](const AppSettings& s)
    {
        return std::tie(s.cameraName, s.onvifPort, s.rtspPort, s.rtspPath,
            s.outputWidth, s.outputHeight, s.frameRate, s.bitrateKbps, s.gopSize,
            s.encoderPreference, s.allowSoftwareEncoder, s.captureMode, s.monitorIndex,
            s.captureRegionX, s.captureRegionY, s.captureRegionWidth, s.captureRegionHeight,
            s.captureWindowTitle, s.authenticationEnabled, s.userName, s.password, s.allowedIpAddresses);
    };
    const auto secondary = [](const AppSettings& s)
    { return std::tie(s.subRtspPath, s.subWidth, s.subHeight, s.subFrameRate, s.subBitrateKbps, s.subGopSize); };
    return structural(a) != structural(b) || a.subStreamEnabled != b.subStreamEnabled ||
        ((a.subStreamEnabled || b.subStreamEnabled) && secondary(a) != secondary(b));
}

std::wstring BuildOverlayText(const AppSettings& settings)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[16]{}, time[16]{};
    swprintf_s(date, L"%02u.%02u.%04u", now.wDay, now.wMonth, now.wYear);
    swprintf_s(time, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
    std::wstring text = settings.overlayTemplate;
    const auto replace = [&text](const std::wstring& token, const std::wstring& value)
    {
        size_t position = 0;
        while ((position = text.find(token, position)) != std::wstring::npos)
        { text.replace(position, token.size(), value); position += value.size(); }
    };
    replace(L"\\n", L"\n");
    replace(L"{camera}", settings.cameraName);
    replace(L"{date}", date);
    replace(L"{time}", time);
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD computerSize = ARRAYSIZE(computer);
    GetComputerNameW(computer, &computerSize);
    replace(L"{computer}", computer);
    wchar_t user[256]{}; DWORD userSize = ARRAYSIZE(user);
    GetUserNameW(user, &userSize);
    replace(L"{user}", user);
    while (!text.empty() && (text.front() == L'\n' || text.front() == L' ')) text.erase(text.begin());
    while (!text.empty() && (text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

void RescalePrivacyMasks(std::vector<PrivacyMask>& masks, uint32_t oldWidth, uint32_t oldHeight,
                         uint32_t newWidth, uint32_t newHeight)
{
    if (!oldWidth || !oldHeight || !newWidth || !newHeight) return;
    for (auto& mask : masks)
    {
        const int right = MulDiv(mask.x + mask.width, newWidth, oldWidth);
        const int bottom = MulDiv(mask.y + mask.height, newHeight, oldHeight);
        mask.x = MulDiv(mask.x, newWidth, oldWidth);
        mask.y = MulDiv(mask.y, newHeight, oldHeight);
        mask.width = std::max(1, right - mask.x);
        mask.height = std::max(1, bottom - mask.y);
    }
}

std::string WideToUtf8String(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
