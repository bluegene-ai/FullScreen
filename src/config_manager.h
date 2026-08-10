#pragma once
#include "app_common.h"
#include <string>
#include <vector>

// ============================================================
// ConfigManager - read/write config from %APPDATA%
// ============================================================

namespace ConfigManager {

// Get config file path: %APPDATA%\FullScreenBrowser\config.dat
std::wstring GetConfigFilePath();

// Get WebView2 user data path: %APPDATA%\FullScreenBrowser\WebView2
std::wstring GetWebView2DataPath();

// Get remote token path: %APPDATA%\FullScreenBrowser\remote_token.dat
std::wstring GetRemoteTokenPath();

// Ensure config directory exists
bool EnsureConfigDir();

// Load config from disk. Returns true if valid config was loaded.
bool LoadConfig(AppConfig& config);

// Save config to disk. Encrypts password before writing.
bool SaveConfig(const AppConfig& config, std::wstring_view plainPassword);

// Persist the remote device token protected with DPAPI.
bool SaveRemoteToken(std::wstring_view token);

// Load and decrypt the persisted remote device token.
bool LoadRemoteToken(std::wstring& token);

// Load consumed remote command ids from local storage.
bool LoadConsumedCommandIds(std::vector<std::wstring>& commandIds);

// Mark a remote command id as consumed locally.
bool SaveConsumedCommandId(std::wstring_view commandId);

} // namespace ConfigManager
