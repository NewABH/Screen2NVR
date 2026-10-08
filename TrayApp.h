#pragma once

#include "Settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class PipelinePhase : uint32_t
{
    starting = 0,
    servicesReady = 1,
    captureReady = 2,
    encoderReady = 3,
    streaming = 4,
};

struct MouseClickPulse
{
    int32_t screenX = 0;
    int32_t screenY = 0;
    uint64_t startedTickMs = 0;
};

struct TrayRuntimeStatus
{
    // UI publishes only saved drafts; the capture thread applies them between frames.
    std::mutex liveSettingsMutex;
    AppSettings liveSettings;
    std::atomic<uint64_t> liveSettingsRevision{ 0 };
    // Optional test-host persistence; production uses SaveAppSettings.
    std::function<bool(const AppSettings&, std::wstring&)> settingsSaver;
    void QueueLiveSettings(const AppSettings& settings)
    {
        std::lock_guard<std::mutex> lock(liveSettingsMutex);
        liveSettings = settings;
        ++liveSettingsRevision;
    }
    bool ConsumeLiveSettings(AppSettings& active, uint64_t& revision)
    {
        if (revision == liveSettingsRevision.load()) return false;
        std::lock_guard<std::mutex> lock(liveSettingsMutex);
        CopyLiveSettings(active, liveSettings);
        revision = liveSettingsRevision.load();
        return true;
    }
    // A bounded thumbnail from the capture thread, requested only by the visible editor.
    std::atomic_bool previewRequested{ false };
    std::mutex previewMutex;
    std::vector<uint8_t> previewPixels;
    uint32_t previewWidth = 0, previewHeight = 0;
    uint64_t previewTickMs = 0;
    bool previewStandby = false;
    std::atomic_bool finished{ false };
    std::atomic<uint32_t> phase{ static_cast<uint32_t>(PipelinePhase::starting) };
    std::atomic<uint64_t> startTickMs{ 0 };
    std::atomic<uint64_t> captureHeartbeatMs{ 0 };
    std::atomic<uint64_t> publishedHeartbeatMs{ 0 };
    std::atomic<uint64_t> captureCycles{ 0 };
    std::atomic<uint64_t> publishedFrames{ 0 };
    std::atomic<uint64_t> publishedBytes{ 0 };
    std::atomic<uint64_t> rtspProbeHeartbeatMs{ 0 };
    std::atomic_bool subStreamEnabled{ false };
    std::atomic<uint64_t> subPublishedFrames{ 0 };
    std::atomic<uint64_t> subPublishedHeartbeatMs{ 0 };
    std::atomic<uint64_t> subRtspProbeHeartbeatMs{ 0 };
    std::atomic<uint32_t> effectiveFrameRate{ 0 };
    std::atomic<uint32_t> effectiveBitrateKbps{ 0 };
    std::atomic_bool streamingPaused{ false };
    std::mutex mouseClicksMutex;
    std::vector<MouseClickPulse> mouseClicks;
    std::mutex mutex;
    uint32_t connectedDevices = 0, mainRtspSessions = 0, subRtspSessions = 0; // Guarded by mutex.
    std::wstring error;
    std::wstring adapterName;
    std::wstring monitorName;
    std::wstring encoderName;
    std::wstring encoderKind;
    std::wstring sourceDescription;
    std::function<void(const std::string&)> logger;
    // Apply directly from the UI, even if capture is paused/stalled and cannot consume live settings.
    std::function<bool(bool, std::wstring&)> configureLogging;
};

int RunTrayApplication(AppSettings settings, std::atomic_bool& running, TrayRuntimeStatus& status);
