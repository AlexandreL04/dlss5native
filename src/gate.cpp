// A game that has DLSS 5 of its own, on a GPU older than RTX 50: NVIDIA's NGX core refuses to load the
// model there. When the game's NGX loads its nvngx_dlssnr.dll, it is handed the model file placed next to
// this DLL instead (one that carries kernels for the GPU), reported as fit for an older architecture.
// Nothing else of the game's DLSS 5 is touched. In a game without DLSS 5 this file does nothing: the
// model is then loaded by ngx.cpp itself, which needs none of this.
#include "common.h"

namespace d5 {
namespace {

// ---- NGX gate bypass for pre-Blackwell GPUs ----
// The driver's NGX core refuses the DLSS-NR snippet twice on RTX 20-40: (1) the snippet's version
// resource declares NGXGpuArchitecture=Blackwell2, (2) the community build that carries kernels for
// older architectures is not NVIDIA-signed. We keep NVIDIA's signed file as nvngx_dlssnr.dll (so the
// signature check runs on a genuine file), report an older architecture string from its version
// resource, and hand the loader the file next to this DLL.
using PFun_VerQueryValueW = BOOL WINAPI(LPCVOID, LPCWSTR, LPVOID*, PUINT);
using PFun_LoadLibraryExW = HMODULE WINAPI(LPCWSTR, HANDLE, DWORD);
PFun_VerQueryValueW* oVerQueryValueW = nullptr;
PFun_VerQueryValueW* oVerQueryValueW2 = nullptr;
PFun_LoadLibraryExW* oLoadLibraryExW = nullptr;
const wchar_t kArchOld[] = L"NVSDK_NGX_GPU_Arch_Turing";
wchar_t g_rtPath[MAX_PATH];
bool g_haveRt = false;

BOOL fixArch(BOOL r, LPVOID* buf, PUINT len, LPCWSTR sub) {
    if (r && !insideOwnCall() && buf && *buf) {
        const wchar_t* v = (const wchar_t*)*buf;
        if (wcsncmp(v, L"NVSDK_NGX_GPU_Arch_Blackwell", 28) == 0) {
            static int n = 0; if (n++ < 2) logf("gate: NGXGpuArchitecture '%ls' reported as '%ls'", v, kArchOld);
            *buf = (LPVOID)kArchOld; if (len) *len = (UINT)(sizeof(kArchOld) / sizeof(wchar_t));
        }
    }
    return r;
}
BOOL WINAPI hkVerQueryValueW(LPCVOID b, LPCWSTR sub, LPVOID* buf, PUINT len) { return fixArch(oVerQueryValueW(b, sub, buf, len), buf, len, sub); }
BOOL WINAPI hkVerQueryValueW2(LPCVOID b, LPCWSTR sub, LPVOID* buf, PUINT len) { return fixArch(oVerQueryValueW2(b, sub, buf, len), buf, len, sub); }

void patchVerBlock(LPVOID data, DWORD len);
// The core also reads NGXGpuArchitecture straight from the loaded module's version resource.
void patchLoadedVersionResource(HMODULE h) {
    if (!h) return;
    // The core asks the loaded snippet itself: NVSDK_NGX_GetGPUArchitecture is `mov eax, imm32; ret`.
    if (BYTE* f = (BYTE*)GetProcAddress(h, "NVSDK_NGX_GetGPUArchitecture")) {
        DWORD arch = 0; if (f[0] == 0xB8 && f[5] == 0xC3) memcpy(&arch, f + 1, 4);
        if (arch > 0x190) {
            DWORD old = 0, turing = 0x160;
            if (VirtualProtect(f, 8, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(f + 1, &turing, 4); VirtualProtect(f, 8, old, &old); FlushInstructionCache(GetCurrentProcess(), f, 8);
                logf("gate: NVSDK_NGX_GetGPUArchitecture 0x%lx -> 0x%lx", arch, turing);
            }
        } else if (f[0] != 0xB8) logf("gate: NVSDK_NGX_GetGPUArchitecture has an unexpected shape (%02x %02x), left alone", f[0], f[1]);
    }
    HRSRC r = FindResourceW(h, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (!r) { logf("gate: no version resource in %p", h); return; }
    HGLOBAL g = LoadResource(h, r); void* d = g ? LockResource(g) : nullptr; DWORD sz = SizeofResource(h, r);
    if (!d || !sz) return;
    DWORD old = 0;
    if (!VirtualProtect(d, sz, PAGE_READWRITE, &old)) { logf("gate: VirtualProtect on version resource failed %lu", GetLastError()); return; }
    patchVerBlock(d, sz);
    VirtualProtect(d, sz, old, &old);
}

HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (g_haveRt && name && !insideOwnCall() && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        size_t n = wcslen(name);
        if (n >= 16 && _wcsicmp(name + n - 16, L"nvngx_dlssnr.dll") == 0 && (n == 16 || name[n - 17] == L'\\' || name[n - 17] == L'/')) {
            HMODULE h = oLoadLibraryExW(g_rtPath, file, flags & ~(DWORD)LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
            static int c = 0; if (c++ < 4) logf("gate: LoadLibraryExW(%ls, flags=0x%lx) -> runtime %ls = %p (err %lu)", name, flags, g_rtPath, h, h ? 0 : GetLastError());
            if (h) { patchLoadedVersionResource(h); return h; }
        }
    }
    return oLoadLibraryExW(name, file, flags);
}

// The NGX core may read the version resource through any of these; patch the block itself.
void patchVerBlock(LPVOID data, DWORD len) {
    if (!data || len < 64 || insideOwnCall()) return;
    static const wchar_t from[] = L"NVSDK_NGX_GPU_Arch_Blackwell";
    const size_t fl = (sizeof(from) / sizeof(wchar_t)) - 1;
    wchar_t* w = (wchar_t*)data; size_t n = len / sizeof(wchar_t);
    for (size_t i = 0; i + fl + 2 < n; i++) {
        if (w[i] == L'N' && memcmp(w + i, from, fl * sizeof(wchar_t)) == 0) {
            size_t e = i; while (e < n && w[e]) e++;
            const size_t tl = (sizeof(kArchOld) / sizeof(wchar_t)) - 1;
            if (e - i >= tl) { memcpy(w + i, kArchOld, tl * sizeof(wchar_t)); for (size_t k = i + tl; k < e; k++) w[k] = 0; }
            static int c = 0; if (c++ < 3) logf("gate: version block NGXGpuArchitecture Blackwell -> Turing");
            return;
        }
    }
    static const char fromA[] = "NVSDK_NGX_GPU_Arch_Blackwell"; static const char toA[] = "NVSDK_NGX_GPU_Arch_Turing";
    char* a = (char*)data;
    for (size_t i = 0; i + sizeof(fromA) + 2 < len; i++) {
        if (a[i] == 'N' && memcmp(a + i, fromA, sizeof(fromA) - 1) == 0) {
            size_t e = i; while (e < len && a[e]) e++;
            memcpy(a + i, toA, sizeof(toA) - 1); for (size_t k = i + sizeof(toA) - 1; k < e; k++) a[k] = 0;
            static int c2 = 0; if (c2++ < 3) logf("gate: version block (ANSI) NGXGpuArchitecture Blackwell -> Turing");
            return;
        }
    }
}
#define VER_HOOK(ID) \
    using PF_W##ID = BOOL WINAPI(LPCWSTR, DWORD, DWORD, LPVOID); using PF_A##ID = BOOL WINAPI(LPCSTR, DWORD, DWORD, LPVOID); \
    using PF_XW##ID = BOOL WINAPI(DWORD, LPCWSTR, DWORD, DWORD, LPVOID); using PF_XA##ID = BOOL WINAPI(DWORD, LPCSTR, DWORD, DWORD, LPVOID); \
    PF_W##ID* oGfviW##ID = nullptr; PF_A##ID* oGfviA##ID = nullptr; PF_XW##ID* oGfviXW##ID = nullptr; PF_XA##ID* oGfviXA##ID = nullptr; \
    BOOL WINAPI hkGfviW##ID(LPCWSTR f, DWORD h, DWORD l, LPVOID d) { BOOL r = oGfviW##ID(f, h, l, d); if (r) patchVerBlock(d, l); return r; } \
    BOOL WINAPI hkGfviA##ID(LPCSTR f, DWORD h, DWORD l, LPVOID d) { BOOL r = oGfviA##ID(f, h, l, d); if (r) patchVerBlock(d, l); return r; } \
    BOOL WINAPI hkGfviXW##ID(DWORD fl, LPCWSTR f, DWORD h, DWORD l, LPVOID d) { BOOL r = oGfviXW##ID(fl, f, h, l, d); if (r) patchVerBlock(d, l); return r; } \
    BOOL WINAPI hkGfviXA##ID(DWORD fl, LPCSTR f, DWORD h, DWORD l, LPVOID d) { BOOL r = oGfviXA##ID(fl, f, h, l, d); if (r) patchVerBlock(d, l); return r; } \
    void attachVer##ID(HMODULE m, HMODULE other) { if (!m) return; \
        oGfviW##ID = (PF_W##ID*)GetProcAddress(m, "GetFileVersionInfoW"); oGfviA##ID = (PF_A##ID*)GetProcAddress(m, "GetFileVersionInfoA"); \
        oGfviXW##ID = (PF_XW##ID*)GetProcAddress(m, "GetFileVersionInfoExW"); oGfviXA##ID = (PF_XA##ID*)GetProcAddress(m, "GetFileVersionInfoExA"); \
        if (other) { if ((void*)oGfviW##ID == (void*)GetProcAddress(other, "GetFileVersionInfoW")) oGfviW##ID = nullptr; if ((void*)oGfviA##ID == (void*)GetProcAddress(other, "GetFileVersionInfoA")) oGfviA##ID = nullptr; \
                     if ((void*)oGfviXW##ID == (void*)GetProcAddress(other, "GetFileVersionInfoExW")) oGfviXW##ID = nullptr; if ((void*)oGfviXA##ID == (void*)GetProcAddress(other, "GetFileVersionInfoExA")) oGfviXA##ID = nullptr; } \
        if (oGfviW##ID) DetourAttach(&(PVOID&)oGfviW##ID, hkGfviW##ID); if (oGfviA##ID) DetourAttach(&(PVOID&)oGfviA##ID, hkGfviA##ID); \
        if (oGfviXW##ID) DetourAttach(&(PVOID&)oGfviXW##ID, hkGfviXW##ID); if (oGfviXA##ID) DetourAttach(&(PVOID&)oGfviXA##ID, hkGfviXA##ID); \
        logf("gate: version hooks " #ID ": W=%p A=%p ExW=%p ExA=%p", oGfviW##ID, oGfviA##ID, oGfviXW##ID, oGfviXA##ID); }
VER_HOOK(1)
VER_HOOK(2)

using PFun_VerQueryValueA = BOOL WINAPI(LPCVOID, LPCSTR, LPVOID*, PUINT);
PFun_VerQueryValueA* oVerQueryValueA = nullptr;
PFun_VerQueryValueA* oVerQueryValueA2 = nullptr;
BOOL fixArchA(BOOL r, LPVOID* buf, PUINT len) {
    static const char toA[] = "NVSDK_NGX_GPU_Arch_Turing";
    if (r && !insideOwnCall() && buf && *buf && strncmp((const char*)*buf, "NVSDK_NGX_GPU_Arch_Blackwell", 28) == 0) {
        static int n = 0; if (n++ < 3) logf("gate: VerQueryValueA '%s' reported as '%s'", (const char*)*buf, toA);
        *buf = (LPVOID)toA; if (len) *len = (UINT)sizeof(toA);
    }
    return r;
}
BOOL WINAPI hkVerQueryValueA(LPCVOID b, LPCSTR sub, LPVOID* buf, PUINT len) { return fixArchA(oVerQueryValueA(b, sub, buf, len), buf, len); }
BOOL WINAPI hkVerQueryValueA2(LPCVOID b, LPCSTR sub, LPVOID* buf, PUINT len) { return fixArchA(oVerQueryValueA2(b, sub, buf, len), buf, len); }

void installGateHooksImpl() {
    fill(g_rtPath, L"%s\\nvngx_dlssnr.dll", g_dir);
    g_haveRt = GetFileAttributesW(g_rtPath) != INVALID_FILE_ATTRIBUTES;
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    wchar_t sysDir[MAX_PATH] = L""; GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t sys[MAX_PATH + 16]; fill(sys, L"%s\\version.dll", sysDir);
    HMODULE ver = LoadLibraryW(sys);
    oVerQueryValueW = (PFun_VerQueryValueW*)GetProcAddress(kb, "VerQueryValueW");
    oVerQueryValueW2 = ver ? (PFun_VerQueryValueW*)GetProcAddress(ver, "VerQueryValueW") : nullptr;
    if (oVerQueryValueW2 == oVerQueryValueW) oVerQueryValueW2 = nullptr;
    oLoadLibraryExW = (PFun_LoadLibraryExW*)GetProcAddress(kb, "LoadLibraryExW");
    logf("gate: model file %ls present=%d", g_rtPath, g_haveRt);
    // Nothing is touched unless the user placed a runtime for an older GPU next to the game.
    if (!g_haveRt) return;
    DetourTransactionBegin();
    if (oVerQueryValueW) DetourAttach(&(PVOID&)oVerQueryValueW, hkVerQueryValueW);
    if (oVerQueryValueW2) DetourAttach(&(PVOID&)oVerQueryValueW2, hkVerQueryValueW2);
    if (oLoadLibraryExW) DetourAttach(&(PVOID&)oLoadLibraryExW, hkLoadLibraryExW);
    attachVer1(kb, nullptr); attachVer2(ver, kb);
    oVerQueryValueA = (PFun_VerQueryValueA*)GetProcAddress(kb, "VerQueryValueA");
    oVerQueryValueA2 = ver ? (PFun_VerQueryValueA*)GetProcAddress(ver, "VerQueryValueA") : nullptr;
    if (oVerQueryValueA2 == oVerQueryValueA) oVerQueryValueA2 = nullptr;
    if (oVerQueryValueA) DetourAttach(&(PVOID&)oVerQueryValueA, hkVerQueryValueA);
    if (oVerQueryValueA2) DetourAttach(&(PVOID&)oVerQueryValueA2, hkVerQueryValueA2);
    logf("gate: VerQueryValueA kb=%p ver=%p", oVerQueryValueA, oVerQueryValueA2);
    logf("gate: hooks installed: %ld", DetourTransactionCommit());
}
} // namespace

void installGateHooks() { installGateHooksImpl(); }

} // namespace d5
