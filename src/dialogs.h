#pragma once
#include "app_common.h"
#include <Windows.h>

// ============================================================
// Dialog functions
// ============================================================

namespace Dialogs {

// Show the config/settings dialog.
// Returns true if user clicked OK (config saved).
// config is in/out: filled with current values on input, updated on output.
// plainPassword is in/out: pre-filled with current password on input,
// receives new plaintext password on output (caller must secure-zero after use)
bool ShowConfigDialog(HINSTANCE hInstance, HWND hParent, AppConfig& config,
                      std::wstring& plainPassword);

// Show the password verification dialog.
// Returns true if correct password was entered.
bool ShowPasswordDialog(HINSTANCE hInstance, HWND hParent,
                        std::wstring_view correctPassword);

// Show the action menu dialog (exit / modify settings).
// Returns: 0=cancel, 1=exit, 2=modify settings
int ShowMenuDialog(HINSTANCE hInstance, HWND hParent);

// Show the one-time device registration code dialog.
// allowCancel=false makes registration mandatory (Cancel hidden, Esc/close ignored).
// Returns true when user confirmed input; registerCode receives trimmed text.
bool ShowRegisterCodeDialog(HINSTANCE hInstance, HWND hParent, bool allowCancel,
                            std::wstring& registerCode);

} // namespace Dialogs
