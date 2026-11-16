// The settings panel, drawn on the finished frame at Present: after the game's exposure, bloom and tone
// mapping, so its brightness is the same in every scene and the game does not react to it.
#include "menu.h"
#include <d3dcompiler.h>
#include <dxgi1_6.h>

namespace d5 {
Menu& panelMenu();
std::atomic<bool> g_presentOverlay{false};      // true while the panel is drawn here and not in the game's frame

namespace {

const char kHLSL[] = R"(
Texture2D<float4> t : register(t0);
cbuffer C : register(b0) { float2 pos; uint mode; float nits; };
float4 VS(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.0, 1.0); }
float3 Lin(float3 c) { return lerp(c / 12.92, pow(abs((c + 0.055) / 1.055), 2.4), step(0.04045, c)); }
float4 PS(float4 p : SV_Position) : SV_Target {
    float3 c = t.Load(int3(int2(p.xy - pos), 0)).rgb;
    if (mode == 0u) return float4(c, 1.0);                       // an sRGB-encoded back buffer
    float3 l = Lin(c);
    if (mode == 3u) return float4(l, 1.0);                       // an _SRGB view encodes by itself
    if (mode == 2u) return float4(l * nits / 80.0, 1.0);         // scRGB
    float3 r = float3(dot(l, float3(0.6274, 0.3293, 0.0433)), dot(l, float3(0.0691, 0.9195, 0.0114)), dot(l, float3(0.0164, 0.0880, 0.8956)));
    float3 y = pow(max(r * nits / 10000.0, 0.0), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375), 1.0);     // HDR10
}
)";

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn oPresent = nullptr;
Present1Fn oPresent1 = nullptr;

struct State {
    ID3D12Device* dev = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 submitted = 0;
    ID3D12DescriptorHeap* rtv = nullptr;
    ID3D12DescriptorHeap* srv = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12Resource* tex = nullptr;
    ID3D12Resource* upload = nullptr;
    UINT texW = 0, texH = 0;
    bool failed = false;
} st;
std::mutex g_lock;

template <class T> void drop(T*& p) { if (p) { p->Release(); p = nullptr; } }
void waitGpu() {
    if (st.fence && st.fence->GetCompletedValue() < st.submitted) { st.fence->SetEventOnCompletion(st.submitted, st.event); WaitForSingleObject(st.event, 500); }
}
void reset() {
    waitGpu();
    drop(st.tex); drop(st.upload); drop(st.pso); drop(st.root); drop(st.srv); drop(st.rtv); drop(st.fence); drop(st.list); drop(st.alloc);
    st.dev = nullptr; st.texW = st.texH = 0; st.psoFormat = DXGI_FORMAT_UNKNOWN; st.submitted = 0;
}
bool create(ID3D12Device* dev) {
    reset();
    st.dev = dev;
    if (!st.event) st.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!st.vs) {
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "panel", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &st.vs, &err))) { if (err) err->Release(); return false; }
        if (FAILED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "panel", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &st.ps, &err))) { if (err) err->Release(); return false; }
    }
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&st.alloc));
    ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, st.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&st.list));
    if (ok) st.list->Close();
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&st.fence));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&st.rtv));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&st.srv));
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.Num32BitValues = 4; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    ok = ok && SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    ok = ok && SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&st.root));
    if (blob) blob->Release();
    if (err) err->Release();
    return ok;
}
bool pipeline(DXGI_FORMAT f) {
    if (st.pso && st.psoFormat == f) return true;
    drop(st.pso);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = st.root;
    d.VS = {st.vs->GetBufferPointer(), st.vs->GetBufferSize()}; d.PS = {st.ps->GetBufferPointer(), st.ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xffffffffu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = f; d.SampleDesc.Count = 1;
    st.psoFormat = f;
    return SUCCEEDED(st.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&st.pso));
}

// The swap chain's command queue. DXGI may answer GetDevice with the device and not the queue, so the
// queue every swap chain is created with is noted when it is created (the DXGI factory's creation
// functions are hooked from the start of the process), by swap chain and by window.
struct Chain { IUnknown* chain = nullptr; HWND window = nullptr; ID3D12CommandQueue* queue = nullptr; };
constexpr int kMaxChains = 8;
Chain g_chains[kMaxChains]; int g_nextChain = 0; std::mutex g_chainLock;
void noteChain(IUnknown* device, IUnknown* chain, HWND window) {
    if (!device || !chain) return;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&q)) || !q) return;     // not a D3D12 swap chain
    std::lock_guard<std::mutex> lock(g_chainLock);
    Chain& c = g_chains[g_nextChain]; g_nextChain = (g_nextChain + 1) % kMaxChains;
    if (c.queue) c.queue->Release();
    c.chain = chain; c.window = window; c.queue = q;       // the reference on the queue is kept
    logf("panel: swap chain %p created on queue %p, window %p", chain, q, window);
}
bool swapChainQueue(IDXGISwapChain* sc, ID3D12CommandQueue** out) {
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12CommandQueue), (void**)out)) && *out) return true;
    *out = nullptr;
    DXGI_SWAP_CHAIN_DESC sd{}; const bool haveDesc = SUCCEEDED(sc->GetDesc(&sd));
    std::lock_guard<std::mutex> lock(g_chainLock);
    ID3D12CommandQueue* q = nullptr;
    for (int n = 1; n <= kMaxChains && !q; n++) { const Chain& c = g_chains[(g_nextChain - n + kMaxChains) % kMaxChains]; if (c.queue && c.chain == (IUnknown*)sc) q = c.queue; }
    for (int n = 1; n <= kMaxChains && !q && haveDesc && sd.OutputWindow; n++) { const Chain& c = g_chains[(g_nextChain - n + kMaxChains) % kMaxChains]; if (c.queue && c.window == sd.OutputWindow) q = c.queue; }
    if (!q) return false;
    q->AddRef(); *out = q;
    return true;
}
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCoreFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCompFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
CreateFn oCreate = nullptr; CreateHwndFn oCreateHwnd = nullptr; CreateCoreFn oCreateCore = nullptr; CreateCompFn oCreateComp = nullptr;
HRESULT STDMETHODCALLTYPE hkCreate(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) {
    const HRESULT hr = oCreate(f, dev, d, sc);
    if (SUCCEEDED(hr) && sc && *sc) noteChain(dev, *sc, d ? d->OutputWindow : nullptr);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** sc) {
    const HRESULT hr = oCreateHwnd(f, dev, w, d, fd, o, sc);
    if (SUCCEEDED(hr) && sc && *sc) noteChain(dev, *sc, w);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateCore(IDXGIFactory2* f, IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** sc) {
    const HRESULT hr = oCreateCore(f, dev, w, d, o, sc);
    if (SUCCEEDED(hr) && sc && *sc) noteChain(dev, *sc, nullptr);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateComp(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** sc) {
    const HRESULT hr = oCreateComp(f, dev, d, o, sc);
    if (SUCCEEDED(hr) && sc && *sc) noteChain(dev, *sc, nullptr);
    return hr;
}
DWORD WINAPI installFactory(LPVOID) {
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory)) || !factory) { logf("panel: no DXGI factory, swap chain creation is not watched"); return 0; }
    void** vt = *(void***)factory;
    oCreate = (CreateFn)vt[10]; oCreateHwnd = (CreateHwndFn)vt[15]; oCreateCore = (CreateCoreFn)vt[16]; oCreateComp = (CreateCompFn)vt[24];
    DetourTransactionBegin();
    DetourAttach(&(PVOID&)oCreate, hkCreate); DetourAttach(&(PVOID&)oCreateHwnd, hkCreateHwnd);
    DetourAttach(&(PVOID&)oCreateCore, hkCreateCore); DetourAttach(&(PVOID&)oCreateComp, hkCreateComp);
    logf("panel: swap chain creation watched: %ld", DetourTransactionCommit());
    factory->Release();
    return 0;
}

// Draws the panel on the back buffer about to be presented. Returns false when it could not.
bool draw(IDXGISwapChain* sc) {
    IDXGISwapChain3* s3 = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Device* dev = nullptr; ID3D12Resource* back = nullptr;
    bool ok = false; int step = 0;
    do {
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&s3)) || !s3) { step = 1; break; }
        if (!swapChainQueue(sc, &queue)) { step = 2; break; }
        if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { step = 3; break; }
        if (dev != st.dev && !create(dev)) { st.failed = true; step = 4; break; }
        if (FAILED(s3->GetBuffer(s3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&back)) || !back) { step = 5; break; }
        const D3D12_RESOURCE_DESC bd = back->GetDesc();
        waitGpu();
        std::vector<uint32_t> pixels; UINT x = 0, y = 0, w = 0, h = 0;
        const bool need = !st.tex;
        if (!panelMenu().snapshot((UINT)bd.Width, bd.Height, need, pixels, x, y, w, h)) { step = 6; break; }
        if (!st.tex || st.texW != w || st.texH != h) {
            if (pixels.empty() && !panelMenu().snapshot((UINT)bd.Width, bd.Height, true, pixels, x, y, w, h)) { step = 7; break; }
            drop(st.tex); drop(st.upload); st.texW = w; st.texH = h;
        }
        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        if (!st.tex) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&st.tex))) { step = 8; break; }
            D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC ud{}; ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; ud.Width = total; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
            ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&st.upload))) { step = 9; break; }
            st.dev->CreateShaderResourceView(st.tex, nullptr, st.srv->GetCPUDescriptorHandleForHeapStart());
        }
        // 0 sRGB-encoded, 1 HDR10, 2 scRGB, 3 an _SRGB format
        UINT mode = 0; DXGI_FORMAT f = bd.Format;
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) mode = 2;
        else if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) mode = 3;
        else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
            IDXGIOutput* out = nullptr; IDXGIOutput6* out6 = nullptr; DXGI_OUTPUT_DESC1 od{};
            if (SUCCEEDED(s3->GetContainingOutput(&out)) && out && SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6)) && out6 && SUCCEEDED(out6->GetDesc1(&od))
                && od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) mode = 1;
            if (out6) out6->Release();
            if (out) out->Release();
        }
        if (!pipeline(f)) { st.failed = true; step = 10; break; }
        if (FAILED(st.alloc->Reset()) || FAILED(st.list->Reset(st.alloc, st.pso))) { step = 11; break; }
        ID3D12GraphicsCommandList* cl = st.list;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = 0;
        if (!pixels.empty()) {
            void* mapped = nullptr; D3D12_RANGE none{0, 0};
            if (SUCCEEDED(st.upload->Map(0, &none, &mapped)) && mapped) {
                for (UINT r = 0; r < h; r++) memcpy((BYTE*)mapped + fp.Offset + (size_t)r * fp.Footprint.RowPitch, &pixels[(size_t)r * w], (size_t)w * 4);
                st.upload->Unmap(0, nullptr);
                b.Transition.pResource = st.tex; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                cl->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = st.tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = st.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                cl->ResourceBarrier(1, &b);
            }
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = st.rtv->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(back, nullptr, rtv);
        b.Transition.pResource = back; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f};
        const D3D12_RECT sr{(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)};
        cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
        cl->SetGraphicsRootSignature(st.root);
        cl->SetDescriptorHeaps(1, &st.srv);
        cl->SetGraphicsRootDescriptorTable(0, st.srv->GetGPUDescriptorHandleForHeapStart());
        struct { float px, py; UINT mode; float nits; } c{(float)x, (float)y, mode, 203.0f};
        cl->SetGraphicsRoot32BitConstants(1, 4, &c, 0);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        if (FAILED(cl->Close())) { step = 12; break; }
        ID3D12CommandList* lists[1] = {cl};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(st.fence, ++st.submitted);
        static bool once = false; if (!once) { once = true; logf("panel: drawn at present, back buffer %ux%u format %d, output mode %u", (UINT)bd.Width, bd.Height, (int)f, mode); }
        ok = true;
    } while (false);
    if (!ok) { static int last = -1; if (step != last) { last = step; logf("panel: not drawn at present (step %d), drawn in the game's frame instead", step); } }
    if (back) back->Release();
    if (dev) dev->Release();
    if (queue) queue->Release();
    if (s3) s3->Release();
    return ok;
}

thread_local int t_depth = 0;
void onPresent(IDXGISwapChain* sc, UINT flags) {
    if (t_depth > 1 || (flags & DXGI_PRESENT_TEST)) return;
    { static bool once = false; if (!once) { once = true; logf("panel: first present seen (flags 0x%x)", flags); } }
    if (!panelMenu().visible) return;
    std::lock_guard<std::mutex> lock(g_lock);
    if (st.failed) { g_presentOverlay = false; return; }
    const bool ok = draw(sc);
    if (ok) g_presentOverlay = true;
}
HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    t_depth++; onPresent(sc, flags);
    const HRESULT hr = oPresent(sc, sync, flags);
    t_depth--; return hr;
}
HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    t_depth++; onPresent(sc, flags);
    const HRESULT hr = oPresent1(sc, sync, flags, p);
    t_depth--; return hr;
}

// Finds DXGI's Present by making a small swap chain of our own, then hooks it.
DWORD WINAPI install(LPVOID param) {
    ID3D12Device* dev = (ID3D12Device*)param;
    ID3D12CommandQueue* queue = nullptr; IDXGIFactory2* factory = nullptr; IDXGISwapChain1* sc = nullptr; HWND wnd = nullptr;
    do {
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue))) break;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory))) break;
        wnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!wnd) break;
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, wnd, &d, nullptr, nullptr, &sc)) || !sc) break;
        void** vt = *(void***)sc;
        oPresent = (PresentFn)vt[8]; oPresent1 = (Present1Fn)vt[22];
        DetourTransactionBegin();
        DetourAttach(&(PVOID&)oPresent, hkPresent);
        DetourAttach(&(PVOID&)oPresent1, hkPresent1);
        const LONG r = DetourTransactionCommit();
        logf("panel: present hook %s", r == NO_ERROR ? "installed" : "failed");
    } while (false);
    if (sc) sc->Release();
    if (factory) factory->Release();
    if (wnd) DestroyWindow(wnd);
    if (queue) queue->Release();
    dev->Release();
    return 0;
}

} // namespace

// Called once when the process starts, before the game makes its swap chain.
void presentWatchStart() {
    HANDLE t = CreateThread(nullptr, 0, installFactory, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void presentOverlayStart(ID3D12GraphicsCommandList* cl) {
    static std::atomic<bool> started{false};
    if (!cl || started.exchange(true)) return;
    ID3D12Device* dev = nullptr; cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return;
    HANDLE t = CreateThread(nullptr, 0, install, dev, 0, nullptr);
    if (t) CloseHandle(t); else dev->Release();
}

} // namespace d5
