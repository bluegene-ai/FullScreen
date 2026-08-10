// ============================================================
// test_refresh_schedule.cpp - runnable self-check for the daily
// multi-time refresh schedule helpers (RefreshSchedule).
//
// Build (VS2022 x64 dev prompt):
//   cl /nologo /W4 /EHsc /std:c++17 test_refresh_schedule.cpp ^
//       /Fe:test_refresh_schedule.exe
// ============================================================
#include "../src/refresh_schedule.cpp"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void Expect(bool cond, const char* what)
{
    std::printf("%-66s [%s]\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

static std::string ToAscii(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

static void CheckParse(const wchar_t* input, bool expectOk, const std::vector<int>& expectMins)
{
    std::vector<int> mins;
    bool ok = RefreshSchedule::ParseTimes(input, mins);
    bool pass = (ok == expectOk) && (!ok || mins == expectMins);
    std::string label = "ParseTimes(\"" + ToAscii(input) + "\")";
    Expect(pass, label.c_str());
    if (!pass) {
        std::printf("      got ok=%d mins=", ok ? 1 : 0);
        for (int m : mins) std::printf("%d ", m);
        std::printf("\n");
    }
}

static void CheckMs(const std::vector<int>& mins, int nowMin, int nowSec, int nowMs, unsigned expect)
{
    char buf[128];
    sprintf_s(buf, "MsUntilNextAt(now=%02d:%02d:%03d)", nowMin, nowSec, nowMs);
    unsigned got = RefreshSchedule::MsUntilNextAt(mins, nowMin, nowSec, nowMs);
    Expect(got == expect, buf);
    if (got != expect) std::printf("      got=%u expect=%u\n", got, expect);
}

int main()
{
    std::printf("RefreshSchedule self-check\n\n");

    // ---- ParseTimes ----
    CheckParse(L"08:00,16:00,24:00", true, { 0, 480, 960 });
    CheckParse(L"24:00", true, { 0 });
    CheckParse(L"00:00", true, { 0 });
    CheckParse(L"08:00, 16:00 ,24:00", true, { 0, 480, 960 });
    CheckParse(L"08:00,08:00", true, { 480 });          // dedupe
    CheckParse(L"08:00,24:00,16:00", true, { 0, 480, 960 }); // sort + 24:00->0
    CheckParse(L"8:00", true, { 480 });                 // leading zero optional
    CheckParse(L"", false, {});
    CheckParse(L"   ", false, {});
    CheckParse(L"08:60", false, {});                    // minutes > 59
    CheckParse(L"25:00", false, {});                    // hours > 24
    CheckParse(L"08:00,banana", false, {});             // any bad token -> fail
    CheckParse(L"08", false, {});                       // no colon
    CheckParse(L"08:", false, {});                      // empty minutes

    // ---- MsUntilNextAt (slots 00:00 / 08:00 / 16:00) ----
    const std::vector<int> s3 = { 0, 480, 960 };
    CheckMs(s3, 479,  0,   0, 60000);        // 07:59 -> 08:00 in 60s
    CheckMs(s3, 479, 30,   0, 30000);        // 07:59:30 -> 30s
    CheckMs(s3, 480,  0,   0, 28800000);     // at 08:00 sharp -> next is 16:00 (no re-fire)
    CheckMs(s3, 960,  0,   0, 28800000);     // at 16:00 sharp -> wraps to 00:00 tomorrow
    CheckMs(s3,   0,  0,   0, 28800000);     // at 00:00 sharp -> 08:00
    CheckMs(s3, 959, 59, 999, 1);            // 15:59:59.999 -> 16:00 in 1ms

    const std::vector<int> single = { 720 }; // legacy single time 12:00
    CheckMs(single, 719, 59, 999, 1);
    CheckMs(single, 720,  0,   0, 86400000); // 12:00 sharp -> tomorrow 12:00

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "All checks passed." : "FAILED.",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
