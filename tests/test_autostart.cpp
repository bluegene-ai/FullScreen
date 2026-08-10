// ============================================================
// test_autostart.cpp - runnable self-check for AutoStart logic.
// Build (in a VS2022 x64 dev prompt):
//   cl /nologo /W4 /EHsc /std:c++17 test_autostart.cpp ^
//       ..\src\auto_start.cpp /Fe:test_autostart.exe advapi32.lib
// Runs the enable->verify->disable->verify->idempotent sequence,
// then restores the pre-test registry state. Prints PASS/FAIL.
// ============================================================
// auto_start.cpp's anonymous-namespace RUN_KEY/RUN_VALUE are reused below
// (same translation unit after #include).
#include "../src/auto_start.cpp"
#include <Windows.h>
#include <cstdio>

namespace {
// Returns true if the Run value currently exists.
bool ValueExists(std::wstring& value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    LONG rc = RegQueryValueExW(key, RUN_VALUE, nullptr, nullptr,
                               reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(key);
    if (rc == ERROR_SUCCESS) {
        value = buf;
        return true;
    }
    return false;
}

void Expect(bool cond, const char* what)
{
    std::printf("%-55s [%s]\n", what, cond ? "PASS" : "FAIL");
    if (!cond) std::exit(1);
}
} // namespace

int main()
{
    std::wstring oldValue;
    bool existedBefore = ValueExists(oldValue);

    std::printf("Test: HKCU Run key auto-start registration\n\n");

    // 1. Enable -> value must be written, pointing at this exe (quoted).
    Expect(AutoStart::SetAutoStart(true), "SetAutoStart(true) returns true");
    std::wstring v;
    Expect(ValueExists(v), "Run value exists after enable");
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring expected = L"\"" + std::wstring(exe) + L"\"";
    Expect(v == expected, "Run value points at quoted exe path");

    // 2. Disable -> value must be removed.
    Expect(AutoStart::SetAutoStart(false), "SetAutoStart(false) returns true");
    std::wstring gone;
    Expect(!ValueExists(gone), "Run value removed after disable");

    // 3. Disable again -> idempotent (removing an absent value is success).
    Expect(AutoStart::SetAutoStart(false), "SetAutoStart(false) again returns true");

    // 4. Restore pre-test state.
    if (existedBefore) {
        AutoStart::SetAutoStart(true);
        std::wstring restored;
        Expect(ValueExists(restored) && restored == oldValue, "Pre-test state restored");
    }

    std::printf("\nAll checks passed.\n");
    return 0;
}
