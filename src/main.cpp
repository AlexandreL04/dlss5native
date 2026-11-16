// dlss5native - runs NVIDIA's DLSS 5 model right after the game's own DLSS, in any DirectX 12 game that
// uses DLSS: the hooks sit on NVIDIA's NGX core, which every DLSS game calls (ngx.cpp). Loaded as a
// hid.dll proxy next to the game's executable.
#include "common.h"
#include <winternl.h>
#include "hid_forward.h"

namespace d5 {

wchar_t g_dir[MAX_PATH];

namespace {

FILE* g_log = nullptr;
std::mutex g_logMx;

// Loader notification, so the hooks are in place as soon as the NGX core is loaded.
struct LdrNotification { ULONG Flags; const UNICODE_STRING* FullDllName; const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID(CALLBACK* LdrNotificationFn)(ULONG reason, const LdrNotification* data, PVOID ctx);
typedef NTSTATUS(NTAPI* LdrRegisterDllNotificationFn)(ULONG, LdrNotificationFn, PVOID, PVOID*);

VOID CALLBACK onDll(ULONG reason, const LdrNotification* d, PVOID) {
    if (reason != 1 || !d || !d->BaseDllName || !d->BaseDllName->Buffer) return;
    const wchar_t* name = d->BaseDllName->Buffer; const size_t len = d->BaseDllName->Length / sizeof(wchar_t);
    onModuleLoaded((HMODULE)d->DllBase, name, len);
}

void start(HMODULE self) {
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\'); exeName = exeName ? exeName + 1 : exe;
    if (_wcsnicmp(exeName, L"crs-", 4) == 0 || wcsstr(exeName, L"CrashReport") || wcsstr(exeName, L"crashpad")) return; // helper processes
    wchar_t lp[MAX_PATH]; fill(lp, L"%s\\dlss5native.log", g_dir);
    g_log = _wfsopen(lp, L"w", _SH_DENYNO);
    logf("dlss5native " D5_VERSION " loaded in %ls", exeName);
    loadSettings();
    installGateHooks();
    presentWatchStart();
    if (HMODULE h = GetModuleHandleW(L"_nvngx.dll")) onModuleLoaded(h, L"_nvngx.dll", 10);
    auto reg = (LdrRegisterDllNotificationFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie = nullptr;
    if (reg) logf("module notification registered: 0x%lx", reg(0, onDll, nullptr, &cookie));
}

} // namespace

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(g_logMx);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

} // namespace d5

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); d5::start(h); }
    return TRUE;
}
