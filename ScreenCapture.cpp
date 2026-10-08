#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "RtspServer.h"
#include "Screen2ONVIF.h"
#include "Settings.h"
#include "Localization.h"
#include "CaptureGeometry.h"
#include "TrayApp.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kMutexName[] = L"Local\\Screen2NVR.ScreenCapture";
UINT kOutputWidth = 1920;
UINT kOutputHeight = 1080;
UINT kFrameRate = 12;
UINT kBitrate = 2'000'000;
UINT kGopSize = 25;
LONGLONG kFrameDuration = 10'000'000LL / 12;

std::atomic_bool g_running{ true };

class HrError final : public std::runtime_error
{
public:
    HrError(const char* operation, HRESULT result) : std::runtime_error(operation), hr(result) {}
    HRESULT hr;
};

void CheckHr(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw HrError(operation, hr);
}

std::string HresultText(HRESULT hr)
{
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return text.str();
}


std::wstring AsciiToWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

class Logger
{
public:
    explicit Logger(std::wstring path = {})
    {
        // Merely constructing/calling the logger must not touch old logs when disabled.
        logPath_ = path.empty() ? GetScreen2NvrLogDirectory() + L"\\Screen2NVR.log" : std::move(path);
        backupLogPath_ = logPath_ + L".1";
    }

    ~Logger()
    {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    DWORD SetEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled)
        {
            if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            fileSize_ = 0;
            return ERROR_SUCCESS;
        }
        if (file_ != INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(logPath_).parent_path(), error);
        if (error) return static_cast<DWORD>(error.value());
        MigrateLegacyLogEncoding();
        fileSize_ = 0;
        OpenFile();
        return file_ != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : GetLastError();
    }

    void Write(const std::string& message)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char prefix[40]{};
        sprintf_s(prefix, "[%04u-%02u-%02u %02u:%02u:%02u] ",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        const std::string line = std::string(prefix) + message;

        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << line << std::endl;
        if (file_ != INVALID_HANDLE_VALUE)
        {
            const std::string diskLine = line + "\r\n";
            if (fileSize_ + diskLine.size() > kMaximumLogSize)
                Rotate();

            DWORD written = 0;
            if (file_ != INVALID_HANDLE_VALUE &&
                WriteFile(file_, diskLine.data(), static_cast<DWORD>(diskLine.size()), &written, nullptr))
                fileSize_ += written;
        }
    }

private:
    void MigrateLegacyLogEncoding()
    {
        HANDLE existing = CreateFileW(logPath_.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (existing == INVALID_HANDLE_VALUE) return;
        BYTE prefix[3]{};
        DWORD read = 0;
        const BOOL readSucceeded = ReadFile(existing, prefix, sizeof(prefix), &read, nullptr);
        CloseHandle(existing);
        if (!readSucceeded) return;
        if (read > 0 && !(read >= 3 && prefix[0] == 0xEF && prefix[1] == 0xBB && prefix[2] == 0xBF))
            MoveFileExW(logPath_.c_str(), backupLogPath_.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void OpenFile()
    {
        file_ = CreateFileW(logPath_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file_, &size)) fileSize_ = static_cast<ULONGLONG>(size.QuadPart);
            if (fileSize_ == 0)
            {
                constexpr BYTE utf8Bom[] = { 0xEF, 0xBB, 0xBF };
                DWORD written = 0;
                if (WriteFile(file_, utf8Bom, sizeof(utf8Bom), &written, nullptr)) fileSize_ = written;
            }
        }
    }

    void Rotate()
    {
        if (file_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        MoveFileExW(logPath_.c_str(), backupLogPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        fileSize_ = 0;
        OpenFile();
    }

    static constexpr ULONGLONG kMaximumLogSize = 1024ULL * 1024ULL;
    std::wstring logPath_;
    std::wstring backupLogPath_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    ULONGLONG fileSize_ = 0;
    std::mutex mutex_;
};

Logger& ApplicationLogger()
{
    static Logger logger;
    return logger;
}

void Log(const std::string& message) { ApplicationLogger().Write(message); }

bool ConfigureLogging(bool enabled, std::wstring& error)
{
    const DWORD code = ApplicationLogger().SetEnabled(enabled);
    if (code == ERROR_SUCCESS) return true;
    error = GetScreen2NvrLogDirectory() + UiText(L"\\Screen2NVR.log\nКод ошибки Windows: ",
                                                  L"\\Screen2NVR.log\nWindows error code: ") + std::to_wstring(code);
    return false;
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

bool IsInteractiveDesktopAvailable()
{
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    wchar_t name[64]{}; DWORD required = 0;
    const bool available = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &required) != FALSE &&
                           _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return available;
}

const char* DxgiFormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case DXGI_FORMAT_NV12: return "DXGI_FORMAT_NV12";
    default: return "other/unknown";
    }
}

UINT SelectH264Level(UINT width, UINT height, UINT frameRate, UINT bitrateKbps)
{
    struct LevelLimit
    {
        UINT level;
        uint64_t maximumFrameMacroblocks;
        uint64_t maximumMacroblocksPerSecond;
        UINT maximumBitrateKbps;
    };
    static constexpr LevelLimit limits[] = {
        { 30, 1'620, 40'500, 10'000 }, { 31, 3'600, 108'000, 14'000 },
        { 32, 5'120, 216'000, 20'000 }, { 40, 8'192, 245'760, 20'000 },
        { 41, 8'192, 245'760, 50'000 }, { 42, 8'704, 522'240, 50'000 },
        { 50, 22'080, 589'824, 135'000 }, { 51, 36'864, 983'040, 240'000 },
        { 52, 36'864, 2'073'600, 240'000 }, { 60, 139'264, 4'177'920, 240'000 },
        { 61, 139'264, 8'355'840, 480'000 }, { 62, 139'264, 16'711'680, 800'000 }
    };
    const uint64_t frameMacroblocks = static_cast<uint64_t>((width + 15) / 16) * ((height + 15) / 16);
    const uint64_t macroblocksPerSecond = frameMacroblocks * frameRate;
    for (const LevelLimit& limit : limits)
        if (frameMacroblocks <= limit.maximumFrameMacroblocks &&
            macroblocksPerSecond <= limit.maximumMacroblocksPerSecond &&
            bitrateKbps <= limit.maximumBitrateKbps)
            return limit.level;
    return 62;
}

std::wstring CurrentTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t value[32]{};
    swprintf_s(value, L"%02u.%02u.%04u %02u:%02u:%02u",
               now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute, now.wSecond);
    return value;
}

struct RuntimeOptions
{
    uint64_t maxFrames = 0;
    uint64_t simulateStallAfterFrames = 0;
    bool captureOnly = false;
    bool pipelineTest = false;
    DWORD restartWaitProcessId = 0;
    int autoStartAction = -1;
    int languageAction = -1;
    bool forceSoftwareEncoderTest = false;
    bool securityTest = false;
    bool dualStreamTest = false;
    bool liveSettingsTest = false;
};

RuntimeOptions ParseOptions(int argc, wchar_t* argv[])
{
    RuntimeOptions options;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--once") options.maxFrames = 1;
        else if (argument == L"--capture-only") options.captureOnly = true;
        else if (argument == L"--pipeline-test") options.pipelineTest = true;
        else if (argument == L"--live-settings-test") options.liveSettingsTest = true;
        else if (argument == L"--dual-stream-test" || argument == L"--dual-capture-test")
        {
            options.dualStreamTest = true;
            options.pipelineTest = argument == L"--dual-stream-test";
            options.maxFrames = 360;
        }
        else if (argument.rfind(L"--frames=", 0) == 0) options.maxFrames = std::stoull(argument.substr(9));
        else if (argument.rfind(L"--simulate-stall-after=", 0) == 0)
            options.simulateStallAfterFrames = std::stoull(argument.substr(23));
        else if (argument == L"--restart-wait" && index + 1 < argc)
            options.restartWaitProcessId = static_cast<DWORD>(std::stoul(argv[++index]));
        else if (argument == L"--set-autostart=1") options.autoStartAction = 1;
        else if (argument == L"--set-autostart=0") options.autoStartAction = 0;
        else if (argument == L"--set-language=ru") options.languageAction = 0;
        else if (argument == L"--set-language=en") options.languageAction = 1;
        else if (argument == L"--software-encoder-test")
        {
            options.forceSoftwareEncoderTest = true;
            options.pipelineTest = true;
            options.maxFrames = 8;
        }
        else if (argument == L"--security-test")
        {
            options.securityTest = true;
            options.maxFrames = 240;
        }
        else throw std::invalid_argument("Unknown command-line argument");
    }
    return options;
}

class DesktopCapture
{
public:
    void Initialize(const AppSettings& settings, TrayRuntimeStatus* health = nullptr)
    {
        ComPtr<IDXGIFactory1> factory;
        CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

        UINT activeOutputIndex = 0;
        ComPtr<IDXGIAdapter1> fallbackAdapter;
        ComPtr<IDXGIOutput> fallbackOutput;
        DXGI_OUTPUT_DESC fallbackDesc{};
        UINT fallbackAdapterIndex = 0, fallbackOutputIndex = 0;
        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterHr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
            CheckHr(adapterHr, "IDXGIFactory1::EnumAdapters1");

            DXGI_ADAPTER_DESC1 adapterDesc{};
            CheckHr(adapter->GetDesc1(&adapterDesc), "IDXGIAdapter1::GetDesc1");
            Log("Adapter " + std::to_string(adapterIndex) + ": " + WideToUtf8String(adapterDesc.Description));

            for (UINT outputIndex = 0;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputHr = adapter->EnumOutputs(outputIndex, &output);
                if (outputHr == DXGI_ERROR_NOT_FOUND) break;
                CheckHr(outputHr, "IDXGIAdapter1::EnumOutputs");

                DXGI_OUTPUT_DESC outputDesc{};
                CheckHr(output->GetDesc(&outputDesc), "IDXGIOutput::GetDesc");
                Log("  Output " + std::to_string(outputIndex) + ": " + WideToUtf8String(outputDesc.DeviceName) +
                    (outputDesc.AttachedToDesktop ? " [ACTIVE]" : " [INACTIVE]"));

                if (outputDesc.AttachedToDesktop && !fallbackOutput)
                {
                    fallbackAdapter = adapter; fallbackOutput = output; fallbackDesc = outputDesc;
                    fallbackAdapterIndex = adapterIndex; fallbackOutputIndex = outputIndex;
                }

                if (outputDesc.AttachedToDesktop && activeOutputIndex++ == settings.monitorIndex)
                {
                    adapter_ = adapter;
                    output_ = output;
                    adapterIndex_ = adapterIndex;
                    outputIndex_ = outputIndex;
                    outputDesc_ = outputDesc;
                }
            }
        }

        if (!output_ && fallbackOutput)
        {
            Log("Configured monitor index was not found; falling back to the first active monitor");
            adapter_ = fallbackAdapter; output_ = fallbackOutput; outputDesc_ = fallbackDesc;
            adapterIndex_ = fallbackAdapterIndex; outputIndex_ = fallbackOutputIndex;
        }

        if (!adapter_ || !output_) throw std::runtime_error("No active DXGI output was found");

        DXGI_ADAPTER_DESC1 selectedDesc{};
        CheckHr(adapter_->GetDesc1(&selectedDesc), "IDXGIAdapter1::GetDesc1(selected)");
        Log("Using adapter " + std::to_string(adapterIndex_) + ", output " + std::to_string(outputIndex_));
        if (health)
        {
            std::lock_guard<std::mutex> lock(health->mutex);
            health->adapterName = selectedDesc.Description;
            health->monitorName = outputDesc_.DeviceName;
            health->sourceDescription = settings.captureMode == 0 ? UiText(L"Монитор целиком", L"Entire monitor") :
                settings.captureMode == 1 ? UiText(L"Область монитора", L"Monitor area") :
                settings.captureMode == 2 ? UiText(L"Окно: ", L"Window: ") + settings.captureWindowTitle :
                                            UiText(L"Активное окно", L"Active window");
        }

        constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actualLevel{};
        CheckHr(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                  levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                  &device_, &actualLevel, &context_), "D3D11CreateDevice");
        std::ostringstream levelText;
        levelText << "D3D11 device created. Feature level=0x" << std::hex << static_cast<unsigned int>(actualLevel);
        Log(levelText.str());
        sourceDesc_.Width = static_cast<UINT>(outputDesc_.DesktopCoordinates.right - outputDesc_.DesktopCoordinates.left);
        sourceDesc_.Height = static_cast<UINT>(outputDesc_.DesktopCoordinates.bottom - outputDesc_.DesktopCoordinates.top);
        sourceDesc_.MipLevels = 1;
        sourceDesc_.ArraySize = 1;
        sourceDesc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sourceDesc_.SampleDesc.Count = 1;
        sourceDesc_.Usage = D3D11_USAGE_DEFAULT;
        sourceDesc_.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        CheckHr(device_->CreateTexture2D(&sourceDesc_, nullptr, &lastFrame_),
                "CreateTexture2D(initial standby frame)");
        ComPtr<ID3D11RenderTargetView> standbyTarget;
        CheckHr(device_->CreateRenderTargetView(lastFrame_.Get(), nullptr, &standbyTarget),
                "CreateRenderTargetView(initial standby frame)");
        const float standbyColor[4] = { 0.035f, 0.045f, 0.06f, 1.0f };
        context_->ClearRenderTargetView(standbyTarget.Get(), standbyColor);
        formatChanged_ = true;
        CreateDuplication();
    }

    bool Acquire(bool waitForFrame)
    {
        lastAcquireChanged_ = false;
        if (!duplication_ && !CreateDuplication())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return static_cast<bool>(lastFrame_);
        }
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> desktopResource;
        HRESULT hr = duplication_->AcquireNextFrame(waitForFrame ? 1000 : 0, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return static_cast<bool>(lastFrame_);
        if (hr == DXGI_ERROR_ACCESS_LOST)
        {
            Log("Desktop Duplication access lost; recreating duplication interface");
            duplication_.Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            CreateDuplication();
            return static_cast<bool>(lastFrame_);
        }
        CheckHr(hr, "IDXGIOutputDuplication::AcquireNextFrame");

        ComPtr<ID3D11Texture2D> acquiredTexture;
        const HRESULT textureHr = desktopResource.As(&acquiredTexture);
        if (FAILED(textureHr))
        {
            duplication_->ReleaseFrame();
            CheckHr(textureHr, "QueryInterface(ID3D11Texture2D)");
        }

        D3D11_TEXTURE2D_DESC desc{};
        acquiredTexture->GetDesc(&desc);
        if (!realFrameReceived_)
        {
            Log("Capture texture: " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                ", format=" + std::to_string(static_cast<unsigned int>(desc.Format)) +
                " (" + DxgiFormatName(desc.Format) + ")");
            realFrameReceived_ = true;
        }
        if (!lastFrame_ || desc.Width != sourceDesc_.Width || desc.Height != sourceDesc_.Height ||
            desc.Format != sourceDesc_.Format)
        {
            sourceDesc_ = desc;
            D3D11_TEXTURE2D_DESC copyDesc = desc;
            copyDesc.ArraySize = 1;
            copyDesc.MipLevels = 1;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            CheckHr(device_->CreateTexture2D(&copyDesc, nullptr, &lastFrame_),
                    "ID3D11Device::CreateTexture2D(capture copy)");
            formatChanged_ = true;
            if (realFrameReceived_)
                Log("Capture format changed to " + std::to_string(desc.Width) + "x" +
                    std::to_string(desc.Height) + ", " + DxgiFormatName(desc.Format));
        }
        context_->CopyResource(lastFrame_.Get(), acquiredTexture.Get());
        CheckHr(duplication_->ReleaseFrame(), "IDXGIOutputDuplication::ReleaseFrame");
        lastAcquireChanged_ = true;
        return true;
    }

    bool ConsumeFormatChanged() { const bool value = formatChanged_; formatChanged_ = false; return value; }
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11Texture2D* Frame() const { return lastFrame_.Get(); }
    const D3D11_TEXTURE2D_DESC& SourceDesc() const { return sourceDesc_; }
    DXGI_MODE_ROTATION Rotation() const { return outputDesc_.Rotation; }
    const RECT& DesktopCoordinates() const { return outputDesc_.DesktopCoordinates; }
    bool LastAcquireChanged() const { return lastAcquireChanged_; }
    bool HasRealFrame() const { return realFrameReceived_; }

private:
    bool CreateDuplication()
    {
        DXGI_OUTPUT_DESC refreshedDesc{};
        const HRESULT refreshResult = output_->GetDesc(&refreshedDesc);
        if (FAILED(refreshResult)) throw HrError("IDXGIOutput::GetDesc(refresh)", refreshResult);
        if (!refreshedDesc.AttachedToDesktop) return false;
        if (refreshedDesc.Rotation != outputDesc_.Rotation ||
            !EqualRect(&refreshedDesc.DesktopCoordinates, &outputDesc_.DesktopCoordinates))
            formatChanged_ = true;
        outputDesc_ = refreshedDesc;
        ComPtr<IDXGIOutput1> output1;
        CheckHr(output_.As(&output1), "QueryInterface(IDXGIOutput1)");
        const HRESULT hr = output1->DuplicateOutput(device_.Get(), &duplication_);
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == E_ACCESSDENIED)
        {
            duplication_.Reset();
            return false;
        }
        CheckHr(hr, "IDXGIOutput1::DuplicateOutput");
        return true;
    }

    UINT adapterIndex_ = 0;
    UINT outputIndex_ = 0;
    bool formatChanged_ = false;
    bool lastAcquireChanged_ = false;
    bool realFrameReceived_ = false;
    DXGI_OUTPUT_DESC outputDesc_{};
    D3D11_TEXTURE2D_DESC sourceDesc_{};
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> lastFrame_;
};

bool TestPixelBlack(ID3D11Device* device, ID3D11DeviceContext* context,
                    ID3D11Texture2D* texture, UINT x, UINT y)
{
    // Diagnostic only: never called by the normal per-frame capture path.
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Width = desc.Height = 1; desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> pixel;
    CheckHr(device->CreateTexture2D(&desc, nullptr, &pixel), "Create diagnostic pixel");
    const D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
    context->CopySubresourceRegion(pixel.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHr(context->Map(pixel.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read diagnostic pixel");
    const BYTE* bgra = static_cast<const BYTE*>(mapped.pData);
    const bool black = bgra[0] == 0 && bgra[1] == 0 && bgra[2] == 0;
    context->Unmap(pixel.Get(), 0);
    return black;
}

class GpuProcessor
{
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    const D3D11_TEXTURE2D_DESC& sourceDesc,
                    DXGI_MODE_ROTATION sourceRotation, const AppSettings& settings,
                    const RECT& desktopCoordinates,
                    TrayRuntimeStatus* health = nullptr)
    {
        device_ = device;
        context_ = context;
        sourceDesc_ = sourceDesc;
        sourceRotation_ = sourceRotation;
        settings_ = settings;
        health_ = health;
        kOutputWidth = settings.outputWidth;
        kOutputHeight = settings.outputHeight;
        kFrameRate = settings.frameRate;
        desktopCoordinates_ = desktopCoordinates;
        ParsePrivacyMasks();
        CheckHr(device_->QueryInterface(IID_PPV_ARGS(&videoDevice_)), "QueryInterface(ID3D11VideoDevice)");
        CheckHr(context_->QueryInterface(IID_PPV_ARGS(&videoContext_)), "QueryInterface(ID3D11VideoContext)");
        videoContext_.As(&videoContext1_);

        convertEnumerator_ = CreateEnumerator(kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight);
        CheckFormat(convertEnumerator_.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);
        CheckFormat(convertEnumerator_.Get(), DXGI_FORMAT_NV12, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT);
        CheckHr(videoDevice_->CreateVideoProcessor(convertEnumerator_.Get(), 0, &convertProcessor_),
                "CreateVideoProcessor(NV12 conversion)");

        D3D11_TEXTURE2D_DESC bgraDesc{};
        bgraDesc.Width = kOutputWidth;
        bgraDesc.Height = kOutputHeight;
        bgraDesc.MipLevels = 1;
        bgraDesc.ArraySize = 1;
        bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bgraDesc.SampleDesc.Count = 1;
        bgraDesc.Usage = D3D11_USAGE_DEFAULT;
        bgraDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bgraDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        CheckHr(device_->CreateTexture2D(&bgraDesc, nullptr, &bgraTexture_), "CreateTexture2D(BGRA intermediate)");
        CheckHr(device_->CreateRenderTargetView(bgraTexture_.Get(), nullptr, &bgraRenderTarget_),
                "CreateRenderTargetView(BGRA intermediate)");
        bgraInputView_ = CreateInputView(bgraTexture_.Get(), convertEnumerator_.Get(), 0);
        InitializeShaders();
        InitializeDirect2D();
        Log("GPU processor initialized: shader resize/HDR tone map -> BGRA8 -> timestamp -> NV12");
    }

    void Process(ID3D11Texture2D* source, ID3D11Texture2D* nv12Target, UINT targetSubresource)
    {
        RenderSource(source);
        PublishPreview(source);
        DrawPrivacyMasks();
        DrawMouseClickHighlights();
        if (!settings_.overlayTemplate.empty()) DrawTimestamp();
        if (settings_.showCursor) DrawCursor();
        ConvertToNv12(nv12Target, targetSubresource);
    }

    void ProcessStandby(ID3D11Texture2D* nv12Target, UINT targetSubresource)
    {
        const float color[4] = { 0.035f, 0.045f, 0.06f, 1.0f };
        context_->ClearRenderTargetView(bgraRenderTarget_.Get(), color);
        d2dContext_->BeginDraw();
        ComPtr<ID2D1SolidColorBrush> brush;
        CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush),
                "CreateSolidColorBrush(standby)");
        ComPtr<IDWriteTextFormat> format;
        CheckHr(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 32.0f, IsEnglishUi() ? L"en-US" : L"ru-RU", &format),
                "CreateTextFormat(standby)");
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const std::wstring text = settings_.standbyText + UiText(L"\nПоследний кадр: ", L"\nLast frame: ") + CurrentTimestamp();
        d2dContext_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                              D2D1::RectF(40, 40, static_cast<float>(kOutputWidth - 40),
                                          static_cast<float>(kOutputHeight - 40)), brush.Get());
        CheckHr(d2dContext_->EndDraw(), "EndDraw(standby)");
        PublishPreview(nullptr);
        ConvertToNv12(nv12Target, targetSubresource);
    }

    ID3D11Texture2D* ComposedFrame() const { return bgraTexture_.Get(); }

    void UpdateLiveSettings(const AppSettings& settings)
    {
        CopyLiveSettings(settings_, settings);
        ParsePrivacyMasks();
        UpdateTextFormat();
        timestampLayout_.Reset();
        cachedTimestamp_.clear();
        backgroundBrush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f,
            settings_.timestampBackgroundOpacity / 100.0f));
    }

private:
    UINT kOutputWidth = 1920, kOutputHeight = 1080, kFrameRate = 12;
    void ConvertToNv12(ID3D11Texture2D* nv12Target, UINT targetSubresource)
    {
        ComPtr<ID3D11VideoProcessorOutputView> nv12View = GetNv12OutputView(nv12Target, targetSubresource);
        RECT outputRect{ 0, 0, static_cast<LONG>(kOutputWidth), static_cast<LONG>(kOutputHeight) };
        videoContext_->VideoProcessorSetStreamSourceRect(convertProcessor_.Get(), 0, TRUE, &outputRect);
        videoContext_->VideoProcessorSetStreamDestRect(convertProcessor_.Get(), 0, TRUE, &outputRect);
        videoContext_->VideoProcessorSetOutputTargetRect(convertProcessor_.Get(), TRUE, &outputRect);
        if (videoContext1_)
        {
            videoContext1_->VideoProcessorSetStreamColorSpace1(convertProcessor_.Get(), 0,
                                                               DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            videoContext1_->VideoProcessorSetOutputColorSpace1(convertProcessor_.Get(),
                                                               DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = bgraInputView_.Get();
        CheckHr(videoContext_->VideoProcessorBlt(convertProcessor_.Get(), nv12View.Get(), 0, 1, &stream),
                "VideoProcessorBlt(NV12)");
    }
    void InitializeShaders()
    {
        static constexpr char shaderSource[] = R"(
Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);
cbuffer ConversionSettings : register(b0)
{
    uint hdrInput;
    uint sourceRotation;
    float2 padding;
    float4 sourceRectangle;
};

struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float2 sourceUv = lerp(sourceRectangle.xy, sourceRectangle.zw, input.uv);
    if (sourceRotation == 2) sourceUv = float2(sourceUv.y, 1.0 - sourceUv.x);
    else if (sourceRotation == 3) sourceUv = float2(1.0 - sourceUv.x, 1.0 - sourceUv.y);
    else if (sourceRotation == 4) sourceUv = float2(1.0 - sourceUv.y, sourceUv.x);
    float4 color = sourceTexture.Sample(linearSampler, sourceUv);
    if (hdrInput != 0)
    {
        float3 value = max(color.rgb, 0.0);
        value = (value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14);
        color.rgb = pow(saturate(value), 1.0 / 2.2);
    }
    return float4(saturate(color.rgb), 1.0);
}
)";

        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> pixelBytecode;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "ScreenCaptureShader", nullptr, nullptr,
                                "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &vertexBytecode, &errors);
        if (FAILED(hr))
        {
            if (errors) Log(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            CheckHr(hr, "D3DCompile(vertex shader)");
        }
        errors.Reset();
        hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "ScreenCaptureShader", nullptr, nullptr,
                        "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &pixelBytecode, &errors);
        if (FAILED(hr))
        {
            if (errors) Log(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            CheckHr(hr, "D3DCompile(pixel shader)");
        }
        CheckHr(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                            nullptr, &vertexShader_), "CreateVertexShader");
        CheckHr(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                           nullptr, &pixelShader_), "CreatePixelShader");

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        CheckHr(device_->CreateSamplerState(&samplerDesc, &sampler_), "CreateSamplerState");

        D3D11_BUFFER_DESC settingsDesc{};
        settingsDesc.ByteWidth = 32;
        settingsDesc.Usage = D3D11_USAGE_DEFAULT;
        settingsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CheckHr(device_->CreateBuffer(&settingsDesc, nullptr, &conversionSettings_),
                "CreateBuffer(conversion settings)");
    }

    void RenderSource(ID3D11Texture2D* source, ID3D11RenderTargetView* previewTarget = nullptr,
                      UINT previewWidth = 0, UINT previewHeight = 0)
    {
        if (!sourceShaderView_)
            CheckHr(device_->CreateShaderResourceView(source, nullptr, &sourceShaderView_),
                    "CreateShaderResourceView(desktop)");

        if (!previewTarget) currentCrop_ = CaptureScreenRectangle();
        struct ConversionConstants
        {
            UINT hdrInput;
            UINT sourceRotation;
            float padding[2];
            float sourceRectangle[4];
        } constants{};
        constants.hdrInput = sourceDesc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1U : 0U;
        constants.sourceRotation = static_cast<UINT>(sourceRotation_);
        const float desktopWidth = static_cast<float>(std::max<LONG>(1, desktopCoordinates_.right - desktopCoordinates_.left));
        const float desktopHeight = static_cast<float>(std::max<LONG>(1, desktopCoordinates_.bottom - desktopCoordinates_.top));
        constants.sourceRectangle[0] = (currentCrop_.left - desktopCoordinates_.left) / desktopWidth;
        constants.sourceRectangle[1] = (currentCrop_.top - desktopCoordinates_.top) / desktopHeight;
        constants.sourceRectangle[2] = (currentCrop_.right - desktopCoordinates_.left) / desktopWidth;
        constants.sourceRectangle[3] = (currentCrop_.bottom - desktopCoordinates_.top) / desktopHeight;
        context_->UpdateSubresource(conversionSettings_.Get(), 0, nullptr, &constants, 0, 0);

        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(previewTarget ? previewWidth : kOutputWidth),
                                      static_cast<float>(previewTarget ? previewHeight : kOutputHeight), 0.0f, 1.0f };
        ID3D11RenderTargetView* target = previewTarget ? previewTarget : bgraRenderTarget_.Get();
        ID3D11ShaderResourceView* sourceResource = sourceShaderView_.Get();
        ID3D11SamplerState* sampler = sampler_.Get();
        ID3D11Buffer* settingsBuffer = conversionSettings_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &sourceResource);
        context_->PSSetSamplers(0, 1, &sampler);
        context_->PSSetConstantBuffers(0, 1, &settingsBuffer);
        context_->Draw(3, 0);

        sourceResource = nullptr;
        context_->PSSetShaderResources(0, 1, &sourceResource);
    }

    void PublishPreview(ID3D11Texture2D* source)
    {
        if (!health_) return;
        if (!health_->previewRequested.load(std::memory_order_relaxed))
        {
            previewTexture_.Reset(); previewStaging_.Reset(); previewTarget_.Reset();
            previewPending_ = false;
            return;
        }
        if (!source)
        {
            std::lock_guard<std::mutex> lock(health_->previewMutex);
            health_->previewPixels.clear();
            health_->previewTickMs = GetTickCount64();
            health_->previewStandby = true;
            previewPending_ = false;
            return;
        }
        if (previewFailed_) return;
        try
        {
            const uint64_t now = GetTickCount64();
            if (!previewTexture_)
            {
                const float scale = std::min(1.0f, 640.0f / std::max(kOutputWidth, kOutputHeight));
                previewWidth_ = std::max(1U, static_cast<UINT>(kOutputWidth * scale));
                previewHeight_ = std::max(1U, static_cast<UINT>(kOutputHeight * scale));
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = previewWidth_; desc.Height = previewHeight_;
                desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                CheckHr(device_->CreateTexture2D(&desc, nullptr, &previewTexture_), "CreateTexture2D(preview)");
                CheckHr(device_->CreateRenderTargetView(previewTexture_.Get(), nullptr, &previewTarget_),
                        "CreateRenderTargetView(preview)");
                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                CheckHr(device_->CreateTexture2D(&desc, nullptr, &previewStaging_), "CreateTexture2D(preview staging)");
            }
            // Read the previous thumbnail without waiting for the GPU. Never read a full-size video frame.
            if (previewPending_)
            {
                previewReadback_.resize(static_cast<size_t>(previewWidth_) * previewHeight_ * 4);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const HRESULT hr = context_->Map(previewStaging_.Get(), 0, D3D11_MAP_READ,
                                                  D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
                CheckHr(hr, "Map(preview)");
                for (UINT y = 0; y < previewHeight_; ++y)
                    memcpy(previewReadback_.data() + static_cast<size_t>(y) * previewWidth_ * 4,
                           static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                           static_cast<size_t>(previewWidth_) * 4);
                context_->Unmap(previewStaging_.Get(), 0);
                {
                    std::lock_guard<std::mutex> lock(health_->previewMutex);
                    health_->previewPixels = previewReadback_;
                    health_->previewWidth = previewWidth_; health_->previewHeight = previewHeight_;
                    health_->previewTickMs = previewSubmittedTick_; health_->previewStandby = false;
                }
                previewPending_ = false;
            }
            if (now - previewSubmittedTick_ < 200) return;
            RenderSource(source, previewTarget_.Get(), previewWidth_, previewHeight_);
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->CopyResource(previewStaging_.Get(), previewTexture_.Get());
            previewPending_ = true;
            previewSubmittedTick_ = now;
        }
        catch (const std::exception& error)
        {
            // A settings preview is optional; failure must not interrupt RTSP recording.
            previewFailed_ = true;
            Log(std::string("Settings preview unavailable: ") + error.what());
        }
    }

    RECT CaptureScreenRectangle()
    {
        return CaptureGeometry::Rectangle(settings_, desktopCoordinates_, GetForegroundWindow());
    }

    void ParsePrivacyMasks()
    {
        privacyMasks_.clear();
        for (const auto& mask : ::ParsePrivacyMasks(settings_.privacyMasks))
            privacyMasks_.push_back({ mask.x, mask.y, mask.x + mask.width, mask.y + mask.height });
    }

    void DrawPrivacyMasks()
    {
        if (privacyMasks_.empty()) return;
        d2dContext_->BeginDraw();
        ComPtr<ID2D1SolidColorBrush> brush;
        CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush),
                "CreateSolidColorBrush(privacy masks)");
        for (const RECT& mask : privacyMasks_)
        {
            const float left = static_cast<float>(std::clamp<LONG>(mask.left, 0, kOutputWidth));
            const float top = static_cast<float>(std::clamp<LONG>(mask.top, 0, kOutputHeight));
            const float right = static_cast<float>(std::clamp<LONG>(mask.right, 0, kOutputWidth));
            const float bottom = static_cast<float>(std::clamp<LONG>(mask.bottom, 0, kOutputHeight));
            d2dContext_->FillRectangle(D2D1::RectF(left, top, right, bottom), brush.Get());
        }
        CheckHr(d2dContext_->EndDraw(), "EndDraw(privacy masks)");
    }

    void DrawMouseClickHighlights()
    {
        if (!settings_.highlightMouseClicks || !health_) return;
        const uint64_t now = GetTickCount64();
        std::vector<MouseClickPulse> clicks;
        {
            std::lock_guard<std::mutex> lock(health_->mouseClicksMutex);
            auto& shared = health_->mouseClicks;
            shared.erase(std::remove_if(shared.begin(), shared.end(), [&](const MouseClickPulse& click)
            {
                return now - click.startedTickMs >= settings_.mouseClickHighlightDurationMs;
            }), shared.end());
            clicks = shared;
        }
        if (clicks.empty()) return;
        if (!clickWaveGradientStops_)
        {
            const D2D1_GRADIENT_STOP waveStops[] = {
                { 0.00f, D2D1::ColorF(1.0f, 0.72f, 0.08f, 0.00f) },
                { 0.42f, D2D1::ColorF(1.0f, 0.65f, 0.04f, 0.00f) },
                { 0.58f, D2D1::ColorF(1.0f, 0.61f, 0.02f, 0.25f) },
                { 0.72f, D2D1::ColorF(1.0f, 0.78f, 0.14f, 0.92f) },
                { 0.84f, D2D1::ColorF(1.0f, 0.47f, 0.00f, 0.30f) },
                { 1.00f, D2D1::ColorF(1.0f, 0.30f, 0.00f, 0.00f) }
            };
            CheckHr(d2dContext_->CreateGradientStopCollection(waveStops, ARRAYSIZE(waveStops),
                                                               &clickWaveGradientStops_),
                    "CreateGradientStopCollection(mouse click waves)");
            CheckHr(d2dContext_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(), 1.0f, 1.0f),
                clickWaveGradientStops_.Get(), &clickWaveGradientBrush_),
                "CreateRadialGradientBrush(mouse click waves)");

            const D2D1_GRADIENT_STOP flashStops[] = {
                { 0.00f, D2D1::ColorF(1.0f, 0.86f, 0.30f, 0.75f) },
                { 0.35f, D2D1::ColorF(1.0f, 0.68f, 0.06f, 0.38f) },
                { 0.72f, D2D1::ColorF(1.0f, 0.45f, 0.00f, 0.10f) },
                { 1.00f, D2D1::ColorF(1.0f, 0.30f, 0.00f, 0.00f) }
            };
            CheckHr(d2dContext_->CreateGradientStopCollection(flashStops, ARRAYSIZE(flashStops),
                                                               &clickFlashGradientStops_),
                    "CreateGradientStopCollection(mouse click flashes)");
            CheckHr(d2dContext_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(), 1.0f, 1.0f),
                clickFlashGradientStops_.Get(), &clickFlashGradientBrush_),
                "CreateRadialGradientBrush(mouse click flashes)");
        }
        const float scaleX = static_cast<float>(kOutputWidth) /
            static_cast<float>(std::max<LONG>(1, currentCrop_.right - currentCrop_.left));
        const float scaleY = static_cast<float>(kOutputHeight) /
            static_cast<float>(std::max<LONG>(1, currentCrop_.bottom - currentCrop_.top));
        const float duration = static_cast<float>(settings_.mouseClickHighlightDurationMs);
        constexpr float kMinimumWaveSeparationMs = 110.0f;
        constexpr int kSameClickAreaDistance = 20;
        d2dContext_->BeginDraw();
        for (size_t clickIndex = 0; clickIndex < clicks.size(); ++clickIndex)
        {
            const MouseClickPulse& click = clicks[clickIndex];
            if (click.screenX < currentCrop_.left || click.screenX >= currentCrop_.right ||
                click.screenY < currentCrop_.top || click.screenY >= currentCrop_.bottom) continue;

            // Fast clicks can arrive between two encoded frames. Give every click in the
            // same small area a distinct visual phase so double/triple clicks remain visible.
            size_t newerNearbyClicks = 0;
            for (size_t newerIndex = clickIndex + 1; newerIndex < clicks.size(); ++newerIndex)
            {
                const MouseClickPulse& newer = clicks[newerIndex];
                if (newer.startedTickMs - click.startedTickMs > 320) continue;
                if (std::abs(newer.screenX - click.screenX) <= kSameClickAreaDistance &&
                    std::abs(newer.screenY - click.screenY) <= kSameClickAreaDistance)
                    ++newerNearbyClicks;
            }

            const float realAge = static_cast<float>(now - click.startedTickMs);
            const float visualAge = std::max(realAge,
                static_cast<float>(newerNearbyClicks) * kMinimumWaveSeparationMs);
            const float phase = std::clamp(visualAge / std::max(1.0f, duration), 0.0f, 1.0f);
            const float fade = std::clamp(1.0f - realAge / std::max(1.0f, duration), 0.0f, 1.0f);
            const float fadeSmooth = fade * fade * (3.0f - 2.0f * fade);
            const float onset = std::clamp(realAge / 45.0f, 0.0f, 1.0f);
            const float waveOpacity = (0.25f + 0.75f * onset) * fadeSmooth;
            const float baseRadius = static_cast<float>(settings_.mouseClickHighlightSize) * 0.5f;
            const float waveRadius = baseRadius * (0.24f + 1.26f * phase);
            const D2D1_POINT_2F center = D2D1::Point2F(
                (click.screenX - currentCrop_.left) * scaleX,
                (click.screenY - currentCrop_.top) * scaleY);

            clickWaveGradientBrush_->SetCenter(center);
            clickWaveGradientBrush_->SetRadiusX(waveRadius);
            clickWaveGradientBrush_->SetRadiusY(waveRadius);
            clickWaveGradientBrush_->SetOpacity(waveOpacity);
            d2dContext_->FillEllipse(D2D1::Ellipse(center, waveRadius, waveRadius),
                                     clickWaveGradientBrush_.Get());

            const float flashLife = std::min(180.0f, duration * 0.35f);
            if (realAge < flashLife)
            {
                const float flashFade = 1.0f - realAge / std::max(1.0f, flashLife);
                const float flashRadius = baseRadius * (0.22f + 0.18f * onset);
                clickFlashGradientBrush_->SetCenter(center);
                clickFlashGradientBrush_->SetRadiusX(flashRadius);
                clickFlashGradientBrush_->SetRadiusY(flashRadius);
                clickFlashGradientBrush_->SetOpacity(flashFade * flashFade);
                d2dContext_->FillEllipse(D2D1::Ellipse(center, flashRadius, flashRadius),
                                         clickFlashGradientBrush_.Get());
            }
        }
        CheckHr(d2dContext_->EndDraw(), "EndDraw(mouse click highlights)");
    }

    void DrawCursor()
    {
        CURSORINFO cursor{ sizeof(cursor) };
        if (!GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) == 0 || !cursor.hCursor) return;
        if (cursor.ptScreenPos.x < currentCrop_.left || cursor.ptScreenPos.x >= currentCrop_.right ||
            cursor.ptScreenPos.y < currentCrop_.top || cursor.ptScreenPos.y >= currentCrop_.bottom) return;
        const double scaleX = static_cast<double>(kOutputWidth) / (currentCrop_.right - currentCrop_.left);
        const double scaleY = static_cast<double>(kOutputHeight) / (currentCrop_.bottom - currentCrop_.top);
        const int x = static_cast<int>((cursor.ptScreenPos.x - currentCrop_.left) * scaleX);
        const int y = static_cast<int>((cursor.ptScreenPos.y - currentCrop_.top) * scaleY);
        ComPtr<IDXGISurface1> surface;
        if (FAILED(bgraTexture_.As(&surface))) return;
        HDC dc = nullptr;
        if (FAILED(surface->GetDC(FALSE, &dc))) return;
        ICONINFO info{};
        int hotspotX = 0, hotspotY = 0;
        if (GetIconInfo(cursor.hCursor, &info))
        {
            hotspotX = static_cast<int>(info.xHotspot);
            hotspotY = static_cast<int>(info.yHotspot);
            if (info.hbmColor) DeleteObject(info.hbmColor);
            if (info.hbmMask) DeleteObject(info.hbmMask);
        }
        DrawIconEx(dc, x - hotspotX, y - hotspotY, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        surface->ReleaseDC(nullptr);
    }

    ComPtr<ID3D11VideoProcessorOutputView> GetNv12OutputView(ID3D11Texture2D* texture, UINT subresource)
    {
        for (const auto& entry : nv12OutputViews_)
        {
            if (entry.texture.Get() == texture && entry.subresource == subresource)
                return entry.view;
        }

        OutputViewEntry entry;
        entry.texture = texture;
        entry.subresource = subresource;
        entry.view = CreateOutputView(texture, convertEnumerator_.Get(), subresource);
        nv12OutputViews_.push_back(entry);
        return entry.view;
    }

    struct OutputViewEntry
    {
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    ComPtr<ID3D11VideoProcessorEnumerator> CreateEnumerator(UINT inputWidth, UINT inputHeight,
                                                             UINT outputWidth, UINT outputHeight)
    {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = { kFrameRate, 1 };
        desc.InputWidth = inputWidth;
        desc.InputHeight = inputHeight;
        desc.OutputFrameRate = { kFrameRate, 1 };
        desc.OutputWidth = outputWidth;
        desc.OutputHeight = outputHeight;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        CheckHr(videoDevice_->CreateVideoProcessorEnumerator(&desc, &enumerator), "CreateVideoProcessorEnumerator");
        return enumerator;
    }

    static void CheckFormat(ID3D11VideoProcessorEnumerator* enumerator, DXGI_FORMAT format, UINT required)
    {
        UINT flags = 0;
        CheckHr(enumerator->CheckVideoProcessorFormat(format, &flags), "CheckVideoProcessorFormat");
        if ((flags & required) == 0)
            throw std::runtime_error(std::string("GPU video processor does not support format ") + DxgiFormatName(format));
    }

    ComPtr<ID3D11VideoProcessorInputView> CreateInputView(ID3D11Texture2D* texture,
                                                          ID3D11VideoProcessorEnumerator* enumerator,
                                                          UINT subresource)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = subresource % textureDesc.MipLevels;
        desc.Texture2D.ArraySlice = subresource / textureDesc.MipLevels;
        ComPtr<ID3D11VideoProcessorInputView> view;
        CheckHr(videoDevice_->CreateVideoProcessorInputView(texture, enumerator, &desc, &view),
                "CreateVideoProcessorInputView");
        return view;
    }

    ComPtr<ID3D11VideoProcessorOutputView> CreateOutputView(ID3D11Texture2D* texture,
                                                            ID3D11VideoProcessorEnumerator* enumerator,
                                                            UINT subresource)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = subresource % textureDesc.MipLevels;
        ComPtr<ID3D11VideoProcessorOutputView> view;
        CheckHr(videoDevice_->CreateVideoProcessorOutputView(texture, enumerator, &desc, &view),
                "CreateVideoProcessorOutputView");
        return view;
    }

    void InitializeDirect2D()
    {
        D2D1_FACTORY_OPTIONS options{};
        CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                  reinterpret_cast<void**>(d2dFactory_.GetAddressOf())), "D2D1CreateFactory");
        ComPtr<IDXGIDevice> dxgiDevice;
        CheckHr(device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "QueryInterface(IDXGIDevice)");
        CheckHr(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_), "ID2D1Factory1::CreateDevice");
        CheckHr(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_),
                "ID2D1Device::CreateDeviceContext");
        ComPtr<IDXGISurface> surface;
        CheckHr(bgraTexture_.As(&surface), "QueryInterface(IDXGISurface)");
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        CheckHr(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &d2dTarget_),
                "CreateBitmapFromDxgiSurface");
        d2dContext_->SetTarget(d2dTarget_.Get());
        CheckHr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())),
                "DWriteCreateFactory");
        UpdateTextFormat();
        CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush_),
                "CreateSolidColorBrush(text)");
        CheckHr(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f,
            settings_.timestampBackgroundOpacity / 100.0f), &backgroundBrush_),
                "CreateSolidColorBrush(background)");
    }

    void UpdateTextFormat()
    {
        textFormat_.Reset();
        CheckHr(writeFactory_->CreateTextFormat(settings_.timestampFontName.c_str(), nullptr,
                                                static_cast<DWRITE_FONT_WEIGHT>(settings_.timestampFontWeight),
                                                settings_.timestampFontItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL,
                                                static_cast<float>(settings_.timestampFontSize),
                                                IsEnglishUi() ? L"en-US" : L"ru-RU", &textFormat_),
                "CreateTextFormat");
        CheckHr(textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "SetWordWrapping");
    }

    void DrawTimestamp()
    {
        const std::wstring timestamp = BuildOverlayText(settings_);
        if (timestamp.empty()) return;
        if (timestamp != cachedTimestamp_ || !timestampLayout_)
        {
            cachedTimestamp_ = timestamp;
            const float maximumWidth = std::max(1.0f, static_cast<float>(kOutputWidth) -
                2.0f * static_cast<float>(settings_.timestampMarginX + settings_.timestampPaddingX));
            const float maximumHeight = std::max(1.0f, static_cast<float>(kOutputHeight) -
                2.0f * static_cast<float>(settings_.timestampMarginY + settings_.timestampPaddingY));
            CheckHr(writeFactory_->CreateTextLayout(timestamp.c_str(), static_cast<UINT32>(timestamp.size()),
                                                    textFormat_.Get(), maximumWidth, maximumHeight,
                                                    &timestampLayout_), "CreateTextLayout(timestamp)");
            CheckHr(timestampLayout_->GetMetrics(&timestampMetrics_), "GetMetrics(timestamp)");
        }
        const float paddingX = static_cast<float>(settings_.timestampPaddingX);
        const float paddingY = static_cast<float>(settings_.timestampPaddingY);
        const float blockWidth = timestampMetrics_.widthIncludingTrailingWhitespace + 2.0f * paddingX;
        const float blockHeight = timestampMetrics_.height + 2.0f * paddingY;
        const bool alignRight = settings_.timestampPosition == 1 || settings_.timestampPosition == 3;
        const bool alignBottom = settings_.timestampPosition >= 2;
        const float left = alignRight
            ? std::max(0.0f, static_cast<float>(kOutputWidth) - static_cast<float>(settings_.timestampMarginX) - blockWidth)
            : static_cast<float>(settings_.timestampMarginX);
        const float top = alignBottom
            ? std::max(0.0f, static_cast<float>(kOutputHeight) - static_cast<float>(settings_.timestampMarginY) - blockHeight)
            : static_cast<float>(settings_.timestampMarginY);
        d2dContext_->BeginDraw();
        d2dContext_->FillRectangle(D2D1::RectF(left, top, left + blockWidth, top + blockHeight),
                                   backgroundBrush_.Get());
        d2dContext_->DrawTextLayout(D2D1::Point2F(left + paddingX, top + paddingY),
                                    timestampLayout_.Get(), textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        CheckHr(d2dContext_->EndDraw(), "ID2D1DeviceContext::EndDraw");
    }

    D3D11_TEXTURE2D_DESC sourceDesc_{};
    DXGI_MODE_ROTATION sourceRotation_ = DXGI_MODE_ROTATION_IDENTITY;
    AppSettings settings_;
    TrayRuntimeStatus* health_ = nullptr;
    RECT desktopCoordinates_{};
    RECT currentCrop_{};
    std::vector<RECT> privacyMasks_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoContext1> videoContext1_;
    ComPtr<ID3D11VideoProcessorEnumerator> convertEnumerator_;
    ComPtr<ID3D11VideoProcessor> convertProcessor_;
    ComPtr<ID3D11Texture2D> bgraTexture_;
    ComPtr<ID3D11RenderTargetView> bgraRenderTarget_;
    ComPtr<ID3D11Texture2D> previewTexture_, previewStaging_;
    ComPtr<ID3D11RenderTargetView> previewTarget_;
    UINT previewWidth_ = 0, previewHeight_ = 0;
    uint64_t previewSubmittedTick_ = 0;
    bool previewPending_ = false, previewFailed_ = false;
    std::vector<uint8_t> previewReadback_;
    ComPtr<ID3D11VideoProcessorInputView> bgraInputView_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11ShaderResourceView> sourceShaderView_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> conversionSettings_;
    std::vector<OutputViewEntry> nv12OutputViews_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<ID2D1Bitmap1> d2dTarget_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextLayout> timestampLayout_;
    DWRITE_TEXT_METRICS timestampMetrics_{};
    std::wstring cachedTimestamp_;
    ComPtr<ID2D1SolidColorBrush> textBrush_, backgroundBrush_;
    ComPtr<ID2D1GradientStopCollection> clickWaveGradientStops_, clickFlashGradientStops_;
    ComPtr<ID2D1RadialGradientBrush> clickWaveGradientBrush_, clickFlashGradientBrush_;
};

class H264Encoder
{
public:
    using OutputCallback = void (*)(void*, const uint8_t*, size_t, LONGLONG, bool);

    void Initialize(ID3D11Device* device, OutputCallback callback, void* callbackContext,
                    const AppSettings& settings, TrayRuntimeStatus* health = nullptr)
    {
        kOutputWidth = settings.outputWidth; kOutputHeight = settings.outputHeight;
        kFrameRate = settings.frameRate; kBitrate = settings.bitrateKbps * 1000U; kGopSize = settings.gopSize;
        kFrameDuration = 10'000'000LL / kFrameRate;
        lastOutputTime_ = -kFrameDuration;
        callback_ = callback;
        callbackContext_ = callbackContext;
        device_ = device;
        device->GetImmediateContext(&context_);
        ComPtr<ID3D10Multithread> multithread;
        CheckHr(device->QueryInterface(IID_PPV_ARGS(&multithread)), "QueryInterface(ID3D10Multithread)");
        multithread->SetMultithreadProtected(TRUE);
        CheckHr(MFCreateDXGIDeviceManager(&deviceResetToken_, &deviceManager_), "MFCreateDXGIDeviceManager");
        CheckHr(deviceManager_->ResetDevice(device, deviceResetToken_), "IMFDXGIDeviceManager::ResetDevice");
        MFT_REGISTER_TYPE_INFO inputInfo{ MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, MFVideoFormat_H264 };
        ComPtr<IDXGIDevice> dxgiDevice;
        CheckHr(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "QueryInterface(IDXGIDevice encoder)");
        ComPtr<IDXGIAdapter> encoderAdapter;
        CheckHr(dxgiDevice->GetAdapter(&encoderAdapter), "IDXGIDevice::GetAdapter encoder");
        DXGI_ADAPTER_DESC encoderAdapterDesc{};
        CheckHr(encoderAdapter->GetDesc(&encoderAdapterDesc), "IDXGIAdapter::GetDesc encoder");
        ComPtr<IMFAttributes> hardwareEnumerationAttributes;
        CheckHr(MFCreateAttributes(&hardwareEnumerationAttributes, 1),
                "MFCreateAttributes(H.264 adapter enumeration)");
        CheckHr(hardwareEnumerationAttributes->SetBlob(
                    MFT_ENUM_ADAPTER_LUID,
                    reinterpret_cast<const UINT8*>(&encoderAdapterDesc.AdapterLuid),
                    sizeof(encoderAdapterDesc.AdapterLuid)),
                "Set MFT_ENUM_ADAPTER_LUID");
        ComPtr<IMFActivate> selected;
        auto enumerate = [&](UINT32 flags, bool hardware) -> bool
        {
            IMFActivate** activations = nullptr;
            UINT32 activationCount = 0;
            const HRESULT enumerationResult = hardware
                ? MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER,
                           &inputInfo, &outputInfo, hardwareEnumerationAttributes.Get(),
                           &activations, &activationCount)
                : MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER,
                            &inputInfo, &outputInfo, &activations, &activationCount);
            CheckHr(enumerationResult,
                    hardware ? "MFTEnum2(H.264 encoder for capture adapter)"
                             : "MFTEnumEx(H.264 software encoder)");
            if (activationCount == 0) { CoTaskMemFree(activations); return false; }
            selected.Attach(activations[0]);
            for (UINT32 index = 0; index < activationCount; ++index)
                if (index != 0) activations[index]->Release();
            CoTaskMemFree(activations);
            hardware_ = hardware;
            return true;
        };
        const bool softwarePreferred = settings.encoderPreference == 2;
        if (softwarePreferred)
            enumerate(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT, false);
        if (!selected)
            enumerate(MFT_ENUM_FLAG_HARDWARE, true);
        if (!selected && settings.allowSoftwareEncoder && settings.encoderPreference != 1)
            enumerate(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT, false);
        if (!selected)
            throw std::runtime_error("No compatible H.264 Media Foundation encoder was found");
        wchar_t* friendlyName = nullptr;
        UINT32 friendlyNameLength = 0;
        if (SUCCEEDED(selected->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                   &friendlyName, &friendlyNameLength)) &&
            friendlyName != nullptr)
        {
            encoderName_.assign(friendlyName, friendlyNameLength);
            CoTaskMemFree(friendlyName);
        }
        Log(std::string(hardware_ ? "Hardware" : "Software") + " H.264 encoder: " + WideToUtf8String(encoderName_));
        if (health)
        {
            std::lock_guard<std::mutex> lock(health->mutex);
            health->encoderName = encoderName_;
            health->encoderKind = hardware_ ? L"Hardware" : L"Software";
        }
        CheckHr(selected->ActivateObject(IID_PPV_ARGS(&transform_)), "ActivateObject(H.264 encoder)");
        ComPtr<IMFAttributes> transformAttributes;
        if (SUCCEEDED(transform_->GetAttributes(&transformAttributes)))
        {
            UINT32 isAsync = FALSE;
            transformAttributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            asynchronous_ = isAsync != FALSE;
            if (asynchronous_)
                CheckHr(transformAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "Set MF_TRANSFORM_ASYNC_UNLOCK");
        }
        if (hardware_)
            CheckHr(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                               reinterpret_cast<ULONG_PTR>(deviceManager_.Get())),
                    "MFT_MESSAGE_SET_D3D_MANAGER");
        outputType_ = CreateVideoType(MFVideoFormat_H264);
        CheckHr(outputType_->SetUINT32(MF_MT_AVG_BITRATE, kBitrate), "Set H.264 bitrate");
        CheckHr(outputType_->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base), "Set H.264 profile");
        const UINT h264Level = SelectH264Level(kOutputWidth, kOutputHeight, kFrameRate, kBitrate / 1000);
        CheckHr(outputType_->SetUINT32(MF_MT_MPEG2_LEVEL, h264Level), "Set H.264 level");
        Log("Requested H.264 profile Baseline, level " + std::to_string(h264Level / 10) + "." +
            std::to_string(h264Level % 10));
        CheckHr(transform_->SetOutputType(0, outputType_.Get(), 0), "SetOutputType(H.264)");
        inputType_ = CreateVideoType(MFVideoFormat_NV12);
        CheckHr(inputType_->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE), "Set fixed-size samples");
        CheckHr(inputType_->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), "Set independent samples");
        CheckHr(transform_->SetInputType(0, inputType_.Get(), 0), "SetInputType(NV12)");
        ReadSequenceHeader();
        ConfigureCodecApi();
        if (hardware_) InitializeAllocator();
        else InitializeSoftwareInput();
        if (asynchronous_) CheckHr(transform_.As(&eventGenerator_), "QueryInterface(IMFMediaEventGenerator)");
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING");
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT_MESSAGE_NOTIFY_START_OF_STREAM");
    }

    ComPtr<IMFSample> AllocateInput(ID3D11Texture2D** texture, UINT* subresource)
    {
        if (!hardware_)
        {
            ComPtr<IMFSample> sample;
            CheckHr(MFCreateSample(&sample), "MFCreateSample(software input)");
            *texture = softwareInput_.Get();
            (*texture)->AddRef();
            *subresource = 0;
            return sample;
        }
        ComPtr<IMFSample> sample;
        CheckHr(allocator_->AllocateSample(&sample), "IMFVideoSampleAllocator::AllocateSample");
        ComPtr<IMFMediaBuffer> buffer;
        CheckHr(sample->GetBufferByIndex(0, &buffer), "IMFSample::GetBufferByIndex");
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        CheckHr(buffer.As(&dxgiBuffer), "QueryInterface(IMFDXGIBuffer)");
        CheckHr(dxgiBuffer->GetResource(IID_PPV_ARGS(texture)), "IMFDXGIBuffer::GetResource");
        CheckHr(dxgiBuffer->GetSubresourceIndex(subresource), "IMFDXGIBuffer::GetSubresourceIndex");
        return sample;
    }

    void Submit(IMFSample* sample, LONGLONG sampleTime)
    {
        ComPtr<IMFSample> softwareSample;
        if (!hardware_)
        {
            context_->CopyResource(softwareStaging_.Get(), softwareInput_.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CheckHr(context_->Map(softwareStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                    "Map(software NV12 input)");
            ComPtr<IMFMediaBuffer> buffer;
            const DWORD frameSize = kOutputWidth * kOutputHeight * 3 / 2;
            CheckHr(MFCreateMemoryBuffer(frameSize, &buffer), "MFCreateMemoryBuffer(software NV12)");
            BYTE* destination = nullptr;
            CheckHr(buffer->Lock(&destination, nullptr, nullptr), "Lock(software NV12)");
            for (UINT row = 0; row < kOutputHeight; ++row)
                memcpy(destination + row * kOutputWidth,
                       static_cast<const BYTE*>(mapped.pData) + row * mapped.RowPitch, kOutputWidth);
            BYTE* uvDestination = destination + kOutputWidth * kOutputHeight;
            const BYTE* uvSource = static_cast<const BYTE*>(mapped.pData) + mapped.RowPitch * kOutputHeight;
            for (UINT row = 0; row < kOutputHeight / 2; ++row)
                memcpy(uvDestination + row * kOutputWidth, uvSource + row * mapped.RowPitch, kOutputWidth);
            buffer->Unlock();
            context_->Unmap(softwareStaging_.Get(), 0);
            CheckHr(buffer->SetCurrentLength(frameSize), "SetCurrentLength(software NV12)");
            CheckHr(MFCreateSample(&softwareSample), "MFCreateSample(software NV12)");
            CheckHr(softwareSample->AddBuffer(buffer.Get()), "AddBuffer(software NV12)");
            sample = softwareSample.Get();
        }
        CheckHr(sample->SetSampleTime(sampleTime), "IMFSample::SetSampleTime");
        CheckHr(sample->SetSampleDuration(kFrameDuration), "IMFSample::SetSampleDuration");
        if (asynchronous_)
        {
            WaitForInputRequest();
            CheckHr(transform_->ProcessInput(0, sample, 0), "IMFTransform::ProcessInput(async)");
            PumpAvailableEvents();
        }
        else
        {
            HRESULT hr = transform_->ProcessInput(0, sample, 0);
            if (hr == MF_E_NOTACCEPTING) { DrainSynchronousOutputs(); hr = transform_->ProcessInput(0, sample, 0); }
            CheckHr(hr, "IMFTransform::ProcessInput");
            DrainSynchronousOutputs();
        }
    }

    bool UpdateBitrate(uint32_t bitrate)
    {
        if (!codec_) return false;
        VARIANT setting; VariantInit(&setting); setting.vt = VT_UI4; setting.ulVal = bitrate;
        const HRESULT meanResult = codec_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &setting);
        const HRESULT bitrateResult = codec_->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &setting);
        if (FAILED(meanResult) && FAILED(bitrateResult)) return false;
        Log("Encoder bitrate changed to " + std::to_string(bitrate / 1000) + " kbps");
        return true;
    }

    void Finalize()
    {
        if (!transform_) return;
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0),
                "MFT_MESSAGE_NOTIFY_END_OF_STREAM");
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                "MFT_MESSAGE_COMMAND_DRAIN");

        if (asynchronous_)
        {
            while (true)
            {
                ComPtr<IMFMediaEvent> event;
                CheckHr(eventGenerator_->GetEvent(0, &event), "GetEvent(encoder drain)");
                HRESULT status = S_OK;
                CheckHr(event->GetStatus(&status), "GetStatus(encoder drain)");
                CheckHr(status, "Asynchronous encoder drain event");
                MediaEventType type = MEUnknown;
                CheckHr(event->GetType(&type), "GetType(encoder drain)");
                if (type == METransformHaveOutput) ProduceOutput();
                else if (type == METransformDrainComplete) break;
                else if (type == MEError) throw std::runtime_error("Encoder drain reported MEError");
            }
        }
        else
        {
            DrainSynchronousOutputs();
        }
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0),
                "MFT_MESSAGE_NOTIFY_END_STREAMING");
    }

private:
#ifdef SCREEN2NVR_ENCODER_TEST
    friend struct H264EncoderTestAccess;
#endif

    void ReadSequenceHeader()
    {
        pendingSequenceHeader_.clear();
        ComPtr<IMFMediaType> current;
        if (FAILED(transform_->GetOutputCurrentType(0, &current))) current = outputType_;
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size != 0)
        {
            if (size > 64 * 1024) throw std::runtime_error("Encoder sequence header exceeds 64 KiB");
            pendingSequenceHeader_.resize(size);
            CheckHr(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, pendingSequenceHeader_.data(), size, nullptr),
                    "Get H.264 sequence header");
        }
    }

    void RenegotiateOutputType()
    {
        // Some hardware drivers refine the H.264 type (including SPS/PPS) on first output.
        // Keep the configured geometry: capture/ONVIF must not silently change resolution.
        if (++consecutiveStreamChanges_ > 8)
            throw std::runtime_error("H.264 encoder repeatedly changes output format without producing video");
        HRESULT lastResult = MF_E_INVALIDMEDIATYPE;
        auto accept = [&](IMFMediaType* offered) -> bool
        {
            ComPtr<IMFMediaType> candidate;
            CheckHr(MFCreateMediaType(&candidate), "Create updated H.264 type");
            CheckHr(outputType_->CopyAllItems(candidate.Get()), "Copy H.264 output attributes");
            candidate->DeleteItem(MF_MT_MPEG_SEQUENCE_HEADER);
            // CopyAllItems clears its destination, so overlay offered attributes individually
            // to preserve size/FPS when a driver enumerates a partial media type.
            UINT32 attributeCount = 0;
            CheckHr(offered->GetCount(&attributeCount), "Count updated encoder attributes");
            for (UINT32 attributeIndex = 0; attributeIndex < attributeCount; ++attributeIndex)
            {
                GUID key{};
                PROPVARIANT value{};
                HRESULT hr = offered->GetItemByIndex(attributeIndex, &key, &value);
                if (SUCCEEDED(hr)) hr = candidate->SetItem(key, value);
                PropVariantClear(&value);
                CheckHr(hr, "Copy updated encoder attribute");
            }
            GUID major{}, subtype{};
            UINT32 width = 0, height = 0;
            if (FAILED(candidate->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
                FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_H264 ||
                FAILED(MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
                width != kOutputWidth || height != kOutputHeight)
                return false;
            lastResult = transform_->SetOutputType(0, candidate.Get(), 0);
            if (FAILED(lastResult)) return false;
            outputType_ = candidate;
            reusableOutputSample_.Reset();
            reusableOutputBuffer_.Reset();
            ReadSequenceHeader();
            MFT_OUTPUT_STREAM_INFO info{};
            CheckHr(transform_->GetOutputStreamInfo(0, &info), "Get updated encoder output buffer requirements");
            Log("H.264 output format renegotiated: " + std::to_string(width) + "x" +
                std::to_string(height) + ", buffer=" + std::to_string(info.cbSize) +
                ", alignment=" + std::to_string(info.cbAlignment) +
                ", flags=" + std::to_string(info.dwFlags));
            return true;
        };
        for (DWORD index = 0; index < 64; ++index)
        {
            ComPtr<IMFMediaType> offered;
            const HRESULT hr = transform_->GetOutputAvailableType(0, index, &offered);
            if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL) break;
            CheckHr(hr, "Get updated H.264 output type");
            if (accept(offered.Get())) return;
        }
        // Drivers that only update the current type may not enumerate it separately.
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &current)) && accept(current.Get())) return;
        CheckHr(lastResult, "Renegotiate H.264 output format at configured resolution");
    }

    ComPtr<IMFMediaType> CreateVideoType(const GUID& subtype)
    {
        ComPtr<IMFMediaType> type;
        CheckHr(MFCreateMediaType(&type), "MFCreateMediaType");
        CheckHr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set MF_MT_MAJOR_TYPE");
        CheckHr(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set MF_MT_SUBTYPE");
        CheckHr(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kOutputWidth, kOutputHeight), "Set frame size");
        CheckHr(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFrameRate, 1), "Set frame rate");
        CheckHr(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set pixel aspect ratio");
        CheckHr(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set interlace mode");
        return type;
    }

    static void SetCodecUInt(ICodecAPI* codec, const GUID& key, ULONG value, const char* name)
    {
        VARIANT setting; VariantInit(&setting); setting.vt = VT_UI4; setting.ulVal = value;
        const HRESULT hr = codec->SetValue(&key, &setting);
        if (FAILED(hr)) Log(std::string("Encoder option not accepted (") + name + "): " + HresultText(hr));
    }

    void ConfigureCodecApi()
    {
        if (FAILED(transform_.As(&codec_))) { Log("Encoder does not expose ICodecAPI"); return; }
        SetCodecUInt(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "CBR");
        SetCodecUInt(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, kBitrate, "bitrate");
        SetCodecUInt(codec_.Get(), CODECAPI_AVEncMPVGOPSize, kGopSize, "GOP");
        VARIANT lowLatency; VariantInit(&lowLatency); lowLatency.vt = VT_BOOL; lowLatency.boolVal = VARIANT_TRUE;
        const HRESULT hr = codec_->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
        if (FAILED(hr)) Log("Encoder low-latency option not accepted: " + HresultText(hr));
    }

    void InitializeAllocator()
    {
        CheckHr(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)), "MFCreateVideoSampleAllocatorEx");
        CheckHr(allocator_->SetDirectXManager(deviceManager_.Get()), "SetDirectXManager");
        ComPtr<IMFAttributes> attributes;
        CheckHr(MFCreateAttributes(&attributes, 4), "MFCreateAttributes(sample allocator)");
        CheckHr(attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT), "Set MF_SA_D3D11_USAGE");
        CheckHr(attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET), "Set MF_SA_D3D11_BINDFLAGS");
        CheckHr(allocator_->InitializeSampleAllocatorEx(4, 8, attributes.Get(), inputType_.Get()),
                "InitializeSampleAllocatorEx");
    }

    void InitializeSoftwareInput()
    {
        D3D11_TEXTURE2D_DESC input{};
        input.Width = kOutputWidth;
        input.Height = kOutputHeight;
        input.MipLevels = 1;
        input.ArraySize = 1;
        input.Format = DXGI_FORMAT_NV12;
        input.SampleDesc.Count = 1;
        input.Usage = D3D11_USAGE_DEFAULT;
        input.BindFlags = D3D11_BIND_RENDER_TARGET;
        CheckHr(device_->CreateTexture2D(&input, nullptr, &softwareInput_),
                "CreateTexture2D(software NV12 input)");
        input.Usage = D3D11_USAGE_STAGING;
        input.BindFlags = 0;
        input.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHr(device_->CreateTexture2D(&input, nullptr, &softwareStaging_),
                "CreateTexture2D(software NV12 staging)");
    }

    void WaitForInputRequest()
    {
        if (pendingInputRequests_ > 0) { --pendingInputRequests_; return; }
        while (true)
        {
            ComPtr<IMFMediaEvent> event;
            CheckHr(eventGenerator_->GetEvent(0, &event), "IMFMediaEventGenerator::GetEvent");
            HRESULT status = S_OK;
            CheckHr(event->GetStatus(&status), "IMFMediaEvent::GetStatus");
            CheckHr(status, "Asynchronous encoder event");
            MediaEventType type = MEUnknown;
            CheckHr(event->GetType(&type), "IMFMediaEvent::GetType");
            if (type == METransformNeedInput) return;
            if (type == METransformHaveOutput) ProduceOutput();
            else if (type == MEError) throw std::runtime_error("Media Foundation encoder reported MEError");
        }
    }

    void PumpAvailableEvents()
    {
        while (true)
        {
            ComPtr<IMFMediaEvent> event;
            const HRESULT hr = eventGenerator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) return;
            CheckHr(hr, "GetEvent(nonblocking)");
            HRESULT status = S_OK;
            CheckHr(event->GetStatus(&status), "IMFMediaEvent::GetStatus");
            CheckHr(status, "Asynchronous encoder event");
            MediaEventType type = MEUnknown;
            CheckHr(event->GetType(&type), "IMFMediaEvent::GetType");
            if (type == METransformHaveOutput) ProduceOutput();
            else if (type == METransformNeedInput) ++pendingInputRequests_;
            else if (type == MEError) throw std::runtime_error("Media Foundation encoder reported MEError");
        }
    }

    void DrainSynchronousOutputs() { while (ProduceOutput() == S_OK) {} }

    HRESULT ProduceOutput()
    {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        CheckHr(transform_->GetOutputStreamInfo(0, &streamInfo), "GetOutputStreamInfo");
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        if ((streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
        {
            const DWORD capacity = std::max<DWORD>(streamInfo.cbSize, 2'000'000);
            if (!reusableOutputSample_ || capacity > outputBufferCapacity_ ||
                streamInfo.cbAlignment != outputBufferAlignment_)
            {
                reusableOutputSample_.Reset();
                reusableOutputBuffer_.Reset();
                CheckHr(MFCreateSample(&reusableOutputSample_), "MFCreateSample(output)");
                CheckHr(MFCreateAlignedMemoryBuffer(capacity,
                            streamInfo.cbAlignment > 0 ? streamInfo.cbAlignment - 1 : 0,
                            &reusableOutputBuffer_), "MFCreateAlignedMemoryBuffer(output)");
                CheckHr(reusableOutputSample_->AddBuffer(reusableOutputBuffer_.Get()), "AddBuffer(output)");
                outputBufferCapacity_ = capacity;
                outputBufferAlignment_ = streamInfo.cbAlignment;
            }
            CheckHr(reusableOutputSample_->DeleteAllItems(), "Clear output sample attributes");
            CheckHr(reusableOutputBuffer_->SetCurrentLength(0), "SetCurrentLength(output)");
            output.pSample = reusableOutputSample_.Get();
        }
        IMFSample* const suppliedSample = output.pSample;
        DWORD status = 0;
        const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
        ComPtr<IMFCollection> events;
        events.Attach(output.pEvents);
        ComPtr<IMFSample> allocatedSample;
        // A driver-allocated sample belongs to the caller, including on error paths.
        if (output.pSample != suppliedSample) allocatedSample.Attach(output.pSample);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            RenegotiateOutputType();
            // Async MFTs require the next HaveOutput event; do not call ProcessOutput twice
            // for one event. The synchronous drain loop can retry immediately.
            return S_OK;
        }
        CheckHr(hr, "IMFTransform::ProcessOutput");
        IMFSample* encodedSample = output.pSample;
        if (!encodedSample) return S_OK;
        LONGLONG sampleTime = lastOutputTime_ + kFrameDuration;
        encodedSample->GetSampleTime(&sampleTime);
        lastOutputTime_ = sampleTime;
        ComPtr<IMFMediaBuffer> contiguous;
        CheckHr(encodedSample->ConvertToContiguousBuffer(&contiguous), "ConvertToContiguousBuffer");
        BYTE* bytes = nullptr;
        DWORD currentLength = 0;
        CheckHr(contiguous->Lock(&bytes, nullptr, &currentLength), "IMFMediaBuffer::Lock");
        try
        {
            if (currentLength != 0)
            {
                consecutiveStreamChanges_ = 0;
                if (!pendingSequenceHeader_.empty())
                {
                    callback_(callbackContext_, pendingSequenceHeader_.data(), pendingSequenceHeader_.size(),
                              sampleTime, true);
                    pendingSequenceHeader_.clear();
                }
                callback_(callbackContext_, bytes, currentLength, sampleTime, false);
            }
        }
        catch (...) { contiguous->Unlock(); throw; }
        CheckHr(contiguous->Unlock(), "IMFMediaBuffer::Unlock");
        return S_OK;
    }

    UINT kOutputWidth = 1920, kOutputHeight = 1080, kFrameRate = 12, kBitrate = 2'000'000, kGopSize = 25;
    LONGLONG kFrameDuration = 10'000'000LL / 12;
    bool asynchronous_ = false;
    bool hardware_ = true;
    UINT32 deviceResetToken_ = 0;
    unsigned int pendingInputRequests_ = 0;
    unsigned int consecutiveStreamChanges_ = 0;
    DWORD outputBufferCapacity_ = 0, outputBufferAlignment_ = 0;
    std::vector<uint8_t> pendingSequenceHeader_;
    LONGLONG lastOutputTime_ = -kFrameDuration;
    std::wstring encoderName_;
    OutputCallback callback_ = nullptr;
    void* callbackContext_ = nullptr;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> softwareInput_, softwareStaging_;
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFMediaEventGenerator> eventGenerator_;
    ComPtr<IMFMediaType> inputType_, outputType_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IMFSample> reusableOutputSample_;
    ComPtr<IMFMediaBuffer> reusableOutputBuffer_;
};

void InitializeH264Encoder(H264Encoder& encoder, ID3D11Device* device,
                           H264Encoder::OutputCallback callback, void* callbackContext,
                           const AppSettings& settings, TrayRuntimeStatus* health = nullptr)
{
    try
    {
        encoder.Initialize(device, callback, callbackContext, settings, health);
    }
    catch (const HrError& error)
    {
        if (settings.encoderPreference != 0 || !settings.allowSoftwareEncoder) throw;
        Log("Hardware H.264 initialization failed (" + std::string(error.what()) + ", " +
            HresultText(error.hr) + "); trying the software encoder.");
        encoder = H264Encoder{};
        AppSettings softwareSettings = settings;
        softwareSettings.encoderPreference = 2;
        encoder.Initialize(device, callback, callbackContext, softwareSettings, health);
    }
    catch (const std::exception& error)
    {
        if (settings.encoderPreference != 0 || !settings.allowSoftwareEncoder) throw;
        Log("Hardware H.264 initialization failed (" + std::string(error.what()) +
            "); trying the software encoder.");
        encoder = H264Encoder{};
        AppSettings softwareSettings = settings;
        softwareSettings.encoderPreference = 2;
        encoder.Initialize(device, callback, callbackContext, softwareSettings, health);
    }
}

struct EncodedFrameContext
{
    RtspServer* server = nullptr;
    TrayRuntimeStatus* health = nullptr;
    unsigned streamIndex = 0;
};

class RtspProbeMonitor
{
public:
    RtspProbeMonitor(RtspServer& server, TrayRuntimeStatus& health) : server_(server), health_(health)
    {
        thread_ = std::thread([this]()
        {
            while (running_ && g_running)
            {
                if (!health_.streamingPaused.load() && server_.Probe())
                {
                    const bool firstSuccess = health_.rtspProbeHeartbeatMs.load() == 0;
                    health_.rtspProbeHeartbeatMs = GetTickCount64();
                    if (firstSuccess && health_.logger)
                        health_.logger("RTSP self-check received an H.264 RTP packet successfully.");
                }
                if (!health_.streamingPaused.load() && health_.subStreamEnabled && server_.Probe(1))
                    health_.subRtspProbeHeartbeatMs = GetTickCount64();
                for (int index = 0; index < 100 && running_ && g_running; ++index)
                {
                    if (index % 5 == 0)
                    {
                        const auto clients = server_.ClientStatistics();
                        std::lock_guard<std::mutex> lock(health_.mutex);
                        health_.connectedDevices = clients.devices;
                        health_.mainRtspSessions = clients.mainSessions;
                        health_.subRtspSessions = clients.subSessions;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    }

    ~RtspProbeMonitor()
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    RtspServer& server_;
    TrayRuntimeStatus& health_;
    std::atomic_bool running_{ true };
    std::thread thread_;
};

void EncodedFrameCallback(void* context, const uint8_t* data, size_t size, LONGLONG sampleTime,
                          bool codecConfiguration)
{
    auto& encoded = *static_cast<EncodedFrameContext*>(context);
    encoded.server->PublishAccessUnit(data, size, sampleTime, encoded.streamIndex);
    if (encoded.health && encoded.streamIndex != 0)
    {
        if (!codecConfiguration)
        {
            encoded.health->subPublishedFrames.fetch_add(1, std::memory_order_relaxed);
            encoded.health->subPublishedHeartbeatMs = GetTickCount64();
        }
        return;
    }
    if (encoded.health && !codecConfiguration)
    {
        encoded.health->publishedBytes.fetch_add(size, std::memory_order_relaxed);
        encoded.health->publishedFrames.fetch_add(1, std::memory_order_relaxed);
        encoded.health->publishedHeartbeatMs.store(GetTickCount64(), std::memory_order_release);
        encoded.health->phase.store(static_cast<uint32_t>(PipelinePhase::streaming), std::memory_order_release);
    }
}

class SecondaryStream
{
public:
    SecondaryStream(ID3D11Device* device, ID3D11DeviceContext* context, GpuProcessor& mainProcessor,
                    RtspServer& server, const AppSettings& settings, TrayRuntimeStatus* health)
        : settings_(MakeSubStreamSettings(settings)), device_(device), context_(context), encoded_{ &server, health, 1 }
    {
        Log("Initializing secondary stream: " + std::to_string(settings_.outputWidth) + "x" +
            std::to_string(settings_.outputHeight) + ", " + std::to_string(settings_.frameRate) + " fps, " +
            std::to_string(settings_.bitrateKbps) + " kbps");
        InitializeH264Encoder(encoder_, device, EncodedFrameCallback, &encoded_, settings_);
        Rebuild(mainProcessor);
    }

    void Rebuild(GpuProcessor& mainProcessor)
    {
        D3D11_TEXTURE2D_DESC source{};
        mainProcessor.ComposedFrame()->GetDesc(&source);
        const RECT bounds{ 0, 0, static_cast<LONG>(source.Width), static_cast<LONG>(source.Height) };
        processor_ = std::make_unique<GpuProcessor>();
        processor_->Initialize(device_.Get(), context_.Get(), source, DXGI_MODE_ROTATION_IDENTITY, settings_, bounds);
    }

    void Submit(GpuProcessor& mainProcessor, LONGLONG time)
    {
        if (time < nextTime_) return;
        ComPtr<ID3D11Texture2D> input;
        UINT subresource = 0;
        auto sample = encoder_.AllocateInput(&input, &subresource);
        processor_->Process(mainProcessor.ComposedFrame(), input.Get(), subresource);
        encoder_.Submit(sample.Get(), time);
        nextTime_ = ((time * settings_.frameRate / 10'000'000LL) + 1) * 10'000'000LL / settings_.frameRate;
        ++submittedFrames_;
    }

    void Finalize() { encoder_.Finalize(); }
    uint64_t SubmittedFrames() const { return submittedFrames_; }
    ID3D11Texture2D* ComposedFrameForTest() const { return processor_->ComposedFrame(); }
    void VerifyTestComposition()
    {
        // Diagnostic-only single-pixel GPU readback: ensure the secondary is neither
        // black nor missing the privacy mask already applied to the shared main image.
        D3D11_TEXTURE2D_DESC desc{};
        processor_->ComposedFrame()->GetDesc(&desc);
        if (TestPixelBlack(device_.Get(), context_.Get(), processor_->ComposedFrame(), desc.Width / 2, desc.Height / 2) ||
            !TestPixelBlack(device_.Get(), context_.Get(), processor_->ComposedFrame(), desc.Width / 12, desc.Height / 12))
            throw std::runtime_error("Secondary stream composition/mask verification failed");
        Log("Secondary GPU image and shared privacy mask verified");
    }

private:
    AppSettings settings_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    EncodedFrameContext encoded_;
    H264Encoder encoder_;
    std::unique_ptr<GpuProcessor> processor_;
    LONGLONG nextTime_ = 0;
    uint64_t submittedFrames_ = 0;
};

void ApplySettings(const AppSettings& settings)
{
    kOutputWidth = settings.outputWidth;
    kOutputHeight = settings.outputHeight;
    kFrameRate = settings.frameRate;
    kBitrate = settings.bitrateKbps * 1000U;
    kGopSize = settings.gopSize;
    kFrameDuration = 10'000'000LL / static_cast<LONGLONG>(kFrameRate);
}

bool ApplyPendingLiveSettings(TrayRuntimeStatus& health, AppSettings& settings, uint64_t& revision,
                              GpuProcessor& processor)
{
    if (!health.ConsumeLiveSettings(settings, revision)) return false;
    processor.UpdateLiveSettings(settings);
    UpdateEmbeddedOnvifLiveSettings(settings);
    Log("Saved overlay and live capture options applied without restarting streams");
    return true;
}

int RunPipelineTest(const RuntimeOptions& options, const AppSettings& settings)
{
    ComPtr<IDXGIFactory1> factory;
    CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1(pipeline test)");
    ComPtr<IDXGIAdapter1> adapter;
    CheckHr(factory->EnumAdapters1(0, &adapter), "EnumAdapters1(pipeline test)");

    DXGI_ADAPTER_DESC1 adapterDesc{};
    CheckHr(adapter->GetDesc1(&adapterDesc), "GetDesc1(pipeline test)");
    Log("Pipeline test adapter: " + WideToUtf8String(adapterDesc.Description));

    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actualLevel{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CheckHr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                              levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                              &device, &actualLevel, &context), "D3D11CreateDevice(pipeline test)");

    D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceDesc.Width = 3440;
    sourceDesc.Height = 1440;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    sourceDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    CheckHr(device->CreateTexture2D(&sourceDesc, nullptr, &source), "CreateTexture2D(pipeline test source)");
    ComPtr<ID3D11RenderTargetView> sourceView;
    CheckHr(device->CreateRenderTargetView(source.Get(), nullptr, &sourceView),
            "CreateRenderTargetView(pipeline test source)");

    GpuProcessor processor;
    RECT testDesktop{ 0, 0, static_cast<LONG>(sourceDesc.Width), static_cast<LONG>(sourceDesc.Height) };
    TrayRuntimeStatus testHealth;
    testHealth.previewRequested = true;
    if (settings.highlightMouseClicks)
    {
        const uint64_t now = GetTickCount64();
        const uint64_t oldest = now > 160 ? now - 160 : 0;
        const uint64_t middle = now > 80 ? now - 80 : 0;
        testHealth.mouseClicks.push_back({ 720, 480, oldest });
        testHealth.mouseClicks.push_back({ 720, 480, middle });
        testHealth.mouseClicks.push_back({ 720, 480, now });
    }
    processor.Initialize(device.Get(), context.Get(), sourceDesc,
                         DXGI_MODE_ROTATION_IDENTITY, settings, testDesktop, &testHealth);
    const EmbeddedOnvifSettings onvifSettings = StartEmbeddedOnvifServer(settings, Log, options.dualStreamTest ? 13704 : 3702);
    RtspServer rtspServer(Log);
    rtspServer.Start(onvifSettings.rtspPort, onvifSettings.rtspPath, settings.authenticationEnabled,
                     WideToUtf8String(settings.userName), WideToUtf8String(settings.password),
                     WideToUtf8String(settings.allowedIpAddresses),
                     settings.subStreamEnabled ? WideToUtf8String(settings.subRtspPath) : "");
    H264Encoder encoder;
    EncodedFrameContext encodedContext{ &rtspServer, &testHealth };
    InitializeH264Encoder(encoder, device.Get(), EncodedFrameCallback, &encodedContext, settings);
    testHealth.subStreamEnabled = settings.subStreamEnabled;
    std::unique_ptr<SecondaryStream> secondary;
    if (settings.subStreamEnabled)
        secondary = std::make_unique<SecondaryStream>(device.Get(), context.Get(), processor, rtspServer, settings, &testHealth);

    const uint64_t frameLimit = options.maxFrames == 0 ? 60 : options.maxFrames;
    uint64_t previewTickWhenHidden = 0;
    AppSettings liveSettings = settings;
    uint64_t liveRevision = 0;
    auto nextFrameTime = std::chrono::steady_clock::now();
    for (uint64_t frame = 0; frame < frameLimit; ++frame)
    {
        if (options.liveSettingsTest && (frame == 72 || frame == 120 || frame == 168))
        {
            AppSettings updated = settings;
            updated.overlayTemplate = frame == 72 ? L"" : L"{camera}";
            updated.timestampFontName = L"Arial";
            updated.timestampFontSize = 28; updated.timestampFontWeight = 700;
            updated.timestampPosition = 3;
            updated.timestampMarginX = updated.timestampMarginY = 20;
            updated.timestampPaddingX = updated.timestampPaddingY = 12;
            updated.timestampBackgroundOpacity = frame == 168 ? 0 : 100;
            if (frame == 72) updated.privacyMasks += L";900,500,120,80";
            testHealth.QueueLiveSettings(updated);
            if (!ApplyPendingLiveSettings(testHealth, liveSettings, liveRevision, processor))
                throw std::runtime_error("Live settings revision not consumed");
        }
        if (frameLimit >= 8 && frame == frameLimit - 2)
        {
            testHealth.previewRequested = false;
            previewTickWhenHidden = testHealth.previewTickMs;
        }
        nextFrameTime += std::chrono::microseconds(1'000'000 / kFrameRate);
        const float phase = static_cast<float>(frame % kFrameRate) / static_cast<float>(kFrameRate);
        const float color[4] = { 0.08f + phase * 0.4f, 0.15f, 0.45f - phase * 0.2f, 1.0f };
        context->ClearRenderTargetView(sourceView.Get(), color);

        ComPtr<ID3D11Texture2D> inputTexture;
        UINT inputSubresource = 0;
        ComPtr<IMFSample> sample = encoder.AllocateInput(&inputTexture, &inputSubresource);
        processor.Process(source.Get(), inputTexture.Get(), inputSubresource);
        encoder.Submit(sample.Get(), static_cast<LONGLONG>(frame) * kFrameDuration);
        if (secondary) secondary->Submit(processor, static_cast<LONGLONG>(frame) * kFrameDuration);
        if (options.liveSettingsTest && (frame == 75 || frame == 123 || frame == 171))
        {
            for (auto* texture : { processor.ComposedFrame(), secondary ? secondary->ComposedFrameForTest() : nullptr })
            {
                if (!texture) continue;
                D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                const bool center = TestPixelBlack(device.Get(), context.Get(), texture, desc.Width / 2, desc.Height / 2);
                const bool corner = TestPixelBlack(device.Get(), context.Get(), texture,
                    desc.Width - std::max(1U, desc.Width * 24 / settings.outputWidth),
                    desc.Height - std::max(1U, desc.Height * 24 / settings.outputHeight));
                if (center != (frame == 75) || corner != (frame == 123))
                    throw std::runtime_error("Live mask, placement or opacity did not reach GPU output");
            }
            Log("Live mask/text/opacity GPU verification passed at frame " + std::to_string(frame));
        }
        std::this_thread::sleep_until(nextFrameTime);
    }

    encoder.Finalize();
    if (secondary)
    {
        secondary->Finalize();
        if (options.dualStreamTest) secondary->VerifyTestComposition();
        if (testHealth.subPublishedFrames != secondary->SubmittedFrames() || testHealth.subPublishedFrames == 0)
            throw std::runtime_error("Secondary GPU/H.264 pipeline lost submitted frames");
        Log("Secondary GPU/H.264 pipeline test completed: " + std::to_string(testHealth.subPublishedFrames.load()) + " frames");
    }
    rtspServer.Stop();
    if (testHealth.publishedFrames != frameLimit || testHealth.publishedBytes == 0)
        throw std::runtime_error("GPU/H.264 pipeline did not publish every submitted frame");
    if (frameLimit >= 8 && (testHealth.previewTickMs == 0 || testHealth.previewWidth > 640 ||
        testHealth.previewHeight > 640 || testHealth.previewPixels.size() !=
        static_cast<size_t>(testHealth.previewWidth) * testHealth.previewHeight * 4))
        throw std::runtime_error("Settings preview did not produce a valid bounded BGRA thumbnail");
    if (frameLimit >= 8)
    {
        if (previewTickWhenHidden != testHealth.previewTickMs || testHealth.previewPixels[3] != 255 ||
            (testHealth.previewPixels[0] == 0 && testHealth.previewPixels[1] == 0 && testHealth.previewPixels[2] == 0))
            throw std::runtime_error("Settings preview pixel data or hidden-window suspension failed");
        Log("Settings preview GPU readback and hidden-window suspension tests passed");
    }
    Log("GPU/H.264 pipeline test completed: " + std::to_string(frameLimit) + " frames");
    return 0;
}

int Run(const RuntimeOptions& options, AppSettings settings, TrayRuntimeStatus* health = nullptr)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, options.dualStreamTest ? L"Local\\Screen2NVR.DualStreamTest" : kMutexName);
    if (!mutex) throw std::runtime_error("CreateMutexW failed");
    const auto closeMutex = [&mutex]() { CloseHandle(mutex); };
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        closeMutex();
        throw std::runtime_error("Another Screen2NVR instance is already running");
    }
    try
    {
        if (options.pipelineTest)
        {
            const int result = RunPipelineTest(options, settings);
            closeMutex();
            return result;
        }

        const EmbeddedOnvifSettings onvifSettings = StartEmbeddedOnvifServer(settings, Log, options.dualStreamTest ? 13704 : 3702);
        RtspServer rtspServer(Log);
        rtspServer.Start(onvifSettings.rtspPort, onvifSettings.rtspPath, settings.authenticationEnabled,
                         WideToUtf8String(settings.userName), WideToUtf8String(settings.password),
                         WideToUtf8String(settings.allowedIpAddresses),
                         settings.subStreamEnabled ? WideToUtf8String(settings.subRtspPath) : "");
        if (health)
            health->phase.store(static_cast<uint32_t>(PipelinePhase::servicesReady), std::memory_order_release);

        DesktopCapture capture;
        capture.Initialize(settings, health);
        if (health)
            health->phase.store(static_cast<uint32_t>(PipelinePhase::captureReady), std::memory_order_release);
        while (g_running && IsInteractiveDesktopAvailable() && !capture.HasRealFrame())
            capture.Acquire(true);
        if (!g_running) { closeMutex(); return 0; }
        if (health)
        {
            health->captureHeartbeatMs.store(GetTickCount64(), std::memory_order_release);
            health->captureCycles.fetch_add(1, std::memory_order_relaxed);
        }
        if (options.captureOnly)
        {
            if (!capture.HasRealFrame())
                throw std::runtime_error("DXGI capture test did not receive a real desktop frame");
            Log("Capture-only check completed successfully");
            closeMutex();
            return 0;
        }
        auto processor = std::make_unique<GpuProcessor>();
        processor->Initialize(capture.Device(), capture.Context(), capture.SourceDesc(),
                              capture.Rotation(), settings, capture.DesktopCoordinates(), health);
        capture.ConsumeFormatChanged();
        H264Encoder encoder;
        EncodedFrameContext encodedContext{ &rtspServer, health };
        InitializeH264Encoder(encoder, capture.Device(), EncodedFrameCallback, &encodedContext,
                              settings, health);
        std::unique_ptr<SecondaryStream> secondary;
        if (settings.subStreamEnabled)
            secondary = std::make_unique<SecondaryStream>(capture.Device(), capture.Context(), *processor, rtspServer, settings, health);
        if (health) health->subStreamEnabled = settings.subStreamEnabled;
        std::unique_ptr<RtspProbeMonitor> rtspProbe;
        if (health) rtspProbe = std::make_unique<RtspProbeMonitor>(rtspServer, *health);
        if (health)
            health->phase.store(static_cast<uint32_t>(PipelinePhase::encoderReady), std::memory_order_release);
        Log("Streaming parameters: " + std::to_string(kOutputWidth) + "x" + std::to_string(kOutputHeight) +
            ", " + std::to_string(kFrameRate) + " fps, H.264, " + std::to_string(kBitrate / 1000) +
            " kbps, GOP " + std::to_string(kGopSize));
        Log("Embedded RTSP URL: rtsp://<PC-LAN-IP>:" + std::to_string(settings.rtspPort) +
            "/" + WideToUtf8String(settings.rtspPath));
        uint64_t frameNumber = 0;
        uint32_t effectiveFrameRate = kFrameRate;
        uint32_t effectiveBitrate = kBitrate;
        uint32_t overloadFrames = 0;
        uint32_t recoveryFrames = 0;
        uint64_t liveRevision = 0;
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto streamStartTime = nextFrameTime;
        auto lastEncodedTime = nextFrameTime - std::chrono::seconds(2);
        if (health)
        {
            health->effectiveFrameRate = effectiveFrameRate;
            health->effectiveBitrateKbps = kBitrate / 1000;
        }
        while (g_running && (options.maxFrames == 0 || frameNumber < options.maxFrames))
        {
            if (health && ApplyPendingLiveSettings(*health, settings, liveRevision, *processor))
            {
                effectiveFrameRate = settings.adaptiveLoad
                    ? std::max(effectiveFrameRate, settings.minimumFrameRate) : kFrameRate;
                health->effectiveFrameRate = effectiveFrameRate;
                // Draw the new overlay/masks even if the desktop and adaptive FPS are idle.
                lastEncodedTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            }
            if (health && health->streamingPaused.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                nextFrameTime = std::chrono::steady_clock::now();
                continue;
            }
            nextFrameTime += std::chrono::microseconds(1'000'000 / std::max(1U, effectiveFrameRate));
            const bool desktopAvailable = IsInteractiveDesktopAvailable();
            if (desktopAvailable && !capture.Acquire(false)) continue;
            if (health)
            {
                health->captureHeartbeatMs.store(GetTickCount64(), std::memory_order_release);
                health->captureCycles.fetch_add(1, std::memory_order_relaxed);
            }
            if (capture.ConsumeFormatChanged())
            {
                Log("Desktop mode or format changed; rebuilding GPU processing resources");
                processor = std::make_unique<GpuProcessor>();
                processor->Initialize(capture.Device(), capture.Context(), capture.SourceDesc(),
                                      capture.Rotation(), settings, capture.DesktopCoordinates(), health);
                if (secondary) secondary->Rebuild(*processor);
            }
            ComPtr<ID3D11Texture2D> inputTexture;
            UINT inputSubresource = 0;
            ComPtr<IMFSample> sample = encoder.AllocateInput(&inputTexture, &inputSubresource);
            const auto processingStart = std::chrono::steady_clock::now();
            if (!desktopAvailable && settings.standbyEnabled)
                processor->ProcessStandby(inputTexture.Get(), inputSubresource);
            else
            {
                if (!desktopAvailable) { std::this_thread::sleep_for(std::chrono::milliseconds(250)); continue; }
                const auto now = std::chrono::steady_clock::now();
                bool clickAnimationActive = false;
                if (settings.highlightMouseClicks && health)
                {
                    const uint64_t tick = GetTickCount64();
                    std::lock_guard<std::mutex> lock(health->mouseClicksMutex);
                    clickAnimationActive = std::any_of(health->mouseClicks.begin(), health->mouseClicks.end(),
                        [&](const MouseClickPulse& click)
                        {
                            return tick - click.startedTickMs < settings.mouseClickHighlightDurationMs;
                        });
                }
                if (settings.adaptiveFrameRate && !capture.LastAcquireChanged() &&
                    !clickAnimationActive && now - lastEncodedTime < std::chrono::seconds(1))
                {
                    std::this_thread::sleep_until(nextFrameTime);
                    continue;
                }
                processor->Process(capture.Frame(), inputTexture.Get(), inputSubresource);
            }
            const LONGLONG sampleTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - streamStartTime).count() / 100;
            encoder.Submit(sample.Get(), sampleTime);
            if (secondary) secondary->Submit(*processor, sampleTime);
            lastEncodedTime = std::chrono::steady_clock::now();
            ++frameNumber;
            const auto processingTime = std::chrono::steady_clock::now() - processingStart;
            const auto frameBudget = std::chrono::microseconds(1'000'000 / std::max(1U, effectiveFrameRate));
            if (settings.adaptiveLoad)
            {
                if (processingTime > frameBudget * 8 / 10)
                {
                    ++overloadFrames; recoveryFrames = 0;
                    if (overloadFrames >= 5 && effectiveFrameRate > settings.minimumFrameRate)
                    {
                        effectiveFrameRate = std::max(settings.minimumFrameRate, effectiveFrameRate * 3 / 4);
                        overloadFrames = 0;
                        Log("Adaptive load control reduced effective FPS to " + std::to_string(effectiveFrameRate));
                        if (health) health->effectiveFrameRate = effectiveFrameRate;
                    }
                    else if (overloadFrames >= 60 && effectiveFrameRate <= settings.minimumFrameRate)
                    {
                        const uint32_t minimumBitrate = std::max(256'000U, kBitrate / 2);
                        if (effectiveBitrate > minimumBitrate)
                        {
                            const uint32_t requested = std::max(minimumBitrate, effectiveBitrate * 4 / 5);
                            if (encoder.UpdateBitrate(requested))
                            {
                                effectiveBitrate = requested;
                                if (health) health->effectiveBitrateKbps = effectiveBitrate / 1000;
                            }
                            overloadFrames = 0;
                        }
                        else if (kOutputWidth > 640 && kOutputHeight > 360)
                        {
                            AppSettings reduced = settings;
                            reduced.videoProfile = 0;
                            reduced.outputWidth = std::max(640U, (settings.outputWidth * 3 / 4) & ~1U);
                            reduced.outputHeight = std::max(360U, (settings.outputHeight * 3 / 4) & ~1U);
                            NormalizeSubStreamSettings(reduced);
                            auto masks = ::ParsePrivacyMasks(settings.privacyMasks);
                            RescalePrivacyMasks(masks, settings.outputWidth, settings.outputHeight,
                                                reduced.outputWidth, reduced.outputHeight);
                            reduced.privacyMasks = SerializePrivacyMasks(masks);
                            std::wstring saveError;
                            if (SaveAppSettings(reduced, saveError))
                            {
                                Log("Adaptive load control reduced persistent resolution to " +
                                    std::to_string(reduced.outputWidth) + "x" + std::to_string(reduced.outputHeight));
                                throw std::runtime_error("Adaptive load control requested restart with lower resolution");
                            }
                            Log("Adaptive resolution reduction failed: " + WideToUtf8String(saveError));
                            overloadFrames = 0;
                        }
                    }
                }
                else
                {
                    overloadFrames = 0;
                    if (++recoveryFrames >= 120 && effectiveFrameRate < kFrameRate)
                    {
                        ++effectiveFrameRate; recoveryFrames = 0;
                        Log("Adaptive load control restored effective FPS to " + std::to_string(effectiveFrameRate));
                        if (health) health->effectiveFrameRate = effectiveFrameRate;
                    }
                }
            }
            if (options.simulateStallAfterFrames != 0 && frameNumber == options.simulateStallAfterFrames)
            {
                Log("Diagnostic: simulated pipeline stall after " + std::to_string(frameNumber) + " frames");
                while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(250));
                break;
            }
            std::this_thread::sleep_until(nextFrameTime);
        }
        encoder.Finalize();
        if (secondary) secondary->Finalize();
        rtspServer.Stop();
        Log("Capture stopped after " + std::to_string(frameNumber) + " encoded frames");
        closeMutex();
        return 0;
    }
    catch (...) { closeMutex(); throw; }
}
}

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    RuntimeOptions options;
    try { options = ParseOptions(argc, argv); }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Screen2NVR", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (options.restartWaitProcessId != 0)
    {
        HANDLE previousProcess = OpenProcess(SYNCHRONIZE, FALSE, options.restartWaitProcessId);
        if (previousProcess)
        {
            WaitForSingleObject(previousProcess, 30'000);
            CloseHandle(previousProcess);
        }
    }
    if (options.languageAction >= 0)
    {
        AppSettings settings = LoadAppSettings();
        settings.uiLanguage = static_cast<uint32_t>(options.languageAction);
        SetUiLanguage(settings.uiLanguage);
        // Preserve user-authored standby messages; translate only the built-in default.
        if (settings.standbyText == L"Экран временно недоступен" ||
            settings.standbyText == L"Screen temporarily unavailable")
            settings.standbyText = UiText(L"Экран временно недоступен", L"Screen temporarily unavailable");
        std::wstring error;
        if (SaveAppSettings(settings, error)) return 0;
        MessageBoxW(nullptr, error.c_str(), UiText(L"Ошибка сохранения", L"Save error"), MB_OK | MB_ICONERROR);
        return 1;
    }
    if (options.autoStartAction >= 0)
    {
        std::wstring error;
        if (SetAutoStartEnabled(options.autoStartAction == 1, error)) return 0;
        MessageBoxW(nullptr, error.c_str(), UiText(L"Screen2NVR — автозагрузка", L"Screen2NVR — startup"), MB_OK | MB_ICONERROR);
        return 1;
    }

    AppSettings settings = LoadAppSettings();
    if (options.dualStreamTest)
    {
        // Isolated diagnostic ports and in-memory settings; never modify a deployed camera.
        settings = AppSettings{};
        settings.onvifPort = 18002; settings.rtspPort = 18556;
        settings.subStreamEnabled = true; settings.adaptiveFrameRate = false; settings.adaptiveLoad = false;
        settings.authenticationEnabled = true;
        settings.userName = L"screen2nvr-test"; settings.password = L"temporary-test-password";
        settings.allowedIpAddresses = L"127.0.0.1";
        if (options.pipelineTest) settings.privacyMasks = L"0,0,320,180";
    }
    if (options.forceSoftwareEncoderTest)
    {
        settings.encoderPreference = 2;
        settings.allowSoftwareEncoder = true;
        settings.highlightMouseClicks = true;
    }
    if (options.securityTest)
    {
        settings.authenticationEnabled = true;
        settings.userName = L"screen2nvr-test";
        settings.password = L"temporary-test-password";
        settings.allowedIpAddresses = L"127.0.0.1";
        settings.adaptiveFrameRate = false;
    }
    ApplySettings(settings);
    std::wstring loggingError;
    ConfigureLogging(settings.loggingEnabled, loggingError);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr)) { std::cerr << "CoInitializeEx failed: " << HresultText(comHr) << '\n'; return 1; }
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
    {
        CoUninitialize(); std::cerr << "WSAStartup failed\n"; return 1;
    }
    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfHr))
    {
        WSACleanup(); CoUninitialize(); std::cerr << "MFStartup failed: " << HresultText(mfHr) << '\n'; return 1;
    }
    int exitCode = 0;
    Log("Screen2NVR integrated native GPU/RTSP/ONVIF pipeline");
    const bool diagnosticMode = options.pipelineTest || options.captureOnly || options.maxFrames != 0;
    if (diagnosticMode)
    {
        try { exitCode = Run(options, settings); }
        catch (const HrError& error)
        {
            Log(std::string(error.what()) + " failed. HRESULT=" + HresultText(error.hr));
            exitCode = 1;
        }
        catch (const std::exception& error) { Log(std::string("Error: ") + error.what()); exitCode = 1; }
    }
    else
    {
        TrayRuntimeStatus status;
        status.startTickMs = GetTickCount64();
        status.logger = [](const std::string& message) { Log(message); };
        status.configureLogging = ConfigureLogging;
        Log("Health watchdog enabled: 60 s startup timeout, 20 s capture/H.264 stall timeout");
        std::thread pipelineThread([&]()
        {
            const HRESULT workerCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                if (FAILED(workerCom)) throw HrError("CoInitializeEx(capture thread)", workerCom);
                Run(options, settings, &status);
            }
            catch (const HrError& error)
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                status.error = AsciiToWide(error.what()) + L". HRESULT=" + AsciiToWide(HresultText(error.hr));
                Log("Pipeline failure: " + std::string(error.what()) + ". HRESULT=" + HresultText(error.hr));
            }
            catch (const std::exception& error)
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                status.error = UiText(L"Ошибка: ", L"Error: ") + AsciiToWide(error.what());
                Log(std::string("Pipeline failure: ") + error.what());
            }
            if (SUCCEEDED(workerCom)) CoUninitialize();
            status.finished = true;
        });
        exitCode = RunTrayApplication(settings, g_running, status);
        g_running = false;
        pipelineThread.join();
    }
    MFShutdown();
    WSACleanup();
    CoUninitialize();
    return exitCode;
}
