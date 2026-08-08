// ============================================================
// UrlMonitor - passive + active URL reachability monitoring
// ============================================================
#include "url_monitor.h"
#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

#pragma comment(lib, "winhttp.lib")

namespace UrlMonitor {

static std::wstring g_url;
static ReachabilityCallback g_callback;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_unreachable{false};
// Bumped on Stop(); workers exit when their generation no longer matches, so a
// Start() right after Stop() can never leave two monitor threads running (race).
static std::atomic<int> g_generation{0};
static std::mutex g_mutex;
static HWND g_timerWnd = nullptr;

// ============================================================
// Check if URL is reachable via HEAD request
// ============================================================
static bool CheckUrlReachable(const std::wstring& url)
{
    if (url.empty()) return false;

    // Parse URL to get host + path
    std::wstring host;
    std::wstring path = L"/";
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool isHttps = true;

    // Skip scheme
    size_t start = 0;
    if (url.find(L"https://") == 0) {
        start = 8;
        isHttps = true;
        port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (url.find(L"http://") == 0) {
        start = 7;
        isHttps = false;
        port = INTERNET_DEFAULT_HTTP_PORT;
    }

    // Find end of host
    size_t hostEnd = url.find(L'/', start);
    if (hostEnd == std::wstring::npos) {
        host = url.substr(start);
    } else {
        host = url.substr(start, hostEnd - start);
        path = url.substr(hostEnd);
    }

    HINTERNET hSession = WinHttpOpen(L"FullScreenBrowser/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    DWORD timeout = 10000; // 10 seconds
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Don't auto-redirect for HEAD, just check if we get any response
    BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    result = WinHttpReceiveResponse(hRequest, nullptr);
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return (statusCode >= 200 && statusCode < 400);
}

// ============================================================
// Worker thread for active URL checking
// ============================================================
static DWORD WINAPI MonitorThreadProc(LPVOID lpParam)
{
    const int myGen = static_cast<int>(reinterpret_cast<intptr_t>(lpParam));
    while (g_running && g_generation.load() == myGen) {
        // Probe immediately on start, then every ~10s.
        bool reachable = CheckUrlReachable(g_url);

        bool prevUnreachable = g_unreachable.exchange(!reachable);

        // Report on transitions; additionally keep reporting while unreachable
        // so the UI re-asserts the message if it was overwritten (e.g. by an
        // auto-refresh). ShowMessage is idempotent while the message is visible.
        if (g_callback && (reachable != !prevUnreachable || !reachable)) {
            g_callback(reachable);
        }

        // Wait for interval or stop signal (~10s)
        for (int i = 0; i < 100 && g_running && g_generation.load() == myGen; ++i) {
            Sleep(100);
        }
        if (!g_running || g_generation.load() != myGen) break;
    }
    return 0;
}

// ============================================================
// Public API
// ============================================================

void Start(const std::wstring& url, ReachabilityCallback callback)
{
    Stop();

    g_url = url;
    g_callback = std::move(callback);
    g_unreachable = false;
    const int myGen = g_generation.load();
    g_running = true;

    // Start worker thread bound to the current generation
    std::thread t(MonitorThreadProc, reinterpret_cast<void*>(static_cast<intptr_t>(myGen)));
    t.detach();
}

void Stop()
{
    g_running = false;
    g_generation.fetch_add(1); // invalidate any in-flight worker thread
    Sleep(50); // Let thread exit
    g_callback = nullptr;
}

void OnNavigationCompleted(bool success)
{
    bool prev = g_unreachable.exchange(!success);
    if (success != !prev && g_callback) {
        g_callback(success);
    }
}

} // namespace UrlMonitor
