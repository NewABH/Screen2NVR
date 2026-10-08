#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "TrayApp.h"
#include "Screen2ONVIF.h"
#include "Security.h"
#include "CaptureGeometry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kApplicationIconResource = 101;
constexpr UINT kTimerId = 1;
constexpr uint64_t kStartupTimeoutMs = 60'000;
constexpr uint64_t kPipelineStallTimeoutMs = 20'000;
constexpr uint64_t kRestartNotificationMs = 6'000;
constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuExit = 1002;
constexpr UINT kMenuToggleStream = 1003;
constexpr UINT kMenuRestart = 1004;
constexpr UINT kMenuCopyRtsp = 1005;
constexpr UINT kMenuOpenLog = 1006;
constexpr int kTab = 2000;
constexpr int kDeviceTab = 0, kVideoTab = 1, kOverlayTab = 2, kSecurityTab = 3, kStatusTab = 4;
constexpr int kCameraName = 2001;
constexpr int kOnvifPort = 2005;
constexpr int kRtspPort = 2006;
constexpr int kRtspPath = 2007;
constexpr int kResolution = 2008;
constexpr int kResolutionSource = 2009;
constexpr int kFps = 2010;
constexpr int kBitrate = 2011;
constexpr int kGop = 2012;
constexpr int kAutoStart = 2014;
constexpr int kSave = 2015;
constexpr int kCancel = 2016;
constexpr int kChooseFont = 2017;
constexpr int kTimestampMarginX = 2018;
constexpr int kTimestampMarginY = 2019;
constexpr int kTimestampPaddingX = 2020;
constexpr int kTimestampPaddingY = 2021;
constexpr int kTimestampOpacity = 2022;
constexpr int kPreview = 2024;
constexpr int kTextPosition = 2027;
constexpr int kMarginXValue = 2030;
constexpr int kMarginYValue = 2031;
constexpr int kPaddingXValue = 2032;
constexpr int kPaddingYValue = 2033;
constexpr int kOpacityValue = 2034;
constexpr int kVideoProfile = 2040;
constexpr int kAdaptiveFps = 2041;
constexpr int kAdaptiveLoad = 2042;
constexpr int kMinimumFps = 2043;
constexpr int kEncoderPreference = 2044;
constexpr int kAllowSoftwareEncoder = 2045;
constexpr int kCaptureMode = 2050;
constexpr int kMonitorIndex = 2051;
constexpr int kRegionX = 2052;
constexpr int kRegionY = 2053;
constexpr int kRegionWidth = 2054;
constexpr int kRegionHeight = 2055;
constexpr int kWindowTitle = 2056;
constexpr int kShowCursor = 2057;
constexpr int kHighlightMouseClicks = 2058;
constexpr int kMouseClickSize = 2059;
constexpr int kStandbyEnabled = 2060;
constexpr int kStandbyText = 2061;
constexpr int kSourceHint = 2062;
constexpr int kMouseClickDuration = 2063;
constexpr int kAuthentication = 2070;
constexpr int kUserName = 2071;
constexpr int kPassword = 2072;
constexpr int kAllowedIps = 2073;
constexpr int kOverlayTemplate = 2080;
constexpr int kStatusText = 2090;
constexpr int kMaskList = 2091;
constexpr int kAddMask = 2092;
constexpr int kDeleteMask = 2093;
constexpr int kClearMasks = 2094;
constexpr int kLoggingEnabled = 2096;
constexpr int kSubEnabled = 2100, kSubResolution = 2101, kSubFps = 2103;
constexpr int kSubBitrate = 2104, kSubGop = 2105, kSubPath = 2106;
constexpr int kMainVideoAddress = 2107, kSubVideoAddress = 2108;
constexpr int kTemplateTokenFirst = 2110;
constexpr const wchar_t* kTemplateTokens[] = { L"{camera}", L"{date}", L"{time}", L"{computer}", L"{user}" };
using Microsoft::WRL::ComPtr;

class TrayApplication
{
public:
    TrayApplication(AppSettings settings, std::atomic_bool& running, TrayRuntimeStatus& status)
        : settings_(std::move(settings)), draftSettings_(settings_), activeSettings_(settings_), running_(running), status_(status) {}

    int Run()
    {
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_TAB_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&controls);
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hIcon = LoadApplicationIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = L"Screen2NVR.SettingsWindow";
        windowClass.hIconSm = LoadApplicationIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        RegisterClassExW(&windowClass);

        RECT bounds{ 0, 0, 960, 640 };
        AdjustWindowRectEx(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_APPWINDOW);
        window_ = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"Настройки Screen2NVR",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
                                  windowClass.hInstance, this);
        if (!window_) return 1;
        // Consume STARTUPINFO's initial show state before opening settings from a hidden/autostart launch.
        ShowWindow(window_, SW_HIDE);
        mouseHookOwner_ = this;
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProcedure, GetModuleHandleW(nullptr), 0);
        if (!mouseHook_ && status_.logger)
            status_.logger("Mouse click highlighting is unavailable: SetWindowsHookExW failed.");
        AddTrayIcon();
        SetTimer(window_, kTimerId, 500, nullptr);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(window_, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
        mouseHookOwner_ = nullptr;
        return static_cast<int>(message.wParam);
    }

private:
    static HICON LoadApplicationIcon(int width, int height)
    {
        HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                   MAKEINTRESOURCEW(kApplicationIconResource),
                                                   IMAGE_ICON, width, height, LR_SHARED));
        if (!icon)
            icon = static_cast<HICON>(LoadImageW(nullptr, MAKEINTRESOURCEW(32512),
                                                 IMAGE_ICON, width, height, LR_SHARED));
        return icon;
    }

    static bool IsInteractiveDesktopAvailable()
    {
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!desktop) return false;
        wchar_t name[64]{};
        DWORD required = 0;
        const bool available = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &required) != FALSE &&
                               _wcsicmp(name, L"Default") == 0;
        CloseDesktop(desktop);
        return available;
    }

    static LRESULT CALLBACK MouseHookProcedure(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && mouseHookOwner_ &&
            (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN))
        {
            mouseHookOwner_->RecordMouseClick(*reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
        }
        return CallNextHookEx(mouseHookOwner_ ? mouseHookOwner_->mouseHook_ : nullptr, code, wParam, lParam);
    }

    void RecordMouseClick(const MSLLHOOKSTRUCT& mouse)
    {
        if (!settings_.highlightMouseClicks) return;
        const uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lock(status_.mouseClicksMutex);
        auto& clicks = status_.mouseClicks;
        clicks.erase(std::remove_if(clicks.begin(), clicks.end(), [&](const MouseClickPulse& click)
        {
            return now - click.startedTickMs > settings_.mouseClickHighlightDurationMs;
        }), clicks.end());
        if (clicks.size() >= 64) clicks.erase(clicks.begin(), clicks.begin() + (clicks.size() - 63));
        clicks.push_back({ mouse.pt.x, mouse.pt.y, now });
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<TrayApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<TrayApplication*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(window, message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == taskbarCreatedMessage_)
        {
            AddTrayIcon();
            return 0;
        }
        switch (message)
        {
        case WM_CREATE:
            window_ = window;
            CreateControls(window);
            return 0;
        case WM_SHOWWINDOW:
            status_.previewRequested = wParam != 0 && selectedTab_ == kOverlayTab && !IsIconic(window_);
            break;
        case WM_SIZE:
            status_.previewRequested = wParam != SIZE_MINIMIZED && selectedTab_ == kOverlayTab && IsWindowVisible(window_);
            break;
        case WM_DISPLAYCHANGE:
        {
            const LRESULT item = SendDlgItemMessageW(window_, kMonitorIndex, CB_GETCURSEL, 0, 0);
            const LRESULT index = SendDlgItemMessageW(window_, kMonitorIndex, CB_GETITEMDATA, item, 0);
            PopulateMonitors(index >= 0 ? static_cast<uint32_t>(index) : settings_.monitorIndex);
            RefreshResolutionChoices(true);
            break;
        }
        case kTrayMessage:
        {
            const UINT trayEvent = LOWORD(lParam);
            if (trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) ShowTrayMenu();
            else if (trayEvent == WM_LBUTTONDBLCLK) ShowSettings();
            return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == tab_ &&
                reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE)
            {
                selectedTab_ = TabCtrl_GetCurSel(tab_);
                ShowSelectedTab();
            }
            return 0;
        case WM_HSCROLL:
            UpdateTimestampFromSliders();
            return 0;
        case WM_DRAWITEM:
            if (wParam >= kTemplateTokenFirst && wParam < kTemplateTokenFirst + ARRAYSIZE(kTemplateTokens))
            {
                DrawTemplateLink(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            if (wParam == kPreview)
            {
                DrawTimestampPreview(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) >= kTemplateTokenFirst && LOWORD(wParam) < kTemplateTokenFirst + ARRAYSIZE(kTemplateTokens))
            {
                InsertTemplateToken(LOWORD(wParam) - kTemplateTokenFirst);
                return 0;
            }
            switch (LOWORD(wParam))
            {
            case kMenuSettings: ShowSettings(); return 0;
            case kMenuToggleStream:
            {
                const bool paused = !status_.streamingPaused.load();
                status_.streamingPaused = paused;
                if (!paused)
                {
                    const uint64_t now = GetTickCount64();
                    status_.captureHeartbeatMs = now;
                    status_.publishedHeartbeatMs = now;
                    status_.rtspProbeHeartbeatMs = now;
                }
                UpdateStatusDisplay();
                return 0;
            }
            case kMenuRestart: RestartApplication(); return 0;
            case kMenuCopyRtsp: CopyRtspAddress(); return 0;
            case kMenuOpenLog:
                ShellExecuteW(window_, L"open", GetScreen2NvrLogDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case kMenuExit: ExitApplication(); return 0;
            case kSave: SaveFromControls(); return 0;
            case IDOK: SaveFromControls(); return 0;
            case IDCANCEL:
                if (addingMask_ || draggingMask_ || draggingText_)
                    SendDlgItemMessageW(window_, kPreview, WM_KEYDOWN, VK_ESCAPE, 0);
                else ShowWindow(window_, SW_HIDE);
                return 0;
            case kCancel: ShowWindow(window_, SW_HIDE); return 0;
            case kCameraName:
                if (HIWORD(wParam) == EN_CHANGE) draftSettings_.cameraName = GetText(kCameraName);
                return 0;
            case kRtspPort:
            case kRtspPath:
            case kSubPath:
                if (HIWORD(wParam) == EN_CHANGE) UpdateRtspAddress();
                return 0;
            case kSubEnabled:
                if (HIWORD(wParam) == BN_CLICKED) UpdateSubStreamControls();
                return 0;
            case kCaptureMode:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    UpdateSourceControls();
                    RefreshResolutionChoices(false);
                }
                return 0;
            case kMonitorIndex:
                if (HIWORD(wParam) == CBN_SELCHANGE) RefreshResolutionChoices(false);
                return 0;
            case kRegionX:
            case kRegionY:
            case kRegionWidth:
            case kRegionHeight:
            case kWindowTitle:
                if (!populatingControls_ && HIWORD(wParam) == EN_KILLFOCUS) RefreshResolutionChoices(false, false);
                return 0;
            case kResolution:
                if (HIWORD(wParam) == CBN_SELCHANGE) RefreshSubResolutions(false);
                return 0;
            case kFps:
            case kBitrate:
            case kGop:
                if (!populatingControls_ && !applyingProfile_ && HIWORD(wParam) == EN_CHANGE)
                    SendDlgItemMessageW(window_, kVideoProfile, CB_SETCURSEL, 0, 0);
                return 0;
            case kHighlightMouseClicks:
            case kStandbyEnabled:
                if (HIWORD(wParam) == BN_CLICKED) UpdateSourceControls();
                return 0;
            case kMaskList:
                if (HIWORD(wParam) == LBN_SELCHANGE)
                {
                    selectedMask_ = static_cast<int>(SendDlgItemMessageW(window_, kMaskList, LB_GETCURSEL, 0, 0));
                    addingMask_ = false;
                    UpdateMaskButtons();
                    InvalidateRect(GetDlgItem(window_, kPreview), nullptr, FALSE);
                }
                return 0;
            case kAddMask:
                addingMask_ = !addingMask_;
                UpdateMaskButtons();
                return 0;
            case kDeleteMask:
                if (selectedMask_ >= 0 && selectedMask_ < static_cast<int>(masks_.size()))
                    masks_.erase(masks_.begin() + selectedMask_);
                selectedMask_ = -1;
                RefreshMaskList();
                return 0;
            case kClearMasks:
                masks_.clear(); selectedMask_ = -1;
                RefreshMaskList();
                return 0;
            case kChooseFont: ChooseTimestampFont(); return 0;
            case kOverlayTemplate:
                if (HIWORD(wParam) == EN_CHANGE)
                {
                    draftSettings_.overlayTemplate = GetText(kOverlayTemplate);
                    InvalidateRect(GetDlgItem(window_, kPreview), nullptr, TRUE);
                }
                return 0;
            case kTextPosition:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    draftSettings_.timestampPosition = static_cast<uint32_t>(
                        SendDlgItemMessageW(window_, kTextPosition, CB_GETCURSEL, 0, 0));
                    InvalidateRect(GetDlgItem(window_, kPreview), nullptr, TRUE);
                }
                return 0;
            case kVideoProfile:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    const LRESULT profile = SendDlgItemMessageW(window_, kVideoProfile, CB_GETCURSEL, 0, 0);
                    if (profile <= 0) return 0; // Custom preserves the values currently being edited.
                    ApplyVideoProfile(draftSettings_, static_cast<uint32_t>(std::max<LRESULT>(0, profile)));
                    applyingProfile_ = true;
                    SetDlgItemInt(window_, kFps, draftSettings_.frameRate, FALSE);
                    SetDlgItemInt(window_, kBitrate, draftSettings_.bitrateKbps, FALSE);
                    SetDlgItemInt(window_, kGop, draftSettings_.gopSize, FALSE);
                    applyingProfile_ = false;
                }
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kTimerId)
            {
                UpdateStatusDisplay(); CheckPipelineHealth();
                RememberForegroundWindow();
                if (IsWindowVisible(window_) && selectedTab_ == kVideoTab)
                    RefreshResolutionChoices(true, false);
                if (IsWindowVisible(window_) && selectedTab_ == kOverlayTab)
                    InvalidateRect(GetDlgItem(window_, kPreview), nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE: ShowWindow(window_, SW_HIDE); return 0;
        case WM_DESTROY:
            status_.previewRequested = false;
            RemoveTrayIcon();
            if (ownsUiFont_) { DeleteObject(uiFont_); uiFont_ = nullptr; }
            if (linkFont_) { DeleteObject(linkFont_); linkFont_ = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void CheckPipelineHealth()
    {
        const uint64_t now = GetTickCount64();
        if (restartScheduled_)
        {
            if (now >= restartDeadlineMs_) ExitProcess(3);
            return;
        }
        if (!running_.load(std::memory_order_acquire) || status_.streamingPaused.load(std::memory_order_acquire) ||
            !IsInteractiveDesktopAvailable()) return;

        if (status_.finished.load(std::memory_order_acquire))
        {
            std::wstring reason;
            { std::lock_guard<std::mutex> lock(status_.mutex); reason = status_.error; }
            if (reason.empty()) reason = L"Видеоконвейер неожиданно завершил работу.";
            if (status_.publishedFrames.load(std::memory_order_acquire) == 0)
            {
                recoveryAttempted_ = true;
                if (status_.logger)
                    status_.logger("Startup failure before the first H.264 frame; automatic restart suppressed: " +
                                   WideToUtf8String(reason));
                MessageBoxW(window_, reason.c_str(), L"Ошибка запуска Screen2NVR",
                            MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
                ExitApplication();
                return;
            }
            ScheduleRecovery(reason);
            return;
        }

        const uint64_t started = status_.startTickMs.load(std::memory_order_acquire);
        const uint64_t uptime = now >= started ? now - started : 0;
        const auto phase = static_cast<PipelinePhase>(status_.phase.load(std::memory_order_acquire));
        const uint64_t lastCapture = status_.captureHeartbeatMs.load(std::memory_order_acquire);
        const uint64_t lastPublished = status_.publishedHeartbeatMs.load(std::memory_order_acquire);

        if (uptime > kStartupTimeoutMs && phase < PipelinePhase::captureReady)
        {
            ScheduleRecovery(L"За 60 секунд не удалось запустить захват экрана DXGI.");
            return;
        }
        if (phase >= PipelinePhase::encoderReady && lastPublished == 0 && uptime > kStartupTimeoutMs)
        {
            ScheduleRecovery(L"Аппаратный H.264-кодировщик не выдал ни одного кадра за 60 секунд.");
            return;
        }
        if (lastPublished != 0 && now - lastPublished > kPipelineStallTimeoutMs)
        {
            const uint64_t ageSeconds = (now - lastPublished) / 1000;
            const uint64_t frames = status_.publishedFrames.load(std::memory_order_relaxed);
            const uint64_t bytes = status_.publishedBytes.load(std::memory_order_relaxed);
            ScheduleRecovery(L"Трансляция H.264/RTSP остановилась: закодированные кадры не поступают " +
                             std::to_wstring(ageSeconds) + L" с. Перед остановкой опубликовано " +
                             std::to_wstring(frames) + L" кадров (" + std::to_wstring(bytes) + L" байт)." );
            return;
        }
        const uint64_t lastProbe = status_.rtspProbeHeartbeatMs.load(std::memory_order_acquire);
        if (status_.subStreamEnabled && phase >= PipelinePhase::encoderReady)
        {
            const uint64_t subPublished = status_.subPublishedHeartbeatMs.load();
            const uint64_t subProbe = status_.subRtspProbeHeartbeatMs.load();
            if ((subPublished == 0 && uptime > kStartupTimeoutMs) ||
                (subPublished != 0 && now - subPublished > kPipelineStallTimeoutMs))
            {
                ScheduleRecovery(L"Вторичный поток H.264 перестал выдавать кадры.");
                return;
            }
            if (subPublished != 0 && uptime > kStartupTimeoutMs && (subProbe == 0 || now - subProbe > 30'000))
            {
                ScheduleRecovery(L"Локальная проверка RTSP вторичного потока не получает видео более 30 секунд.");
                return;
            }
        }
        if (lastPublished != 0 && uptime > 30'000 && (lastProbe == 0 || now - lastProbe > 30'000))
        {
            ScheduleRecovery(L"Встроенная внешняя проверка RTSP не может подключиться к локальному серверу более 30 секунд.");
            return;
        }
        if (lastCapture != 0 && now - lastCapture > kPipelineStallTimeoutMs)
        {
            const uint64_t ageSeconds = (now - lastCapture) / 1000;
            ScheduleRecovery(L"Завис захват или GPU-обработка экрана: конвейер не обновлялся " +
                             std::to_wstring(ageSeconds) + L" с.");
        }
    }

    void ShowRecoveryNotification(const std::wstring& reason)
    {
        tray_.uFlags = NIF_INFO;
        wcscpy_s(tray_.szInfoTitle, L"Screen2NVR: поток остановлен");
        wcsncpy_s(tray_.szInfo, reason.c_str(), _TRUNCATE);
        tray_.dwInfoFlags = NIIF_ERROR | NIIF_LARGE_ICON;
        tray_.uTimeout = static_cast<UINT>(kRestartNotificationMs);
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
        MessageBeep(MB_ICONERROR);
    }

    void ScheduleRecovery(const std::wstring& reason)
    {
        if (recoveryAttempted_) return;
        recoveryAttempted_ = true;
        {
            std::lock_guard<std::mutex> lock(status_.mutex);
            status_.error = reason;
        }
        if (status_.logger)
            status_.logger("WATCHDOG ERROR: " + WideToUtf8String(reason) +
                           " Automatic process restart scheduled.");
        ShowRecoveryNotification(reason);

        std::vector<wchar_t> executable(32768);
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        const std::wstring parameters = L"--restart-wait " + std::to_wstring(GetCurrentProcessId());
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.data(), parameters.c_str(),
                                                    nullptr, SW_SHOWNORMAL)) <= 32)
        {
            if (status_.logger) status_.logger("WATCHDOG ERROR: failed to launch replacement process.");
            MessageBoxW(window_, (reason + L"\n\nНе удалось автоматически запустить новый экземпляр.").c_str(),
                        L"Ошибка Screen2NVR", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
            return;
        }
        running_ = false;
        restartScheduled_ = true;
        restartDeadlineMs_ = GetTickCount64() + kRestartNotificationMs;
    }

    static LRESULT CALLBACK GroupBoxProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR, DWORD_PTR)
    {
        if (message == WM_ERASEBKGND)
        {
            // Group boxes are sibling backgrounds, not containers. Their stock painter
            // leaves the interior transparent, while WS_CLIPSIBLINGS clips the tab behind
            // them. Explicitly erase that interior or pixels from the previous tab remain.
            RECT bounds{};
            GetClientRect(window, &bounds);
            FillRect(reinterpret_cast<HDC>(wParam), &bounds, GetSysColorBrush(COLOR_BTNFACE));
            return 1;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, GroupBoxProcedure, 0);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    HWND CreateControl(std::vector<HWND>* group, DWORD exStyle, const wchar_t* className,
                       const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0)
    {
        const bool interactive = wcscmp(className, L"EDIT") == 0 || wcscmp(className, L"COMBOBOX") == 0 ||
            wcscmp(className, L"LISTBOX") == 0 || wcscmp(className, TRACKBAR_CLASSW) == 0 ||
            (wcscmp(className, L"BUTTON") == 0 && (style & BS_TYPEMASK) != BS_GROUPBOX);
        HWND control = CreateWindowExW(exStyle, className, text,
                                       WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style | (interactive ? WS_TABSTOP : 0),
                                       x, y, width, height, window_,
                                       id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        if (wcscmp(className, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_GROUPBOX)
            SetWindowSubclass(control, GroupBoxProcedure, 0, 0);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        if (group) group->push_back(control);
        return control;
    }

    void CreateLabeledEdit(std::vector<HWND>& group, const wchar_t* label, int id, int y,
                           DWORD editStyle = ES_AUTOHSCROLL)
    {
        CreateControl(&group, 0, L"STATIC", label, SS_CENTERIMAGE, 30, y, 190, 25);
        CreateControl(&group, WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle, 225, y, 420, 25, id);
    }

    void CreateSlider(std::vector<HWND>& group, const wchar_t* label, int sliderId, int valueId,
                      int x, int y, int width)
    {
        CreateControl(&group, 0, L"STATIC", label, SS_CENTERIMAGE, x, y, width - 55, 23);
        HWND slider = CreateControl(&group, 0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS,
                                    x - 3, y + 24, width + 3, 24, sliderId);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        CreateControl(&group, WS_EX_CLIENTEDGE, L"STATIC", L"0", SS_CENTER | SS_CENTERIMAGE,
                      x + width - 45, y + 2, 45, 23, valueId);
    }

    void CreateControls(HWND)
    {
        uiFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        ownsUiFont_ = uiFont_ != nullptr;
        if (!uiFont_) uiFont_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        tab_ = CreateControl(nullptr, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP,
                             10, 10, 940, 580, kTab);
        for (const wchar_t* name : { L"Устройство", L"Видео", L"Наложение", L"Безопасность", L"Состояние" })
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(name);
            TabCtrl_InsertItem(tab_, TabCtrl_GetItemCount(tab_), &item);
        }

        CreateControl(&deviceControls_, 0, L"BUTTON", L"Камера и подключение", BS_GROUPBOX, 25, 50, 420, 220);
        CreateControl(&deviceControls_, 0, L"STATIC", L"Имя камеры", SS_LEFT, 45, 80, 380, 22);
        CreateControl(&deviceControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 45, 106, 380, 26, kCameraName);
        CreateControl(&deviceControls_, 0, L"STATIC", L"Порт ONVIF", SS_LEFT, 45, 151, 170, 22);
        CreateControl(&deviceControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL, 45, 177, 170, 26, kOnvifPort);
        CreateControl(&deviceControls_, 0, L"STATIC", L"Порт RTSP", SS_LEFT, 245, 151, 170, 22);
        CreateControl(&deviceControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL, 245, 177, 170, 26, kRtspPort);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Запускать при входе в Windows",
                      BS_AUTOCHECKBOX, 45, 232, 380, 25, kAutoStart);

        CreateControl(&videoControls_, 0, L"STATIC", L"Основной поток · H.264", SS_CENTERIMAGE, 250, 50, 350, 25);
        CreateControl(&videoControls_, 0, L"BUTTON", L"Включить вторичный поток", BS_AUTOCHECKBOX,
                      640, 50, 280, 25, kSubEnabled);
        CreateControl(&videoControls_, 0, L"STATIC", L"Профиль качества", SS_CENTERIMAGE, 30, 90, 210, 25);
        HWND profile = CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                     CBS_DROPDOWNLIST | WS_VSCROLL, 250, 90, 360, 160, kVideoProfile);
        for (const wchar_t* item : { L"Пользовательский", L"Экономичный — 8 FPS",
                                     L"Стандартный — 12 FPS", L"Плавный — 25 FPS" })
            SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        CreateControl(&videoControls_, 0, L"STATIC", L"H.264 · уменьшенная копия", SS_CENTERIMAGE, 640, 90, 275, 25);
        const auto streamRow = [&](const wchar_t* label, int mainId, int subId, int y)
        {
            CreateControl(&videoControls_, 0, L"STATIC", label, SS_CENTERIMAGE, 30, y, 210, 25);
            CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                          250, y, 360, 25, mainId);
            CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                          640, y, 275, 25, subId);
        };
        CreateControl(&videoControls_, 0, L"STATIC", L"Разрешение", SS_CENTERIMAGE, 30, 134, 210, 25);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      250, 134, 360, 270, kResolution);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      640, 134, 275, 200, kSubResolution);
        CreateControl(&videoControls_, 0, L"STATIC", L"", SS_CENTERIMAGE, 250, 168, 665, 25, kResolutionSource);
        streamRow(L"Кадров в секунду", kFps, kSubFps, 202);
        streamRow(L"Битрейт, Кбит/с", kBitrate, kSubBitrate, 236);
        streamRow(L"Интервал ключевых кадров", kGop, kSubGop, 270);
        CreateControl(&videoControls_, 0, L"STATIC", L"Путь RTSP", SS_CENTERIMAGE, 30, 304, 210, 25);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 250, 304, 360, 25, kRtspPath);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 640, 304, 275, 25, kSubPath);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL,
                      30, 355, 435, 26, kMainVideoAddress);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL,
                      480, 355, 435, 26, kSubVideoAddress);
        CreateControl(&videoControls_, 0, L"STATIC", L"Минимальный FPS", SS_CENTERIMAGE, 30, 408, 170, 25);
        CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                      205, 408, 65, 25, kMinimumFps);
        CreateControl(&videoControls_, 0, L"BUTTON", L"Адаптивный FPS при неподвижном экране",
                      BS_AUTOCHECKBOX, 290, 408, 335, 24, kAdaptiveFps);
        CreateControl(&videoControls_, 0, L"BUTTON", L"Автоматически снижать нагрузку",
                      BS_AUTOCHECKBOX, 640, 408, 280, 24, kAdaptiveLoad);
        CreateControl(&videoControls_, 0, L"STATIC", L"Кодирование обоих потоков", SS_CENTERIMAGE, 30, 452, 220, 25);
        HWND encoder = CreateControl(&videoControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                     CBS_DROPDOWNLIST, 250, 452, 665, 130, kEncoderPreference);
        for (const wchar_t* item : { L"Автоматически: аппаратный, затем программный",
                                     L"Только аппаратный", L"Предпочитать программный" })
            SendMessageW(encoder, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        CreateControl(&videoControls_, 0, L"BUTTON", L"Разрешить программный H.264 как резервный вариант",
                      BS_AUTOCHECKBOX, 250, 488, 665, 24, kAllowSoftwareEncoder);
        CreateControl(&videoControls_, 0, L"STATIC",
                      L"Вторичный поток использует тот же экран, текст и маски. Его размеры и FPS не превышают основной поток.\n"
                      L"При адаптивном FPS частота обоих потоков снижается на неподвижном экране. Второй кодировщик увеличивает нагрузку.",
                      SS_LEFT, 30, 530, 885, 45);

        CreateControl(&deviceControls_, 0, L"BUTTON", L"Источник изображения", BS_GROUPBOX, 465, 50, 460, 515);
        CreateControl(&deviceControls_, 0, L"STATIC", L"Что захватывать", SS_LEFT, 485, 80, 410, 22);
        HWND sourceMode = CreateControl(&deviceControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                        CBS_DROPDOWNLIST, 485, 106, 420, 150, kCaptureMode);
        for (const wchar_t* item : { L"Монитор целиком", L"Прямоугольная область монитора",
                                     L"Окно по заголовку", L"Активное окно" })
            SendMessageW(sourceMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        CreateControl(&deviceControls_, 0, L"STATIC", L"Монитор", SS_LEFT, 485, 151, 410, 22);
        CreateControl(&deviceControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      485, 177, 420, 180, kMonitorIndex);
        CreateControl(&regionControls_, 0, L"STATIC", L"Слева, пикс.", SS_LEFT, 485, 225, 190, 22);
        CreateControl(&regionControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL,
                      485, 251, 190, 25, kRegionX);
        CreateControl(&regionControls_, 0, L"STATIC", L"Сверху, пикс.", SS_LEFT, 715, 225, 190, 22);
        CreateControl(&regionControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL,
                      715, 251, 190, 25, kRegionY);
        CreateControl(&regionControls_, 0, L"STATIC", L"Ширина, пикс.", SS_LEFT, 485, 290, 190, 22);
        CreateControl(&regionControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                      485, 316, 190, 25, kRegionWidth);
        CreateControl(&regionControls_, 0, L"STATIC", L"Высота, пикс.", SS_LEFT, 715, 290, 190, 22);
        CreateControl(&regionControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                      715, 316, 190, 25, kRegionHeight);
        CreateControl(&windowControls_, 0, L"STATIC", L"Часть заголовка окна", SS_LEFT, 485, 225, 410, 22);
        CreateControl(&windowControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 485, 251, 420, 25, kWindowTitle);
        CreateControl(&deviceControls_, 0, L"STATIC", L"", SS_LEFT, 485, 366, 420, 105, kSourceHint);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Мышь", BS_GROUPBOX, 25, 285, 420, 153);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Показывать курсор", BS_AUTOCHECKBOX,
                      45, 307, 380, 25, kShowCursor);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Подсвечивать каждое нажатие мыши", BS_AUTOCHECKBOX,
                      45, 337, 380, 25, kHighlightMouseClicks);
        CreateControl(&clickControls_, 0, L"STATIC", L"Диаметр, пикс.", SS_LEFT, 45, 370, 170, 22);
        CreateControl(&clickControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                      45, 396, 170, 25, kMouseClickSize);
        CreateControl(&clickControls_, 0, L"STATIC", L"Длительность, мс", SS_LEFT, 245, 370, 170, 22);
        CreateControl(&clickControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL,
                      245, 396, 170, 25, kMouseClickDuration);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Если экран недоступен", BS_GROUPBOX, 25, 452, 420, 113);
        CreateControl(&deviceControls_, 0, L"BUTTON", L"Передавать заставку при блокировке",
                      BS_AUTOCHECKBOX, 45, 478, 380, 25, kStandbyEnabled);
        CreateControl(&standbyControls_, 0, L"STATIC", L"Текст", SS_CENTERIMAGE, 45, 519, 60, 25);
        CreateControl(&standbyControls_, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, 115, 519, 310, 25, kStandbyText);

        CreateControl(&timestampControls_, 0, L"STATIC", L"Предпросмотр", SS_LEFT, 30, 47, 560, 20);
        HWND preview = CreateControl(&timestampControls_, 0, L"STATIC", L"",
                                     SS_OWNERDRAW | SS_NOTIFY | WS_TABSTOP, 30, 70, 560, 315, kPreview);
        SetWindowSubclass(preview, PreviewProcedure, 1, reinterpret_cast<DWORD_PTR>(this));

        CreateControl(&timestampControls_, 0, L"STATIC", L"Положение", SS_LEFT, 610, 47, 310, 20);
        HWND position = CreateControl(&timestampControls_, WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      CBS_DROPDOWNLIST | WS_VSCROLL, 610, 70, 310, 180, kTextPosition);
        for (const wchar_t* item : { L"Верхний левый угол", L"Верхний правый угол",
                                     L"Нижний левый угол", L"Нижний правый угол" })
            SendMessageW(position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        CreateSlider(timestampControls_, L"Отступ по горизонтали", kTimestampMarginX, kMarginXValue, 610, 120, 310);
        CreateSlider(timestampControls_, L"Отступ по вертикали", kTimestampMarginY, kMarginYValue, 610, 172, 310);
        CreateSlider(timestampControls_, L"Подложка по горизонтали", kTimestampPaddingX, kPaddingXValue, 610, 224, 310);
        CreateSlider(timestampControls_, L"Подложка по вертикали", kTimestampPaddingY, kPaddingYValue, 610, 276, 310);
        CreateSlider(timestampControls_, L"Непрозрачность, %", kTimestampOpacity, kOpacityValue, 610, 328, 310);

        CreateControl(&timestampControls_, 0, L"STATIC", L"Шаблон:", SS_CENTERIMAGE, 30, 400, 60, 24);
        LOGFONTW linkFont{};
        GetObjectW(uiFont_, sizeof(linkFont), &linkFont);
        linkFont.lfUnderline = TRUE;
        linkFont_ = CreateFontIndirectW(&linkFont);
        int tokenX = 94;
        const int tokenWidths[] = { 70, 54, 54, 86, 56 };
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(kTemplateTokens)); ++i)
        {
            HWND link = CreateControl(&timestampControls_, 0, L"BUTTON", kTemplateTokens[i],
                BS_OWNERDRAW, tokenX, 400, tokenWidths[i], 24, kTemplateTokenFirst + i);
            SetWindowSubclass(link, TemplateLinkProcedure, 1, 0);
            tokenX += tokenWidths[i] + 8;
        }
        HWND templateEdit = CreateControl(&timestampControls_, WS_EX_CLIENTEDGE, L"EDIT", L"",
            ES_AUTOHSCROLL, 30, 430, 755, 28, kOverlayTemplate);
        SendMessageW(templateEdit, EM_SETLIMITTEXT, 2000, 0);
        CreateControl(&timestampControls_, 0, L"BUTTON", L"Шрифт", BS_PUSHBUTTON,
                      800, 429, 120, 30, kChooseFont);

        CreateControl(&timestampControls_, 0, L"BUTTON", L"Скрываемые области",
                      BS_GROUPBOX, 25, 478, 900, 104);
        CreateControl(&timestampControls_, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                      LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 40, 500, 550, 65, kMaskList);
        CreateControl(&timestampControls_, 0, L"BUTTON", L"Добавить", BS_PUSHBUTTON, 610, 500, 145, 30, kAddMask);
        CreateControl(&timestampControls_, 0, L"BUTTON", L"Удалить", BS_PUSHBUTTON, 770, 500, 145, 30, kDeleteMask);
        CreateControl(&timestampControls_, 0, L"BUTTON", L"Удалить все", BS_PUSHBUTTON, 610, 535, 305, 30, kClearMasks);

        CreateControl(&securityControls_, 0, L"BUTTON", L"Требовать имя пользователя и пароль для RTSP и ONVIF",
                      BS_AUTOCHECKBOX, 30, 60, 590, 25, kAuthentication);
        CreateLabeledEdit(securityControls_, L"Имя пользователя", kUserName, 105);
        CreateLabeledEdit(securityControls_, L"Пароль", kPassword, 145, ES_PASSWORD | ES_AUTOHSCROLL);
        CreateLabeledEdit(securityControls_, L"Разрешённые IP", kAllowedIps, 205);
        CreateControl(&securityControls_, 0, L"STATIC",
                      L"Укажите IP через запятую. Пустое поле разрешает все адреса. Локальная проверка 127.0.0.1 разрешена всегда.\n"
                      L"RTSP: Basic/Digest. ONVIF: Basic/Digest и UsernameToken.\n"
                      L"Для PasswordDigest часы компьютера и клиента должны быть синхронизированы.",
                      SS_LEFT, 30, 245, 870, 100);

        CreateControl(&statusControls_, WS_EX_CLIENTEDGE, L"STATIC", L"Запуск...",
                      SS_LEFT, 30, 60, 885, 415, kStatusText);
        CreateControl(&statusControls_, 0, L"STATIC",
                      L"Устройство определяется по IP. Несколько потоков с одного IP считаются одним устройством.\n"
                      L"Данные обновляются автоматически. Встроенная проверка RTSP в счётчики не включается.",
                      SS_LEFT, 30, 495, 885, 35);
        CreateControl(&statusControls_, 0, L"BUTTON", L"Вести журнал в файл", BS_AUTOCHECKBOX,
                      30, 539, 265, 25, kLoggingEnabled);
        CreateControl(&statusControls_, 0, L"STATIC", L"До 2 МБ. Применяется без перезапуска после сохранения.",
                      SS_CENTERIMAGE, 310, 539, 605, 25);

        CreateControl(nullptr, 0, L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON,
                      720, 602, 105, 30, kSave);
        CreateControl(nullptr, 0, L"BUTTON", L"Отмена", BS_PUSHBUTTON,
                      835, 602, 105, 30, kCancel);
        // The tab and group boxes are sibling backgrounds, not parents of their contents.
        // Keep them behind controls so WS_CLIPSIBLINGS excludes fields from background painting.
        for (const auto* group : { &deviceControls_, &timestampControls_ })
            for (HWND control : *group)
            {
                wchar_t name[32]{};
                GetClassNameW(control, name, ARRAYSIZE(name));
                if (wcscmp(name, L"Button") == 0 && (GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
                    SetWindowPos(control, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        SetWindowPos(tab_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PopulateControls();
        ShowSelectedTab();
    }

    void SetSliderRange(int id, int minimum, int maximum, int value)
    {
        HWND slider = GetDlgItem(window_, id);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
        SendMessageW(slider, TBM_SETPOS, TRUE, std::clamp(value, minimum, maximum));
    }

    void ShowSelectedTab()
    {
        const auto showGroup = [&](const std::vector<HWND>& group, bool visible)
        {
            for (HWND control : group) ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        };
        showGroup(deviceControls_, selectedTab_ == kDeviceTab);
        showGroup(videoControls_, selectedTab_ == kVideoTab);
        showGroup(timestampControls_, selectedTab_ == kOverlayTab);
        showGroup(securityControls_, selectedTab_ == kSecurityTab);
        showGroup(statusControls_, selectedTab_ == kStatusTab);
        UpdateSourceControls(false);
        if (selectedTab_ == kVideoTab) RefreshResolutionChoices(true, false);
        status_.previewRequested = selectedTab_ == kOverlayTab && IsWindowVisible(window_) && !IsIconic(window_);
        if (selectedTab_ == kOverlayTab)
        {
            PreparePreviewDimensions();
            const std::wstring currentCameraName = GetText(kCameraName);
            if (!currentCameraName.empty()) draftSettings_.cameraName = currentCameraName;
            InvalidateRect(GetDlgItem(window_, kPreview), nullptr, TRUE);
        }
        // All visibility/layout changes are finished; repaint the entire selected page,
        // not just the tab header or the fields whose visibility changed.
        if (IsWindowVisible(window_))
            RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void PopulateControls()
    {
        if (!window_) return;
        populatingControls_ = true;
        draftSettings_ = settings_;
        mainResolutions_.clear(); subResolutions_.clear();
        SetDlgItemTextW(window_, kCameraName, settings_.cameraName.c_str());
        SetDlgItemInt(window_, kOnvifPort, settings_.onvifPort, FALSE);
        SetDlgItemInt(window_, kRtspPort, settings_.rtspPort, FALSE);
        SetDlgItemTextW(window_, kRtspPath, settings_.rtspPath.c_str());
        SetDlgItemInt(window_, kFps, settings_.frameRate, FALSE);
        SetDlgItemInt(window_, kBitrate, settings_.bitrateKbps, FALSE);
        SetDlgItemInt(window_, kGop, settings_.gopSize, FALSE);
        CheckDlgButton(window_, kSubEnabled, settings_.subStreamEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(window_, kSubFps, settings_.subFrameRate, FALSE);
        SetDlgItemInt(window_, kSubBitrate, settings_.subBitrateKbps, FALSE);
        SetDlgItemInt(window_, kSubGop, settings_.subGopSize, FALSE);
        SetDlgItemTextW(window_, kSubPath, settings_.subRtspPath.c_str());
        UpdateSubStreamControls();
        SendDlgItemMessageW(window_, kVideoProfile, CB_SETCURSEL, settings_.videoProfile, 0);
        CheckDlgButton(window_, kAdaptiveFps, settings_.adaptiveFrameRate ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, kAdaptiveLoad, settings_.adaptiveLoad ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(window_, kMinimumFps, settings_.minimumFrameRate, FALSE);
        SendDlgItemMessageW(window_, kEncoderPreference, CB_SETCURSEL, settings_.encoderPreference, 0);
        CheckDlgButton(window_, kAllowSoftwareEncoder, settings_.allowSoftwareEncoder ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(window_, kCaptureMode, CB_SETCURSEL, settings_.captureMode, 0);
        PopulateMonitors(settings_.monitorIndex);
        SetDlgItemInt(window_, kRegionX, static_cast<UINT>(settings_.captureRegionX), TRUE);
        SetDlgItemInt(window_, kRegionY, static_cast<UINT>(settings_.captureRegionY), TRUE);
        SetDlgItemInt(window_, kRegionWidth, settings_.captureRegionWidth, FALSE);
        SetDlgItemInt(window_, kRegionHeight, settings_.captureRegionHeight, FALSE);
        SetDlgItemTextW(window_, kWindowTitle, settings_.captureWindowTitle.c_str());
        RefreshResolutionChoices(true);
        CheckDlgButton(window_, kShowCursor, settings_.showCursor ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, kHighlightMouseClicks, settings_.highlightMouseClicks ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(window_, kMouseClickSize, settings_.mouseClickHighlightSize, FALSE);
        SetDlgItemInt(window_, kMouseClickDuration, settings_.mouseClickHighlightDurationMs, FALSE);
        CheckDlgButton(window_, kStandbyEnabled, settings_.standbyEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(window_, kStandbyText, settings_.standbyText.c_str());
        masks_ = ParsePrivacyMasks(settings_.privacyMasks);
        selectedMask_ = -1; addingMask_ = false;
        maskReferenceWidth_ = settings_.outputWidth; maskReferenceHeight_ = settings_.outputHeight;
        RefreshMaskList();
        SendDlgItemMessageW(window_, kTextPosition, CB_SETCURSEL, settings_.timestampPosition, 0);
        SetDlgItemTextW(window_, kOverlayTemplate, settings_.overlayTemplate.c_str());
        CheckDlgButton(window_, kAuthentication, settings_.authenticationEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(window_, kUserName, settings_.userName.c_str());
        SetDlgItemTextW(window_, kPassword, settings_.password.c_str());
        SetDlgItemTextW(window_, kAllowedIps, settings_.allowedIpAddresses.c_str());
        CheckDlgButton(window_, kAutoStart, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, kLoggingEnabled, settings_.loggingEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetSliderRange(kTimestampMarginX, 0, settings_.outputWidth - 1, settings_.timestampMarginX);
        SetSliderRange(kTimestampMarginY, 0, settings_.outputHeight - 1, settings_.timestampMarginY);
        SetSliderRange(kTimestampPaddingX, 0, 100, settings_.timestampPaddingX);
        SetSliderRange(kTimestampPaddingY, 0, 100, settings_.timestampPaddingY);
        SetSliderRange(kTimestampOpacity, 0, 100, settings_.timestampBackgroundOpacity);
        UpdateTimestampFromSliders();
        UpdateRtspAddress();
        UpdateSourceControls();
        populatingControls_ = false;
    }

    void PopulateMonitors(uint32_t selectedMonitor)
    {
        HWND combo = GetDlgItem(window_, kMonitorIndex);
        monitorRectangles_.clear();
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        ComPtr<IDXGIFactory1> factory;
        UINT index = 0;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            for (UINT a = 0;; ++a)
            {
                ComPtr<IDXGIAdapter1> adapter;
                if (FAILED(factory->EnumAdapters1(a, &adapter))) break;
                for (UINT o = 0;; ++o)
                {
                    ComPtr<IDXGIOutput> output;
                    if (FAILED(adapter->EnumOutputs(o, &output))) break;
                    DXGI_OUTPUT_DESC desc{};
                    if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
                    monitorRectangles_.push_back(desc.DesktopCoordinates);
                    const std::wstring name = L"Монитор " + std::to_wstring(index + 1) + L" — " + desc.DeviceName +
                        L"  (" + std::to_wstring(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left) + L" × " +
                        std::to_wstring(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top) + L")";
                    const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                    SendMessageW(combo, CB_SETITEMDATA, item, index);
                    if (index == selectedMonitor) SendMessageW(combo, CB_SETCURSEL, item, 0);
                    ++index;
                }
            }
        }
        if (selectedMonitor >= index)
        {
            const std::wstring name = L"Монитор " + std::to_wstring(selectedMonitor + 1) + L" — сейчас не подключён";
            const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            SendMessageW(combo, CB_SETITEMDATA, item, selectedMonitor);
            SendMessageW(combo, CB_SETCURSEL, item, 0);
        }
    }

    void UpdateSourceControls(bool repaint = true)
    {
        const auto show = [](const std::vector<HWND>& controls, bool visible)
        { for (HWND control : controls) ShowWindow(control, visible ? SW_SHOW : SW_HIDE); };
        const LRESULT mode = SendDlgItemMessageW(window_, kCaptureMode, CB_GETCURSEL, 0, 0);
        show(regionControls_, selectedTab_ == kDeviceTab && mode == 1);
        show(windowControls_, selectedTab_ == kDeviceTab && mode == 2);
        show(clickControls_, selectedTab_ == kDeviceTab && IsDlgButtonChecked(window_, kHighlightMouseClicks) == BST_CHECKED);
        show(standbyControls_, selectedTab_ == kDeviceTab && IsDlgButtonChecked(window_, kStandbyEnabled) == BST_CHECKED);
        const wchar_t* hint = mode == 1
            ? L"Положение и размер прямоугольника задаются в пикселях выбранного монитора.\nНачало координат — его верхний левый угол."
            : mode == 2 ? L"Укажите узнаваемую часть названия окна. Захватывается его видимая область на выбранном мониторе.\nПерекрывающие окна тоже попадут в кадр."
            : mode == 3 ? L"Захватывается видимая область активного окна в пределах выбранного монитора.\nПри переключении приложений источник будет меняться."
            : L"В кадр попадает весь выбранный монитор.\nТекст и скрываемые области можно настроить на вкладке «Наложение».";
        SetDlgItemTextW(window_, kSourceHint, hint);
        const int hintY = mode == 1 ? 366 : mode == 2 ? 301 : 225;
        constexpr UINT layoutFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
        SetWindowPos(GetDlgItem(window_, kSourceHint), nullptr, 485, hintY, 420, 105, layoutFlags);
        // Erase both old and new positions, including tab/group-box backgrounds and all child controls.
        if (repaint && IsWindowVisible(window_))
            RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void RememberForegroundWindow()
    {
        HWND foreground = GetForegroundWindow();
        DWORD process = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &process);
        if (foreground && process != GetCurrentProcessId()) foregroundBeforeSettings_ = foreground;
    }

    VideoResolution CaptureSourceSize()
    {
        if (monitorRectangles_.empty()) return {};
        const LRESULT item = SendDlgItemMessageW(window_, kMonitorIndex, CB_GETCURSEL, 0, 0);
        const LRESULT index = SendDlgItemMessageW(window_, kMonitorIndex, CB_GETITEMDATA, item, 0);
        // DesktopCapture uses the first attached output if the saved monitor has disappeared.
        const RECT desktop = monitorRectangles_[index >= 0 && static_cast<size_t>(index) < monitorRectangles_.size() ? index : 0];
        AppSettings source = draftSettings_;
        const LRESULT mode = SendDlgItemMessageW(window_, kCaptureMode, CB_GETCURSEL, 0, 0);
        source.captureMode = mode >= 0 ? static_cast<uint32_t>(mode) : 0;
        source.captureWindowTitle = GetText(kWindowTitle);
        if (source.captureMode == 1)
        {
            BOOL xOk = FALSE, yOk = FALSE, wOk = FALSE, hOk = FALSE;
            source.captureRegionX = static_cast<int32_t>(GetDlgItemInt(window_, kRegionX, &xOk, TRUE));
            source.captureRegionY = static_cast<int32_t>(GetDlgItemInt(window_, kRegionY, &yOk, TRUE));
            source.captureRegionWidth = GetDlgItemInt(window_, kRegionWidth, &wOk, FALSE);
            source.captureRegionHeight = GetDlgItemInt(window_, kRegionHeight, &hOk, FALSE);
            if (!xOk || !yOk || !wOk || !hOk || source.captureRegionX < -32768 || source.captureRegionX > 32768 ||
                source.captureRegionY < -32768 || source.captureRegionY > 32768 || source.captureRegionWidth < 16 ||
                source.captureRegionWidth > 16384 || source.captureRegionHeight < 16 || source.captureRegionHeight > 16384)
                return {};
        }
        RememberForegroundWindow();
        const RECT crop = CaptureGeometry::Rectangle(source, desktop, foregroundBeforeSettings_);
        return { static_cast<uint32_t>(crop.right - crop.left), static_cast<uint32_t>(crop.bottom - crop.top) };
    }

    VideoResolution SelectedResolution(bool secondary) const
    {
        const auto& values = secondary ? subResolutions_ : mainResolutions_;
        const LRESULT item = SendDlgItemMessageW(window_, secondary ? kSubResolution : kResolution, CB_GETCURSEL, 0, 0);
        return item >= 0 && static_cast<size_t>(item) < values.size() ? values[item] : VideoResolution{};
    }

    void FillResolutions(int id, std::vector<VideoResolution>& choices, VideoResolution preferred, bool keepCurrent)
    {
        const bool legacy = keepCurrent && preferred.width >= 16 && preferred.height >= 16 &&
            std::find(choices.begin(), choices.end(), preferred) == choices.end();
        if (legacy) choices.push_back(preferred);
        HWND combo = GetDlgItem(window_, id);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        size_t selected = 0;
        uint64_t closest = UINT64_MAX;
        for (size_t i = 0; i < choices.size(); ++i)
        {
            const auto& size = choices[i];
            std::wstring label = std::to_wstring(size.width) + L" × " + std::to_wstring(size.height);
            if (legacy && i + 1 == choices.size()) label += L" — выбрано ранее";
            else if (id == kResolution && size.width == (resolutionSource_.width & ~1U)) label += L" — исходное";
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            const uint64_t distance = size.width > preferred.width ? size.width - preferred.width : preferred.width - size.width;
            if (size == preferred) { selected = i; closest = 0; }
            else if (distance < closest) { selected = i; closest = distance; }
        }
        if (!choices.empty()) SendMessageW(combo, CB_SETCURSEL, selected, 0);
    }

    void RefreshSubResolutions(bool keepCurrent)
    {
        auto preferred = SelectedResolution(true);
        if (!preferred.width) preferred = { draftSettings_.subWidth, draftSettings_.subHeight };
        const auto main = SelectedResolution(false);
        subResolutions_ = BuildStreamResolutions(resolutionSource_, true);
        subResolutions_.erase(std::remove_if(subResolutions_.begin(), subResolutions_.end(), [&](const VideoResolution& size)
            { return size.width > main.width || size.height > main.height; }), subResolutions_.end());
        FillResolutions(kSubResolution, subResolutions_, preferred,
                        keepCurrent && preferred.width <= main.width && preferred.height <= main.height);
    }

    void RefreshResolutionChoices(bool keepCurrent, bool force = true)
    {
        if (!force && (SendDlgItemMessageW(window_, kResolution, CB_GETDROPPEDSTATE, 0, 0) ||
                       SendDlgItemMessageW(window_, kSubResolution, CB_GETDROPPEDSTATE, 0, 0))) return;
        const auto source = CaptureSourceSize();
        if (!force && source == resolutionSource_) return;
        resolutionSource_ = source;
        auto preferred = SelectedResolution(false);
        if (!preferred.width) preferred = { draftSettings_.outputWidth, draftSettings_.outputHeight };
        mainResolutions_ = BuildStreamResolutions(source, false);
        FillResolutions(kResolution, mainResolutions_, preferred, keepCurrent);
        RefreshSubResolutions(keepCurrent);
        const std::wstring description = source.width && source.height
            ? L"Область захвата: " + std::to_wstring(source.width) + L" × " + std::to_wstring(source.height) +
                L" пикс. · размеры потоков с теми же пропорциями"
            : L"Размер области недоступен — проверьте источник на вкладке «Устройство».";
        SetDlgItemTextW(window_, kResolutionSource, description.c_str());
    }

    std::wstring RtspAddress(bool draft, bool secondary = false)
    {
        if (lanAddress_.empty())
        {
            const std::string ip = GetLocalIp();
            lanAddress_.assign(ip.begin(), ip.end());
        }
        return L"rtsp://" + lanAddress_ + L":" +
            (draft ? GetText(kRtspPort) : std::to_wstring(settings_.rtspPort)) + L"/" +
            (draft ? GetText(secondary ? kSubPath : kRtspPath) : secondary ? settings_.subRtspPath : settings_.rtspPath);
    }

    void UpdateRtspAddress()
    {
        SetDlgItemTextW(window_, kMainVideoAddress, RtspAddress(true).c_str());
        SetDlgItemTextW(window_, kSubVideoAddress, IsDlgButtonChecked(window_, kSubEnabled) == BST_CHECKED ?
                        RtspAddress(true, true).c_str() : L"Вторичный поток выключен");
    }

    void UpdateSubStreamControls()
    {
        const bool enabled = IsDlgButtonChecked(window_, kSubEnabled) == BST_CHECKED;
        for (int id : { kSubResolution, kSubFps, kSubBitrate, kSubGop, kSubPath })
            EnableWindow(GetDlgItem(window_, id), enabled);
        UpdateRtspAddress();
    }

    void UpdateMaskButtons()
    {
        SetDlgItemTextW(window_, kAddMask, addingMask_ ? L"Отменить" : L"Добавить");
        EnableWindow(GetDlgItem(window_, kAddMask), masks_.size() < 32);
        EnableWindow(GetDlgItem(window_, kDeleteMask), selectedMask_ >= 0);
        EnableWindow(GetDlgItem(window_, kClearMasks), !masks_.empty());
    }

    void RefreshMaskList()
    {
        SendDlgItemMessageW(window_, kMaskList, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < masks_.size(); ++i)
        {
            const auto& m = masks_[i];
            const std::wstring label = L"Область " + std::to_wstring(i + 1) + L"   " +
                std::to_wstring(m.width) + L" × " + std::to_wstring(m.height) + L" пикс.   (" +
                std::to_wstring(m.x) + L", " + std::to_wstring(m.y) + L")";
            SendDlgItemMessageW(window_, kMaskList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendDlgItemMessageW(window_, kMaskList, LB_SETCURSEL, selectedMask_, 0);
        UpdateMaskButtons();
        InvalidateRect(GetDlgItem(window_, kPreview), nullptr, FALSE);
    }

    void UpdateTimestampFromSliders()
    {
        if (!window_) return;
        draftSettings_.timestampMarginX = static_cast<uint32_t>(SendDlgItemMessageW(window_, kTimestampMarginX, TBM_GETPOS, 0, 0));
        draftSettings_.timestampMarginY = static_cast<uint32_t>(SendDlgItemMessageW(window_, kTimestampMarginY, TBM_GETPOS, 0, 0));
        draftSettings_.timestampPaddingX = static_cast<uint32_t>(SendDlgItemMessageW(window_, kTimestampPaddingX, TBM_GETPOS, 0, 0));
        draftSettings_.timestampPaddingY = static_cast<uint32_t>(SendDlgItemMessageW(window_, kTimestampPaddingY, TBM_GETPOS, 0, 0));
        draftSettings_.timestampBackgroundOpacity = static_cast<uint32_t>(SendDlgItemMessageW(window_, kTimestampOpacity, TBM_GETPOS, 0, 0));
        SetDlgItemInt(window_, kMarginXValue, draftSettings_.timestampMarginX, FALSE);
        SetDlgItemInt(window_, kMarginYValue, draftSettings_.timestampMarginY, FALSE);
        SetDlgItemInt(window_, kPaddingXValue, draftSettings_.timestampPaddingX, FALSE);
        SetDlgItemInt(window_, kPaddingYValue, draftSettings_.timestampPaddingY, FALSE);
        SetDlgItemInt(window_, kOpacityValue, draftSettings_.timestampBackgroundOpacity, FALSE);
        InvalidateRect(GetDlgItem(window_, kPreview), nullptr, TRUE);
    }


    static LRESULT CALLBACK TemplateLinkProcedure(HWND window, UINT message, WPARAM wParam,
                                                   LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_SETCURSOR)
        {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
            return TRUE;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TemplateLinkProcedure, 1);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void DrawTemplateLink(const DRAWITEMSTRUCT& draw)
    {
        FillRect(draw.hDC, &draw.rcItem, GetSysColorBrush(COLOR_BTNFACE));
        const HGDIOBJ previous = SelectObject(draw.hDC, linkFont_ ? linkFont_ : uiFont_);
        const int mode = SetBkMode(draw.hDC, TRANSPARENT);
        const COLORREF color = SetTextColor(draw.hDC, GetSysColor(COLOR_HOTLIGHT));
        RECT bounds = draw.rcItem;
        DrawTextW(draw.hDC, kTemplateTokens[draw.CtlID - kTemplateTokenFirst], -1, &bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if ((draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT)) DrawFocusRect(draw.hDC, &bounds);
        SetTextColor(draw.hDC, color);
        SetBkMode(draw.hDC, mode);
        SelectObject(draw.hDC, previous);
    }

    void InsertTemplateToken(unsigned index)
    {
        if (index >= ARRAYSIZE(kTemplateTokens)) return;
        HWND edit = GetDlgItem(window_, kOverlayTemplate);
        // Keep the edit's selection across focus loss; insert exactly at the caret.
        SetFocus(edit);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(kTemplateTokens[index]));
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    }

    void ChooseTimestampFont()
    {
        LOGFONTW font{};
        wcsncpy_s(font.lfFaceName, draftSettings_.timestampFontName.c_str(), LF_FACESIZE - 1);
        font.lfWeight = static_cast<LONG>(draftSettings_.timestampFontWeight);
        font.lfItalic = draftSettings_.timestampFontItalic ? TRUE : FALSE;
        HDC screen = GetDC(window_);
        const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(window_, screen);
        const int currentPoints = std::max(6, MulDiv(static_cast<int>(draftSettings_.timestampFontSize), 72, 96));
        font.lfHeight = -MulDiv(currentPoints, dpi, 72);

        CHOOSEFONTW choice{ sizeof(choice) };
        choice.hwndOwner = window_;
        choice.lpLogFont = &font;
        choice.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST |
                       CF_LIMITSIZE | CF_NOVERTFONTS;
        choice.nSizeMin = 6;
        choice.nSizeMax = 96;
        if (!ChooseFontW(&choice)) return;

        draftSettings_.timestampFontName = font.lfFaceName;
        draftSettings_.timestampFontWeight = std::clamp<uint32_t>(font.lfWeight, 100, 900);
        draftSettings_.timestampFontItalic = font.lfItalic != FALSE;
        draftSettings_.timestampFontSize = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::max(1, MulDiv(choice.iPointSize, 96, 720))), 8, 128);
        InvalidateRect(GetDlgItem(window_, kPreview), nullptr, TRUE);
    }


    void PreparePreviewDimensions()
    {
        const auto size = SelectedResolution(false);
        const UINT width = size.width, height = size.height;
        if (!width || !height) return;
        if (maskReferenceWidth_ != width || maskReferenceHeight_ != height)
        {
            RescalePrivacyMasks(masks_, maskReferenceWidth_, maskReferenceHeight_, width, height);
            maskReferenceWidth_ = width; maskReferenceHeight_ = height;
            RefreshMaskList();
        }
        draftSettings_.outputWidth = width; draftSettings_.outputHeight = height;
        SetSliderRange(kTimestampMarginX, 0, width - 1, draftSettings_.timestampMarginX);
        SetSliderRange(kTimestampMarginY, 0, height - 1, draftSettings_.timestampMarginY);
        UpdateTimestampFromSliders();
    }

    RECT PreviewImageRectangle(HWND preview) const
    {
        RECT client{}; GetClientRect(preview, &client);
        const float scale = std::min(static_cast<float>(client.right) / draftSettings_.outputWidth,
                                     static_cast<float>(client.bottom) / draftSettings_.outputHeight);
        const LONG width = std::max(1L, std::lround(draftSettings_.outputWidth * scale));
        const LONG height = std::max(1L, std::lround(draftSettings_.outputHeight * scale));
        const LONG left = (client.right - width) / 2, top = (client.bottom - height) / 2;
        return { left, top, left + width, top + height };
    }

    POINT PreviewVideoPoint(HWND preview, LPARAM position) const
    {
        const RECT rect = PreviewImageRectangle(preview);
        return {
            std::clamp<LONG>(MulDiv(GET_X_LPARAM(position) - rect.left, draftSettings_.outputWidth,
                                   rect.right - rect.left), 0, static_cast<LONG>(draftSettings_.outputWidth)),
            std::clamp<LONG>(MulDiv(GET_Y_LPARAM(position) - rect.top, draftSettings_.outputHeight,
                                   rect.bottom - rect.top), 0, static_cast<LONG>(draftSettings_.outputHeight)) };
    }

    static LRESULT CALLBACK PreviewProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR, DWORD_PTR reference)
    {
        auto* self = reinterpret_cast<TrayApplication*>(reference);
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(window, PreviewProcedure, 1);
            return DefSubclassProc(window, message, wParam, lParam);
        }
        if (message == WM_GETDLGCODE && (wParam == VK_ESCAPE || wParam == VK_DELETE))
            return DLGC_WANTALLKEYS;
        if (message == WM_SETCURSOR && self->addingMask_)
        {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32515)));
            return TRUE;
        }
        if (message == WM_LBUTTONDOWN)
        {
            SetFocus(window);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            const RECT rect = self->PreviewImageRectangle(window);
            POINT mouse{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!PtInRect(&rect, mouse)) return 0;
            const POINT point = self->PreviewVideoPoint(window, lParam);
            self->dragStart_ = point;
            self->textSelected_ = false;
            const float tolerance = 5.0f * self->draftSettings_.outputWidth / std::max(1L, rect.right - rect.left);
            if (!self->addingMask_ && self->textBoundsValid_ &&
                point.x >= self->textBounds_.left - tolerance && point.x <= self->textBounds_.right + tolerance &&
                point.y >= self->textBounds_.top - tolerance && point.y <= self->textBounds_.bottom + tolerance)
            {
                self->draggingText_ = true; self->textSelected_ = true;
                self->originalTextBounds_ = self->textBounds_;
                self->originalMarginX_ = self->draftSettings_.timestampMarginX;
                self->originalMarginY_ = self->draftSettings_.timestampMarginY;
                self->selectedMask_ = -1;
                self->RefreshMaskList();
                SetCapture(window);
                return 0;
            }
            if (self->addingMask_ && self->masks_.size() < 32)
            {
                self->draggingMask_ = true;
                self->newMaskDrag_ = true;
                self->dragMask_ = { point.x, point.y, 0, 0 };
            }
            else
            {
                self->selectedMask_ = -1;
                for (int i = static_cast<int>(self->masks_.size()) - 1; i >= 0; --i)
                {
                    const auto& mask = self->masks_[i];
                    if (point.x >= mask.x && point.x < mask.x + mask.width &&
                        point.y >= mask.y && point.y < mask.y + mask.height)
                    {
                        self->selectedMask_ = i;
                        self->originalMask_ = mask;
                        self->draggingMask_ = true;
                        self->newMaskDrag_ = false;
                        break;
                    }
                }
                self->RefreshMaskList();
            }
            if (self->draggingMask_) SetCapture(window);
            return 0;
        }
        if (message == WM_MOUSEMOVE && self->draggingText_)
        {
            const POINT point = self->PreviewVideoPoint(window, lParam);
            const float width = self->originalTextBounds_.right - self->originalTextBounds_.left;
            const float height = self->originalTextBounds_.bottom - self->originalTextBounds_.top;
            const float maxX = std::max(0.0f, self->draftSettings_.outputWidth - width);
            const float maxY = std::max(0.0f, self->draftSettings_.outputHeight - height);
            const float x = std::clamp(self->originalTextBounds_.left + point.x - self->dragStart_.x, 0.0f, maxX);
            const float y = std::clamp(self->originalTextBounds_.top + point.y - self->dragStart_.y, 0.0f, maxY);
            const bool right = self->draftSettings_.timestampPosition == 1 || self->draftSettings_.timestampPosition == 3;
            const bool bottom = self->draftSettings_.timestampPosition >= 2;
            SendDlgItemMessageW(self->window_, kTimestampMarginX, TBM_SETPOS, TRUE, std::lround(right ? maxX - x : x));
            SendDlgItemMessageW(self->window_, kTimestampMarginY, TBM_SETPOS, TRUE, std::lround(bottom ? maxY - y : y));
            self->UpdateTimestampFromSliders();
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32646)));
            return 0;
        }
        if (message == WM_LBUTTONUP && self->draggingText_)
        {
            self->draggingText_ = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_MOUSEMOVE && self->draggingMask_)
        {
            const POINT point = self->PreviewVideoPoint(window, lParam);
            if (self->newMaskDrag_)
            {
                self->dragMask_ = { std::min(point.x, self->dragStart_.x), std::min(point.y, self->dragStart_.y),
                                    std::abs(point.x - self->dragStart_.x), std::abs(point.y - self->dragStart_.y) };
            }
            else if (self->selectedMask_ >= 0)
            {
                auto& mask = self->masks_[self->selectedMask_];
                mask.x = std::clamp<int32_t>(self->originalMask_.x + point.x - self->dragStart_.x, 0,
                    std::max(0, static_cast<int>(self->draftSettings_.outputWidth) - mask.width));
                mask.y = std::clamp<int32_t>(self->originalMask_.y + point.y - self->dragStart_.y, 0,
                    std::max(0, static_cast<int>(self->draftSettings_.outputHeight) - mask.height));
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_LBUTTONUP && self->draggingMask_)
        {
            if (self->newMaskDrag_ && self->dragMask_.width > 0 && self->dragMask_.height > 0)
            {
                self->masks_.push_back(self->dragMask_);
                self->selectedMask_ = static_cast<int>(self->masks_.size()) - 1;
                self->addingMask_ = false;
            }
            self->draggingMask_ = false;
            ReleaseCapture();
            self->RefreshMaskList();
            return 0;
        }
        if (message == WM_CAPTURECHANGED || (message == WM_KEYDOWN && wParam == VK_ESCAPE))
        {
            if (self->draggingText_)
            {
                SendDlgItemMessageW(self->window_, kTimestampMarginX, TBM_SETPOS, TRUE, self->originalMarginX_);
                SendDlgItemMessageW(self->window_, kTimestampMarginY, TBM_SETPOS, TRUE, self->originalMarginY_);
                self->UpdateTimestampFromSliders();
            }
            self->draggingText_ = false;
            if (self->draggingMask_ && !self->newMaskDrag_ && self->selectedMask_ >= 0)
                self->masks_[self->selectedMask_] = self->originalMask_;
            self->draggingMask_ = false; self->addingMask_ = false;
            if (GetCapture() == window) ReleaseCapture();
            self->RefreshMaskList();
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_DELETE && !self->draggingMask_)
        {
            SendMessageW(self->window_, WM_COMMAND, kDeleteMask, 0);
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void DrawTimestampPreview(const DRAWITEMSTRUCT& draw)
    {
        textBoundsValid_ = false;
        // Use the same DirectWrite units/metrics as the encoder and scale the entire video to fit.
        if (!previewFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                                        previewFactory_.GetAddressOf()))) return;
        if (!previewWriteFactory_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(previewWriteFactory_.GetAddressOf())))) return;
        if (!previewTarget_)
        {
            const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            if (FAILED(previewFactory_->CreateDCRenderTarget(&properties, &previewTarget_))) return;
        }
        if (FAILED(previewTarget_->BindDC(draw.hDC, &draw.rcItem))) return;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(previewTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f), &brush))) return;
        previewTarget_->BeginDraw();
        previewTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        previewTarget_->Clear(D2D1::ColorF(0.12f, 0.14f, 0.17f));
        const RECT rect = PreviewImageRectangle(draw.hwndItem);
        const D2D1_RECT_F imageRect = D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                                                static_cast<float>(rect.right), static_cast<float>(rect.bottom));
        bool hasFrame = false, standby = false;
        uint64_t frameTick = 0;
        {
            std::lock_guard<std::mutex> lock(status_.previewMutex);
            standby = status_.previewStandby; frameTick = status_.previewTickMs;
            if (status_.previewWidth && status_.previewHeight && frameTick &&
                status_.previewPixels.size() == static_cast<size_t>(status_.previewWidth) * status_.previewHeight * 4)
            {
                ComPtr<ID2D1Bitmap> frame;
                const auto properties = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                if (SUCCEEDED(previewTarget_->CreateBitmap(D2D1::SizeU(status_.previewWidth, status_.previewHeight),
                    status_.previewPixels.data(), status_.previewWidth * 4, properties, &frame)))
                {
                    previewTarget_->DrawBitmap(frame.Get(), imageRect);
                    hasFrame = true;
                }
            }
        }
        const float scaleX = static_cast<float>(rect.right - rect.left) / draftSettings_.outputWidth;
        const float scaleY = static_cast<float>(rect.bottom - rect.top) / draftSettings_.outputHeight;
        if (hasFrame)
        {
            previewTarget_->SetTransform(D2D1::Matrix3x2F::Scale(scaleX, scaleY) *
                D2D1::Matrix3x2F::Translation(static_cast<float>(rect.left), static_cast<float>(rect.top)));
            previewTarget_->PushAxisAlignedClip(D2D1::RectF(0, 0, static_cast<float>(draftSettings_.outputWidth),
                static_cast<float>(draftSettings_.outputHeight)), D2D1_ANTIALIAS_MODE_ALIASED);
            const auto maskRect = [](const PrivacyMask& m)
            { return D2D1::RectF(static_cast<float>(m.x), static_cast<float>(m.y),
                                static_cast<float>(m.x + m.width), static_cast<float>(m.y + m.height)); };
            for (const auto& mask : masks_) previewTarget_->FillRectangle(maskRect(mask), brush.Get());
            if (!BuildOverlayText(draftSettings_).empty())
            {
                AppSettings textSettings = draftSettings_;
                textSettings.overlayTemplate = GetText(kOverlayTemplate);
                const std::wstring text = BuildOverlayText(textSettings);
                ComPtr<IDWriteTextFormat> format;
                ComPtr<IDWriteTextLayout> layout;
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(previewWriteFactory_->CreateTextFormat(textSettings.timestampFontName.c_str(), nullptr,
                    static_cast<DWRITE_FONT_WEIGHT>(textSettings.timestampFontWeight),
                    textSettings.timestampFontItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(textSettings.timestampFontSize), L"ru-RU", &format)) &&
                    SUCCEEDED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) &&
                    SUCCEEDED(previewWriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                        format.Get(), std::max(1.0f, static_cast<float>(textSettings.outputWidth) -
                            2.0f * (textSettings.timestampMarginX + textSettings.timestampPaddingX)),
                        std::max(1.0f, static_cast<float>(textSettings.outputHeight) -
                            2.0f * (textSettings.timestampMarginY + textSettings.timestampPaddingY)), &layout)) &&
                    SUCCEEDED(layout->GetMetrics(&metrics)))
                {
                    const float px = static_cast<float>(textSettings.timestampPaddingX);
                    const float py = static_cast<float>(textSettings.timestampPaddingY);
                    const float width = metrics.widthIncludingTrailingWhitespace + 2 * px;
                    const float height = metrics.height + 2 * py;
                    const bool right = textSettings.timestampPosition == 1 || textSettings.timestampPosition == 3;
                    const bool bottom = textSettings.timestampPosition >= 2;
                    const float x = right ? std::max(0.0f, textSettings.outputWidth -
                        static_cast<float>(textSettings.timestampMarginX) - width) : static_cast<float>(textSettings.timestampMarginX);
                    const float y = bottom ? std::max(0.0f, textSettings.outputHeight -
                        static_cast<float>(textSettings.timestampMarginY) - height) : static_cast<float>(textSettings.timestampMarginY);
                    textBounds_ = D2D1::RectF(x, y, x + width, y + height);
                    textBoundsValid_ = !text.empty();
                    brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, textSettings.timestampBackgroundOpacity / 100.0f));
                    previewTarget_->FillRectangle(D2D1::RectF(x, y, x + width, y + height), brush.Get());
                    brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                    previewTarget_->DrawTextLayout(D2D1::Point2F(x + px, y + py), layout.Get(), brush.Get());
                    if (textSelected_)
                    {
                        brush->SetColor(D2D1::ColorF(1.0f, 0.75f, 0.1f));
                        previewTarget_->DrawRectangle(textBounds_, brush.Get(), 1.0f / scaleX);
                    }
                }
            }
            brush->SetColor(D2D1::ColorF(0.1f, 0.8f, 1.0f));
            if (selectedMask_ >= 0 && selectedMask_ < static_cast<int>(masks_.size()))
                previewTarget_->DrawRectangle(maskRect(masks_[selectedMask_]), brush.Get(), 1.5f / scaleX);
            if (draggingMask_ && newMaskDrag_)
            {
                brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f));
                previewTarget_->FillRectangle(maskRect(dragMask_), brush.Get());
                brush->SetColor(D2D1::ColorF(0.1f, 0.8f, 1.0f));
                previewTarget_->DrawRectangle(maskRect(dragMask_), brush.Get(), 1.5f / scaleX);
            }
            previewTarget_->PopAxisAlignedClip();
            previewTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        const bool stale = frameTick != 0 && GetTickCount64() - frameTick > 3000;
        const wchar_t* message = standby ? L"Экран недоступен — передаётся заставка" :
            status_.streamingPaused.load() ? L"Трансляция приостановлена" :
            status_.finished.load() ? L"Захват остановлен" :
            !hasFrame ? L"Ожидание кадра текущей трансляции…" :
            stale ? L"Кадр не обновляется" : L"Живой захват · текст и маски — черновик";
        ComPtr<IDWriteTextFormat> statusFormat;
        if (SUCCEEDED(previewWriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"ru-RU", &statusFormat)))
        {
            brush->SetColor(D2D1::ColorF(0.85f, 0.89f, 0.94f));
            const float bottom = rect.top >= 24 ? static_cast<float>(rect.top) : 28.0f;
            // Status belongs to the square's letterbox, never to the transmitted picture.
            if (!hasFrame || rect.top >= 24)
                previewTarget_->DrawTextW(message, static_cast<UINT32>(wcslen(message)), statusFormat.Get(),
                    D2D1::RectF(8, 6, static_cast<float>(draw.rcItem.right - 8), bottom), brush.Get());
        }
        if (FAILED(previewTarget_->EndDraw())) previewTarget_.Reset();
    }

    HICON CreateStatusIcon(COLORREF color)
    {
        const int size = GetSystemMetrics(SM_CXSMICON);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP colorBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!dc || !colorBitmap)
        {
            if (dc) DeleteDC(dc);
            if (colorBitmap) DeleteObject(colorBitmap);
            return CopyIcon(LoadApplicationIcon(size, size));
        }
        HGDIOBJ oldBitmap = SelectObject(dc, colorBitmap);
        DrawIconEx(dc, 0, 0, LoadApplicationIcon(size, size), size, size, 0, nullptr, DI_NORMAL);
        auto* pixels = static_cast<uint8_t*>(bits);
        for (int index = 0; index < size * size; ++index)
        {
            uint8_t* pixel = pixels + index * 4;
            const uint8_t alpha = pixel[3];
            if (alpha == 0) continue;
            const float luminance = (0.114f * pixel[0] + 0.587f * pixel[1] + 0.299f * pixel[2]) / 255.0f;
            const float brightness = 0.62f + 0.38f * luminance;
            pixel[0] = static_cast<uint8_t>(GetBValue(color) * brightness * alpha / 255.0f);
            pixel[1] = static_cast<uint8_t>(GetGValue(color) * brightness * alpha / 255.0f);
            pixel[2] = static_cast<uint8_t>(GetRValue(color) * brightness * alpha / 255.0f);
        }
        SelectObject(dc, oldBitmap); DeleteDC(dc);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        if (!mask) { DeleteObject(colorBitmap); return CopyIcon(LoadApplicationIcon(size, size)); }
        ICONINFO iconInfo{}; iconInfo.fIcon = TRUE; iconInfo.hbmColor = colorBitmap; iconInfo.hbmMask = mask;
        HICON icon = CreateIconIndirect(&iconInfo);
        DeleteObject(colorBitmap); DeleteObject(mask);
        return icon;
    }

    void UpdateStatusDisplay()
    {
        if (!window_) return;
        const uint64_t now = GetTickCount64();
        const uint64_t started = status_.startTickMs.load();
        const uint64_t uptimeSeconds = now >= started ? (now - started) / 1000 : 0;
        const auto phase = static_cast<PipelinePhase>(status_.phase.load());
        const bool paused = status_.streamingPaused.load();
        std::wstring phaseText = paused ? L"Остановлена пользователем" :
            phase == PipelinePhase::starting ? L"Запуск" :
            phase == PipelinePhase::servicesReady ? L"RTSP и ONVIF запущены" :
            phase == PipelinePhase::captureReady ? L"Захват экрана запущен" :
            phase == PipelinePhase::encoderReady ? L"Ожидание первого H.264-кадра" : L"Трансляция работает";
        std::wstring adapter, monitor, encoder, encoderKind, source, error;
        uint32_t devices = 0, mainSessions = 0, subSessions = 0;
        {
            std::lock_guard<std::mutex> lock(status_.mutex);
            adapter = status_.adapterName; monitor = status_.monitorName;
            encoder = status_.encoderName; encoderKind = status_.encoderKind;
            source = status_.sourceDescription; error = status_.error;
            devices = status_.connectedDevices; mainSessions = status_.mainRtspSessions; subSessions = status_.subRtspSessions;
        }
        const uint64_t bytes = status_.publishedBytes.load();
        const std::wstring text =
            L"Состояние: " + phaseText + L"\r\n"
            L"Время работы: " + std::to_wstring(uptimeSeconds / 3600) + L" ч " +
                std::to_wstring((uptimeSeconds / 60) % 60) + L" мин\r\n\r\n"
            L"Источник: " + source + L"\r\nМонитор: " + monitor + L"\r\nGPU: " + adapter +
            L"\r\nКодировщик: " + encoder + L" — " + encoderKind + L"\r\n\r\n"
            L"Фактический FPS: " + std::to_wstring(status_.effectiveFrameRate.load()) +
            L"\r\nБитрейт: " + std::to_wstring(status_.effectiveBitrateKbps.load()) + L" Кбит/с\r\n"
            L"Опубликовано кадров: " + std::to_wstring(status_.publishedFrames.load()) +
            L"\r\nПередано H.264: " + std::to_wstring(bytes / 1024) + L" КиБ\r\n"
            L"Подключено устройств (по IP): " + std::to_wstring(devices) +
            L"\r\nRTSP-сессии основного потока: " + std::to_wstring(mainSessions) +
            L"\r\nRTSP-сессии вторичного потока: " + (status_.subStreamEnabled ? std::to_wstring(subSessions) : std::wstring(L"выключен")) +
            L"\r\nЛокальная проверка RTSP: " +
                (status_.rtspProbeHeartbeatMs.load() ? L"успешно" : L"ожидание") +
            L"\r\nВторичный поток: " + (status_.subStreamEnabled ?
                std::to_wstring(status_.subPublishedFrames.load()) + L" кадров; RTSP: " +
                    (status_.subRtspProbeHeartbeatMs.load() ? L"успешно" : L"ожидание") : std::wstring(L"выключен")) +
            (error.empty() ? L"" : L"\r\n\r\nПоследняя ошибка: " + error);
        SetDlgItemTextW(window_, kStatusText, text.c_str());

        const COLORREF color = paused ? RGB(128, 128, 128) : status_.finished.load() ? RGB(210, 40, 40) :
            phase == PipelinePhase::streaming ? RGB(20, 180, 70) : RGB(230, 170, 20);
        if (color != statusColor_)
        {
            statusColor_ = color;
            if (statusIcon_) DestroyIcon(statusIcon_);
            statusIcon_ = CreateStatusIcon(color);
            tray_.uFlags = NIF_ICON;
            tray_.hIcon = statusIcon_;
            Shell_NotifyIconW(NIM_MODIFY, &tray_);
        }
        const std::wstring tip = L"Screen2NVR — " + phaseText + L", FPS " +
                                 std::to_wstring(status_.effectiveFrameRate.load());
        tray_.uFlags = NIF_TIP;
        wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void CopyRtspAddress()
    {
        const std::wstring address = RtspAddress(false);
        if (!OpenClipboard(window_)) return;
        EmptyClipboard();
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (address.size() + 1) * sizeof(wchar_t));
        if (memory)
        {
            void* data = GlobalLock(memory);
            if (data)
            {
                memcpy(data, address.c_str(), (address.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            }
            else GlobalFree(memory);
        }
        CloseClipboard();
    }

    void RestartApplication()
    {
        std::vector<wchar_t> executable(32768);
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        const std::wstring parameters = L"--restart-wait " + std::to_wstring(GetCurrentProcessId());
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.data(), parameters.c_str(),
                                                    nullptr, SW_SHOWNORMAL)) > 32)
            ExitApplication();
    }



    void AddTrayIcon()
    {
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = LoadApplicationIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        wcscpy_s(tray_.szTip, L"Screen2NVR — трансляция экрана");
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void RemoveTrayIcon()
    {
        if (tray_.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_.hWnd = nullptr;
        if (statusIcon_) { DestroyIcon(statusIcon_); statusIcon_ = nullptr; }
    }

    void ShowTrayMenu()
    {
        RememberForegroundWindow();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuToggleStream,
                    status_.streamingPaused.load() ? L"Запустить трансляцию" : L"Остановить трансляцию");
        AppendMenuW(menu, MF_STRING, kMenuRestart, L"Перезапустить трансляцию");
        AppendMenuW(menu, MF_STRING, kMenuCopyRtsp, L"Скопировать RTSP-адрес");
        AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"Открыть папку журналов");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"Настройки...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Выход");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                       point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void ShowSettings()
    {
        RememberForegroundWindow();
        lanAddress_.clear();
        PopulateControls();
        TabCtrl_SetCurSel(tab_, selectedTab_);
        ShowSelectedTab();
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
    }

    std::wstring GetText(int id) const
    {
        const int length = GetWindowTextLengthW(GetDlgItem(window_, id));
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetDlgItemTextW(window_, id, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
        return text;
    }

    bool ReadNumber(int id, uint32_t minimum, uint32_t maximum, uint32_t& result,
                    const wchar_t* fieldName)
    {
        BOOL valid = FALSE;
        const UINT value = GetDlgItemInt(window_, id, &valid, FALSE);
        if (!valid || value < minimum || value > maximum)
        {
            const std::wstring message = std::wstring(L"Недопустимое значение поля «") + fieldName + L"».";
            MessageBoxW(window_, message.c_str(), L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(window_, id));
            return false;
        }
        result = value;
        return true;
    }

    bool ReadSignedNumber(int id, int32_t minimum, int32_t maximum, int32_t& result,
                          const wchar_t* fieldName)
    {
        const std::wstring text = GetText(id);
        try
        {
            size_t consumed = 0;
            const int value = std::stoi(text, &consumed);
            if (consumed != text.size() || value < minimum || value > maximum) throw std::out_of_range("range");
            result = value;
            return true;
        }
        catch (...)
        {
            const std::wstring message = std::wstring(L"Недопустимое значение поля «") + fieldName + L"».";
            MessageBoxW(window_, message.c_str(), L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(window_, id));
            return false;
        }
    }

    void SaveFromControls()
    {
        // Enter can save while a source edit still owns focus (no EN_KILLFOCUS yet).
        RefreshResolutionChoices(false, false);
        PreparePreviewDimensions();
        AppSettings updated = draftSettings_;
        updated.cameraName = GetText(kCameraName);
        updated.rtspPath = GetText(kRtspPath);
        updated.captureWindowTitle = GetText(kWindowTitle);
        updated.standbyText = GetText(kStandbyText);
        updated.privacyMasks = SerializePrivacyMasks(masks_);
        updated.overlayTemplate = GetText(kOverlayTemplate);
        updated.userName = GetText(kUserName);
        updated.password = GetText(kPassword);
        updated.allowedIpAddresses = GetText(kAllowedIps);
        if (!Security::NormalizeIpList(updated.allowedIpAddresses, updated.allowedIpAddresses))
        {
            MessageBoxW(window_, L"Укажите корректные IPv4-адреса через запятую, например: 192.168.1.10, 192.168.1.20.",
                        L"Разрешённые IP", MB_OK | MB_ICONWARNING);
            return;
        }
        if (updated.userName.find_first_of(L":\r\n") != std::wstring::npos)
        {
            MessageBoxW(window_, L"Имя пользователя не должно содержать двоеточие или перевод строки.",
                        L"Имя пользователя", MB_OK | MB_ICONWARNING);
            return;
        }
        uint32_t value = 0;
        if (!ReadNumber(kOnvifPort, 1, 65535, value, L"Порт ONVIF")) return;
        updated.onvifPort = static_cast<uint16_t>(value);
        if (!ReadNumber(kRtspPort, 1, 65535, value, L"Порт RTSP")) return;
        updated.rtspPort = static_cast<uint16_t>(value);
        const auto mainSize = SelectedResolution(false), subSize = SelectedResolution(true);
        if (!mainSize.width || !mainSize.height)
        {
            MessageBoxW(window_, L"Для области захвата нет подходящего разрешения. Проверьте её размеры и положение.",
                        L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            return;
        }
        updated.outputWidth = mainSize.width; updated.outputHeight = mainSize.height;
        if (!ReadNumber(kFps, 1, 60, updated.frameRate, L"Кадров в секунду")) return;
        if (!ReadNumber(kBitrate, 128, 50000, updated.bitrateKbps, L"Битрейт")) return;
        if (!ReadNumber(kGop, 1, 600, updated.gopSize, L"Интервал ключевых кадров")) return;
        updated.subStreamEnabled = IsDlgButtonChecked(window_, kSubEnabled) == BST_CHECKED;
        if (updated.subStreamEnabled)
        {
            if (!subSize.width || !subSize.height || subSize.width > mainSize.width || subSize.height > mainSize.height)
            {
                MessageBoxW(window_, L"Выберите разрешение вторичного потока не больше основного.",
                            L"Вторичный поток", MB_OK | MB_ICONWARNING);
                return;
            }
            updated.subWidth = subSize.width; updated.subHeight = subSize.height;
            if (!ReadNumber(kSubFps, 1, updated.frameRate, updated.subFrameRate, L"FPS вторичного потока")) return;
            if (!ReadNumber(kSubBitrate, 64, 50000, updated.subBitrateKbps, L"Битрейт вторичного потока")) return;
            if (!ReadNumber(kSubGop, 1, 600, updated.subGopSize, L"Интервал ключевых кадров вторичного потока")) return;
            updated.subRtspPath = GetText(kSubPath);
            if ((updated.subWidth & 1U) || (updated.subHeight & 1U) || !IsValidRtspPath(updated.subRtspPath) ||
                updated.subRtspPath == updated.rtspPath)
            {
                MessageBoxW(window_, L"Размеры вторичного потока должны быть чётными. Путь RTSP должен отличаться от основного "
                            L"и задаваться без начального /, пробелов, \\, ? или #. Пример: Streaming/Channels/102.",
                            L"Вторичный поток", MB_OK | MB_ICONWARNING);
                return;
            }
        }
        else
        {
            if (subSize.width && subSize.height) { updated.subWidth = subSize.width; updated.subHeight = subSize.height; }
            NormalizeSubStreamSettings(updated);
        }
        if (!ReadNumber(kMinimumFps, 1, 30, updated.minimumFrameRate, L"Минимальный FPS")) return;
        updated.minimumFrameRate = std::min(updated.minimumFrameRate, updated.frameRate);
        const LRESULT captureMode = SendDlgItemMessageW(window_, kCaptureMode, CB_GETCURSEL, 0, 0);
        updated.captureMode = captureMode >= 0 ? static_cast<uint32_t>(captureMode) : 0;
        const LRESULT monitor = SendDlgItemMessageW(window_, kMonitorIndex, CB_GETCURSEL, 0, 0);
        if (monitor >= 0)
            updated.monitorIndex = static_cast<uint32_t>(SendDlgItemMessageW(window_, kMonitorIndex, CB_GETITEMDATA, monitor, 0));
        if (updated.captureMode == 1)
        {
            if (!ReadSignedNumber(kRegionX, -32768, 32768, updated.captureRegionX, L"Слева, пикс.")) return;
            if (!ReadSignedNumber(kRegionY, -32768, 32768, updated.captureRegionY, L"Сверху, пикс.")) return;
            if (!ReadNumber(kRegionWidth, 16, 16384, updated.captureRegionWidth, L"Ширина области")) return;
            if (!ReadNumber(kRegionHeight, 16, 16384, updated.captureRegionHeight, L"Высота области")) return;
        }
        if (IsDlgButtonChecked(window_, kHighlightMouseClicks) == BST_CHECKED)
        {
            if (!ReadNumber(kMouseClickSize, 32, 320, updated.mouseClickHighlightSize, L"Размер подсветки нажатия")) return;
            if (!ReadNumber(kMouseClickDuration, 200, 3000, updated.mouseClickHighlightDurationMs, L"Время затухания нажатия")) return;
        }
        if (updated.timestampMarginX + updated.timestampPaddingX * 2 >= updated.outputWidth ||
            updated.timestampMarginY + updated.timestampPaddingY * 2 >= updated.outputHeight)
        {
            MessageBoxW(window_, L"Отступы текста слишком велики для выбранного разрешения.",
                        L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            return;
        }
        if (updated.cameraName.empty() || !IsValidRtspPath(updated.rtspPath))
        {
            MessageBoxW(window_, L"Укажите имя камеры и путь RTSP без начального /, пробелов, \\, ? или #. "
                        L"Пример пути: Streaming/Channels/101.",
                        L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            return;
        }
        const LRESULT selectedPosition = SendDlgItemMessageW(window_, kTextPosition, CB_GETCURSEL, 0, 0);
        updated.timestampPosition = selectedPosition >= 0 ? static_cast<uint32_t>(selectedPosition) : 0;
        const LRESULT selectedProfile = SendDlgItemMessageW(window_, kVideoProfile, CB_GETCURSEL, 0, 0);
        updated.videoProfile = selectedProfile >= 0 ? static_cast<uint32_t>(selectedProfile) : 0;
        const LRESULT selectedEncoder = SendDlgItemMessageW(window_, kEncoderPreference, CB_GETCURSEL, 0, 0);
        updated.encoderPreference = selectedEncoder >= 0 ? static_cast<uint32_t>(selectedEncoder) : 0;
        updated.adaptiveFrameRate = IsDlgButtonChecked(window_, kAdaptiveFps) == BST_CHECKED;
        updated.adaptiveLoad = IsDlgButtonChecked(window_, kAdaptiveLoad) == BST_CHECKED;
        updated.allowSoftwareEncoder = IsDlgButtonChecked(window_, kAllowSoftwareEncoder) == BST_CHECKED;
        updated.showCursor = IsDlgButtonChecked(window_, kShowCursor) == BST_CHECKED;
        updated.highlightMouseClicks = IsDlgButtonChecked(window_, kHighlightMouseClicks) == BST_CHECKED;
        updated.standbyEnabled = IsDlgButtonChecked(window_, kStandbyEnabled) == BST_CHECKED;
        updated.authenticationEnabled = IsDlgButtonChecked(window_, kAuthentication) == BST_CHECKED;
        updated.autoStart = IsDlgButtonChecked(window_, kAutoStart) == BST_CHECKED;
        updated.loggingEnabled = IsDlgButtonChecked(window_, kLoggingEnabled) == BST_CHECKED;
        if ((updated.captureMode == 2 && updated.captureWindowTitle.empty()) ||
            (updated.authenticationEnabled && (updated.userName.empty() || updated.password.empty())))
        {
            MessageBoxW(window_, L"Для выбранного режима заполните заголовок окна и данные авторизации.",
                        L"Настройки Screen2NVR", MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring error;
        if (!(status_.settingsSaver ? status_.settingsSaver(updated, error) : SaveAppSettings(updated, error)))
        {
            if (status_.logger) status_.logger("Settings save failed: " + WideToUtf8String(error));
            MessageBoxW(window_, error.c_str(), L"Ошибка сохранения", MB_OK | MB_ICONERROR);
            return;
        }
        settings_ = updated;
        draftSettings_ = updated;
        if (status_.configureLogging && !status_.configureLogging(updated.loggingEnabled, error))
            MessageBoxW(window_, (L"Настройки сохранены, но запись журнала недоступна.\n\n" + error).c_str(),
                        L"Журнал Screen2NVR", MB_OK | MB_ICONWARNING);
        const bool restartNeeded = SettingsRequireRestart(activeSettings_, updated);
        status_.QueueLiveSettings(updated);
        CopyLiveSettings(activeSettings_, updated);
        ShowWindow(window_, SW_HIDE);
        if (restartNeeded && MessageBoxW(window_, L"Настройки сохранены. Для изменения источника, параметров видео, "
                        L"камеры или сети требуется перезапуск. Перезапустить Screen2NVR сейчас?",
                        L"Screen2NVR", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        {
            std::vector<wchar_t> executable(32768);
            GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            const std::wstring parameters = L"--restart-wait " + std::to_wstring(GetCurrentProcessId());
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.data(), parameters.c_str(),
                                                        nullptr, SW_SHOWNORMAL)) <= 32)
            {
                MessageBoxW(window_, L"Не удалось запустить перезапуск. Настройки применятся при следующем запуске.",
                            L"Screen2NVR", MB_OK | MB_ICONWARNING);
                return;
            }
            ExitApplication();
        }
    }

    void ExitApplication()
    {
        running_ = false;
        if (window_) DestroyWindow(window_);
        window_ = nullptr;
    }

    AppSettings settings_;
    AppSettings draftSettings_;
    AppSettings activeSettings_;
    std::atomic_bool& running_;
    TrayRuntimeStatus& status_;
    HWND window_ = nullptr;
    HWND tab_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT linkFont_ = nullptr;
    bool ownsUiFont_ = false;
    int selectedTab_ = 0;
    std::vector<HWND> deviceControls_;
    std::vector<HWND> videoControls_;
    std::vector<HWND> regionControls_, windowControls_, clickControls_, standbyControls_;
    std::vector<HWND> timestampControls_;
    std::vector<HWND> securityControls_;
    std::vector<HWND> statusControls_;
    std::wstring lanAddress_;
    std::vector<RECT> monitorRectangles_;
    std::vector<VideoResolution> mainResolutions_, subResolutions_;
    VideoResolution resolutionSource_;
    HWND foregroundBeforeSettings_ = nullptr;
    bool populatingControls_ = false, applyingProfile_ = false;
    std::vector<PrivacyMask> masks_;
    uint32_t maskReferenceWidth_ = 1920, maskReferenceHeight_ = 1080;
    int selectedMask_ = -1;
    bool addingMask_ = false, draggingMask_ = false, newMaskDrag_ = false;
    bool draggingText_ = false, textSelected_ = false, textBoundsValid_ = false;
    D2D1_RECT_F textBounds_{}, originalTextBounds_{};
    uint32_t originalMarginX_ = 0, originalMarginY_ = 0;
    POINT dragStart_{};
    PrivacyMask dragMask_{}, originalMask_{};
    ComPtr<ID2D1Factory> previewFactory_;
    ComPtr<ID2D1DCRenderTarget> previewTarget_;
    ComPtr<IDWriteFactory> previewWriteFactory_;
    bool recoveryAttempted_ = false;
    bool restartScheduled_ = false;
    uint64_t restartDeadlineMs_ = 0;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW tray_{};
    HICON statusIcon_ = nullptr;
    COLORREF statusColor_ = CLR_INVALID;
    HHOOK mouseHook_ = nullptr;
    inline static TrayApplication* mouseHookOwner_ = nullptr;
};
}

int RunTrayApplication(AppSettings settings, std::atomic_bool& running, TrayRuntimeStatus& status)
{
    TrayApplication application(std::move(settings), running, status);
    return application.Run();
}
