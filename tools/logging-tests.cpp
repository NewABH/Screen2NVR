// Exercise the production logger on private workspace files, without starting capture/network services.
#define wmain Screen2NvrEntryForLoggingTest
#include "../ScreenCapture.cpp"
#undef wmain
#include <fstream>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TestDirectory
{
    std::filesystem::path path;
    TestDirectory()
    {
        GUID id{};
        wchar_t suffix[40]{};
        Require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, suffix, ARRAYSIZE(suffix)), "Test directory ID failed");
        path = std::filesystem::current_path() / L"x64" / L"logging-tests" / suffix;
        Require(std::filesystem::create_directory(path), "Test directory already exists or cannot be created");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored); // Only this exact unique test directory created above.
    }
};

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "Cannot read test log");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

struct QuietConsole : std::streambuf
{
    std::streambuf* original = std::cout.rdbuf(this);
    ~QuietConsole() { std::cout.rdbuf(original); }
    std::streamsize xsputn(const char*, std::streamsize size) override { return size; }
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
};

void Tests()
{
    TestDirectory directory;
    const auto path = directory.path / L"Screen2NVR.log";
    const auto backup = directory.path / L"Screen2NVR.log.1";
    {
        QuietConsole quiet;
        Logger logger(path.wstring());
        logger.Write("disabled by default");
        Require(!std::filesystem::exists(path) && !std::filesystem::exists(backup), "Disabled logger created a file");
        Require(logger.SetEnabled(true) == ERROR_SUCCESS, "Cannot enable logging");
        logger.Write("first enabled message");
        Require(logger.SetEnabled(false) == ERROR_SUCCESS, "Cannot disable logging");
        const auto initial = Read(path);
        Require(initial.compare(0, 3, "\xEF\xBB\xBF") == 0 && initial.find("first enabled message") != std::string::npos,
                "Enabled log missing UTF-8 BOM/message");
        logger.Write("must not reach disk");
        Require(Read(path) == initial && !std::filesystem::exists(backup), "Disabled logger modified old logs");
        Require(logger.SetEnabled(true) == ERROR_SUCCESS, "Cannot re-enable logging");
        logger.Write("second enabled message");
        logger.SetEnabled(false);
        const auto appended = Read(path);
        Require(appended.find(initial) == 0 && appended.find("second enabled message") != std::string::npos &&
                appended.find("must not reach disk") == std::string::npos, "Re-enable lost old content or flushed disabled messages");

        logger.SetEnabled(true);
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; ++i)
            writers.emplace_back([&logger] { for (int n = 0; n < 2000; ++n) logger.Write("concurrent message"); });
        for (int i = 0; i < 20; ++i)
        {
            logger.SetEnabled(false);
            Require(logger.SetEnabled(true) == ERROR_SUCCESS, "Concurrent toggle failed");
        }
        logger.SetEnabled(false);
        const auto stopped = Read(path);
        for (auto& writer : writers) writer.join();
        Require(Read(path) == stopped, "A worker wrote to disk after disabling completed");

        logger.SetEnabled(true);
        for (int i = 0; i < 2600; ++i) logger.Write(std::string(1024, 'r'));
        logger.SetEnabled(false);
        Require(std::filesystem::exists(backup) && std::filesystem::file_size(path) <= 1024 * 1024 &&
                std::filesystem::file_size(backup) <= 1024 * 1024, "Log rotation exceeded its bounds");
        const auto rotated = Read(path), rotatedBackup = Read(backup);
        logger.Write("disabled after rotation");
        Require(Read(path) == rotated && Read(backup) == rotatedBackup, "Disabled logger modified rotated logs");
        HANDLE exclusive = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(exclusive != INVALID_HANDLE_VALUE, "Disabling left the file handle open");
        CloseHandle(exclusive);
    }
    std::cout << "PASS: default-off, UTF-8, enable/disable/re-enable, concurrent writes, immediate handle close, bounded rotation\n";
    {
        const auto legacyPath = directory.path / L"legacy.log";
        { std::ofstream legacy(legacyPath, std::ios::binary); legacy << "old log without BOM"; }
        QuietConsole quiet;
        Logger logger(legacyPath.wstring());
        logger.Write("not enabled");
        Require(Read(legacyPath) == "old log without BOM" && !std::filesystem::exists(legacyPath.wstring() + L".1"),
                "Disabled constructor migrated the old log");
        Require(logger.SetEnabled(true) == ERROR_SUCCESS, "Legacy enable failed");
        logger.Write("new UTF-8 log");
        logger.SetEnabled(false);
        Require(Read(legacyPath.wstring() + L".1") == "old log without BOM", "Explicit enable failed to preserve legacy log");
        Logger impossible((legacyPath / L"cannot-create.log").wstring());
        Require(impossible.SetEnabled(true) != ERROR_SUCCESS, "Logging failure was silently reported as success");
    }
    std::cout << "PASS: old logs untouched while disabled; explicit enable preserves legacy backup; I/O errors reported\n";
}
}

int main()
{
    try { Tests(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
