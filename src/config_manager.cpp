// ============================================================
// ConfigManager - read/write config from %APPDATA%
// ============================================================
#include "config_manager.h"
#include "crypto.h"
#include <Windows.h>
#include <ShlObj.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "Crypt32.lib")

namespace {

// v3 persisted layout kept for backward-compatible migration into v4 AppConfig.
struct AppConfigV3 {
    wchar_t url[2048] = {};
    wchar_t encryptedPassword[512] = {};
    wchar_t unreachableMsg[1024] = {};
    int zoomPercent = AppConstants::ZOOM_DEFAULT;
    int refreshMode = AppConstants::REFRESH_MODE_DEFAULT;
    int refreshIntervalSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
    int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;
    bool burnInPrevention = false;
};

// v4 persisted layout kept for backward-compatible migration into v5 AppConfig.
struct AppConfigV4 {
    wchar_t url[2048] = {};
    wchar_t encryptedPassword[512] = {};
    wchar_t unreachableMsg[1024] = {};
    int zoomPercent = AppConstants::ZOOM_DEFAULT;
    int refreshMode = AppConstants::REFRESH_MODE_DEFAULT;
    int refreshIntervalSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
    int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;
    bool burnInPrevention = false;
    bool remoteEnabled = false;
    wchar_t remoteBaseUrl[1024] = {};
    wchar_t deviceId[128] = {};
    int configRevision = 0;
    bool allowRemotePasswordUpdate = false;
    int pollBaseSec = AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int pollJitterSec = AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int pollMaxBackoffSec = AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;
};

// v5 persisted layout kept for backward-compatible migration into v6 AppConfig.
struct AppConfigV5 {
    wchar_t url[2048] = {};
    wchar_t encryptedPassword[512] = {};
    wchar_t unreachableMsg[1024] = {};
    int zoomPercent = AppConstants::ZOOM_DEFAULT;
    int refreshMode = AppConstants::REFRESH_MODE_DEFAULT;
    int refreshIntervalSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
    int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;
    bool burnInPrevention = false;
    bool autoStart = false;
    bool remoteEnabled = false;
    wchar_t remoteBaseUrl[1024] = {};
    wchar_t deviceId[128] = {};
    int configRevision = 0;
    bool allowRemotePasswordUpdate = false;
    int pollBaseSec = AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int pollJitterSec = AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int pollMaxBackoffSec = AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;
};

// v6 persisted layout kept for backward-compatible migration into v7 AppConfig.
// Matches the pre-v7 AppConfig exactly (auto-update fields are v7 additions).
struct AppConfigV6 {
    wchar_t url[2048] = {};
    wchar_t encryptedPassword[512] = {};
    wchar_t unreachableMsg[1024] = {};
    int zoomPercent = AppConstants::ZOOM_DEFAULT;
    int refreshMode = AppConstants::REFRESH_MODE_DEFAULT;
    int refreshIntervalSec = AppConstants::REFRESH_INTERVAL_DEFAULT;
    int refreshDailyMin = AppConstants::REFRESH_DAILY_DEFAULT;
    wchar_t refreshTimes[256] = {};
    bool burnInPrevention = false;
    bool autoStart = false;
    bool remoteEnabled = false;
    wchar_t remoteBaseUrl[1024] = {};
    wchar_t deviceId[128] = {};
    int configRevision = 0;
    bool allowRemotePasswordUpdate = false;
    int pollBaseSec = AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int pollJitterSec = AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int pollMaxBackoffSec = AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;
};

static FILE* OpenFile(const std::wstring& path, const wchar_t* mode)
{
    FILE* file = nullptr;
    return (_wfopen_s(&file, path.c_str(), mode) == 0) ? file : nullptr;
}

} // namespace

namespace ConfigManager {

std::wstring GetConfigFilePath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"";
    }

    std::wstring path = appData;
    path += L"\\";
    path += AppConstants::CONFIG_DIR;
    path += L"\\";
    path += AppConstants::CONFIG_FILE;
    return path;
}

std::wstring GetWebView2DataPath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"";
    }

    std::wstring path = appData;
    path += L"\\";
    path += AppConstants::CONFIG_DIR;
    path += L"\\";
    path += AppConstants::WEBVIEW2_DIR;
    return path;
}

std::wstring GetRemoteTokenPath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"";
    }

    std::wstring path = appData;
    path += L"\\";
    path += AppConstants::CONFIG_DIR;
    path += L"\\remote_token.dat";
    return path;
}

static std::wstring GetConsumedCommandsPath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"";
    }

    std::wstring path = appData;
    path += L"\\";
    path += AppConstants::CONFIG_DIR;
    path += L"\\consumed_commands.dat";
    return path;
}

bool EnsureConfigDir()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return false;
    }

    std::wstring dir = appData;
    dir += L"\\";
    dir += AppConstants::CONFIG_DIR;

    CreateDirectoryW(dir.c_str(), nullptr);
    return (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES);
}

bool LoadConfig(AppConfig& config)
{
    std::wstring path = GetConfigFilePath();
    if (path.empty()) return false;

    FILE* f = OpenFile(path, L"rb");
    if (!f) return false;

    // Read header
    unsigned int magic = 0, version = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1) {
        fclose(f);
        return false;
    }

    // Validate magic first
    if (magic != AppConstants::CONFIG_MAGIC) {
        fclose(f);
        return false;
    }

    if (version == AppConstants::CONFIG_VERSION) {
        // Read current config body (v4)
        if (fread(&config, sizeof(AppConfig), 1, f) != 1) {
            fclose(f);
            return false;
        }
    } else if (version == 3) {
        // Migrate v3 layout into v4 in-memory config.
        AppConfigV3 oldCfg = {};
        if (fread(&oldCfg, sizeof(oldCfg), 1, f) != 1) {
            fclose(f);
            return false;
        }

        config = AppConfig{};
        wcsncpy_s(config.url, oldCfg.url, _TRUNCATE);
        wcsncpy_s(config.encryptedPassword, oldCfg.encryptedPassword, _TRUNCATE);
        wcsncpy_s(config.unreachableMsg, oldCfg.unreachableMsg, _TRUNCATE);
        config.zoomPercent = oldCfg.zoomPercent;
        config.refreshMode = oldCfg.refreshMode;
        config.refreshIntervalSec = oldCfg.refreshIntervalSec;
        config.refreshDailyMin = oldCfg.refreshDailyMin;
        config.burnInPrevention = oldCfg.burnInPrevention;
    } else if (version == 5) {
        // Migrate v5 layout into v6 (refreshTimes defaults to empty list).
        AppConfigV5 oldCfg = {};
        if (fread(&oldCfg, sizeof(oldCfg), 1, f) != 1) {
            fclose(f);
            return false;
        }

        config = AppConfig{};
        wcsncpy_s(config.url, oldCfg.url, _TRUNCATE);
        wcsncpy_s(config.encryptedPassword, oldCfg.encryptedPassword, _TRUNCATE);
        wcsncpy_s(config.unreachableMsg, oldCfg.unreachableMsg, _TRUNCATE);
        config.zoomPercent = oldCfg.zoomPercent;
        config.refreshMode = oldCfg.refreshMode;
        config.refreshIntervalSec = oldCfg.refreshIntervalSec;
        config.refreshDailyMin = oldCfg.refreshDailyMin;
        config.burnInPrevention = oldCfg.burnInPrevention;
        config.autoStart = oldCfg.autoStart;
        config.remoteEnabled = oldCfg.remoteEnabled;
        wcsncpy_s(config.remoteBaseUrl, oldCfg.remoteBaseUrl, _TRUNCATE);
        wcsncpy_s(config.deviceId, oldCfg.deviceId, _TRUNCATE);
        config.configRevision = oldCfg.configRevision;
        config.allowRemotePasswordUpdate = oldCfg.allowRemotePasswordUpdate;
        config.pollBaseSec = oldCfg.pollBaseSec;
        config.pollJitterSec = oldCfg.pollJitterSec;
        config.pollMaxBackoffSec = oldCfg.pollMaxBackoffSec;
    } else if (version == 6) {
        // Migrate v6 layout into v7 (auto-update fields default off / self-hosted).
        AppConfigV6 oldCfg = {};
        if (fread(&oldCfg, sizeof(oldCfg), 1, f) != 1) {
            fclose(f);
            return false;
        }

        config = AppConfig{};
        wcsncpy_s(config.url, oldCfg.url, _TRUNCATE);
        wcsncpy_s(config.encryptedPassword, oldCfg.encryptedPassword, _TRUNCATE);
        wcsncpy_s(config.unreachableMsg, oldCfg.unreachableMsg, _TRUNCATE);
        config.zoomPercent = oldCfg.zoomPercent;
        config.refreshMode = oldCfg.refreshMode;
        config.refreshIntervalSec = oldCfg.refreshIntervalSec;
        config.refreshDailyMin = oldCfg.refreshDailyMin;
        wcsncpy_s(config.refreshTimes, oldCfg.refreshTimes, _TRUNCATE);
        config.burnInPrevention = oldCfg.burnInPrevention;
        config.autoStart = oldCfg.autoStart;
        config.remoteEnabled = oldCfg.remoteEnabled;
        wcsncpy_s(config.remoteBaseUrl, oldCfg.remoteBaseUrl, _TRUNCATE);
        wcsncpy_s(config.deviceId, oldCfg.deviceId, _TRUNCATE);
        config.configRevision = oldCfg.configRevision;
        config.allowRemotePasswordUpdate = oldCfg.allowRemotePasswordUpdate;
        config.pollBaseSec = oldCfg.pollBaseSec;
        config.pollJitterSec = oldCfg.pollJitterSec;
        config.pollMaxBackoffSec = oldCfg.pollMaxBackoffSec;
        // autoUpdate stays false (v7 default): updates require explicit opt-in.
    } else if (version == 4) {
        // Migrate v4 layout into v5 in-memory config (autoStart defaults to off).
        AppConfigV4 oldCfg = {};
        if (fread(&oldCfg, sizeof(oldCfg), 1, f) != 1) {
            fclose(f);
            return false;
        }

        config = AppConfig{};
        wcsncpy_s(config.url, oldCfg.url, _TRUNCATE);
        wcsncpy_s(config.encryptedPassword, oldCfg.encryptedPassword, _TRUNCATE);
        wcsncpy_s(config.unreachableMsg, oldCfg.unreachableMsg, _TRUNCATE);
        config.zoomPercent = oldCfg.zoomPercent;
        config.refreshMode = oldCfg.refreshMode;
        config.refreshIntervalSec = oldCfg.refreshIntervalSec;
        config.refreshDailyMin = oldCfg.refreshDailyMin;
        config.burnInPrevention = oldCfg.burnInPrevention;
        config.remoteEnabled = oldCfg.remoteEnabled;
        wcsncpy_s(config.remoteBaseUrl, oldCfg.remoteBaseUrl, _TRUNCATE);
        wcsncpy_s(config.deviceId, oldCfg.deviceId, _TRUNCATE);
        config.configRevision = oldCfg.configRevision;
        config.allowRemotePasswordUpdate = oldCfg.allowRemotePasswordUpdate;
        config.pollBaseSec = oldCfg.pollBaseSec;
        config.pollJitterSec = oldCfg.pollJitterSec;
        config.pollMaxBackoffSec = oldCfg.pollMaxBackoffSec;
    } else {
        fclose(f);
        return false;
    }

    fclose(f);
    return config.IsValid();
}

bool SaveConfig(const AppConfig& config, std::wstring_view plainPassword)
{
    if (!EnsureConfigDir()) return false;

    // Encrypt password
    AppConfig toSave = config;
    std::wstring encrypted = Crypto::Encrypt(plainPassword);
    wcsncpy_s(toSave.encryptedPassword, encrypted.c_str(), 511);

    std::wstring path = GetConfigFilePath();
    if (path.empty()) return false;

    FILE* f = OpenFile(path, L"wb");
    if (!f) return false;

    // Write header
    unsigned int magic = AppConstants::CONFIG_MAGIC;
    unsigned int version = AppConstants::CONFIG_VERSION;
    bool ok = (fwrite(&magic, sizeof(magic), 1, f) == 1 &&
               fwrite(&version, sizeof(version), 1, f) == 1 &&
               fwrite(&toSave, sizeof(AppConfig), 1, f) == 1);

    fclose(f);
    return ok;
}

bool SaveRemoteToken(std::wstring_view token)
{
    if (!EnsureConfigDir()) return false;

    std::wstring path = GetRemoteTokenPath();
    if (path.empty()) return false;

    DATA_BLOB inBlob = {};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(token.data()));
    inBlob.cbData = static_cast<DWORD>(token.size() * sizeof(wchar_t));

    DATA_BLOB outBlob = {};
    if (!CryptProtectData(&inBlob, L"FullScreenBrowserRemoteToken", nullptr, nullptr, nullptr, 0, &outBlob)) {
        return false;
    }

    FILE* f = OpenFile(path, L"wb");
    if (!f) {
        LocalFree(outBlob.pbData);
        return false;
    }

    bool ok = (fwrite(&outBlob.cbData, sizeof(outBlob.cbData), 1, f) == 1 &&
               fwrite(outBlob.pbData, 1, outBlob.cbData, f) == outBlob.cbData);
    fclose(f);
    LocalFree(outBlob.pbData);
    return ok;
}

bool LoadRemoteToken(std::wstring& token)
{
    token.clear();

    std::wstring path = GetRemoteTokenPath();
    if (path.empty()) return false;

    FILE* f = OpenFile(path, L"rb");
    if (!f) return false;

    DWORD size = 0;
    if (fread(&size, sizeof(size), 1, f) != 1 || size == 0) {
        fclose(f);
        return false;
    }

    std::vector<BYTE> encrypted(size);
    if (fread(encrypted.data(), 1, size, f) != size) {
        fclose(f);
        return false;
    }
    fclose(f);

    DATA_BLOB inBlob = {};
    inBlob.pbData = encrypted.data();
    inBlob.cbData = size;
    DATA_BLOB outBlob = {};

    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob)) {
        return false;
    }

    const wchar_t* data = reinterpret_cast<const wchar_t*>(outBlob.pbData);
    size_t chars = outBlob.cbData / sizeof(wchar_t);
    token.assign(data, data + chars);
    LocalFree(outBlob.pbData);
    return !token.empty();
}

bool LoadConsumedCommandIds(std::vector<std::wstring>& commandIds)
{
    commandIds.clear();

    std::wstring path = GetConsumedCommandsPath();
    if (path.empty()) return false;

    FILE* f = OpenFile(path, L"rt, ccs=UTF-8");
    if (!f) return false;

    wchar_t line[512] = {};
    while (fgetws(line, _countof(line), f)) {
        std::wstring value(line);
        while (!value.empty() && (value.back() == L'\n' || value.back() == L'\r')) {
            value.pop_back();
        }
        if (!value.empty()) {
            commandIds.push_back(std::move(value));
        }
    }

    fclose(f);
    return true;
}

bool SaveConsumedCommandId(std::wstring_view commandId)
{
    if (commandId.empty()) return false;
    if (!EnsureConfigDir()) return false;

    std::wstring path = GetConsumedCommandsPath();
    if (path.empty()) return false;

    FILE* f = OpenFile(path, L"at, ccs=UTF-8");
    if (!f) return false;

    bool ok = (fputws(std::wstring(commandId).c_str(), f) >= 0 && fputws(L"\n", f) >= 0);
    fclose(f);
    return ok;
}

} // namespace ConfigManager
