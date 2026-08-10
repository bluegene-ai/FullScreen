// ============================================================
// AutoUpdate - pure logic (version compare, maintenance window).
// Kept free of Windows/network dependencies so it can be unit-tested
// directly (see tests/test_autoupdate.cpp).
// ============================================================
#include "auto_update.h"
#include <algorithm>
#include <ctime>
#include <vector>

namespace {

static void ParseVersionNums(const std::wstring& v, std::vector<int>& out)
{
    int cur = 0;
    bool inNum = false;
    for (wchar_t c : v) {
        if (c >= L'0' && c <= L'9') {
            cur = cur * 10 + (c - L'0');
            inNum = true;
        } else if (inNum) {
            out.push_back(cur);
            cur = 0;
            inNum = false;
        }
    }
    if (inNum) out.push_back(cur);
}

// Parses "HH:MM" at s[i], advances i. Strict.
static bool ParseHmToken(const std::wstring& s, size_t& i, int& out)
{
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
    if (i + 5 > s.size()) return false;
    int h = (s[i] - L'0') * 10 + (s[i + 1] - L'0');
    if (s[i + 2] != L':') return false;
    int m = (s[i + 3] - L'0') * 10 + (s[i + 4] - L'0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    i += 5;
    out = h * 60 + m;
    return true;
}

} // namespace

namespace AutoUpdate {

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current)
{
    std::vector<int> cN, aN;
    ParseVersionNums(candidate, cN);
    ParseVersionNums(current, aN);
    if (cN.empty()) return false;
    size_t n = std::max(cN.size(), aN.size());
    for (size_t i = 0; i < n; ++i) {
        int cv = i < cN.size() ? cN[i] : 0;
        int av = i < aN.size() ? aN[i] : 0;
        if (cv > av) return true;
        if (cv < av) return false;
    }
    return false; // equal
}

bool InUpdateWindow(const std::wstring& window, time_t now)
{
    if (window.empty()) return true; // no window constraint

    size_t i = 0;
    int start = 0, end = 0;
    if (!ParseHmToken(window, i, start)) return false; // malformed -> fail closed
    while (i < window.size() && window[i] == L' ') ++i;
    if (i >= window.size() || window[i] != L'-') return false;
    ++i;
    if (!ParseHmToken(window, i, end)) return false;

    struct tm tmv = {};
    localtime_s(&tmv, &now);
    int cur = tmv.tm_hour * 60 + tmv.tm_min;

    if (start < end)  return cur >= start && cur < end;
    if (start > end)  return cur >= start || cur < end; // wraps midnight
    return cur == start;
}

} // namespace AutoUpdate
