// ============================================================
// Dialogs - Settings, Password, Menu dialogs
// ============================================================
#include "dialogs.h"
#include "resource.h"
#include "config_manager.h"
#include "crypto.h"
#include "remote_config_client.h"
#include "auto_start.h"
#include "refresh_schedule.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

// ============================================================
// Helper: center dialog relative to parent or screen
// ============================================================
static void CenterDialog(HWND hDlg, HWND hParent)
{
    RECT rcDlg, rcParent;
    GetWindowRect(hDlg, &rcDlg);

    if (hParent && IsWindowVisible(hParent)) {
        GetWindowRect(hParent, &rcParent);
    } else {
        rcParent.left = 0;
        rcParent.top = 0;
        rcParent.right = GetSystemMetrics(SM_CXSCREEN);
        rcParent.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    int w = rcDlg.right - rcDlg.left;
    int h = rcDlg.bottom - rcDlg.top;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - w) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - h) / 2;

    SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// ============================================================
// Config Dialog
// ============================================================
namespace {

struct ConfigDialogData {
    AppConfig* config;
    std::wstring* plainPassword;
    bool saved;
};

// Return the refresh mode selected by the radio buttons
static int GetSelectedRefreshMode(HWND hDlg)
{
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_REFRESH_INTERVAL)) {
        return AppConstants::REFRESH_MODE_INTERVAL;
    }
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_REFRESH_DAILY)) {
        return AppConstants::REFRESH_MODE_DAILY;
    }
    return AppConstants::REFRESH_MODE_OFF;
}

// Enable/disable the interval and daily-time inputs per selected mode
static void UpdateRefreshControls(HWND hDlg)
{
    int mode = GetSelectedRefreshMode(hDlg);
    EnableWindow(GetDlgItem(hDlg, IDC_REFRESH_INTERVAL),
                 mode == AppConstants::REFRESH_MODE_INTERVAL);
    EnableWindow(GetDlgItem(hDlg, IDC_REFRESH_DAILY),
                 mode == AppConstants::REFRESH_MODE_DAILY);
}

// Enable/disable the auto-update source radios and inputs per the checkbox
// and selected source radio (GitHub repo vs self-hosted URL).
static void UpdateUpdateControls(HWND hDlg)
{
    bool enabled = IsDlgButtonChecked(hDlg, IDC_AUTO_UPDATE_CHECK) == BST_CHECKED;
    bool github = IsDlgButtonChecked(hDlg, IDC_RADIO_UPDATE_GITHUB) == BST_CHECKED;

    EnableWindow(GetDlgItem(hDlg, IDC_RADIO_UPDATE_GITHUB), enabled);
    EnableWindow(GetDlgItem(hDlg, IDC_RADIO_UPDATE_SELF), enabled);
    EnableWindow(GetDlgItem(hDlg, IDC_UPDATE_REPO), enabled && github);
    EnableWindow(GetDlgItem(hDlg, IDC_UPDATE_BASE_URL), enabled && !github);
    EnableWindow(GetDlgItem(hDlg, IDC_UPDATE_WINDOW), enabled);
}

static std::wstring FormatHhMm(int minute)
{
    wchar_t buf[8];
    swprintf_s(buf, L"%02d:%02d", minute / 60, minute % 60);
    return std::wstring(buf);
}

static INT_PTR CALLBACK ConfigDialogProcImpl(HWND hDlg, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    static ConfigDialogData* dlgData = nullptr;

    switch (msg) {
    case WM_INITDIALOG:
        {
            dlgData = reinterpret_cast<ConfigDialogData*>(lParam);

            // Populate fields
            SetDlgItemTextW(hDlg, IDC_URL, dlgData->config->url);
            SetDlgItemTextW(hDlg, IDC_UNREACHABLE_MSG,
                            dlgData->config->unreachableMsg);

            // Pre-fill password fields if modifying existing settings
            if (!dlgData->plainPassword->empty()) {
                SetDlgItemTextW(hDlg, IDC_PASSWORD, dlgData->plainPassword->c_str());
                SetDlgItemTextW(hDlg, IDC_CONFIRM_PASSWORD, dlgData->plainPassword->c_str());
            }

            // Set zoom and configure spin control
            HWND hSpin = GetDlgItem(hDlg, IDC_ZOOM_SPIN);
            SendMessageW(hSpin, UDM_SETRANGE, 0,
                         MAKELPARAM(AppConstants::ZOOM_MAX, AppConstants::ZOOM_MIN));
            SendMessageW(hSpin, UDM_SETPOS, 0, dlgData->config->zoomPercent);

            // Limit text length
            SendMessageW(GetDlgItem(hDlg, IDC_URL), EM_SETLIMITTEXT, 2047, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_PASSWORD), EM_SETLIMITTEXT, 255, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_CONFIRM_PASSWORD), EM_SETLIMITTEXT, 255, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_UNREACHABLE_MSG), EM_SETLIMITTEXT, 1023, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_REFRESH_INTERVAL), EM_SETLIMITTEXT, 6, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_REFRESH_DAILY), EM_SETLIMITTEXT, 250, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_REMOTE_BASE_URL), EM_SETLIMITTEXT, 1023, 0);

            // Set refresh mode radios and values
            int checkId = IDC_RADIO_REFRESH_OFF;
            switch (dlgData->config->refreshMode) {
            case AppConstants::REFRESH_MODE_INTERVAL:
                checkId = IDC_RADIO_REFRESH_INTERVAL;
                break;
            case AppConstants::REFRESH_MODE_DAILY:
                checkId = IDC_RADIO_REFRESH_DAILY;
                break;
            default:
                break;
            }
            CheckRadioButton(hDlg, IDC_RADIO_REFRESH_OFF, IDC_RADIO_REFRESH_DAILY, checkId);
            SetDlgItemInt(hDlg, IDC_REFRESH_INTERVAL, dlgData->config->refreshIntervalSec, FALSE);
            std::wstring dailyText = dlgData->config->refreshTimes[0]
                ? dlgData->config->refreshTimes
                : FormatHhMm(dlgData->config->refreshDailyMin);
            SetDlgItemTextW(hDlg, IDC_REFRESH_DAILY, dailyText.c_str());
            UpdateRefreshControls(hDlg);

            // Set burn-in prevention checkbox
            CheckDlgButton(hDlg, IDC_BURNIN_CHECK,
                           dlgData->config->burnInPrevention ? BST_CHECKED : BST_UNCHECKED);

            // Set auto-start checkbox
            CheckDlgButton(hDlg, IDC_AUTOSTART_CHECK,
                           dlgData->config->autoStart ? BST_CHECKED : BST_UNCHECKED);

            CheckDlgButton(hDlg, IDC_REMOTE_ENABLE,
                           dlgData->config->remoteEnabled ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextW(hDlg, IDC_REMOTE_BASE_URL, dlgData->config->remoteBaseUrl);

            // Auto update settings
            CheckDlgButton(hDlg, IDC_AUTO_UPDATE_CHECK,
                           dlgData->config->autoUpdate ? BST_CHECKED : BST_UNCHECKED);
            CheckRadioButton(hDlg, IDC_RADIO_UPDATE_GITHUB, IDC_RADIO_UPDATE_SELF,
                             dlgData->config->updateSource == AppConstants::UPDATE_SOURCE_GITHUB
                                 ? IDC_RADIO_UPDATE_GITHUB : IDC_RADIO_UPDATE_SELF);
            SetDlgItemTextW(hDlg, IDC_UPDATE_REPO, dlgData->config->updateRepo);
            // GitHub repo: prefill this project's release page when empty.
            if (dlgData->config->updateRepo[0] == L'\0' &&
                dlgData->config->autoUpdate &&
                dlgData->config->updateSource == AppConstants::UPDATE_SOURCE_GITHUB) {
                SetDlgItemTextW(hDlg, IDC_UPDATE_REPO, AppConstants::UPDATE_GITHUB_REPO_DEFAULT);
            }
            SetDlgItemTextW(hDlg, IDC_UPDATE_BASE_URL, dlgData->config->updateBaseUrl);
            SetDlgItemTextW(hDlg, IDC_UPDATE_WINDOW, dlgData->config->updateWindow);
            SendMessageW(GetDlgItem(hDlg, IDC_UPDATE_REPO), EM_SETLIMITTEXT, 127, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_UPDATE_BASE_URL), EM_SETLIMITTEXT, 1023, 0);
            SendMessageW(GetDlgItem(hDlg, IDC_UPDATE_WINDOW), EM_SETLIMITTEXT, 63, 0);
            UpdateUpdateControls(hDlg);

            CenterDialog(hDlg, GetParent(hDlg));
            SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(hDlg);
            SetActiveWindow(hDlg);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_RADIO_REFRESH_OFF:
        case IDC_RADIO_REFRESH_INTERVAL:
        case IDC_RADIO_REFRESH_DAILY:
            if (HIWORD(wParam) == BN_CLICKED) {
                UpdateRefreshControls(hDlg);
            }
            break;

        case IDC_AUTO_UPDATE_CHECK:
        case IDC_RADIO_UPDATE_GITHUB:
        case IDC_RADIO_UPDATE_SELF:
            if (HIWORD(wParam) == BN_CLICKED) {
                UpdateUpdateControls(hDlg);
                // Default the GitHub repo to this project's release page when
                // auto-update is on, GitHub is selected and the field is empty.
                if (IsDlgButtonChecked(hDlg, IDC_AUTO_UPDATE_CHECK) == BST_CHECKED &&
                    IsDlgButtonChecked(hDlg, IDC_RADIO_UPDATE_GITHUB) == BST_CHECKED) {
                    wchar_t repoBuf[128] = {};
                    GetDlgItemTextW(hDlg, IDC_UPDATE_REPO, repoBuf, 128);
                    if (repoBuf[0] == L'\0') {
                        SetDlgItemTextW(hDlg, IDC_UPDATE_REPO,
                                        AppConstants::UPDATE_GITHUB_REPO_DEFAULT);
                    }
                }
            }
            break;

        case IDOK:
            {
                // Validate URL
                wchar_t url[2048] = {};
                GetDlgItemTextW(hDlg, IDC_URL, url, 2048);

                std::wstring urlStr(url);
                if (urlStr.empty()) {
                    MessageBoxW(hDlg, L"Please enter a target URL.", L"Input Error",
                                MB_OK | MB_ICONWARNING);
                    SetFocus(GetDlgItem(hDlg, IDC_URL));
                    return TRUE;
                }
                if (urlStr.find(L"http://") != 0 && urlStr.find(L"https://") != 0) {
                    MessageBoxW(hDlg, L"URL must start with http:// or https://.",
                                L"Input Error", MB_OK | MB_ICONWARNING);
                    SetFocus(GetDlgItem(hDlg, IDC_URL));
                    return TRUE;
                }

                // Validate passwords match
                wchar_t pwd[256] = {};
                wchar_t confirmPwd[256] = {};
                GetDlgItemTextW(hDlg, IDC_PASSWORD, pwd, 256);
                GetDlgItemTextW(hDlg, IDC_CONFIRM_PASSWORD, confirmPwd, 256);

                if (wcscmp(pwd, confirmPwd) != 0) {
                    MessageBoxW(hDlg, L"Passwords do not match.", L"Input Error",
                                MB_OK | MB_ICONWARNING);
                    SetDlgItemTextW(hDlg, IDC_PASSWORD, L"");
                    SetDlgItemTextW(hDlg, IDC_CONFIRM_PASSWORD, L"");
                    SetFocus(GetDlgItem(hDlg, IDC_PASSWORD));
                    return TRUE;
                }

                // Save to config
                wcsncpy_s(dlgData->config->url, url, 2047);

                // Get zoom
                BOOL translated = FALSE;
                int zoom = (int)GetDlgItemInt(hDlg, IDC_ZOOM, &translated, FALSE);
                if (!translated) zoom = AppConstants::ZOOM_DEFAULT;
                if (zoom < AppConstants::ZOOM_MIN) zoom = AppConstants::ZOOM_MIN;
                if (zoom > AppConstants::ZOOM_MAX) zoom = AppConstants::ZOOM_MAX;
                dlgData->config->zoomPercent = zoom;

                // Get refresh settings (mode + interval or daily time)
                int refreshMode = GetSelectedRefreshMode(hDlg);

                int refreshSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
                if (refreshMode == AppConstants::REFRESH_MODE_INTERVAL) {
                    BOOL intervalOk = FALSE;
                    refreshSec = (int)GetDlgItemInt(hDlg, IDC_REFRESH_INTERVAL, &intervalOk, FALSE);
                    if (!intervalOk || refreshSec < AppConstants::REFRESH_INTERVAL_MIN) {
                        MessageBoxW(hDlg, L"Please enter a refresh interval of at least 1 second.",
                                    L"Input Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_REFRESH_INTERVAL));
                        return TRUE;
                    }
                    if (refreshSec > AppConstants::REFRESH_INTERVAL_MAX) refreshSec = AppConstants::REFRESH_INTERVAL_MAX;
                }

                int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;
                wchar_t dailyBuf[256] = {};
                if (refreshMode == AppConstants::REFRESH_MODE_DAILY) {
                    GetDlgItemTextW(hDlg, IDC_REFRESH_DAILY, dailyBuf, 256);
                    std::vector<int> dailyMins;
                    if (!RefreshSchedule::ParseTimes(dailyBuf, dailyMins)) {
                        MessageBoxW(hDlg, L"Please enter one or more daily refresh times in HH:MM (24h) "
                                    L"format, comma-separated, e.g. 08:00,16:00,24:00.",
                                    L"Input Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_REFRESH_DAILY));
                        return TRUE;
                    }
                    refreshDailyMin = dailyMins.front();
                }

                dlgData->config->refreshMode = refreshMode;
                dlgData->config->refreshIntervalSec = refreshSec;
                dlgData->config->refreshDailyMin = refreshDailyMin;
                // Keep the raw list; when DAILY is off this clears any stale list.
                wcsncpy_s(dlgData->config->refreshTimes, _countof(dlgData->config->refreshTimes),
                          dailyBuf, _TRUNCATE);

                // Get burn-in prevention
                dlgData->config->burnInPrevention =
                    (IsDlgButtonChecked(hDlg, IDC_BURNIN_CHECK) == BST_CHECKED);

                dlgData->config->remoteEnabled =
                    (IsDlgButtonChecked(hDlg, IDC_REMOTE_ENABLE) == BST_CHECKED);

                dlgData->config->autoStart =
                    (IsDlgButtonChecked(hDlg, IDC_AUTOSTART_CHECK) == BST_CHECKED);

                wchar_t remoteBaseUrl[1024] = {};
                GetDlgItemTextW(hDlg, IDC_REMOTE_BASE_URL, remoteBaseUrl, 1024);
                if (dlgData->config->remoteEnabled) {
                    std::wstring remoteUrl(remoteBaseUrl);
                    if (remoteUrl.empty()) {
                        MessageBoxW(hDlg, L"Please enter a remote server URL when remote configuration is enabled.",
                                    L"Input Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_REMOTE_BASE_URL));
                        return TRUE;
                    }
                    if (remoteUrl.find(L"http://") != 0 && remoteUrl.find(L"https://") != 0) {
                        MessageBoxW(hDlg, L"Remote server URL must start with http:// or https://.",
                                    L"Input Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_REMOTE_BASE_URL));
                        return TRUE;
                    }
                }
                wcsncpy_s(dlgData->config->remoteBaseUrl, remoteBaseUrl, _TRUNCATE);

                // Auto update settings
                dlgData->config->autoUpdate =
                    (IsDlgButtonChecked(hDlg, IDC_AUTO_UPDATE_CHECK) == BST_CHECKED);
                dlgData->config->updateSource =
                    (IsDlgButtonChecked(hDlg, IDC_RADIO_UPDATE_GITHUB) == BST_CHECKED)
                        ? AppConstants::UPDATE_SOURCE_GITHUB
                        : AppConstants::UPDATE_SOURCE_SELF;

                wchar_t updateRepo[128] = {};
                wchar_t updateBaseUrl[1024] = {};
                GetDlgItemTextW(hDlg, IDC_UPDATE_REPO, updateRepo, 128);
                GetDlgItemTextW(hDlg, IDC_UPDATE_BASE_URL, updateBaseUrl, 1024);
                GetDlgItemTextW(hDlg, IDC_UPDATE_WINDOW,
                                dlgData->config->updateWindow, 64);

                if (dlgData->config->autoUpdate) {
                    if (dlgData->config->updateSource == AppConstants::UPDATE_SOURCE_GITHUB) {
                        std::wstring repoStr(updateRepo);
                        if (repoStr.empty() || repoStr.find(L'/') == std::wstring::npos) {
                            MessageBoxW(hDlg, L"Enter the GitHub repo as owner/repo "
                                        L"(e.g. owner/FullScreen).",
                                        L"Input Error", MB_OK | MB_ICONWARNING);
                            SetFocus(GetDlgItem(hDlg, IDC_UPDATE_REPO));
                            return TRUE;
                        }
                    } else {
                        std::wstring updateUrlStr(updateBaseUrl);
                        if (updateUrlStr.empty()) {
                            MessageBoxW(hDlg, L"Enter the self-hosted update directory URL.",
                                        L"Input Error", MB_OK | MB_ICONWARNING);
                            SetFocus(GetDlgItem(hDlg, IDC_UPDATE_BASE_URL));
                            return TRUE;
                        }
                        if (updateUrlStr.find(L"http://") != 0 && updateUrlStr.find(L"https://") != 0) {
                            MessageBoxW(hDlg, L"Update URL must start with http:// or https://.",
                                        L"Input Error", MB_OK | MB_ICONWARNING);
                            SetFocus(GetDlgItem(hDlg, IDC_UPDATE_BASE_URL));
                            return TRUE;
                        }
                    }
                    // The maintenance window is not validated here; a malformed
                    // value fails closed at runtime (updates simply don't apply).
                }
                wcsncpy_s(dlgData->config->updateRepo, updateRepo, _TRUNCATE);
                wcsncpy_s(dlgData->config->updateBaseUrl, updateBaseUrl, _TRUNCATE);

                // Get unreachable message
                GetDlgItemTextW(hDlg, IDC_UNREACHABLE_MSG,
                                dlgData->config->unreachableMsg, 1024);

                // Store plain password
                std::wstring plainPwd(pwd);
                *dlgData->plainPassword = std::move(plainPwd);

                // Save to file
                if (ConfigManager::SaveConfig(*dlgData->config, *dlgData->plainPassword)) {
                    // Apply the HKCU Run entry: register when checked, remove when unchecked.
                    // Failing here is non-fatal (config is already saved); just warn.
                    if (!AutoStart::SetAutoStart(dlgData->config->autoStart)) {
                        MessageBoxW(hDlg, L"Failed to update the auto-start registry entry.",
                                    AppConstants::APP_NAME, MB_OK | MB_ICONWARNING);
                    }

                    // Non-blocking preflight: warn if the remote server is
                    // unreachable, but still allow the save (offline deploy).
                    // Remote eligibility is re-evaluated at runtime.
                    if (dlgData->config->remoteEnabled &&
                        !RemoteConfigClient::CheckServerReachable(dlgData->config->remoteBaseUrl)) {
                        std::wstring warnMsg = L"Remote configuration server is not reachable:\n\n";
                        warnMsg += dlgData->config->remoteBaseUrl;
                        warnMsg += L"\n\nThe configuration was saved; local settings remain active.\n"
                                   L"Remote sync will be re-checked when the program runs.";
                        MessageBoxW(hDlg, warnMsg.c_str(), AppConstants::APP_NAME,
                                    MB_OK | MB_ICONWARNING);
                    }
                    dlgData->saved = true;
                    EndDialog(hDlg, IDOK);
                } else {
                    MessageBoxW(hDlg, L"Failed to save config file. Check disk space and permissions.",
                                L"Save Failed", MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;

        case IDCANCEL:
            dlgData->saved = false;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case IDC_BTN_TEST_URL:
            {
                wchar_t url[2048] = {};
                GetDlgItemTextW(hDlg, IDC_URL, url, 2048);

                // Disable button during test
                HWND hBtn = GetDlgItem(hDlg, IDC_BTN_TEST_URL);
                EnableWindow(hBtn, FALSE);
                SetDlgItemTextW(hDlg, IDC_URL_STATUS, L"Testing URL reachability...");

                // Check URL asynchronously (simple approach: synchronous in dialog)
                // Since we're in a modal dialog, do a quick synchronous check
                bool reachable = false;
                {
                    std::wstring urlStr(url);
                    HINTERNET hSession = WinHttpOpen(L"FullScreenBrowser/1.0",
                                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                      WINHTTP_NO_PROXY_NAME,
                                                      WINHTTP_NO_PROXY_BYPASS, 0);
                    if (hSession) {
                        DWORD timeout = 5000;
                        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
                        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

                        // Parse URL
                        URL_COMPONENTS urlComp = {};
                        urlComp.dwStructSize = sizeof(urlComp);
                        wchar_t host[256] = {}, path[2048] = { L"/" };
                        urlComp.lpszHostName = host;
                        urlComp.dwHostNameLength = 256;
                        urlComp.lpszUrlPath = path;
                        urlComp.dwUrlPathLength = 2048;

                        if (WinHttpCrackUrl(urlStr.c_str(), 0, 0, &urlComp)) {
                            bool isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
                            INTERNET_PORT port = urlComp.nPort ? urlComp.nPort :
                                                  (isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

                            HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
                            if (hConnect) {
                                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", path,
                                                                         nullptr, WINHTTP_NO_REFERER,
                                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                         isHttps ? WINHTTP_FLAG_SECURE : 0);
                                if (hRequest) {
                                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                                        WinHttpReceiveResponse(hRequest, nullptr)) {
                                        DWORD statusCode = 0;
                                        DWORD size = sizeof(statusCode);
                                        WinHttpQueryHeaders(hRequest,
                                                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                                             WINHTTP_HEADER_NAME_BY_INDEX,
                                                             &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
                                        reachable = (statusCode >= 200 && statusCode < 400);
                                    }
                                    WinHttpCloseHandle(hRequest);
                                }
                                WinHttpCloseHandle(hConnect);
                            }
                        }
                        WinHttpCloseHandle(hSession);
                    }
                }

                EnableWindow(hBtn, TRUE);
                if (reachable) {
                    SetDlgItemTextW(hDlg, IDC_URL_STATUS, L"URL is reachable.");
                    MessageBoxW(hDlg, L"URL is reachable!", L"Test Result",
                                MB_OK | MB_ICONINFORMATION);
                } else {
                    SetDlgItemTextW(hDlg, IDC_URL_STATUS, L"URL is NOT reachable. You can still save the settings.");
                    MessageBoxW(hDlg, L"URL is not reachable.\n\nYou can still save the settings and try again later.",
                                L"Test Result", MB_OK | MB_ICONWARNING);
                }
            }
            return TRUE;
        }
        break;
    }

    return FALSE;
}

} // anonymous namespace

// ============================================================
// Password Dialog
// ============================================================
namespace {

struct PasswordDialogData {
    std::wstring_view correctPassword;
    bool verified;
};

static INT_PTR CALLBACK PasswordDialogProcImpl(HWND hDlg, UINT msg,
                                                WPARAM wParam, LPARAM lParam)
{
    static PasswordDialogData* dlgData = nullptr;
    static int attemptCount = 0;

    switch (msg) {
    case WM_INITDIALOG:
        dlgData = reinterpret_cast<PasswordDialogData*>(lParam);
        attemptCount = 0;
        SendMessageW(GetDlgItem(hDlg, IDC_PWD_INPUT), EM_SETLIMITTEXT, 255, 0);
        CenterDialog(hDlg, GetParent(hDlg));
        SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hDlg);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_PWD_INPUT));
        return FALSE;

    case WM_ACTIVATE:
        // Keep the password dialog on top while active, in case the fullscreen
        // owner window (WS_EX_TOPMOST) competes for Z-order.
        if (LOWORD(wParam) != WA_INACTIVE) {
            SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            {
                wchar_t input[256] = {};
                GetDlgItemTextW(hDlg, IDC_PWD_INPUT, input, 256);

                if (wcscmp(input, dlgData->correctPassword.data()) == 0) {
                    dlgData->verified = true;
                    EndDialog(hDlg, IDOK);
                } else {
                    attemptCount++;
                    if (attemptCount >= AppConstants::MAX_PWD_ATTEMPTS) {
                        MessageBoxW(hDlg, L"Too many failed attempts. Please try again later.",
                                    L"Verification Failed", MB_OK | MB_ICONWARNING);
                        dlgData->verified = false;
                        EndDialog(hDlg, IDCANCEL);
                    } else {
                        std::wstring attemptMsg = L"Incorrect password (attempts remaining: ";
                        attemptMsg += std::to_wstring(AppConstants::MAX_PWD_ATTEMPTS - attemptCount);
                        attemptMsg += L")";
                        MessageBoxW(hDlg, attemptMsg.c_str(), L"Verification Failed",
                                    MB_OK | MB_ICONWARNING);
                        SetDlgItemTextW(hDlg, IDC_PWD_INPUT, L"");
                        SetFocus(GetDlgItem(hDlg, IDC_PWD_INPUT));
                    }
                }
            }
            return TRUE;

        case IDCANCEL:
            dlgData->verified = false;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

} // anonymous namespace

// ============================================================
// Menu Dialog
// ============================================================
namespace {

struct MenuDialogData {
    int choice; // 0=cancel, 1=exit, 2=settings
};

static INT_PTR CALLBACK MenuDialogProcImpl(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    static MenuDialogData* dlgData = nullptr;

    switch (msg) {
    case WM_INITDIALOG:
        dlgData = reinterpret_cast<MenuDialogData*>(lParam);
        dlgData->choice = 0;
        CenterDialog(hDlg, GetParent(hDlg));
        SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hDlg);
        SetActiveWindow(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_EXIT:
            dlgData->choice = 1;
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDC_BTN_SETTINGS:
            dlgData->choice = 2;
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDC_BTN_CANCEL:
        case IDCANCEL:
            dlgData->choice = 0;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

} // anonymous namespace

// ============================================================
// Register Dialog
// ============================================================
namespace {

struct RegisterDialogData {
    std::wstring* registerCode;
    bool accepted;
    bool allowCancel;
};

static INT_PTR CALLBACK RegisterDialogProcImpl(HWND hDlg, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    static RegisterDialogData* dlgData = nullptr;

    switch (msg) {
    case WM_INITDIALOG:
        dlgData = reinterpret_cast<RegisterDialogData*>(lParam);
        dlgData->accepted = false;
        SendMessageW(GetDlgItem(hDlg, IDC_REGISTER_CODE), EM_SETLIMITTEXT, 255, 0);
        if (!dlgData->allowCancel) {
            // Mandatory registration: hide Cancel, ignore Esc / title-bar close.
            ShowWindow(GetDlgItem(hDlg, IDCANCEL), SW_HIDE);
        }
        CenterDialog(hDlg, GetParent(hDlg));
        SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hDlg);
        SetActiveWindow(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            {
                wchar_t code[256] = {};
                GetDlgItemTextW(hDlg, IDC_REGISTER_CODE, code, 256);
                std::wstring value(code);
                value.erase(0, value.find_first_not_of(L" \t\r\n"));
                value.erase(value.find_last_not_of(L" \t\r\n") == std::wstring::npos ? 0 : value.find_last_not_of(L" \t\r\n") + 1);
                if (value.empty()) {
                    MessageBoxW(hDlg, L"Please enter a registration code.",
                                L"Input Error", MB_OK | MB_ICONWARNING);
                    SetFocus(GetDlgItem(hDlg, IDC_REGISTER_CODE));
                    return TRUE;
                }
                *dlgData->registerCode = std::move(value);
                dlgData->accepted = true;
                EndDialog(hDlg, IDOK);
            }
            return TRUE;

        case IDCANCEL:
            if (!dlgData->allowCancel) {
                return TRUE; // mandatory registration: cancel is not allowed
            }
            dlgData->accepted = false;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (!dlgData->allowCancel) {
            return TRUE; // swallow title-bar close during mandatory registration
        }
        break;
    }

    return FALSE;
}

} // anonymous namespace

// ============================================================
// Public API
// ============================================================

namespace Dialogs {

bool ShowConfigDialog(HINSTANCE hInstance, HWND hParent, AppConfig& config,
                      std::wstring& plainPassword)
{
    ConfigDialogData data;
    data.config = &config;
    data.plainPassword = &plainPassword;
    data.saved = false;

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONFIG_DIALOG),
                    hParent, ConfigDialogProcImpl,
                    reinterpret_cast<LPARAM>(&data));
    return data.saved;
}

bool ShowPasswordDialog(HINSTANCE hInstance, HWND hParent,
                        std::wstring_view correctPassword)
{
    PasswordDialogData data;
    data.correctPassword = correctPassword;
    data.verified = false;

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_PASSWORD_DIALOG),
                     hParent, PasswordDialogProcImpl,
                     reinterpret_cast<LPARAM>(&data));
    return data.verified;
}

int ShowMenuDialog(HINSTANCE hInstance, HWND hParent)
{
    MenuDialogData data;
    data.choice = 0;

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MENU_DIALOG),
                     hParent, MenuDialogProcImpl,
                     reinterpret_cast<LPARAM>(&data));
    return data.choice;
}

bool ShowRegisterCodeDialog(HINSTANCE hInstance, HWND hParent,
                            bool allowCancel,
                            std::wstring& registerCode)
{
    RegisterDialogData data;
    data.registerCode = &registerCode;
    data.accepted = false;
    data.allowCancel = allowCancel;

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_REGISTER_DIALOG),
                    hParent, RegisterDialogProcImpl,
                    reinterpret_cast<LPARAM>(&data));
    return data.accepted;
}

} // namespace Dialogs