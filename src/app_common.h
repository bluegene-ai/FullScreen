#pragma once
#include <string>
#include <string_view>

// ============================================================
// Application-wide constants
// ============================================================

namespace AppConstants {
    inline constexpr wchar_t APP_NAME[] = L"FullScreenBrowser";
    inline constexpr wchar_t CONFIG_DIR[] = L"FullScreenBrowser";
    inline constexpr wchar_t CONFIG_FILE[] = L"config.dat";
    inline constexpr wchar_t WEBVIEW2_DIR[] = L"WebView2";
    inline constexpr wchar_t MUTEX_NAME[] = L"Global\\FullScreenBrowser_SingleInstance";
    inline constexpr wchar_t HOTKEY_DESC[] = L"ESC / Alt+F4";

    // Magic number for config file validation
    inline constexpr unsigned int CONFIG_MAGIC = 0x46534243; // "FSBC"
    // v7 adds auto-update settings (autoUpdate/updateSource/...).
    // LoadConfig migrates v3-v6.
    inline constexpr unsigned int CONFIG_VERSION = 7;

    // Zoom range
    inline constexpr int ZOOM_MIN = 50;
    inline constexpr int ZOOM_MAX = 300;
    inline constexpr int ZOOM_DEFAULT = 100;

    // URL check interval (ms)
    inline constexpr unsigned int URL_CHECK_INTERVAL = 30000;

    // Max password attempts before cooldown
    inline constexpr int MAX_PWD_ATTEMPTS = 5;
    inline constexpr unsigned int PWD_COOLDOWN_MS = 30000;

    // Cursor hide interval (ms) - re-hide periodically
    inline constexpr unsigned int CURSOR_HIDE_INTERVAL = 2000;

    // Pixel shift interval (ms) for burn-in prevention
    inline constexpr unsigned int PIXEL_SHIFT_INTERVAL = 120000; // 2 min
    inline constexpr int PIXEL_SHIFT_RANGE = 4;   // shift +/-4 pixels max

    // Auto-refresh modes
    inline constexpr int REFRESH_MODE_OFF = 0;        // disabled
    inline constexpr int REFRESH_MODE_INTERVAL = 1;   // every N seconds
    inline constexpr int REFRESH_MODE_DAILY = 2;      // at a specific time each day

    // Auto-refresh defaults
    inline constexpr int REFRESH_MODE_DEFAULT = REFRESH_MODE_OFF;
    inline constexpr int REFRESH_INTERVAL_MIN = 1;
    inline constexpr int REFRESH_INTERVAL_MAX = 86400; // max 24 hours
    inline constexpr int REFRESH_INTERVAL_DEFAULT = 0;
    inline constexpr int REFRESH_DAILY_DEFAULT = 0;    // minutes since midnight (00:00)

    // Remote config polling defaults (base + random[0..jitter])
    inline constexpr int REMOTE_POLL_BASE_SEC_DEFAULT = 30;
    inline constexpr int REMOTE_POLL_JITTER_SEC_DEFAULT = 30;
    inline constexpr int REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT = 600;

    // Auto update (v7+)
    inline constexpr int UPDATE_SOURCE_GITHUB = 0; // GitHub releases (daily check)
    inline constexpr int UPDATE_SOURCE_SELF   = 1; // self-hosted server (default)
    inline constexpr int UPDATE_SOURCE_DEFAULT = UPDATE_SOURCE_SELF;

    // Self-hosted manifest name inside the configured update directory.
    inline constexpr wchar_t UPDATE_MANIFEST[] = L"latest.txt";
    // Fixed asset name on GitHub releases and self-hosted update dir.
    inline constexpr wchar_t UPDATE_EXE_NAME[] = L"FullScreenBrowser.exe";

    // Throttle: self-hosted checks at most every 6h; GitHub at most once a day.
    inline constexpr long long UPDATE_SELF_INTERVAL_SEC = 6 * 3600;
    inline constexpr long long UPDATE_GITHUB_INTERVAL_SEC = 24 * 3600;

    // Default GitHub repo for the update source (this project's release page).
    inline constexpr wchar_t UPDATE_GITHUB_REPO_DEFAULT[] = L"bluegene-ai/FullScreen";

    // Startup crash-protection: after applying an update the new exe must
    // stay alive for UPDATE_CONFIRM_SEC before it is trusted; otherwise it is
    // rolled back after UPDATE_MAX_STARTUP_ATTEMPTS consecutive failed starts.
    inline constexpr int UPDATE_MAX_STARTUP_ATTEMPTS = 3;
    inline constexpr unsigned int UPDATE_CONFIRM_SEC = 60;

    // Timer cadence for the auto-update check (re-checked against the
    // throttle file inside AutoUpdate, so this is just a low-cost poll).
    inline constexpr unsigned int UPDATE_POLL_MS = 3600 * 1000; // 1h
    inline constexpr unsigned int UPDATE_INITIAL_DELAY_MS = 10000; // 10s after start
}

// ============================================================
// Application configuration structure
// ============================================================
struct AppConfig {
    wchar_t url[2048] = {};
    wchar_t encryptedPassword[512] = {};
    wchar_t unreachableMsg[1024] = {};
    int zoomPercent = AppConstants::ZOOM_DEFAULT;
    int refreshMode = AppConstants::REFRESH_MODE_DEFAULT;
    int refreshIntervalSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
    int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;

    // Daily multi-time refresh: comma-separated "HH:MM" list, e.g. "08:00,16:00,24:00" (v6+)
    wchar_t refreshTimes[256] = {};

    bool burnInPrevention = false;

    // Auto-start at Windows logon (HKCU Run key, no admin needed)
    bool autoStart = false;

    // Remote configuration settings (v4+)
    bool remoteEnabled = false;
    wchar_t remoteBaseUrl[1024] = {};
    wchar_t deviceId[128] = {};
    int configRevision = 0;
    // Server-gated: the server only delivers password-update commands when
    // this is true in the merged config. The client persists/echoes it but
    // never consumes it locally.
    bool allowRemotePasswordUpdate = false;
    int pollBaseSec = AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int pollJitterSec = AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int pollMaxBackoffSec = AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;

    // Auto update settings (v7+)
    bool autoUpdate = false;
    int updateSource = AppConstants::UPDATE_SOURCE_DEFAULT;
    wchar_t updateRepo[128] = {};      // GitHub source: "owner/repo"
    wchar_t updateBaseUrl[1024] = {};  // self-hosted source: base update dir URL
    wchar_t updateWindow[64] = {};     // maintenance window "HH:MM-HH:MM"; empty = anytime

    bool IsValid() const {
        return url[0] != L'\0';
    }
};
