// dlss5native - shared declarations.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"
#include "detours.h"

namespace d5 {

#define D5_VERSION "1.0"
#define D5_VERSION_W L"1.0"

// ---- the model's options, sent to it as the DLSSNR.* parameters of the same names ----
struct DLSSNROptions {
    float intensity = 1.0f;               // DLSSNR.Intensity
    float localToneStrength = 1.0f;       // DLSSNR.LocalToneStrength
    float localStructureStrength = 1.0f;  // DLSSNR.LocalStructureStrength
    float globalToneStrength = 1.0f;      // DLSSNR.GlobalToneStrength
    uint32_t style = 0;                   // DLSSNR.Style
    uint32_t preset = 0;                  // DLSSNR.Hint.Render.Preset
    bool useAutoMask = false;             // DLSSNR.UseAutoMask
    float skinStructureStrength = -1.0f;  // DLSSNR.SkinStructureStrength
};
// What goes out to the model for structure and skin, as RenoDX sends them with its default "Independent
// skin". The model reads the skin setting only with the character mask; a negative skin value is the
// stock one, 1; and above Structure 1 the model ignores the skin setting, so Structure goes out at most 1.
// Without the mask the stored values go out unchanged.
inline DLSSNROptions sentOptions(DLSSNROptions o) {
    if (!o.useAutoMask) return o;
    if (o.localStructureStrength > 1.0f) o.localStructureStrength = 1.0f;
    if (o.skinStructureStrength < 0.0f) o.skinStructureStrength = 1.0f;
    return o;
}

constexpr int kMaxPasses = 4;

// ---- colour stage settings (colorstage.h) ----
struct ColorStageParams {          // root constants, 52 dwords, mirrored by cbuffer C in shaders.h
    float fixedDivisor = 0.0f;     // > 0: codec divisor from the units; 0: read the 1x1 scale texel
    float chromaClamp = 1.0f;      // Display codec: bound on the chroma part of the edit, in stops
    float lumaClamp = 2.0f;        // Display codec: bound on the luma part of the edit, in stops
    float colorStrength = 1.0f;    // 0 = luminance only, 1 = the model's colour
    uint32_t w = 0, h = 0;         // size of the DLSS output rectangle
    uint32_t flags = 0;
    float dt = 1.0f / 60.0f;       // seconds since the previous evaluate
    float attack = 4.0f;           // stops per second the measured divisor may rise
    float release = 0.5f;          // stops per second the measured divisor may fall
    float snap = 0.0f;             // != 0: adopt the candidate divisor at once
    float alpha = 0.15f;           // weight of a new edit in the temporal filter (1 = no filtering)
    float motionScaleX = 0.0f, motionScaleY = 0.0f;   // motion vector -> working pixels
    uint32_t motionBaseX = 0, motionBaseY = 0;        // motion vector subrect origin
    uint32_t motionGridX = 1, motionGridY = 1;        // motion vector subrect size
    uint32_t encoding = 1;         // 0 SDR code values, 1 linear, 2 PQ, 3 sRGB in a float buffer
    uint32_t curve = 1;            // 1 Display, 2 divisor family with the neural floor guard, 0 without
    float transferStrength = 1.0f; // divisor family: 0 = game frame, 1 = full edit; also scales the pedestal
    uint32_t transferMode = 0;     // Display codec: 0 bounded ratio, 1 consistent
    float darkGate = 0.005f;       // pedestal: block maximum under which a block counts as dark
    float pedestalCap = 0.01f;     // pedestal: largest lift removed
    float pedestalGate = 0.0f;     // pedestal: 0 off, 1 on
    uint32_t darkGateMode = 0;     // pedestal: 2 = smooth gate
    float preExposure = 1.0f;      // exposure feed: the game's pre-exposure
    float exposureScale = 1.0f;    // exposure feed: the game's exposure scale
    uint32_t exposureSource = 0;   // exposure feed: 0 none (measured divisor), 1 texture, 2 fixed
    uint32_t exposureGuard = 0;    // exposure feed: fall back to the measured divisor when out of range
    uint32_t nrW = 0, nrH = 0;     // working resolution of the model
    uint32_t srcBaseX = 0, srcBaseY = 0;   // origin of the DLSS output rectangle in the game's texture
    uint32_t panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    float panelWhite = 0.8f;
    // Look: gains on the model's edit, in stops, after RenoDX's look stage. 1 = the model's own edit.
    float lookStrength = 1.0f;     // the whole edit
    float lookBrighten = 1.0f;     // the part that brightens
    float lookDarken = 1.0f;       // the part that darkens
    float lookColour = 1.0f;       // colour change toward or away from grey
    float lookHue = 1.0f;          // colour change across the hue
    float lookShadows = 1.0f, lookMidtones = 1.0f, lookHighlights = 1.0f;   // by the input's brightness
    float lookMaxBrighten = 0.0f;  // soft limits in stops, 0 = none
    float lookMaxDarken = 0.0f;
    float lookMaxColour = 0.0f;
    uint32_t pad[2] = {0, 0};
    enum : uint32_t { kSkipModel = 1, kTemporal = 2, kMotion = 4, kHistoryValid = 8, kGateInInputUnits = 16, kShaped = 32, kLook = 64 };
    bool lookNeutral() const {
        return lookStrength == 1.0f && lookBrighten == 1.0f && lookDarken == 1.0f && lookColour == 1.0f && lookHue == 1.0f && lookShadows == 1.0f
            && lookMidtones == 1.0f && lookHighlights == 1.0f && lookMaxBrighten == 0.0f && lookMaxDarken == 0.0f && lookMaxColour == 0.0f;
    }
};
static_assert(sizeof(ColorStageParams) == 52 * 4, "ColorStageParams must stay 52 dwords");

// How to treat the frame; resolved per frame against the format and the game's DLSS flags.
struct ColorConfig {
    int codecMode = 2;             // 0 classic, 1 anchored, 2 auto, 3 display
    int sourceEncoding = 0;        // 0 auto, 1 SDR in a float buffer, 2 linear, 3 PQ
    float linearUnitNits = 0.0f;   // > 0: a linear source is in absolute units of this many nits
    float proxyAnchor = 4.0f;
    float paperWhiteScale = 2.5375f;
    float diffuseWhiteNits = 203.0f;
    float pqCalibration = 1.0f;
    bool neuralFloorGuard = true;
    int displayPedestal = 0;       // 0 auto, 1 on, 2 smooth auto
    int sdrBlackRestore = 0;       // 0 auto, 1 every block, 2 smooth
    int feedMode = 0;              // 0 auto, 1 measured only, 2 the game's exposure
};

// What the game told DLSS, as the colour stage needs it (ngxframe.h fills it).
struct ModelFrame {
    ID3D12Resource* motion = nullptr;
    float mvScaleX = 0, mvScaleY = 0;
    uint32_t mvBaseX = 0, mvBaseY = 0, mvW = 0, mvH = 0;
    bool reset = false;
    bool valid = false;
    bool flagsKnown = false;
    uint32_t createFlags = 0;      // NVSDK_NGX_DLSS_Feature_Flags of the game's own DLSS feature
    float preExposure = 1.0f, exposureScale = 1.0f;
};

// ---- settings (settings.cpp) ----
struct Settings {
    bool enabled = true;
    int toggleKey = VK_F6, reloadKey = VK_F7, menuKey = VK_F8;
    DLSSNROptions opts;
    bool colorStage = true;        // false: strict native in-place call, no conditioning
    bool inPlace = true;           // write straight into the game's DLSS output when the format allows
    ColorStageParams cs;
    ColorConfig cfg;
    int detailStability = 0;       // 0 auto (on when the game supplies motion vectors), 1 off, 2 always
    float resolutionScale = 1.0f;  // working resolution of the model, fraction of the DLSS output
    bool followRenderRes = false;  // working resolution = the game's render resolution
    int passes = 1;                // the model run again on its own result
    float passIntensity[kMaxPasses - 1] = {1.0f, 1.0f, 1.0f};
    bool skipModel = false;        // diagnostics: run the conditioning without the model's edit
    bool dumpNgx = false;
};
extern Settings g;
extern std::atomic<uint32_t> g_settingsGeneration;   // bumped whenever a value changes
void loadSettings();                                  // creates the ini with every key on first run
void saveSetting(const wchar_t* key);                 // persists one key after a change from the menu
bool settingsChangedOnDisk();                         // the file was edited by someone else since we last read or wrote it

// ---- log (main.cpp) ----
extern wchar_t g_dir[MAX_PATH];
void logf(const char* fmt, ...);
// Formatting into a fixed buffer, truncated when it does not fit. The CRT's own "_s" functions end the
// process in that case, which must never happen inside someone's game.
template <size_t N> void fill(char (&out)[N], const char* format, ...) { va_list a; va_start(a, format); _vsnprintf_s(out, N, _TRUNCATE, format, a); va_end(a); }
template <size_t N> void fill(wchar_t (&out)[N], const wchar_t* format, ...) { va_list a; va_start(a, format); _vsnwprintf_s(out, N, _TRUNCATE, format, a); va_end(a); }
inline void filln(wchar_t* out, size_t n, const wchar_t* format, ...) { va_list a; va_start(a, format); _vsnwprintf_s(out, n, _TRUNCATE, format, a); va_end(a); }

// ---- a game with DLSS 5 of its own on a GPU older than RTX 50 (gate.cpp) ----
void installGateHooks();
void presentWatchStart();      // present.cpp: notes the command queue of every swap chain the game creates
bool insideOwnCall();          // this thread is inside one of our own NGX calls: the gate stands aside

// ---- the NGX hooks and the model (ngx.cpp) ----
extern bool g_hostNative;          // the game runs DLSS 5 itself: nothing is added
void onModuleLoaded(HMODULE h, const wchar_t* baseName, size_t baseLen);

} // namespace d5
