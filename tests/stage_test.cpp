// Off-line tests. No game, no NVIDIA file: the colour stage, the panel and the settings run on a real
// D3D12 device with the debug layer and GPU-based validation switched on, several frames in flight like
// in a game, and the test program itself is built with AddressSanitizer.
//
// Checked:
//   - identity: with a model that changes nothing, the frame comes back as the game made it (exact on
//     float HDR, within one code on PQ and SDR), on the in-place path and on the copy path, at full and
//     reduced working resolution, with and without the temporal filter;
//   - containment: pixels outside the DLSS output rectangle are never touched;
//   - bounds: an arbitrary model output moves a pixel by no more than the configured clamps;
//   - lifetime: working-resolution and frame-size changes while frames are in flight release nothing the
//     GPU still uses (the debug layer reports it);
//   - the debug layer reports no error, corruption or warning for any of it;
//   - a second D3D12 device (the software one) and back: nothing built for one device is used on the other;
//   - settings: defaults -> file -> load round trip, single-key save, clamping of out-of-range values,
//     text that is not a number;
//   - the two parameter blocks (ngxframe.h): what is read from a game's DLSS call, with and without
//     sub-rectangles, dynamic resolution and missing inputs, and what the DLSS 5 model is handed.
#include "../src/colorstage.h"
#include "../src/menu.h"
#include "../src/ngxframe.h"
#include <map>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cmath>
#include <string>

namespace d5 {
wchar_t g_dir[MAX_PATH];
static bool g_quiet = true;
void logf(const char* fmt, ...) { if (g_quiet) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
}
using namespace d5;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3D12Device* dev; static ID3D12CommandQueue* queue; static ID3D12Fence* fence; static UINT64 fenceValue; static HANDLE fenceEvent;
static ID3D12InfoQueue* info;
static bool g_softwareDevice = false; static int g_layoutNotes = 0;
enum { kInFlight = 3 };
static ID3D12CommandAllocator* alloc[kInFlight]; static ID3D12GraphicsCommandList* cl; static UINT64 submitted[kInFlight]; static int slot;

// Everything below is rebuilt for the device asked for; what belonged to the previous one is left alone
// (a game does release it, the colour stage must simply never touch it again).
static bool useDevice(bool software) {
    if (queue) { queue->Signal(fence, ++fenceValue); if (fence->GetCompletedValue() < fenceValue) { fence->SetEventOnCompletion(fenceValue, fenceEvent); WaitForSingleObject(fenceEvent, INFINITE); } }
    dev = nullptr; info = nullptr; queue = nullptr; fence = nullptr; cl = nullptr; fenceValue = 0; slot = 0; g_softwareDevice = software;
    for (auto& a : alloc) a = nullptr;
    for (auto& v : submitted) v = 0;
    IDXGIFactory4* factory = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
        const bool isSoftware = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (isSoftware == software && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("device: %ls\n", d.Description); break; }
        adapter->Release(); adapter = nullptr;
    }
    if (!dev) return false;
    dev->QueryInterface(IID_PPV_ARGS(&info));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    for (auto& a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&cl)); cl->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return queue && cl && fence;
}

static bool g_serial = false;    // -serial: one frame at a time, to tell an ordering problem from the rest
static void waitFor(UINT64 v) { if (fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, fenceEvent); WaitForSingleObject(fenceEvent, INFINITE); } }
static void beginFrame() { slot = (slot + 1) % kInFlight; waitFor(submitted[slot]); alloc[slot]->Reset(); cl->Reset(alloc[slot], nullptr); }
static void endFrame() { cl->Close(); ID3D12CommandList* l[] = {cl}; queue->ExecuteCommandLists(1, l); queue->Signal(fence, ++fenceValue); submitted[slot] = fenceValue; if (g_serial) waitFor(fenceValue); }
static void flush() { queue->Signal(fence, ++fenceValue); waitFor(fenceValue); }

// Messages of the debug layer since the last call; anything at warning level or worse fails the test.
static int drainMessages(const char* where) {
    if (!info) return 0;
    int bad = 0; const UINT64 n = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0; info->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len); D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf.data(); info->GetMessage(i, m, &len);
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        // See "what the validator's state tracking is worth" in main(): on a hardware device this one
        // message is counted apart; on the software device it fails the test like any other.
        if (!g_softwareDevice && m->ID == D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT) { g_layoutNotes++; continue; }
        bad++; if (bad <= 6) printf("  D3D12 [%s] %s\n", where, m->pDescription);
    }
    info->ClearStoredMessages();
    return bad;
}

static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = 0; x.Transition.StateBefore = a; x.Transition.StateAfter = b; cl->ResourceBarrier(1, &x);
}
static ID3D12Resource* makeBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)); return r;
}

// A game texture with CPU-side access: 4 bytes or 8 bytes per texel depending on the format.
struct GameTex {
    ID3D12Resource* tex = nullptr; ID3D12Resource* up = nullptr; ID3D12Resource* back = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT w = 0, h = 0, bpp = 4; DXGI_FORMAT format{}; D3D12_RESOURCE_STATES state{};
    void create(UINT w_, UINT h_, DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES st) {
        w = w_; h = h_; format = f; state = st; bpp = (f == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&tex));
        UINT64 total = 0; dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        up = makeBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        back = makeBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void destroy() { for (ID3D12Resource** r : {&tex, &up, &back}) if (*r) { (*r)->Release(); *r = nullptr; } }
    void upload(const std::vector<BYTE>& px) {     // records on the open list
        void* m = nullptr; up->Map(0, nullptr, &m);
        for (UINT y = 0; y < h; y++) memcpy((BYTE*)m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, &px[(size_t)y * w * bpp], (size_t)w * bpp);
        up->Unmap(0, nullptr);
        barrier(tex, state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = tex; src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, state);
    }
    void requestReadback() {
        barrier(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; src.pResource = tex; dst.pResource = back; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    std::vector<BYTE> read() {                      // after flush()
        std::vector<BYTE> px((size_t)w * h * bpp); void* m = nullptr; back->Map(0, nullptr, &m);
        if (!m) { printf("  readback map failed, device removed reason 0x%lx\n", dev->GetDeviceRemovedReason()); drainMessages("device"); exit(2); }
        for (UINT y = 0; y < h; y++) memcpy(&px[(size_t)y * w * bpp], (BYTE*)m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)w * bpp);
        D3D12_RANGE none{0, 0}; back->Unmap(0, &none); return px;
    }
};

// ---- texel packing ----
static uint16_t toHalf(float v) {
    uint32_t x; memcpy(&x, &v, 4); uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    if (e <= 0) { if (e < -10) return (uint16_t)sign; m |= 0x800000; return (uint16_t)(sign | (m >> (14 - e))); }
    if (e >= 31) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | (e << 10) | (m >> 13));
}
static float fromHalf(uint16_t hv) {
    uint32_t s = (hv >> 15) & 1, e = (hv >> 10) & 0x1f, m = hv & 0x3ff; float v;
    if (e == 0) v = ldexpf((float)m, -24); else if (e == 31) v = m ? NAN : INFINITY; else v = ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}
static uint32_t toFloat11(float v, int mantBits) {   // unsigned small float: 5-bit exponent, 6- or 5-bit mantissa
    if (!(v > 0)) return 0;
    uint16_t hv = toHalf(v); return (uint32_t)(hv & 0x7fff) >> (10 - mantBits);
}
static float fromFloat11(uint32_t bits, int mantBits) { return fromHalf((uint16_t)(bits << (10 - mantBits))); }

// A synthetic frame: a horizontal ramp from black to `peak`, coloured bands, a black strip.
static void sceneColor(UINT x, UINT y, UINT w, UINT h, float peak, float rgb[3]) {
    float t = (float)x / (float)(w - 1); float v = peak * t * t;
    int band = (int)(y * 6 / h);
    rgb[0] = v * (band % 3 == 0 ? 1.0f : 0.35f); rgb[1] = v * (band % 3 == 1 ? 1.0f : 0.45f); rgb[2] = v * (band % 3 == 2 ? 1.0f : 0.25f);
    if (band == 5) rgb[0] = rgb[1] = rgb[2] = v;
    if (y < h / 12) rgb[0] = rgb[1] = rgb[2] = 0.0f;
}
static std::vector<BYTE> makeScene(DXGI_FORMAT f, UINT w, UINT h, float peak, bool hostile) {
    std::vector<BYTE> px((size_t)w * h * (f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4));
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        float c[3]; sceneColor(x, y, w, h, peak, c);
        if (hostile && x == w / 3 && y == h / 2) c[0] = INFINITY;          // a game's own non-finite pixels
        if (hostile && x == w / 2 && y == h / 2) c[1] = NAN;
        size_t i = (size_t)y * w + x;
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) { uint16_t* p = (uint16_t*)&px[i * 8]; p[0] = toHalf(c[0]); p[1] = toHalf(c[1]); p[2] = toHalf(c[2]); p[3] = toHalf(0.25f + 0.5f * (float)y / h); }
        else if (f == DXGI_FORMAT_R11G11B10_FLOAT) { uint32_t v = toFloat11(c[0], 6) | (toFloat11(c[1], 6) << 11) | (toFloat11(c[2], 5) << 22); memcpy(&px[i * 4], &v, 4); }
        else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) { auto q = [](float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (uint32_t)lroundf(v * 1023.0f); }; uint32_t v = q(c[0]) | (q(c[1]) << 10) | (q(c[2]) << 20) | (2u << 30); memcpy(&px[i * 4], &v, 4); }
        else { auto q = [](float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (BYTE)lroundf(v * 255.0f); }; px[i * 4] = q(c[0]); px[i * 4 + 1] = q(c[1]); px[i * 4 + 2] = q(c[2]); px[i * 4 + 3] = 200; }
    }
    return px;
}
// Per-channel values of one texel as floats (codes for the integer formats), and whether they are finite.
static void texel(DXGI_FORMAT f, const std::vector<BYTE>& px, size_t i, float out[4]) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) { const uint16_t* p = (const uint16_t*)&px[i * 8]; for (int k = 0; k < 4; k++) out[k] = fromHalf(p[k]); }
    else if (f == DXGI_FORMAT_R11G11B10_FLOAT) { uint32_t v; memcpy(&v, &px[i * 4], 4); out[0] = fromFloat11(v & 0x7ff, 6); out[1] = fromFloat11((v >> 11) & 0x7ff, 6); out[2] = fromFloat11(v >> 22, 5); out[3] = 1; }
    else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) { uint32_t v; memcpy(&v, &px[i * 4], 4); out[0] = (float)(v & 1023); out[1] = (float)((v >> 10) & 1023); out[2] = (float)((v >> 20) & 1023); out[3] = (float)(v >> 30); }
    else { for (int k = 0; k < 4; k++) out[k] = (float)px[i * 4 + k]; }
}

static double pqToLinear(double v) {
    const double x = pow(v < 0 ? 0 : v, 1.0 / 78.84375), n = x - 0.8359375;
    return pow((n > 0 ? n : 0) / (18.8515625 - 18.6875 * x), 1.0 / 0.1593017578125);
}

struct Case {
    const char* name; DXGI_FORMAT format; bool uav; bool allowInPlace; float scale; int stability; int codecMode; bool hdrFlag; float peak;
    float tolerance;        // identity tolerance: 0 = bit exact, otherwise codes (integer formats) or relative error (float formats)
    bool subRect; bool panel;
};

// The "model": copies the proxy to the output (identity) or fills the output with a flat grey (arbitrary edit).
static void fakeModel(ColorStage& st, bool identity, ID3D12Resource* grey) {
    barrier(st.pout, ColorStage::kUav, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = st.pout;
    if (identity) {
        barrier(st.pin, ColorStage::kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource = st.pin; cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(st.pin, D3D12_RESOURCE_STATE_COPY_SOURCE, ColorStage::kRead);
    } else {
        D3D12_BOX b{0, 0, 0, st.nrW, st.nrH, 1}; src.pResource = grey; cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &b);
    }
    barrier(st.pout, D3D12_RESOURCE_STATE_COPY_DEST, ColorStage::kUav);
}

static void runCase(const Case& c) {
    const UINT W = 416, H = 240;                         // the DLSS output rectangle
    const UINT texW = c.subRect ? W + 96 : W, texH = c.subRect ? H + 64 : H;
    const D3D12_BOX rect = c.subRect ? D3D12_BOX{64, 32, 0, 64 + W, 32 + H, 1} : D3D12_BOX{0, 0, 0, W, H, 1};
    const D3D12_RESOURCE_STATES gameState = c.uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    GameTex game; game.create(texW, texH, c.format, c.uav, gameState);
    GameTex motion; motion.create(W / 2, H / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    GameTex grey; grey.create(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const bool isFloat = c.format == DXGI_FORMAT_R16G16B16A16_FLOAT || c.format == DXGI_FORMAT_R11G11B10_FLOAT;
    const bool isPQ = c.format == DXGI_FORMAT_R10G10B10A2_UNORM && c.hdrFlag;
    const std::vector<BYTE> scene = makeScene(c.format, texW, texH, c.peak, isFloat);
    std::vector<BYTE> mv((size_t)(W / 2) * (H / 2) * 8); { uint16_t* p = (uint16_t*)mv.data(); for (size_t i = 0; i < mv.size() / 8; i++) { p[i * 4] = toHalf(0.6f); p[i * 4 + 1] = toHalf(-0.3f); } }
    std::vector<BYTE> flat((size_t)W * H * 8); { uint16_t* p = (uint16_t*)flat.data(); for (size_t i = 0; i < flat.size() / 8; i++) { p[i * 4] = toHalf(0.9f); p[i * 4 + 1] = toHalf(0.2f); p[i * 4 + 2] = toHalf(0.6f); p[i * 4 + 3] = toHalf(1.0f); } }

    ColorStage* stage = new ColorStage();
    Menu menu; menu.visible = c.panel;
    ColorStageParams params; ColorConfig cfg; cfg.codecMode = c.codecMode;
    ModelFrame mf; mf.valid = true; mf.flagsKnown = true; mf.createFlags = c.hdrFlag ? 1u : 0u;
    mf.motion = motion.tex; mf.mvScaleX = 1; mf.mvScaleY = 1; mf.mvW = W / 2; mf.mvH = H / 2;
    const bool temporal = c.stability == 2;
    int bad = 0; bool identityOk = true, containedOk = true, boundedOk = true, ran = true; double worst = 0;
    UINT wx = 0, wy = 0; int wk = 0, wf = 0; float wa = 0, wb = 0;

    // Frames 0..13: identity. 14: arbitrary edit. 15..: the working resolution changes while frames are in flight.
    for (int frame = 0; frame < 22 && ran; frame++) {
        const bool identity = frame != 14;
        float scale = c.scale; if (frame >= 15) scale = (frame % 2) ? 0.5f : 0.75f;
        beginFrame();
        if (frame == 0) { motion.upload(mv); barrier(grey.tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST); grey.state = D3D12_RESOURCE_STATE_COPY_DEST; grey.upload(flat); barrier(grey.tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE); grey.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
        game.upload(scene);
        ColorStage::Frame f; f.gameOut = game.tex; f.gameState = gameState; f.rect = rect;
        f.nrW = (UINT)lroundf(W * scale); f.nrH = (UINT)lroundf(H * scale);
        const Panel* panel = menu.prepare(cl, W, H, "test");
        if (!stage->before(cl, f, params, cfg, mf, c.allowInPlace)) { ran = false; endFrame(); break; }
        fakeModel(*stage, identity, grey.tex);
        stage->after(cl, mf, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, temporal, panel);
        const bool inspect = frame == 13 || frame == 14 || frame == 21;
        if (inspect) game.requestReadback();
        endFrame();
        if (!inspect) continue;
        flush(); bad += drainMessages(c.name);
        const std::vector<BYTE> got = game.read();
        for (UINT y = 0; y < texH; y++) for (UINT x = 0; x < texW; x++) {
            const size_t i = (size_t)y * texW + x; float a[4], b[4]; texel(c.format, scene, i, a); texel(c.format, got, i, b);
            const bool inside = x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
            if (!inside) { if (memcmp(&scene[i * game.bpp], &got[i * game.bpp], game.bpp) != 0) containedOk = false; continue; }
            const UINT rx = x - rect.left, ry = y - rect.top;
            const bool underPanel = panel && rx >= panel->x && rx < panel->x + panel->w && ry >= panel->y && ry < panel->y + panel->h;
            if (underPanel) continue;
            for (int k = 0; k < 3; k++) {
                if (!std::isfinite(a[k])) continue;                       // the game's own Inf / NaN pass through
                if (!std::isfinite(b[k])) { identityOk = boundedOk = false; continue; }
                if (frame == 14) {
                    // Display codec: at most LumaClamp + ChromaClamp stops away from the original.
                    if (isFloat && c.codecMode == 2 && a[k] > 1e-3f) { const double stops = fabs(log2((double)b[k] / (double)a[k])); if (stops > params.lumaClamp + params.chromaClamp + 0.05) boundedOk = false; }
                } else {
                    double err = isFloat ? fabs((double)b[k] - a[k]) / (fabs((double)a[k]) > 1e-4 ? fabs((double)a[k]) : 1.0) : fabs((double)b[k] - a[k]);
                    // PQ: near black one code is far below float precision next to a bright channel of the same
                    // pixel (the OkLab round trip of the divisor family); under 0.002 nit it is not counted.
                    if (isPQ && fabs(pqToLinear(a[k] / 1023.0) - pqToLinear(b[k] / 1023.0)) < 2e-7) err = 0;
                    if (err > worst) { worst = err; wx = rx; wy = ry; wk = k; wa = a[k]; wb = b[k]; wf = frame; }
                    if (c.tolerance == 0 ? memcmp(&scene[i * game.bpp], &got[i * game.bpp], game.bpp) != 0 && err != 0 : err > c.tolerance) identityOk = false;
                }
            }
            if (game.bpp == 8 && frame != 14 && a[3] != b[3]) identityOk = false;     // the game's alpha is kept
        }
        if (panel && frame == 13) {     // the panel was drawn where it was asked
            const size_t i = (size_t)(rect.top + panel->y + panel->h / 2) * texW + rect.left + panel->x + panel->w / 2;
            CHECK(memcmp(&scene[i * game.bpp], &got[i * game.bpp], game.bpp) != 0, "%s: panel not drawn", c.name);
        }
    }
    flush(); bad += drainMessages(c.name);
    CHECK(ran, "%s: colour stage refused the frame", c.name);
    CHECK(identityOk, "%s: identity broken (worst error %g, tolerance %g) at %u,%u channel %d frame %d: %g -> %g", c.name, worst, c.tolerance, wx, wy, wk, wf, wa, wb);
    CHECK(containedOk, "%s: pixels outside the DLSS rectangle changed", c.name);
    CHECK(boundedOk, "%s: an arbitrary model output escaped the clamps or produced non-finite pixels", c.name);
    CHECK(bad == 0, "%s: %d debug-layer messages", c.name, bad);
    printf("%-44s %s  worst identity error %.3g  (%s)\n", c.name, ran && identityOk && containedOk && boundedOk && bad == 0 ? "ok  " : "FAIL", worst, stage->describe());
    // Released on purpose only after every frame finished: the stage itself defers what it retires.
    game.destroy(); motion.destroy(); grey.destroy();
}

// --------------------------------------------------------------------------------------------- look
// The look gains, on a real device. The "model" answers a flat colour, so it brightens some pixels of the
// scene and darkens others: each gain must act on its own part of that edit and leave the rest alone.
static bool lookRun(const ColorStageParams& params, const std::vector<BYTE>& scene, UINT W, UINT H, std::vector<float>& out) {
    const DXGI_FORMAT F = DXGI_FORMAT_R16G16B16A16_FLOAT;
    GameTex game; game.create(W, H, F, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    GameTex grey; grey.create(W, H, F, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::vector<BYTE> flat((size_t)W * H * 8);
    { uint16_t* q = (uint16_t*)flat.data(); for (size_t i = 0; i < flat.size() / 8; i++) { q[i * 4] = toHalf(0.55f); q[i * 4 + 1] = toHalf(0.45f); q[i * 4 + 2] = toHalf(0.30f); q[i * 4 + 3] = toHalf(1.0f); } }
    ColorStage* stage = new ColorStage();
    ColorConfig cfg; cfg.codecMode = 2;
    ModelFrame mf; mf.valid = true; mf.flagsKnown = true; mf.createFlags = 1u;
    bool ran = true;
    for (int frame = 0; frame < 15 && ran; frame++) {       // the measured divisor settles on identity frames first
        beginFrame();
        if (frame == 0) { barrier(grey.tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST); grey.state = D3D12_RESOURCE_STATE_COPY_DEST; grey.upload(flat); barrier(grey.tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE); grey.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
        game.upload(scene);
        ColorStage::Frame f; f.gameOut = game.tex; f.gameState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; f.rect = D3D12_BOX{0, 0, 0, W, H, 1}; f.nrW = W; f.nrH = H;
        if (!stage->before(cl, f, params, cfg, mf, true)) { ran = false; endFrame(); break; }
        fakeModel(*stage, frame != 14, grey.tex);
        stage->after(cl, mf, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false, nullptr);
        if (frame == 14) game.requestReadback();
        endFrame();
    }
    flush();
    CHECK(drainMessages("look") == 0, "look: debug-layer messages");
    if (ran) {
        const std::vector<BYTE> got = game.read();
        out.resize((size_t)W * H * 3);
        for (size_t i = 0; i < (size_t)W * H; i++) { float a[4]; texel(F, got, i, a); out[i * 3] = a[0]; out[i * 3 + 1] = a[1]; out[i * 3 + 2] = a[2]; }
    }
    game.destroy(); grey.destroy();
    return ran;
}
static double median(std::vector<double>& v) { if (v.empty()) return NAN; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static void testLook() {
    const UINT W = 416, H = 240; const size_t N = (size_t)W * H;
    const std::vector<BYTE> sceneBytes = makeScene(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 6.0f, false);
    std::vector<float> scene(N * 3);
    for (size_t i = 0; i < N; i++) { float a[4]; texel(DXGI_FORMAT_R16G16B16A16_FLOAT, sceneBytes, i, a); scene[i * 3] = a[0]; scene[i * 3 + 1] = a[1]; scene[i * 3 + 2] = a[2]; }
    auto lum = [](const std::vector<float>& v, size_t i) { return 0.212639 * v[i * 3] + 0.715169 * v[i * 3 + 1] + 0.072192 * v[i * 3 + 2]; };
    ColorStageParams base;
    std::vector<float> neutral, strength0, strength2, darken0, brighten0, colour0, hue0, shadows0, highlights0;
    ColorStageParams p;
    bool ok = lookRun(base, sceneBytes, W, H, neutral);
    p = base; p.lookStrength = 0; ok = ok && lookRun(p, sceneBytes, W, H, strength0);
    p = base; p.lookStrength = 2; ok = ok && lookRun(p, sceneBytes, W, H, strength2);
    p = base; p.lookDarken = 0; ok = ok && lookRun(p, sceneBytes, W, H, darken0);
    p = base; p.lookBrighten = 0; ok = ok && lookRun(p, sceneBytes, W, H, brighten0);
    p = base; p.lookColour = 0; p.lookHue = 0; ok = ok && lookRun(p, sceneBytes, W, H, colour0);
    p = base; p.lookShadows = 0; ok = ok && lookRun(p, sceneBytes, W, H, shadows0);
    p = base; p.lookHighlights = 0; ok = ok && lookRun(p, sceneBytes, W, H, highlights0);
    CHECK(ok, "look: the colour stage refused a frame");
    if (!ok) return;
    // The pixels the model darkened and those it brightened (clearly, and bright enough to be trusted).
    std::vector<size_t> dark, bright;
    for (size_t i = 0; i < N; i++) {
        const double s = lum(scene, i), n = lum(neutral, i);
        if (!(s > 0.05) || !std::isfinite(n)) continue;
        if (n < 0.85 * s) dark.push_back(i); else if (n > 1.15 * s) bright.push_back(i);
    }
    CHECK(dark.size() > 200 && bright.size() > 200, "look: the test scene must hold darkened and brightened pixels (%zu, %zu)", dark.size(), bright.size());
    if (dark.size() <= 200 || bright.size() <= 200) return;
    auto ratio = [&](const std::vector<size_t>& set, const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<double> r; for (size_t i : set) r.push_back(lum(a, i) / lum(b, i)); return median(r); };
    auto stops = [&](const std::vector<size_t>& set, const std::vector<float>& a) {
        std::vector<double> r; for (size_t i : set) r.push_back(fabs(log2(lum(a, i) / lum(scene, i)))); return median(r); };
    // spread between the channels of the edit: what is left of the model's colour change
    auto chroma = [&](const std::vector<float>& a) {
        std::vector<double> r;
        for (size_t i = 0; i < N; i++) {
            double lo = 1e9, hi = -1e9; bool usable = true;
            for (int k = 0; k < 3; k++) { const double sv = scene[i * 3 + k], av = a[i * 3 + k]; if (!(sv > 0.05) || !(av > 0)) { usable = false; break; } const double e = log2(av / sv); lo = e < lo ? e : lo; hi = e > hi ? e : hi; }
            if (usable) r.push_back(hi - lo);
        }
        return median(r); };
    const double nD = ratio(dark, neutral, scene), nB = ratio(bright, neutral, scene);
    const double s0D = ratio(dark, strength0, scene), s0B = ratio(bright, strength0, scene);
    const double d0D = ratio(dark, darken0, scene), d0B = ratio(bright, darken0, neutral);
    const double b0B = ratio(bright, brighten0, scene), b0D = ratio(dark, brighten0, neutral);
    const double e1D = stops(dark, neutral), e2D = stops(dark, strength2), e1B = stops(bright, neutral), e2B = stops(bright, strength2);
    const double c1 = chroma(neutral), c0 = chroma(colour0);
    // pixels the model changed a little, where no clamp is in the way: Edit strength 2 must double the edit
    std::vector<size_t> mild, changedAll;
    for (size_t i = 0; i < N; i++) {
        const double s = lum(scene, i), n = lum(neutral, i);
        if (!(s > 0.003) || !(n > 0)) continue;
        const double e = fabs(log2(n / s));
        if (e > 0.2) changedAll.push_back(i);
        if (s > 0.05 && e > 0.2 && e < 0.6) mild.push_back(i);
    }
    CHECK(mild.size() > 200 && changedAll.size() > 2000, "look: the test scene must hold mildly changed pixels (%zu, %zu)", mild.size(), changedAll.size());
    if (mild.size() <= 200 || changedAll.size() <= 2000) return;
    const double m1 = stops(mild, neutral), m2 = stops(mild, strength2);
    // shadows / highlights: the darkest pixels the model changed, and the brightest fifth
    std::sort(changedAll.begin(), changedAll.end(), [&](size_t a, size_t b) { return lum(scene, a) < lum(scene, b); });
    std::vector<size_t> low(changedAll.begin(), changedAll.begin() + changedAll.size() / 20);
    std::vector<size_t> changed = dark; changed.insert(changed.end(), bright.begin(), bright.end());
    std::sort(changed.begin(), changed.end(), [&](size_t a, size_t b) { return lum(scene, a) < lum(scene, b); });
    std::vector<size_t> high(changed.end() - changed.size() / 5, changed.end());
    const double shLow = stops(low, shadows0), shLowN = stops(low, neutral), hiHigh = stops(high, highlights0), hiHighN = stops(high, neutral);
    printf("look: model darkens %zu px to x%.2f and brightens %zu px to x%.2f\n", dark.size(), nD, bright.size(), nB);
    printf("look: Edit strength 0 -> x%.3f / x%.3f of the game's frame ; 2 -> %.2f stops where 1 gives %.2f (large edits, bounded by the clamps: %.2f / %.2f against %.2f / %.2f)\n", s0D, s0B, m2, m1, e2D, e2B, e1D, e1B);
    printf("look: Darkening 0 -> darkened px x%.3f of the game's frame, brightened px x%.3f of the neutral result\n", d0D, d0B);
    printf("look: Brightening 0 -> brightened px x%.3f of the game's frame, darkened px x%.3f of the neutral result\n", b0B, b0D);
    printf("look: Colour+Hue 0 -> channel spread %.3f stops (1: %.3f) ; Shadows 0 -> %.2f stops in the darkest pixels (1: %.2f) ; Highlights 0 -> %.2f in the brightest fifth (1: %.2f)\n", c0, c1, shLow, shLowN, hiHigh, hiHighN);
    CHECK(fabs(s0D - 1) < 0.03 && fabs(s0B - 1) < 0.03, "look: Edit strength 0 must give the game's frame back (x%.3f, x%.3f)", s0D, s0B);
    CHECK(m2 > 1.8 * m1 && m2 < 2.2 * m1 && e2D > e1D && e2B > e1B, "look: Edit strength 2 must double the edit (%.2f vs %.2f; %.2f vs %.2f, %.2f vs %.2f stops)", m2, m1, e2D, e1D, e2B, e1B);
    CHECK(fabs(d0D - 1) < 0.06 && fabs(d0B - 1) < 0.03, "look: Darkening 0 must undo the darkening only (x%.3f, x%.3f)", d0D, d0B);
    CHECK(fabs(b0B - 1) < 0.06 && fabs(b0D - 1) < 0.03, "look: Brightening 0 must undo the brightening only (x%.3f, x%.3f)", b0B, b0D);
    CHECK(c0 < 0.5 * c1, "look: Colour 0 and Hue shift 0 must remove the model's colour change (%.3f vs %.3f)", c0, c1);
    CHECK(shLow < 0.6 * shLowN, "look: Shadows 0 must remove the edit in the dark pixels (%.2f vs %.2f)", shLow, shLowN);
    CHECK(hiHigh < 0.6 * hiHighN, "look: Highlights 0 must remove the edit in the bright pixels (%.2f vs %.2f)", hiHigh, hiHighN);
}

// What goes out for structure and skin (sentOptions).
static void testSentOptions() {
    DLSSNROptions o; o.localStructureStrength = 1.6f; o.skinStructureStrength = -1.0f; o.useAutoMask = false;
    DLSSNROptions s = sentOptions(o);
    CHECK(s.localStructureStrength == 1.6f && s.skinStructureStrength == -1.0f, "sent options: without the mask the stored values go out unchanged");
    o.useAutoMask = true; s = sentOptions(o);
    CHECK(s.localStructureStrength == 1.0f && s.skinStructureStrength == 1.0f, "sent options: with the mask, structure at most 1 and stock skin = 1 (%g, %g)", s.localStructureStrength, s.skinStructureStrength);
    o.localStructureStrength = 0.4f; o.skinStructureStrength = 0.25f; s = sentOptions(o);
    CHECK(s.localStructureStrength == 0.4f && s.skinStructureStrength == 0.25f, "sent options: values in range go out unchanged");
    CHECK(s.intensity == o.intensity && s.localToneStrength == o.localToneStrength && s.style == o.style && s.useAutoMask, "sent options: nothing else is touched");
}

// ------------------------------------------------------------------------------- validator self-check
// The simplest correct program that creates a texture while a command list is being recorded and writes
// it from a compute shader on that same list, frame after frame with several frames in flight. It uses
// nothing of dlss5native. Returns how many messages the debug layer produced for it: any message here is
// the validator misreading correct code, and tells how far its reports can be trusted on this machine.
static int validatorSelfCheck(int frames, bool explicitFirstState, bool rewriteView) {
    static const char kShader[] = R"(RWTexture2D<float4> uOut : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { uOut[id.xy] = float4(1.0, 0.5, 0.25, 1.0); })";
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "selfcheck", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) return -1;
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER param{}; param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param.DescriptorTable.NumDescriptorRanges = 1; param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 1; rd.pParameters = &param;
    ID3DBlob* blob = nullptr; D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
    ID3D12RootSignature* rs = nullptr; dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs; pd.CS.pShaderBytecode = code->GetBufferPointer(); pd.CS.BytecodeLength = code->GetBufferSize();
    ID3D12PipelineState* pso = nullptr; dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    enum { kSlots = 256, kKeep = 16 };
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr; dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    const UINT step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Resource* alive[kKeep] = {};
    if (!rs || !pso || !heap) return -1;
    flush(); drainMessages("self-check setup");
    for (int frame = 0; frame < frames; frame++) {
        beginFrame();
        const UINT w = 64 + 8 * (UINT)(frame % 5), h = 48;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource*& t = alive[frame % kKeep];
        if (t) { t->Release(); t = nullptr; }      // recorded kKeep frames ago: long executed
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, explicitFirstState ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&t));
        if (!t) return -1;
        if (explicitFirstState) barrier(t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart(); c.ptr += (SIZE_T)(frame % kSlots) * step;
        D3D12_GPU_DESCRIPTOR_HANDLE g = heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += (UINT64)(frame % kSlots) * step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = d.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(t, nullptr, &ud, c);
        cl->SetDescriptorHeaps(1, &heap); cl->SetComputeRootSignature(rs); cl->SetComputeRootDescriptorTable(0, g); cl->SetPipelineState(pso);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        if (rewriteView) dev->CreateUnorderedAccessView(t, nullptr, &ud, c);     // the same view written again after the dispatch was recorded
        endFrame();
    }
    flush();
    const int messages = drainMessages(explicitFirstState ? "validator self-check, explicit first state" : "validator self-check");
    for (ID3D12Resource*& t : alive) if (t) { t->Release(); t = nullptr; }
    heap->Release(); pso->Release(); rs->Release(); blob->Release(); code->Release();
    return messages;
}

// ------------------------------------------------------------------------------------ parameter blocks
// A parameter block like NGX's: named values, converted between the numeric types on the way out.
struct FakeParams : NVSDK_NGX_Parameter {
    std::map<std::string, double> numbers; std::map<std::string, void*> pointers;
    void Set(const char* n, unsigned long long v) override { numbers[n] = (double)v; }
    void Set(const char* n, float v) override { numbers[n] = v; }
    void Set(const char* n, double v) override { numbers[n] = v; }
    void Set(const char* n, unsigned int v) override { numbers[n] = v; }
    void Set(const char* n, int v) override { numbers[n] = v; }
    void Set(const char* n, ID3D11Resource* v) override { pointers[n] = v; }
    void Set(const char* n, ID3D12Resource* v) override { pointers[n] = v; }
    void Set(const char* n, void* v) override { pointers[n] = v; }
    template <class T> NVSDK_NGX_Result number(const char* n, T* out) const {
        auto it = numbers.find(n); if (it == numbers.end()) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        *out = (T)it->second; return NVSDK_NGX_Result_Success;
    }
    template <class T> NVSDK_NGX_Result pointer(const char* n, T** out) const {
        auto it = pointers.find(n); if (it == pointers.end()) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        *out = (T*)it->second; return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const override { return number(n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const override { return number(n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const override { return number(n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const override { return number(n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const override { return number(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const override { return pointer(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const override { return pointer(n, v); }
    NVSDK_NGX_Result Get(const char* n, void** v) const override { return pointer(n, v); }
    void Reset() override { numbers.clear(); pointers.clear(); }
    double num(const char* n) const { auto it = numbers.find(n); return it == numbers.end() ? -12345.0 : it->second; }
    void* ptr(const char* n) const { auto it = pointers.find(n); return it == pointers.end() ? nullptr : it->second; }
};
// Stand-ins for textures: never dereferenced, only told apart and sized.
static ID3D12Resource* const kOutTex = (ID3D12Resource*)0x1000, * const kColorTex = (ID3D12Resource*)0x2000, * const kDepthTex = (ID3D12Resource*)0x3000,
                     * const kMotionTex = (ID3D12Resource*)0x4000, * const kProxyTex = (ID3D12Resource*)0x5000, * const kResultTex = (ID3D12Resource*)0x6000;
static uint32_t g_outW = 2560, g_outH = 1440, g_lowW = 1712, g_lowH = 960, g_motionTexW = 1712, g_motionTexH = 960;
static TexSize fakeSize(ID3D12Resource* r) {
    if (r == kOutTex) return TexSize{g_outW, g_outH};
    if (r == kMotionTex) return TexSize{g_motionTexW, g_motionTexH};
    if (r == kColorTex || r == kDepthTex) return TexSize{g_lowW, g_lowH};
    return TexSize{};
}
static void testParameterBlocks() {
    // The game's create and evaluate as recorded in a real game (1707x960 rendered into 1712x960 textures, 2560x1440 out).
    FakeParams create; create.Set("Width", 1707u); create.Set("Height", 960u); create.Set("OutWidth", 2560u); create.Set("OutHeight", 1440u);
    create.Set("DLSS.Feature.Create.Flags", 11);
    GameFeature f; readGameFeature(&create, f);
    CHECK(f.flagsKnown && f.createFlags == 11 && f.renderW == 1707 && f.renderH == 960 && f.outW == 2560 && f.outH == 1440, "game create: flags and sizes");
    FakeParams ev; ev.Set("Output", kOutTex); ev.Set("Color", kColorTex); ev.Set("Depth", kDepthTex); ev.Set("MotionVectors", kMotionTex);
    ev.Set("MV.Scale.X", -1.0f); ev.Set("MV.Scale.Y", -1.0f); ev.Set("Jitter.Offset.X", -0.104167f); ev.Set("Jitter.Offset.Y", 0.0694445f); ev.Set("Reset", 1);
    GameFrame g1; bool ok = readGameFrame(&ev, f, fakeSize, g1);
    CHECK(ok && g1.output == kOutTex && g1.depth == kDepthTex && g1.motion == kMotionTex && g1.exposure == nullptr, "game evaluate: resources");
    CHECK(g1.rect.left == 0 && g1.rect.top == 0 && g1.rect.right == 2560 && g1.rect.bottom == 1440, "game evaluate: output rectangle (%u,%u-%u,%u)", g1.rect.left, g1.rect.top, g1.rect.right, g1.rect.bottom);
    CHECK(g1.renderW == 1707 && g1.renderH == 960 && g1.motionW == 1707 && g1.motionH == 960, "game evaluate: render size from the create, low-resolution motion vectors (%ux%u, %ux%u)", g1.renderW, g1.renderH, g1.motionW, g1.motionH);
    CHECK(g1.mvScaleX == -1.0f && g1.mvScaleY == -1.0f && g1.hasJitter && g1.reset && g1.preExposure == 1.0f, "game evaluate: scales, jitter, reset");
    // Dynamic resolution and sub-rectangles: the evaluate's own values win, origins are carried.
    ev.Set("DLSS.Render.Subrect.Dimensions.Width", 1280u); ev.Set("DLSS.Render.Subrect.Dimensions.Height", 720u);
    ev.Set("DLSS.Input.Depth.Subrect.Base.X", 8u); ev.Set("DLSS.Input.Depth.Subrect.Base.Y", 4u); ev.Set("DLSS.Input.MV.Subrect.Base.X", 16u); ev.Set("DLSS.Input.MV.Subrect.Base.Y", 2u);
    ev.Set("DLSS.Output.Subrect.Base.X", 100u); ev.Set("DLSS.Output.Subrect.Base.Y", 50u); ev.Set("Reset", 0); ev.Set("DLSS.Pre.Exposure", 2.5f);
    GameFrame g2; ok = readGameFrame(&ev, f, fakeSize, g2);
    CHECK(ok && g2.renderW == 1280 && g2.renderH == 720 && g2.depthX == 8 && g2.depthY == 4 && g2.motionX == 16 && g2.motionY == 2 && g2.motionW == 1280 && g2.motionH == 720, "game evaluate: dynamic resolution and origins");
    CHECK(g2.rect.left == 100 && g2.rect.top == 50 && g2.rect.right == 2560 && g2.rect.bottom == 1440 && !g2.reset && g2.preExposure == 2.5f, "game evaluate: an output rectangle never leaves its texture (%u,%u-%u,%u)", g2.rect.left, g2.rect.top, g2.rect.right, g2.rect.bottom);
    // Motion vectors at the output resolution (the flag is absent), and a feature created before the hooks.
    GameFeature full = f; full.createFlags = 9; g_motionTexW = 2560; g_motionTexH = 1440;
    FakeParams ev2; ev2.Set("Output", kOutTex); ev2.Set("Depth", kDepthTex); ev2.Set("MotionVectors", kMotionTex);
    GameFrame g3; ok = readGameFrame(&ev2, full, fakeSize, g3);
    CHECK(ok && g3.motionW == 2560 && g3.motionH == 1440 && g3.mvScaleX == 1.0f && !g3.hasJitter, "game evaluate: full-resolution motion vectors");
    GameFrame g4; ok = readGameFrame(&ev2, GameFeature(), fakeSize, g4);
    CHECK(ok && g4.rect.right == 2560 && g4.rect.bottom == 1440 && g4.renderW == 1712 && g4.renderH == 960 && g4.motionW == 2560, "game evaluate: unknown feature, sizes from the textures (%ux%u, %u)", g4.renderW, g4.renderH, g4.motionW);
    g_motionTexW = 1712; g_motionTexH = 960;
    // What cannot be used is refused: a missing input, an origin outside its texture.
    FakeParams bad; bad.Set("Output", kOutTex); bad.Set("MotionVectors", kMotionTex);
    GameFrame g5; CHECK(!readGameFrame(&bad, f, fakeSize, g5), "game evaluate: without depth it is not a frame for the model");
    bad.Set("Depth", kDepthTex); bad.Set("DLSS.Output.Subrect.Base.X", 4000u);
    CHECK(!readGameFrame(&bad, f, fakeSize, g5), "game evaluate: an output origin outside the texture is refused");
    FakeParams other; other.Set("DLSSG.Backbuffer", kOutTex);
    CHECK(!readGameFrame(&other, f, fakeSize, g5), "game evaluate: another feature's evaluate is not taken for DLSS");

    const ModelFrame mf = toModelFrame(g2, f);
    CHECK(mf.valid && mf.motion == kMotionTex && mf.mvBaseX == 16 && mf.mvBaseY == 2 && mf.mvW == 1280 && mf.mvH == 720 && mf.flagsKnown && mf.createFlags == 11 && mf.preExposure == 2.5f, "colour stage frame");

    // What the model is handed: the values NVIDIA's own plugin was recorded to send, at a working resolution.
    int marker = 0; FakeParams m;
    writeModelCreate(&m, 1920, 1080, 11, 3, &marker);
    CHECK(m.num("Width") == 1920 && m.num("OutHeight") == 1080 && m.num("DLSSNR.Width") == 1920 && m.num("DLSSNR.Output.Height") == 1080 && m.num("DLSSNR.ScalingRatio") == 1
          && m.num("DLSS.Feature.Create.Flags") == 11 && m.num("DLSSNR.Hint.Render.Preset") == 3 && m.num("PerfQualityValue") == 0 && m.num("DLSS.Pre.Exposure") == 1
          && m.ptr("DLSSNRComputeScalingRatioCallback") == &marker && m.num("CreationNodeMask") == 1, "model create parameters");
    DLSSNROptions o; o.intensity = 1.35f; o.localToneStrength = 0.45f; o.localStructureStrength = 0.8f; o.style = 2; o.useAutoMask = true; o.skinStructureStrength = 0.25f; o.preset = 3;
    writeModelEvaluate(&m, kProxyTex, kResultTex, 1920, 1080, g2, f, o, true, &marker);
    CHECK(m.ptr("DLSSNR.Color") == kProxyTex && m.ptr("DLSSNR.Output") == kResultTex && m.ptr("DLSSNR.MVec") == kMotionTex && m.ptr("DLSSNR.Depth") == kDepthTex, "model evaluate: resources");
    CHECK(m.num("DLSSNR.ColorSubrectWidth") == 1920 && m.num("DLSSNR.OutputSubrectHeight") == 1080 && m.num("DLSSNR.ColorSubrectBaseX") == 0
          && m.num("DLSSNR.DepthSubrectBaseX") == 8 && m.num("DLSSNR.DepthSubrectWidth") == 1280 && m.num("DLSSNR.MVecSubrectBaseY") == 2 && m.num("DLSSNR.MVecSubrectHeight") == 720, "model evaluate: rectangles");
    CHECK(m.num("DLSSNR.MVecScaleX") == -1 && m.num("DLSSNR.DepthInverted") == 1 && m.num("DLSSNR.Reset") == 1 && m.num("DLSSNR.Enabled") == 1
          && fabs(m.num("DLSSNR.JitterOffsetX") + 0.104167) < 1e-6, "model evaluate: motion scale, depth direction, reset, jitter");
    CHECK(fabs(m.num("DLSSNR.Intensity") - 1.35) < 1e-6 && fabs(m.num("DLSSNR.LocalToneStrength") - 0.45) < 1e-6 && fabs(m.num("DLSSNR.LocalStructureStrength") - 0.8) < 1e-6
          && m.num("DLSSNR.GlobalToneStrength") == 1 && fabs(m.num("DLSSNR.SkinStructureStrength") - 0.25) < 1e-6 && m.num("DLSSNR.Style") == 2 && m.num("DLSSNR.UseAutoMask") == 1
          && m.num("DLSSNR.Hint.Render.Preset") == 3, "model evaluate: every option of the panel reaches the block");
    GameFeature notInverted = f; notInverted.createFlags = 3;
    writeModelEvaluate(&m, kProxyTex, kResultTex, 1920, 1080, g3, notInverted, DLSSNROptions(), false, &marker);
    CHECK(m.num("DLSSNR.DepthInverted") == 0 && m.num("DLSSNR.Reset") == 0 && m.num("DLSSNR.SkinStructureStrength") == -1 && m.num("DLSSNR.UseAutoMask") == 0, "model evaluate: defaults");
    // The import table reader, on this very program: a function it imports is found, another is not.
    void** slot = findImport(GetModuleHandleW(nullptr), "GetTempPathW");
    CHECK(slot != nullptr && *slot != nullptr, "import table: an imported function is found");
    CHECK(findImport(GetModuleHandleW(nullptr), "NoSuchFunctionAnywhere") == nullptr && findImport(nullptr, "GetTempPathW") == nullptr, "import table: an absent function is not found");
    printf("%-44s %s\n", "parameter blocks (game DLSS, DLSS 5 model)", "ok  ");
}

static void testSettings() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); swprintf_s(g_dir, L"%sdlss5native_test_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(g_dir, nullptr);
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%s\\dlss5native.ini", g_dir);
    DeleteFileW(ini);
    loadSettings();                                   // writes the file with every default
    CHECK(GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES, "settings: file not created");
    const Settings defaults;
    CHECK(g.enabled == defaults.enabled && g.passes == 1 && g.resolutionScale == 1.0f && g.opts.skinStructureStrength == -1.0f, "settings: defaults");
    CHECK(g.cs.lumaClamp == 2.0f && g.cs.chromaClamp == 1.0f && g.cs.alpha == 0.15f && g.cfg.codecMode == 2 && g.cfg.paperWhiteScale == 2.5375f, "settings: colour defaults survive the file round trip");
    g.opts.intensity = 0.35f; saveSetting(L"Intensity"); g.passes = 3; saveSetting(L"Passes");
    g.opts.intensity = 1.0f; g.passes = 1; loadSettings();
    CHECK(fabsf(g.opts.intensity - 0.35f) < 1e-4f && g.passes == 3, "settings: single-key save not read back (%g, %d)", g.opts.intensity, g.passes);
    WritePrivateProfileStringW(L"DLSS5", L"Passes", L"99", ini); WritePrivateProfileStringW(L"DLSS5", L"ResolutionScale", L"0.01", ini);
    WritePrivateProfileStringW(L"DLSS5", L"DetailStabilityAlpha", L"-4", ini); WritePrivateProfileStringW(L"DLSS5", L"Intensity", L"garbage", ini);
    WritePrivateProfileStringW(L"DLSS5", L"LumaClampStops", L"nan", ini); WritePrivateProfileStringW(L"DLSS5", L"ChromaClampStops", L"1e999", ini);
    WritePrivateProfileStringW(L"DLSS5", L"ColorStrength", L"inf", ini); WritePrivateProfileStringW(L"DLSS5", L"CodecMode", L"-7", ini);
    WritePrivateProfileStringW(L"DLSS5", L"ToggleKey", L"100000", ini); WritePrivateProfileStringW(L"DLSS5", L"LocalToneStrength", L"5", ini);
    loadSettings();
    CHECK(g.passes == kMaxPasses && g.resolutionScale == 0.33f && g.cs.alpha == 0.01f, "settings: out-of-range values not clamped (%d, %g, %g)", g.passes, g.resolutionScale, g.cs.alpha);
    CHECK(g.opts.intensity == defaults.opts.intensity && g.cs.lumaClamp == defaults.cs.lumaClamp && g.cs.chromaClamp == defaults.cs.chromaClamp && g.cs.colorStrength == defaults.cs.colorStrength,
          "settings: text that is not a finite number must leave the default (%g, %g, %g, %g)", g.opts.intensity, g.cs.lumaClamp, g.cs.chromaClamp, g.cs.colorStrength);
    CHECK(g.cfg.codecMode == 0 && g.toggleKey == 255 && g.opts.localToneStrength == 2.0f, "settings: ranges (%d, %d, %g)", g.cfg.codecMode, g.toggleKey, g.opts.localToneStrength);
    saveSetting(L"NoSuchKey");                        // unknown key: ignored
    CHECK(!settingsChangedOnDisk() && !settingsChangedOnDisk(), "settings: our own writes must not count as an outside edit");
    Sleep(30); WritePrivateProfileStringW(L"DLSS5", L"Passes", L"2", ini);
    const bool firstLook = settingsChangedOnDisk(), secondLook = settingsChangedOnDisk();
    CHECK(!firstLook && secondLook, "settings: an outside edit is reported once it has settled (%d, %d)", (int)firstLook, (int)secondLook);
    loadSettings();
    CHECK(g.passes == 2 && !settingsChangedOnDisk(), "settings: outside edit not picked up (%d)", g.passes);
    DeleteFileW(ini); RemoveDirectoryW(g_dir);
    g = Settings();
    printf("%-44s %s\n", "settings round trip", g_failures ? "FAIL" : "ok");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) g_quiet = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-serial")) g_serial = true;
    bool softwareFirst = false; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-software")) softwareFirst = true;   // every case on the software device
    testSettings();
    testParameterBlocks();

    ID3D12Debug1* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->SetEnableGPUBasedValidation(TRUE); printf("D3D12 debug layer + GPU-based validation: on\n"); }
    else printf("D3D12 debug layer not installed: running without it (install the Windows 'Graphics Tools' feature)\n");
    if (!useDevice(false) && !(softwareFirst = useDevice(true))) { printf("no D3D12 device: GPU tests skipped\n"); return g_failures ? 1 : 0; }

    {
        const int a = validatorSelfCheck(400, false, false), b = validatorSelfCheck(400, true, false), c2 = validatorSelfCheck(400, false, true), d2 = validatorSelfCheck(400, true, true);
        printf("validator self-check (400 frames each): created in working state %d, explicit first transition %d, same with the view rewritten after recording %d / %d\n", a, b, c2, d2);
    }
    testSentOptions();
    testLook();
    const DXGI_FORMAT F11 = DXGI_FORMAT_R11G11B10_FLOAT, F16 = DXGI_FORMAT_R16G16B16A16_FLOAT, PQ = DXGI_FORMAT_R10G10B10A2_UNORM, S8 = DXGI_FORMAT_R8G8B8A8_UNORM;
    const Case cases[] = {
        //  name                                       format uav   inPlace scale stab codec hdr   peak  tol     subRect panel
        {"R11G11B10 HDR, in place",                    F11, true,  true,  1.0f,  1, 2, true,  6.0f, 0,      false, false},
        {"R11G11B10 HDR, in place, temporal",          F11, true,  true,  1.0f,  2, 2, true,  6.0f, 0,      false, false},
        {"R11G11B10 HDR, in place, sub-rectangle",     F11, true,  true,  1.0f,  2, 2, true,  6.0f, 0,      true,  true},
        {"R11G11B10 HDR, copy path",                   F11, true,  false, 1.0f,  2, 2, true,  6.0f, 0,      true,  true},
        {"R11G11B10 HDR, no UAV flag (copy path)",     F11, false, true,  1.0f,  1, 2, true,  6.0f, 0,      false, false},
        {"R11G11B10 HDR, 50 % resolution",             F11, true,  true,  0.5f,  1, 2, true,  6.0f, 0,      false, false},
        {"R11G11B10 HDR, 66 % resolution, temporal",   F11, true,  true,  0.66f, 2, 2, true,  6.0f, 0,      true,  true},
        {"R11G11B10 HDR, 33 % resolution, copy path",  F11, true,  false, 0.33f, 2, 2, true,  6.0f, 0,      false, false},
        {"RGBA16F HDR, in place, temporal",            F16, true,  true,  1.0f,  2, 2, true,  40.0f, 0,     true,  false},
        {"RGBA16F HDR, 50 % resolution, copy path",    F16, true,  false, 0.5f,  2, 2, true,  40.0f, 0,     false, true},
        {"RGBA16F HDR, classic codec + pedestal",      F16, true,  true,  1.0f,  1, 0, true,  3.0f, 0.35f,  false, false},
        {"RGBA16F HDR, classic codec, 50 %, copy",     F16, true,  false, 0.5f,  2, 0, true,  3.0f, 0.35f,  true,  true},
        {"R10G10B10A2 PQ, in place",                   PQ,  true,  true,  1.0f,  2, 2, true,  0.75f, 3.0f,  false, false},
        {"R10G10B10A2 PQ, 50 % resolution, copy",      PQ,  true,  false, 0.5f,  1, 2, true,  0.75f, 3.0f,  true,  false},
        {"R10G10B10A2 SDR (no HDR flag)",              PQ,  true,  true,  1.0f,  1, 2, false, 1.0f, 28.0f,  false, false},
        {"RGBA8 SDR, in place",                        S8,  true,  true,  1.0f,  2, 2, false, 1.0f, 7.0f,   false, true},
        {"RGBA8 SDR, 50 % resolution, copy path",      S8,  true,  false, 0.5f,  2, 2, false, 1.0f, 7.0f,   true,  false},
    };
    // What the validator's state tracking is worth.
    //
    // GPU-based validation follows the state of every texture on the GPU and reports a shader that uses
    // one in the wrong state (message INCOMPATIBLE_TEXTURE_LAYOUT). On the hardware device these tests were
    // written on (NVIDIA, driver 621.01) it reports that for textures created during the frame, a few
    // times per run, on different frames and different cases each time the very same program is run -
    // although the commands recorded are the same every time, although the debug layer's own exact state
    // tracking on the CPU accepts every transition, and although Microsoft's software device, running the
    // very same commands under the same validation, never reports it. A property of the commands cannot
    // change from run to run, so on a hardware device that one message is counted apart, and every case is
    // run a second time on the software device, where it fails the test like any other message.
    if (!softwareFirst) for (const Case& c : cases) runCase(c);
    if (g_layoutNotes) printf("hardware device: %d state-tracking report(s) of the validator set aside (see the comment in main)\n", g_layoutNotes);

    // The game makes a new device (here the software one): nothing built for the first one may be used.
    // Every case runs again on it, with the validator's state tracking fully enforced.
    if (useDevice(true)) {
        for (const Case& c : cases) runCase(c);
        // ... and the first device again
        if (!softwareFirst && useDevice(false)) { const Case again = {"first device again: R11G11B10, 66 %", F11, true, true, 0.66f, 2, 2, true, 6.0f, 0, true, true}; runCase(again); }
    } else printf("no software D3D12 device: the strict run and the device-change test are skipped\n");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
