// Compiles every colour-stage shader in both variants; exits non-zero on the first error.
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include "../src/shaders.h"
int main() {
    const char* entries[] = {"Measure", "ExposureScale", "Encode", "Shape", "Resolve", "ResolveUp", "Reduce", "Commit", "Overlay"};
    int bad = 0;
    for (int ip = 0; ip < 2; ip++)
        for (const char* e : entries) {
            const D3D_SHADER_MACRO defs[] = {{"IN_PLACE", ip ? "1" : "0"}, {nullptr, nullptr}};
            ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
            HRESULT hr = D3DCompile(kColorStageHLSL, strlen(kColorStageHLSL), "colorstage", defs, nullptr, e, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
            printf("%-14s in_place=%d %s %zu bytes\n", e, ip, SUCCEEDED(hr) ? "ok" : "FAILED", code ? code->GetBufferSize() : 0);
            if (err) { printf("%s\n", (const char*)err->GetBufferPointer()); }
            if (FAILED(hr)) bad++;
        }
    return bad;
}
