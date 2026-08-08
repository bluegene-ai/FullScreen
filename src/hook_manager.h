#pragma once
#include <Windows.h>
#include <functional>

// ============================================================
// HookManager - global low-level keyboard hook
// ============================================================

namespace HookManager {

using PasswordCallback = std::function<void()>;
// Called when F5 (hardRefresh=false) or Ctrl+F5 (hardRefresh=true) is pressed
// while kiosk-locked. Optional; if unset, F5 stays swallowed like any other key.
using RefreshCallback = std::function<void(bool hardRefresh)>;

// Install WH_KEYBOARD_LL + WH_MOUSE_LL hooks. Returns true on success.
bool InstallHook(PasswordCallback onHotkey);

// Register the refresh hotkey handler (F5 / Ctrl+F5). May be null.
void SetRefreshCallback(RefreshCallback cb);

// Uninstall hooks and cleanup.
void UninstallHook();

// Kiosk lock: when true, swallow all keyboard (except ESC / Alt+F4 / Alt+Tab)
// and all mouse input. Call with false while dialogs are open so the user can
// type and click, then re-lock with true afterwards.
void SetInputLocked(bool locked);

} // namespace HookManager
