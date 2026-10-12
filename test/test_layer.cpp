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
    CHECK(approx(seen(0.035f), 0.07f), "3.5cm releases -> 7cm");
    CHECK(approx(seen(0.003f), 0.003f), "already closer than pinched distance left alone");
    // noisy tip direction: thumb must open along index->distal (+x), not sideways
    seen(0.10f);  // release the pinch from the previous step
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
