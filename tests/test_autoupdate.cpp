// ============================================================
// test_autoupdate.cpp - runnable self-check for AutoUpdate pure
// logic: version comparison (IsNewerVersion) and maintenance
// window parsing (InUpdateWindow).
//
// Build (VS2022 x64 dev prompt):
//   cl /nologo /W4 /EHsc /std:c++17 test_autoupdate.cpp ^
//       /Fe:test_autoupdate.exe
// ============================================================
#include "../src/auto_update_logic.cpp"
#include <cstdio>
#include <ctime>
#include <string>

static int failures = 0;

static void Expect(bool cond, const char* what)
{
    std::printf("%-72s [%s]\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static std::string ToAscii(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

static void CheckNewer(const wchar_t* cand, const wchar_t* cur, bool expect)
{
    bool got = AutoUpdate::IsNewerVersion(cand, cur);
    std::string label = "IsNewerVersion(\"" + ToAscii(cand) + "\", \"" + ToAscii(cur) + "\")";
    Expect(got == expect, label.c_str());
    if (got != expect) std::printf("      got=%d expect=%d\n", got ? 1 : 0, expect ? 1 : 0);
}

// Build a local-time time_t for the given wall clock.
static time_t MakeTime(int year, int mon, int day, int hour, int min)
{
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_isdst = -1;
    return mktime(&t);
}

static void CheckWindow(const wchar_t* window, time_t now, bool expect)
{
    bool got = AutoUpdate::InUpdateWindow(window, now);
    std::string label = "InUpdateWindow(\"" + ToAscii(window) + "\", " + std::to_string(now) + ")";
    Expect(got == expect, label.c_str());
    if (got != expect) std::printf("      got=%d expect=%d\n", got ? 1 : 0, expect ? 1 : 0);
}

int main()
{
    std::printf("AutoUpdate logic self-check\n\n");

    // ---- IsNewerVersion ----
    CheckNewer(L"2026.08.10", L"2026.08.10", false); // equal
    CheckNewer(L"2026.08.11", L"2026.08.10", true);
    CheckNewer(L"2026.09.01", L"2026.08.31", true);
    CheckNewer(L"2026.08.10", L"2026.08.9", true);   // 10 > 9
    CheckNewer(L"2026.8.9", L"2026.8.10", false);
    CheckNewer(L"1.2.3.1", L"1.2.3", true);          // build suffix
    CheckNewer(L"1.2.3", L"1.2.3.1", false);         // missing digits = 0
    CheckNewer(L"v2026.08.10", L"2026.08.10", false);// prefix ignored, equal
    CheckNewer(L"2026.08.10.1", L"2026.08.10", true);// CI turns a-suffix into build=1
    CheckNewer(L"0.0.0", L"", false);                // both effectively zero
    CheckNewer(L"", L"1.0.0", false);                // empty candidate
    CheckNewer(L"garbage", L"1.0.0", false);         // no digits

    // ---- InUpdateWindow ----
    time_t t0200 = MakeTime(2026, 8, 10, 2, 0);
    time_t t0230 = MakeTime(2026, 8, 10, 2, 30);
    time_t t0300 = MakeTime(2026, 8, 10, 3, 0);
    time_t t0159 = MakeTime(2026, 8, 10, 1, 59);
    time_t t1200 = MakeTime(2026, 8, 10, 12, 0);
    time_t t2300 = MakeTime(2026, 8, 10, 23, 0);
    time_t t0100 = MakeTime(2026, 8, 11, 1, 0);

    CheckWindow(L"", t1200, true);                    // empty = anytime
    CheckWindow(L"02:00-03:00", t0230, true);         // inside
    CheckWindow(L"02:00-03:00", t0200, true);         // start inclusive
    CheckWindow(L"02:00-03:00", t0300, false);        // end exclusive
    CheckWindow(L"02:00-03:00", t0159, false);        // before
    CheckWindow(L"22:00-02:00", t2300, true);         // wraps midnight
    CheckWindow(L"22:00-02:00", t0100, true);         // wraps midnight (after)
    CheckWindow(L"22:00-02:00", t1200, false);        // outside wrap window
    CheckWindow(L"abc", t1200, false);                // malformed -> fail closed
    CheckWindow(L"02:00", t0230, false);              // no range -> fail closed

    std::printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
