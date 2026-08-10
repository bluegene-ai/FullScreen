// ============================================================
// AutoStart - HKCU Run key self-registration
// ============================================================
#include "auto_start.h"
#include <Windows.h>
#include <string>

namespace {

constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RUN_VALUE[] = L"FullScreenBrowser";

} // namespace

namespace AutoStart {

bool SetAutoStart(bool enable)
{
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                              &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    bool ok = false;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        // Quoted path so a spaced install dir (e.g. Program Files) still runs.
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
            rc = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.c_str()),
                                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            ok = (rc == ERROR_SUCCESS);
        }
    } else {
        // Removing an already-absent value is success (idempotent uncheck).
        rc = RegDeleteValueW(key, RUN_VALUE);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(key);
    return ok;
}

} // namespace AutoStart
