


// ============================================================
// FullScreen Browser - main.cpp
// WinMain entry point and application orchestration
// ============================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <memory>
#include <vector>
#include "resource.h"
#include "app_common.h"
#include "config_manager.h"
#include "crypto.h"
#include "hook_manager.h"
#include "webview_window.h"
#include "url_monitor.h"
#include "dialogs.h"
#include "remote_config_client.h"
#include "auto_update.h"

// ============================================================
// Application State
// ============================================================
struct AppState {
    HINSTANCE hInstance = nullptr;
    HWND hMainWnd = nullptr;
    AppConfig config;
    std::wstring currentPassword; // plaintext, held only in memory
    std::wstring remoteToken;
    bool isUnreachable = false;
    bool exiting = false;
    bool dialogActive = false;    // prevent re-entrant dialogs
    bool remoteSyncAllowed = false;
    std::wstring lastRemoteError;
    unsigned int remotePollDelayMs = 0;
    int remoteFailureCount = 0;
    bool needsUpdateConfirm = false; // an applied update awaits confirmation
    std::vector<std::wstring> consumedCommandIds;
};

inline constexpr UINT_PTR TIMER_REMOTE_CONFIG = 5005;
inline constexpr UINT_PTR TIMER_AUTO_UPDATE = 5006;
inline constexpr UINT_PTR TIMER_UPDATE_CONFIRM = 5007;

static AppState g_state;

// ============================================================
// Forward declarations
// ============================================================
static bool RunFirstTimeSetup();
static bool RunFullscreenMode();
static void OnHotkeyPressed();
static void ApplyConfigRuntime(const AppConfig& cfg);
static bool EnsureRemoteRegistration();
static void ScheduleNextRemotePoll();
static void PerformRemoteSync(bool startupSync);
static bool HasConsumedCommandId(std::wstring_view commandId);
static void RememberConsumedCommandId(std::wstring_view commandId);
static void OnUrlReachabilityChanged(bool reachable);
static void SafeWipePassword();
static void OnWebViewInitError(const wchar_t* errorMsg);
static void ShutdownForUpdate();

// ============================================================
// WinMain
// ============================================================
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow)
{
    (void)nCmdShow;

    // --- Initialize COM (required by WebView2) ---
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr)) {
        MessageBoxW(nullptr, L"COM initialization failed.", AppConstants::APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize common controls (needed for spin control in config dialog)
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_state.hInstance = hInstance;

    // --- Prevent multiple instances ---
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, AppConstants::MUTEX_NAME);
    if (hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"The program is already running.",
                    AppConstants::APP_NAME, MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }

    // --- Auto-update startup recovery (roll back a crashing new exe, or
    // retry an interrupted replacement). If it says to exit, the mutex was
    // relaunched into a fresh process; leave now. ---
    auto startupRecovery = AutoUpdate::HandleStartupRecovery();
    if (startupRecovery == AutoUpdate::StartupResult::RolledBackRestarted) {
        CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }
    g_state.needsUpdateConfirm =
        (startupRecovery == AutoUpdate::StartupResult::Confirming);

    // --- Check configuration ---
    bool hasConfig = ConfigManager::LoadConfig(g_state.config);
    if (!hasConfig || !g_state.config.IsValid()) {
        bool setupOk = RunFirstTimeSetup();
        if (!setupOk) {
            CloseHandle(hMutex);
            CoUninitialize();
            return 0;
        }
    } else {
        g_state.currentPassword = Crypto::Decrypt(g_state.config.encryptedPassword);
    }

    // Remote-sync eligibility is evaluated once in EnsureRemoteRegistration():
    // 1) remote config on? -> 2) remote URL usable? -> 3) already registered?
    // Honor the saved config on first-time setup instead of forcing it off.
    g_state.remoteSyncAllowed = g_state.config.remoteEnabled;

    if (g_state.remoteSyncAllowed) {
        ConfigManager::LoadRemoteToken(g_state.remoteToken);
        ConfigManager::LoadConsumedCommandIds(g_state.consumedCommandIds);
        EnsureRemoteRegistration();
        PerformRemoteSync(true);
    }

    // --- Enter fullscreen mode ---
    int exitCode = 0;
    bool fsOk = RunFullscreenMode();
    if (!fsOk) {
        exitCode = 1;
    }

    SafeWipePassword();
    CloseHandle(hMutex);
    CoUninitialize();
    return exitCode;
}

// ============================================================
// First-time setup flow
// ============================================================
static bool RunFirstTimeSetup()
{
    bool saved = Dialogs::ShowConfigDialog(
        g_state.hInstance, nullptr,
        g_state.config, g_state.currentPassword);

    if (!saved) {
        return false;
    }

    return true;
}

// ============================================================
// Fullscreen mode entry point
// ============================================================
static bool RunFullscreenMode()
{
    // --- Register window class ---
    if (!WebViewWindow::RegisterWindowClass(g_state.hInstance)) {
        MessageBoxW(nullptr, L"Window class registration failed.",
                    AppConstants::APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    // --- Check WebView2 runtime availability ---
    if (!WebViewWindow::IsRuntimeAvailable()) {
        MessageBoxW(nullptr,
            L"WebView2 Runtime is not installed.\n\n"
            L"Please download and install it from:\n"
            L"https://go.microsoft.com/fwlink/p/?LinkId=2124703\n\n"
            L"Then restart this program.",
            AppConstants::APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    // --- Initialize WebView2 environment (async) ---
    if (!WebViewWindow::Initialize(nullptr, OnWebViewInitError)) {
        MessageBoxW(nullptr, L"WebView2 initialization failed.",
                    AppConstants::APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    // --- Set navigation callback ---
    WebViewWindow::SetNavigationCallback(OnUrlReachabilityChanged);

    // --- Create fullscreen window ---
    g_state.hMainWnd = WebViewWindow::CreateFullscreenWindow(g_state.hInstance);
    if (!g_state.hMainWnd) {
        MessageBoxW(nullptr, L"Window creation failed.",
                    AppConstants::APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    // --- Install keyboard hook ---
    if (!HookManager::InstallHook(OnHotkeyPressed)) {
        // Non-fatal: continue without hook (degraded mode)
    }

    // F5 = reload, Ctrl+F5 = hard reload (clear cache). While the
    // "unreachable" message page is shown, refresh means "retry the page".
    HookManager::SetRefreshCallback([](bool hardRefresh) {
        if (g_state.exiting || g_state.dialogActive) return;
        if (g_state.isUnreachable) {
            WebViewWindow::NavigateTo(g_state.config.url);
        } else if (hardRefresh) {
            WebViewWindow::ReloadIgnoringCache();
        } else {
            WebViewWindow::Reload();
        }
    });

    // --- Navigate to target URL ---
    WebViewWindow::NavigateTo(g_state.config.url);

    // --- Set zoom ---
    double zoom = g_state.config.zoomPercent / 100.0;
    WebViewWindow::SetZoom(zoom);

    // --- Start URL monitoring ---
    UrlMonitor::Start(g_state.config.url, OnUrlReachabilityChanged);

    // --- Start auto-refresh if configured ---
    WebViewWindow::StartAutoRefresh(g_state.config.url,
                                     g_state.config.refreshMode,
                                     g_state.config.refreshIntervalSec,
                                     g_state.config.refreshDailyMin,
                                     g_state.config.refreshTimes);

    // --- Start pixel shift (burn-in prevention) if enabled ---
    if (g_state.config.burnInPrevention) {
        WebViewWindow::StartPixelShift();
    }

    if (g_state.remoteSyncAllowed && g_state.config.remoteEnabled) {
        ScheduleNextRemotePoll();
    }

    // --- Auto-update timers ---
    if (g_state.config.autoUpdate) {
        SetTimer(g_state.hMainWnd, TIMER_AUTO_UPDATE,
                 AppConstants::UPDATE_INITIAL_DELAY_MS, nullptr);
    }
    if (g_state.needsUpdateConfirm) {
        SetTimer(g_state.hMainWnd, TIMER_UPDATE_CONFIRM,
                 AppConstants::UPDATE_CONFIRM_SEC * 1000, nullptr);
    }

    // --- Message loop ---
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER && msg.wParam == TIMER_REMOTE_CONFIG) {
            PerformRemoteSync(false);
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == TIMER_AUTO_UPDATE) {
            if (AutoUpdate::CheckAndApply(g_state.config) ==
                AutoUpdate::CheckResult::AppliedRestartNeeded) {
                ShutdownForUpdate();
            } else {
                SetTimer(g_state.hMainWnd, TIMER_AUTO_UPDATE,
                         AppConstants::UPDATE_POLL_MS, nullptr);
            }
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == TIMER_UPDATE_CONFIRM) {
            KillTimer(g_state.hMainWnd, TIMER_UPDATE_CONFIRM);
            AutoUpdate::ConfirmAndCleanup();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetThreadExecutionState(ES_CONTINUOUS);

    return true;
}

// ============================================================
// WebView2 initialization error handler
// ============================================================
static void OnWebViewInitError(const wchar_t* errorMsg)
{
    if (g_state.exiting) return;

    std::wstring msg = L"WebView2 initialization error:\n";
    msg += (errorMsg && *errorMsg) ? errorMsg : L"Unknown error";
    msg += L"\n\nThe program will now exit.";

    MessageBoxW(g_state.hMainWnd, msg.c_str(),
                AppConstants::APP_NAME, MB_OK | MB_ICONERROR);

    g_state.exiting = true;
    PostQuitMessage(1);
}

// ============================================================
// Clean shutdown after staging an auto-update: the detached powershell
// (launched via -EncodedCommand) swaps the exe once this process has
// exited, then relaunches.
// ============================================================
static void ShutdownForUpdate()
{
    if (g_state.exiting) return;
    g_state.exiting = true;
    UrlMonitor::Stop();
    HookManager::UninstallHook();
    WebViewWindow::Cleanup();
    if (g_state.hMainWnd) {
        DestroyWindow(g_state.hMainWnd);
        g_state.hMainWnd = nullptr;
    }
    PostQuitMessage(0);
}

// ============================================================
// Verification handler (ESC / Alt+F4)
// ============================================================
static void OnHotkeyPressed()
{
    if (g_state.exiting) return;
    if (g_state.dialogActive) return; // prevent re-entrant dialogs

    g_state.dialogActive = true;

    // Let the password/menu dialogs receive keyboard & mouse input.
    HookManager::SetInputLocked(false);

    bool verified = Dialogs::ShowPasswordDialog(
        g_state.hInstance, g_state.hMainWnd,
        g_state.currentPassword);

    if (!verified) {
        g_state.dialogActive = false;
        HookManager::SetInputLocked(true); // back to kiosk lock
        return;
    }

    int choice = Dialogs::ShowMenuDialog(g_state.hInstance, g_state.hMainWnd);

    switch (choice) {
    case 1: // Exit
        g_state.exiting = true;
        g_state.dialogActive = false;
        UrlMonitor::Stop();
        HookManager::UninstallHook();
        WebViewWindow::Cleanup();
        if (g_state.hMainWnd) {
            DestroyWindow(g_state.hMainWnd);
            g_state.hMainWnd = nullptr;
        }
        PostQuitMessage(0);
        break;

    case 2: // Modify settings
        {
            std::wstring newPassword = g_state.currentPassword; // pre-fill
            bool saved = Dialogs::ShowConfigDialog(
                g_state.hInstance, g_state.hMainWnd,
                g_state.config, newPassword);

            if (saved) {
                SafeWipePassword();
                g_state.currentPassword = std::move(newPassword);
                ApplyConfigRuntime(g_state.config);

                if (g_state.hMainWnd) {
                    KillTimer(g_state.hMainWnd, TIMER_REMOTE_CONFIG);
                    KillTimer(g_state.hMainWnd, TIMER_AUTO_UPDATE);
                    KillTimer(g_state.hMainWnd, TIMER_UPDATE_CONFIRM);
                }

                // Re-arm the update check if auto-update was just enabled.
                if (g_state.hMainWnd && g_state.config.autoUpdate) {
                    SetTimer(g_state.hMainWnd, TIMER_AUTO_UPDATE,
                             AppConstants::UPDATE_INITIAL_DELAY_MS, nullptr);
                }

                if (g_state.config.remoteEnabled) {
                    ConfigManager::LoadRemoteToken(g_state.remoteToken);
                    ConfigManager::LoadConsumedCommandIds(g_state.consumedCommandIds);
                    // Same evaluation chain as startup: URL usable, then registered.
                    if (EnsureRemoteRegistration()) {
                        PerformRemoteSync(false);
                        if (!g_state.lastRemoteError.empty()) {
                            std::wstring msg = L"Remote configuration check failed. Local settings remain active.\n\nReason: ";
                            msg += g_state.lastRemoteError;
                            MessageBoxW(g_state.hMainWnd, msg.c_str(),
                                        AppConstants::APP_NAME, MB_OK | MB_ICONWARNING);
                        }
                    }
                } else {
                    g_state.remoteSyncAllowed = false;
                    g_state.remoteFailureCount = 0;
                    g_state.remotePollDelayMs = 0;
                    g_state.lastRemoteError.clear();
                }
            }
        }
        g_state.dialogActive = false;
        HookManager::SetInputLocked(true);
        break;

    default: // Cancel
        g_state.dialogActive = false;
        HookManager::SetInputLocked(true);
        break;
    }
}

// ============================================================
// Apply config while app is running
// ============================================================
static void ApplyConfigRuntime(const AppConfig& cfg)
{
    UrlMonitor::Stop();
    WebViewWindow::NavigateTo(cfg.url);

    double zoom = cfg.zoomPercent / 100.0;
    WebViewWindow::SetZoom(zoom);

    UrlMonitor::Start(cfg.url, OnUrlReachabilityChanged);

    WebViewWindow::StartAutoRefresh(cfg.url,
                                     cfg.refreshMode,
                                     cfg.refreshIntervalSec,
                                     cfg.refreshDailyMin,
                                     cfg.refreshTimes);

    WebViewWindow::StopPixelShift();
    if (cfg.burnInPrevention) {
        WebViewWindow::StartPixelShift();
    }
}

// ============================================================
// Remote config helpers
// ============================================================
// Remote registration state machine. Rungs in order:
//   1. remote config enabled?     -> no: leave disabled, never prompt
//   2. remote URL usable?         -> no: warn + disable remote (saved)
//   3. already registered (token)?-> yes: no prompt
//   4. else: mandatory registration dialog (cancel disabled)
static bool EnsureRemoteRegistration()
{
    // Rung 1: remote config on?
    if (!g_state.config.remoteEnabled) {
        g_state.remoteSyncAllowed = false;
        return false;
    }

    // Rung 2: remote URL usable? (syntax + reachability)
    if (!RemoteConfigClient::CheckServerReachable(g_state.config.remoteBaseUrl)) {
        std::wstring msg = L"Remote configuration URL is not reachable:\n\n";
        msg += g_state.config.remoteBaseUrl;
        msg += L"\n\nRemote configuration has been disabled. Local settings remain active.";
        MessageBoxW(g_state.hMainWnd, msg.c_str(),
                    AppConstants::APP_NAME, MB_OK | MB_ICONWARNING);
        g_state.config.remoteEnabled = false;
        ConfigManager::SaveConfig(g_state.config, g_state.currentPassword);
        g_state.remoteSyncAllowed = false;
        return false;
    }

    g_state.remoteSyncAllowed = true;

    // Ensure a stable device id before registration.
    if (g_state.config.deviceId[0] == L'\0') {
        GUID guid = {};
        if (CoCreateGuid(&guid) == S_OK) {
            wchar_t buf[64] = {};
            swprintf_s(buf, L"FSB-%08lX%04X%04X%04X%04X%08lX",
                       guid.Data1, guid.Data2, guid.Data3,
                       (guid.Data4[0] << 8) | guid.Data4[1],
                       (guid.Data4[2] << 8) | guid.Data4[3],
                       (unsigned long)((guid.Data4[4] << 24) | (guid.Data4[5] << 16) | (guid.Data4[6] << 8) | guid.Data4[7]));
            wcsncpy_s(g_state.config.deviceId, buf, _TRUNCATE);
        }
    }

    // Rung 3: already registered?
    if (!g_state.remoteToken.empty()) {
        return true;
    }

    // Rung 4: mandatory registration. The dialog cannot be cancelled, so a
    // failed attempt re-prompts until it succeeds.
    // ponytail: a permanently-down-but-reachable server or a bad code can block
    // the device on this loop; the escape hatch is disabling remote via the
    // settings dialog (password-protected).
    for (;;) {
        std::wstring registerCode;
        bool accepted = Dialogs::ShowRegisterCodeDialog(
            g_state.hInstance, g_state.hMainWnd, false, registerCode);
        if (!accepted) {
            // Unreachable when cancel is disabled; defensive fallback.
            return false;
        }

        auto result = RemoteConfigClient::RegisterDevice(g_state.config.remoteBaseUrl,
                                                         g_state.config.deviceId,
                                                         registerCode,
                                                         AutoUpdate::CurrentVersion());
        if (result.ok) {
            g_state.remoteToken = std::move(result.deviceToken);
            g_state.config.pollBaseSec = result.pollBaseSec;
            g_state.config.pollJitterSec = result.pollJitterSec;
            g_state.config.pollMaxBackoffSec = result.pollMaxBackoffSec;
            ConfigManager::SaveRemoteToken(g_state.remoteToken);
            ConfigManager::SaveConfig(g_state.config, g_state.currentPassword);
            return true;
        }

        std::wstring msg = L"Device registration failed.";
        if (!result.error.empty()) {
            msg += L"\n\nError: ";
            msg += result.error;
        }
        MessageBoxW(g_state.hMainWnd, msg.c_str(),
                    AppConstants::APP_NAME, MB_OK | MB_ICONWARNING);
        // Loop: registration is mandatory, keep prompting.
    }
}

static void ScheduleNextRemotePoll()
{
    if (!g_state.remoteSyncAllowed || !g_state.config.remoteEnabled || !g_state.hMainWnd) {
        return;
    }

    int baseSec = g_state.config.pollBaseSec > 0 ? g_state.config.pollBaseSec : AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int jitterSec = g_state.config.pollJitterSec >= 0 ? g_state.config.pollJitterSec : AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int maxBackoffSec = g_state.config.pollMaxBackoffSec > 0 ? g_state.config.pollMaxBackoffSec : AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;

    int delaySec = baseSec;
    if (jitterSec > 0) {
        delaySec += rand() % (jitterSec + 1);
    }

    if (g_state.remoteFailureCount > 0) {
        // Cap the exponential shift so the backoff can never overflow int
        // during a long outage (baseSec << count), then clamp to maxBackoffSec.
        const int shift = g_state.remoteFailureCount < 20 ? g_state.remoteFailureCount : 20;
        long long backoffSec = static_cast<long long>(baseSec) << shift;
        if (backoffSec > maxBackoffSec) backoffSec = maxBackoffSec;
        if (delaySec < backoffSec) delaySec = static_cast<int>(backoffSec);
    }

    if (delaySec < 1) delaySec = 1;
    g_state.remotePollDelayMs = static_cast<unsigned int>(delaySec * 1000);
    SetTimer(g_state.hMainWnd, TIMER_REMOTE_CONFIG, g_state.remotePollDelayMs, nullptr);
}

static void PerformRemoteSync(bool startupSync)
{
    if (!g_state.remoteSyncAllowed || !g_state.config.remoteEnabled || g_state.config.remoteBaseUrl[0] == L'\0') {
        return;
    }

    if (g_state.remoteToken.empty() && !EnsureRemoteRegistration()) {
        g_state.lastRemoteError = L"registration_skipped_or_failed";
        if (!startupSync) {
            g_state.remoteFailureCount++;
            ScheduleNextRemotePoll();
        }
        return;
    }

    auto result = RemoteConfigClient::FetchMergedConfig(g_state.config.remoteBaseUrl,
                                                        g_state.config.deviceId,
                                                        g_state.remoteToken,
                                                        g_state.config.configRevision,
                                                        g_state.config);
    if (!result.ok) {
        g_state.lastRemoteError = result.error.empty() ? L"remote_sync_failed" : result.error;
        g_state.remoteFailureCount++;
        if (!startupSync) {
            ScheduleNextRemotePoll();
        }
        return;
    }

    g_state.remoteFailureCount = 0;
    g_state.lastRemoteError.clear();

    std::wstring commandStatus;
    const auto* passwordCommand = result.passwordCommand.present ? &result.passwordCommand : nullptr;

    if (passwordCommand != nullptr &&
        !passwordCommand->commandId.empty() &&
        !HasConsumedCommandId(passwordCommand->commandId)) {
        bool withinWindow = true;
        time_t now = time(nullptr);
        if (passwordCommand->effectiveAt > 0 && now < passwordCommand->effectiveAt) {
            withinWindow = false;
        }
        if (passwordCommand->expireAt > 0 && now > passwordCommand->expireAt) {
            withinWindow = false;
        }

        if (withinWindow && !passwordCommand->encryptedPassword.empty()) {
            std::wstring newPassword = Crypto::Decrypt(passwordCommand->encryptedPassword);
            if (!newPassword.empty()) {
                if (ConfigManager::SaveConfig(g_state.config, newPassword)) {
                    SafeWipePassword();
                    g_state.currentPassword = std::move(newPassword);
                    RememberConsumedCommandId(passwordCommand->commandId);
                    commandStatus = L"success";
                } else {
                    commandStatus = L"failed";
                }
            } else {
                commandStatus = L"failed";
            }
        }
    }

    if (!result.notModified) {
        result.config.remoteEnabled = g_state.config.remoteEnabled;
        wcsncpy_s(result.config.remoteBaseUrl, g_state.config.remoteBaseUrl, _TRUNCATE);
        wcsncpy_s(result.config.deviceId, g_state.config.deviceId, _TRUNCATE);
        result.config.configRevision = result.revision;
        result.config.pollBaseSec = g_state.config.pollBaseSec;
        result.config.pollJitterSec = g_state.config.pollJitterSec;
        result.config.pollMaxBackoffSec = g_state.config.pollMaxBackoffSec;

        // Auto-update settings are local policy, never overridden by the
        // remote merged config.
        result.config.autoUpdate = g_state.config.autoUpdate;
        result.config.updateSource = g_state.config.updateSource;
        wcsncpy_s(result.config.updateRepo, g_state.config.updateRepo, _TRUNCATE);
        wcsncpy_s(result.config.updateBaseUrl, g_state.config.updateBaseUrl, _TRUNCATE);
        wcsncpy_s(result.config.updateWindow, g_state.config.updateWindow, _TRUNCATE);

        g_state.config = result.config;
        if (!startupSync) {
            ApplyConfigRuntime(g_state.config);
        }
        ConfigManager::SaveConfig(g_state.config, g_state.currentPassword);
        RemoteConfigClient::AckAppliedRevision(g_state.config.remoteBaseUrl,
                                              g_state.config.deviceId,
                                              g_state.remoteToken,
                                              g_state.config.configRevision,
                                              L"success",
                                              L"applied",
                                              passwordCommand,
                                              commandStatus);
    }
    // Note: a notModified response never carries commands (the client returns
    // early before parsing them), so there is no command ack to send in that case.

    if (!startupSync) {
        ScheduleNextRemotePoll();
    }
}

static bool HasConsumedCommandId(std::wstring_view commandId)
{
    return std::find(g_state.consumedCommandIds.begin(),
                     g_state.consumedCommandIds.end(),
                     std::wstring(commandId)) != g_state.consumedCommandIds.end();
}

static void RememberConsumedCommandId(std::wstring_view commandId)
{
    if (commandId.empty() || HasConsumedCommandId(commandId)) {
        return;
    }

    g_state.consumedCommandIds.emplace_back(commandId);
    ConfigManager::SaveConsumedCommandId(commandId);
}

// ============================================================
// URL reachability change handler
// ============================================================
static void OnUrlReachabilityChanged(bool reachable)
{
    if (g_state.exiting) return;

    if (!reachable) {
        g_state.isUnreachable = true;
        // Re-assert on every notification (not just the transition) so the
        // message reappears if it was overwritten (e.g. by an auto-refresh).
        // ShowMessage is idempotent while the message page is still visible.
        WebViewWindow::ShowMessage(g_state.config.unreachableMsg);
    } else if (reachable && g_state.isUnreachable) {
        g_state.isUnreachable = false;
        WebViewWindow::NavigateTo(g_state.config.url);
    }
}

// ============================================================
// Securely wipe password from memory
// ============================================================
static void SafeWipePassword()
{
    if (!g_state.currentPassword.empty()) {
        SecureZeroMemory(g_state.currentPassword.data(),
                         g_state.currentPassword.size() * sizeof(wchar_t));
        g_state.currentPassword.clear();
    }
}