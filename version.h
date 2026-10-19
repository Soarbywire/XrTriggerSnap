// Single source of truth for the version and the GitHub repo the updater checks.
// To release: bump these numbers, commit, then push a tag "v<MAJOR>.<MINOR>.<PATCH>".
// The release workflow refuses to build if the tag and this file disagree.
#pragma once

#define XRTS_VERSION_MAJOR 1
#define XRTS_VERSION_MINOR 0
#define XRTS_VERSION_PATCH 1
#define XRTS_VERSION "1.0.1"

// owner/repo on GitHub whose Releases the tray app checks for updates.
#define XRTS_GITHUB_REPO "Soarbywire/XrTriggerSnap"

#define XRTS_WIDEN2(x) L##x
#define XRTS_WIDEN(x) XRTS_WIDEN2(x)
