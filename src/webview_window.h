#pragma once
#include "app_common.h"
#include <Windows.h>
#include <WebView2.h>
#include <wrl/client.h>
#include <string>
#include <functional>

// ============================================================
// WebViewWindow - fullscreen window with WebView2
// ============================================================

namespace WebViewWindow {

using UrlStatusCallback = std::function<void(bool reachable)>;
using InitErrorCallback = std::function<void(const wchar_t* errorMsg)>;

// Initialize WebView2 environment (async). Must call before CreateWindow.
// hParent: reserved for future use (can be nullptr)
// onError: called if WebView2 runtime is missing or init fails
// Returns true if environment creation was initiated.
bool Initialize(HWND hParent, InitErrorCallback onError);

// Create the fullscreen window. Returns HWND on success, NULL on failure.
// Must call Initialize() first.
HWND CreateFullscreenWindow(HINSTANCE hInstance);

// Navigate to a URL
void NavigateTo(const std::wstring& url);

// Show an HTML message in the WebView (e.g., "page unreachable")
void ShowMessage(const std::wstring& htmlMessage);

// Reload the current page (F5-like).
void Reload();

// Hard reload: clear the browser cache, then reload (Ctrl+F5-like).
void ReloadIgnoringCache();

// Set zoom factor (e.g., 1.0 = 100%)
void SetZoom(double factor);

// Set callback for navigation completion events
void SetNavigationCallback(UrlStatusCallback callback);

// Resize WebView to fill window
void ResizeWebView();

// Auto-refresh.
// mode: REFRESH_MODE_OFF (disabled) / REFRESH_MODE_INTERVAL (every intervalSec
// seconds) / REFRESH_MODE_DAILY (at dailyMin minutes since midnight each day).
void StartAutoRefresh(const std::wstring& url, int mode, int intervalSec, int dailyMin);

// Pixel shift for burn-in prevention
void StartPixelShift();
void StopPixelShift();

// Destroy WebView and cleanup
void Cleanup();

// Window procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Register window class
ATOM RegisterWindowClass(HINSTANCE hInstance);

// Check if WebView2 runtime is available (synchronous check)
bool IsRuntimeAvailable();

} // namespace WebViewWindow
