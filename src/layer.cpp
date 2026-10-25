// XrTriggerSnap - OpenXR API layer that removes the "half-pressed trigger" state.
//
// X-Plane shows its VR laser while the trigger is partly pressed and clicks on a
// full press. With hand tracking (e.g. Apple Vision Pro over CloudXR) the pinch
// gap is reported as the trigger value, so a loose near-pinch while reaching for a
// switch reads as "half pressed" and the laser appears.
//
// This layer intercepts xrGetActionStateFloat for actions the app bound to the
// trigger (and similar pinch/select inputs) and snaps the value to 0 or 1:
//   raw >= press_threshold            -> 1   (full click)
//   raw <  release_threshold          -> 0
//   in between                        -> keeps previous output (hysteresis)
// So the app never sees an in-between value, and the laser never appears.
//
// It also writes a log of the interaction profiles, actions and bindings the app
// registers, which tells you exactly how X-Plane wires the hand input.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <fstream>
#include <sys/stat.h>
#include <chrono>
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#include <dlfcn.h>
#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#define LAYER_NAME "XR_APILAYER_BENLIM_trigger_snap"
#include "../version.h"
#define LAYER_VERSION XRTS_VERSION

namespace {

// ---------------------------------------------------------------- config

struct Config {
    bool enabled = true;
    float pressThreshold = 0.90f;
    float releaseThreshold = 0.60f;
    std::string hands = "both";  // both | left | right
    // Binding path suffixes whose float actions get snapped.
    std::vector<std::string> floatSuffixes = {
        "/input/trigger/value",
        "/input/pinch_ext/value",
        "/input/aim_activate_ext/value",
        "/input/select/value",
    };
    // Boolean "touch" inputs forced to false (some apps show the laser on touch).
    bool suppressTouch = true;
    std::vector<std::string> touchSuffixes = {"/input/trigger/touch"};
    // Hand-skeleton pinch (XR_EXT_hand_tracking): distance between thumb tip and
    // index tip, in metres. A half-closed pinch is reported as fully open.
    bool jointSnap = true;
    float jointClose = 0.011f;    // closer than this = pinch (click)
    float jointRelease = 0.030f;  // once pinched, stays pinched until wider than this
    float jointOpen = 0.070f;     // anything in between is reported at this distance
    float jointPinched = 0.005f;  // distance reported while pinched
    // Quick, deliberate pinches pass through untouched (real finger positions, so X-Plane
    // places the pinch exactly where the fingers are). Only slow drift / resting near-pinches
    // are hidden. "Quick" = thumb-index gap closing faster than this (m/s).
    bool fastPinch = true;
    float fastPinchSpeed = 0.10f;
    int logLevel = 1;  // 0 off, 1 setup info, 2 also value changes
    bool launchTray = true;  // start XrTriggerSnap.exe (tray app) when X-Plane loads the layer
};

Config g_cfg;
std::string g_dir;  // folder containing the DLL
FILE* g_log = nullptr;
std::mutex g_mutex;

void logf(int level, const char* fmt, ...) {
    if (!g_log || level > g_cfg.logLevel) return;
    char ts[32];
    time_t t = time(nullptr);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    fprintf(g_log, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t c = s.find(',', start);
        std::string item = trim(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (!item.empty()) out.push_back(item);
        if (c == std::string::npos) break;
        start = c + 1;
    }
    return out;
}

bool parseBool(const std::string& v) {
    std::string l = v;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)tolower(c); });
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

void findDllDir() {
#ifdef _WIN32
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&findDllDir), &mod);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    g_dir = slash == std::string::npos ? "." : p.substr(0, slash);
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&findDllDir), &info) && info.dli_fname) {
        std::string p(info.dli_fname);
        size_t slash = p.find_last_of('/');
        g_dir = slash == std::string::npos ? "." : p.substr(0, slash);
    } else {
        g_dir = ".";
    }
#endif
}

long long g_iniMtime = -1;

long long iniMtime() {
    struct stat st;
    // mtime alone has 1 s resolution; mixing in the size catches quick successive saves.
    return stat((g_dir + "/trigger_snap.ini").c_str(), &st) == 0 ? (long long)st.st_mtime * 1000003LL + st.st_size : -1;
}

void loadConfig() {
    g_cfg = Config();  // start from defaults so a removed line reverts to its default
    g_iniMtime = iniMtime();
    std::ifstream in(g_dir + "/trigger_snap.ini");
    if (!in) return;  // defaults
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "enabled") g_cfg.enabled = parseBool(v);
        else if (k == "press_threshold") g_cfg.pressThreshold = (float)atof(v.c_str());
        else if (k == "release_threshold") g_cfg.releaseThreshold = (float)atof(v.c_str());
        else if (k == "hands") g_cfg.hands = v;
        else if (k == "float_paths") g_cfg.floatSuffixes = splitList(v);
        else if (k == "suppress_touch") g_cfg.suppressTouch = parseBool(v);
        else if (k == "touch_paths") g_cfg.touchSuffixes = splitList(v);
        else if (k == "log_level") g_cfg.logLevel = atoi(v.c_str());
        else if (k == "joint_snap") g_cfg.jointSnap = parseBool(v);
        else if (k == "joint_close_cm") g_cfg.jointClose = (float)atof(v.c_str()) / 100.f;
        else if (k == "joint_release_cm") g_cfg.jointRelease = (float)atof(v.c_str()) / 100.f;
        else if (k == "joint_open_cm") g_cfg.jointOpen = (float)atof(v.c_str()) / 100.f;
        else if (k == "joint_pinched_cm") g_cfg.jointPinched = (float)atof(v.c_str()) / 100.f;
        else if (k == "fast_pinch") g_cfg.fastPinch = parseBool(v);
        else if (k == "fast_pinch_speed_cm_s") g_cfg.fastPinchSpeed = (float)atof(v.c_str()) / 100.f;
        else if (k == "launch_tray") g_cfg.launchTray = parseBool(v);
    }
    if (g_cfg.releaseThreshold > g_cfg.pressThreshold) g_cfg.releaseThreshold = g_cfg.pressThreshold;
    if (g_cfg.jointRelease < g_cfg.jointClose) g_cfg.jointRelease = g_cfg.jointClose;
    if (g_cfg.jointOpen < g_cfg.jointRelease) g_cfg.jointOpen = g_cfg.jointRelease;
}

void openLog() {
    if (g_log || g_cfg.logLevel <= 0) return;
    // Append, so a second OpenXR instance in the same X-Plane run doesn't wipe the first.
    // Start fresh once the file gets large.
    auto open = [](const std::string& path) -> FILE* {
        FILE* f = fopen(path.c_str(), "a");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) > 2 * 1024 * 1024) {
                fclose(f);
                f = fopen(path.c_str(), "w");
            }
        }
        return f;
    };
    g_log = open(g_dir + "/trigger_snap.log");
#ifdef _WIN32
    if (!g_log) {  // DLL folder not writable (e.g. under Program Files): fall back to %TEMP%
        char tmp[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, tmp)) g_log = open(std::string(tmp) + "trigger_snap.log");
    }
#endif
    if (g_log) fprintf(g_log, "\n======================================================\n");
}

void logConfig(const char* what) {
    logf(1, "%s: enabled=%d hands=%s trigger press=%.2f release=%.2f | pinch close=%.1fcm release=%.1fcm open=%.1fcm"
            " | quick pinch pass-through=%d above %.0fcm/s",
         what, g_cfg.enabled, g_cfg.hands.c_str(), g_cfg.pressThreshold, g_cfg.releaseThreshold,
         g_cfg.jointClose * 100.f, g_cfg.jointRelease * 100.f, g_cfg.jointOpen * 100.f, g_cfg.fastPinch,
         g_cfg.fastPinchSpeed * 100.f);
}

#ifdef _WIN32
// Named mutex the tray app checks to show "active in X-Plane".
HANDLE g_activeMutex = nullptr;

void launchTrayApp() {
    if (!g_cfg.launchTray) return;
    if (FindWindowW(L"XrTriggerSnapTray", nullptr)) return;  // already running
    std::string exe = g_dir + "\\XrTriggerSnap.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::string cmd = "\"" + exe + "\" /xplane";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB,
                       nullptr, g_dir.c_str(), &si, &pi) ||
        CreateProcessA(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, g_dir.c_str(), &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logf(1, "started tray app");
    }
}
#endif

// ---------------------------------------------------------------- state

PFN_xrGetInstanceProcAddr next_xrGetInstanceProcAddr = nullptr;
PFN_xrDestroyInstance next_xrDestroyInstance = nullptr;
PFN_xrCreateAction next_xrCreateAction = nullptr;
PFN_xrDestroyAction next_xrDestroyAction = nullptr;
PFN_xrSuggestInteractionProfileBindings next_xrSuggestInteractionProfileBindings = nullptr;
PFN_xrGetActionStateFloat next_xrGetActionStateFloat = nullptr;
PFN_xrGetActionStateBoolean next_xrGetActionStateBoolean = nullptr;
PFN_xrSyncActions next_xrSyncActions = nullptr;
PFN_xrPollEvent next_xrPollEvent = nullptr;
PFN_xrPathToString next_xrPathToString = nullptr;
PFN_xrStringToPath next_xrStringToPath = nullptr;
PFN_xrGetCurrentInteractionProfile next_xrGetCurrentInteractionProfile = nullptr;
PFN_xrCreateHandTrackerEXT next_xrCreateHandTrackerEXT = nullptr;
PFN_xrDestroyHandTrackerEXT next_xrDestroyHandTrackerEXT = nullptr;
PFN_xrLocateHandJointsEXT next_xrLocateHandJointsEXT = nullptr;

struct HandState {
    // Hidden:      fingers apart or drifting; any half pinch is reported as an open hand
    // PinchHidden: slow pinch reached; reported firmly pinched until the fingers open again
    // Pass:        fingers closing quickly (a deliberate pinch); real positions passed through
    // PassPinched: that quick pinch landed; held as a firm pinch until the fingers open
    enum Mode { Hidden, PinchHidden, Pass, PassPinched };
    XrHandEXT hand = XR_HAND_LEFT_EXT;
    bool firstLocateLogged = false;
    Mode mode = Hidden;
    bool aimPinched = false;   // FB aim pinchStrength latch
    int64_t lastT = 0;         // time of the last motion sample (ns)
    float ds = -1.f;           // smoothed thumb-index gap (m)
    float v = 0.f;             // smoothed rate of change of the gap (m/s, negative = closing)
    float reported = -1.f;     // gap reported to the app last frame (m)
    int64_t stallSince = 0;    // when a quick close stopped moving (ns)
    int64_t passSince = 0;     // when the current quick close started (ns)
    struct Sample { int64_t t; float ds; };
    Sample hist[48] = {};      // recent smoothed gaps, for "how far has it closed lately"
    int histN = 0, histHead = 0;
    void push(int64_t t, float v) {
        hist[histHead] = {t, v};
        histHead = (histHead + 1) % 48;
        if (histN < 48) ++histN;
    }
    float maxSince(int64_t t0) const {
        float m = -1.f;
        for (int i = 0; i < histN; ++i)
            if (hist[i].t >= t0 && hist[i].ds > m) m = hist[i].ds;
        return m;
    }
    float minSince(int64_t t0) const {
        float m = 1e9f;
        for (int i = 0; i < histN; ++i)
            if (hist[i].t >= t0 && hist[i].ds < m) m = hist[i].ds;
        return m;
    }
    float minDist = 1e9f, maxAim = 0.f;  // activity since last summary
    int quickPinches = 0;                // since last summary
    void reset() { mode = Hidden; aimPinched = false; reported = -1.f; stallSince = 0; passSince = 0; histN = 0; }
};

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::map<XrHandTrackerEXT, HandState> g_hands;

// Periodic diagnostics, driven from xrSyncActions.
uint64_t g_syncCount = 0;
XrPath g_lastProfile[2] = {XR_NULL_PATH, XR_NULL_PATH};
std::map<XrAction, float> g_maxRaw;  // max raw trigger value since last summary
time_t g_lastSummary = 0;

XrInstance g_instance = XR_NULL_HANDLE;
XrPath g_leftHand = XR_NULL_PATH, g_rightHand = XR_NULL_PATH;

struct ActionInfo {
    std::string name;
    XrActionType type;
};
std::map<XrAction, ActionInfo> g_actions;
std::set<XrAction> g_snapActions;   // float actions to snap
std::set<XrAction> g_touchActions;  // boolean actions to force false

struct SnapState {
    float out = 0.f;           // last value we reported
    float outAtLastSync = 0.f; // value reported as of the previous xrSyncActions
    uint64_t seq = 0;
    float lastLoggedRaw = -1.f;
};
std::map<std::pair<XrAction, XrPath>, SnapState> g_state;
uint64_t g_syncSeq = 0;

std::string pathStr(XrPath p) {
    if (p == XR_NULL_PATH) return "(null)";
    if (!next_xrPathToString) return "?";
    uint32_t n = 0;
    if (XR_FAILED(next_xrPathToString(g_instance, p, 0, &n, nullptr)) || n == 0) return "?";
    std::string s(n, '\0');
    if (XR_FAILED(next_xrPathToString(g_instance, p, n, &n, &s[0]))) return "?";
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool matchesAny(const std::string& s, const std::vector<std::string>& suffixes) {
    for (auto& suf : suffixes)
        if (endsWith(s, suf)) return true;
    return false;
}

bool handApplies(XrPath sub) {
    if (g_cfg.hands == "both" || sub == XR_NULL_PATH) return true;
    if (g_cfg.hands == "left") return sub == g_leftHand;
    if (g_cfg.hands == "right") return sub == g_rightHand;
    return true;
}

const char* typeName(XrActionType t) {
    switch (t) {
        case XR_ACTION_TYPE_BOOLEAN_INPUT: return "bool";
        case XR_ACTION_TYPE_FLOAT_INPUT: return "float";
        case XR_ACTION_TYPE_VECTOR2F_INPUT: return "vec2";
        case XR_ACTION_TYPE_POSE_INPUT: return "pose";
        case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "haptic";
        default: return "other";
    }
}

// ---------------------------------------------------------------- hooks

XrResult XRAPI_CALL hook_xrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    XrResult r = next_xrCreateAction(set, info, action);
    if (XR_SUCCEEDED(r) && info) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_actions[*action] = {info->actionName, info->actionType};
        logf(1, "action created: %-32s type=%s", info->actionName, typeName(info->actionType));
    }
    return r;
}

XrResult XRAPI_CALL hook_xrDestroyAction(XrAction action) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_actions.erase(action);
        g_snapActions.erase(action);
        g_touchActions.erase(action);
        for (auto it = g_state.begin(); it != g_state.end();)
            it = it->first.first == action ? g_state.erase(it) : std::next(it);
    }
    return next_xrDestroyAction(action);
}

XrResult XRAPI_CALL hook_xrSuggestInteractionProfileBindings(XrInstance inst,
                                                             const XrInteractionProfileSuggestedBinding* sb) {
    XrResult r = next_xrSuggestInteractionProfileBindings(inst, sb);
    if (!sb) return r;
    std::lock_guard<std::mutex> lk(g_mutex);
    logf(1, "bindings for profile %s -> %s", pathStr(sb->interactionProfile).c_str(),
         XR_SUCCEEDED(r) ? "accepted" : "REJECTED by runtime");
    for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = sb->suggestedBindings[i];
        std::string p = pathStr(b.binding);
        auto it = g_actions.find(b.action);
        std::string name = it != g_actions.end() ? it->second.name : "?";
        XrActionType type = it != g_actions.end() ? it->second.type : XR_ACTION_TYPE_MAX_ENUM;
        const char* tag = "";
        if (type == XR_ACTION_TYPE_FLOAT_INPUT && matchesAny(p, g_cfg.floatSuffixes)) {
            g_snapActions.insert(b.action);
            tag = "  <-- SNAPPED";
        } else if (type == XR_ACTION_TYPE_BOOLEAN_INPUT && g_cfg.suppressTouch && matchesAny(p, g_cfg.touchSuffixes)) {
            g_touchActions.insert(b.action);
            tag = "  <-- FORCED OFF";
        }
        logf(1, "    %-48s -> %s (%s)%s", p.c_str(), name.c_str(), typeName(type), tag);
    }
    return r;
}

XrResult XRAPI_CALL hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    XrResult r = next_xrSyncActions(session, info);
    if (!XR_SUCCEEDED(r)) return r;
    std::lock_guard<std::mutex> lk(g_mutex);
    ++g_syncSeq;

    // About once a second, pick up settings changed in the ini (e.g. by the tray app).
    static time_t lastIniCheck = 0;
    time_t nowCheck = time(nullptr);
    if (nowCheck != lastIniCheck) {
        lastIniCheck = nowCheck;
        if (iniMtime() != g_iniMtime) {
            int oldLog = g_cfg.logLevel;
            loadConfig();
            if (g_cfg.logLevel > 0 && !g_log) openLog();
            if (oldLog > 0 || g_cfg.logLevel > 0) {
                int keep = g_cfg.logLevel;
                g_cfg.logLevel = std::max(1, keep);
                logConfig("settings reloaded");
                g_cfg.logLevel = keep;
            }
            // Drop any latched state so a change takes effect cleanly.
            for (auto& kv : g_hands) kv.second.reset();
        }
    }

    // Every ~100 syncs (about a second), log the active controller profile if it changed.
    if (next_xrGetCurrentInteractionProfile && (g_syncCount++ % 100) == 0) {
        XrPath hands[2] = {g_leftHand, g_rightHand};
        for (int i = 0; i < 2; ++i) {
            XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE, nullptr, XR_NULL_PATH};
            if (XR_SUCCEEDED(next_xrGetCurrentInteractionProfile(session, hands[i], &ps)) &&
                ps.interactionProfile != g_lastProfile[i]) {
                g_lastProfile[i] = ps.interactionProfile;
                logf(1, "active profile %s: %s", pathStr(hands[i]).c_str(),
                     ps.interactionProfile == XR_NULL_PATH ? "(none)" : pathStr(ps.interactionProfile).c_str());
            }
        }
    }

    // Every 5 s, summarise input activity so we can see what the hands actually drive.
    time_t now = time(nullptr);
    if (now - g_lastSummary >= 5) {
        g_lastSummary = now;
        std::string line;
        char buf[160];
        for (auto& kv : g_maxRaw) {
            if (kv.second < 0.02f) continue;
            snprintf(buf, sizeof(buf), " %s=%.2f", g_actions[kv.first].name.c_str(), kv.second);
            line += buf;
            kv.second = 0.f;
        }
        for (auto& kv : g_hands) {
            HandState& h = kv.second;
            if (h.minDist < 1e8f) {
                snprintf(buf, sizeof(buf), " %s-hand(min thumb-index %.1fcm, quick pinches %d, max aim pinch %.2f)",
                         h.hand == XR_HAND_LEFT_EXT ? "left" : "right", h.minDist * 100.f, h.quickPinches, h.maxAim);
                line += buf;
            }
            h.minDist = 1e9f;
            h.maxAim = 0.f;
            h.quickPinches = 0;
        }
        if (!line.empty()) logf(1, "activity (last 5s):%s", line.c_str());
    }
    return r;
}

XrResult XRAPI_CALL hook_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                               XrActionStateFloat* state) {
    XrResult r = next_xrGetActionStateFloat(session, getInfo, state);
    if (r != XR_SUCCESS || !getInfo || !state || !state->isActive) return r;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_cfg.enabled) return r;
    if (!g_snapActions.count(getInfo->action) || !handApplies(getInfo->subactionPath)) return r;

    SnapState& s = g_state[{getInfo->action, getInfo->subactionPath}];
    if (s.seq != g_syncSeq) {  // first read since the last sync
        s.outAtLastSync = s.out;
        s.seq = g_syncSeq;
    }
    const float raw = state->currentState;
    float& mx = g_maxRaw[getInfo->action];
    mx = std::max(mx, raw);
    float out = s.out;
    if (raw >= g_cfg.pressThreshold) out = 1.f;
    else if (raw < g_cfg.releaseThreshold) out = 0.f;

    if (g_cfg.logLevel >= 2 && (s.lastLoggedRaw < 0 || std::abs(raw - s.lastLoggedRaw) >= 0.1f || out != s.out)) {
        logf(2, "%s %s raw=%.2f -> %.0f", g_actions[getInfo->action].name.c_str(),
             pathStr(getInfo->subactionPath).c_str(), raw, out);
        s.lastLoggedRaw = raw;
    }

    s.out = out;
    state->currentState = out;
    state->changedSinceLastSync = (out != s.outAtLastSync) ? XR_TRUE : XR_FALSE;
    return r;
}

XrResult XRAPI_CALL hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                 XrActionStateBoolean* state) {
    XrResult r = next_xrGetActionStateBoolean(session, getInfo, state);
    if (r != XR_SUCCESS || !getInfo || !state) return r;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_cfg.enabled && g_touchActions.count(getInfo->action) && handApplies(getInfo->subactionPath)) {
        state->currentState = XR_FALSE;
        state->changedSinceLastSync = XR_FALSE;
    }
    return r;
}

// ---- hand skeleton (XR_EXT_hand_tracking, optionally with XR_FB_hand_tracking_aim)

const char* handName(XrHandEXT h) { return h == XR_HAND_LEFT_EXT ? "left" : "right"; }

XrResult XRAPI_CALL hook_xrCreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT* ci,
                                                XrHandTrackerEXT* tracker) {
    XrResult r = next_xrCreateHandTrackerEXT(session, ci, tracker);
    if (XR_SUCCEEDED(r) && ci) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_hands[*tracker].hand = ci->hand;
        logf(1, "hand tracker created: %s hand, joint set %d", handName(ci->hand), (int)ci->handJointSet);
    }
    return r;
}

XrResult XRAPI_CALL hook_xrDestroyHandTrackerEXT(XrHandTrackerEXT tracker) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_hands.erase(tracker);
    }
    return next_xrDestroyHandTrackerEXT(tracker);
}

XrResult XRAPI_CALL hook_xrLocateHandJointsEXT(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT* info,
                                               XrHandJointLocationsEXT* loc) {
    XrResult r = next_xrLocateHandJointsEXT(tracker, info, loc);
    if (r != XR_SUCCESS || !loc || !loc->isActive) return r;

    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_hands.find(tracker);
    if (it == g_hands.end()) return r;
    HandState& h = it->second;
    const bool applies =
        g_cfg.enabled && handApplies(h.hand == XR_HAND_LEFT_EXT ? g_leftHand : g_rightHand);

    if (!h.firstLocateLogged) {
        h.firstLocateLogged = true;
        std::string chain;
        for (auto* p = reinterpret_cast<XrBaseOutStructure*>(loc->next); p; p = p->next) {
            char buf[64];
            snprintf(buf, sizeof(buf), " %s",
                     p->type == XR_TYPE_HAND_TRACKING_AIM_STATE_FB ? "FB_aim_state"
                     : p->type == XR_TYPE_HAND_JOINT_VELOCITIES_EXT ? "joint_velocities"
                                                                     : std::to_string((int)p->type).c_str());
            chain += buf;
        }
        logf(1, "%s hand joints being read: %u joints, extra data:%s", handName(h.hand), loc->jointCount,
             chain.empty() ? " none" : chain.c_str());
    }

    // FB aim state: a ready-made 0-1 pinch strength. Snap it like the trigger.
    for (auto* p = reinterpret_cast<XrBaseOutStructure*>(loc->next); p; p = p->next) {
        if (p->type != XR_TYPE_HAND_TRACKING_AIM_STATE_FB) continue;
        auto* aim = reinterpret_cast<XrHandTrackingAimStateFB*>(p);
        h.maxAim = std::max(h.maxAim, aim->pinchStrengthIndex);
        if (!applies) continue;
        bool was = h.aimPinched;
        if (aim->pinchStrengthIndex >= g_cfg.pressThreshold) h.aimPinched = true;
        else if (aim->pinchStrengthIndex < g_cfg.releaseThreshold) h.aimPinched = false;
        if (was != h.aimPinched)
            logf(1, "%s aim pinch %s (strength %.2f)", handName(h.hand), h.aimPinched ? "ON" : "off",
                 aim->pinchStrengthIndex);
        aim->pinchStrengthIndex = h.aimPinched ? 1.f : 0.f;
        if (h.aimPinched) aim->status |= XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB;
        else aim->status &= ~XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB;
    }

    // Joint distance: thumb tip to index tip.
    if (loc->jointCount <= XR_HAND_JOINT_INDEX_TIP_EXT || !loc->jointLocations) return r;
    XrHandJointLocationEXT& thumbTip = loc->jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT];
    XrHandJointLocationEXT& thumbDistal = loc->jointLocations[XR_HAND_JOINT_THUMB_DISTAL_EXT];
    const XrHandJointLocationEXT& indexTip = loc->jointLocations[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(thumbTip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(indexTip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return r;

    const XrVector3f& a = thumbTip.pose.position;
    const XrVector3f& b = indexTip.pose.position;
    XrVector3f dv{a.x - b.x, a.y - b.y, a.z - b.z};
    const float d = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
    h.minDist = std::min(h.minDist, d);
    if (!applies || !g_cfg.jointSnap || d < 1e-5f) return r;

    // ---- how fast is the gap changing? (smoothed; tracking jitter is a few mm per frame)
    // Use the app's frame time; fall back to a local clock if the app passes none.
    int64_t now = (info && info->time > 0) ? (int64_t)info->time : steadyNs();
    float frameDt = 0.f;
    if (h.ds < 0.f || h.lastT == 0) {
        h.ds = d;
        h.v = 0.f;
        h.lastT = now;
        h.histN = 0;
        h.push(now, d);
    } else if (now > h.lastT) {
        frameDt = (float)((now - h.lastT) * 1e-9);
        if (frameDt > 0.2f) {  // hand was lost for a while: start afresh
            h.ds = d;
            h.v = 0.f;
            h.histN = 0;
            frameDt = 0.f;
        } else {
            float a = 1.f - std::exp(-frameDt / 0.020f);
            float nds = h.ds + (d - h.ds) * a;
            float vi = (nds - h.ds) / frameDt;
            h.v += (vi - h.v) * (1.f - std::exp(-frameDt / 0.020f));
            h.ds = nds;
        }
        h.lastT = now;
        h.push(now, h.ds);
    }  // same timestamp queried again: no new motion sample

    const float close = g_cfg.jointClose, release = g_cfg.jointRelease, open = g_cfg.jointOpen;
    const float speed = g_cfg.fastPinchSpeed;
    const char* hn = handName(h.hand);
    HandState::Mode before = h.mode;
    switch (h.mode) {
        case HandState::Hidden:
            if (d < close) {
                h.mode = HandState::PinchHidden;
            } else if (g_cfg.fastPinch && d < open && h.v < -speed &&
                       h.maxSince(now - 250000000LL) - h.ds >= 0.010f) {
                // Closing fast AND by a real distance (>= 1 cm in the last 0.25 s), so
                // tracking jitter on a resting hand can't trigger it.
                h.mode = HandState::Pass;
                h.stallSince = 0;
                h.passSince = now;
            }
            break;
        case HandState::PinchHidden:
            if (d > release) h.mode = HandState::Hidden;
            break;
        case HandState::Pass: {
            // Touching fingertips track anywhere from ~0.8 to ~1.5 cm apart and bounce around
            // while they touch, so "the fingers got within 0.4 cm of the pinch mark" counts as
            // the pinch landing even if the gap never quite crosses the mark.
            const bool gotNear = h.minSince(h.passSince) < close + 0.004f;
            bool settle = false;
            if (d < close) {
                h.mode = HandState::PassPinched;
            } else if (d >= open) {
                h.mode = HandState::Hidden;
            } else if (h.v > -speed * 0.3f) {
                // Closing has stopped (or the gap is bouncing back up).
                if (!h.stallSince) h.stallSince = now;
                else if (now - h.stallSince > 80000000LL) settle = true;
            } else {
                h.stallSince = 0;
            }
            if (h.mode == HandState::Pass && now - h.passSince > 400000000LL) settle = true;
            if (settle) h.mode = gotNear ? HandState::PassPinched : HandState::Hidden;
            if (h.mode == HandState::PassPinched) ++h.quickPinches;
            break;
        }
        case HandState::PassPinched:
            // Held as a firm pinch, exactly like a slow pinch, until the fingers open past
            // the release distance. (Passing raw positions here let a near-pinch resting just
            // above the pinch mark show up as a half pinch, i.e. a lingering laser.)
            if (d > release) h.mode = HandState::Hidden;
            break;
    }
    if (h.mode != before) {
        switch (h.mode) {
            case HandState::PinchHidden: logf(1, "%s pinch ON at %.1fcm", hn, d * 100.f); break;
            case HandState::Pass: logf(1, "%s quick pinch starting at %.1fcm (%.0fcm/s): passing through", hn, d * 100.f, -h.v * 100.f); break;
            case HandState::PassPinched: logf(1, "%s quick pinch ON at %.1fcm", hn, d * 100.f); break;
            case HandState::Hidden:
                if (before == HandState::Pass) logf(1, "%s quick close ended at %.1fcm without a pinch: hiding again", hn, d * 100.f);
                else logf(1, "%s pinch off at %.1fcm", hn, d * 100.f);
                break;
        }
    }

    float target = d;
    switch (h.mode) {
        case HandState::Hidden: target = d < open ? open : d; break;
        case HandState::PinchHidden: target = std::min(d, g_cfg.jointPinched); break;
        case HandState::Pass: {
            // Ease from what the app saw last frame to the real gap over a few frames, so the
            // thumb doesn't teleport in from the "open hand" distance.
            float prev = h.reported > 0.f ? h.reported : d;
            float step = frameDt > 0.f ? 1.5f * std::min(frameDt, 0.05f) : 0.f;  // no new frame: hold
            target = prev + std::max(-step, std::min(step, d - prev));
            break;
        }
        case HandState::PassPinched: target = std::min(d, g_cfg.jointPinched); break;
    }
    h.reported = target;
    if (target == d) return r;

    // Move the thumb tip to `target` from the index tip. When the tips are close, the
    // tip-to-tip direction is mostly tracking noise, so aim along index tip -> thumb
    // distal joint instead: that joint is a few cm back down the thumb, so the direction
    // is stable and the thumb opens the way a real pinch opens.
    XrVector3f dir{dv.x / d, dv.y / d, dv.z / d};
    if (thumbDistal.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        const XrVector3f& c = thumbDistal.pose.position;
        XrVector3f dd{c.x - b.x, c.y - b.y, c.z - b.z};
        const float n = std::sqrt(dd.x * dd.x + dd.y * dd.y + dd.z * dd.z);
        if (n > 0.01f) dir = {dd.x / n, dd.y / n, dd.z / n};
    }
    XrVector3f off{b.x + dir.x * target - a.x, b.y + dir.y * target - a.y, b.z + dir.z * target - a.z};
    auto shift = [&](XrHandJointLocationEXT& j) {
        if (!(j.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
        j.pose.position.x += off.x;
        j.pose.position.y += off.y;
        j.pose.position.z += off.z;
    };
    shift(thumbTip);
    shift(thumbDistal);
    return r;
}

XrResult XRAPI_CALL hook_xrPollEvent(XrInstance inst, XrEventDataBuffer* ev) {
    XrResult r = next_xrPollEvent(inst, ev);
    if (r == XR_SUCCESS && ev && ev->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED &&
        next_xrGetCurrentInteractionProfile) {
        auto* e = reinterpret_cast<XrEventDataInteractionProfileChanged*>(ev);
        std::lock_guard<std::mutex> lk(g_mutex);
        for (XrPath hand : {g_leftHand, g_rightHand}) {
            XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE, nullptr, XR_NULL_PATH};
            if (XR_SUCCEEDED(next_xrGetCurrentInteractionProfile(e->session, hand, &ps)))
                logf(1, "active profile %s: %s", pathStr(hand).c_str(), pathStr(ps.interactionProfile).c_str());
        }
    }
    return r;
}

XrResult XRAPI_CALL hook_xrDestroyInstance(XrInstance inst) {
    XrResult r = next_xrDestroyInstance(inst);
    std::lock_guard<std::mutex> lk(g_mutex);
    logf(1, "instance destroyed");
    g_actions.clear();
    g_snapActions.clear();
    g_touchActions.clear();
    g_state.clear();
    g_hands.clear();
    g_maxRaw.clear();
    g_lastProfile[0] = g_lastProfile[1] = XR_NULL_PATH;
    g_instance = XR_NULL_HANDLE;
#ifdef _WIN32
    if (g_activeMutex) {
        CloseHandle(g_activeMutex);
        g_activeMutex = nullptr;
    }
#endif
    return r;
}

XrResult XRAPI_CALL layer_xrGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    if (!next_xrGetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    XrResult r = next_xrGetInstanceProcAddr(inst, name, fn);
    if (XR_FAILED(r) || !name) return r;

#define HOOK(f)                                                   \
    if (strcmp(name, #f) == 0) {                                  \
        next_##f = reinterpret_cast<PFN_##f>(*fn);                \
        *fn = reinterpret_cast<PFN_xrVoidFunction>(&hook_##f);    \
        return r;                                                 \
    }
    HOOK(xrDestroyInstance)
    HOOK(xrCreateAction)
    HOOK(xrDestroyAction)
    HOOK(xrSuggestInteractionProfileBindings)
    HOOK(xrSyncActions)
    HOOK(xrGetActionStateFloat)
    HOOK(xrGetActionStateBoolean)
    HOOK(xrPollEvent)
    HOOK(xrCreateHandTrackerEXT)
    HOOK(xrDestroyHandTrackerEXT)
    HOOK(xrLocateHandJointsEXT)
#undef HOOK
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) *fn = reinterpret_cast<PFN_xrVoidFunction>(&layer_xrGetInstanceProcAddr);
    return r;
}

template <typename T>
void resolve(XrInstance inst, const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_SUCCEEDED(next_xrGetInstanceProcAddr(inst, name, &f))) out = reinterpret_cast<T>(f);
}

XrResult XRAPI_CALL layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                                   XrInstance* instance) {
    if (!layerInfo || !layerInfo->nextInfo || strcmp(layerInfo->nextInfo->layerName, LAYER_NAME) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;

    // Hand the chain on to the next layer / runtime.
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = layerInfo->nextInfo->next;
    next_xrGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    if (XR_FAILED(r)) {
        logf(1, "next xrCreateApiLayerInstance failed: %d", (int)r);
        return r;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    g_instance = *instance;
    resolve(g_instance, "xrDestroyInstance", next_xrDestroyInstance);
    resolve(g_instance, "xrCreateAction", next_xrCreateAction);
    resolve(g_instance, "xrDestroyAction", next_xrDestroyAction);
    resolve(g_instance, "xrSuggestInteractionProfileBindings", next_xrSuggestInteractionProfileBindings);
    resolve(g_instance, "xrSyncActions", next_xrSyncActions);
    resolve(g_instance, "xrGetActionStateFloat", next_xrGetActionStateFloat);
    resolve(g_instance, "xrGetActionStateBoolean", next_xrGetActionStateBoolean);
    resolve(g_instance, "xrPollEvent", next_xrPollEvent);
    resolve(g_instance, "xrPathToString", next_xrPathToString);
    resolve(g_instance, "xrStringToPath", next_xrStringToPath);
    resolve(g_instance, "xrGetCurrentInteractionProfile", next_xrGetCurrentInteractionProfile);
    if (next_xrStringToPath) {
        next_xrStringToPath(g_instance, "/user/hand/left", &g_leftHand);
        next_xrStringToPath(g_instance, "/user/hand/right", &g_rightHand);
    }

    std::string runtimeName = "?";
    PFN_xrGetInstanceProperties getProps = nullptr;
    resolve(g_instance, "xrGetInstanceProperties", getProps);
    if (getProps) {
        XrInstanceProperties props{};
        props.type = XR_TYPE_INSTANCE_PROPERTIES;
        if (XR_SUCCEEDED(getProps(g_instance, &props))) runtimeName = props.runtimeName;
    }
    logf(1, "instance created: app=\"%s\" runtime=\"%s\"", info ? info->applicationInfo.applicationName : "?",
         runtimeName.c_str());
    if (info) {
        std::string ext;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) ext += std::string(" ") + info->enabledExtensionNames[i];
        logf(1, "extensions enabled by app:%s", ext.empty() ? " none" : ext.c_str());
    }
#ifdef _WIN32
    if (!g_activeMutex) g_activeMutex = CreateMutexA(nullptr, FALSE, "Local\\XrTriggerSnapActive");
    launchTrayApp();
#endif
    return r;
}

}  // namespace

LAYER_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                    const char* layerName,
                                                                    XrNegotiateApiLayerRequest* req) {
    if (!loaderInfo || !req || !layerName || strcmp(layerName, LAYER_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        req->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || req->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;

    findDllDir();
    loadConfig();
    openLog();
    logf(1, "%s %s loaded from %s", LAYER_NAME, LAYER_VERSION, g_dir.c_str());
    logConfig("config");

    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_API_VERSION_1_0;  // widest compatibility; we hook only 1.0 functions
    req->getInstanceProcAddr = &layer_xrGetInstanceProcAddr;
    req->createApiLayerInstance = &layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
