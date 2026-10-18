// Minimal isolation for the DX12 CreateCommittedResource E_INVALIDARG.
//
// ID3D12InfoQueue / D3D12_CREATE_DEVICE_DEBUG are NOT available in this SDK
// (absent from d3d12.h), so the debug layer cannot explain the failure. Instead
// this binary-searches the resource description with a strictly increasing set
// of variations, so the first variation that SUCCEEDS identifies the field that
// was wrong. If even the simplest buffer fails, the problem is not my descriptor.
//
// Build/run via build_probe_uav3.bat

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

struct Variant {
    const char* name;
    UINT64 width; UINT height; UINT depth;
    DXGI_FORMAT format;
    D3D12_TEXTURE_LAYOUT layout;
    D3D12_RESOURCE_FLAGS flags;
    D3D12_RESOURCE_STATES init;
    int heap;            // 0 = DEFAULT, 1 = READBACK, 2 = UPLOAD
    int useClearValue;
};

int main()
{
    std::printf("=== DX12 CreateCommittedResource isolation ===\n\n");

    IDXGIFactory4* f4 = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f4));
    IDXGIAdapter1* ad = nullptr;
    f4->EnumAdapters1(0, &ad);
    DXGI_ADAPTER_DESC1 adesc{};
    ad->GetDesc1(&adesc);
    std::printf("adapter: %ls\n\n", adesc.Description);

    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev));
    ad->Release();
    std::printf("D3D12CreateDevice(hardware) hr=0x%08lx\n\n", (unsigned long)hr);
    if (FAILED(hr)) { f4->Release(); return 1; }

    // WARP control: if the software rasteriser ALSO fails to allocate, the
    // problem is in the DX12 runtime/loader on this machine, not the GPU driver
    // and not my descriptor.
    {
        ID3D12Device* warp = nullptr;
        HRESULT hw = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&warp));
        std::printf("WARP control: D3D12CreateDevice(nullptr) hr=0x%08lx %s\n",
                    (unsigned long)hw, SUCCEEDED(hw) ? "OK" : "FAILED");
        if (SUCCEEDED(hw)) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC wd{};
            wd.Width = 256; wd.Height = 1; wd.DepthOrArraySize = 1;
            wd.MipLevels = 1; wd.SampleDesc.Count = 1; wd.SampleDesc.Quality = 1;
            wd.Format = DXGI_FORMAT_UNKNOWN;
            wd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            wd.Flags = D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource* wr = nullptr;
            HRESULT hwr = warp->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &wd,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&wr));
            std::printf("WARP control: CreateCommittedResource(256B buffer) hr=0x%08lx %s\n",
                        (unsigned long)hwr, SUCCEEDED(hwr) ? "OK" : "FAILED");
            if (wr) wr->Release();
            warp->Release();
        }
        std::printf("\n");
    }

    D3D12_HEAP_PROPERTIES h[3]{};
    h[0].Type = D3D12_HEAP_TYPE_DEFAULT;
    h[1].Type = D3D12_HEAP_TYPE_READBACK;
    h[2].Type = D3D12_HEAP_TYPE_UPLOAD;

    Variant vs[] = {
      // Buffers first: simplest possible resources.
      { "DEFAULT 256B buffer, init=COMMON, no cv                 ", 256,1,1, DXGI_FORMAT_UNKNOWN,         D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, 0, 0 },
      { "READBACK 256B buffer, init=COPY_DEST                    ", 256,1,1, DXGI_FORMAT_UNKNOWN,         D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, 1, 0 },
      { "UPLOAD  256B buffer, init=GENERIC_READ                  ", 256,1,1, DXGI_FORMAT_UNKNOWN,         D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, 2, 0 },
      // Then textures without UAV.
      { "DEFAULT 64x1 RGBA32F texture, NO flags, init=COMMON     ", 64,1,1,  DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, 0, 0 },
      // Then UAV textures, varying the suspicious fields one at a time.
      { "DEFAULT 64x1 RGBA32F texture, ALLOW_UAV, init=COMMON    ", 64,1,1,  DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, 0, 0 },
      { "DEFAULT 64x1 RGBA32F texture, ALLOW_UAV, init=UAV, cv   ", 64,1,1,  DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 },
      { "DEFAULT 64x1 R32_UINT texture,  ALLOW_UAV, init=UAV, cv ", 64,1,1,  DXGI_FORMAT_R32_UINT,          D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 },
      { "DEFAULT 256x1 RGBA16F tex,    ALLOW_UAV, init=UAV, cv   ", 256,1,1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 },
    };

    int firstOk = -1;
    for (size_t i = 0; i < sizeof(vs) / sizeof(vs[0]); ++i) {
        const Variant& v = vs[i];
        D3D12_RESOURCE_DESC d{};
        d.Width = v.width; d.Height = v.height; d.DepthOrArraySize = v.depth;
        d.MipLevels = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 1;
        d.Format = v.format; d.Layout = v.layout; d.Flags = v.flags;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = v.format;

        ID3D12Resource* r = nullptr;
        HRESULT hh = dev->CreateCommittedResource(&h[v.heap], D3D12_HEAP_FLAG_NONE, &d,
                                                   v.init, v.useClearValue ? &cv : nullptr,
                                                   IID_PPV_ARGS(&r));
        std::printf("%s -> hr=0x%08lx %s\n", v.name, (unsigned long)hh,
                    SUCCEEDED(hh) ? "OK" : "FAILED");
        if (SUCCEEDED(hh)) { if (firstOk < 0) firstOk = (int)i; if (r) r->Release(); }
    }

    std::printf("\nfirst succeeding variant index: %d\n", firstOk);
    if (firstOk <= 0)
        std::printf("=> Even the SIMPLEST resource failed: this DX12 device rejects\n"
                    "   CreateCommittedResource outright. That is a platform/driver\n"
                    "   issue, not a mistake in the probe's descriptor.\n");

    dev->Release();
    f4->Release();
    return 0;
}