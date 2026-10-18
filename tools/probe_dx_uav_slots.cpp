// Standalone probe: how many compute UAV slots does DX11 actually offer on THIS
// machine, and can a dispatch rebind a different set between passes?
//
// Why this exists: the Nova compute shader occupies UAV registers u0..u7 (all
// eight) and the backend binds CSSetUnorderedAccessViews(0, 8, ...)
// (DX11Nova.h:371 and :477). A planned denoiser wants a NINTH UAV (packed
// mean / mean-of-squares moments). Whether a 9th is legal was guessed from the
// D3D11 spec table (8 at FL 11_0, 64 at FL 11_1). This file replaces the guess
// with a measurement.
//
// Method note -- why empirical and not a spec quote:
//   D3D11 exposes NO UAV-count query. Checked the real headers:
//     - d3d11.h D3D11_SDK_VERSION == 7, and the D3D11_FEATURE enum terminates at
//       D3D11_FEATURE_D3D11_OPTIONS6, whose payload is only
//       ShaderAccessRestrictedResourceTier (d3d11.h:7545; struct near :7742).
//     - The single UAV constant is D3D11_1_UAV_SLOT_COUNT == 64 (d3d11.h:1054).
//       D3D11_UAV_SLOT_COUNT is NOT DEFINED anywhere in the SDK.
//   D3D12 is the same story: D3D12_FEATURE_DATA_D3D12_OPTIONS (d3d12.h:2520)
//   carries ResourceBindingTier and TypedUAVLoadAdditionalFormats but NO
//   per-stage UAV count field.
//   So the only trustworthy answer is to bind N, dispatch, then READ BACK the
//   UAV contents to prove the write actually landed. CSSetUnorderedAccessViews
//   returns void -- there is no HRESULT to check -- which is exactly why a
//   sentinel readback is required rather than trusting the bind call.
//
// Build: see build_probe_uav.bat
// Run:   probe_dx_uav_slots.exe [path\to\nova_uav64.dxil]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const int kMaxN = 64;   // D3D11_1_UAV_SLOT_COUNT
static const int kTexDim = 1;  // 1x1 textures: only slot count matters

// Globals so the side-by-side table can report DX11 results next to DX12.
struct SweepResult {
    int firstCompileFail = -1;
    int firstBindFail = -1;
    std::string compileErr;
};
static SweepResult g50, g51;

// ---------------------------------------------------------------- helpers ---

static const char* FLName(D3D_FEATURE_LEVEL f)
{
    switch (f) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default:                     return "other/lower";
    }
}

// Builds HLSL declaring n UAVs at u0..u(n-1). Each UAV is written a distinct
// sentinel (slot index + 1) so a readback can prove that SPECIFIC slot was
// really bound and really written. Register space is the thing under test, so
// this must be generated rather than hardcoded.
static std::string MakeHlsl(int n)
{
    std::string s;
    s += "RWTexture2D<float4> u0 : register(u0);\n";
    for (int i = 1; i < n; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "RWTexture2D<float4> u%d : register(u%d);\n", i, i);
        s += line;
    }
    s += "[numthreads(1,1,1)]\n";
    s += "void main(uint3 id : SV_DispatchThreadID)\n{\n";
    s += "  if (id.x != 0) return;\n";
    for (int i = 0; i < n; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "  u%d[uint2(0,0)] = float4(%d,0,0,0);\n", i, i + 1);
        s += line;
    }
    s += "}\n";
    return s;
}

struct TexSet {
    ID3D11Texture2D* tex[kMaxN] = {};
    ID3D11UnorderedAccessView* uav[kMaxN] = {};
    int count = 0;
};

static bool CreateTexSet(ID3D11Device* dev, TexSet& ts, int n)
{
    for (int i = 0; i < n; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kTexDim; d.Height = kTexDim;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &ts.tex[i]))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(ts.tex[i], &ud, &ts.uav[i]))) return false;
    }
    ts.count = n;
    return true;
}

static void DestroyTexSet(TexSet& ts)
{
    for (int i = 0; i < ts.count; ++i) {
        if (ts.uav[i]) ts.uav[i]->Release();
        if (ts.tex[i]) ts.tex[i]->Release();
        ts.uav[i] = nullptr; ts.tex[i] = nullptr;
    }
    ts.count = 0;
}

// Copies a UAV texture to staging and reads it back. This is the proof step: a
// bind that "succeeded" but left the slot unwired shows up as a sentinel
// mismatch here, not as an error anywhere.
static bool Readback11(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                       ID3D11Texture2D* src, float out[4])
{
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
    ctx->CopyResource(staging, src);

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { staging->Release(); return false; }
    std::memcpy(out, m.pData, sizeof(float) * 4);
    ctx->Unmap(staging, 0);
    staging->Release();
    return true;
}

// ---------------------------------------------------------- DX11 UAV sweep ---
//
// Sweeps n = 1..64. For each n, per profile:
//   1. compile a shader declaring n UAVs  -> first compile failure bounds the
//      SHADER-side limit (register space available in the bytecode)
//   2. if it compiled, bind n UAVs + dispatch + read back -> first mismatch or
//      device removal bounds the RUNTIME-side limit
// The two limits are independent and can differ; BOTH matter for a 9th UAV.
// Note the engine compiles cs_5_0 today (DX11Nova.h:191, DX12Nova.cpp:122), so
// cs_5_0 is the profile that actually governs the Nova shader.

static void SweepProfile(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         const char* profile, SweepResult& out)
{
    TexSet ts;
    if (!CreateTexSet(dev, ts, kMaxN)) {
        std::printf("  [%s] FATAL: could not create %d UAV textures\n", profile, kMaxN);
        return;
    }

    // Poison every target so a never-bound slot reads -1 rather than a
    // coincidental match. Re-poisoning each iteration is what makes the
    // readback airtight.
    const float poison[4] = { -1.f, -1.f, -1.f, -1.f };
    for (int i = 0; i < kMaxN; ++i) ctx->ClearUnorderedAccessViewFloat(ts.uav[i], poison);

    std::printf("  --- profile %s ---\n", profile);
    std::printf("   n   compile    dispatch + sentinel readback\n");

    for (int n = 1; n <= kMaxN; ++n) {
        std::string src = MakeHlsl(n);

        ID3DBlob* code = nullptr;
        ID3DBlob* errs = nullptr;
        HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr,
                                "main", profile, D3DCOMPILE_ENABLE_STRICTNESS,
                                0, &code, &errs);
        if (FAILED(hr) || !code) {
            if (out.firstCompileFail < 0) {
                out.firstCompileFail = n;
                if (errs) out.compileErr = (const char*)errs->GetBufferPointer();
            }
            std::printf("  %3d  FAIL       -\n", n);
            if (errs) errs->Release();
            if (code) code->Release();
            continue;
        }
        if (errs) errs->Release();

        ID3D11ComputeShader* cs = nullptr;
        hr = dev->CreateComputeShader(code->GetBufferPointer(),
                                      code->GetBufferSize(), nullptr, &cs);
        code->Release();
        if (FAILED(hr)) {
            if (out.firstCompileFail < 0) out.firstCompileFail = n;
            std::printf("  %3d  CreateCS FAILED hr=0x%08lx\n", n, (unsigned long)hr);
            continue;
        }

        // Re-poison, then bind n UAVs at slot 0 and dispatch.
        // CSSetUnorderedAccessViews returns void, so nothing here reports
        // failure -- the readback below is what decides.
        for (int i = 0; i < n; ++i) ctx->ClearUnorderedAccessViewFloat(ts.uav[i], poison);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, n, ts.uav, nullptr);
        ctx->Dispatch(1, 1, 1);

        HRESULT removed = dev->GetDeviceRemovedReason();
        if (removed != S_OK) {
            out.firstBindFail = n;
            std::printf("  %3d  ok         DEVICE REMOVED hr=0x%08lx\n", n, (unsigned long)removed);
            cs->Release();
            break;
        }

        // Verify every slot this shader declared actually got written.
        int bad = -1;
        float got[4] = { 0,0,0,0 };
        for (int i = 0; i < n; ++i) {
            if (!Readback11(dev, ctx, ts.tex[i], got)) { bad = i; break; }
            if (got[0] != (float)(i + 1)) { bad = i; break; }
        }
        if (bad >= 0) {
            out.firstBindFail = n;
            std::printf("  %3d  ok         FAIL at u%d: read %.1f, want %.1f\n",
                        n, bad, got[0], (float)(bad + 1));
            cs->Release();
            break;
        }

        std::printf("  %3d  ok         ok (all %d slots written)\n", n, n);
        cs->Release();
    }

    DestroyTexSet(ts);
}

// Report the effective max: whichever bound bit first.
static int EffMax(const SweepResult& r)
{
    if (r.firstCompileFail >= 0) return r.firstCompileFail - 1;
    if (r.firstBindFail >= 0)    return r.firstBindFail - 1;
    return kMaxN;
}

static void ReportProfile(const char* label, const SweepResult& r)
{
    std::printf("  %-34s : ", label);
    if (r.firstCompileFail >= 0)
        std::printf("compile FAIL at n=%d -> max usable = %d", r.firstCompileFail, r.firstCompileFail - 1);
    else if (r.firstBindFail >= 0)
        std::printf("compiles all %d, bind FAIL at n=%d -> max = %d", kMaxN, r.firstBindFail, r.firstBindFail - 1);
    else
        std::printf("all %d UAVs compiled, bound, dispatched, verified", kMaxN);
    std::printf("\n");
}

// ------------------------------------------- DX11 per-pass UAV slot swapping ---
//
// The design question: can a dispatch bind a DIFFERENT set of UAVs than the
// previous dispatch, so a denoiser pass reuses an existing slot rather than
// needing a 9th? Two distinct 8-resource sets are bound to the SAME eight slots
// across two dispatches, and the sentinels prove which set each dispatch hit.

static void SlotSwapTest(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const int kSetA = 0;   // textures[0..7]
    const int kSetB = 32;  // textures[32..39]

    TexSet ts;
    if (!CreateTexSet(dev, ts, kMaxN)) {
        std::printf("SLOTSWAP FATAL: texture creation failed\n");
        return;
    }

    const float poison[4] = { -1.f, -1.f, -1.f, -1.f };
    for (int i = 0; i < kMaxN; ++i) ctx->ClearUnorderedAccessViewFloat(ts.uav[i], poison);

    const char* src =
        "RWTexture2D<float4> u0 : register(u0);\n"
        "RWTexture2D<float4> u1 : register(u1);\n"
        "RWTexture2D<float4> u2 : register(u2);\n"
        "RWTexture2D<float4> u3 : register(u3);\n"
        "RWTexture2D<float4> u4 : register(u4);\n"
        "RWTexture2D<float4> u5 : register(u5);\n"
        "RWTexture2D<float4> u6 : register(u6);\n"
        "RWTexture2D<float4> u7 : register(u7);\n"
        "[numthreads(1,1,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "  if (id.x != 0) return;\n"
        "  u0[uint2(0,0)]=float4(101,0,0,0); u1[uint2(0,0)]=float4(102,0,0,0);\n"
        "  u2[uint2(0,0)]=float4(103,0,0,0); u3[uint2(0,0)]=float4(104,0,0,0);\n"
        "  u4[uint2(0,0)]=float4(105,0,0,0); u5[uint2(0,0)]=float4(106,0,0,0);\n"
        "  u6[uint2(0,0)]=float4(107,0,0,0); u7[uint2(0,0)]=float4(108,0,0,0);\n"
        "}\n";

    ID3DBlob* code = nullptr; ID3DBlob* errs = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        std::printf("SLOTSWAP FATAL: compile failed 0x%08lx\n", (unsigned long)hr);
        if (errs) std::printf("%s\n", (const char*)errs->GetBufferPointer());
        if (errs) errs->Release();
        if (code) code->Release();
        DestroyTexSet(ts);
        return;
    }
    if (errs) errs->Release();

    ID3D11ComputeShader* cs = nullptr;
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    code->Release();
    if (FAILED(hr)) {
        std::printf("SLOTSWAP FATAL: CreateComputeShader 0x%08lx\n", (unsigned long)hr);
        DestroyTexSet(ts);
        return;
    }

    ctx->CSSetShader(cs, nullptr, 0);

    // Pass 1: slots 0..7 -> set A.
    ID3D11UnorderedAccessView* setA[8];
    for (int i = 0; i < 8; ++i) setA[i] = ts.uav[kSetA + i];
    ctx->CSSetUnorderedAccessViews(0, 8, setA, nullptr);
    ctx->Dispatch(1, 1, 1);

    // Pass 2: SAME slots 0..7 -> a completely different set of 8 resources.
    ID3D11UnorderedAccessView* setB[8];
    for (int i = 0; i < 8; ++i) setB[i] = ts.uav[kSetB + i];
    ctx->CSSetUnorderedAccessViews(0, 8, setB, nullptr);
    ctx->Dispatch(1, 1, 1);

    HRESULT removed = dev->GetDeviceRemovedReason();

    // Pass 2 must have hit set B; set A must NOT have been re-touched by it.
    // If slot contents were sticky across dispatches, set B would read -1.
    bool bHit[8], aIntact[8];
    float got[4];
    for (int i = 0; i < 8; ++i) {
        Readback11(dev, ctx, ts.tex[kSetB + i], got);
        bHit[i] = (got[0] == (float)(101 + i));
        Readback11(dev, ctx, ts.tex[kSetA + i], got);
        aIntact[i] = (got[0] == (float)(101 + i));
    }

    std::printf("deviceRemoved                = 0x%08lx\n", (unsigned long)removed);
    Readback11(dev, ctx, ts.tex[kSetA + 0], got);
    std::printf("after both passes, setA[0]    = %.0f  (want 101: pass1 wrote it)\n", got[0]);
    Readback11(dev, ctx, ts.tex[kSetB + 0], got);
    std::printf("after both passes, setB[0]    = %.0f  (want 101: pass2 wrote it)\n", got[0]);

    bool ok = (removed == S_OK);
    for (int i = 0; i < 8; ++i) ok = ok && bHit[i] && aIntact[i];

    std::printf("setB[0..7] all written by pass2 : %s\n", (bHit[0] && bHit[7]) ? "YES" : "NO");
    std::printf("setA[0..7] untouched by pass2   : %s\n", (aIntact[0] && aIntact[7]) ? "YES" : "NO");
    std::printf("SLOTSWAP RESULT: %s\n",
                ok ? "PASS -- a dispatch CAN rebind the same 8 slots to a different\n"
                     "       set of 8 UAVs; slot meaning is per-bind, not sticky."
                   : "FAIL");

    cs->Release();
    DestroyTexSet(ts);
}

// ------------------------------------------------------------- DX12 probe ---
//
// Side-by-side. DX12 has no UAV-count query either, so the same empirical
// approach: a descriptor table of N UAVs, dispatch, read back through a readback
// heap. Needs a cs_6_0 DXIL container (SM 5.1 DXBC cannot declare more than 8
// UAVs), compiled ahead of time by dxc.exe into nova_uav64.dxil.

static int DX12Probe(const char* dxilPath)
{
    std::printf("=== DX12 (side-by-side) ===\n");

    IDXGIFactory4* f4 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f4)))) {
        std::printf("DX12: CreateDXGIFactory1 failed\n");
        return -1;
    }
    IDXGIAdapter1* ad = nullptr;
    if (FAILED(f4->EnumAdapters1(0, &ad))) {
        std::printf("DX12: EnumAdapters1 failed\n");
        f4->Release();
        return -1;
    }
    DXGI_ADAPTER_DESC1 adesc{};
    ad->GetDesc1(&adesc);
    std::printf("adapter: %ls\n", adesc.Description);

    struct { D3D_FEATURE_LEVEL fl; const char* name; } tries[] = {
        { D3D_FEATURE_LEVEL_12_1, "12_1" },
        { D3D_FEATURE_LEVEL_12_0, "12_0" },
        { D3D_FEATURE_LEVEL_11_0, "11_0" },
    };
    ID3D12Device* dev = nullptr;
    for (auto& t : tries) {
        if (SUCCEEDED(D3D12CreateDevice(ad, t.fl, IID_PPV_ARGS(&dev)))) {
            std::printf("device created at FL %s\n", t.name);
            break;
        }
    }
    ad->Release();
    if (!dev) {
        std::printf("DX12: D3D12CreateDevice failed for all feature levels\n");
        f4->Release();
        return -1;
    }

    // The closest thing DX12 has to a declared limit.
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        std::printf("D3D12_FEATURE_DATA_D3D12_OPTIONS.ResourceBindingTier = %d\n",
                    (int)opts.ResourceBindingTier);
        std::printf("  (no per-stage UAV count field exists in this struct)\n");
    }

    FILE* f = nullptr;
    if (fopen_s(&f, dxilPath, "rb") != 0 || !f) {
        std::printf("DX12: cannot open DXIL '%s' -- skipping empirical sweep\n", dxilPath);
        dev->Release(); f4->Release();
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> il((size_t)(len > 0 ? len : 0));
    size_t nread = il.empty() ? 0 : fread(il.data(), 1, il.size(), f);
    fclose(f);
    std::printf("DXIL loaded: %s (%zu bytes)\n", dxilPath, nread);

    D3D12_SHADER_BYTECODE sb{};
    sb.pShaderBytecode = il.data();
    sb.BytecodeLength = nread;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ID3D12CommandQueue* queue = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc));

    // ID3D12CommandList in this SDK only declares GetType (d3d12.h:6208) --
    // every recording method (Close/Reset/Dispatch/CopyTextureRegion/...)
    // lives on ID3D12GraphicsCommandList. Request that interface.
    ID3D12GraphicsCommandList* list = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, nullptr,
                           IID_PPV_ARGS(&list));
    list->Close();

    // 64 UAV textures + 64 readback buffers.
    const int N = kMaxN;
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readHeap{};
    readHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd0{};
    rd0.Width = kTexDim; rd0.Height = kTexDim; rd0.DepthOrArraySize = 1;
    rd0.MipLevels = 1; rd0.SampleDesc.Count = 1; rd0.SampleDesc.Quality = 1;
    rd0.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    // TEXTURE_LAYOUT_UNKNOWN is legal only for BUFFER resources; a texture must
    // declare ROW_MAJOR or CreateCommittedResource returns E_INVALIDARG.
    rd0.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_CLEAR_VALUE zero{};
    zero.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    ID3D12Resource* uavTex[kMaxN] = {};
    ID3D12Resource* readTex[kMaxN] = {};
    bool allocOk = true;
    for (int i = 0; i < N; ++i) {
        D3D12_RESOURCE_DESC d = rd0;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               &zero, IID_PPV_ARGS(&uavTex[i])))) { allocOk = false; break; }
        D3D12_RESOURCE_DESC r2 = rd0;
        r2.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &r2,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readTex[i])))) { allocOk = false; break; }
    }
    if (!allocOk) {
        std::printf("DX12: resource allocation failed\n");
        for (int i = 0; i < N; ++i) { if (uavTex[i]) uavTex[i]->Release(); if (readTex[i]) readTex[i]->Release(); }
        list->Release(); alloc->Release(); queue->Release(); dev->Release(); f4->Release();
        return -1;
    }

    // UAV descriptor heap with N UAV descriptors.
    // Field names verified against d3d12.h's D3D12_DESCRIPTOR_RANGE
    // (RangeType, NumDescriptors, BaseShaderRegister, RegisterSpace,
    //  OffsetInDescriptorsFromTableStart) -- there is no
    // BaseShaderVisibleInDescriptorHeapIndex on this struct.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = N;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = N;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));

    D3D12_ROOT_DESCRIPTOR_TABLE table{};
    table.NumDescriptorRanges = 1;
    table.pDescriptorRanges = &range;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = table;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3D12RootSignature* rsig = nullptr;
    // Serialize the root signature explicitly and use the 5-arg blob overload.
    // This header declares only the blob form of CreateRootSignature as a virtual
    // (d3d12.h:9047); the desc+version convenience overload is not declared here,
    // so calling that form is a hard compile error rather than a silent ABI risk.
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                             &rsBlob, &rsErr);
    if (FAILED(hr) || !rsBlob) {
        std::printf("DX12: D3D12SerializeRootSignature failed 0x%08lx\n", (unsigned long)hr);
        if (rsErr) std::printf("%s\n", (const char*)rsErr->GetBufferPointer());
        if (rsErr) rsErr->Release();
        return -1;
    }
    hr = dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                  IID_PPV_ARGS(&rsig));
    rsBlob->Release();
    if (FAILED(hr)) {
        std::printf("DX12: CreateRootSignature failed 0x%08lx\n", (unsigned long)hr);
        return -1;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rsig;
    pso.CS = sb;
    ID3D12PipelineState* pipe = nullptr;
    hr = dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipe));
    if (FAILED(hr)) {
        std::printf("DX12: CreateComputePipelineState failed 0x%08lx\n", (unsigned long)hr);
        return -1;
    }

    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceVal = 0;

    std::printf("  n   descriptor-table UAVs bound + dispatch + readback\n");
    int firstFail = -1;
    std::vector<D3D12_RESOURCE_BARRIER> bar(N);
    std::vector<D3D12_TEXTURE_COPY_LOCATION> dst(N);

    for (int n = 1; n <= N; ++n) {
        alloc->Reset();
        list->Reset(alloc, pipe);

        list->SetDescriptorHeaps(1, &heap);
        list->SetComputeRootSignature(rsig);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch(1, 1, 1);

        for (int i = 0; i < n; ++i) {
            bar[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar[i].Transition.pResource = uavTex[i];
            bar[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            bar[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

            dst[i].pResource = readTex[i];
            dst[i].Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst[i].SubresourceIndex = 0;
        }
        list->ResourceBarrier((UINT)n, bar.data());
        for (int i = 0; i < n; ++i) {
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = uavTex[i];
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;
            // Both src and dst are D3D12_TEXTURE_COPY_LOCATION*, not resources.
            list->CopyTextureRegion(&dst[i], 0, 0, 0, &srcLoc, nullptr);
        }
        list->Close();

        // ExecuteCommandLists takes ID3D12CommandList**, not the graphics subtype.
        ID3D12CommandList* lists[1] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fenceVal);
        if (fence->GetCompletedValue() < fenceVal) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(fenceVal, ev);
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
        }

        int bad = -1;
        float val = 0;
        for (int i = 0; i < n; ++i) {
            // Signature is Map(Subresource, const D3D12_RANGE* pReadRange,
            //                  void** ppData) -- range first, data pointer last.
            void* p = nullptr;
            if (FAILED(readTex[i]->Map(0, nullptr, &p))) { bad = i; break; }
            val = *(float*)p;
            readTex[i]->Unmap(0, nullptr);
            if (val != (float)(i + 1)) { bad = i; break; }
        }

        if (bad >= 0) {
            firstFail = n;
            std::printf("  %3d  FAIL at u%d: read %.1f, want %.1f\n",
                        n, bad, val, (float)(bad + 1));
            break;
        }
        std::printf("  %3d  ok (all %d slots written)\n", n, n);
    }

    int maxD12 = (firstFail < 0) ? N : firstFail - 1;
    std::printf("DX12 MEASURED MAX COMPUTE UAVs (descriptor table) = %d%s\n",
                maxD12, firstFail < 0 ? " (all passed)" : "");
    std::printf("NOTE: D3D12 exposes no per-stage UAV count field; the number above\n"
                "      is measured by binding and readback, not read from a struct.\n");

    for (int i = 0; i < N; ++i) { uavTex[i]->Release(); readTex[i]->Release(); }
    pipe->Release(); rsig->Release(); heap->Release(); fence->Release();
    list->Release(); alloc->Release(); queue->Release();
    dev->Release(); f4->Release();
    return maxD12;
}

// ------------------------------------------------- NEGATIVE CONTROL (crucial) ---
//
// The sweep above reports "all 64 slots written". That claim is worthless unless
// the readback can actually FAIL when a slot is not written. This proves it can:
// bind 64 UAVs but run a shader that declares and writes only u0. Slots u1..u63
// stay poisoned at -1, and the checker must report exactly that. If this control
// ever reports "ok", the whole sweep is a false positive and the 64 result must
// be discarded.

static void NegativeControl(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    std::printf("\n=== NEGATIVE CONTROL: can the readback detect an unwritten slot? ===\n");

    TexSet ts;
    if (!CreateTexSet(dev, ts, kMaxN)) {
        std::printf("NEGCTL FATAL: texture creation failed\n");
        return;
    }
    const float poison[4] = { -1.f, -1.f, -1.f, -1.f };
    for (int i = 0; i < kMaxN; ++i) ctx->ClearUnorderedAccessViewFloat(ts.uav[i], poison);

    // Shader writes ONLY u0, but we bind 64 UAVs.
    const char* src =
        "RWTexture2D<float4> u0 : register(u0);\n"
        "[numthreads(1,1,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "  u0[uint2(0,0)] = float4(1,0,0,0);\n"
        "}\n";

    ID3DBlob* code = nullptr; ID3DBlob* errs = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        std::printf("NEGCTL: compile failed 0x%08lx\n", (unsigned long)hr);
        if (errs) errs->Release();
        if (code) code->Release();
        DestroyTexSet(ts);
        return;
    }
    if (errs) errs->Release();

    ID3D11ComputeShader* cs = nullptr;
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    code->Release();
    if (FAILED(hr)) {
        std::printf("NEGCTL: CreateComputeShader failed 0x%08lx\n", (unsigned long)hr);
        DestroyTexSet(ts);
        return;
    }

    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, kMaxN, ts.uav, nullptr);  // 64 bound...
    ctx->Dispatch(1, 1, 1);                                      // ...1 written

    int firstStrayWrite = -1;   // first slot != u0 that is NOT still at poison
    float got[4];
    for (int i = 0; i < kMaxN; ++i) {
        Readback11(dev, ctx, ts.tex[i], got);
        if (i == 0) {
            std::printf("  u0  = %.1f (want 1)   %s\n", got[0], got[0] == 1.f ? "ok" : "UNEXPECTED");
        } else if (got[0] != poison[0] && firstStrayWrite < 0) {
            firstStrayWrite = i;
        }
    }

    // The control PASSES when NO slot beyond u0 was written: the shader declared
    // only u0, so u1..u63 must all still read the poison value. A stray write
    // would mean the readback cannot be trusted.
    bool ok = (firstStrayWrite == -1);
    std::printf("  bound 64 UAVs, shader declared+wrote only u0.\n");
    std::printf("  u1 still at poison -1: %s\n", ok ? "YES" : "NO");
    std::printf("  first stray written slot: u%d\n", firstStrayWrite);
    std::printf("NEGATIVE CONTROL: %s\n",
                ok ? "PASS -- u0 written, every other slot left at poison -1. The\n"
                     "         readback genuinely detects an unwritten slot, so the\n"
                     "         64-UAV sweep result is a real positive, not vacuous."
                   : "FAIL -- a slot the shader never wrote shows a value other than\n"
                     "         poison; the readback cannot be trusted.");

    cs->Release();
    DestroyTexSet(ts);
}

// ------------------------------------------------------------------- main ---

int main(int argc, char** argv)
{
    const char* dxilPath = (argc > 1) ? argv[1] : "nova_uav64.dxil";

    std::printf("=====================================================\n");
    std::printf(" DX11/DX12 compute UAV slot probe\n");
    std::printf("=====================================================\n\n");

    IDXGIFactory4* f4 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f4)))) {
        std::printf("FATAL: CreateDXGIFactory1 failed\n");
        return 1;
    }

    std::printf("=== adapters ===\n");
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; f4->EnumAdapters1(i, &ad) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        std::printf("  [%u] %ls  (vendor 0x%04x device 0x%04x)\n",
                    i, d.Description, d.VendorId, d.DeviceId);
        ad->Release();
        ad = nullptr;
    }

    ad = nullptr;
    f4->EnumAdapters1(0, &ad);
    DXGI_ADAPTER_DESC1 adesc{};
    ad->GetDesc1(&adesc);

    // 1. Highest feature level this device will actually give us.
    D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   fls, (UINT)(sizeof(fls) / sizeof(fls[0])),
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {
        std::printf("FATAL: D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr);
        ad->Release(); f4->Release();
        return 1;
    }

    std::printf("\n=== DX11 device ===\n");
    std::printf("adapter:              %ls\n", adesc.Description);
    std::printf("requested FL order:   11_1, 11_0, 10_1, 10_0\n");
    std::printf("MEASURED FEATURE LEVEL: %s (0x%04x)\n", FLName(got), (unsigned)got);
    std::printf("GetFeatureLevel():     0x%04x (%s)\n",
                (unsigned)dev->GetFeatureLevel(), FLName(dev->GetFeatureLevel()));
    if (got == D3D_FEATURE_LEVEL_11_1)
        std::printf("  -> FL 11_1 obtained: the larger UAV budget is in scope per spec.\n");
    else if (got == D3D_FEATURE_LEVEL_11_0)
        std::printf("  -> FL 11_0 obtained: no 11_1 UAV budget.\n");
    else
        std::printf("  -> BELOW 11_0: Nova's cs_5_0 compute path is not supported.\n");

    // 2. Show that no D3D11 feature struct reports a UAV count. Printing the
    //    struct sizes makes the absence concrete rather than asserted.
    std::printf("\n=== D3D11 UAV-count query attempt ===\n");
    std::printf("D3D11_SDK_VERSION        = %d\n", D3D11_SDK_VERSION);
    std::printf("last D3D11_FEATURE value = %d (D3D11_FEATURE_D3D11_OPTIONS6)\n",
                (int)D3D11_FEATURE_D3D11_OPTIONS6);
    std::printf("D3D11_1_UAV_SLOT_COUNT  = %d\n", D3D11_1_UAV_SLOT_COUNT);
    std::printf("D3D11_UAV_SLOT_COUNT    = NOT DEFINED anywhere in the Windows SDK\n");
    std::printf("sizeof OPTIONS  struct  = %zu bytes\n", sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS));
    std::printf("sizeof OPTIONS1 struct  = %zu bytes\n", sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1));
    std::printf("sizeof OPTIONS2 struct  = %zu bytes\n", sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2));
    std::printf("sizeof OPTIONS3 struct  = %zu bytes\n", sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3));
    std::printf("sizeof OPTIONS5 struct  = %zu bytes\n", sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS5));
    std::printf("sizeof OPTIONS6 struct  = %zu bytes (ShaderAccessRestrictedResourceTier only)\n",
                sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS6));
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS6 o6{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS6, &o6, sizeof(o6))))
            std::printf("OPTIONS6.ShaderAccessRestrictedResourceTier = %d  <-- the ONLY payload\n"
                        "   OPTIONS6 carries; there is no UAV count here.\n",
                        (int)o6.ShaderAccessRestrictedResourceTier);
    }
    std::printf("=> D3D11 provides NO API reporting the compute UAV count. Measuring.\n");

    // 3. Empirical sweep. cs_5_0 is what the engine actually uses
    //    (DX11Nova.h:191, DX12Nova.cpp:122); cs_5_1 is the 11_1-era profile.
    std::printf("\n=== DX11 EMPIRICAL compute UAV sweep ===\n");
    SweepProfile(dev, ctx, "cs_5_0", g50);
    SweepProfile(dev, ctx, "cs_5_1", g51);

    std::printf("\n=== DX11 SUMMARY ===\n");
    ReportProfile("cs_5_0 (engine's profile)", g50);
    ReportProfile("cs_5_1", g51);
    if (!g50.compileErr.empty()) {
        std::printf("cs_5_0 compile error: %s", g50.compileErr.c_str());
        if (g50.compileErr.back() != '\n') std::printf("\n");
    }
    if (!g51.compileErr.empty()) {
        std::printf("cs_5_1 compile error: %s", g51.compileErr.c_str());
        if (g51.compileErr.back() != '\n') std::printf("\n");
    }

    // 4. Can a dispatch bind a different set than the previous dispatch?
    std::printf("\n=== DX11 per-pass UAV slot swap test ===\n");
    SlotSwapTest(dev, ctx);

    // 5. Negative control, then DX12 side by side.
    NegativeControl(dev, ctx);

    std::printf("\n");
    int maxD12 = DX12Probe(dxilPath);

    std::printf("\n=== SIDE BY SIDE: compute UAVs per dispatch ===\n");
    std::printf("  DX11 cs_5_0 (engine's profile today) : %d\n", EffMax(g50));
    std::printf("  DX11 cs_5_1                           : %d\n", EffMax(g51));
    if (maxD12 >= 0)
        std::printf("  DX12 (descriptor table, cs_6_0)       : %d\n", maxD12);

    ctx->Release();
    dev->Release();
    ad->Release();
    f4->Release();
    return 0;
}