// Windows side of the updater: talks to GitHub Releases, downloads and verifies assets,
// stages them in <app folder>\update\, and swaps them into place.
#pragma once

#include <windows.h>
#include <string>

namespace updater {

struct Result {
    enum Kind { UpToDate, Available, Staged, Error } kind = Error;
    std::wstring version;  // latest version on GitHub (when known)
    std::wstring pageUrl;  // release page
    std::wstring message;  // error text, or release notes summary
    bool manual = false;   // the user asked for this check
};

// Checks GitHub in a background thread. If a newer version exists and `download` is true, it
// downloads and verifies it into the staging folder. Posts `msg` to `notify` with a heap
// Result* in LPARAM (the receiver deletes it). Only one check runs at a time; returns false
// if one is already running.
bool startCheck(HWND notify, UINT msg, const std::wstring& appDir, bool download, bool manual);
bool busy();

// Version string of a fully downloaded and verified update waiting in the staging folder,
// or empty if there isn't one.
std::wstring stagedVersion(const std::wstring& appDir);

// Swaps the staged files into the app folder (old copies are kept as *.old until the next
// start). Rolls back on failure. On success the caller should relaunch the new exe.
bool applyStaged(const std::wstring& appDir, std::wstring& err);

// Removes *.old files left by a previous update and any abandoned staging folder.
void cleanupOld(const std::wstring& appDir);

}  // namespace updater
