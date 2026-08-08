#pragma once

#include "app_common.h"
#include <string>

namespace RemoteConfigClient {

struct PasswordUpdateCommand {
    bool present = false;
    std::wstring commandId;
    int effectiveAt = 0;
    int expireAt = 0;
    std::wstring encryptedPassword;
};

struct RegisterResult {
    bool ok = false;
    std::wstring deviceToken;
    int pollBaseSec = AppConstants::REMOTE_POLL_BASE_SEC_DEFAULT;
    int pollJitterSec = AppConstants::REMOTE_POLL_JITTER_SEC_DEFAULT;
    int pollMaxBackoffSec = AppConstants::REMOTE_POLL_MAX_BACKOFF_SEC_DEFAULT;
    std::wstring error;
};

struct MergedConfigResult {
    bool ok = false;
    bool notModified = false;
    int revision = 0;
    AppConfig config = {};
    PasswordUpdateCommand passwordCommand;
    std::wstring signature;
    std::wstring error;
};

struct AckResult {
    bool ok = false;
    std::wstring error;
};

// Returns true if the server base URL is syntactically valid AND answers
// GET /health with HTTP 200. Used to gate registration on a usable URL.
bool CheckServerReachable(const std::wstring& baseUrl);

// POST /api/v1/device/register
RegisterResult RegisterDevice(const std::wstring& baseUrl,
                             const std::wstring& deviceId,
                             const std::wstring& registerCode,
                             const std::wstring& clientVersion);

// GET /api/v1/config/merged
MergedConfigResult FetchMergedConfig(const std::wstring& baseUrl,
                                     const std::wstring& deviceId,
                                     const std::wstring& deviceToken,
                                     int localRevision,
                                     const AppConfig& currentConfig);

// POST /api/v1/config/ack
AckResult AckAppliedRevision(const std::wstring& baseUrl,
                             const std::wstring& deviceId,
                             const std::wstring& deviceToken,
                             int revision,
                             const std::wstring& status,
                             const std::wstring& message,
                             const PasswordUpdateCommand* passwordCommand,
                             const std::wstring& commandStatus);

bool VerifyMergedConfigSignature(const std::wstring& rawResponse,
                                 const std::wstring& deviceToken,
                                 const std::wstring& expectedSignature);

} // namespace RemoteConfigClient
