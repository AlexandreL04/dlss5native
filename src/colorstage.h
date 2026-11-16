// Range conditioning around the DLSS-NR evaluate, for games that were not prepared for DLSS 5.
//
// The model is trained on a display-referred, sRGB-encoded [0,1] image. A game hands whatever its DLSS
// output is: scene-linear HDR (relative or absolute units), PQ, or SDR code values. The frame is
// classified, converted to a proxy for the model, and the model's edit is carried back to the frame:
//
//   classify   SDR / relative linear HDR / absolute linear HDR / PQ, from the texture format, the
//              game's DLSS create flags and the settings;
//   scale      relative HDR: a one-sided divisor measured on the frame (it only reduces an over-range
//              frame, never brightens), or the game's own exposure when it supplies one; absolute HDR:
//              a fixed divisor from the units;
//   encode     frame / divisor, shadow toe (Display codec), hue-preserving shoulder, sRGB encode, at the
//              model's working resolution (area-filtered when that is below the frame's);
//   model      NVIDIA's plugin, proxy in -> neural out;
//   stabilize  the edit (log2 neural/proxy, luma + opponent chroma) is filtered over time: reprojected
//              with the game's motion vectors, clipped to the 3x3 neighbourhood, rejected on change;
//   transport  below the frame's resolution the edit is brought up with joint-bilateral weights;
//   resolve    Display codec: bounded log-ratio gain on the untouched frame; divisor family: luminance
//              upgrade with the neural hue; SDR: luminance ratio on code values;
//   pedestal   optional removal of a black-level lift, measured on 32x32 blocks;
//   commit     back to the game's own encoding, into the DLSS output rectangle.
//
// An unchanged model output is an exact no-op on the Display codec, black stays black and HDR highlights
// keep their value. When the game's texture allows it everything is read and written in place, with no
// copy of the frame; otherwise the rectangle is copied out and back.
//
// before() runs ahead of the model, after() right behind it on the same command list.
#pragma once
#include "common.h"

namespace d5 {

// The settings panel, drawn into the frame by the last pass (menu.cpp owns the texture).
struct Panel { ID3D12Resource* tex = nullptr; UINT x = 0, y = 0, w = 0, h = 0; float white = 0.8f; };

struct ColorStage {
    struct Frame {
        ID3D12Resource* gameOut = nullptr; D3D12_RESOURCE_STATES gameState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_BOX rect{};                       // the DLSS output rectangle inside gameOut
        ID3D12Resource* exposure = nullptr; D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_COMMON;
        UINT nrW = 0, nrH = 0;                  // working resolution of the model
        int pass = 0;                           // > 0: the model run again on its own result
    };
    static constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12Resource* pin = nullptr;      // proxy handed to the model
    ID3D12Resource* pout = nullptr;     // model output
    UINT nrW = 0, nrH = 0;

    bool before(ID3D12GraphicsCommandList* cl, const Frame& f, const ColorStageParams& settings, const ColorConfig& cfg,
                const ModelFrame& known, bool allowInPlace);
    void after(ID3D12GraphicsCommandList* cl, const ModelFrame& mf, D3D12_RESOURCE_STATES motionState, bool temporal, const Panel* panel);
    void abandon(ID3D12GraphicsCommandList* cl);
    // The panel alone, while the model is switched off. Needs a frame that went through before() once.
    bool overlayOnly(ID3D12GraphicsCommandList* cl, const Frame& f, const Panel& panel);
    const char* describe() const { return description; }
    // Can these motion vectors be bound as they are (2D, one sample, a float format)?
    static bool motionUsable(ID3D12Resource* motion);
    // Drops every GPU object at once. Only for a stage whose command lists have long been executed.
    void releaseAll();

private:
    enum { kMeasure, kExposure, kEncode, kShape, kResolve, kResolveUp, kReduce, kCommit, kOverlay, kPipelines };
    enum { kRing = 64, kSrvs = 11, kUavs = 6, kSets = 6, kSlots = kSrvs + kSets * kUavs, kConstants = 52 };
    enum { kSetEncode = 0, kSetResolve, kSetReduce, kSetCommit, kSetShape, kSetOverlay };
    struct Shared {                     // one device, one root signature, pipelines built on first use
        ID3D12Device* dev = nullptr; ID3D12RootSignature* rs = nullptr;
        ID3D12PipelineState* pso[2][kPipelines] = {};
        bool failed = false;
        uint32_t generation = 0;        // counts the devices seen: what was built for another one is dropped
    };
    static Shared& shared() { static Shared s; return s; }

    ID3D12DescriptorHeap* heap = nullptr; UINT dsz = 0; UINT ring = 0;
    ID3D12Resource* hdr = nullptr;      // copy path: the DLSS output rectangle, game encoding
    ID3D12Resource* fin = nullptr;      // copy path: final frame, game encoding
    ID3D12Resource* guide = nullptr; ID3D12Resource* shaped = nullptr;
    ID3D12Resource* resolved = nullptr; ID3D12Resource* blocks = nullptr;
    ID3D12Resource* scale = nullptr; ID3D12Resource* commit = nullptr;
    ID3D12Resource* history[kMaxPasses][2] = {};
    int historyWritten[kMaxPasses] = {};
    bool historyValid[kMaxPasses] = {};
    UINT width = 0, height = 0; DXGI_FORMAT gameFormat = DXGI_FORMAT_UNKNOWN;
    bool inPlace = false, pedestal = false, reduced = false, ready = false;
    DXGI_FORMAT supportFormat = DXGI_FORMAT_UNKNOWN; bool supportOk = false;
    int lastFeed = -1, lastClass = -1;
    char description[160] = "";
    ColorStageParams plan;              // this frame's constants
    Frame cur;
    std::vector<std::pair<ID3D12Resource*, uint64_t>> retired; uint64_t tick = 0;
    uint32_t builtFor = 0;              // Shared::generation the objects above belong to

    static DXGI_FORMAT resourceFormat(DXGI_FORMAT f);
    static DXGI_FORMAT viewFormat(DXGI_FORMAT f);
    static DXGI_FORMAT scalarViewFormat(DXGI_FORMAT f);
    static bool exposureReadable(const D3D12_RESOURCE_DESC& d);
    static void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b);
    static void uavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r);
    bool init(ID3D12GraphicsCommandList* cl);
    ID3D12PipelineState* pipeline(int which);
    ID3D12Resource* makeTex(UINT64 w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st);
    void retire(ID3D12Resource*& r);
    bool ensure(const D3D12_RESOURCE_DESC& game, UINT w, UINT h, UINT nw, UINT nh, bool allowInPlace);
    bool want(ID3D12Resource*& r, UINT64 w, UINT h, DXGI_FORMAT f);
    void classify(const ColorConfig& cfg, const ModelFrame& mf, bool haveExposure, bool exposureIsReadable);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i);
    void srv(ID3D12Resource* r, DXGI_FORMAT f, UINT slot);
    void uav(ID3D12Resource* r, DXGI_FORMAT f, UINT slot);
    void uavSet(int set, ID3D12Resource* target, DXGI_FORMAT f);
    void bindFrame(const Panel* panel);
    void run(ID3D12GraphicsCommandList* cl, int which, int set, UINT gx, UINT gy);
    void drawPanel(ID3D12GraphicsCommandList* cl, const Panel& panel);
};

} // namespace d5
