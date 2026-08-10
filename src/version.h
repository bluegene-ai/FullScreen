#pragma once
// ============================================================
// Version - generated/overridden by the build pipeline.
//   - GitHub Actions (build-release.yml) rewrites this file before
//     compiling, injecting the release version (e.g. 2026.08.10).
//   - Local builds keep the defaults below unless you edit by hand.
// The values must stay in sync with server/publish_update.php so the
// self-hosted update source can be compared against the running exe.
// ============================================================

#ifndef APP_VERSION_MAJOR
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_VERSION_BUILD 0
#endif

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "0.0.0"
#endif

#ifndef APP_VERSION
#define APP_VERSION APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, APP_VERSION_BUILD
#endif
