#pragma once
#include "app_common.h"
#include <string>

// ============================================================
// AutoUpdate - client self-update (GitHub releases / self-hosted)
//
// Two update sources share one pipeline:  throttle -> detect new version
// -> download -> verify -> stage -> apply on restart.
//   - Self-hosted: manifest latest.txt provides version + sha256 (strong verify).
//   - GitHub:      fixed latest/download URL; verified by size + FileVersion
//                  sanity, protected by HTTPS and the crash rollback.
// Crash safety: the replacement is done by a detached powershell (launched
// via -EncodedCommand, no script file) after the main process exits. A
// "pending"/"applied" marker + startup attempt counter roll the device back
// to the previous exe if the new one fails to stay up.
// ============================================================

namespace AutoUpdate {

enum class StartupResult {
    None,                 // nothing pending
    Confirming,           // an update was applied; arm the confirmation timer
    RolledBackRestarted,  // rolled back / retried staging; caller must exit now
};

enum class CheckResult {
    NoUpdate,             // disabled, throttled, up-to-date, or waiting for window
    AppliedRestartNeeded, // staged + detached swap launched; caller must exit & restart
    Error,                // transient failure; keep running
};

// Called early (before WebView2/UI). Handles pending-update bookkeeping:
// rolls back a crashing new exe, or retries an interrupted replacement.
StartupResult HandleStartupRecovery();

// Called by the main-loop timer once the new exe has survived long enough:
// drops the applied marker, the attempt counter and the .bak/.new files.
void ConfirmAndCleanup();

// Timer-driven update check (throttled; self 6h, github 24h/day).
CheckResult CheckAndApply(const AppConfig& cfg);

// --- helpers, public for the self-check test ---
// Strictly-newer comparison of dotted versions ("2026.08.10", "v1.2.3", ...).
bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current);

// True when `now` falls inside "HH:MM-HH:MM" (may wrap midnight).
// Empty window = always allowed; malformed window = never (fail closed).
bool InUpdateWindow(const std::wstring& window, time_t now);

// Compile-time running version (APP_VERSION_STR).
std::wstring CurrentVersion();

// Latest GitHub release download URL for the fixed asset name.
std::wstring GitHubLatestUrl(const std::wstring& repo);

} // namespace AutoUpdate
