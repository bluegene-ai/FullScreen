// ============================================================
// WebViewWindow - fullscreen window with WebView2
// ============================================================
#include "webview_window.h"
#include "resource.h"
#include "config_manager.h"
#include "url_monitor.h"
#include <Windows.h>
#include <wrl.h>
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace Microsoft::WRL;

namespace WebViewWindow {

// ============================================================
// Global state
// ============================================================
static HWND                          g_hWnd = nullptr;
static HINSTANCE                     g_hInstance = nullptr;
static ComPtr<ICoreWebView2Environment>    g_environment;
static ComPtr<ICoreWebView2Controller>     g_controller;
static ComPtr<ICoreWebView2>               g_webView;
static std::wstring                   g_currentUrl;
static double                         g_pendingZoom = 1.0;
static bool                           g_webViewReady = false;
static std::wstring                   g_pendingNavigate;
static UrlStatusCallback              g_navCallback;
static InitErrorCallback              g_initErrorCallback;
static EventRegistrationToken         g_navCompletedToken;
static EventRegistrationToken         g_newWindowToken;
static std::mutex                     g_mutex;

// True while a ShowMessage (NavigateToString) navigation is in flight.
// Its completion must NOT be reported as "URL reachable", otherwise the
// message would be immediately overridden by a retry navigation.
static std::atomic<bool>              g_showingMessage{false};

// Last message shown and whether the message page is currently visible.
// Makes ShowMessage idempotent and lets the monitor re-show the message if it
// got overwritten (e.g. by an auto-refresh reload of the target URL).
static std::wstring                   g_messageText;
static std::atomic<bool>              g_messageVisible{false};

// Pixel shift state
static int g_pixelShiftIndex = 0;
static bool g_pixelShiftActive = false;
static int g_pixelShiftDx = 0;
static int g_pixelShiftDy = 0;

// Auto-refresh state
static std::wstring g_refreshUrl;
static int g_refreshMode = AppConstants::REFRESH_MODE_OFF;
static int g_refreshIntervalSec = 0;
static int g_refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;

// ============================================================
// Forward declarations
// ============================================================
static void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env);
static void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);
static void RegisterEventHandlers();
static void ApplyPendingOperations();

// ============================================================
// Helper: Generate an HTML page for error messages
// ============================================================
// HTML-escape user content before embedding in the message page
static void AppendHtmlEscaped(std::wostringstream& html, const std::wstring& s)
{
    for (wchar_t ch : s) {
        switch (ch) {
        case L'<': html << L"&lt;"; break;
        case L'>': html << L"&gt;"; break;
        case L'&': html << L"&amp;"; break;
        case L'"': html << L"&quot;"; break;
        default:   html << ch; break;
        }
    }
}

static std::wstring BuildMessageHtml(const std::wstring& message)
{
    std::wostringstream html;
    html << L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
         << L"<style>"
         << L"*{margin:0;padding:0;box-sizing:border-box}"
         << L"body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;"
         << L"height:100vh;overflow:hidden;color:#fff;"
         << L"background:radial-gradient(circle at 50% 35%,#4a1212 0%,#200707 55%,#0d0303 100%);"
         << L"display:flex;align-items:center;justify-content:center}"
         << L".card{text-align:center;max-width:760px;width:92%;padding:48px 56px;"
         << L"background:rgba(0,0,0,.55);border:3px solid #ff3b3b;border-radius:18px;"
         << L"box-shadow:0 0 90px rgba(255,59,59,.5)}"
         << L".icon{width:128px;height:128px;margin:0 auto 28px;display:block;"
         << L"animation:pulse 1.1s ease-in-out infinite}"
         << L"@keyframes pulse{0%,100%{transform:scale(1)}50%{transform:scale(1.1)}}"
         << L".msg{font-size:42px;line-height:1.5;font-weight:700;margin:0 0 34px;"
         << L"word-break:break-word;text-shadow:0 0 30px rgba(255,255,255,.25)}"
         << L".status{font-size:15px;color:#ffb3b3;letter-spacing:1px;line-height:1.8;"
         << L"word-break:break-all}"
         << L".dot{display:inline-block;width:9px;height:9px;border-radius:50%;"
         << L"background:#ff5252;margin-right:8px;animation:blink 1s steps(2,start) infinite}"
         << L"@keyframes blink{to{visibility:hidden}}"
         << L"</style></head><body>"
         << L"<div class=\"card\">"
         << L"<svg class=\"icon\" viewBox=\"0 0 24 24\">"
         << L"<circle cx=\"12\" cy=\"12\" r=\"11\" fill=\"#ff3b3b\"/>"
         << L"<rect x=\"11\" y=\"5\" width=\"2\" height=\"9\" rx=\"1\" fill=\"#fff\"/>"
         << L"<circle cx=\"12\" cy=\"17.5\" r=\"1.6\" fill=\"#fff\"/>"
         << L"</svg>"
         << L"<p class=\"msg\">";
    const std::wstring& text = message.empty()
        ? L"The target page is currently unreachable."
        : message;
    AppendHtmlEscaped(html, text);
    html << L"</p>"
         << L"<p class=\"status\"><span class=\"dot\"></span>Monitoring ";
    if (!g_refreshUrl.empty()) {
        html << L"<span style=\"font-weight:700;color:#ffd0d0\">";
        AppendHtmlEscaped(html, g_refreshUrl);
        html << L"</span> · ";
    }
    html << L"auto-reconnect when available · <span id=\"clock\">--:--:--</span></p>"
         << L"</div>"
         << L"<script>"
         << L"function pad(n){return (n<10?'0':'')+n}"
         << L"setInterval(function(){var d=new Date();"
         << L"document.getElementById('clock').textContent="
         << L"pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());},1000);"
         << L"</script>"
         << L"</body></html>";
    return html.str();
}

// ============================================================
// Register window class
// ============================================================
ATOM RegisterWindowClass(HINSTANCE hInstance)
{
    g_hInstance = hInstance;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"FullScreenBrowserWnd";

    return RegisterClassExW(&wc);
}

// ============================================================
// Check if WebView2 runtime is available
// ============================================================
bool IsRuntimeAvailable()
{
    wchar_t* versionStr = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionStr)) || !versionStr) {
        return false;
    }
    CoTaskMemFree(versionStr);
    return true;
}

// ============================================================
// Initialize WebView2 environment
// ============================================================
bool Initialize(HWND /*hParent*/, InitErrorCallback onError)
{
    g_initErrorCallback = std::move(onError);

    if (!IsRuntimeAvailable()) {
        if (g_initErrorCallback) {
            g_initErrorCallback(L"WebView2 Runtime is not installed.");
        }
        return false;
    }

    std::wstring userDataDir = ConfigManager::GetWebView2DataPath();
    if (userDataDir.empty()) {
        if (g_initErrorCallback) {
            g_initErrorCallback(L"Failed to get user data directory.");
        }
        return false;
    }

    ConfigManager::EnsureConfigDir();
    CreateDirectoryW(userDataDir.c_str(), nullptr);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDir.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                OnEnvironmentCreated(result, env);
                return S_OK;
            }).Get());

    return SUCCEEDED(hr);
}

// ============================================================
// Environment created callback
// ============================================================
static void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (FAILED(result) || !env) {
        if (g_initErrorCallback) {
            wchar_t errBuf[128]; swprintf_s(errBuf, L"WebView2 environment creation failed. HRESULT: 0x%08X", (unsigned)result); g_initErrorCallback(errBuf);
        }
        return;
    }

    g_environment = env;

    if (g_hWnd) {
        g_environment->CreateCoreWebView2Controller(
            g_hWnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    OnControllerCreated(result, controller);
                    return S_OK;
                }).Get());
    }
}

// ============================================================
// Controller created callback
// ============================================================
static void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (FAILED(result) || !controller) {
        if (g_initErrorCallback) {
            wchar_t errBuf2[128]; swprintf_s(errBuf2, L"WebView2 controller creation failed. HRESULT: 0x%08X", (unsigned)result); g_initErrorCallback(errBuf2);
        }
        return;
    }

    g_controller = controller;
    g_controller->get_CoreWebView2(&g_webView);

    if (g_webView) {
        // Get settings and disable dev tools
        ComPtr<ICoreWebView2Settings> settings;
        g_webView->get_Settings(&settings);
        if (settings) {
            settings->put_AreDevToolsEnabled(FALSE);
        }

        RegisterEventHandlers();
        ResizeWebView();
        g_webViewReady = true;
        ApplyPendingOperations();
    }
}

// ============================================================
// Register WebView2 event handlers
// ============================================================
static void RegisterEventHandlers()
{
    if (!g_webView) return;

    g_webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                (void)sender;
                BOOL success = FALSE;
                args->get_IsSuccess(&success);

                // A message navigation (ShowMessage -> NavigateToString) must
                // never be treated as "URL reachable": doing so would override
                // the message with a retry navigation and reset the
                // unreachable state so recovery would never be detected.
                if (g_showingMessage.exchange(false)) {
                    return S_OK;
                }

                // A real page navigation completed; the message page is gone.
                g_messageVisible.store(false);

                UrlMonitor::OnNavigationCompleted(success != FALSE);

                if (g_navCallback) {
                    g_navCallback(success != FALSE);
                }
                return S_OK;
            }).Get(),
        &g_navCompletedToken);

    g_webView->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                (void)sender;
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(),
        &g_newWindowToken);
}

// ============================================================
// Apply pending operations
// ============================================================
static void ApplyPendingOperations()
{
    if (!g_webView) return;

    if (g_pendingZoom != 1.0) {
        ComPtr<ICoreWebView2Controller2> ctrl2;
        if (SUCCEEDED(g_controller.As(&ctrl2))) {
            ctrl2->put_ZoomFactor(g_pendingZoom);
        }
    }

    if (!g_pendingNavigate.empty()) {
        g_webView->Navigate(g_pendingNavigate.c_str());
        g_pendingNavigate.clear();
    }
}

// ============================================================
// Create fullscreen window
// ============================================================
HWND CreateFullscreenWindow(HINSTANCE hInstance)
{
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"FullScreenBrowserWnd",
        L"",
        WS_POPUP | WS_VISIBLE,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return nullptr;

    g_hWnd = hWnd;

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)nullptr);

    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hWnd);

    ShowCursor(FALSE);
    SetTimer(hWnd, TIMER_CURSOR_HIDE, AppConstants::CURSOR_HIDE_INTERVAL, nullptr);

    if (g_environment && !g_controller) {
        g_environment->CreateCoreWebView2Controller(
            hWnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    OnControllerCreated(result, controller);
                    return S_OK;
                }).Get());
    }

    return hWnd;
}

// ============================================================
// Navigate to URL
// ============================================================
void NavigateTo(const std::wstring& url)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_currentUrl = url;

    if (g_webView && g_webViewReady) {
        g_webView->Navigate(url.c_str());
    } else {
        g_pendingNavigate = url;
    }
}

// ============================================================
// Show message in WebView
// ============================================================
void ShowMessage(const std::wstring& htmlMessage)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_messageVisible.load() && g_messageText == htmlMessage) {
            return; // already showing this message; keep it (no flicker)
        }
        g_messageText = htmlMessage;
    }
    g_messageVisible.store(true);

    std::wstring html = BuildMessageHtml(htmlMessage);

    if (g_webView && g_webViewReady) {
        g_showingMessage = true;
        g_webView->NavigateToString(html.c_str());
    }
}

// ============================================================
// Reload / hard reload (F5 / Ctrl+F5)
// ============================================================
void Reload()
{
    if (g_webView && g_webViewReady) {
        g_webView->Reload();
    }
}

void ReloadIgnoringCache()
{
    if (!g_webView || !g_webViewReady) return;

    ComPtr<ICoreWebView2_13> wv13;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile2> profile2;
    if (SUCCEEDED(g_webView.As(&wv13)) &&
        SUCCEEDED(wv13->get_Profile(&profile)) &&
        SUCCEEDED(profile.As(&profile2))) {
        profile2->ClearBrowsingData(
            (COREWEBVIEW2_BROWSING_DATA_KINDS)
                (COREWEBVIEW2_BROWSING_DATA_KINDS_CACHE_STORAGE |
                 COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE),
            Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                [](HRESULT) -> HRESULT {
                    // Cache cleared: reload the current page.
                    if (g_webView && g_webViewReady) {
                        g_webView->Reload();
                    }
                    return S_OK;
                }).Get());
    } else {
        g_webView->Reload();
    }
}

// ============================================================
// Set zoom
// ============================================================
void SetZoom(double factor)
{
    g_pendingZoom = factor;

    if (g_controller && g_webViewReady) {
        ComPtr<ICoreWebView2Controller2> ctrl2;
        if (SUCCEEDED(g_controller.As(&ctrl2))) {
            ctrl2->put_ZoomFactor(factor);
        }
    }
}

// ============================================================
// Set navigation callback
// ============================================================
void SetNavigationCallback(UrlStatusCallback callback)
{
    g_navCallback = std::move(callback);
}

// ============================================================
// Resize WebView to fill window
// ============================================================
void ResizeWebView()
{
    if (!g_controller || !g_hWnd) return;

    RECT rc;
    GetClientRect(g_hWnd, &rc);

    // Apply current pixel shift offset if active
    if (g_pixelShiftActive) {
        rc.left += g_pixelShiftDx;
        rc.top += g_pixelShiftDy;
        rc.right += g_pixelShiftDx;
        rc.bottom += g_pixelShiftDy;
    }

    g_controller->put_Bounds(rc);
}

// ============================================================
// Pixel shift for burn-in prevention
// ============================================================
void StartPixelShift()
{
    g_pixelShiftIndex = 0;
    g_pixelShiftActive = true;
    if (g_hWnd) {
        SetTimer(g_hWnd, TIMER_PIXEL_SHIFT, AppConstants::PIXEL_SHIFT_INTERVAL, nullptr);
    }
}

void StopPixelShift()
{
    g_pixelShiftActive = false;
    if (g_hWnd) {
        KillTimer(g_hWnd, TIMER_PIXEL_SHIFT);
    }
}

static void DoPixelShift()
{
    if (!g_hWnd || !g_controller) return;

    // Circular pattern offsets in pixels
    static const int offsets[][2] = {
        {0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1},
        {-1, 0}, {-1, -1}, {0, -1}, {1, -1}
    };
    static const int numOffsets = sizeof(offsets) / sizeof(offsets[0]);

    g_pixelShiftIndex = (g_pixelShiftIndex + 1) % numOffsets;
    g_pixelShiftDx = offsets[g_pixelShiftIndex][0];
    g_pixelShiftDy = offsets[g_pixelShiftIndex][1];

    // Shift WebView controller bounds instead of moving the window.
    // Window stays fullscreen (black background fills any gap).
    RECT rc;
    GetClientRect(g_hWnd, &rc);
    rc.left += g_pixelShiftDx;
    rc.top += g_pixelShiftDy;
    rc.right += g_pixelShiftDx;
    rc.bottom += g_pixelShiftDy;
    g_controller->put_Bounds(rc);
}

// ============================================================
// Auto-refresh
// ============================================================
// Milliseconds until the next occurrence of dailyMin (minutes since midnight).
static unsigned MsUntilNextDaily(int dailyMin)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    int nowMin = st.wHour * 60 + st.wMinute;
    int delta = dailyMin - nowMin; // minutes until target (<= 0 = already passed today)
    if (delta <= 0) delta += 24 * 60;

    long long ms = (long long)delta * 60000;
    ms -= (long long)st.wSecond * 1000 + st.wMilliseconds; // land exactly on :00
    if (ms <= 0) ms += 24LL * 60 * 60 * 1000;

    return (unsigned)ms;
}

void StartAutoRefresh(const std::wstring& url, int mode, int intervalSec, int dailyMin)
{
    g_refreshUrl = url;
    g_refreshMode = mode;
    g_refreshIntervalSec = intervalSec;
    g_refreshDailyMin = dailyMin;

    if (g_hWnd) {
        KillTimer(g_hWnd, TIMER_AUTO_REFRESH);
    }

    if (!g_hWnd) return;

    if (mode == AppConstants::REFRESH_MODE_INTERVAL && intervalSec > 0) {
        SetTimer(g_hWnd, TIMER_AUTO_REFRESH, (unsigned)(intervalSec * 1000), nullptr);
    } else if (mode == AppConstants::REFRESH_MODE_DAILY) {
        SetTimer(g_hWnd, TIMER_AUTO_REFRESH, MsUntilNextDaily(dailyMin), nullptr);
    }
}

static void DoAutoRefresh()
{
    // Only refresh if we're showing the target URL (not the unreachable message)
    if (g_currentUrl == g_refreshUrl && !g_messageVisible.load() &&
        g_webView && g_webViewReady) {
        g_webView->Reload();
    }
}

// ============================================================
// Cleanup
// ============================================================
void Cleanup()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_webView) {
        g_webView->remove_NavigationCompleted(g_navCompletedToken);
        g_webView->remove_NewWindowRequested(g_newWindowToken);
    }

    g_controller.Reset();
    g_webView.Reset();
    g_environment.Reset();

    g_webViewReady = false;
    g_pendingNavigate.clear();
    g_pixelShiftActive = false;

    if (g_hWnd) {
        KillTimer(g_hWnd, TIMER_CURSOR_HIDE);
        KillTimer(g_hWnd, TIMER_PIXEL_SHIFT);
        KillTimer(g_hWnd, TIMER_AUTO_REFRESH);
        ShowCursor(TRUE);
    }

    g_navCallback = nullptr;
    g_initErrorCallback = nullptr;
}

// ============================================================
// Window procedure
// ============================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CLOSE:
    case WM_QUERYENDSESSION:
        return 0;

    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_CURSOR_HIDE) {
            ShowCursor(FALSE);
        } else if (wParam == TIMER_PIXEL_SHIFT) {
            if (g_pixelShiftActive) DoPixelShift();
        } else if (wParam == TIMER_AUTO_REFRESH) {
            DoAutoRefresh();
            // Daily mode timer is one-shot: re-arm for the next day
            if (g_refreshMode == AppConstants::REFRESH_MODE_DAILY && g_hWnd) {
                SetTimer(g_hWnd, TIMER_AUTO_REFRESH, MsUntilNextDaily(g_refreshDailyMin), nullptr);
            }
        }
        break;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(hdc, &rc, black);
            EndPaint(hWnd, &ps);
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace WebViewWindow