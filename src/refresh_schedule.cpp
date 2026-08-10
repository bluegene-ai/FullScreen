// ============================================================
// RefreshSchedule - daily multi-time refresh schedule helpers
// ============================================================
#include "refresh_schedule.h"
#include <Windows.h>
#include <algorithm>

namespace RefreshSchedule {

bool ParseTimes(const std::wstring& text, std::vector<int>& outMins)
{
    outMins.clear();

    // Split on comma / whitespace.
    std::vector<std::wstring> tokens;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    if (tokens.empty()) return false;

    for (const std::wstring& tok : tokens) {
        const size_t colon = tok.find(L':');
        if (colon == std::wstring::npos || colon == 0 || colon + 1 == tok.size()) {
            return false; // needs exactly "HH:MM"
        }
        int h = 0, m = 0;
        for (size_t i = 0; i < colon; i++) {
            if (tok[i] < L'0' || tok[i] > L'9') return false;
            h = h * 10 + (tok[i] - L'0');
        }
        for (size_t i = colon + 1; i < tok.size(); i++) {
            if (tok[i] < L'0' || tok[i] > L'9') return false;
            m = m * 10 + (tok[i] - L'0');
        }
        if (h == 24 && m == 0) h = 0; // "24:00" == midnight
        if (h > 23 || m > 59) return false;
        outMins.push_back(h * 60 + m);
    }

    std::sort(outMins.begin(), outMins.end());
    outMins.erase(std::unique(outMins.begin(), outMins.end()), outMins.end());
    return !outMins.empty();
}

unsigned MsUntilNextAt(const std::vector<int>& mins, int nowMin, int nowSec, int nowMs)
{
    // Earliest slot strictly after now (mins is sorted, so first hit is best).
    int delta = -1;
    for (int m : mins) {
        if (m > nowMin) {
            delta = m - nowMin;
            break;
        }
    }
    if (delta < 0) {
        // All slots passed today: wrap to the earliest slot tomorrow.
        delta = (mins[0] + 24 * 60) - nowMin;
    }

    // Land exactly on the target second (:00), then guard against rounding to 0.
    long long ms = (long long)delta * 60000;
    ms -= (long long)nowSec * 1000 + nowMs;
    if (ms <= 0) ms += 24LL * 60 * 60 * 1000;
    return (unsigned)ms;
}

unsigned MsUntilNext(const std::vector<int>& mins)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return MsUntilNextAt(mins, st.wHour * 60 + st.wMinute, st.wSecond, st.wMilliseconds);
}

} // namespace RefreshSchedule
