#include "remote_config_client.h"
#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace RemoteConfigClient {
namespace {

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool https = true;
};

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), size);
    return out;
}

static std::string Sha256Hex(const std::string& input)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD objLen = 0, cb = 0, hashLen = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    std::vector<UCHAR> obj(objLen), hash(hashLen);
    if (BCryptCreateHash(hAlg, &hHash, obj.data(), objLen, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), (ULONG)input.size(), 0);
    BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(hash.size() * 2);
    for (UCHAR b : hash) {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

static std::wstring HmacSha256Hex(const std::wstring& payload, const std::wstring& secret)
{
    std::string payloadUtf8 = WideToUtf8(payload);
    std::string secretUtf8 = WideToUtf8(secret);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD objLen = 0, cb = 0, hashLen = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return {};
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    std::vector<UCHAR> obj(objLen), hash(hashLen);
    if (BCryptCreateHash(hAlg, &hHash, obj.data(), objLen,
                         reinterpret_cast<PUCHAR>(secretUtf8.data()), (ULONG)secretUtf8.size(), 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(payloadUtf8.data()), (ULONG)payloadUtf8.size(), 0);
    BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(hash.size() * 2);
    for (UCHAR b : hash) {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

static bool ParseUrl(const std::wstring& url, UrlParts& parts)
{
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        return false;
    }

    parts.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    parts.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    parts.port = uc.nPort;
    parts.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    if (parts.path.empty()) parts.path = L"/";
    return !parts.host.empty();
}

static bool HttpJsonRequest(const std::wstring& method,
                            const std::wstring& url,
                            const std::wstring& headers,
                            const std::string& bodyUtf8,
                            DWORD& statusCode,
                            std::wstring& responseText)
{
    responseText.clear();
    statusCode = 0;

    UrlParts p;
    if (!ParseUrl(url, p)) return false;

    HINTERNET hSession = WinHttpOpen(L"FullScreenBrowser/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, p.host.c_str(), p.port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD timeoutMs = 2500;
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    DWORD flags = p.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            method.c_str(),
                                            p.path.c_str(),
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL sent = WinHttpSendRequest(hRequest,
                                   headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : (DWORD)-1L,
                                   bodyUtf8.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)bodyUtf8.data(),
                                   (DWORD)bodyUtf8.size(),
                                   (DWORD)bodyUtf8.size(),
                                   0);
    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &size,
                        WINHTTP_NO_HEADER_INDEX);

    std::string all;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;

        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
        all.append(buf.data(), read);
    }

    responseText = Utf8ToWide(all);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

static bool JsonGetString(const std::wstring& json, const std::wstring& key, std::wstring& value)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;

    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;
    pos = json.find(L'\"', pos);
    if (pos == std::wstring::npos) return false;

    size_t end = json.find(L'\"', pos + 1);
    if (end == std::wstring::npos) return false;

    value = json.substr(pos + 1, end - pos - 1);
    return true;
}

static bool JsonExtractArray(const std::wstring& json, const std::wstring& key, std::wstring& arrayText)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;

    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;
    pos = json.find(L'[', pos);
    if (pos == std::wstring::npos) return false;

    int depth = 0;
    for (size_t end = pos; end < json.size(); ++end) {
        if (json[end] == L'[') ++depth;
        else if (json[end] == L']') {
            --depth;
            if (depth == 0) {
                arrayText = json.substr(pos, end - pos + 1);
                return true;
            }
        }
    }
    return false;
}

static bool JsonExtractObject(const std::wstring& json, const std::wstring& key, std::wstring& objectText)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;

    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;
    pos = json.find(L'{', pos);
    if (pos == std::wstring::npos) return false;

    int depth = 0;
    size_t end = pos;
    for (; end < json.size(); ++end) {
        if (json[end] == L'{') {
            ++depth;
        } else if (json[end] == L'}') {
            --depth;
            if (depth == 0) {
                objectText = json.substr(pos, end - pos + 1);
                return true;
            }
        }
    }

    return false;
}

static bool JsonGetInt(const std::wstring& json, const std::wstring& key, int& out)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;

    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;

    while (pos < json.size() && (json[pos] == L':' || json[pos] == L' ' || json[pos] == L'\t' || json[pos] == L'\r' || json[pos] == L'\n')) {
        ++pos;
    }
    if (pos >= json.size()) return false;

    wchar_t* endPtr = nullptr;
    long v = wcstol(json.c_str() + pos, &endPtr, 10);
    if (endPtr == json.c_str() + pos) return false;
    out = (int)v;
    return true;
}

static bool JsonGetBool(const std::wstring& json, const std::wstring& key, bool& out)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;

    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return false;

    while (pos < json.size() && (json[pos] == L':' || json[pos] == L' ' || json[pos] == L'\t' || json[pos] == L'\r' || json[pos] == L'\n')) {
        ++pos;
    }
    if (json.compare(pos, 4, L"true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(pos, 5, L"false") == 0) {
        out = false;
        return true;
    }
    return false;
}

static std::wstring JoinUrl(const std::wstring& base, const std::wstring& suffix)
{
    if (base.empty()) return suffix;
    if (base.back() == L'/' && !suffix.empty() && suffix.front() == L'/') {
        return base.substr(0, base.size() - 1) + suffix;
    }
    if (base.back() != L'/' && !suffix.empty() && suffix.front() != L'/') {
        return base + L"/" + suffix;
    }
    return base + suffix;
}

static std::wstring StripSignatureField(const std::wstring& json)
{
    const std::wstring key = L"\"signature\"";
    size_t pos = json.find(key);
    if (pos == std::wstring::npos) return json;

    size_t fieldStart = pos;
    if (fieldStart > 0 && json[fieldStart - 1] == L',') {
        --fieldStart;
    }

    size_t colon = json.find(L':', pos + key.size());
    if (colon == std::wstring::npos) return json;
    size_t valueStart = json.find(L'\"', colon);
    if (valueStart == std::wstring::npos) return json;
    size_t valueEnd = json.find(L'\"', valueStart + 1);
    if (valueEnd == std::wstring::npos) return json;

    size_t fieldEnd = valueEnd + 1;
    if (fieldStart == pos) {
        size_t maybeComma = json.find_first_not_of(L" \t\r\n", fieldEnd);
        if (maybeComma != std::wstring::npos && json[maybeComma] == L',') {
            fieldEnd = maybeComma + 1;
        }
    }

    std::wstring stripped = json;
    stripped.erase(fieldStart, fieldEnd - fieldStart);
    return stripped;
}

} // namespace

RegisterResult RegisterDevice(const std::wstring& baseUrl,
                              const std::wstring& deviceId,
                              const std::wstring& registerCode,
                              const std::wstring& clientVersion)
{
    RegisterResult res;

    std::wstring url = JoinUrl(baseUrl, L"/api/v1/device/register");
    std::wstring body = L"{\"deviceId\":\"" + deviceId + L"\",\"registerCode\":\"" + registerCode + L"\",\"clientVersion\":\"" + clientVersion + L"\"}";
    DWORD code = 0;
    std::wstring response;

    if (!HttpJsonRequest(L"POST", url, L"Content-Type: application/json\r\n", WideToUtf8(body), code, response)) {
        res.error = L"network_error";
        return res;
    }
    if (code != 200) {
        res.error = L"http_" + std::to_wstring(code);
        return res;
    }

    if (!JsonGetString(response, L"deviceToken", res.deviceToken) || res.deviceToken.empty()) {
        res.error = L"invalid_response";
        return res;
    }
    JsonGetInt(response, L"pollBaseSec", res.pollBaseSec);
    JsonGetInt(response, L"pollJitterSec", res.pollJitterSec);
    JsonGetInt(response, L"pollMaxBackoffSec", res.pollMaxBackoffSec);

    res.ok = true;
    return res;
}

MergedConfigResult FetchMergedConfig(const std::wstring& baseUrl,
                                     const std::wstring& deviceId,
                                     const std::wstring& deviceToken,
                                     int localRevision,
                                     const AppConfig& currentConfig)
{
    MergedConfigResult res;
    // Patch semantics: start from the current local config and only override the
    // fields the server actually sends. This prevents an empty/partial server
    // config from wiping locally-configured fields (e.g. url) on the first sync.
    res.config = currentConfig;

    std::wstring url = JoinUrl(baseUrl, L"/api/v1/config/merged?deviceId=" + deviceId + L"&localRevision=" + std::to_wstring(localRevision));
    std::wstring headers = L"Authorization: Bearer " + deviceToken + L"\r\n";
    DWORD code = 0;
    std::wstring response;

    if (!HttpJsonRequest(L"GET", url, headers, {}, code, response)) {
        res.error = L"network_error";
        return res;
    }
    if (code != 200) {
        res.error = L"http_" + std::to_wstring(code);
        return res;
    }

    bool notModified = false;
    if (JsonGetBool(response, L"notModified", notModified) && notModified) {
        res.ok = true;
        res.notModified = true;
        return res;
    }

    if (!JsonGetInt(response, L"revision", res.revision)) {
        res.error = L"invalid_response";
        return res;
    }

    JsonGetString(response, L"signature", res.signature);
    if (res.signature.empty()) {
        res.error = L"missing_signature";
        return res;
    }

    if (!VerifyMergedConfigSignature(response, deviceToken, res.signature)) {
        res.error = L"invalid_signature";
        return res;
    }

    std::wstring configObj;
    if (!JsonExtractObject(response, L"config", configObj)) {
        res.error = L"invalid_response";
        return res;
    }

    // Minimal flat-field parser for first integration step.
    std::wstring urlValue;
    if (JsonGetString(configObj, L"url", urlValue)) {
        wcsncpy_s(res.config.url, urlValue.c_str(), _TRUNCATE);
    }

    std::wstring msg;
    if (JsonGetString(configObj, L"unreachableMsg", msg)) {
        wcsncpy_s(res.config.unreachableMsg, msg.c_str(), _TRUNCATE);
    }

    JsonGetInt(configObj, L"zoomPercent", res.config.zoomPercent);
    JsonGetInt(configObj, L"refreshMode", res.config.refreshMode);
    JsonGetInt(configObj, L"refreshIntervalSec", res.config.refreshIntervalSec);
    JsonGetInt(configObj, L"refreshDailyMin", res.config.refreshDailyMin);
    JsonGetBool(configObj, L"burnInPrevention", res.config.burnInPrevention);
    JsonGetBool(configObj, L"allowRemotePasswordUpdate", res.config.allowRemotePasswordUpdate);

    std::wstring commandsArray;
    if (JsonExtractArray(response, L"commands", commandsArray) && commandsArray.find(L"password_update") != std::wstring::npos) {
        // ponytail: only the first command is parsed today (single PasswordUpdateCommand
        // in the result). The server can return several; this needs a proper array loop
        // plus per-command ack. Ceiling is documented, not urgent: the server currently
        // enqueues one password_update at a time per device.
        PasswordUpdateCommand cmd;
        if (JsonGetString(commandsArray, L"commandId", cmd.commandId) && !cmd.commandId.empty()) {
            cmd.present = true;
            JsonGetInt(commandsArray, L"effectiveAt", cmd.effectiveAt);
            JsonGetInt(commandsArray, L"expireAt", cmd.expireAt);
            JsonGetString(commandsArray, L"encryptedPassword", cmd.encryptedPassword);
            res.passwordCommand = std::move(cmd);
        }
    }

    res.ok = true;
    return res;
}

AckResult AckAppliedRevision(const std::wstring& baseUrl,
                             const std::wstring& deviceId,
                             const std::wstring& deviceToken,
                             int revision,
                             const std::wstring& status,
                             const std::wstring& message,
                             const PasswordUpdateCommand* passwordCommand,
                             const std::wstring& commandStatus)
{
    AckResult res;

    std::wstring url = JoinUrl(baseUrl, L"/api/v1/config/ack");
    std::wstring headers = L"Authorization: Bearer " + deviceToken + L"\r\nContent-Type: application/json\r\n";
    std::wstring body = L"{\"deviceId\":\"" + deviceId +
                        L"\",\"revision\":" + std::to_wstring(revision) +
                        L",\"status\":\"" + status +
                        L"\",\"message\":\"" + message + L"\"";
    if (passwordCommand != nullptr && passwordCommand->present && !commandStatus.empty()) {
        body += L",\"commandResults\":[{\"commandId\":\"" + passwordCommand->commandId +
                L"\",\"status\":\"" + commandStatus + L"\"}]";
    }
    body += L"}";

    DWORD code = 0;
    std::wstring response;
    if (!HttpJsonRequest(L"POST", url, headers, WideToUtf8(body), code, response)) {
        res.error = L"network_error";
        return res;
    }
    if (code != 200) {
        res.error = L"http_" + std::to_wstring(code);
        return res;
    }

    res.ok = true;
    return res;
}

bool VerifyMergedConfigSignature(const std::wstring& rawResponse,
                                 const std::wstring& deviceToken,
                                 const std::wstring& expectedSignature)
{
    if (rawResponse.empty() || deviceToken.empty() || expectedSignature.empty()) {
        return false;
    }

    std::wstring stripped = StripSignatureField(rawResponse);
    std::wstring secret = Utf8ToWide(Sha256Hex(WideToUtf8(deviceToken)));
    if (secret.empty()) {
        return false;
    }

    std::wstring actual = HmacSha256Hex(stripped, secret);
    return !actual.empty() && _wcsicmp(actual.c_str(), expectedSignature.c_str()) == 0;
}

} // namespace RemoteConfigClient
