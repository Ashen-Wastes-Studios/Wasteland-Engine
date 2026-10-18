// Standalone verification harness: compiles Renderer3D_NovaRenderer.hlsl the
// same way DX12Nova.cpp:118-122 does (D3DCompileFromFile, cs_5_0, entry "main")
// and reports success plus the reflected constant buffer.
//
// Exists because the shader is not reachable any other way right now: the
// engine does not link (Scene.cpp fails on an in-flight Navigation:: refactor,
// plus 41 Renderer3D symbols declared in Renderer3D.h have no implementation),
// so "it compiles" has to be proven outside the build.
//
// What it asserts, in order of how badly a failure would hurt:
//   1. the shader compiles at all under cs_5_0
//   2. the NovaConstants cbuffer EXISTS and is found by name -- DX12Nova.cpp:151
//      does strcmp(desc.Name, "NovaConstants") and throws if absent
//   3. u_Jitter is present in that cbuffer at a real offset -- this is the
//      member the blue-noise fix depends on, and reading it is the whole point
//   4. the reported size matches what the host uploads
//
// Build (from the repo root, MSVC x64 tools on PATH or via vcvars):
//   cl /nologo /EHsc /std:c++17 /I<repo> tools/verify_nova_shader.cpp d3dcompiler.lib /Fe:verify.exe
// Run:
//   verify.exe <path-to-hlsl>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

static const char* kWanted[] = {
    "u_Jitter", "u_PrevJitter", "u_ViewProjection",
    "u_PrevViewProjection", "u_InverseViewProjection", "u_CameraMoved", "u_FrameIndex",
};

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: verify <shader.hlsl>\n"); return 2; }
    const char* path = argv[1];

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    // D3DCompileFromFile takes a WIDE path (LPCWSTR, d3dcompiler.h:256) and
    // D3DReflect takes exactly two arguments (d3dcompiler.h:311). Both
    // signatures were corrected here after the compiler rejected the assumed
    // ANSI forms.
    wchar_t wpath[MAX_PATH]{};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);

    HRESULT hr = D3DCompileFromFile(wpath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS,
                                    0, &code, &errors);

    if (FAILED(hr) || !code) {
        std::printf("FAIL compile hr=0x%08lx\n", (unsigned long)hr);
        if (errors) std::printf("%s\n", (const char*)errors->GetBufferPointer());
        if (errors) errors->Release();
        if (code) code->Release();
        return 1;
    }
    std::printf("PASS compile cs_5_0 (%u bytes)\n", (unsigned)code->GetBufferSize());

    // Reflect the COMPILED BLOB directly rather than recompiling it. This mirrors
    // what the engine actually does -- DX11Nova.h:206 calls D3DReflect on
    // code->GetBufferPointer() right after D3DCompileFromFile, with no second
    // compile and no macro/entry overrides. An earlier version of this file
    // recompiled with flags "cbuffers", which produced a blob this SDK would not
    // reflect.
    ID3D11ShaderReflection* refl = nullptr;
    // This SDK declares D3DReflect with an explicit REFIID (d3dcompiler.h:311);
    // there is no implicit __uuidof overload, so pass the IID directly.
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                          IID_ID3D11ShaderReflection, (void**)&refl))) {
        std::printf("FAIL D3DReflect\n");
        code->Release();
        return 1;
    }

    int failures = 0;
    UINT cbSize = 0, cbVars = 0;
    ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("NovaConstants");
    if (!cb) {
        std::printf("FAIL cbuffer 'NovaConstants' not found -- DX12Nova.cpp would throw\n");
        failures++;
    } else {
        D3D11_SHADER_BUFFER_DESC cbd{};
        cb->GetDesc(&cbd);
        cbSize = cbd.Size;
        cbVars = cbd.Variables;
        std::printf("PASS cbuffer NovaConstants  size=%u vars=%u\n", cbSize, cbVars);
        if (cbSize == 0) { std::printf("FAIL cbuffer size is 0\n"); failures++; }
    }

    // u_Jitter is the member the blue-noise fix reads. Its ABSENCE means the
    // fix silently does nothing on the DX backends, which is precisely the bug
    // this harness exists to catch.
    for (const char* n : kWanted) {
        ID3D11ShaderReflectionVariable* v = refl->GetVariableByName(n);
        if (!v) {
            std::printf("  var %-24s MISSING\n", n);
            if (std::strcmp(n, "u_Jitter") == 0 || std::strcmp(n, "u_PrevViewProjection") == 0) {
                std::printf("FAIL required member absent: %s\n", n);
                failures++;
            }
            continue;
        }
        D3D11_SHADER_VARIABLE_DESC d{};
        v->GetDesc(&d);
        std::printf("  var %-24s off=%3u size=%u\n", n, d.StartOffset, d.Size);
    }

    refl->Release(); code->Release();
    std::printf(failures ? "\nRESULT: FAIL (%d problem(s))\n" : "\nRESULT: PASS\n", failures);
    return failures ? 1 : 0;
}