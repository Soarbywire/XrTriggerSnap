// XrTriggerSnap tray app.
//
// The OpenXR layer (XR_APILAYER_trigger_snap.dll) does the actual work inside X-Plane and
// works whether or not this app is running. This app is the control panel:
//   - tray icon showing whether the filter is on and whether X-Plane is using it
//   - settings window (writes trigger_snap.ini; the layer picks changes up live)
//   - install / remove the layer registration (asks for admin only for that)
//   - start with Windows, and launch with X-Plane
//
// Command line:
//   (none)        normal start
//   /startup      started by Windows at sign-in (stay quiet)
//   /xplane       started by the layer when X-Plane starts VR (exit when X-Plane closes)
//   /updated V    relaunched by the updater after replacing version V (shows "Updated" once)
//   /register     elevated helper: register the layer, then exit
//   /unregister   elevated helper: remove the layer registration, then exit
//
// Updates: checks GitHub Releases for XRTS_GITHUB_REPO (version.h) shortly after start and every
// 6 hours. A newer release is downloaded in the background, verified against SHA256SUMS.txt and
// swapped in when X-Plane isn't running (see updater.cpp). trigger_snap.ini is never replaced.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include "resource.h"
#include "../version.h"
#include "updater.h"

namespace {

const wchar_t* kAppName = L"XrTriggerSnap";
const wchar_t* kVersion = XRTS_WIDEN(XRTS_VERSION);
const wchar_t* kRepo = XRTS_WIDEN(XRTS_GITHUB_REPO);
const wchar_t* kTrayClass = L"XrTriggerSnapTray";  // the layer looks for this window class
const wchar_t* kSettingsClass = L"XrTriggerSnapSettings";
const wchar_t* kLayerKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kManifestName = L"XR_APILAYER_trigger_snap.json";
const wchar_t* kActiveMutex = L"Local\\XrTriggerSnapActive";

const UINT WM_TRAY = WM_APP + 1;
const UINT WM_SHOW_SETTINGS = WM_APP + 2;
const UINT WM_UPDATE_RESULT = WM_APP + 3;
const ULONGLONG kFirstCheckDelayMs = 15 * 1000;
const ULONGLONG kCheckIntervalMs = 6ULL * 60 * 60 * 1000;
const UINT TIMER_STATUS = 1;
UINT WM_TASKBAR_CREATED = 0;

enum MenuId {
    ID_ENABLED = 1000,
    ID_HANDS_BOTH,
    ID_HANDS_LEFT,
    ID_HANDS_RIGHT,
    ID_SETTINGS,
    ID_STARTUP,
    ID_LAUNCH_XPLANE,
    ID_INSTALL,
    ID_REMOVE,
    ID_OPEN_LOG,
    ID_OPEN_FOLDER,
    ID_ABOUT,
    ID_EXIT,
    ID_CHECK_UPDATES,
    ID_INSTALL_UPDATE,
    ID_PROJECT_PAGE,
};

enum CtlId {
    IDC_ENABLED = 2000,
    IDC_HANDS,
    IDC_CLOSE,
    IDC_CLOSE_SPIN,
    IDC_RELEASE,
    IDC_RELEASE_SPIN,
    IDC_OPEN,
    IDC_OPEN_SPIN,
    IDC_STARTUP,
    IDC_LAUNCH,
    IDC_LOG,
    IDC_STATUS,
    IDC_DEFAULTS,
    IDC_OPENLOG,
    IDC_SAVE,
    IDC_CANCEL,
    IDC_CHECK_UPDATES,
    IDC_AUTO_UPDATE,
    IDC_CHECK_NOW,
    IDC_UPDATE_STATUS,
};

// ---------------------------------------------------------------- defaults

struct Settings {
    bool enabled = true;
    int hands = 0;  // 0 both, 1 left, 2 right
    double closeCm = 1.1;
    double releaseCm = 3.0;
    double openCm = 7.0;
    bool launchWithXPlane = true;
    int logLevel = 1;
    bool checkUpdates = true;
    bool autoUpdate = true;
};

// ---------------------------------------------------------------- globals

HINSTANCE g_inst;
HWND g_tray;
HWND g_settings;
HICON g_iconOn, g_iconOff, g_iconOnLarge;
HFONT g_font;
std::wstring g_dir, g_exe, g_ini, g_manifest, g_dll, g_log;
bool g_fromXPlane = false;
bool g_seenXPlane = false;
int g_xplaneGoneSecs = 0;
int g_noXPlaneSecs = 0;
bool g_iconAdded = false;
std::wstring g_lastTip;
bool g_lastIconOn = true;
ULONGLONG g_savedUntil = 0;  // show "Saved." in the settings status until this tick
HANDLE g_single = nullptr;   // single-instance mutex (released before the updater relaunches us)

// Updater state (all on the UI thread; the worker only posts WM_UPDATE_RESULT).
ULONGLONG g_nextCheck = 0;        // tick of the next automatic check (0 = none scheduled)
std::wstring g_updateVersion;     // newer version known to be on GitHub
std::wstring g_updatePage;        // its release page
std::wstring g_updateStatus;      // one-line status for the settings window
std::wstring g_balloonShownFor;   // don't nag about the same version twice
bool g_installWhenReady = false;  // apply the staged update as soon as X-Plane isn't running
int g_cleanupTries = 0;

// ---------------------------------------------------------------- small helpers

std::wstring dirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool endsWithI(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string fmt1(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

void msg(HWND owner, const std::wstring& text, UINT flags = MB_ICONINFORMATION) {
    MessageBoxW(owner, text.c_str(), kAppName, flags | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------- ini (keeps comments and unknown keys)

std::vector<std::string> readLines() {
    std::vector<std::string> lines;
    FILE* f = _wfopen(g_ini.c_str(), L"rb");
    if (!f) return lines;
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += (char)c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    fclose(f);
    return lines;
}

bool iniGet(const std::vector<std::string>& lines, const std::string& key, std::string& out) {
    for (auto& l : lines) {
        std::string t = trim(l);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (trim(t.substr(0, eq)) == key) {
            out = trim(t.substr(eq + 1));
            return true;
        }
    }
    return false;
}

void iniSet(std::vector<std::string>& lines, const std::string& key, const std::string& value) {
    for (auto& l : lines) {
        std::string t = trim(l);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos && trim(t.substr(0, eq)) == key) {
            l = key + " = " + value;
            return;
        }
    }
    lines.push_back(key + " = " + value);
}

bool writeLines(const std::vector<std::string>& lines) {
    // Write to a temp file then swap, so the layer never reads a half-written file.
    std::wstring tmp = g_ini + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    for (auto& l : lines) {
        fputs(l.c_str(), f);
        fputs("\r\n", f);
    }
    bool ok = fclose(f) == 0;
    return ok && MoveFileExW(tmp.c_str(), g_ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool parseBool(const std::string& v) {
    std::string l = v;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

Settings loadSettings() {
    Settings s;
    auto lines = readLines();
    std::string v;
    if (iniGet(lines, "enabled", v)) s.enabled = parseBool(v);
    if (iniGet(lines, "hands", v)) s.hands = v == "left" ? 1 : v == "right" ? 2 : 0;
    if (iniGet(lines, "joint_close_cm", v)) s.closeCm = atof(v.c_str());
    if (iniGet(lines, "joint_release_cm", v)) s.releaseCm = atof(v.c_str());
    if (iniGet(lines, "joint_open_cm", v)) s.openCm = atof(v.c_str());
    if (iniGet(lines, "launch_tray", v)) s.launchWithXPlane = parseBool(v);
    if (iniGet(lines, "log_level", v)) s.logLevel = std::max(0, std::min(2, atoi(v.c_str())));
    if (iniGet(lines, "check_updates", v)) s.checkUpdates = parseBool(v);
    if (iniGet(lines, "auto_update", v)) s.autoUpdate = parseBool(v);
    return s;
}

bool saveSettings(const Settings& s) {
    auto lines = readLines();
    iniSet(lines, "enabled", s.enabled ? "1" : "0");
    iniSet(lines, "hands", s.hands == 1 ? "left" : s.hands == 2 ? "right" : "both");
    iniSet(lines, "joint_close_cm", fmt1(s.closeCm));
    iniSet(lines, "joint_release_cm", fmt1(s.releaseCm));
    iniSet(lines, "joint_open_cm", fmt1(s.openCm));
    iniSet(lines, "launch_tray", s.launchWithXPlane ? "1" : "0");
    iniSet(lines, "log_level", std::to_string(s.logLevel));
    iniSet(lines, "check_updates", s.checkUpdates ? "1" : "0");
    iniSet(lines, "auto_update", s.autoUpdate ? "1" : "0");
    return writeLines(lines);
}

bool saveOne(const std::string& key, const std::string& value) {
    auto lines = readLines();
    iniSet(lines, key, value);
    return writeLines(lines);
}

// ---------------------------------------------------------------- registry

enum class LayerState { Installed, OtherFolder, NotInstalled };

LayerState layerState() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLayerKey, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return LayerState::NotInstalled;
    LayerState st = LayerState::NotInstalled;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD nameLen = 1024, type = 0, data = 0, dataLen = sizeof(data);
        LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, (BYTE*)&data, &dataLen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA) continue;
        std::wstring n(name, nameLen);
        if (!endsWithI(n, std::wstring(L"\\") + kManifestName)) continue;
        if (lower(n) == lower(g_manifest) && type == REG_DWORD && data == 0) {
            st = LayerState::Installed;
            break;
        }
        st = LayerState::OtherFolder;
    }
    RegCloseKey(k);
    return st;
}

// Runs in the elevated helper process.
int doRegister(bool add) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kLayerKey, 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr,
                        &k, nullptr) != ERROR_SUCCESS)
        return 2;
    // Remove every registration of this layer (old folders included)...
    std::vector<std::wstring> stale;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD nameLen = 1024;
        LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        std::wstring n(name, nameLen);
        if (endsWithI(n, std::wstring(L"\\") + kManifestName)) stale.push_back(n);
    }
    for (auto& n : stale) RegDeleteValueW(k, n.c_str());
    // ...then add this folder's (0 = enabled).
    LONG r = ERROR_SUCCESS;
    if (add) {
        DWORD zero = 0;
        r = RegSetValueExW(k, g_manifest.c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? 0 : 3;
}

// Starts this exe elevated with /register or /unregister and waits for it.
bool runElevated(HWND owner, bool add) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = g_exe.c_str();
    sei.lpParameters = add ? L"/register" : L"/unregister";
    sei.lpDirectory = g_dir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (GetLastError() != ERROR_CANCELLED) msg(owner, L"Couldn't start the installer (admin rights are needed).", MB_ICONERROR);
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        msg(owner, L"Changing the layer registration failed.", MB_ICONERROR);
        return false;
    }
    return true;
}

bool startupEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[2048];
    DWORD len = sizeof(buf), type = 0;
    bool on = RegQueryValueExW(k, kAppName, nullptr, &type, (BYTE*)buf, &len) == ERROR_SUCCESS && type == REG_SZ;
    // Treat an entry pointing at another copy as "off" for this copy.
    if (on) on = lower(buf).find(lower(g_exe)) != std::wstring::npos;
    RegCloseKey(k);
    return on;
}

bool setStartup(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (on) {
        std::wstring cmd = L"\"" + g_exe + L"\" /startup";
        r = RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, kAppName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool xplaneActive() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kActiveMutex);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------- tray icon

std::wstring statusText(LayerState ls, bool active, bool enabled) {
    if (ls == LayerState::NotInstalled) return L"Layer not installed";
    if (ls == LayerState::OtherFolder) return L"Layer installed from another folder";
    std::wstring s = active ? L"Active in X-Plane" : L"Waiting for X-Plane";
    if (!enabled) s += L" (filter off)";
    return s;
}

void updateTray(bool force = false) {
    if (!g_tray || !IsWindow(g_tray)) return;  // never re-add an icon for a closing window
    Settings s = loadSettings();
    LayerState ls = layerState();
    bool active = xplaneActive();
    std::wstring tip = std::wstring(kAppName) + L"\n" + statusText(ls, active, s.enabled);
    bool iconOn = s.enabled && ls == LayerState::Installed;
    if (!force && g_iconAdded && tip == g_lastTip && iconOn == g_lastIconOn) return;

    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_tray;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = iconOn ? g_iconOn : g_iconOff;
    wcsncpy(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip) - 1);
    if (!g_iconAdded || force) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_iconAdded = Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    g_lastTip = tip;
    g_lastIconOn = iconOn;
}

void removeTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_tray;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_iconAdded = false;
}

void openLog(HWND owner) {
    std::wstring path = g_log;
    if (!fileExists(path)) {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) path = std::wstring(tmp) + L"trigger_snap.log";
    }
    if (!fileExists(path)) {
        msg(owner, L"No log yet. It's created the first time X-Plane starts VR with the layer installed.");
        return;
    }
    ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showSettings();
void refreshSettingsStatus();

// ---------------------------------------------------------------- updates

void showBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g_tray || !g_iconAdded) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_tray;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;
    wcsncpy(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring projectUrl() { return std::wstring(L"https://github.com/") + kRepo; }

void beginCheck(bool manual, bool download) {
    if (updater::busy()) {
        if (manual) msg(g_settings, L"Already checking for updates...");
        return;
    }
    if (updater::startCheck(g_tray, WM_UPDATE_RESULT, g_dir, download, manual)) {
        g_updateStatus = download ? L"Downloading update..." : L"Checking for updates...";
        refreshSettingsStatus();
    }
}

// Releases the single-instance mutex, starts the (new) exe and closes this one.
void relaunch(const std::wstring& args) {
    removeTray();
    if (g_single) {
        ReleaseMutex(g_single);
        CloseHandle(g_single);
        g_single = nullptr;
    }
    std::wstring cmd = L"\"" + g_exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(g_exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (g_settings) DestroyWindow(g_settings);
    if (g_tray) DestroyWindow(g_tray);
}

// Installs a downloaded, verified update if X-Plane isn't using the files right now.
void tryApplyUpdate(bool interactive) {
    std::wstring v = updater::stagedVersion(g_dir);
    if (v.empty()) return;
    if (xplaneActive()) {
        g_updateStatus = L"Version " + v + L" downloaded; installs when X-Plane closes";
        if (interactive)
            msg(g_settings, L"Version " + v + L" is downloaded and will install automatically as soon as X-Plane closes.");
        refreshSettingsStatus();
        return;
    }
    std::wstring err;
    if (updater::applyStaged(g_dir, err)) {
        g_installWhenReady = false;
        relaunch(L"/updated " + std::wstring(kVersion) + (g_fromXPlane ? L" /xplane" : L""));
    } else {
        g_installWhenReady = false;
        g_updateStatus = L"Update failed: " + err;
        refreshSettingsStatus();
        if (interactive) msg(g_settings, L"The update couldn't be installed: " + err, MB_ICONERROR);
    }
}

void installUpdateNow() {
    g_installWhenReady = true;
    if (!updater::stagedVersion(g_dir).empty())
        tryApplyUpdate(true);
    else
        beginCheck(true, true);
}

std::wstring trimNotes(std::wstring n) {
    for (auto& c : n)
        if (c == L'\r') c = L' ';
    if (n.size() > 700) n = n.substr(0, 700) + L"...";
    return n;
}

void onUpdateResult(updater::Result* r) {
    Settings s = loadSettings();
    switch (r->kind) {
        case updater::Result::UpToDate:
            g_updateVersion.clear();
            g_updateStatus = L"Up to date (version " + std::wstring(kVersion) + L")";
            if (r->manual) msg(g_settings, L"You have the latest version (" + std::wstring(kVersion) + L").");
            break;
        case updater::Result::Available:
            g_updateVersion = r->version;
            g_updatePage = r->pageUrl;
            g_updateStatus = L"Version " + r->version + L" is available";
            if (r->manual) {
                std::wstring q = L"XrTriggerSnap " + r->version + L" is available (you have " + kVersion + L").\n\n";
                if (!r->message.empty()) q += L"What's new:\n" + trimNotes(r->message) + L"\n\n";
                q += L"Download and install it now?";
                if (MessageBoxW(g_settings, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES)
                    installUpdateNow();
            } else if (g_balloonShownFor != r->version) {
                g_balloonShownFor = r->version;
                showBalloon(L"XrTriggerSnap " + r->version + L" is available",
                            L"Click here, or right-click the tray icon, to install it.");
            }
            break;
        case updater::Result::Staged:
            g_updateVersion = r->version;
            g_updatePage = r->pageUrl;
            g_updateStatus = L"Version " + r->version + L" downloaded";
            if (s.autoUpdate || r->manual) {
                g_installWhenReady = true;
                tryApplyUpdate(r->manual);
            }
            break;
        case updater::Result::Error:
            g_updateStatus = L"Update check failed: " + r->message;
            if (r->manual) msg(g_settings, L"Couldn't check for updates: " + r->message, MB_ICONWARNING);
            break;
    }
    refreshSettingsStatus();
    updateTray(true);
}

void installLayer(HWND owner) {
    if (!fileExists(g_manifest) || !fileExists(g_dll)) {
        msg(owner, L"XR_APILAYER_trigger_snap.json and XR_APILAYER_trigger_snap.dll must be in the same folder as XrTriggerSnap.exe.",
            MB_ICONERROR);
        return;
    }
    if (runElevated(owner, true)) {
        updateTray(true);
        refreshSettingsStatus();
        msg(owner, L"Layer installed. It will be active next time X-Plane starts VR.");
    }
}

void removeLayer(HWND owner) {
    if (MessageBoxW(owner, L"Remove the layer? X-Plane will go back to its normal pinch and laser behaviour.", kAppName,
                    MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
        return;
    if (runElevated(owner, false)) {
        updateTray(true);
        refreshSettingsStatus();
        msg(owner, L"Layer removed. Restart X-Plane for it to take effect.");
    }
}

void showMenu(HWND hwnd, POINT pt) {
    Settings s = loadSettings();
    LayerState ls = layerState();
    bool active = xplaneActive();

    HMENU m = CreatePopupMenu();
    std::wstring title = std::wstring(kAppName) + L" " + kVersion;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, title.c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, statusText(ls, active, s.enabled).c_str());
    if (!g_updateVersion.empty()) {
        std::wstring u = L"Install &update " + g_updateVersion + L"...";
        AppendMenuW(m, MF_STRING, ID_INSTALL_UPDATE, u.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.enabled ? MF_CHECKED : 0), ID_ENABLED, L"&Filter on");
    HMENU hands = CreatePopupMenu();
    AppendMenuW(hands, MF_STRING, ID_HANDS_BOTH, L"&Both hands");
    AppendMenuW(hands, MF_STRING, ID_HANDS_LEFT, L"&Left hand only");
    AppendMenuW(hands, MF_STRING, ID_HANDS_RIGHT, L"&Right hand only");
    CheckMenuRadioItem(hands, ID_HANDS_BOTH, ID_HANDS_RIGHT, ID_HANDS_BOTH + s.hands, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)hands, L"&Hands");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"&Settings...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with &Windows");
    AppendMenuW(m, MF_STRING | (s.launchWithXPlane ? MF_CHECKED : 0), ID_LAUNCH_XPLANE, L"Start with &X-Plane");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (ls == LayerState::Installed)
        AppendMenuW(m, MF_STRING, ID_REMOVE, L"&Remove layer...");
    else
        AppendMenuW(m, MF_STRING, ID_INSTALL, ls == LayerState::OtherFolder ? L"&Install layer from this folder..." : L"&Install layer...");
    AppendMenuW(m, MF_STRING, ID_OPEN_LOG, L"Open &log");
    AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"Open f&older");
    AppendMenuW(m, MF_STRING, ID_CHECK_UPDATES, L"&Check for updates");
    AppendMenuW(m, MF_STRING, ID_PROJECT_PAGE, L"&Project page on GitHub");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"&About");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"E&xit");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);

    SetForegroundWindow(hwnd);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    TrackPopupMenuEx(m, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void onMenu(HWND hwnd, int id) {
    switch (id) {
        case ID_ENABLED: {
            Settings s = loadSettings();
            saveOne("enabled", s.enabled ? "0" : "1");
            break;
        }
        case ID_HANDS_BOTH: saveOne("hands", "both"); break;
        case ID_HANDS_LEFT: saveOne("hands", "left"); break;
        case ID_HANDS_RIGHT: saveOne("hands", "right"); break;
        case ID_SETTINGS: showSettings(); break;
        case ID_STARTUP:
            if (!setStartup(!startupEnabled())) msg(hwnd, L"Couldn't change the Windows startup setting.", MB_ICONERROR);
            break;
        case ID_LAUNCH_XPLANE: {
            Settings s = loadSettings();
            saveOne("launch_tray", s.launchWithXPlane ? "0" : "1");
            break;
        }
        case ID_INSTALL: installLayer(hwnd); break;
        case ID_REMOVE: removeLayer(hwnd); break;
        case ID_OPEN_LOG: openLog(hwnd); break;
        case ID_OPEN_FOLDER: ShellExecuteW(hwnd, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_CHECK_UPDATES: beginCheck(true, false); break;
        case ID_INSTALL_UPDATE: installUpdateNow(); break;
        case ID_PROJECT_PAGE: ShellExecuteW(hwnd, L"open", projectUrl().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_ABOUT:
            msg(hwnd, std::wstring(kAppName) + L" " + kVersion +
                          L"\n\nStops X-Plane's VR laser from appearing on a half pinch during hand tracking.\n"
                          L"The filter runs inside X-Plane and keeps working even if this tray app is closed.\n\n"
                          L"Folder: " + g_dir + L"\n" + projectUrl());
            break;
        case ID_EXIT:
            if (g_settings) DestroyWindow(g_settings);
            DestroyWindow(hwnd);
            return;
    }
    // Keep an open settings window in step with what the menu just changed.
    if (g_settings) {
        Settings s = loadSettings();
        CheckDlgButton(g_settings, IDC_ENABLED, s.enabled ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(g_settings, IDC_HANDS, CB_SETCURSEL, s.hands, 0);
        CheckDlgButton(g_settings, IDC_LAUNCH, s.launchWithXPlane ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_settings, IDC_STARTUP, startupEnabled() ? BST_CHECKED : BST_UNCHECKED);
    }
    updateTray();
    refreshSettingsStatus();
}

// ---------------------------------------------------------------- settings window

int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

HWND ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
         DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

double getNum(HWND dlg, int id, double fallback) {
    wchar_t buf[64];
    GetDlgItemTextW(dlg, id, buf, 64);
    for (wchar_t* p = buf; *p; ++p)
        if (*p == L',') *p = L'.';
    wchar_t* end = nullptr;
    double v = wcstod(buf, &end);
    return (end == buf) ? fallback : v;
}

void setNum(HWND dlg, int id, double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", v);
    SetDlgItemTextW(dlg, id, buf);
}

void fillSettings(HWND dlg, const Settings& s, bool startup) {
    CheckDlgButton(dlg, IDC_ENABLED, s.enabled ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_HANDS, CB_SETCURSEL, s.hands, 0);
    setNum(dlg, IDC_CLOSE, s.closeCm);
    setNum(dlg, IDC_RELEASE, s.releaseCm);
    setNum(dlg, IDC_OPEN, s.openCm);
    CheckDlgButton(dlg, IDC_STARTUP, startup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_LAUNCH, s.launchWithXPlane ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_LOG, CB_SETCURSEL, s.logLevel, 0);
    CheckDlgButton(dlg, IDC_CHECK_UPDATES, s.checkUpdates ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_AUTO_UPDATE, s.autoUpdate ? BST_CHECKED : BST_UNCHECKED);
}

void refreshSettingsStatus() {
    if (!g_settings) return;
    Settings s = loadSettings();
    std::wstring t = std::wstring(GetTickCount64() < g_savedUntil ? L"Saved.  " : L"") +
                     L"Status: " + statusText(layerState(), xplaneActive(), s.enabled);
    SetDlgItemTextW(g_settings, IDC_STATUS, t.c_str());
    std::wstring u = g_updateStatus.empty() ? L"Version " + std::wstring(kVersion) : g_updateStatus;
    wchar_t cur[512];
    GetDlgItemTextW(g_settings, IDC_UPDATE_STATUS, cur, 512);
    if (u != cur) SetDlgItemTextW(g_settings, IDC_UPDATE_STATUS, u.c_str());
}

bool applySettings(HWND dlg) {
    Settings s = loadSettings();
    s.enabled = IsDlgButtonChecked(dlg, IDC_ENABLED) == BST_CHECKED;
    s.hands = (int)SendDlgItemMessageW(dlg, IDC_HANDS, CB_GETCURSEL, 0, 0);
    if (s.hands < 0) s.hands = 0;
    s.closeCm = getNum(dlg, IDC_CLOSE, s.closeCm);
    s.releaseCm = getNum(dlg, IDC_RELEASE, s.releaseCm);
    s.openCm = getNum(dlg, IDC_OPEN, s.openCm);
    s.launchWithXPlane = IsDlgButtonChecked(dlg, IDC_LAUNCH) == BST_CHECKED;
    s.logLevel = (int)SendDlgItemMessageW(dlg, IDC_LOG, CB_GETCURSEL, 0, 0);
    if (s.logLevel < 0) s.logLevel = 1;
    bool wasChecking = s.checkUpdates;
    s.checkUpdates = IsDlgButtonChecked(dlg, IDC_CHECK_UPDATES) == BST_CHECKED;
    s.autoUpdate = IsDlgButtonChecked(dlg, IDC_AUTO_UPDATE) == BST_CHECKED;
    if (s.checkUpdates && !wasChecking) g_nextCheck = GetTickCount64() + 2000;  // just switched on: check soon
    if (!s.checkUpdates) g_nextCheck = 0;

    if (s.closeCm < 0.3 || s.closeCm > 5.0) {
        msg(dlg, L"Pinch distance should be between 0.3 and 5 cm.", MB_ICONWARNING);
        return false;
    }
    if (s.releaseCm < s.closeCm || s.releaseCm > 10.0) {
        msg(dlg, L"Release distance must be at least the pinch distance (and no more than 10 cm).", MB_ICONWARNING);
        return false;
    }
    if (s.openCm < s.releaseCm || s.openCm > 20.0) {
        msg(dlg, L"Open-hand distance must be at least the release distance (and no more than 20 cm).", MB_ICONWARNING);
        return false;
    }
    if (!saveSettings(s)) {
        msg(dlg, L"Couldn't save trigger_snap.ini. Is the folder read-only?", MB_ICONERROR);
        return false;
    }
    bool wantStartup = IsDlgButtonChecked(dlg, IDC_STARTUP) == BST_CHECKED;
    if (wantStartup != startupEnabled() && !setStartup(wantStartup))
        msg(dlg, L"Couldn't change the Windows startup setting.", MB_ICONERROR);
    fillSettings(dlg, s, startupEnabled());  // show normalised values
    updateTray();
    return true;
}

LRESULT CALLBACK settingsProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: {
            // Layout in 96-dpi units; S() scales.
            int y = 12;
            ctl(hwnd, L"BUTTON", L"Filter", BS_GROUPBOX, 12, y, 396, 74, 0);
            ctl(hwnd, L"BUTTON", L"&Filter on (the laser only appears on a full pinch)", BS_AUTOCHECKBOX | WS_TABSTOP,
                24, y + 20, 370, 20, IDC_ENABLED);
            ctl(hwnd, L"STATIC", L"&Apply to:", SS_LEFT, 24, y + 47, 80, 18, 0);
            HWND hands = ctl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 110, y + 43, 150,
                             120, IDC_HANDS);
            SendMessageW(hands, CB_ADDSTRING, 0, (LPARAM)L"Both hands");
            SendMessageW(hands, CB_ADDSTRING, 0, (LPARAM)L"Left hand only");
            SendMessageW(hands, CB_ADDSTRING, 0, (LPARAM)L"Right hand only");

            y += 86;
            ctl(hwnd, L"BUTTON", L"Pinch distances (thumb tip to index tip)", BS_GROUPBOX, 12, y, 396, 132, 0);
            struct Row {
                const wchar_t* label;
                const wchar_t* hint;
                int edit, spin;
            } rows[] = {
                {L"&Pinch at", L"lower = fewer accidental pinches", IDC_CLOSE, IDC_CLOSE_SPIN},
                {L"&Release at", L"stays pinched until this open", IDC_RELEASE, IDC_RELEASE_SPIN},
                {L"&Open hand", L"half pinches are shown as this", IDC_OPEN, IDC_OPEN_SPIN},
            };
            int ry = y + 24;
            for (auto& r : rows) {
                ctl(hwnd, L"STATIC", r.label, SS_LEFT, 24, ry + 3, 80, 18, 0);
                ctl(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 110, ry, 52, 22, r.edit, WS_EX_CLIENTEDGE);
                ctl(hwnd, UPDOWN_CLASSW, L"", UDS_ARROWKEYS | UDS_ALIGNRIGHT, 162, ry, 18, 22, r.spin);
                SendDlgItemMessageW(hwnd, r.spin, UDM_SETBUDDY, (WPARAM)GetDlgItem(hwnd, r.edit), 0);
                SendDlgItemMessageW(hwnd, r.spin, UDM_SETRANGE32, 0, 1000);
                SendDlgItemMessageW(hwnd, r.spin, UDM_SETPOS32, 0, 500);  // never moves; arrows always fire
                ctl(hwnd, L"STATIC", L"cm", SS_LEFT, 186, ry + 3, 24, 18, 0);
                ctl(hwnd, L"STATIC", r.hint, SS_LEFT | SS_ENDELLIPSIS, 214, ry + 3, 186, 18, 0);
                ry += 34;
            }

            y += 144;
            ctl(hwnd, L"BUTTON", L"Startup", BS_GROUPBOX, 12, y, 396, 74, 0);
            ctl(hwnd, L"BUTTON", L"Start with &Windows (sits in the tray)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 20,
                370, 20, IDC_STARTUP);
            ctl(hwnd, L"BUTTON", L"Start with &X-Plane (opens when VR starts, closes with X-Plane)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 44, 378, 20, IDC_LAUNCH);

            y += 86;
            ctl(hwnd, L"BUTTON", L"Updates", BS_GROUPBOX, 12, y, 396, 100, 0);
            ctl(hwnd, L"BUTTON", L"Check for &updates automatically", BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 20, 370,
                20, IDC_CHECK_UPDATES);
            ctl(hwnd, L"BUTTON", L"&Install updates automatically (when X-Plane isn't running)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 44, 378, 20, IDC_AUTO_UPDATE);
            ctl(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 24, y + 74, 278, 18, IDC_UPDATE_STATUS);
            ctl(hwnd, L"BUTTON", L"Check &now", BS_PUSHBUTTON | WS_TABSTOP, 308, y + 68, 90, 26, IDC_CHECK_NOW);

            y += 112;
            ctl(hwnd, L"STATIC", L"&Logging:", SS_LEFT, 12, y + 4, 60, 18, 0);
            HWND log = ctl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 76, y, 120, 120, IDC_LOG);
            SendMessageW(log, CB_ADDSTRING, 0, (LPARAM)L"Off");
            SendMessageW(log, CB_ADDSTRING, 0, (LPARAM)L"Normal");
            SendMessageW(log, CB_ADDSTRING, 0, (LPARAM)L"Detailed");
            ctl(hwnd, L"BUTTON", L"Open lo&g", BS_PUSHBUTTON | WS_TABSTOP, 206, y - 1, 90, 26, IDC_OPENLOG);

            y += 36;
            ctl(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 12, y, 396, 18, IDC_STATUS);
            ctl(hwnd, L"STATIC", L"Changes apply within a second, even mid-flight.", SS_LEFT, 12, y + 18, 396, 18, 0);

            y += 46;
            ctl(hwnd, L"BUTTON", L"&Defaults", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 90, 28, IDC_DEFAULTS);
            ctl(hwnd, L"BUTTON", L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 222, y, 90, 28, IDC_SAVE);
            ctl(hwnd, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 318, y, 90, 28, IDC_CANCEL);

            fillSettings(hwnd, loadSettings(), startupEnabled());
            refreshSettingsStatus();
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->code == UDN_DELTAPOS) {
                auto* ud = reinterpret_cast<NMUPDOWN*>(lp);
                int edit = (int)nm->idFrom - 1;  // each spin id follows its edit id
                double v = getNum(hwnd, edit, 0.0) + ud->iDelta * 0.1;
                setNum(hwnd, edit, std::max(0.1, std::min(20.0, v)));
                return 1;  // we handled it; don't let the control change its own position
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_DEFAULTS: fillSettings(hwnd, Settings(), IsDlgButtonChecked(hwnd, IDC_STARTUP) == BST_CHECKED); return 0;
                case IDC_OPENLOG: openLog(hwnd); return 0;
                case IDC_CHECK_NOW: beginCheck(true, false); return 0;
                case IDC_SAVE:
                case IDOK:
                    if (applySettings(hwnd)) {
                        g_savedUntil = GetTickCount64() + 4000;
                        refreshSettingsStatus();
                    }
                    return 0;
                case IDC_CANCEL:
                case IDCANCEL: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: g_settings = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

void showSettings() {
    if (g_settings) {
        ShowWindow(g_settings, SW_SHOWNORMAL);
        SetForegroundWindow(g_settings);
        return;
    }
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    RECT rc{0, 0, S(420), S(568)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    std::wstring title = std::wstring(kAppName) + L" Settings";
    g_settings = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kSettingsClass, title.c_str(), style,
                                 work.right - w - S(24), work.bottom - h - S(24), w, h, nullptr, nullptr, g_inst,
                                 nullptr);
    SendMessageW(g_settings, WM_SETICON, ICON_BIG, (LPARAM)g_iconOnLarge);
    SendMessageW(g_settings, WM_SETICON, ICON_SMALL, (LPARAM)g_iconOn);
    ShowWindow(g_settings, SW_SHOWNORMAL);
    SetForegroundWindow(g_settings);
}

// ---------------------------------------------------------------- tray window

LRESULT CALLBACK trayProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TASKBAR_CREATED && WM_TASKBAR_CREATED) {  // Explorer restarted: put the icon back
        g_iconAdded = false;
        updateTray(true);
        return 0;
    }
    switch (m) {
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: {
                    POINT pt{(short)LOWORD(wp), (short)HIWORD(wp)};
                    showMenu(hwnd, pt);
                    return 0;
                }
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONDBLCLK: showSettings(); return 0;
                case NIN_BALLOONUSERCLICK:
                    if (!g_updateVersion.empty() && updater::stagedVersion(g_dir).empty() && !g_installWhenReady)
                        installUpdateNow();
                    else
                        showSettings();
                    return 0;
            }
            return 0;
        case WM_SHOW_SETTINGS: showSettings(); return 0;
        case WM_UPDATE_RESULT: {
            auto* r = reinterpret_cast<updater::Result*>(lp);
            if (r) {
                onUpdateResult(r);
                delete r;
            }
            return 0;
        }
        case WM_COMMAND: onMenu(hwnd, LOWORD(wp)); return 0;
        case WM_TIMER: {
            bool active = xplaneActive();
            ULONGLONG now = GetTickCount64();

            // Leftovers from the previous version can only be deleted once it has exited.
            if (g_cleanupTries < 30 && (++g_cleanupTries % 5) == 0) updater::cleanupOld(g_dir);

            // A downloaded update goes in as soon as X-Plane isn't using the files.
            if (g_installWhenReady && !active && !updater::busy()) {
                tryApplyUpdate(false);
                if (!g_tray) return 0;  // relaunching
            }

            // Scheduled update checks.
            if (g_nextCheck && now >= g_nextCheck) {
                Settings us = loadSettings();
                g_nextCheck = us.checkUpdates ? now + kCheckIntervalMs : 0;
                if (us.checkUpdates) beginCheck(false, us.autoUpdate);
            }

            if (g_fromXPlane) {
                // Started by X-Plane: close again once X-Plane has gone (unless a window is open).
                if (active) {
                    g_seenXPlane = true;
                    g_xplaneGoneSecs = 0;
                } else if (g_seenXPlane) {
                    if (++g_xplaneGoneSecs >= 10 && !g_settings && !updater::busy() && !g_installWhenReady) {
                        DestroyWindow(hwnd);
                        return 0;
                    }
                } else if (++g_noXPlaneSecs >= 120 && !g_settings && !updater::busy() && !g_installWhenReady) {
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            updateTray();
            refreshSettingsStatus();
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_STATUS);
            removeTray();
            g_tray = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = inst;
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    g_exe = path;
    g_dir = dirOf(g_exe);
    g_ini = g_dir + L"\\trigger_snap.ini";
    g_manifest = g_dir + L"\\" + kManifestName;
    g_dll = g_dir + L"\\XR_APILAYER_trigger_snap.dll";
    g_log = g_dir + L"\\trigger_snap.log";

    std::wstring args = lower(cmdLine ? cmdLine : L"");
    if (args.find(L"/register") != std::wstring::npos && args.find(L"/unregister") == std::wstring::npos)
        return doRegister(true);
    if (args.find(L"/unregister") != std::wstring::npos) return doRegister(false);
    g_fromXPlane = args.find(L"/xplane") != std::wstring::npos;
    bool fromStartup = args.find(L"/startup") != std::wstring::npos;
    bool justUpdated = args.find(L"/updated") != std::wstring::npos;

    // One tray icon only. A second manual launch just opens the settings of the first.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\XrTriggerSnapTrayInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && justUpdated) {
        // The old version is still shutting down; give it a moment to let go.
        CloseHandle(single);
        single = nullptr;
        for (int i = 0; i < 50 && !single; ++i) {
            Sleep(100);
            HANDLE h = CreateMutexW(nullptr, TRUE, L"Local\\XrTriggerSnapTrayInstance");
            if (GetLastError() == ERROR_ALREADY_EXISTS) CloseHandle(h);
            else single = h;
        }
        if (!single) return 0;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!g_fromXPlane && !fromStartup) {
            HWND other = FindWindowW(kTrayClass, nullptr);
            if (other) {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, WM_SHOW_SETTINGS, 0, 0);
            }
        }
        return 0;
    }

    g_single = single;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    int sm = GetSystemMetrics(SM_CXSMICON);
    g_iconOn = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, sm, sm, 0);
    g_iconOff = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_OFF), IMAGE_ICON, sm, sm, 0);
    g_iconOnLarge = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), 0);

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = trayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    wc.hIcon = g_iconOnLarge;
    RegisterClassExW(&wc);

    WNDCLASSEXW sc{sizeof(sc)};
    sc.lpfnWndProc = settingsProc;
    sc.hInstance = inst;
    sc.lpszClassName = kSettingsClass;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    sc.hIcon = g_iconOnLarge;
    sc.hIconSm = g_iconOn;
    RegisterClassExW(&sc);

    WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray = CreateWindowExW(0, kTrayClass, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (WM_TASKBAR_CREATED) ChangeWindowMessageFilterEx(g_tray, WM_TASKBAR_CREATED, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_tray, WM_SHOW_SETTINGS, MSGFLT_ALLOW, nullptr);

    // Fresh copy with no ini: write the defaults so the file is there to edit.
    if (!fileExists(g_ini)) saveSettings(Settings());

    updateTray(true);
    SetTimer(g_tray, TIMER_STATUS, 1000, nullptr);

    updater::cleanupOld(g_dir);
    {
        Settings s = loadSettings();
        if (s.checkUpdates) g_nextCheck = GetTickCount64() + kFirstCheckDelayMs;
        // An update downloaded earlier but not installed yet (e.g. X-Plane was running).
        std::wstring staged = updater::stagedVersion(g_dir);
        if (!staged.empty()) {
            g_updateVersion = staged;
            g_updateStatus = L"Version " + staged + L" downloaded";
            if (s.autoUpdate) g_installWhenReady = true;
        }
    }
    if (justUpdated) {
        g_updateStatus = L"Updated to version " + std::wstring(kVersion);
        showBalloon(L"XrTriggerSnap updated", L"Now running version " + std::wstring(kVersion) + L".");
    }

    // First manual run: offer to install the layer.
    if (!g_fromXPlane && !fromStartup && !justUpdated) {
        LayerState ls = layerState();
        if (ls != LayerState::Installed) {
            std::wstring q = ls == LayerState::OtherFolder
                                 ? L"The layer is registered from a different folder.\n\nUse this folder instead?\n" + g_dir
                                 : std::wstring(L"The layer isn't installed yet. Install it now?\n\n"
                                                L"Windows will ask for admin permission once.");
            if (MessageBoxW(nullptr, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES)
                installLayer(nullptr);
        } else {
            showSettings();
        }
    }

    MSG msgLoop;
    while (GetMessageW(&msgLoop, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msgLoop)) continue;
        TranslateMessage(&msgLoop);
        DispatchMessageW(&msgLoop);
    }
    if (g_single) {
        ReleaseMutex(g_single);
        CloseHandle(g_single);
    }
    return 0;
}
