// Follow-up diagnostics for two anomalies in probe_dx_uav_slots.cpp:
//
//  1. cs_5_1 "CreateComputeShader FAILED hr=0x80070057" at n=1 -- every size,
//     including a single UAV. That pattern smells like the profile string itself
//     rather than a register-space limit, so this prints what D3DCompile does
//     with each profile and what the resulting blob reflects as.
//
//  2. DX12 "resource allocation failed" -- this prints the actual HRESULT and the
//     failing index instead of a bare message, so the cause is visible rather
//     than guessed at.
//
// Build/run via build_probe_uav2.bat

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
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* kSrc =
    "RWTexture2D<float4> u0 : register(u0);\n"
    "[numthreads(1,1,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  u0[uint2(0,0)] = float4(1,0,0,0);\n"
    "}\n";

// ---------------------------------------------------- profile reality check ---

static void ProfileCheck(ID3D11Device* dev)
{
    std::printf("=== PROFILE CHECK: which profile strings actually work? ===\n");
    std::printf("shader: 1 UAV (u0) only. If cs_5_1 fails HERE, it is not a\n");
    std::printf("register-space limit -- it is the profile string itself.\n\n");

    const char* profiles[] = { "cs_5_0", "cs_5_1", "cs_5_1_1", "cs_6_0" };
    for (const char* prof : profiles) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errs = nullptr;
        HRESULT hr = D3DCompile(kSrc, (UINT)std::strlen(kSrc), nullptr, nullptr, nullptr,
                                "main", prof, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errs);
        std::printf("profile %-9s D3DCompile hr=0x%08lx", prof, (unsigned long)hr);
        if (errs) {
            std::string e = (const char*)errs->GetBufferPointer();
            // first line only
            size_t nl = e.find('\n');
            std::printf("  msg: %s", (nl == std::string::npos ? e : e.substr(0, nl)).c_str());
        }
        std::printf("\n");

        if (FAILED(hr) || !code) {
            std::printf("    -> compile failed, cannot test CreateComputeShader\n\n");
            if (errs) errs->Release();
            if (code) code->Release();
            continue;
        }

        // NOTE: this SDK's ID3D11ShaderReflection is a C-style DECLARE_INTERFACE
        // (d3d11shader.h:333) with no GetMajorVersion/GetMinorVersion/
        // GetReadWriteTextureResourceSlots accessors, so version reporting is
        // omitted. The decisive test is CreateComputeShader below: D3DCompile can
        // emit a blob that the runtime still refuses, and only the runtime's
        // CreateComputeShader verdict distinguishes "profile rejected" from
        // "register space exhausted".
        ID3D11ShaderReflection* refl = nullptr;
        if (SUCCEEDED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                                 IID_ID3D11ShaderReflection, (void**)&refl)) && refl) {
            std::printf("    D3DReflect: ok\n");
            refl->Release();
        } else {
            std::printf("    D3DReflect: failed\n");
        }

        ID3D11ComputeShader* cs = nullptr;
        HRESULT hr2 = dev->CreateComputeShader(code->GetBufferPointer(),
                                               code->GetBufferSize(), nullptr, &cs);
        std::printf("    CreateComputeShader hr=0x%08lx  %s\n",
                    (unsigned long)hr2, SUCCEEDED(hr2) ? "OK" : "REJECTED");
        if (cs) cs->Release();
        code->Release();
        if (errs) errs->Release();
        std::printf("\n");
    }

    // Show the fourcc at the head of a cs_5_1 blob vs cs_5_0 blob for the record.
    std::printf("--- blob header fourcc comparison ---\n");
    for (const char* prof : { "cs_5_0", "cs_5_1" }) {
        ID3DBlob* code = nullptr; ID3DBlob* errs = nullptr;
        HRESULT hr = D3DCompile(kSrc, (UINT)std::strlen(kSrc), nullptr, nullptr, nullptr,
                                "main", prof, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errs);
        if (SUCCEEDED(hr) && code) {
            const unsigned char* b = (const unsigned char*)code->GetBufferPointer();
            std::printf("%s: fourcc='%c%c%c%c' size=%u\n", prof, b[0], b[1], b[2], b[3],
                        (unsigned)code->GetBufferSize());
            code->Release();
        } else {
            std::printf("%s: compile failed 0x%08lx\n", prof, (unsigned long)hr);
        }
        if (errs) errs->Release();
    }
}

// -------------------------------------------------- DX12 allocation detail ---

static void Dx12AllocDetail()
{
    std::printf("\n=== DX12 RESOURCE ALLOCATION DETAIL ===\n");

    IDXGIFactory4* f4 = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f4));
    IDXGIAdapter1* ad = nullptr;
    f4->EnumAdapters1(0, &ad);

    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev));
    ad->Release();
    f4->Release();
    if (FAILED(hr)) {
        std::printf("D3D12CreateDevice failed 0x%08lx\n", (unsigned long)hr);
        return;
    }

    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readHeap{};
    readHeap.Type = D3D12_HEAP_TYPE_READBACK;

    // Vary the ONE thing most likely to be wrong (layout) and print every
    // HRESULT, so the accepted shape is visible instead of guessed.
    struct Case { const char* name; int heap; D3D12_RESOURCE_FLAGS flags;
                  D3D12_TEXTURE_LAYOUT layout; D3D12_RESOURCE_STATES init; };
    Case cases[] = {
        { "UAV tex 1x1 RGBA32F ALLOW_UAV layout=UNKNOWN init=UAV    ", 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { "UAV tex 1x1 RGBA32F ALLOW_UAV layout=ROW_MAJOR init=UAV  ", 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { "UAV tex 1x1 RGBA32F ALLOW_UAV layout=ROW_MAJOR init=COMMON", 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_COMMON },
        { "UAV tex 1x1 R32_FLOAT   ALLOW_UAV layout=ROW_MAJOR init=UAV", 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { "UAV tex 64x1 RGBA32F   ALLOW_UAV layout=ROW_MAJOR init=UAV", 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { "readback 256B NONE     layout=ROW_MAJOR init=COPY_DEST    ", 1, D3D12_RESOURCE_FLAG_NONE, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_COPY_DEST },
    };

    for (const Case& c : cases) {
        D3D12_RESOURCE_DESC d{};
        // A READBACK heap takes a buffer; give it a byte width. Textures use 1x1.
        if (c.heap == 1) { d.Width = 256; d.Height = 1; }
        else            { d.Width = 64;  d.Height = 1; }
        d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 1;
        d.Format = (strstr(c.name, "R32_FLOAT") != nullptr)
                     ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Layout = c.layout;
        d.Flags = c.flags;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = d.Format;

        ID3D12Resource* r = nullptr;
        HRESULT h = dev->CreateCommittedResource(
            c.heap == 0 ? &defHeap : &readHeap, D3D12_HEAP_FLAG_NONE, &d, c.init,
            (c.heap == 0) ? &cv : nullptr,
            IID_PPV_ARGS(&r));
        std::printf("%s -> hr=0x%08lx %s\n", c.name, (unsigned long)h,
                    SUCCEEDED(h) ? "OK" : "FAILED");
        if (r) r->Release();
    }

    // Also probe: is a 64-descriptor UAV range accepted by the root signature?
    std::printf("\n--- root signature with a 64-entry UAV descriptor range ---\n");
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 64;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_DESCRIPTOR_TABLE tbl{};
    tbl.NumDescriptorRanges = 1;
    tbl.pDescriptorRanges = &range;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = tbl;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* blob = nullptr; ID3DBlob* be = nullptr;
    HRESULT hs = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &be);
    std::printf("D3D12SerializeRootSignature hr=0x%08lx %s\n", (unsigned long)hs,
                SUCCEEDED(hs) ? "OK" : "FAILED");
    if (be) std::printf("  err: %s\n", (const char*)be->GetBufferPointer());
    if (be) be->Release();

    if (SUCCEEDED(hs) && blob) {
        ID3D12RootSignature* rsig = nullptr;
        HRESULT hc = dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                              blob->GetBufferSize(), IID_PPV_ARGS(&rsig));
        std::printf("CreateRootSignature hr=0x%08lx %s\n", (unsigned long)hc,
                    SUCCEEDED(hc) ? "OK" : "FAILED");
        if (rsig) rsig->Release();
        blob->Release();
    }

    dev->Release();
}

int main()
{
    std::printf("=====================================================\n");
    std::printf(" Probe addendum: profile + DX12 alloc diagnostics\n");
    std::printf("=====================================================\n\n");

    IDXGIFactory4* f4 = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f4));
    IDXGIAdapter1* ad = nullptr;
    f4->EnumAdapters1(0, &ad);

    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   fls, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
    ad->Release();
    f4->Release();
    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    std::printf("DX11 FL = 0x%04x\n\n", (unsigned)got);

    ProfileCheck(dev);
    Dx12AllocDetail();

    ctx->Release();
    dev->Release();
    return 0;
}