// dlss5native.ini: one table drives the defaults, the file written on first run, loading and saving.
#include "common.h"

namespace d5 {

Settings g;
std::atomic<uint32_t> g_settingsGeneration{1};

namespace {

enum Type { kBool, kInt, kUInt, kFloat };
struct Key { const wchar_t* name; Type type; void* ptr; const char* help; };
#define D5_GROUP(text) {nullptr, kInt, nullptr, text}

const Key kKeys[] = {
    D5_GROUP("General. Keys are virtual-key codes (117 = F6, 118 = F7, 119 = F8)."),
    {L"Enabled", kBool, &g.enabled, "DLSS 5 on at start"},
    {L"ToggleKey", kInt, &g.toggleKey, "switches DLSS 5 on and off"},
    {L"ReloadKey", kInt, &g.reloadKey, "reloads this file while the game runs"},
    {L"MenuKey", kInt, &g.menuKey, "opens the in-game panel (arrows to move and change)"},

    D5_GROUP("NVIDIA DLSS 5 model options, passed to the official plugin as a native game would."),
    {L"Intensity", kFloat, &g.opts.intensity, "0..2, strength of the whole effect"},
    {L"LocalToneStrength", kFloat, &g.opts.localToneStrength, "0..2"},
    {L"LocalStructureStrength", kFloat, &g.opts.localStructureStrength, "0..2"},
    {L"GlobalToneStrength", kFloat, &g.opts.globalToneStrength, "0..1"},
    {L"SkinStructureStrength", kFloat, &g.opts.skinStructureStrength, "0..1, read by the model only with UseAutoMask=1; below 0 = stock"},
    {L"Style", kUInt, &g.opts.style, "0 default, 1 natural, 2 cinematic"},
    {L"Preset", kUInt, &g.opts.preset, "0 = default model"},
    {L"UseAutoMask", kBool, &g.opts.useAutoMask, ""},

    D5_GROUP("Model working resolution and stacking."),
    {L"ResolutionScale", kFloat, &g.resolutionScale, "0.33..1: the model runs at this fraction of the DLSS output (1 = full quality, lower = faster)"},
    {L"FollowRenderResolution", kBool, &g.followRenderRes, "1: the model runs at the game's render resolution instead"},
    {L"Passes", kInt, &g.passes, "1..4: the model applied again on its own result (each pass costs one evaluate)"},
    {L"Pass2Intensity", kFloat, &g.passIntensity[0], ""},
    {L"Pass3Intensity", kFloat, &g.passIntensity[1], ""},
    {L"Pass4Intensity", kFloat, &g.passIntensity[2], ""},

    D5_GROUP("Colour stage: conversion between the game's frame and the image the model expects.\n; ColorStage=0 calls the plugin strictly in place on the game's frame, with no conversion."),
    {L"ColorStage", kBool, &g.colorStage, ""},
    {L"InPlace", kBool, &g.inPlace, "read and write the game's texture directly when its format allows (0 = always copy)"},
    {L"DetailStability", kInt, &g.detailStability, "temporal filter on the model's edit: 0 auto (on with motion vectors), 1 off, 2 on"},
    {L"DetailStabilityAlpha", kFloat, &g.cs.alpha, "weight of each new frame in that filter"},
    {L"ColorStrength", kFloat, &g.cs.colorStrength, "0 = luminance only, 1 = the model's colour"},
    D5_GROUP("Look: gains on what the model changed (same controls as RenoDX's look stage). 1 = the model's own edit, 0..2."),
    {L"LookStrength", kFloat, &g.cs.lookStrength, "the whole edit"},
    {L"LookBrightening", kFloat, &g.cs.lookBrighten, "where the model brightens"},
    {L"LookDarkening", kFloat, &g.cs.lookDarken, "where the model darkens"},
    {L"LookColour", kFloat, &g.cs.lookColour, "colour change toward or away from grey"},
    {L"LookHueShift", kFloat, &g.cs.lookHue, "colour change across the hue"},
    {L"LookShadows", kFloat, &g.cs.lookShadows, "the edit in the dark parts of the picture"},
    {L"LookMidtones", kFloat, &g.cs.lookMidtones, ""},
    {L"LookHighlights", kFloat, &g.cs.lookHighlights, ""},
    {L"LookMaxBrightening", kFloat, &g.cs.lookMaxBrighten, "soft limit in stops, 0 = none (0..4)"},
    {L"LookMaxDarkening", kFloat, &g.cs.lookMaxDarken, "soft limit in stops, 0 = none (0..4)"},
    {L"LookMaxColourChange", kFloat, &g.cs.lookMaxColour, "soft limit in stops, 0 = none (0..2)"},
    {L"LumaClampStops", kFloat, &g.cs.lumaClamp, "Display codec: largest brightness change, in stops"},
    {L"ChromaClampStops", kFloat, &g.cs.chromaClamp, "Display codec: largest colour change, in stops"},
    {L"TransferMode", kUInt, &g.cs.transferMode, "Display codec: 0 bounded ratio, 1 consistent with the encode curve"},
    {L"TransferStrength", kFloat, &g.cs.transferStrength, "divisor codecs: 0 = game frame, 1 = full edit"},
    {L"ScaleAttackStops", kFloat, &g.cs.attack, "measured divisor: stops per second upward"},
    {L"ScaleReleaseStops", kFloat, &g.cs.release, "measured divisor: stops per second downward"},
    {L"FixedDivisor", kFloat, &g.cs.fixedDivisor, "relative HDR: > 0 replaces the measured divisor"},

    D5_GROUP("Frame classification. Everything is automatic at 0; override only for a game that is misread."),
    {L"CodecMode", kInt, &g.cfg.codecMode, "0 classic, 1 anchored, 2 auto, 3 display"},
    {L"SourceEncoding", kInt, &g.cfg.sourceEncoding, "0 auto, 1 SDR in a float buffer, 2 linear, 3 PQ"},
    {L"LinearUnitNits", kFloat, &g.cfg.linearUnitNits, "> 0: a linear frame is in absolute units of this many nits (scRGB = 80)"},
    {L"ProxyAnchor", kFloat, &g.cfg.proxyAnchor, ""},
    {L"PaperWhiteScale", kFloat, &g.cfg.paperWhiteScale, ""},
    {L"DiffuseWhiteNits", kFloat, &g.cfg.diffuseWhiteNits, ""},
    {L"PQCalibration", kFloat, &g.cfg.pqCalibration, ""},
    {L"NeuralFloorGuard", kBool, &g.cfg.neuralFloorGuard, ""},
    {L"DisplayPedestal", kInt, &g.cfg.displayPedestal, "0 auto, 1 on, 2 smooth auto"},
    {L"SdrBlackRestore", kInt, &g.cfg.sdrBlackRestore, "0 auto, 1 every block, 2 smooth"},
    {L"ExposureFeed", kInt, &g.cfg.feedMode, "0 auto, 1 measured divisor only, 2 the game's exposure"},

    D5_GROUP("GPUs older than RTX 50, and diagnostics."),
    {L"SkipModel", kBool, &g.skipModel, "runs the conversion without the model's edit (must leave the image unchanged)"},
    {L"DumpNGX", kBool, &g.dumpNgx, "logs every parameter the game hands to DLSS"},
};

void iniPath(wchar_t* out) { filln(out, MAX_PATH, L"%s\\dlss5native.ini", g_dir); }

void format(const Key& k, wchar_t* out, size_t n) {
    switch (k.type) {
        case kBool: filln(out, n, L"%d", *(bool*)k.ptr ? 1 : 0); break;
        case kInt: filln(out, n, L"%d", *(int*)k.ptr); break;
        case kUInt: filln(out, n, L"%u", *(uint32_t*)k.ptr); break;
        case kFloat: { filln(out, n, L"%.4f", *(float*)k.ptr);
                       wchar_t* e = out + wcslen(out) - 1; while (e > out && *e == L'0' && e[-1] != L'.') *e-- = 0; } break;
    }
}

void writeDefaults(const wchar_t* path) {
    FILE* f = nullptr; _wfopen_s(&f, path, L"w");
    if (!f) return;
    fputs("; dlss5native - official NVIDIA DLSS 5 plugin in Streamline games that do not ship it.\n"
          "; Single-player use only: this is a DLL loaded into the game, which anti-cheat systems reject.\n\n[DLSS5]\n", f);
    for (const Key& k : kKeys) {
        if (!k.name) { fprintf(f, "\n; %s\n", k.help); continue; }
        wchar_t v[64]; format(k, v, 64);
        if (k.help[0]) fprintf(f, "; %s\n", k.help);
        fprintf(f, "%ls=%ls\n", k.name, v);
    }
    fclose(f);
}

// Last-write time of the file as we last read or wrote it, to notice an edit made outside the game.
FILETIME g_stamp{}, g_pending{};
FILETIME fileStamp() {
    wchar_t ini[MAX_PATH]; iniPath(ini);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    return GetFileAttributesExW(ini, GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}

// Accepted range of every value that has one. A value outside is brought back inside; a value that is
// not a number at all leaves the built-in default in place.
struct Range { const void* ptr; double lo, hi; };
const Range kRanges[] = {
    {&g.toggleKey, 0, 255}, {&g.reloadKey, 0, 255}, {&g.menuKey, 0, 255},
    {&g.opts.intensity, 0, 2}, {&g.opts.localToneStrength, 0, 2}, {&g.opts.localStructureStrength, 0, 2}, {&g.opts.style, 0, 2}, {&g.opts.globalToneStrength, 0, 1},
    {&g.opts.skinStructureStrength, -1, 1},
    {&g.resolutionScale, 0.33, 1}, {&g.passes, 1, kMaxPasses}, {&g.passIntensity[0], 0, 1}, {&g.passIntensity[1], 0, 1}, {&g.passIntensity[2], 0, 1},
    {&g.cs.lookStrength, 0, 2}, {&g.cs.lookBrighten, 0, 2}, {&g.cs.lookDarken, 0, 2}, {&g.cs.lookColour, 0, 2}, {&g.cs.lookHue, 0, 2},
    {&g.cs.lookShadows, 0, 2}, {&g.cs.lookMidtones, 0, 2}, {&g.cs.lookHighlights, 0, 2},
    {&g.cs.lookMaxBrighten, 0, 4}, {&g.cs.lookMaxDarken, 0, 4}, {&g.cs.lookMaxColour, 0, 2},
    {&g.detailStability, 0, 2}, {&g.cs.alpha, 0.01, 1}, {&g.cs.colorStrength, 0, 1}, {&g.cs.lumaClamp, 0, 8}, {&g.cs.chromaClamp, 0, 8},
    {&g.cs.transferMode, 0, 1}, {&g.cs.transferStrength, 0, 1}, {&g.cs.attack, 0, 1000}, {&g.cs.release, 0, 1000}, {&g.cs.fixedDivisor, 0, 1e6},
    {&g.cfg.codecMode, 0, 3}, {&g.cfg.sourceEncoding, 0, 3}, {&g.cfg.linearUnitNits, 0, 10000}, {&g.cfg.proxyAnchor, 0.01, 1000},
    {&g.cfg.paperWhiteScale, 0.01, 100}, {&g.cfg.diffuseWhiteNits, 1, 10000}, {&g.cfg.pqCalibration, 0.01, 100},
    {&g.cfg.displayPedestal, 0, 2}, {&g.cfg.sdrBlackRestore, 0, 2}, {&g.cfg.feedMode, 0, 2},
};
double bounded(const void* ptr, double v) {
    for (const Range& r : kRanges) if (r.ptr == ptr) return v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
    return v;
}
// Reads one value. False when the text is not a number (or not a finite one).
bool parse(const Key& k, const wchar_t* text) {
    wchar_t* end = nullptr;
    switch (k.type) {
        case kBool: { const long v = wcstol(text, &end, 10); if (end == text) return false; *(bool*)k.ptr = v != 0; return true; }
        case kInt: { const long v = wcstol(text, &end, 10); if (end == text) return false; *(int*)k.ptr = (int)bounded(k.ptr, (double)v); return true; }
        case kUInt: { const unsigned long v = wcstoul(text, &end, 10); if (end == text) return false; *(uint32_t*)k.ptr = (uint32_t)bounded(k.ptr, (double)v); return true; }
        case kFloat: { const double v = wcstod(text, &end); if (end == text || !(v == v) || v > 3.0e38 || v < -3.0e38) return false; *(float*)k.ptr = (float)bounded(k.ptr, v); return true; }
    }
    return false;
}

} // namespace

void loadSettings() {
    wchar_t ini[MAX_PATH]; iniPath(ini);
    g = Settings();
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) writeDefaults(ini);
    for (const Key& k : kKeys) {
        if (!k.name) continue;
        wchar_t b[64]; GetPrivateProfileStringW(L"DLSS5", k.name, L"", b, 64, ini);
        if (!b[0]) continue;       // a key the file does not have keeps the built-in default
        if (!parse(k, b)) logf("settings: %ls=%ls is not a number, the default is kept", k.name, b);
    }
    g.cs.flags = g.skipModel ? ColorStageParams::kSkipModel : 0u;
    g_settingsGeneration++;
    g_stamp = g_pending = fileStamp();
    logf("settings: enabled=%d intensity=%.2f localTone=%.2f localStructure=%.2f globalTone=%.2f skin=%.2f style=%u preset=%u autoMask=%d",
         g.enabled, g.opts.intensity, g.opts.localToneStrength, g.opts.localStructureStrength, g.opts.globalToneStrength,
         g.opts.skinStructureStrength, g.opts.style, g.opts.preset, (int)g.opts.useAutoMask);
    logf("settings: %s, resolution %s%.0f%%, passes=%d, detail stability=%d (alpha %.2f), colour=%.2f, clamps luma %.1f chroma %.1f, skipModel=%d",
         g.colorStage ? "colour stage" : "strict native in-place", g.followRenderRes ? "render / " : "", g.resolutionScale * 100.0f, g.passes,
         g.detailStability, g.cs.alpha, g.cs.colorStrength, g.cs.lumaClamp, g.cs.chromaClamp, (int)g.skipModel);
}

// True once the file's time differs from ours and has stopped moving between two calls (an editor may
// still be writing the first time the change is seen).
bool settingsChangedOnDisk() {
    const FILETIME now = fileStamp();
    if (!now.dwLowDateTime && !now.dwHighDateTime) return false;
    if (CompareFileTime(&now, &g_stamp) == 0) return false;
    const bool settled = CompareFileTime(&now, &g_pending) == 0;
    g_pending = now;
    return settled;
}

void saveSetting(const wchar_t* key) {
    wchar_t ini[MAX_PATH]; iniPath(ini);
    for (const Key& k : kKeys) {
        if (!k.name || wcscmp(k.name, key) != 0) continue;
        wchar_t v[64]; format(k, v, 64);
        WritePrivateProfileStringW(L"DLSS5", k.name, v, ini);
        break;
    }
    g_stamp = g_pending = fileStamp();      // our own write is not an outside edit
    g.cs.flags = g.skipModel ? ColorStageParams::kSkipModel : 0u;
    g_settingsGeneration++;
}

} // namespace d5
