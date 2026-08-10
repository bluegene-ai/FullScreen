#pragma once

// ============================================================
// AutoStart - register/unregister the "run at Windows login"
// entry in HKCU (no admin needed, no extra files, single-exe).
// ============================================================

namespace AutoStart {

// enable=true  -> write HKCU Run value pointing at this exe.
// enable=false -> remove the value (idempotent: no-op if absent).
// Returns true on success.
bool SetAutoStart(bool enable);

} // namespace AutoStart
