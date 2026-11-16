#include "colorstage.h"
#include "shaders.h"
#include <d3dcompiler.h>

namespace d5 {

namespace {
const DXGI_FORMAT kWork = DXGI_FORMAT_R16G16B16A16_FLOAT;
const DXGI_FORMAT kStat = DXGI_FORMAT_R32G32B32A32_FLOAT;
const D3D12_RESOURCE_FLAGS kUa = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
const char* const kEntry[] = {"Measure", "ExposureScale", "Encode", "Shape", "Resolve", "ResolveUp", "Reduce", "Commit", "Overlay"};
}

// Typed formats that cannot be unordered-access targets (sRGB) are kept typeless and viewed as UNORM,
// so SDR code values pass through unchanged.
DXGI_FORMAT ColorStage::resourceFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        default: return f;
    }
}
DXGI_FORMAT ColorStage::viewFormat(DXGI_FORMAT f) {
    switch (resourceFormat(f)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}
DXGI_FORMAT ColorStage::scalarViewFormat(DXGI_FORMAT f) {   // 1-channel exposure textures, 2-channel motion vectors
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}
bool ColorStage::exposureReadable(const D3D12_RESOURCE_DESC& d) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) return false;
    switch (scalarViewFormat(d.Format)) {
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: return true;
        default: return false;
    }
}

void ColorStage::transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b || !r) return;
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &x);
}
void ColorStage::uavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; x.UAV.pResource = r; cl->ResourceBarrier(1, &x);
}

bool ColorStage::motionUsable(ID3D12Resource* motion) {
    if (!motion) return false;
    const D3D12_RESOURCE_DESC d = motion->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) return false;
    switch (scalarViewFormat(d.Format)) {
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: return true;
        default: return false;
    }
}

void ColorStage::releaseAll() {
    for (ID3D12Resource** r : {&pin, &pout, &guide, &shaped, &resolved, &blocks, &hdr, &fin, &scale, &commit}) if (*r) { (*r)->Release(); *r = nullptr; }
    for (int p = 0; p < kMaxPasses; p++) for (int i = 0; i < 2; i++) if (history[p][i]) { history[p][i]->Release(); history[p][i] = nullptr; }
    for (auto& r : retired) r.first->Release();
    retired.clear();
    if (heap) { heap->Release(); heap = nullptr; }
    for (bool& v : historyValid) v = false;
    width = height = nrW = nrH = 0; gameFormat = DXGI_FORMAT_UNKNOWN; supportFormat = DXGI_FORMAT_UNKNOWN;
    ready = false; cur = Frame();
}

bool ColorStage::init(ID3D12GraphicsCommandList* cl) {
    Shared& s = shared();
    ID3D12Device* dev = nullptr; cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return false;
    if (dev != s.dev) {
        // First use, or the game made a new device: nothing built for the previous one may be used on this one.
        if (s.dev) logf("colour stage: new D3D12 device, everything is rebuilt");
        for (auto& variant : s.pso) for (ID3D12PipelineState*& p : variant) if (p) { p->Release(); p = nullptr; }
        if (s.rs) { s.rs->Release(); s.rs = nullptr; }
        if (s.dev) s.dev->Release();
        s.dev = dev; s.failed = false; s.generation++;      // the reference from GetDevice is kept
    } else dev->Release();
    if (builtFor != s.generation) { releaseAll(); builtFor = s.generation; }
    if (s.failed) return false;
    if (!s.rs) {
        D3D12_DESCRIPTOR_RANGE r[2]{};
        r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[0].NumDescriptors = kSrvs; r[0].BaseShaderRegister = 0;
        r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[1].NumDescriptors = kUavs; r[1].BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[0].DescriptorTable.NumDescriptorRanges = 1; p[0].DescriptorTable.pDescriptorRanges = &r[0];
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &r[1];
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[2].Constants.ShaderRegister = 0; p[2].Constants.Num32BitValues = kConstants;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 3; rd.pParameters = p;
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr)) hr = s.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&s.rs);
        if (blob) blob->Release();
        if (err) err->Release();
        if (FAILED(hr)) { logf("colour stage: root signature failed 0x%lx", hr); s.failed = true; return false; }
    }
    if (!heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kRing * kSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = s.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&heap);
        dsz = s.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        scale = makeTex(1, 1, kStat, kUa, kUav);
        commit = makeTex(1, 1, kStat, kUa, kUav);
        if (FAILED(hr) || !scale || !commit) { s.failed = true; return false; }
    }
    return true;
}

ID3D12PipelineState* ColorStage::pipeline(int which) {
    Shared& s = shared();
    ID3D12PipelineState*& p = s.pso[inPlace ? 1 : 0][which];
    if (p || s.failed) return p;
    const D3D_SHADER_MACRO defines[] = {{"IN_PLACE", inPlace ? "1" : "0"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kColorStageHLSL, strlen(kColorStageHLSL), "colorstage", defines, nullptr, kEntry[which], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        logf("colour stage: compile %s failed 0x%lx: %s", kEntry[which], hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        s.failed = true; return nullptr;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = s.rs; d.CS.pShaderBytecode = code->GetBufferPointer(); d.CS.BytecodeLength = code->GetBufferSize();
    hr = s.dev->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&p);
    code->Release();
    if (err) err->Release();
    if (FAILED(hr)) { logf("colour stage: pipeline %s failed 0x%lx", kEntry[which], hr); s.failed = true; p = nullptr; }
    else logf("colour stage: pipeline %s ready (%s)", kEntry[which], inPlace ? "in place" : "copy");
    return p;
}

ID3D12Resource* ColorStage::makeTex(UINT64 w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1; d.Flags = fl;
    ID3D12Resource* r = nullptr; HRESULT hr = shared().dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource), (void**)&r);
    if (FAILED(hr)) { logf("colour stage: texture %llux%u fmt=%u failed 0x%lx", w, h, (unsigned)f, hr); return nullptr; }
    { wchar_t name[64]; fill(name, L"dlss5native %llux%u format %u (frame %llu)", w, h, (unsigned)f, tick); r->SetName(name); }   // for graphics debuggers
    return r;
}
// A texture a recorded command list may still reference is released a few frames later.
void ColorStage::retire(ID3D12Resource*& r) { if (r) { retired.emplace_back(r, tick); r = nullptr; } }
bool ColorStage::want(ID3D12Resource*& r, UINT64 w, UINT h, DXGI_FORMAT f) {
    if (!r) r = makeTex(w, h, f, kUa, kUav);
    return r != nullptr;
}

bool ColorStage::ensure(const D3D12_RESOURCE_DESC& game, UINT w, UINT h, UINT nw, UINT nh, bool allowInPlace) {
    for (size_t i = 0; i < retired.size();) {
        if (tick - retired[i].second > 64) { retired[i].first->Release(); retired[i] = retired.back(); retired.pop_back(); } else i++;
    }
    const DXGI_FORMAT vf = viewFormat(game.Format);
    bool ip = allowInPlace && (game.Flags & kUa) && (game.Format == vf || game.Format == resourceFormat(vf));
    if (ip) {
        if (supportFormat != vf) {      // reading the frame through its own unordered-access view needs typed loads
            D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{}; fs.Format = vf;
            const UINT need = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
            supportOk = SUCCEEDED(shared().dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support2 & need) == need;
            supportFormat = vf;
        }
        ip = supportOk;
    }
    const bool frameChange = width != w || height != h || gameFormat != game.Format || ip != inPlace;
    const bool gridChange = nrW != nw || nrH != nh;
    if (frameChange) {
        for (ID3D12Resource** r : {&hdr, &fin, &resolved, &blocks}) retire(*r);
        width = w; height = h; gameFormat = game.Format; inPlace = ip;
        logf("colour stage: frame %ux%u format %u, %s", w, h, (unsigned)game.Format, inPlace ? "read and written in place" : "copied out and back");
    }
    if (frameChange || gridChange) {
        for (ID3D12Resource** r : {&pin, &pout, &guide, &shaped}) retire(*r);
        for (int p = 0; p < kMaxPasses; p++) { retire(history[p][0]); retire(history[p][1]); historyValid[p] = false; }
        nrW = nw; nrH = nh;
        if (gridChange) logf("colour stage: model working resolution %ux%u (frame %ux%u)", nw, nh, w, h);
    }
    return want(pin, nrW, nrH, kWork) && want(pout, nrW, nrH, kWork);
}

D3D12_CPU_DESCRIPTOR_HANDLE ColorStage::cpu(UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)(ring * kSlots + i) * dsz; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE ColorStage::gpu(UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)(ring * kSlots + i) * dsz; return h; }
void ColorStage::srv(ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {     // a null resource makes a null view
    D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = f; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
    shared().dev->CreateShaderResourceView(r, &d, cpu(slot));
}
void ColorStage::uav(ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.Format = f; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    shared().dev->CreateUnorderedAccessView(r, nullptr, &d, cpu(slot));
}
// UAV table of one pass: u0 target, u1 scale, u2 history out, u3 commit, u4 the game's texture, u5 guide
void ColorStage::uavSet(int set, ID3D12Resource* target, DXGI_FORMAT f) {
    const UINT base = kSrvs + set * kUavs;
    const int pass = cur.pass;
    uav(target, f, base); uav(scale, kStat, base + 1);
    uav(history[pass][1 - historyWritten[pass]], kWork, base + 2);
    uav(commit, kStat, base + 3);
    uav(inPlace ? cur.gameOut : nullptr, viewFormat(gameFormat), base + 4);
    uav(guide, DXGI_FORMAT_R16_FLOAT, base + 5);
}
// Every view of this frame's block of the ring. Views are read when the list executes, so the block is
// written whole and belongs to one before()/after() pair.
void ColorStage::bindFrame(const Panel* panel) {
    const DXGI_FORMAT vf = viewFormat(gameFormat);
    const int pass = cur.pass;
    srv(hdr, vf, 0); srv(pin, kWork, 1); srv(pout, kWork, 2);
    srv(nullptr, DXGI_FORMAT_R16G16_FLOAT, 3);
    srv(history[pass][historyWritten[pass]], kWork, 4);
    srv(resolved, kWork, 5); srv(blocks, kStat, 6);
    if (plan.exposureSource == 1 && cur.exposure) srv(cur.exposure, scalarViewFormat(cur.exposure->GetDesc().Format), 7); else srv(nullptr, DXGI_FORMAT_R32_FLOAT, 7);
    srv(panel ? panel->tex : nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 8);
    srv(guide, DXGI_FORMAT_R16_FLOAT, 9); srv(shaped, kWork, 10);
    ID3D12Resource* out = inPlace ? nullptr : fin;
    uavSet(kSetEncode, pin, kWork);
    uavSet(kSetResolve, pedestal ? resolved : out, pedestal ? kWork : vf);
    uavSet(kSetReduce, blocks, kStat);
    uavSet(kSetCommit, out, vf);
    uavSet(kSetShape, shaped, kWork);
    uavSet(kSetOverlay, out, vf);
}
void ColorStage::run(ID3D12GraphicsCommandList* cl, int which, int set, UINT gx, UINT gy) {
    ID3D12PipelineState* p = pipeline(which);
    if (!p) return;
    cl->SetDescriptorHeaps(1, &heap);
    cl->SetComputeRootSignature(shared().rs);
    cl->SetComputeRootDescriptorTable(0, gpu(0));
    cl->SetComputeRootDescriptorTable(1, gpu(kSrvs + set * kUavs));
    cl->SetComputeRoot32BitConstants(2, kConstants, &plan, 0);
    cl->SetPipelineState(p);
    cl->Dispatch(gx, gy, 1);
}

// Classify the frame and derive the codec, the divisor and the pedestal.
void ColorStage::classify(const ColorConfig& cfg, const ModelFrame& mf, bool haveExposure, bool exposureIsReadable) {
    const DXGI_FORMAT f = viewFormat(gameFormat);
    const bool isHdrFlag = mf.flagsKnown ? (mf.createFlags & 1u) != 0 : false;     // NVSDK_NGX_DLSS_Feature_Flags_IsHDR
    const bool autoExposure = mf.flagsKnown && (mf.createFlags & 64u) != 0;         // ..._AutoExposure
    int inferred = (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT) ? 1
                 : (f == DXGI_FORMAT_R10G10B10A2_UNORM ? 2 : 0);
    int hdrMode = isHdrFlag ? (inferred > 1 ? inferred : 1) : (f == DXGI_FORMAT_R10G10B10A2_UNORM ? 0 : inferred);
    int choice = cfg.sourceEncoding != 0 ? cfg.sourceEncoding : (hdrMode == 2 ? 3 : hdrMode == 1 ? 2 : 0);
    bool pq = false, sdrInFloat = false, absolute = false; float unitNits = 0.0f;
    if (hdrMode != 0) {
        if (choice == 1) sdrInFloat = true;
        else if (choice == 3) { pq = true; absolute = true; unitNits = 10000.0f; }
        else if (cfg.linearUnitNits > 0.0f) { absolute = true; unitNits = cfg.linearUnitNits; }
    }
    plan.encoding = hdrMode == 0 ? 0u : pq ? 2u : sdrInFloat ? 3u : 1u;
    const bool display = hdrMode != 0 && (cfg.codecMode == 3 || (cfg.codecMode == 2 && !(absolute && unitNits > 0.0f)));
    plan.curve = display ? 1u : (cfg.neuralFloorGuard ? 2u : 0u);

    auto mx = [](float a, float b) { return a > b ? a : b; };
    if (hdrMode == 0) plan.fixedDivisor = 1.0f;
    else if (absolute) {
        if (display) plan.fixedDivisor = pq ? mx(1e-6f, mx(1.0f, cfg.diffuseWhiteNits) / 10000.0f) : mx(1e-6f, mx(0.02f, cfg.proxyAnchor) / unitNits);
        else if (cfg.codecMode == 1) plan.fixedDivisor = mx(1e-8f, mx(0.02f, cfg.proxyAnchor) / (0.75f * unitNits));
        else plan.fixedDivisor = pq ? mx(1e-8f, (mx(1.0f, cfg.diffuseWhiteNits) / 10000.0f) * mx(0.005f, cfg.paperWhiteScale / 2.5375f) * mx(0.005f, cfg.pqCalibration))
                                    : mx(1e-4f, cfg.paperWhiteScale);
    }
    // otherwise the caller's FixedDivisor override (0 = measured) stands

    // The game's own exposure replaces the measured divisor on relative HDR when it is usable.
    int feed = 0;
    if (hdrMode != 0 && !absolute && !sdrInFloat && plan.fixedDivisor <= 0.0f && cfg.feedMode != 1 && haveExposure) {
        if (cfg.feedMode == 2) feed = exposureIsReadable ? 1 : 2;
        else if (exposureIsReadable && !autoExposure && mf.flagsKnown) feed = 1;
    }
    plan.exposureSource = (uint32_t)feed;
    plan.exposureGuard = (feed == 1 && cfg.feedMode == 0) ? 1u : 0u;
    plan.preExposure = mf.preExposure > 0.0f ? mf.preExposure : 1.0f;
    plan.exposureScale = mf.exposureScale > 0.0f ? mf.exposureScale : 1.0f;

    // Pedestal: every divisor-family frame, Display frames on request or in absolute units, SDR unless classic.
    if (hdrMode == 0) {
        pedestal = cfg.codecMode != 0;
        plan.darkGate = cfg.sdrBlackRestore == 1 ? 2.0f : 0.061f; plan.pedestalCap = 0.025f;
        plan.darkGateMode = cfg.sdrBlackRestore == 2 ? 2u : 0u;
    } else {
        pedestal = !display || cfg.displayPedestal == 1 || (absolute && unitNits > 0.0f);
        plan.darkGate = absolute ? 1.0f / unitNits : 0.005f;
        plan.pedestalCap = absolute ? 2.0f / unitNits : 0.01f;
        plan.darkGateMode = (uint32_t)cfg.displayPedestal;
    }
    plan.pedestalGate = pedestal ? 1.0f : 0.0f;
    if (feed != 0) plan.flags |= ColorStageParams::kGateInInputUnits;

    const int cls = (int)plan.encoding * 1000 + (int)plan.curve * 100 + feed * 10 + (pedestal ? 1 : 0);
    if (cls != lastClass) {
        lastClass = cls;
        static const char* enc[4] = {"SDR", "linear HDR", "PQ HDR", "sRGB in float"};
        fill(description, "%s%s, %s codec, divisor %s", enc[plan.encoding], hdrMode == 0 ? "" : absolute ? " (absolute)" : " (relative)",
                  hdrMode == 0 ? "SDR" : display ? "Display" : "divisor", plan.fixedDivisor > 0.0f ? "fixed" : feed ? "from the game's exposure" : "measured");
        logf("frame class: %s, pedestal %s (format %u, game DLSS flags %s0x%x)", description, pedestal ? "on" : "off",
             (unsigned)gameFormat, mf.flagsKnown ? "" : "not seen yet, assumed ", mf.createFlags);
    }
    if (feed != lastFeed) { if (lastFeed != -1) plan.snap = 1.0f; lastFeed = feed; }
}

// Before the model: derive the divisor and write the proxy.
bool ColorStage::before(ID3D12GraphicsCommandList* cl, const Frame& f, const ColorStageParams& settings, const ColorConfig& cfg,
                        const ModelFrame& known, bool allowInPlace) {
    const D3D12_RESOURCE_DESC g = f.gameOut->GetDesc();
    if (g.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || g.SampleDesc.Count != 1) return false;
    if (f.rect.right <= f.rect.left || f.rect.bottom <= f.rect.top || f.rect.right > g.Width || f.rect.bottom > g.Height) return false;
    const UINT w = f.rect.right - f.rect.left, h = f.rect.bottom - f.rect.top;
    const UINT nw = f.nrW ? (f.nrW < w ? f.nrW : w) : w, nh = f.nrH ? (f.nrH < h ? f.nrH : h) : h;
    tick++;
    if (!init(cl) || !ensure(g, w, h, nw, nh, allowInPlace)) return false;
    cur = f;
    plan = settings; plan.w = w; plan.h = h; plan.nrW = nrW; plan.nrH = nrH;
    plan.flags &= ColorStageParams::kSkipModel;
    if (!plan.lookNeutral()) plan.flags |= ColorStageParams::kLook;
    {   // what the shaders are handed, logged whenever it changes: the proof that a look setting reaches them
        static float last[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
        const float now[9] = {plan.lookStrength, plan.lookBrighten, plan.lookDarken, plan.lookColour, plan.lookHue,
                              plan.lookShadows, plan.lookMidtones, plan.lookHighlights, plan.colorStrength};
        if (memcmp(last, now, sizeof(now)) != 0) {
            memcpy(last, now, sizeof(now));
            logf("shaders receive: EditStrength=%.2f Brightening=%.2f Darkening=%.2f Colour=%.2f Hue=%.2f Shadows=%.2f Midtones=%.2f Highlights=%.2f (look %s)",
                 now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], (plan.flags & ColorStageParams::kLook) ? "applied" : "neutral, not computed");
        }
    }
    plan.srcBaseX = inPlace ? f.rect.left : 0; plan.srcBaseY = inPlace ? f.rect.top : 0;
    reduced = nrW != w || nrH != h;
    const bool expReadable = f.exposure && exposureReadable(f.exposure->GetDesc());
    classify(cfg, known, f.exposure != nullptr, expReadable);

    bool ok = true;
    if (reduced) ok = ok && want(guide, nrW, nrH, DXGI_FORMAT_R16_FLOAT);
    if (pedestal) ok = ok && want(resolved, w, h, kWork) && want(blocks, (w + 31) / 32, (h + 31) / 32, kStat);
    if (!inPlace) {
        if (!hdr) hdr = makeTex(w, h, resourceFormat(g.Format), D3D12_RESOURCE_FLAG_NONE, kRead);
        if (!fin) fin = makeTex(w, h, resourceFormat(g.Format), kUa, kUav);
        ok = ok && hdr && fin;
    }
    const bool measure = plan.fixedDivisor <= 0.0f && f.pass == 0;   // a stacked pass keeps the first pass's divisor
    ok = ok && pipeline(kEncode) && (!measure || (pipeline(kMeasure) && pipeline(kExposure)));
    // Everything after() cannot do without is built now, so that it never stops half-way with the
    // frame's result unwritten. (The temporal filter and the panel are dropped on their own if they fail.)
    ok = ok && pipeline(reduced ? kResolveUp : kResolve) && (!pedestal || (pipeline(kReduce) && pipeline(kCommit)));
    if (!ok) return false;

    ring = (ring + 1) % kRing;
    bindFrame(nullptr);
    if (inPlace) transition(cl, f.gameOut, f.gameState, kUav);
    else {
        transition(cl, f.gameOut, f.gameState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cl, hdr, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = hdr; src.pResource = f.gameOut;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &f.rect);
        transition(cl, f.gameOut, D3D12_RESOURCE_STATE_COPY_SOURCE, f.gameState);
        transition(cl, hdr, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    }
    if (measure) {
        if (plan.exposureSource == 0 || plan.exposureGuard) { run(cl, kMeasure, kSetEncode, 1, 1); uavBarrier(cl, scale); uavBarrier(cl, commit); }
        if (plan.exposureSource != 0) {
            const bool needState = plan.exposureSource == 1 && !(f.exposureState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (needState) transition(cl, f.exposure, f.exposureState, kRead);
            run(cl, kExposure, kSetEncode, 1, 1); uavBarrier(cl, scale); uavBarrier(cl, commit);
            if (needState) transition(cl, f.exposure, kRead, f.exposureState);
        }
    }
    run(cl, kEncode, kSetEncode, (nrW + 7) / 8, (nrH + 7) / 8);
    if (inPlace) transition(cl, f.gameOut, kUav, f.gameState);
    transition(cl, pin, kUav, kRead);
    ready = true;
    return true;
}

void ColorStage::drawPanel(ID3D12GraphicsCommandList* cl, const Panel& panel) {
    if (!panel.tex || !pipeline(kOverlay)) return;
    plan.panelX = panel.x; plan.panelY = panel.y; plan.panelW = panel.w; plan.panelH = panel.h; plan.panelWhite = panel.white;
    uavBarrier(cl, inPlace ? cur.gameOut : fin);
    run(cl, kOverlay, kSetOverlay, (panel.w + 7) / 8, (panel.h + 7) / 8);
}

// After the model wrote its output: stabilize, bring the edit to the frame, optionally remove the
// pedestal, and leave the result in the game's DLSS output rectangle.
void ColorStage::after(ID3D12GraphicsCommandList* cl, const ModelFrame& mf, D3D12_RESOURCE_STATES motionState, bool temporal, const Panel* panel) {
    const int pass = cur.pass;
    ID3D12Resource* gameOut = cur.gameOut; const D3D12_RESOURCE_STATES gameState = cur.gameState;
    if (temporal && !(want(history[pass][0], nrW, nrH, kWork) && want(history[pass][1], nrW, nrH, kWork))) temporal = false;
    if (temporal && reduced && !(want(shaped, nrW, nrH, kWork) && pipeline(kShape))) temporal = false;
    // below the frame's resolution the shaped edit goes through its own pass, for the look as for the filter
    const bool shape = reduced && (temporal || ((plan.flags & ColorStageParams::kLook) && want(shaped, nrW, nrH, kWork) && pipeline(kShape)));
    bool motionBound = false;
    if (temporal) {
        plan.flags |= ColorStageParams::kTemporal;
        if (historyValid[pass] && !mf.reset) plan.flags |= ColorStageParams::kHistoryValid;
        if (mf.valid && mf.mvW && mf.mvH && motionUsable(mf.motion)) {
            plan.flags |= ColorStageParams::kMotion;
            plan.motionBaseX = mf.mvBaseX; plan.motionBaseY = mf.mvBaseY; plan.motionGridX = mf.mvW; plan.motionGridY = mf.mvH;
            float sx = mf.mvScaleX, sy = mf.mvScaleY;
            if (!(sx == sx) || sx == 0.0f) sx = 1.0f;
            if (!(sy == sy) || sy == 0.0f) sy = 1.0f;
            plan.motionScaleX = sx * (float)nrW / (float)mf.mvW; plan.motionScaleY = sy * (float)nrH / (float)mf.mvH;
            motionBound = true;
        }
    }
    bindFrame(panel);
    if (motionBound) srv(mf.motion, scalarViewFormat(mf.motion->GetDesc().Format), 3);

    const bool motionNeedsState = motionBound && !(motionState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* historyIn = temporal ? history[pass][historyWritten[pass]] : nullptr;
    transition(cl, pout, kUav, kRead);
    if (reduced) transition(cl, guide, kUav, kRead);
    transition(cl, historyIn, kUav, kRead);
    if (motionNeedsState) transition(cl, mf.motion, motionState, kRead);
    if (inPlace) transition(cl, gameOut, gameState, kUav);

    if (reduced) {
        if (shape) {
            run(cl, kShape, kSetShape, (nrW + 7) / 8, (nrH + 7) / 8);
            transition(cl, shaped, kUav, kRead);
            plan.flags |= ColorStageParams::kShaped;
        }
        run(cl, kResolveUp, kSetResolve, (plan.w + 15) / 16, (plan.h + 15) / 16);
        if (shape) transition(cl, shaped, kRead, kUav);
    } else run(cl, kResolve, kSetResolve, (plan.w + 7) / 8, (plan.h + 7) / 8);

    if (motionNeedsState) transition(cl, mf.motion, kRead, motionState);
    transition(cl, historyIn, kRead, kUav);
    if (temporal) historyWritten[pass] = 1 - historyWritten[pass];
    historyValid[pass] = temporal;

    if (pedestal) {
        transition(cl, resolved, kUav, kRead);
        run(cl, kReduce, kSetReduce, ((plan.w + 31) / 32 + 7) / 8, ((plan.h + 31) / 32 + 7) / 8);
        transition(cl, blocks, kUav, kRead);
        run(cl, kCommit, kSetCommit, (plan.w + 7) / 8, (plan.h + 7) / 8);
        transition(cl, blocks, kRead, kUav);
        transition(cl, resolved, kRead, kUav);
    }
    if (panel) drawPanel(cl, *panel);

    if (inPlace) transition(cl, gameOut, kUav, gameState);
    else {
        transition(cl, fin, kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cl, gameOut, gameState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = gameOut; src.pResource = fin;
        cl->CopyTextureRegion(&dst, cur.rect.left, cur.rect.top, 0, &src, nullptr);
        transition(cl, gameOut, D3D12_RESOURCE_STATE_COPY_DEST, gameState);
        transition(cl, fin, D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
    }
    if (reduced) transition(cl, guide, kRead, kUav);
    transition(cl, pout, kRead, kUav);
    transition(cl, pin, kRead, kUav);
}

// The model evaluate failed after before(): put the proxy back to its resting state.
void ColorStage::abandon(ID3D12GraphicsCommandList* cl) {
    transition(cl, pin, kRead, kUav);
    for (bool& v : historyValid) v = false;
}

bool ColorStage::overlayOnly(ID3D12GraphicsCommandList* cl, const Frame& f, const Panel& panel) {
    if (!ready || !panel.tex) return false;
    const D3D12_RESOURCE_DESC g = f.gameOut->GetDesc();
    if (f.rect.right <= f.rect.left || f.rect.bottom <= f.rect.top || f.rect.right > g.Width || f.rect.bottom > g.Height) return false;
    if (g.Format != gameFormat || f.rect.right - f.rect.left != width || f.rect.bottom - f.rect.top != height) return false;
    if (!inPlace && !fin) return false;
    tick++;
    cur = f; cur.pass = 0;
    plan.flags = 0; plan.srcBaseX = inPlace ? f.rect.left : 0; plan.srcBaseY = inPlace ? f.rect.top : 0;
    ring = (ring + 1) % kRing;
    bindFrame(&panel);
    if (inPlace) {
        transition(cl, f.gameOut, f.gameState, kUav);
        drawPanel(cl, panel);
        transition(cl, f.gameOut, kUav, f.gameState);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        transition(cl, f.gameOut, f.gameState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cl, fin, kUav, D3D12_RESOURCE_STATE_COPY_DEST);
        dst.pResource = fin; src.pResource = f.gameOut;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &f.rect);
        transition(cl, fin, D3D12_RESOURCE_STATE_COPY_DEST, kUav);
        drawPanel(cl, panel);
        transition(cl, fin, kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cl, f.gameOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        dst.pResource = f.gameOut; src.pResource = fin;
        cl->CopyTextureRegion(&dst, f.rect.left, f.rect.top, 0, &src, nullptr);
        transition(cl, f.gameOut, D3D12_RESOURCE_STATE_COPY_DEST, f.gameState);
        transition(cl, fin, D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
    }
    return true;
}

} // namespace d5
