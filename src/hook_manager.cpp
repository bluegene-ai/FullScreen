// ============================================================
// HookManager - global low-level keyboard hook
// ============================================================
#include "hook_manager.h"
#include <Windows.h>
#include <mutex>
#include <atomic>

namespace HookManager {

static HHOOK g_hook = nullptr;
static HHOOK g_mouseHook = nullptr;
static PasswordCallback g_callback;
static RefreshCallback g_refreshCallback;
static std::mutex g_mutex;

// Kiosk lock: when true, all keyboard (except ESC / Alt+F4 / Alt+Tab) and all
// mouse input is swallowed. Disabled while password/menu dialogs are open so
// the user can type and click.
static std::atomic<bool> g_inputLocked{true};

// Tick when we last opened a dialog via ESC/Alt+F4. ESC auto-repeat shortly
// after that is swallowed so a held ESC doesn't instantly close the dialog we
// just showed (leaving no time to type the password).
static DWORD g_lastVerifyTick = 0;
static bool g_escArmed = false;
static bool g_altF4Armed = false;
static bool g_f5Armed = false;

// ============================================================
// True while our fullscreen main window is the foreground window.
// While any other window (dialog or another app) is foreground, kiosk
// input blocking is suspended so the user can type / click / switch tasks.
// ============================================================
static bool IsMainWindowForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {};
    if (GetClassNameW(fg, cls, 64) == 0) return false;
    return wcscmp(cls, L"FullScreenBrowserWnd") == 0;
}

// ============================================================
// Low-level keyboard procedure
// ============================================================
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        bool altDown = (kb->flags & LLKHF_ALTDOWN) != 0;

        if (keyDown || keyUp) {
            // A held ESC auto-repeats after we open the verification dialog;
            // swallow repeats for a short window so they don't instantly close
            // the dialog we just showed (no time to type the password).
            if (kb->vkCode == VK_ESCAPE &&
                (GetTickCount() - g_lastVerifyTick) < 400) {
                return 1;
            }

            // Pass input through when we are not kiosk-locked, or whenever
            // the foreground window is not our fullscreen main window (a
            // password/menu dialog is open, or the user Alt+Tab'ed away).
            if (!g_inputLocked || !IsMainWindowForeground()) {
                g_escArmed = false;
                g_altF4Armed = false;
                return CallNextHookEx(g_hook, nCode, wParam, lParam);
            }

            // Kiosk mode: only ESC, Alt+F4 and Alt+Tab are answered; every
            // other key is swallowed.
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) || altDown;

            // Arm triggers on keydown, execute on keyup. This prevents the
            // same physical key press from instantly cancelling the dialog.
            if (keyDown) {
                if (kb->vkCode == VK_ESCAPE && !alt && !ctrl) {
                    g_escArmed = true;
                    return 1;
                }
                if (kb->vkCode == VK_F4 && alt && !ctrl) {
                    g_altF4Armed = true;
                    return 1;
                }

                // F5 / Ctrl+F5: refresh (executed on keyup). Swallowed here so
                // it never reaches the page; we drive the reload ourselves.
                if (kb->vkCode == VK_F5) {
                    g_f5Armed = true;
                    return 1;
                }

                // Alt+Tab: system task switching. A low-level hook cannot stop
                // it; pass it through so it keeps working.
                if (kb->vkCode == VK_TAB && alt && !ctrl) {
                    g_escArmed = false;
                    g_altF4Armed = false;
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                }

                // Let the Alt key itself through so system Alt+ combos work.
                if (kb->vkCode == VK_MENU) {
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                }

                // Everything else: swallow (Start Menu, browser keys, typed
                // characters, etc.).
                return 1;
            }

            // Key-up path: execute armed actions.
            if (keyUp) {
                if (kb->vkCode == VK_ESCAPE && g_escArmed) {
                    g_escArmed = false;
                    g_altF4Armed = false;
                    if (g_callback) g_callback();
                    g_lastVerifyTick = GetTickCount();
                    return 1;
                }
                if (kb->vkCode == VK_F4 && g_altF4Armed) {
                    g_altF4Armed = false;
                    g_escArmed = false;
                    if (g_callback) g_callback();
                    g_lastVerifyTick = GetTickCount();
                    return 1;
                }

                if (kb->vkCode == VK_F5 && g_f5Armed) {
                    g_f5Armed = false;
                    if (g_refreshCallback) {
                        g_refreshCallback(ctrl);
                    }
                    return 1;
                }

                if (kb->vkCode == VK_MENU) {
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                }

                // Swallow key-up in locked mode to avoid leaking suppressed
                // keys to the page.
                return 1;
            }
        }
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ============================================================
// Low-level mouse procedure (kiosk input blocking)
// ============================================================
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_inputLocked && IsMainWindowForeground()) {
        // Swallow all mouse input while kiosk-locked and our main window is
        // foreground (clicks, scroll, movement). Dialogs pass through.
        return 1;
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ============================================================
// Public API
// ============================================================

bool InstallHook(PasswordCallback onHotkey)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_hook) return true; // already installed

    g_callback = std::move(onHotkey);
    g_inputLocked = true; // start in kiosk-locked state

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        return false;
    }

    // Mouse blocking is non-critical: proceed even if it fails.
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                     GetModuleHandleW(nullptr), 0);

    return true;
}

void UninstallHook()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    g_escArmed = false;
    g_altF4Armed = false;
    g_f5Armed = false;
    g_callback = nullptr;
    g_refreshCallback = nullptr;
}

// Register the F5 / Ctrl+F5 refresh handler (wired by main.cpp). May be null.
void SetRefreshCallback(RefreshCallback cb)
{
    g_refreshCallback = std::move(cb);
}

// Temporarily allow input while a password/menu/settings dialog is open.
void SetInputLocked(bool locked)
{
    g_inputLocked = locked;
}

} // namespace HookManager
