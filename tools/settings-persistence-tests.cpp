#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <aclapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "../Settings.h"

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<BYTE> Read(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "Cannot read test file");
    const DWORD size = GetFileSize(file, nullptr);
    std::vector<BYTE> bytes(size);
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), size, &read, nullptr) && read == size;
    CloseHandle(file);
    Require(ok, "Short read of test file");
    return bytes;
}

void Write(const std::wstring& path, const std::vector<BYTE>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "Cannot write test file");
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    Require(ok, "Short write of test file");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

std::wstring Value(const std::wstring& path, const wchar_t* section, const wchar_t* key)
{
    wchar_t buffer[4096]{};
    GetPrivateProfileStringW(section, key, L"__missing__", buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

struct TestDirectory
{
    std::filesystem::path path;
    TestDirectory()
    {
        GUID id{};
        wchar_t suffix[40]{};
        Require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, suffix, ARRAYSIZE(suffix)) != 0,
                "Cannot create test directory ID");
        // Stay in the build workspace; never touch ProgramData or the user's HKCU Run entry.
        path = std::filesystem::current_path() / L"x64" / L"settings-tests" / (std::wstring(L"persistence-") + suffix);
        Require(std::filesystem::create_directories(path), "Cannot create test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored); // Exact unique directory created above, no user data.
    }
};

struct DenyDirectoryCreation
{
    std::wstring path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL acl = nullptr;
    PACL denied = nullptr;
    DenyDirectoryCreation(const std::wstring& directory) : path(directory)
    {
        Require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, &acl, nullptr, &original) == ERROR_SUCCESS,
                "Cannot read test directory ACL");
        BYTE sid[SECURITY_MAX_SID_SIZE]{};
        DWORD size = sizeof(sid);
        Require(CreateWellKnownSid(WinWorldSid, nullptr, sid, &size) != FALSE, "Cannot create test SID");
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        entry.grfAccessMode = DENY_ACCESS;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        Require(SetEntriesInAclW(1, &entry, acl, &denied) == ERROR_SUCCESS, "Cannot build denied test ACL");
        Require(SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, denied, nullptr) == ERROR_SUCCESS,
                "Cannot apply denied test ACL");
    }
    ~DenyDirectoryCreation()
    {
        SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, acl, nullptr);
        LocalFree(denied);
        LocalFree(original);
    }
};
}

int wmain()
{
    try
    {
        TestDirectory directory;
        const std::wstring path = (directory.path / L"config.ini").wstring();
        const std::wstring backup = path + L".bak";
        AppSettings first;
        first.cameraName = L"Касса №5 & Вход";
        first.captureWindowTitle = L"Окно кассира";
        first.timestampFontName = L"Segoe UI";
        first.authenticationEnabled = true;
        first.userName = L"test-user";
        first.password = L"test-only-password";
        first.privacyMasks = L"10,20,200,100";
        first.subStreamEnabled = true;
        first.subRtspPath = L"custom-sub";
        first.subWidth = 800; first.subHeight = 450; first.subFrameRate = 7;
        first.subBitrateKbps = 600; first.subGopSize = 14;
        std::wstring error;
        Require(SaveAppSettingsFile(first, path, error), "Initial Unicode save failed");
        const auto original = Read(path);
        Require(original.size() > 2 && original[0] == 0xFF && original[1] == 0xFE, "Missing UTF-16LE BOM");
        Require(Value(path, L"Camera", L"Name") == first.cameraName &&
                Value(path, L"Capture", L"WindowTitle") == first.captureWindowTitle &&
                Value(path, L"Security", L"Password") == first.password &&
                Value(path, L"Privacy", L"Masks") == first.privacyMasks, "Unicode or settings roundtrip failed");
        std::cout << "PASS: first save, UTF-16 and settings roundtrip\n";
        Require(!first.loggingEnabled && Value(path, L"Logging", L"Enabled") == L"0", "Logging must be disabled by default");
        auto logging = first; logging.loggingEnabled = true;
        Require(SaveAppSettingsFile(logging, path, error) && Value(path, L"Logging", L"Enabled") == L"1",
                "Logging enabled value not persisted");
        Require(SaveAppSettingsFile(first, path, error) && Value(path, L"Logging", L"Enabled") == L"0",
                "Logging disabled value not persisted");
        std::cout << "PASS: logging default-off and enable/disable persistence\n";
        auto english = first; english.uiLanguage = 1;
        Require(SaveAppSettingsFile(english, path, error) && Value(path, L"General", L"Language") == L"1",
                "English language was not persisted");
        Require(SaveAppSettingsFile(first, path, error) && Value(path, L"General", L"Language") == L"0",
                "Russian language was not persisted");
        std::cout << "PASS: Russian/English language persistence\n";
        Require(Value(path, L"Camera", L"Manufacturer") == L"__missing__" && Value(path, L"Camera", L"Serial") == L"__missing__",
                "Hardware identity must not be persisted as portable camera settings");
        Require(Value(path, L"Network", L"OnvifPort") == L"80" &&
                Value(path, L"Network", L"RtspPath") == L"Streaming/Channels/101",
                "New ONVIF port/hierarchical RTSP path not persisted");
        Require(Value(path, L"SubStream", L"Enabled") == L"1" &&
                Value(path, L"SubStream", L"RtspPath") == L"custom-sub" &&
                Value(path, L"SubStream", L"Width") == L"800" &&
                Value(path, L"SubStream", L"Height") == L"450" &&
                Value(path, L"SubStream", L"Fps") == L"7" &&
                Value(path, L"SubStream", L"BitrateKbps") == L"600" &&
                Value(path, L"SubStream", L"Gop") == L"14", "Secondary settings were not persisted independently");
        auto sub = MakeSubStreamSettings(first);
        Require(sub.outputWidth == 800 && sub.outputHeight == 450 && sub.frameRate == 7 &&
                sub.bitrateKbps == 600 && sub.gopSize == 14 && sub.overlayTemplate.empty() &&
                !sub.showCursor && sub.privacyMasks.empty(), "Secondary stream reprocesses shared overlays");
        std::cout << "PASS: independent secondary settings and shared composition\n";
        Require(Value(path, L"Timestamp", L"TemplateVersion") == L"2", "Missing template migration marker");
        auto emptyText = first; emptyText.overlayTemplate.clear();
        Require(SaveAppSettingsFile(emptyText, path, error) && Value(path, L"Timestamp", L"Template").empty(),
                "Empty overlay template was not saved");
        Require(SaveAppSettingsFile(first, path, error), "Restore test settings failed");
        std::cout << "PASS: explicit empty overlay template preserved\n";

        AppSettings second = first;
        second.cameraName = L"Второе имя камеры";
        second.frameRate = 25;
        Require(SaveAppSettingsFile(second, path, error), "Second save failed");
        Require(Read(backup) == original && Value(path, L"Camera", L"Name") == second.cameraName &&
                Value(path, L"Video", L"Fps") == L"25", "Replacement, backup or INI cache failed");
        std::cout << "PASS: replacement, byte-exact backup and cache refresh\n";

        // Reproduce the old EnsureUnicodeIniFile failure despite full file permissions.
        HANDLE observer = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        Require(observer != INVALID_HANDLE_VALUE, "Cannot open sharing regression handle");
        HANDLE oldReader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
        const DWORD oldError = GetLastError();
        if (oldReader != INVALID_HANDLE_VALUE) CloseHandle(oldReader);
        const bool savedWithObserver = SaveAppSettingsFile(first, path, error);
        CloseHandle(observer);
        Require(oldReader == INVALID_HANDLE_VALUE && oldError == ERROR_SHARING_VIOLATION,
                "Old restrictive sharing failure was not reproduced");
        Require(savedWithObserver, "Correctly shared observer blocks saving");
        std::cout << "PASS: old Unicode-file error reproduced; fixed save succeeds with shared delete handle\n";

        HANDLE shortLock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(shortLock != INVALID_HANDLE_VALUE, "Cannot create transient lock");
        std::thread unlock([shortLock] { Sleep(150); CloseHandle(shortLock); });
        const bool retried = SaveAppSettingsFile(second, path, error);
        unlock.join();
        Require(retried, "Short file lock was not retried");
        std::cout << "PASS: transient file lock is retried\n";

        const auto beforeFailure = Read(path);
        const auto backupBeforeFailure = Read(backup);
        HANDLE longLock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(longLock != INVALID_HANDLE_VALUE, "Cannot create persistent lock");
        const bool lockedResult = SaveAppSettingsFile(first, path, error);
        CloseHandle(longLock);
        Require(!lockedResult && error.find(L"32") != std::wstring::npos &&
                Read(path) == beforeFailure && Read(backup) == backupBeforeFailure,
                "Persistent lock damages files or hides Windows error");
        std::cout << "PASS: persistent lock preserves both files and reports Windows error 32\n";

        HANDLE noRename = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        Require(noRename != INVALID_HANDLE_VALUE, "Cannot lock final rename");
        const bool renameResult = SaveAppSettingsFile(first, path, error);
        CloseHandle(noRename);
        Require(!renameResult && error.find(L"32") != std::wstring::npos &&
                Read(path) == beforeFailure && Read(backup) == backupBeforeFailure,
                "Failed final rename damages live settings or backup");
        std::cout << "PASS: final replacement failure keeps both files unchanged\n";

        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Cannot set read-only");
        const bool readOnlyResult = SaveAppSettingsFile(first, path, error);
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        Require(!readOnlyResult && error.find(L"5.") != std::wstring::npos && Read(path) == beforeFailure &&
                Read(backup) == backupBeforeFailure, "Read-only file was overwritten or misreported");
        std::cout << "PASS: read-only file is preserved, with actionable error 5\n";

        Require(SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Cannot protect backup");
        const bool backupResult = SaveAppSettingsFile(first, path, error);
        SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
        Require(!backupResult && Read(path) == beforeFailure && Read(backup) == backupBeforeFailure,
                "Protected backup caused partial overwrite");
        std::cout << "PASS: protected backup prevents commit without losing data\n";

        {
            DenyDirectoryCreation denied(directory.path.wstring());
            Require(!SaveAppSettingsFile(first, path, error) && error.find(L"5.") != std::wstring::npos,
                    "Denied directory does not report access denied");
        }
        Require(Read(path) == beforeFailure && Read(backup) == backupBeforeFailure,
                "Denied directory damaged existing data");
        std::cout << "PASS: denied directory preserves data and reports exact permission error\n";

        const std::string legacy = "[Camera]\r\nName=Legacy camera\r\n[Network]\r\nRtspPort=8554\r\n";
        Write(path, std::vector<BYTE>(legacy.begin(), legacy.end()));
        const auto legacyBytes = Read(path);
        Require(SaveAppSettingsFile(first, path, error) && Read(backup) == legacyBytes &&
                Value(path, L"Camera", L"Name") == first.cameraName, "Legacy migration loses backup or Unicode");
        std::cout << "PASS: legacy INI converted without destroying original bytes\n";

        Require(WritePrivateProfileStringW(L"Future", L"UnknownKey", L"keep me", path.c_str()) != FALSE,
                "Cannot prepare unknown INI key");
        Require(SaveAppSettingsFile(second, path, error) && Value(path, L"Future", L"UnknownKey") == L"keep me",
                "Unknown UTF-16 INI key lost");
        std::cout << "PASS: unknown existing Unicode INI keys preserved\n";

        const auto goodBackup = Read(backup);
        Write(path, { 0xFF, 0xFE });
        Require(SaveAppSettingsFile(first, path, error) && Read(backup) == goodBackup,
                "Recovery save replaced good backup with damaged data");
        std::cout << "PASS: recovery save keeps the known-good backup\n";

        const auto good = Read(path);
        AppSettings invalid = first;
        invalid.cameraName = L"broken\r\n[Security]";
        Require(!SaveAppSettingsFile(invalid, path, error) && Read(path) == good,
                "Invalid INI value altered live settings");
        std::cout << "PASS: invalid multiline value cannot partially overwrite settings\n";

        bool savedOne = false, savedTwo = false;
        std::thread one([&] { std::wstring failure; savedOne = SaveAppSettingsFile(first, path, failure); });
        std::thread two([&] { std::wstring failure; savedTwo = SaveAppSettingsFile(second, path, failure); });
        one.join(); two.join();
        const auto name = Value(path, L"Camera", L"Name");
        const auto fps = Value(path, L"Video", L"Fps");
        Require(savedOne && savedTwo &&
                ((name == first.cameraName && fps == L"12") || (name == second.cameraName && fps == L"25")),
                "Concurrent saves interleaved settings");
        for (const auto& item : std::filesystem::directory_iterator(directory.path))
            Require(item.path().filename() == L"config.ini" || item.path().filename() == L"config.ini.bak",
                    "Temporary settings files leaked");
        std::cout << "PASS: concurrent saves serialized; no temporary files left\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
