#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <cstring>

namespace Wasteland {

// Native DX12 compute resources for the Nova ray-tracer. Mirrors DX11Nova
// but uses explicit D3D12 command lists, descriptor heaps, and resource
// barriers. Every dispatch reads immutable snapshots, never its UAVs.
class DX12Nova {
    template<class T> using Com = Microsoft::WRL::ComPtr<T>;

    struct Image {
        Com<ID3D12Resource> texture, snapshot;
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    };

    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> commandQueue;
    Com<ID3D12CommandAllocator> commandAllocator;
    Com<ID3D12GraphicsCommandList> commandList;
    Com<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    Com<ID3D12RootSignature> rootSignature;
    Com<ID3D12PipelineState> pipelineState;
    Com<ID3DBlob> computeShader;

    Com<ID3D12Resource> constantBuffer;
    UINT constantBufferSize = 0;
    void* constantBufferMapped = nullptr;
    std::unordered_map<std::string, D3D12_SHADER_VARIABLE_DESC> variables;
    std::vector<unsigned char> bytes;

    std::array<Image, 8> images;
    Image history;
    std::array<Com<ID3D12Resource>, 3> structuredBuffers;

    Com<ID3D12DescriptorHeap> cbvSrvUavHeap;
    Com<ID3D12DescriptorHeap> samplerHeap;
    UINT cbvSrvUavDescriptorSize = 0;
    UINT samplerDescriptorSize = 0;

    // Scene textures (ID3D12Resource* from NVRHI)
    std::array<ID3D12Resource*, 32> sceneTextureResources{};

    UINT width = 0, height = 0;

    static void Check(HRESULT hr) {
        if (FAILED(hr)) throw std::runtime_error("DX12 Nova HRESULT " + std::to_string((unsigned long)hr));
    }

    void CreateImages(UINT w, UINT h);
    void CreateConstantBuffer(UINT size);
    void CreateRootSignature();
    void CreatePSO();
    void CreateDescriptorHeaps();
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void FlushCommandList();
    void EnsureConstantBufferSize(UINT size);

public:
    ~DX12Nova();

    void Init(ID3D12Device* d, ID3D12CommandQueue* q);
    void Resize(UINT w, UINT h);
    void Clear();
    template<class T> void Set(const char* name, const T& value, UINT index = 0);
    void Upload(UINT slot, const void* data, UINT count, UINT stride);
    void Unbind();
    void Dispatch(int pass, UINT w, UINT h, const std::vector<ID3D12Resource*>& textures);
    void CommitHistory();
    void CommitDenoise();
    void* Output(UINT index = 0);
};

} // namespace Wasteland
