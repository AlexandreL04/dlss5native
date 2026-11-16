// The two parameter blocks of the method, as plain functions so they can be tested without a game:
//   - what the game handed to DLSS (read at its NGX create and at every NGX evaluate);
//   - what the DLSS 5 model is handed (the same keys NVIDIA's own plugin sets for it).
#pragma once
#include "common.h"

namespace d5 {

// NVSDK_NGX_DLSS_Feature_Flags
constexpr uint32_t kDlssIsHDR = 1u, kDlssMVLowRes = 2u, kDlssDepthInverted = 8u;

// The game's DLSS feature, from the parameters of its create.
struct GameFeature {
    bool flagsKnown = false; uint32_t createFlags = 0;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
};

// One DLSS evaluate of the game.
struct GameFrame {
    ID3D12Resource* output = nullptr; ID3D12Resource* color = nullptr; ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr; ID3D12Resource* exposure = nullptr;
    D3D12_BOX rect{};                       // the DLSS output rectangle inside `output`
    uint32_t renderW = 0, renderH = 0;      // what the game rendered this frame
    uint32_t depthX = 0, depthY = 0;
    uint32_t motionX = 0, motionY = 0, motionW = 0, motionH = 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    float jitterX = 0.0f, jitterY = 0.0f; bool hasJitter = false;
    bool reset = false;
    float preExposure = 1.0f, exposureScale = 1.0f;
};

struct TexSize { uint32_t w = 0, h = 0; };
using TexSizeFn = TexSize (*)(ID3D12Resource*);

inline uint32_t ngxUInt(const NVSDK_NGX_Parameter* p, const char* name, uint32_t fallback = 0) {
    unsigned int v = 0; return p->Get(name, &v) == NVSDK_NGX_Result_Success ? v : fallback;
}
inline bool ngxFloat(const NVSDK_NGX_Parameter* p, const char* name, float& out) {
    float v = 0; if (p->Get(name, &v) != NVSDK_NGX_Result_Success) return false; out = v; return true;
}
inline ID3D12Resource* ngxResource(const NVSDK_NGX_Parameter* p, const char* name) {
    ID3D12Resource* r = nullptr; return p->Get(name, &r) == NVSDK_NGX_Result_Success ? r : nullptr;
}

inline void readGameFeature(const NVSDK_NGX_Parameter* p, GameFeature& f) {
    unsigned int flags = 0;
    f.flagsKnown = p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success;
    f.createFlags = f.flagsKnown ? flags : 0;
    f.renderW = ngxUInt(p, "Width"); f.renderH = ngxUInt(p, "Height");
    f.outW = ngxUInt(p, "OutWidth"); f.outH = ngxUInt(p, "OutHeight");
}

// False when this evaluate does not carry what the model needs (an output, depth and motion vectors).
inline bool readGameFrame(const NVSDK_NGX_Parameter* p, const GameFeature& f, TexSizeFn sizeOf, GameFrame& g) {
    g = GameFrame();
    g.output = ngxResource(p, "Output"); g.color = ngxResource(p, "Color");
    g.depth = ngxResource(p, "Depth"); g.motion = ngxResource(p, "MotionVectors"); g.exposure = ngxResource(p, "ExposureTexture");
    if (!g.output || !g.depth || !g.motion) return false;
    const TexSize out = sizeOf(g.output), depth = sizeOf(g.depth), motion = sizeOf(g.motion);
    if (!out.w || !out.h || !depth.w || !depth.h || !motion.w || !motion.h) return false;

    // the output rectangle: the size the feature was created for, at the origin the game gives
    const uint32_t ox = ngxUInt(p, "DLSS.Output.Subrect.Base.X"), oy = ngxUInt(p, "DLSS.Output.Subrect.Base.Y");
    if (ox >= out.w || oy >= out.h) return false;
    uint32_t ow = f.outW ? f.outW : out.w - ox, oh = f.outH ? f.outH : out.h - oy;
    if (ow > out.w - ox) ow = out.w - ox;
    if (oh > out.h - oy) oh = out.h - oy;
    g.rect = D3D12_BOX{ox, oy, 0, ox + ow, oy + oh, 1};

    // what was rendered this frame: the evaluate's own rectangle (dynamic resolution), else the create's
    g.renderW = ngxUInt(p, "DLSS.Render.Subrect.Dimensions.Width"); g.renderH = ngxUInt(p, "DLSS.Render.Subrect.Dimensions.Height");
    if (!g.renderW || !g.renderH) { g.renderW = f.renderW; g.renderH = f.renderH; }
    if (!g.renderW || !g.renderH) { g.renderW = depth.w; g.renderH = depth.h; }

    g.depthX = ngxUInt(p, "DLSS.Input.Depth.Subrect.Base.X"); g.depthY = ngxUInt(p, "DLSS.Input.Depth.Subrect.Base.Y");
    g.motionX = ngxUInt(p, "DLSS.Input.MV.Subrect.Base.X"); g.motionY = ngxUInt(p, "DLSS.Input.MV.Subrect.Base.Y");
    if (g.depthX >= depth.w || g.depthY >= depth.h || g.motionX >= motion.w || g.motionY >= motion.h) return false;
    if (g.renderW > depth.w - g.depthX) g.renderW = depth.w - g.depthX;
    if (g.renderH > depth.h - g.depthY) g.renderH = depth.h - g.depthY;
    // motion vectors are at the render resolution or at the output resolution
    const bool lowRes = f.flagsKnown ? (f.createFlags & kDlssMVLowRes) != 0 : motion.w < ow;
    g.motionW = lowRes ? g.renderW : ow; g.motionH = lowRes ? g.renderH : oh;
    if (g.motionW > motion.w - g.motionX) g.motionW = motion.w - g.motionX;
    if (g.motionH > motion.h - g.motionY) g.motionH = motion.h - g.motionY;

    ngxFloat(p, "MV.Scale.X", g.mvScaleX); ngxFloat(p, "MV.Scale.Y", g.mvScaleY);
    const bool jx = ngxFloat(p, "Jitter.Offset.X", g.jitterX), jy = ngxFloat(p, "Jitter.Offset.Y", g.jitterY);
    g.hasJitter = jx && jy;
    int reset = 0; g.reset = p->Get("Reset", &reset) == NVSDK_NGX_Result_Success && reset != 0;
    float e = 0;
    if (ngxFloat(p, "DLSS.Pre.Exposure", e) && e > 0.0f) g.preExposure = e;
    if (ngxFloat(p, "DLSS.Exposure.Scale", e) && e > 0.0f) g.exposureScale = e;
    return true;
}

// The same frame as the colour stage wants it.
inline ModelFrame toModelFrame(const GameFrame& g, const GameFeature& f) {
    ModelFrame m;
    m.motion = g.motion; m.mvScaleX = g.mvScaleX; m.mvScaleY = g.mvScaleY;
    m.mvBaseX = g.motionX; m.mvBaseY = g.motionY; m.mvW = g.motionW; m.mvH = g.motionH;
    m.reset = g.reset; m.valid = true; m.flagsKnown = f.flagsKnown; m.createFlags = f.createFlags;
    m.preExposure = g.preExposure; m.exposureScale = g.exposureScale;
    return m;
}

// The model's feature is created for one size: it reads the proxy and writes its result at the working
// resolution. `ratioCallback` answers the model's question about the scaling ratio (always 1).
inline void writeModelCreate(NVSDK_NGX_Parameter* p, uint32_t w, uint32_t h, uint32_t gameFlags, uint32_t preset, void* ratioCallback) {
    p->Set("Width", (unsigned int)w); p->Set("Height", (unsigned int)h);
    p->Set("OutWidth", (unsigned int)w); p->Set("OutHeight", (unsigned int)h);
    p->Set("DLSSNR.Width", (unsigned int)w); p->Set("DLSSNR.Height", (unsigned int)h);
    p->Set("DLSSNR.InputWidth", (unsigned int)w); p->Set("DLSSNR.InputHeight", (unsigned int)h);
    p->Set("DLSSNR.OutputWidth", (unsigned int)w); p->Set("DLSSNR.OutputHeight", (unsigned int)h);
    p->Set("DLSSNR.Output.Width", (unsigned int)w); p->Set("DLSSNR.Output.Height", (unsigned int)h);
    p->Set("DLSSNR.ScalingRatio", 1.0f); p->Set("DLSSNR.Scale", 1.0f);
    p->Set("DLSSNRComputeScalingRatioCallback", ratioCallback);
    p->Set("DLSSNR.Hint.Render.Preset", (unsigned int)preset);
    p->Set("PerfQualityValue", (int)0);
    // The game's own flags, as the model gets them from NVIDIA's plugin (it shares the game's block).
    p->Set("DLSS.Feature.Create.Flags", (int)gameFlags);
    p->Set("DLSS.Pre.Exposure", 1.0f); p->Set("DLSS.Exposure.Scale", 1.0f);
    p->Set("CreationNodeMask", (unsigned int)1); p->Set("VisibilityNodeMask", (unsigned int)1);
}

inline void writeModelEvaluate(NVSDK_NGX_Parameter* p, ID3D12Resource* proxy, ID3D12Resource* result, uint32_t w, uint32_t h,
                               const GameFrame& g, const GameFeature& f, const DLSSNROptions& o, bool reset, void* ratioCallback) {
    p->Set("DLSSNR.Color", proxy); p->Set("DLSSNR.Output", result);
    p->Set("DLSSNR.MVec", g.motion); p->Set("DLSSNR.Depth", g.depth);
    p->Set("Width", (unsigned int)w); p->Set("Height", (unsigned int)h);
    p->Set("OutWidth", (unsigned int)w); p->Set("OutHeight", (unsigned int)h);
    p->Set("DLSSNR.Width", (unsigned int)w); p->Set("DLSSNR.Height", (unsigned int)h);
    p->Set("DLSSNR.InputWidth", (unsigned int)w); p->Set("DLSSNR.InputHeight", (unsigned int)h);
    p->Set("DLSSNR.OutputWidth", (unsigned int)w); p->Set("DLSSNR.OutputHeight", (unsigned int)h);
    p->Set("DLSSNR.ColorSubrectBaseX", (unsigned int)0); p->Set("DLSSNR.ColorSubrectBaseY", (unsigned int)0);
    p->Set("DLSSNR.ColorSubrectWidth", (unsigned int)w); p->Set("DLSSNR.ColorSubrectHeight", (unsigned int)h);
    p->Set("DLSSNR.OutputSubrectBaseX", (unsigned int)0); p->Set("DLSSNR.OutputSubrectBaseY", (unsigned int)0);
    p->Set("DLSSNR.OutputSubrectWidth", (unsigned int)w); p->Set("DLSSNR.OutputSubrectHeight", (unsigned int)h);
    p->Set("DLSSNR.DepthSubrectBaseX", (unsigned int)g.depthX); p->Set("DLSSNR.DepthSubrectBaseY", (unsigned int)g.depthY);
    p->Set("DLSSNR.DepthSubrectWidth", (unsigned int)g.renderW); p->Set("DLSSNR.DepthSubrectHeight", (unsigned int)g.renderH);
    p->Set("DLSSNR.MVecSubrectBaseX", (unsigned int)g.motionX); p->Set("DLSSNR.MVecSubrectBaseY", (unsigned int)g.motionY);
    p->Set("DLSSNR.MVecSubrectWidth", (unsigned int)g.motionW); p->Set("DLSSNR.MVecSubrectHeight", (unsigned int)g.motionH);
    p->Set("DLSSNR.ScalingRatio", 1.0f); p->Set("DLSSNR.Scale", 1.0f);
    p->Set("DLSSNRComputeScalingRatioCallback", ratioCallback);
    p->Set("DLSSNR.MVecScaleX", g.mvScaleX); p->Set("DLSSNR.MVecScaleY", g.mvScaleY);
    p->Set("MV.Scale.X", g.mvScaleX); p->Set("MV.Scale.Y", g.mvScaleY);
    if (g.hasJitter) {
        p->Set("DLSSNR.JitterOffsetX", g.jitterX); p->Set("DLSSNR.JitterOffsetY", g.jitterY);
        p->Set("Jitter.Offset.X", g.jitterX); p->Set("Jitter.Offset.Y", g.jitterY);
    }
    p->Set("DLSSNR.DepthInverted", (int)((f.createFlags & kDlssDepthInverted) ? 1 : 0));
    p->Set("DLSSNR.Reset", (int)(reset ? 1 : 0)); p->Set("Reset", (int)(reset ? 1 : 0));
    p->Set("DLSSNR.Enabled", (int)1);
    p->Set("DLSSNR.UICorrection", (int)0);
    p->Set("DLSSNR.Hint.Render.Preset", (unsigned int)o.preset);
    p->Set("DLSSNR.Intensity", o.intensity);
    p->Set("DLSSNR.LocalToneStrength", o.localToneStrength);
    p->Set("DLSSNR.LocalStructureStrength", o.localStructureStrength);
    p->Set("DLSSNR.GlobalToneStrength", o.globalToneStrength);
    p->Set("DLSSNR.SkinStructureStrength", o.skinStructureStrength);
    p->Set("DLSSNR.Style", (unsigned int)o.style);
    p->Set("DLSSNR.UseAutoMask", (int)(o.useAutoMask ? 1 : 0));
}

// Where a module keeps the address of a function it imports by name, or nullptr. Every offset is checked
// against the size of the mapped image before it is read.
inline void** findImport(HMODULE module, const char* function) {
    if (!module || !function) return nullptr;
    const BYTE* base = (const BYTE*)module;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return nullptr;
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    const size_t wanted = strlen(function);
    for (size_t at = dir.VirtualAddress; at + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= size; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(base + at);
        if (!d->Name) break;
        size_t names = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk, slots = d->FirstThunk;
        if (!names || !slots) continue;
        for (; names + sizeof(IMAGE_THUNK_DATA) <= size && slots + sizeof(IMAGE_THUNK_DATA) <= size; names += sizeof(IMAGE_THUNK_DATA), slots += sizeof(IMAGE_THUNK_DATA)) {
            const IMAGE_THUNK_DATA* n = (const IMAGE_THUNK_DATA*)(base + names);
            if (!n->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL(n->u1.Ordinal)) continue;
            const size_t name = (size_t)n->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (name + wanted + 1 > size) continue;
            if (memcmp(base + name, function, wanted + 1) == 0) return (void**)(base + slots);
        }
    }
    return nullptr;
}

} // namespace d5
