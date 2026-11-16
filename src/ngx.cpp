// The method: every DLSS game ends up calling NVIDIA's NGX core (_nvngx.dll), whether it goes through
// Streamline or not. That call is the one place common to all of them.
//
//   in    the core's CreateFeature / EvaluateFeature / ReleaseFeature are hooked. A DLSS Super Resolution
//         or Ray Reconstruction evaluate of the game gives the frame, the depth, the motion vectors and
//         the jitter, straight from its parameters.
//   out   right after that evaluate, on the same command list, the DLSS 5 model (NGX feature 18, the file
//         nvngx_dlssnr.dll next to the game) is evaluated with the parameters NVIDIA's own plugin gives it.
//
// Nothing of the game is replaced. A game that creates feature 18 itself is a native DLSS 5 title and is
// left alone.
#include "colorstage.h"
#include "menu.h"
#include "ngxframe.h"
#include <cmath>

namespace d5 {

extern std::atomic<bool> g_presentOverlay;
void presentOverlayStart(ID3D12GraphicsCommandList* cl);
bool g_hostNative = false;

namespace {

using PFun_Create = NVSDK_NGX_Result NVSDK_CONV(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFun_Evaluate = NVSDK_NGX_Result NVSDK_CONV(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFun_Release = NVSDK_NGX_Result NVSDK_CONV(NVSDK_NGX_Handle*);
using PFun_Allocate = NVSDK_NGX_Result NVSDK_CONV(NVSDK_NGX_Parameter**);
using PFun_Destroy = NVSDK_NGX_Result NVSDK_CONV(NVSDK_NGX_Parameter*);
using PFun_Init = NVSDK_NGX_Result NVSDK_CONV(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using PFun_Shutdown = NVSDK_NGX_Result NVSDK_CONV(ID3D12Device*);

constexpr NVSDK_NGX_Feature kFeatureNR = NVSDK_NGX_Feature_Reserved18;
// The application id the RenoDX DLSS 5 add-on initializes the model with; the model accepts it.
constexpr unsigned long long kApplicationId = 0x876232cULL;

// ---- the NGX core
HMODULE g_core = nullptr;
PFun_Create* oCoreCreate = nullptr;
PFun_Evaluate* oCoreEvaluate = nullptr;
PFun_Release* oCoreRelease = nullptr;
PFun_Allocate* coreAllocate = nullptr;
PFun_Destroy* coreDestroy = nullptr;

// ---- the model's own file
struct Runtime {
    HMODULE module = nullptr; bool looked = false;
    PFun_Init* init = nullptr; PFun_Shutdown* shutdown = nullptr;
    PFun_Create* create = nullptr; PFun_Evaluate* evaluate = nullptr; PFun_Release* release = nullptr;
    PFun_Allocate* allocate = nullptr; PFun_Destroy* destroy = nullptr;
    ID3D12Device* device = nullptr;     // the device it was initialized for (a reference is held)
    bool ready = false, failed = false;
} rt;

// True while this thread is inside one of our own NGX calls: the hooks then stand aside.
thread_local bool t_ours = false;
struct Ours { bool was; Ours() : was(t_ours) { t_ours = true; } ~Ours() { t_ours = was; } };

// One model feature: a pass of one game feature, at one size.
struct Slot {
    NVSDK_NGX_Handle* handle = nullptr; NVSDK_NGX_Parameter* params = nullptr;
    bool viaCore = false, paramsFromCore = false, failed = false, fresh = false;
    UINT w = 0, h = 0;
};
// One DLSS feature of the game, and what serves it.
struct Game {
    int feature = -1;                   // NVSDK_NGX_Feature, -1 = created before the hooks were in place
    GameFeature info;
    ColorStage stage;
    Slot slot[kMaxPasses];
    uint64_t evaluates = 0;
    UINT nrW = 0, nrH = 0, pendingW = 0, pendingH = 0; int settle = 0; uint32_t resolutionGeneration = 0;
    LARGE_INTEGER lastTime{}; ULONGLONG lastReset = 0, lastUsed = 0;
};
struct Retired { Slot slot; ULONGLONG since; };

std::recursive_mutex g_mx;
std::unordered_map<const NVSDK_NGX_Handle*, Game> g_games;      // nodes never move
std::vector<Retired> g_retired;
Menu g_menu;
uint64_t g_evalOk = 0, g_evalFail = 0;
int g_lastErr = 0;
bool g_faulted = false;

NVSDK_NGX_Result NVSDK_CONV ratioCallback(NVSDK_NGX_Parameter* p) {
    if (!p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    p->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
}

TexSize sizeOfTexture(ID3D12Resource* r) {
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return TexSize{};
    return TexSize{(uint32_t)d.Width, d.Height};
}

// ------------------------------------------------------------------------------------ the model's file
void releaseSlot(Slot& s) {
    Ours ours;
    if (s.handle) { if (s.viaCore) { if (oCoreRelease) oCoreRelease(s.handle); } else if (rt.release) rt.release(s.handle); }
    if (s.params) { if (s.paramsFromCore) { if (coreDestroy) coreDestroy(s.params); } else if (rt.destroy) rt.destroy(s.params); }
    s = Slot();
}
// A feature may still be used by command lists the GPU has not run yet: it is released a few seconds later.
void retireSlot(Slot& s) {
    if (s.handle || s.params) g_retired.push_back(Retired{s, GetTickCount64()});
    s = Slot();
}
void sweepRetired(bool all) {
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_retired.size();) {
        if (all || now - g_retired[i].since > 3000) { releaseSlot(g_retired[i].slot); g_retired[i] = g_retired.back(); g_retired.pop_back(); }
        else i++;
    }
}

// The model asks Windows for the file name of the module that calls it and only answers a caller named
// like the NGX core. That one question, asked about this DLL, is answered with the core's name: the
// model's import of GetModuleFileNameW is pointed at the function below. (The RenoDX add-on does the same.)
using PFun_GetModuleFileNameW = DWORD WINAPI(HMODULE, LPWSTR, DWORD);
PFun_GetModuleFileNameW* oModelGetModuleFileName = nullptr;
HMODULE g_self = nullptr;
DWORD WINAPI modelGetModuleFileName(HMODULE module, LPWSTR name, DWORD size) {
    static const wchar_t kCore[] = L"nvngx.dll"; const DWORD len = (DWORD)(sizeof(kCore) / sizeof(wchar_t)) - 1;
    if (module && module == g_self && name && size) {
        if (len >= size) { name[0] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
        memcpy(name, kCore, sizeof(kCore));
        return len;
    }
    return oModelGetModuleFileName ? oModelGetModuleFileName(module, name, size) : 0;
}
bool answerAsCore(HMODULE model) {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&modelGetModuleFileName, &g_self) || !g_self) return false;
    void** slot = findImport(model, "GetModuleFileNameW");
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    oModelGetModuleFileName = (PFun_GetModuleFileNameW*)*slot;
    *slot = (void*)&modelGetModuleFileName;
    DWORD ignored = 0; VirtualProtect(slot, sizeof(*slot), old, &ignored);
    return true;
}

bool loadRuntime() {
    if (rt.looked) return rt.module != nullptr;
    rt.looked = true;
    wchar_t path[MAX_PATH]; fill(path, L"%s\\nvngx_dlssnr.dll", g_dir);
    { Ours ours; rt.module = LoadLibraryW(path); }
    if (!rt.module) { logf("model: %ls could not be loaded (error %lu): DLSS 5 stays off", path, GetLastError()); return false; }
    rt.init = (PFun_Init*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_Init_Ext");
    rt.shutdown = (PFun_Shutdown*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_Shutdown1");
    rt.create = (PFun_Create*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_CreateFeature");
    rt.evaluate = (PFun_Evaluate*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_EvaluateFeature");
    rt.release = (PFun_Release*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_ReleaseFeature");
    rt.allocate = (PFun_Allocate*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_AllocateParameters");
    rt.destroy = (PFun_Destroy*)GetProcAddress(rt.module, "NVSDK_NGX_D3D12_DestroyParameters");
    logf("model: %ls loaded at %p (init=%d create=%d evaluate=%d release=%d shutdown=%d allocator=%d)", path, rt.module,
         rt.init != nullptr, rt.create != nullptr, rt.evaluate != nullptr, rt.release != nullptr, rt.shutdown != nullptr, rt.allocate && rt.destroy);
    if (!rt.init || !rt.create || !rt.evaluate || !rt.release) { logf("model: this file does not export the NGX D3D12 functions: DLSS 5 stays off"); rt.module = nullptr; return false; }
    const bool asCore = answerAsCore(rt.module);
    logf("model: caller name answered as the NGX core: %s", asCore ? "yes" : "NO (the model will refuse a direct call)");
    return rt.module != nullptr;
}

// The model is initialized once per device, on the game's own device.
bool ensureRuntime(ID3D12GraphicsCommandList* cl) {
    ID3D12Device* dev = nullptr; cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return false;
    if (rt.device == dev) { dev->Release(); return rt.ready; }
    if (rt.device) {    // the game made a new device: what belonged to the old one goes with it
        for (auto& g2 : g_games) for (Slot& s : g2.second.slot) retireSlot(s);
        sweepRetired(true);
        if (rt.ready && rt.shutdown) { Ours ours; rt.shutdown(rt.device); }
        rt.device->Release(); rt.device = nullptr; rt.ready = false; rt.failed = false;
        logf("model: the game changed device, the model is initialized again");
    }
    if (!loadRuntime()) { dev->Release(); return false; }
    rt.device = dev;        // keeps the reference
    NVSDK_NGX_Result r;
    { Ours ours; r = rt.init(kApplicationId, g_dir, dev, NVSDK_NGX_Version_API, nullptr); }
    rt.ready = NVSDK_NGX_SUCCEED(r); rt.failed = !rt.ready;
    logf("model: init result=0x%x%s", (unsigned)r, rt.ready ? "" : " - DLSS 5 stays off on this device");
    return rt.ready;
}

bool createSlot(ID3D12GraphicsCommandList* cl, Slot& s, UINT w, UINT h, uint32_t gameFlags, int pass) {
    if (!s.params) {
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
        { Ours ours;
          if (coreAllocate && coreDestroy) { r = coreAllocate(&s.params); s.paramsFromCore = NVSDK_NGX_SUCCEED(r) && s.params; }
          if (!s.params && rt.allocate && rt.destroy) { r = rt.allocate(&s.params); s.paramsFromCore = false; } }
        if (!s.params) { s.failed = true; logf("model: no parameter block could be allocated (0x%x)", (unsigned)r); return false; }
    }
    writeModelCreate(s.params, w, h, gameFlags, g.opts.preset, (void*)&ratioCallback);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail, viaCore = NVSDK_NGX_Result_Fail;
    { Ours ours;
      // through the core when the game's NGX has one (the model expects the core as its caller), else straight
      if (oCoreCreate && s.paramsFromCore) { r = viaCore = oCoreCreate(cl, kFeatureNR, s.params, &s.handle); s.viaCore = NVSDK_NGX_SUCCEED(r) && s.handle; }
      if (!s.viaCore) { s.handle = nullptr; r = rt.create(cl, kFeatureNR, s.params, &s.handle); } }
    if (NVSDK_NGX_FAILED(r) || !s.handle) {
        s.handle = nullptr; s.failed = true;
        logf("model: create FAILED at %ux%u pass %d (through the core 0x%x, direct 0x%x)", w, h, pass, (unsigned)viaCore, (unsigned)r);
        return false;
    }
    s.w = w; s.h = h; s.fresh = true;
    logf("model: created at %ux%u pass %d, %s, game DLSS flags 0x%x", w, h, pass, s.viaCore ? "through the NGX core" : "directly", gameFlags);
    return true;
}

// What the model is handed, logged whenever it changes: the proof that a setting reaches it.
void reportOptions(const DLSSNROptions& o, int pass) {
    static DLSSNROptions last[kMaxPasses]; static bool seen[kMaxPasses] = {};
    if (seen[pass] && memcmp(&last[pass], &o, sizeof(o)) == 0) return;
    last[pass] = o; seen[pass] = true;
    logf("model receives: Intensity=%.2f LocalTone=%.2f LocalStructure=%.2f GlobalTone=%.2f Skin=%.2f Style=%u AutoMask=%u Preset=%u (pass %d)",
         o.intensity, o.localToneStrength, o.localStructureStrength, o.globalToneStrength, o.skinStructureStrength, o.style, (unsigned)o.useAutoMask, o.preset, pass);
}

NVSDK_NGX_Result evaluateModel(ID3D12GraphicsCommandList* cl, Game& gm, int pass, const GameFrame& gf, ID3D12Resource* proxy, ID3D12Resource* result,
                               UINT w, UINT h, UINT baseX, UINT baseY) {
    Slot& s = gm.slot[pass];
    if (s.handle && (s.w != w || s.h != h)) retireSlot(s);
    if (!s.handle) { if (s.failed || !createSlot(cl, s, w, h, gm.info.createFlags, pass)) return NVSDK_NGX_Result_Fail; }
    DLSSNROptions o = sentOptions(g.opts);
    if (pass > 0) o.intensity = g.passIntensity[pass - 1];
    reportOptions(o, pass);
    const bool reset = gf.reset || s.fresh; s.fresh = false;
    writeModelEvaluate(s.params, proxy, result, w, h, gf, gm.info, o, reset, (void*)&ratioCallback);
    if (baseX || baseY) {       // the game's own texture, in place: the rectangle is where the game put it
        s.params->Set("DLSSNR.ColorSubrectBaseX", (unsigned int)baseX); s.params->Set("DLSSNR.ColorSubrectBaseY", (unsigned int)baseY);
        s.params->Set("DLSSNR.OutputSubrectBaseX", (unsigned int)baseX); s.params->Set("DLSSNR.OutputSubrectBaseY", (unsigned int)baseY);
    }
    Ours ours;
    return s.viaCore ? oCoreEvaluate(cl, s.handle, s.params, nullptr) : rt.evaluate(cl, s.handle, s.params, nullptr);
}

// ------------------------------------------------------------------------------------ per frame
void keys() {
    static bool wasToggle = false, wasReload = false;
    const bool toggle = (GetAsyncKeyState(g.toggleKey) & 0x8000) != 0, reload = (GetAsyncKeyState(g.reloadKey) & 0x8000) != 0;
    if (toggle && !wasToggle) { g.enabled = !g.enabled; g_settingsGeneration++; logf("toggle: enabled=%d", g.enabled); }
    if (reload && !wasReload) loadSettings();
    wasToggle = toggle; wasReload = reload;
    // the file edited while the game runs is picked up on its own (checked twice a second)
    static ULONGLONG lastCheck = 0; const ULONGLONG nowMs = GetTickCount64();
    if (nowMs - lastCheck >= 500) { lastCheck = nowMs; if (settingsChangedOnDisk()) { logf("settings: file changed on disk, reloaded"); loadSettings(); } }
    g_menu.input();
}

// Working resolution of the model. A change coming from the game (dynamic resolution) is applied once
// it has held for 30 frames, so the model is not rebuilt on every fluctuation; a change of the setting
// is applied at once.
void workingResolution(Game& v, UINT w, UINT h, const GameFrame& gf) {
    UINT nw, nh;
    if (g.followRenderRes && gf.renderW && gf.renderH) { nw = gf.renderW < w ? gf.renderW : w; nh = gf.renderH < h ? gf.renderH : h; }
    else {
        const float s = g.followRenderRes ? 1.0f : g.resolutionScale;
        nw = (UINT)lroundf((float)w * s); nh = (UINT)lroundf((float)h * s);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
    }
    const uint32_t gen = g_settingsGeneration.load();
    if (v.nrW == 0 || v.resolutionGeneration != gen) { v.nrW = nw; v.nrH = nh; v.settle = 0; v.resolutionGeneration = gen; return; }
    if (nw == v.nrW && nh == v.nrH) { v.settle = 0; return; }
    if (nw == v.pendingW && nh == v.pendingH) { if (++v.settle >= 30) { v.nrW = nw; v.nrH = nh; v.settle = 0; } }
    else { v.pendingW = nw; v.pendingH = nh; v.settle = 0; }
}

void countResult(NVSDK_NGX_Result r) {
    if (NVSDK_NGX_SUCCEED(r)) { if (g_evalOk++ == 0) logf("DLSS 5 evaluate OK (first frame)"); }
    else { if (g_evalFail++ < 5 || (int)r != g_lastErr) logf("DLSS 5 evaluate FAILED result=0x%x (fail #%llu)", (unsigned)r, g_evalFail); g_lastErr = (int)r; }
    if (((g_evalOk + g_evalFail) % 3000) == 0) logf("stats: ok=%llu fail=%llu lastErr=0x%x", g_evalOk, g_evalFail, (unsigned)g_lastErr);
}

const char* const kDumpNumeric[] = {
    "Width", "Height", "OutWidth", "OutHeight", "PerfQualityValue", "DLSS.Feature.Create.Flags", "DLSS.Pre.Exposure", "DLSS.Exposure.Scale", "Reset",
    "MV.Scale.X", "MV.Scale.Y", "Jitter.Offset.X", "Jitter.Offset.Y", "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
    "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y", "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
    "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y", "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y",
};
const char* const kDumpResource[] = {"Color", "Output", "MotionVectors", "Depth", "ExposureTexture"};
// DumpNGX=1: what the game hands to DLSS, value by value.
void dumpGameParams(const NVSDK_NGX_Parameter* p, unsigned call) {
    for (const char* n : kDumpNumeric) {
        float f = 0; unsigned int u = 0; int i = 0;
        const NVSDK_NGX_Result rf = p->Get(n, &f), ru = p->Get(n, &u), ri = p->Get(n, &i);
        if (rf == NVSDK_NGX_Result_Success || ru == NVSDK_NGX_Result_Success || ri == NVSDK_NGX_Result_Success) logf("NGX #%u %-40s float=%g uint=%u int=%d", call, n, f, u, i);
    }
    for (const char* n : kDumpResource) {
        ID3D12Resource* r = nullptr;
        if (p->Get(n, &r) == NVSDK_NGX_Result_Success && r) { const D3D12_RESOURCE_DESC d = r->GetDesc(); logf("NGX #%u %-40s resource %llux%u dxgiFormat=%u flags=0x%x", call, n, d.Width, d.Height, (unsigned)d.Format, (unsigned)d.Flags); }
    }
}

// Runs right after a DLSS evaluate of the game, on the game's command list. At this point the resources
// are in the states NGX asks of every caller: the output writable, depth and motion vectors readable.
void afterGameDlss(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    Game& gm = g_games[handle];
    if (gm.feature != -1 && gm.feature != NVSDK_NGX_Feature_SuperSampling && gm.feature != NVSDK_NGX_Feature_RayReconstruction) return;
    GameFrame gf;
    if (!readGameFrame(p, gm.info, sizeOfTexture, gf)) {
        if (gm.feature != -1) { static bool once = false; if (!once) { once = true; logf("skip: a DLSS evaluate without an output, depth or motion vectors"); } }
        return;
    }
    const ULONGLONG nowMs = GetTickCount64();
    gm.lastUsed = nowMs;
    if (gm.evaluates++ == 0) {
        logf("game DLSS evaluate seen: feature %d, output %ux%u at %u,%u, rendered %ux%u, motion vectors %ux%u scale %g,%g, game DLSS flags %s0x%x",
             gm.feature, gf.rect.right - gf.rect.left, gf.rect.bottom - gf.rect.top, gf.rect.left, gf.rect.top, gf.renderW, gf.renderH, gf.motionW, gf.motionH,
             gf.mvScaleX, gf.mvScaleY, gm.info.flagsKnown ? "" : "unknown ", gm.info.createFlags);
    }
    if (g.dumpNgx && (gm.evaluates == 1 || gm.evaluates == 120)) dumpGameParams(p, (unsigned)gm.evaluates);
    keys();
    sweepRetired(false);
    if ((gm.evaluates & 1023u) == 0)    // a feature the game stopped using gives its GPU memory back
        for (auto& other : g_games) if (&other.second != &gm && other.second.lastUsed && nowMs - other.second.lastUsed > 30000) {
            for (Slot& s : other.second.slot) retireSlot(s);
            other.second.stage.releaseAll(); other.second.lastUsed = 0; other.second.nrW = other.second.nrH = 0;
        }
    // A native DLSS 5 title creates the model itself within its first frames: nothing is started before
    // that question is settled.
    if (gm.evaluates <= 60 || g_hostNative) return;
    if (!g.enabled && !g_menu.visible) return;

    const UINT w = gf.rect.right - gf.rect.left, h = gf.rect.bottom - gf.rect.top;
    const ModelFrame mf = toModelFrame(gf, gm.info);
    const D3D12_RESOURCE_STATES kOut = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (g.enabled && !g.colorStage) {
        // Strict in-place call, what a DLSS 5 title does: the DLSS output is the model's input and output.
        if (!ensureRuntime(cl)) return;
        countResult(evaluateModel(cl, gm, 0, gf, gf.output, gf.output, w, h, gf.rect.left, gf.rect.top));
        return;
    }

    ColorStage::Frame f;
    f.gameOut = gf.output; f.gameState = kOut; f.rect = gf.rect;
    if (gf.exposure) { f.exposure = gf.exposure; f.exposureState = ColorStage::kRead; }

    char status[200] = "";
    if (g_menu.visible) {
        if (!g.enabled) fill(status, "Off");
        else if (rt.failed || (rt.looked && !rt.module)) fill(status, "The DLSS 5 model could not be started (see dlss5native.log)");
        else fill(status, "Running, model at %ux%u", gm.nrW ? gm.nrW : w, gm.nrH ? gm.nrH : h);
    }
    presentOverlayStart(cl);
    const Panel* panel = nullptr;      // drawn at Present when that works, else into the game's frame
    if (g_presentOverlay.load()) g_menu.setStatus(status); else panel = g_menu.prepare(cl, w, h, status);

    if (!g.enabled) { if (panel) gm.stage.overlayOnly(cl, f, *panel); return; }      // the panel alone
    if (!g.skipModel && !ensureRuntime(cl)) return;

    ColorStageParams params = g.cs;
    {
        LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
        const double dt = gm.lastTime.QuadPart ? double(now.QuadPart - gm.lastTime.QuadPart) / double(freq.QuadPart) : 1.0 / 60.0; gm.lastTime = now;
        params.dt = (float)(dt < 0.001 ? 0.001 : (dt > 0.1 ? 0.1 : dt));
        // a history reset flagged by the game re-primes the divisor, unless resets come in a burst
        params.snap = (gf.reset && nowMs - gm.lastReset >= 250) ? 1.0f : 0.0f;
        if (gf.reset) gm.lastReset = nowMs;
    }
    workingResolution(gm, w, h, gf);
    f.nrW = gm.nrW; f.nrH = gm.nrH;

    const int passes = g.passes;
    { static int lastPasses = 0; if (passes != lastPasses) { lastPasses = passes; logf("model runs %d time(s) per frame", passes); } }
    for (int pass = 0; pass < passes; pass++) {
        f.pass = pass;
        if (!gm.stage.before(cl, f, params, g.cfg, mf, g.inPlace)) { static bool once = false; if (!once) { once = true; logf("skip: colour stage unavailable"); } break; }
        params.snap = 0.0f;
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
        if (!g.skipModel) r = evaluateModel(cl, gm, pass, gf, gm.stage.pin, gm.stage.pout, gm.stage.nrW, gm.stage.nrH, 0, 0);
        if (NVSDK_NGX_SUCCEED(r)) {
            ModelFrame use = mf;
            if (use.motion && !ColorStage::motionUsable(use.motion)) use.motion = nullptr;
            const bool temporal = g.detailStability == 2 || (g.detailStability == 0 && use.motion);
            gm.stage.after(cl, use, ColorStage::kRead, temporal, pass == passes - 1 ? panel : nullptr);
        } else gm.stage.abandon(cl);
        if (!g.skipModel) countResult(r);
        if (NVSDK_NGX_FAILED(r)) break;
    }
}

// A fault in our own work must not take the game down: it is logged and the work is not attempted again.
void afterGameDlssGuarded(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p) {
    __try { afterGameDlss(cl, handle, p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; logf("FAULT 0x%lx in the work after the game's DLSS; DLSS 5 is now off until the game restarts", GetExceptionCode()); }
}

// ------------------------------------------------------------------------------------ the core's hooks
NVSDK_NGX_Result NVSDK_CONV hkCoreCreate(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** handle) {
    const NVSDK_NGX_Result r = oCoreCreate(cl, feature, p, handle);
    if (t_ours || NVSDK_NGX_FAILED(r) || !handle || !*handle) return r;
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    if (feature == kFeatureNR) {
        if (!g_hostNative) logf("the game creates DLSS 5 (NGX feature 18) itself: native DLSS 5 title, nothing is added");
        g_hostNative = true;
        for (auto& g2 : g_games) for (Slot& s : g2.second.slot) retireSlot(s);
        g_games[*handle].feature = (int)feature;        // its evaluates are the game's own DLSS 5, not a DLSS frame
        return r;
    }
    Game& gm = g_games[*handle];
    for (Slot& s : gm.slot) retireSlot(s);      // the address may be one a released feature had
    gm.feature = (int)feature; gm.info = GameFeature(); gm.evaluates = 0; gm.nrW = gm.nrH = 0;
    if (feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_RayReconstruction) {
        if (p) readGameFeature(p, gm.info);
        logf("game creates NGX feature %d (%s): render %ux%u -> output %ux%u, flags %s0x%x", (int)feature, feature == NVSDK_NGX_Feature_SuperSampling ? "DLSS" : "DLSS Ray Reconstruction",
             gm.info.renderW, gm.info.renderH, gm.info.outW, gm.info.outH, gm.info.flagsKnown ? "" : "unknown ", gm.info.createFlags);
    } else logf("game creates NGX feature %d (not DLSS: ignored)", (int)feature);
    return r;
}

// A game with DLSS 5 of its own: its evaluates are only counted, so the log says whether it really runs.
void countHostEvaluate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Result r) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    auto it = g_games.find(handle);
    if (it == g_games.end() || it->second.feature != (int)kFeatureNR) return;
    static uint64_t ok = 0, fail = 0; static int lastErr = 0;
    if (NVSDK_NGX_SUCCEED(r)) { if (ok++ == 0) logf("the game's own DLSS 5 evaluate OK (first frame)"); }
    else { if (fail++ < 3 || (int)r != lastErr) logf("the game's own DLSS 5 evaluate FAILED result=0x%x (fail #%llu)", (unsigned)r, fail); lastErr = (int)r; }
    if (((ok + fail) % 1000) == 0) logf("the game's own DLSS 5: ok=%llu fail=%llu", ok, fail);
}

NVSDK_NGX_Result NVSDK_CONV hkCoreEvaluate(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback cb) {
    const NVSDK_NGX_Result r = oCoreEvaluate(cl, handle, p, cb);
    if (!t_ours && handle && g_hostNative) countHostEvaluate(handle, r);
    if (t_ours || g_faulted || NVSDK_NGX_FAILED(r) || !cl || !handle || !p) return r;
    afterGameDlssGuarded(cl, handle, p);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV hkCoreRelease(NVSDK_NGX_Handle* handle) {
    if (!t_ours && handle) {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        auto it = g_games.find(handle);
        if (it != g_games.end()) { for (Slot& s : it->second.slot) retireSlot(s); it->second.feature = -1; it->second.info = GameFeature(); it->second.evaluates = 0; }
    }
    return oCoreRelease(handle);
}

void hookCore(HMODULE h) {
    if (h == g_core) return;
    PFun_Create* c = (PFun_Create*)GetProcAddress(h, "NVSDK_NGX_D3D12_CreateFeature");
    PFun_Evaluate* e = (PFun_Evaluate*)GetProcAddress(h, "NVSDK_NGX_D3D12_EvaluateFeature");
    PFun_Release* r = (PFun_Release*)GetProcAddress(h, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!c || !e || !r) { logf("NGX core at %p does not export the D3D12 functions: not hooked", h); return; }
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    g_core = h; oCoreCreate = c; oCoreEvaluate = e; oCoreRelease = r;
    coreAllocate = (PFun_Allocate*)GetProcAddress(h, "NVSDK_NGX_D3D12_AllocateParameters");
    coreDestroy = (PFun_Destroy*)GetProcAddress(h, "NVSDK_NGX_D3D12_DestroyParameters");
    DetourTransactionBegin();
    DetourAttach(&(PVOID&)oCoreCreate, hkCoreCreate);
    DetourAttach(&(PVOID&)oCoreEvaluate, hkCoreEvaluate);
    DetourAttach(&(PVOID&)oCoreRelease, hkCoreRelease);
    const LONG err = DetourTransactionCommit();
    logf("NGX core hooks (create, evaluate, release) in %p: %ld, parameter allocator %s", h, err, coreAllocate && coreDestroy ? "present" : "absent");
}

bool named(const wchar_t* name, size_t len, const wchar_t* what) { return len == wcslen(what) && _wcsnicmp(name, what, len) == 0; }

} // namespace

Menu& panelMenu() { return g_menu; }
bool insideOwnCall() { return t_ours; }

// Called for every module the process loads, and once at start for a core that is already there.
void onModuleLoaded(HMODULE h, const wchar_t* baseName, size_t baseLen) {
    if (!h) return;
    if (named(baseName, baseLen, L"_nvngx.dll") || named(baseName, baseLen, L"nvngx.dll")) hookCore(h);
}

} // namespace d5
