#include "menu.h"
#include <cmath>

namespace d5 {

namespace {

// The panel shows what a player changes; everything else lives in dlss5native.ini.
// The rows are settings of RenoDX's DLSS Neural Rendering panel, under its names and with its ranges.
// Each one is shown only because its value reaches the model or the shaders (the log lines "model
// receives" and "shaders receive" tell what arrived).
enum Kind { kSwitch, kSlider, kStyle, kSkin, kResScale };
struct Row { const wchar_t* label; Kind kind; void* ptr; float lo, hi, step; const wchar_t* key; };
const Row kRows[] = {
    {L"DLSS 5", kSwitch, &g.enabled, 0, 1, 1, L"Enabled"},
    {L"Intensity", kSlider, &g.opts.intensity, 0, 2, 0.05f, L"Intensity"},
    {L"Style", kStyle, &g.opts.style, 0, 2, 1, L"Style"},
    {L"Local tone", kSlider, &g.opts.localToneStrength, 0, 2, 0.05f, L"LocalToneStrength"},
    {L"Structure", kSlider, &g.opts.localStructureStrength, 0, 2, 0.05f, L"LocalStructureStrength"},
    {L"Character mask", kSwitch, &g.opts.useAutoMask, 0, 1, 1, L"UseAutoMask"},
    {L"Character / skin", kSkin, &g.opts.skinStructureStrength, 0, 1, 0.05f, L"SkinStructureStrength"},
    {L"Edit strength", kSlider, &g.cs.lookStrength, 0, 2, 0.05f, L"LookStrength"},
    {L"Brightening", kSlider, &g.cs.lookBrighten, 0, 2, 0.05f, L"LookBrightening"},
    {L"Darkening", kSlider, &g.cs.lookDarken, 0, 2, 0.05f, L"LookDarkening"},
    {L"Colour", kSlider, &g.cs.lookColour, 0, 2, 0.05f, L"LookColour"},
    {L"Resolution scale", kResScale, &g.resolutionScale, 0.33f, 1, 0.05f, L"ResolutionScale"},
};
const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));

// A key press, repeated while held (after 400 ms, every 70 ms).
bool pressed(int vk, bool repeat) {
    static ULONGLONG since[256], last[256];
    if (vk <= 0 || vk > 255) return false;
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    if (!down) { since[vk] = 0; return false; }
    if (!since[vk]) { since[vk] = last[vk] = now; return true; }
    if (repeat && now - since[vk] > 400 && now - last[vk] > 70) { last[vk] = now; return true; }
    return false;
}
bool gameInFront() {
    DWORD pid = 0; HWND w = GetForegroundWindow();
    if (!w) return false;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void valueText(const Row& r, wchar_t* out, size_t n) {
    switch (r.kind) {
        case kSwitch: filln(out, n, L"%s", *(bool*)r.ptr ? L"On" : L"Off"); break;
        case kSlider: {
            const float v = *(float*)r.ptr;
            // with the character mask, Structure goes out at most 1 (sentOptions)
            if (r.ptr == &g.opts.localStructureStrength && g.opts.useAutoMask && v > 1.0f) filln(out, n, L"%.2f (sent 1.00)", v);
            else filln(out, n, L"%.2f", v);
        } break;
        case kStyle: {
            static const wchar_t* names[3] = {L"Default", L"Natural", L"Cinematic"};
            const uint32_t v = *(uint32_t*)r.ptr; filln(out, n, L"%s", names[v > 2 ? 0 : v]);
        } break;
        case kSkin: {
            const float v = *(float*)r.ptr;
            if (!g.opts.useAutoMask) filln(out, n, L"%s", L"needs Character mask");
            else if (v < 0) filln(out, n, L"%s", L"1.00 (stock)");
            else filln(out, n, L"%.2f", v);
        } break;
        case kResScale:
            if (g.followRenderRes) filln(out, n, L"%s", L"Render resolution");
            else filln(out, n, L"%.0f%%", g.resolutionScale * 100.0f);
            break;
    }
}

} // namespace

void Menu::adjust(int row, int dir) {
    const Row& r = kRows[row];
    switch (r.kind) {
        case kSwitch: *(bool*)r.ptr = !*(bool*)r.ptr; break;
        case kSlider: {
            float& v = *(float*)r.ptr;
            v = roundf((v + dir * r.step) / r.step) * r.step;
            if (v > r.hi) v = r.hi;
            if (v < r.lo) v = r.lo;
        } break;
        case kStyle: { uint32_t& v = *(uint32_t*)r.ptr; const int n = (int)(v > 2 ? 0 : v) + dir; v = (uint32_t)(n < 0 ? 0 : (n > 2 ? 2 : n)); } break;
        case kSkin: {
            float& v = *(float*)r.ptr;
            if (!g.opts.useAutoMask) return;            // the model ignores it without the mask
            if (v < 0 && dir > 0) return;               // stock is already the top of the range
            const float next = roundf(((v < 0 ? 1.0f : v) + dir * r.step) / r.step) * r.step;
            v = next > 1.0f ? 1.0f : (next < 0.0f ? 0.0f : next);
        } break;
        case kResScale: {
            float v = roundf(((g.followRenderRes ? 1.0f : g.resolutionScale) + dir * r.step) / r.step) * r.step;
            if (v > 1.0f) v = 1.0f;
            if (v < 0.33f) v = 0.33f;
            g.resolutionScale = v;
            if (g.followRenderRes) { g.followRenderRes = false; saveSetting(L"FollowRenderResolution"); }
        } break;
    }
    if (r.key) saveSetting(r.key);
    dirty = true;
}

void Menu::input() {
    if (!gameInFront()) return;
    if (pressed(g.menuKey, false)) { visible = !visible; dirty = true; logf("panel %s", visible ? "opened" : "closed"); }
    if (!visible) return;
    if (pressed(VK_UP, true)) { selected = (selected + kRowCount - 1) % kRowCount; dirty = true; }
    if (pressed(VK_DOWN, true)) { selected = (selected + 1) % kRowCount; dirty = true; }
    if (pressed(VK_LEFT, true)) adjust(selected, -1);
    if (pressed(VK_RIGHT, true)) adjust(selected, +1);
}

void Menu::render(std::vector<uint32_t>& pixels, const char* status) {
    const int rowH = (int)(fontPx * 1.55f), pad = (int)fontPx;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = (LONG)texW; bi.bmiHeader.biHeight = -(LONG)texH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); pixels.assign((size_t)texW * texH, 0xff202020); return; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-(int)fontPx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(-(int)fontPx, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT tiny = CreateFontW(-(int)(fontPx * 0.78f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto fill = [&](int x0, int y0, int x1, int y1, COLORREF c) { RECT r{x0, y0, x1, y1}; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); };
    auto text = [&](const wchar_t* s, int x0, int y0, int x1, int y1, COLORREF c, UINT fmt, HFONT f) {
        RECT r{x0, y0, x1, y1}; SelectObject(dc, f); SetTextColor(dc, c); DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    SetBkMode(dc, TRANSPARENT);
    const COLORREF kBack = RGB(16, 17, 20), kHead = RGB(30, 33, 40), kSel = RGB(46, 66, 104), kText = RGB(232, 232, 232), kDim = RGB(150, 154, 162), kAccent = RGB(130, 190, 255), kTrack = RGB(52, 56, 64);
    fill(0, 0, texW, texH, kBack);
    fill(0, 0, texW, rowH + pad / 2, kHead);
    text(L"DLSS 5", pad, 0, texW - pad, rowH + pad / 2, kText, DT_LEFT, bold);
    text(L"dlss5native " D5_VERSION_W, pad, 0, texW - pad, rowH + pad / 2, kDim, DT_RIGHT, tiny);
    int y = rowH + pad;
    for (int i = 0; i < kRowCount; i++, y += rowH) {
        const Row& r = kRows[i];
        if (i == selected) fill(pad / 2, y, texW - pad / 2, y + rowH, kSel);
        text(r.label, pad, y, texW - pad, y + rowH, kText, DT_LEFT, font);
        wchar_t v[64]; valueText(r, v, 64);
        text(v, pad, y, texW - pad, y + rowH, i == selected ? RGB(255, 255, 255) : kAccent, DT_RIGHT, i == selected ? bold : font);
        const bool bar = r.kind == kSlider || (r.kind == kSkin && g.opts.useAutoMask) || (r.kind == kResScale && !g.followRenderRes);
        if (bar) {      // a track, the filled part, a mark at the neutral value
            float value = *(float*)r.ptr;
            if (r.kind == kSkin && value < 0) value = 1.0f;
            const int x0 = (int)(texW * 0.44f), x1 = (int)(texW * 0.74f), mid = y + rowH / 2, half = (int)fontPx / 6 + 1;
            fill(x0, mid - half, x1, mid + half, i == selected ? RGB(24, 34, 56) : kTrack);
            if (value > r.lo) fill(x0, mid - half, x0 + (int)((x1 - x0) * (value - r.lo) / (r.hi - r.lo)), mid + half, i == selected ? RGB(255, 255, 255) : kAccent);
            if (r.hi > 1.0f) { const int n = x0 + (int)((x1 - x0) * (1.0f - r.lo) / (r.hi - r.lo)); fill(n, mid - half * 2, n + 1, mid + half * 2, kDim); }
        }
    }
    y += pad / 2;
    fill(pad, y, texW - pad, y + 1, RGB(60, 64, 72));
    y += pad / 2;
    wchar_t line[260]; d5::fill(line, L"%hs", status ? status : "");
    text(line, pad, y, texW - pad, y + rowH, kDim, DT_LEFT | DT_END_ELLIPSIS, tiny);
    y += rowH;
    wchar_t keyName[32] = L"F8"; { UINT sc = MapVirtualKeyW((UINT)g.menuKey, MAPVK_VK_TO_VSC); if (!sc || !GetKeyNameTextW((LONG)(sc << 16), keyName, 32)) d5::fill(keyName, L"?"); }
    d5::fill(line, L"\x2191 \x2193 select    \x2190 \x2192 change    %s close", keyName);
    text(line, pad, y, texW - pad, y + rowH, kDim, DT_LEFT, tiny);
    GdiFlush();
    pixels.resize((size_t)texW * texH);
    const uint32_t* src = (const uint32_t*)bits;          // BGRX -> RGBA, opaque
    for (size_t i = 0; i < pixels.size(); i++) { uint32_t p = src[i]; pixels[i] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff); }
    SelectObject(dc, oldBmp);
    DeleteObject(font); DeleteObject(bold); DeleteObject(tiny); DeleteObject(bmp); DeleteDC(dc);
}

static std::mutex g_menuLock;
void Menu::setStatus(const char* status) {
    std::lock_guard<std::mutex> lock(g_menuLock);
    if (status && strncmp(status, lastStatus, sizeof(lastStatus) - 1) != 0) { strncpy_s(lastStatus, status, _TRUNCATE); dirty = true; }
}
bool Menu::snapshot(UINT frameW, UINT frameH, bool force, std::vector<uint32_t>& pixels, UINT& x, UINT& y, UINT& w, UINT& h) {
    std::lock_guard<std::mutex> lock(g_menuLock);
    if (!visible || frameW < 320 || frameH < 240) return false;
    UINT px = frameH / 58; if (px < 14) px = 14; if (px > 44) px = 44;
    const UINT rowH = (UINT)(px * 1.55f);
    w = px * 30; h = rowH * (kRowCount + 3) + px * 3;
    if (w > frameW - 16) w = frameW - 16;
    if (h > frameH - 16) h = frameH - 16;
    if (seenGeneration != g_settingsGeneration.load()) { seenGeneration = g_settingsGeneration.load(); dirty = true; }
    if (texW != w || texH != h) { texW = w; texH = h; fontPx = px; dirty = true; }
    if (dirty || force) { render(pixels, lastStatus); dirty = false; }
    x = px * 2; y = px * 2;
    if (x + w > frameW) x = frameW - w;
    if (y + h > frameH) y = frameH - h;
    return true;
}

const Panel* Menu::prepare(ID3D12GraphicsCommandList* cl, UINT frameW, UINT frameH, const char* status) {
    std::lock_guard<std::mutex> lock(g_menuLock);
    for (size_t i = 0; i < retired.size();) { if (--retired[i].second <= 0) { retired[i].first->Release(); retired[i] = retired.back(); retired.pop_back(); } else i++; }
    if (!visible || !cl || frameW < 320 || frameH < 240) return nullptr;
    UINT px = frameH / 58; if (px < 14) px = 14; if (px > 44) px = 44;
    const UINT rowH = (UINT)(px * 1.55f);
    UINT w = px * 30, h = rowH * (kRowCount + 3) + px * 3;
    if (w > frameW - 16) w = frameW - 16;
    if (h > frameH - 16) h = frameH - 16;
    if (status && strncmp(status, lastStatus, sizeof(lastStatus) - 1) != 0) { strncpy_s(lastStatus, status, _TRUNCATE); dirty = true; }
    if (seenGeneration != g_settingsGeneration.load()) { seenGeneration = g_settingsGeneration.load(); dirty = true; }

    ID3D12Device* dev = nullptr; cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return nullptr;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    bool ok = true;
    if (dev != owner) { owner = dev; texW = texH = 0; }      // the game made a new device: nothing of the old one is reused
    if (!panel.tex || texW != w || texH != h) {
        if (panel.tex) retired.emplace_back(panel.tex, 16);
        if (upload) retired.emplace_back(upload, 16);
        panel.tex = nullptr; upload = nullptr; texW = w; texH = h; fontPx = px; dirty = true;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ColorStage::kRead, nullptr, __uuidof(ID3D12Resource), (void**)&panel.tex));
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = ok && SUCCEEDED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload));
    }
    dev->Release();
    if (!ok || !panel.tex || !upload) { static bool once = false; if (!once) { once = true; logf("panel: texture creation failed"); } return nullptr; }
    if (dirty) {
        std::vector<uint32_t> pixels; render(pixels, lastStatus);
        void* mapped = nullptr; D3D12_RANGE none{0, 0};
        if (SUCCEEDED(upload->Map(0, &none, &mapped)) && mapped) {
            for (UINT y = 0; y < h; y++) memcpy((BYTE*)mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, &pixels[(size_t)y * w], (size_t)w * 4);
            upload->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = panel.tex; b.Transition.Subresource = 0;
            b.Transition.StateBefore = ColorStage::kRead; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{}; dst.pResource = panel.tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = ColorStage::kRead;
            cl->ResourceBarrier(1, &b);
            dirty = false;
        }
    }
    panel.w = w; panel.h = h; panel.x = px * 2; panel.y = px * 2; panel.white = 0.85f;
    if (panel.x + w > frameW) panel.x = frameW - w;
    if (panel.y + h > frameH) panel.y = frameH - h;
    return &panel;
}

} // namespace d5
