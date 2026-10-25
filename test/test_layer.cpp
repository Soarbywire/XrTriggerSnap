// Host-side test: acts as the OpenXR loader plus a fake runtime, loads the layer
// and checks that trigger values are snapped with hysteresis.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#define dlopen(p, f) ((void*)LoadLibraryA(p))
#define dlsym(h, n) ((void*)GetProcAddress((HMODULE)(h), n))
#define dlerror() "LoadLibrary failed"
#define sleep(s) Sleep((s) * 1000)
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <fstream>
#include <chrono>
#include <thread>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- fake runtime
static std::vector<std::string> paths = {""};
static float rawTrigger = 0.f;
static XrBool32 rawTouch = XR_TRUE;
static XrAction trigAction = (XrAction)0x10, touchAction = (XrAction)0x20, gripAction = (XrAction)0x30;
static int actionCounter = 0;

static XrResult XRAPI_CALL rt_StringToPath(XrInstance, const char* s, XrPath* p) {
    for (size_t i = 1; i < paths.size(); ++i) if (paths[i] == s) { *p = i; return XR_SUCCESS; }
    paths.push_back(s); *p = paths.size() - 1; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_PathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* n, char* buf) {
    const std::string& s = paths.at(p);
    *n = (uint32_t)s.size() + 1;
    if (cap == 0) return XR_SUCCESS;
    if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, s.c_str(), *n); return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_CreateAction(XrActionSet, const XrActionCreateInfo*, XrAction* a) {
    XrAction list[] = {trigAction, touchAction, gripAction}; *a = list[actionCounter++]; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_DestroyAction(XrAction) { return XR_SUCCESS; }
static XrResult XRAPI_CALL rt_Suggest(XrInstance, const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL rt_Sync(XrSession, const XrActionsSyncInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL rt_Float(XrSession, const XrActionStateGetInfo*, XrActionStateFloat* s) {
    s->currentState = rawTrigger; s->isActive = XR_TRUE; s->changedSinceLastSync = XR_TRUE; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_Bool(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* s) {
    s->currentState = rawTouch; s->isActive = XR_TRUE; s->changedSinceLastSync = XR_FALSE; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_Poll(XrInstance, XrEventDataBuffer*) { return XR_EVENT_UNAVAILABLE; }
static XrResult XRAPI_CALL rt_DestroyInstance(XrInstance) { return XR_SUCCESS; }
static XrResult XRAPI_CALL rt_Props(XrInstance, XrInstanceProperties* p) { strcpy(p->runtimeName, "FakeRuntime"); return XR_SUCCESS; }
// fake hand tracking: thumb tip at (dist,0,0), index tip at origin
static float handDist = 0.1f, aimStrength = 0.f;
static bool tipSideways = false;  // tip off to the side (noisy direction), distal along +x
static XrResult XRAPI_CALL rt_CreateHT(XrSession, const XrHandTrackerCreateInfoEXT*, XrHandTrackerEXT* t) {
    *t = (XrHandTrackerEXT)0x77; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_DestroyHT(XrHandTrackerEXT) { return XR_SUCCESS; }
static XrResult XRAPI_CALL rt_Locate(XrHandTrackerEXT, const XrHandJointsLocateInfoEXT*, XrHandJointLocationsEXT* l) {
    l->isActive = XR_TRUE;
    for (uint32_t i = 0; i < l->jointCount; ++i) {
        l->jointLocations[i].locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
        l->jointLocations[i].pose.position = {0, 0, 0};
    }
    if (tipSideways) {
        l->jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position = {0, handDist, 0};
        l->jointLocations[XR_HAND_JOINT_THUMB_DISTAL_EXT].pose.position = {0.04f, 0, 0};
    } else {
        l->jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position = {handDist, 0, 0};
        l->jointLocations[XR_HAND_JOINT_THUMB_DISTAL_EXT].pose.position = {handDist + 0.03f, 0, 0};
    }
    if (l->next) ((XrHandTrackingAimStateFB*)l->next)->pinchStrengthIndex = aimStrength;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_GIPA(XrInstance, const char* n, PFN_xrVoidFunction* f) {
    static std::map<std::string, PFN_xrVoidFunction> m = {
        {"xrStringToPath", (PFN_xrVoidFunction)rt_StringToPath}, {"xrPathToString", (PFN_xrVoidFunction)rt_PathToString},
        {"xrCreateAction", (PFN_xrVoidFunction)rt_CreateAction}, {"xrDestroyAction", (PFN_xrVoidFunction)rt_DestroyAction},
        {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)rt_Suggest}, {"xrSyncActions", (PFN_xrVoidFunction)rt_Sync},
        {"xrGetActionStateFloat", (PFN_xrVoidFunction)rt_Float}, {"xrGetActionStateBoolean", (PFN_xrVoidFunction)rt_Bool},
        {"xrPollEvent", (PFN_xrVoidFunction)rt_Poll}, {"xrDestroyInstance", (PFN_xrVoidFunction)rt_DestroyInstance},
        {"xrGetInstanceProperties", (PFN_xrVoidFunction)rt_Props},
        {"xrCreateHandTrackerEXT", (PFN_xrVoidFunction)rt_CreateHT}, {"xrDestroyHandTrackerEXT", (PFN_xrVoidFunction)rt_DestroyHT},
        {"xrLocateHandJointsEXT", (PFN_xrVoidFunction)rt_Locate},
    };
    auto it = m.find(n); if (it == m.end()) { *f = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED; }
    *f = it->second; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_CreateInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance* i) {
    *i = (XrInstance)0x1234; return XR_SUCCESS;
}

int main(int argc, char** argv) {
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)dlsym(lib, "xrNegotiateLoaderApiLayerInterface");
    CHECK(negotiate, "export missing");

    XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li), 1, 1,
                             XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION};
    XrNegotiateApiLayerRequest req{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(req)};
    CHECK(negotiate(&li, "XR_APILAYER_BENLIM_trigger_snap", &req) == XR_SUCCESS, "negotiate");
    CHECK(negotiate(&li, "wrong_name", &req) != XR_SUCCESS, "negotiate should reject wrong name");

    XrApiLayerNextInfo ni{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(ni)};
    strcpy(ni.layerName, "XR_APILAYER_BENLIM_trigger_snap");
    ni.nextGetInstanceProcAddr = rt_GIPA; ni.nextCreateApiLayerInstance = rt_CreateInstance;
    XrApiLayerCreateInfo ci{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(ci)};
    ci.nextInfo = &ni;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "X-Plane");
    XrInstance inst;
    CHECK(req.createApiLayerInstance(&ici, &ci, &inst) == XR_SUCCESS, "create instance");

    auto gipa = req.getInstanceProcAddr;
    auto get = [&](const char* n) { PFN_xrVoidFunction f = nullptr; gipa(inst, n, &f); return f; };
    auto createAction = (PFN_xrCreateAction)get("xrCreateAction");
    auto suggest = (PFN_xrSuggestInteractionProfileBindings)get("xrSuggestInteractionProfileBindings");
    auto sync = (PFN_xrSyncActions)get("xrSyncActions");
    auto getFloat = (PFN_xrGetActionStateFloat)get("xrGetActionStateFloat");
    auto getBool = (PFN_xrGetActionStateBoolean)get("xrGetActionStateBoolean");
    auto s2p = (PFN_xrStringToPath)get("xrStringToPath");
    CHECK(getFloat != (PFN_xrGetActionStateFloat)rt_Float, "float not hooked");

    XrAction a;
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy(aci.actionName, "click"); aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT; createAction(0, &aci, &a);
    strcpy(aci.actionName, "touch"); aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT; createAction(0, &aci, &a);
    strcpy(aci.actionName, "grip");  aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT; createAction(0, &aci, &a);

    XrPath prof, pTrig, pTouch, pSqueeze, right;
    s2p(inst, "/interaction_profiles/valve/index_controller", &prof);
    s2p(inst, "/user/hand/right/input/trigger/value", &pTrig);
    s2p(inst, "/user/hand/right/input/trigger/touch", &pTouch);
    s2p(inst, "/user/hand/right/input/squeeze/value", &pSqueeze);
    s2p(inst, "/user/hand/right", &right);
    XrActionSuggestedBinding b[] = {{trigAction, pTrig}, {touchAction, pTouch}, {gripAction, pSqueeze}};
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = prof; sb.countSuggestedBindings = 3; sb.suggestedBindings = b;
    suggest(inst, &sb);

    XrSession sess = (XrSession)0x99;
    auto read = [&](XrAction act, float raw, XrBool32* changed = nullptr) {
        rawTrigger = raw; sync(sess, nullptr);
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = act; gi.subactionPath = right;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        getFloat(sess, &gi, &st);
        if (changed) *changed = st.changedSinceLastSync;
        return st.currentState;
    };

    XrBool32 ch;
    CHECK(read(trigAction, 0.0f) == 0.f, "0.0 -> 0");
    CHECK(read(trigAction, 0.5f, &ch) == 0.f && !ch, "half press 0.5 -> 0, unchanged");
    CHECK(read(trigAction, 0.85f) == 0.f, "0.85 below press -> 0");
    CHECK(read(trigAction, 0.95f, &ch) == 1.f && ch, "0.95 -> 1, changed");
    CHECK(read(trigAction, 0.95f, &ch) == 1.f && !ch, "held -> 1, unchanged");
    CHECK(read(trigAction, 0.7f) == 1.f, "0.7 while held stays 1 (hysteresis)");
    CHECK(read(trigAction, 0.55f, &ch) == 0.f && ch, "0.55 releases -> 0, changed");
    CHECK(read(trigAction, 0.7f) == 0.f, "0.7 after release stays 0");
    CHECK(read(gripAction, 0.5f) == 0.5f, "grip untouched");

    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = touchAction; gi.subactionPath = right;
    XrActionStateBoolean bs{XR_TYPE_ACTION_STATE_BOOLEAN};
    getBool(sess, &gi, &bs);
    CHECK(bs.currentState == XR_FALSE, "trigger touch forced off");

    // ---- hand skeleton
    auto createHT = (PFN_xrCreateHandTrackerEXT)get("xrCreateHandTrackerEXT");
    auto locate = (PFN_xrLocateHandJointsEXT)get("xrLocateHandJointsEXT");
    CHECK(locate != (PFN_xrLocateHandJointsEXT)rt_Locate, "locate not hooked");
    XrHandTrackerCreateInfoEXT hci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    hci.hand = XR_HAND_RIGHT_EXT; hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
    XrHandTrackerEXT ht; createHT(sess, &hci, &ht);
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandTrackingAimStateFB aimSt{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
    XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locs.next = &aimSt; locs.jointCount = XR_HAND_JOINT_COUNT_EXT; locs.jointLocations = joints;
    XrHandJointsLocateInfoEXT li2{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    auto seen = [&](float d, float aim = 0.f) {
        handDist = d; aimStrength = aim; locate(ht, &li2, &locs);
        auto& t = joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
        return std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    };
    auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    CHECK(approx(seen(0.10f), 0.10f), "10cm open passes through");
    CHECK(approx(seen(0.04f), 0.07f), "4cm half pinch reported as 7cm");
    CHECK(approx(joints[XR_HAND_JOINT_THUMB_DISTAL_EXT].pose.position.x, 0.10f), "distal shifted with tip");
    CHECK(approx(seen(0.02f), 0.07f), "2cm still open (not yet latched)");
    CHECK(approx(seen(0.013f), 0.07f), "1.3cm not a pinch with default 1.1cm close");
    CHECK(approx(seen(0.01f), 0.005f), "1cm pinches -> 0.5cm");
    CHECK(approx(seen(0.025f), 0.005f), "2.5cm held pinched (hysteresis)");
    seen(0.035f);  // a release only counts once the fingers stay apart for 80 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    CHECK(approx(seen(0.035f), 0.07f), "3.5cm (held 90 ms) releases -> 7cm");
    CHECK(approx(seen(0.003f), 0.003f), "already closer than pinched distance left alone");
    // noisy tip direction: thumb must open along index->distal (+x), not sideways
    seen(0.10f);  // release the pinch from the previous step (held past the 80 ms debounce)
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    seen(0.10f);
    tipSideways = true;
    seen(0.02f);
    { auto& t = joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
      CHECK(approx(t.x, 0.07f) && approx(t.y, 0.f), "opens along stable direction (got %.3f,%.3f)", t.x, t.y);
      auto& dj = joints[XR_HAND_JOINT_THUMB_DISTAL_EXT].pose.position;
      CHECK(approx(dj.x, 0.11f) && approx(dj.y, -0.02f), "distal moved by same offset (got %.3f,%.3f)", dj.x, dj.y); }
    tipSideways = false;
    seen(0.1f, 0.5f);
    CHECK(aimSt.pinchStrengthIndex == 0.f && !(aimSt.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB), "aim 0.5 -> 0");
    seen(0.1f, 0.95f);
    CHECK(aimSt.pinchStrengthIndex == 1.f && (aimSt.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB), "aim 0.95 -> 1");

    // ---- quick vs slow pinches, with real frame timing (11 ms frames)
    {
        long long t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count() + 10000000000LL;
        const long long frame = 11000000LL;  // 11 ms
        auto at = [&](float d) {
            t += frame;
            li2.time = (XrTime)t;
            return seen(d);
        };
        auto rest = [&](float d, int frames) { float r = 0; for (int i = 0; i < frames; ++i) r = at(d); return r; };

        // A. Quick deliberate pinch (~30 cm/s): hidden at rest, then the real positions get
        //    through, so the pinch lands exactly where the fingers are.
        tipSideways = false;
        CHECK(approx(rest(0.10f, 10), 0.10f), "A: open hand passes through");
        CHECK(approx(rest(0.05f, 40), 0.07f), "A: resting half pinch hidden (7cm)");
        float d = 0.05f, rep = 0;
        bool sawReal = false;
        while (d > 0.009f) {
            d -= 0.0033f;  // 30 cm/s
            rep = at(std::max(d, 0.009f));
            if (getenv("TRACE")) printf("  quick pinch frame: real %.1fcm  reported %.1fcm\n", std::max(d, 0.009f) * 100, rep * 100);
            if (d > 0.011f && approx(rep, std::max(d, 0.009f))) sawReal = true;
        }
        CHECK(sawReal, "A: real gap reaches the app before the pinch lands");
        CHECK(rep <= 0.009f + 1e-4f && rep >= 0.005f - 1e-4f, "A: quick pinch lands as a firm pinch (got %.4f)", rep);
        CHECK(approx(rest(0.009f, 20), 0.005f), "A: held quick pinch held firmly");
        CHECK(approx(rest(0.013f, 30), 0.005f), "A: resting just above the pinch mark stays a firm pinch, not a half pinch");
        // open again quickly
        d = 0.009f;
        for (int i = 0; i < 15; ++i) { d += 0.006f; rep = at(d); }
        CHECK(approx(rep, std::max(d, 0.07f)) || approx(rep, d), "A: after opening, back to hiding (got %.4f, d %.4f)", rep, d);
        CHECK(approx(rest(0.05f, 40), 0.07f), "A: half pinch hidden again afterwards");

        // B. Slow drift toward a pinch (3 cm/s) and resting at 1.6 cm: never passed through.
        rest(0.10f, 10);
        rest(0.06f, 40);  // settle (the jump from 10 cm is itself a quick move)
        d = 0.06f;
        bool leaked = false;
        while (d > 0.016f) { d -= 0.00033f; if (!approx(at(d), 0.07f)) leaked = true; }
        CHECK(!leaked, "B: slow drift stays hidden the whole way");
        CHECK(approx(rest(0.016f, 60), 0.07f), "B: resting near-pinch stays hidden");

        // C. Quick close that stops short (fingers come to rest at 2 cm): hidden again soon after.
        rest(0.10f, 10);
        rest(0.06f, 40);
        d = 0.06f;
        while (d > 0.02f) { d -= 0.0033f; at(d); }
        float early = at(0.02f);
        CHECK(early < 0.069f, "C: during the quick close the real gap was coming through (got %.4f)", early);
        CHECK(approx(rest(0.02f, 25), 0.07f), "C: stopped short of a pinch -> hidden again within ~0.3 s");

        // D. Slow pinch all the way: old behaviour, firm pinch at 0.5 cm.
        rest(0.10f, 10);
        rest(0.04f, 40);
        d = 0.04f;
        while (d > 0.010f) { d -= 0.0002f; at(d); }
        CHECK(approx(at(0.010f), 0.005f), "D: slow pinch latches to the firm 0.5cm pinch");
        CHECK(approx(rest(0.025f, 5), 0.005f), "D: holds until opened past release");
        CHECK(approx(rest(0.035f, 3), 0.005f), "D: a 33 ms spike past release doesn't let go");
        CHECK(approx(rest(0.035f, 8), 0.07f), "D: released once open for 80 ms");

        // F. Realistic jitter: fingers held close together (1.6 cm) with +/-2.5 mm tracking noise
        //    per frame, after a quick move toward the thumb. Must end up hidden and stay hidden.
        rest(0.10f, 10);
        rest(0.06f, 40);
        d = 0.06f;
        while (d > 0.016f) { d -= 0.0033f; at(d); }
        unsigned seed = 12345;
        auto noise = [&]() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) % 1000) / 1000.f * 0.005f - 0.0025f; };
        int visible = 0, visibleAfter300ms = 0;
        for (int i = 0; i < 270; ++i) {  // 3 seconds
            float r = at(0.016f + noise());
            if (r < 0.069f && r > 0.0051f) { ++visible; if (i >= 27) ++visibleAfter300ms; }  // half pinch
        }
        if (getenv("TRACE")) printf("  jitter: half pinch visible %d of 270 frames, %d after the first 300 ms\n", visible, visibleAfter300ms);
        if (getenv("SWEEP")) {
            for (float amp : {0.0025f, 0.005f, 0.008f})
                for (float hold : {0.013f, 0.016f, 0.02f, 0.03f}) {
                    rest(0.10f, 10); rest(0.06f, 40);
                    float dd = 0.06f;
                    while (dd > hold) { dd -= 0.0033f; at(dd); }
                    int vis = 0, late = 0, entries = 0;
                    for (int i = 0; i < 270; ++i) {
                        seed = seed * 1103515245u + 12345u;
                        float n = ((seed >> 16) % 1000) / 1000.f * 2 * amp - amp;
                        float r = at(hold + n);
                        if (r < 0.069f && r > 0.0051f) { ++vis; if (i >= 27) ++late; }
                    }
                    (void)entries;
                    printf("  amp +/-%.1fmm hold %.1fcm: half pinch shown %3d/270 frames, %3d after 300ms\n", amp * 1000, hold * 100, vis, late);
                }
        }
        CHECK(visibleAfter300ms == 0, "F: resting fingers with jitter hidden after 300 ms (visible %d frames later)", visibleAfter300ms);
        // G. The case from VR: quick move to ~1.3 cm and rest there with jitter that dips under
        //    the 1.1 cm pinch mark. Never a lingering half pinch.
        // (Up to +/-5 mm per frame; at +/-8 mm, far beyond normal tracking, the sweep below
        //  still shows only brief flickers at large gaps.)
        for (float amp : {0.0025f, 0.005f}) {
            for (float hold : {0.013f, 0.016f, 0.02f, 0.03f}) {
                rest(0.10f, 10); rest(0.06f, 40);
                float dd = 0.06f;
                while (dd > hold) { dd -= 0.0033f; at(dd); }
                int late = 0;
                for (int i = 0; i < 270; ++i) {
                    seed = seed * 1103515245u + 12345u;
                    float n = ((seed >> 16) % 1000) / 1000.f * 2 * amp - amp;
                    float r = at(hold + n);
                    if (r < 0.069f && r > 0.0051f && i >= 33) ++late;  // half pinch after 0.36 s
                }
                CHECK(late == 0, "G: +/-%.1fmm jitter resting at %.1fcm: half pinch still shown %d frames", amp * 1000, hold * 100, late);
            }
        }

        // H. What the VR log showed: a quick pinch where, once the fingers touch, the tracked gap
        //    bounces between 1.15 and 2.4 cm and never crosses the 1.1 cm pinch mark. It must be
        //    recognised early, never pushed back out to the open-hand distance, and held as a pinch.
        for (float speedCm : {12.f, 15.f, 20.f, 30.f}) {
            rest(0.10f, 10);
            rest(0.045f, 40);
            float dd = 0.045f, startGap = -1, lastRep = 0;
            bool pushedOut = false;
            while (dd > 0.012f) {
                dd -= speedCm / 100.f * 0.011f;
                float r = at(std::max(dd, 0.012f));
                if (startGap < 0 && r < 0.069f) startGap = std::max(dd, 0.012f);
            }
            for (int i = 0; i < 45; ++i) {  // 0.5 s of bouncy "touching"
                seed = seed * 1103515245u + 12345u;
                float g = 0.0115f + ((seed >> 16) % 1000) / 1000.f * 0.0125f;
                lastRep = at(g);
                if (lastRep > 0.069f) pushedOut = true;
            }
            if (getenv("TRACE")) printf("  H %2.0fcm/s: real positions from %.1fcm, pushed out %d, end %.1fcm\n", speedCm, startGap * 100, pushedOut, lastRep * 100);
            CHECK(startGap >= 0.02f, "H %.0fcm/s: real positions should start by 2 cm (started at %.1fcm)", speedCm, startGap * 100);
            CHECK(!pushedOut, "H %.0fcm/s: thumb pushed back out while fingers touching", speedCm);
            CHECK(approx(lastRep, 0.005f), "H %.0fcm/s: held as a firm pinch (got %.4f)", speedCm, lastRep);
            rest(0.05f, 10);  // open again
        }

        // I. One pinch must be exactly one click. Model X-Plane's own pinch detection on what it
        //    is shown (press below `xp`, release above xp + 0.5 cm) for several plausible thresholds,
        //    and replay pinches whose tracked gap bounces while the fingers touch, like the VR log.
        for (float xp : {0.009f, 0.012f, 0.015f, 0.02f}) {
            for (int kind = 0; kind < 2; ++kind) {  // 0 = quick pinch, 1 = slow pinch
                int presses = 0;
                bool down = false;
                auto xplane = [&](float r) {
                    if (!down && r < xp) { down = true; ++presses; }
                    else if (down && r > xp + 0.005f) down = false;
                };
                rest(0.10f, 10);
                rest(0.045f, 40);
                float dd = 0.045f;
                float step = kind == 0 ? 0.0025f : 0.0002f;
                while (dd > 0.013f) { dd -= step; xplane(at(dd)); }
                // touching: bounce 0.8..2.5 cm, with an occasional spike to 3.2 cm
                for (int i = 0; i < 60; ++i) {
                    seed = seed * 1103515245u + 12345u;
                    float g = 0.008f + ((seed >> 16) % 1000) / 1000.f * 0.017f;
                    if (i == 20 || i == 41) g = 0.032f;
                    xplane(at(g));
                }
                // let go
                for (int i = 0; i < 20; ++i) xplane(at(0.08f));
                if (getenv("TRACE")) printf("  I xp=%.1fcm %s: %d click(s)\n", xp * 100, kind ? "slow " : "quick", presses);
                CHECK(presses == 1, "I: X-Plane threshold %.1fcm, %s pinch with bounce: %d clicks (want 1)", xp * 100, kind ? "slow" : "quick", presses);
            }
        }

        // E. Quick-pinch pass-through can be switched off.
        li2.time = 0;
    }

    // ---- live reload: disable via ini while running
    {
        std::string dir = argv[1];
        dir = dir.substr(0, dir.find_last_of("/\\"));
        std::string ini = dir + "/trigger_snap.ini";
        { std::ofstream o(ini); o << "enabled = 0\n"; }
        sleep(2);
        read(trigAction, 0.f);  // sync triggers the reload check
        CHECK(read(trigAction, 0.5f) == 0.5f, "after reload enabled=0, raw value passes through");
        CHECK(approx(seen(0.04f), 0.04f), "after reload enabled=0, joints untouched");
        { std::ofstream o(ini); o << "enabled = 1\njoint_close_cm = 2.0\n"; }
        sleep(2);
        read(trigAction, 0.f);
        CHECK(read(trigAction, 0.5f) == 0.f, "re-enabled, snapping again");
        CHECK(approx(seen(0.018f), 0.005f), "new close distance 2.0cm applied live");
        remove(ini.c_str());
    }

    printf(failures ? "%d FAILURE(S)\n" : "ALL TESTS PASSED\n", failures);
    fflush(stdout);
    if (const char* hold = getenv("HOLD_SECS")) sleep(atoi(hold));  // keep the "X-Plane" instance alive
    return failures != 0;
}
