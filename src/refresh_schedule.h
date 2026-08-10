#pragma once
#include <string>
#include <vector>

// ============================================================
// RefreshSchedule - daily multi-time refresh schedule helpers.
// Times are "HH:MM" (24h); a list is comma/space separated, e.g.
// "08:00,16:00,24:00". "24:00" is accepted as midnight (00:00).
// ============================================================

namespace RefreshSchedule {

// Parse text into sorted unique minutes-of-day. Strict: returns false if
// there are no valid times or any token is malformed.
bool ParseTimes(const std::wstring& text, std::vector<int>& outMins);

// Milliseconds until the next slot strictly after nowMin:nowSec:nowMs,
// wrapping to the next day. mins must be non-empty & sorted (as produced
// by ParseTimes).
unsigned MsUntilNextAt(const std::vector<int>& mins, int nowMin, int nowSec, int nowMs);

// Convenience wrapper using the current local time.
unsigned MsUntilNext(const std::vector<int>& mins);

} // namespace RefreshSchedule
