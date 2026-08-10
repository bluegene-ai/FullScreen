// ============================================================
// AutoUpdate - client self-update (GitHub releases / self-hosted)
// ============================================================
#include "auto_update.h"
#include "config_manager.h"
#include "crypto.h"
#include "resource.h" // APP_VERSION_STR
#include <Windows.h>
#include <ShlObj.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Crypt32.lib")

namespace {

// ============================================================
// Small file / path helpers
// ============================================================
std::wstring AppDataDir()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"";
    }
    return std::wstring(appData) + L"\\" + AppConstants::CONFIG_DIR;
}

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

std::wstring ExeDir()
{
    std::wstring p = ExePath();
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

static bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool WriteAllBytes(const std::wstring& path, const void* data, size_t len)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, (DWORD)len, &written, nullptr) && written == len;
    CloseHandle(h);
    return ok;
}

static bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    return WriteAllBytes(path, text.data(), text.size() * sizeof(wchar_t));
}

static bool ReadTextFile(const std::wstring& path, std::wstring& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = false;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {
        std::wstring buf((size_t)(size.QuadPart / sizeof(wchar_t)), L'\0');
        DWORD read = 0;
        if (ReadFile(h, &buf[0], (DWORD)size.QuadPart, &read, nullptr)) {
            out.assign(buf.data(), read / sizeof(wchar_t));
            ok = true;
        }
    }
    CloseHandle(h);
    return ok;
}

// ============================================================
// Persisted throttle timestamps (two lines: github_ts / self_ts)
// ============================================================
static std::wstring LastCheckPath()
{
    return AppDataDir() + L"\\update_lastcheck.dat";
}

static void ReadTimestamps(long long& github, long long& self)
{
    github = 0; self = 0;
    std::wstring text;
    if (!ReadTextFile(LastCheckPath(), text)) return;
    size_t nl = text.find(L'\n');
    std::wstring a = text.substr(0, nl);
    std::wstring b = (nl == std::wstring::npos) ? L"" : text.substr(nl + 1);
    auto trim = [](std::wstring s) {
        s.erase(0, s.find_first_not_of(L" \t\r\n"));
        s.erase(s.find_last_not_of(L" \t\r\n") + 1);
        return s;
    };
    a = trim(a); b = trim(b);
    github = a.empty() ? 0 : _wtoi64(a.c_str());
    self = b.empty() ? 0 : _wtoi64(b.c_str());
}

static void WriteTimestamps(long long github, long long self)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%lld\n%lld\n", github, self);
    WriteTextFile(LastCheckPath(), buf);
}

// ============================================================
// UTF-8 <-> wide
// ============================================================
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], len);
    return out;
}

// ============================================================
// WinHTTP GET into memory (follows redirects, e.g. GitHub codeload)
// ============================================================
static bool WinHttpGet(const std::wstring& url, std::string& outBody)
{
    outBody.clear();

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort :
                         (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    HINTERNET hSession = WinHttpOpen(L"FullScreenBrowser-Updater/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    DWORD timeout = 30000;
    WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                https ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest) {
            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                             &redirectPolicy, sizeof(redirectPolicy));

            static const wchar_t* uaHeader = L"User-Agent: FullScreenBrowser-Updater/1.0\r\n";
            if (WinHttpSendRequest(hRequest, uaHeader, (DWORD)wcslen(uaHeader),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                        std::vector<char> buf(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
                        outBody.append(buf.data(), read);
                    }
                    ok = true;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

// ============================================================
// Version helpers
// ============================================================
static bool GetFileVersion(const std::wstring& path, std::wstring& ver)
{
    ver.clear();
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return false;

    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return false;

    struct LangCp { WORD lang; WORD cp; };
    LangCp* trans = nullptr;
    UINT tlen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&trans), &tlen) || tlen < sizeof(LangCp)) {
        return false;
    }

    wchar_t sub[64] = {};
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileVersion", trans[0].lang, trans[0].cp);
    wchar_t* val = nullptr;
    UINT vlen = 0;
    if (!VerQueryValueW(buf.data(), sub, reinterpret_cast<void**>(&val), &vlen) || !val) {
        return false;
    }
    ver = val;
    return !ver.empty();
}

// ============================================================
// Replacement staging (applied marker + detached PowerShell swap)
// ============================================================
// A detached powershell.exe (launched via -EncodedCommand, so no script file
// and no code-page issues with non-ASCII paths) waits for this process to
// exit, backs up the exe, swaps in .new and relaunches. Returns false if
// staging could not be prepared.

// PowerShell single-quoted literal with '' escaping.
static std::wstring PsQuote(const std::wstring& s)
{
    std::wstring out = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    out += L"'";
    return out;
}

static std::wstring AsciiToWide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

// UTF-16LE command -> base64 for PowerShell -EncodedCommand.
static std::wstring EncodePsCommand(const std::wstring& command)
{
    const BYTE* bytes = reinterpret_cast<const BYTE*>(command.c_str());
    DWORD len = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    DWORD outLen = 0;
    CryptBinaryToStringA(bytes, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         nullptr, &outLen);
    std::string b64(outLen, '\0');
    CryptBinaryToStringA(bytes, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         &b64[0], &outLen);
    return AsciiToWide(b64);
}

static bool StageApply(const std::wstring& exe, const std::wstring& newPath)
{
    std::wstring appDir = AppDataDir();
    if (appDir.empty() || !ConfigManager::EnsureConfigDir()) return false;

    // The staged .new must still be present.
    if (!FileExists(newPath)) return false;

    // Mark "applied, awaiting confirmation" so the next boot can roll back.
    if (!WriteTextFile(appDir + L"\\update.applied", L"1")) return false;

    // Detached PowerShell performs the swap after this process exits.
    std::wstring ps =
        L"$e=" + PsQuote(exe) + L"; $n=" + PsQuote(newPath) + L"; "
        L"Start-Sleep -Seconds 3; "
        L"Copy-Item -LiteralPath $e -Destination ($e+'.bak') -Force; "
        L"if(!(Test-Path -LiteralPath ($e+'.bak'))){exit 1}; "
        L"Move-Item -LiteralPath $n -Destination $e -Force; "
        L"if(!(Test-Path -LiteralPath $e)){exit 1}; "
        L"Start-Process -FilePath $e; ";

    std::wstring args =
        L"-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " +
        EncodePsCommand(ps);

    // Clear the "new version waiting for window" marker: applying now.
    DeleteFileW((appDir + L"\\update.pending").c_str());

    HINSTANCE hr = ShellExecuteW(nullptr, L"open", L"powershell.exe",
                                 args.c_str(), nullptr, SW_HIDE);
    return (INT_PTR)hr > 32;
}

static void Relaunch(const std::wstring& exe)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

} // namespace

// ============================================================
// Public API
// ============================================================
namespace AutoUpdate {

std::wstring CurrentVersion()
{
    // APP_VERSION_STR is ASCII (e.g. "2026.08.10"); widen it for compares.
    std::string s = APP_VERSION_STR;
    return std::wstring(s.begin(), s.end());
}

std::wstring GitHubLatestUrl(const std::wstring& repo)
{
    return std::wstring(L"https://github.com/") + repo +
           L"/releases/latest/download/" + AppConstants::UPDATE_EXE_NAME;
}

StartupResult HandleStartupRecovery()
{
    std::wstring appDir = AppDataDir();
    std::wstring appliedPath = appDir + L"\\update.applied";
    DeleteFileW((ExeDir() + L"\\update.cmd").c_str()); // stale cmd is never needed now

    if (!FileExists(appliedPath)) {
        return StartupResult::None;
    }

    std::wstring exe = ExePath();
    std::wstring newPath = exe + L".new";

    // Replacement was never completed (crash before the swap, or the swap
    // failed to move .new): retry it now. The detached powershell waits for
    // this process to exit; returning RolledBackRestarted makes the caller
    // exit immediately. If staging fails, the applied marker stays so the
    // next boot retries again.
    if (FileExists(newPath)) {
        StageApply(exe, newPath);
        return StartupResult::RolledBackRestarted;
    }

    // Replacement completed; this is the new exe booting. Count attempts.
    int attempts = 0;
    std::wstring attemptsPath = appDir + L"\\update.attempts";
    std::wstring attemptsText;
    if (ReadTextFile(attemptsPath, attemptsText)) {
        attempts = _wtoi(attemptsText.c_str());
    }
    attempts += 1;

    if (attempts > AppConstants::UPDATE_MAX_STARTUP_ATTEMPTS) {
        // New exe keeps failing: roll back to the previous version.
        std::wstring bak = exe + L".bak";
        if (FileExists(bak)) {
            CopyFileW(bak.c_str(), exe.c_str(), FALSE);
        }
        DeleteFileW(newPath.c_str());
        DeleteFileW(appliedPath.c_str());
        DeleteFileW(attemptsPath.c_str());
        DeleteFileW((appDir + L"\\update.pending").c_str());
        Relaunch(exe);
        return StartupResult::RolledBackRestarted;
    }

    WriteTextFile(attemptsPath, std::to_wstring(attempts));
    return StartupResult::Confirming;
}

void ConfirmAndCleanup()
{
    std::wstring appDir = AppDataDir();
    DeleteFileW((appDir + L"\\update.applied").c_str());
    DeleteFileW((appDir + L"\\update.attempts").c_str());
    DeleteFileW((appDir + L"\\update.pending").c_str());
    DeleteFileW((ExePath() + L".bak").c_str());
    DeleteFileW((ExePath() + L".new").c_str());
    DeleteFileW((ExeDir() + L"\\update.cmd").c_str());
}

CheckResult CheckAndApply(const AppConfig& cfg)
{
    if (!cfg.autoUpdate) return CheckResult::NoUpdate;

    time_t now = time(nullptr);
    std::wstring appDir = AppDataDir();
    std::wstring exe = ExePath();
    std::wstring newPath = exe + L".new";
    bool github = (cfg.updateSource == AppConstants::UPDATE_SOURCE_GITHUB);

    // 1) A verified new version is already staged, waiting for the window.
    if (FileExists(appDir + L"\\update.pending")) {
        if (!FileExists(newPath)) {
            // Staged file was lost (e.g. manual cleanup); drop the marker and
            // re-download instead of wedging on a permanent Error.
            DeleteFileW((appDir + L"\\update.pending").c_str());
        } else if (InUpdateWindow(cfg.updateWindow, now)) {
            return StageApply(exe, newPath) ? CheckResult::AppliedRestartNeeded
                                            : CheckResult::Error;
        } else {
            return CheckResult::NoUpdate; // wait for the maintenance window
        }
    }

    // 2) Throttled version check.
    long long interval = github ? AppConstants::UPDATE_GITHUB_INTERVAL_SEC
                                : AppConstants::UPDATE_SELF_INTERVAL_SEC;
    long long ghLast = 0, selfLast = 0;
    ReadTimestamps(ghLast, selfLast);
    long long last = github ? ghLast : selfLast;
    if ((now - last) < interval) return CheckResult::NoUpdate;

    std::wstring url, expectVer, expectSha;
    std::wstring fv; // GitHub: FileVersion of the downloaded exe
    if (github) {
        if (cfg.updateRepo[0] == L'\0') return CheckResult::NoUpdate;
        url = GitHubLatestUrl(cfg.updateRepo);
    } else {
        if (cfg.updateBaseUrl[0] == L'\0') return CheckResult::NoUpdate;
        // latest.txt: <version>\n<sha256hex>\n[<filename>]
        std::string body;
        std::wstring manifestUrl = cfg.updateBaseUrl;
        if (manifestUrl.back() != L'/') manifestUrl += L'/';
        manifestUrl += AppConstants::UPDATE_MANIFEST;
        if (!WinHttpGet(manifestUrl, body)) return CheckResult::Error;

        std::wstring text = Utf8ToWide(body);
        std::vector<std::wstring> lines;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find_first_of(L"\r\n", pos);
            std::wstring line = (nl == std::wstring::npos) ? text.substr(pos)
                                                           : text.substr(pos, nl - pos);
            line.erase(0, line.find_first_not_of(L" \t"));
            line.erase(line.find_last_not_of(L" \t") + 1);
            if (!line.empty()) lines.push_back(line);
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
            while (pos < text.size() && (text[pos] == L'\r' || text[pos] == L'\n')) ++pos;
        }
        if (lines.size() < 2) return CheckResult::Error;
        expectVer = lines[0];
        expectSha = lines[1];
        std::wstring file = (lines.size() >= 3 && !lines[2].empty())
                            ? lines[2] : AppConstants::UPDATE_EXE_NAME;
        if (expectVer.empty() || expectSha.empty() ||
            !IsNewerVersion(expectVer, CurrentVersion())) {
            WriteTimestamps(github ? now : ghLast, github ? selfLast : now);
            return CheckResult::NoUpdate;
        }
        url = manifestUrl.substr(0, manifestUrl.find_last_of(L'/') + 1) + file;
    }

    // 3) Download (this is the GitHub "query" - counted once per day).
    std::string body;
    if (!WinHttpGet(url, body) || body.size() < 1024 * 1024) {
        WriteTimestamps(github ? now : ghLast, github ? selfLast : now);
        return CheckResult::Error;
    }

    // 4) Verify (manifest sha is ASCII hex; narrow it for the compare).
    if (!github) {
        std::string expectShaA;
        expectShaA.reserve(expectSha.size());
        for (wchar_t c : expectSha) expectShaA.push_back(static_cast<char>(c));
        std::string hex = Crypto::Sha256Hex(body.data(), body.size());
        if (hex.empty() || hex != expectShaA) {
            WriteTimestamps(github ? now : ghLast, github ? selfLast : now);
            return CheckResult::Error;
        }
    }

    // 5) Persist staged file.
    if (!WriteAllBytes(newPath, body.data(), body.size())) {
        WriteTimestamps(github ? now : ghLast, github ? selfLast : now);
        return CheckResult::Error;
    }

    // GitHub has no external hash: validate the downloaded exe's own version.
    if (github) {
        if (!GetFileVersion(newPath, fv) || !IsNewerVersion(fv, CurrentVersion())) {
            DeleteFileW(newPath.c_str());
            WriteTimestamps(github ? now : ghLast, github ? selfLast : now);
            return CheckResult::NoUpdate; // same/older version or unreadable exe
        }
    }

    WriteTimestamps(github ? now : ghLast, github ? selfLast : now);

    // 6) Stage for application (immediately if inside the window, else wait).
    if (!WriteTextFile(appDir + L"\\update.pending", github ? fv : expectVer)) {
        DeleteFileW(newPath.c_str());
        return CheckResult::Error;
    }
    if (InUpdateWindow(cfg.updateWindow, now)) {
        return StageApply(exe, newPath) ? CheckResult::AppliedRestartNeeded
                                        : CheckResult::Error;
    }
    return CheckResult::NoUpdate; // applied by a later poll inside the window
}

} // namespace AutoUpdate
