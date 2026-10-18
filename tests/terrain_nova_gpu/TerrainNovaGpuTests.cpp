#include "Platform/NVRHI/DX12Nova.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Renderer/TerrainNovaMesh.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <Windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
ID3D12InfoQueue* g_crashInfoQueue = nullptr;

constexpr UINT kImageWidth = 32;
constexpr UINT kImageHeight = 32;

// This test-only declaration mirrors the production Renderer3D record and the
// HLSL RayTracingInstance layout. Its size/field ordering are the GPU contract.
struct alignas(16) RayTracingInstanceGPU
{
    glm::mat4 InvTransform{1.0f};
    glm::mat4 WorldTransform{1.0f};
    glm::vec4 Albedo{};
    glm::vec4 MaterialParams{};
    glm::vec4 Min{};
    glm::vec4 Max{};
    glm::vec4 Emission{};
    float MaxDistance = 0.0f;
    int LODLevel = 0;
    int TextureID = -1;
    int PackedMaterialMapID = -1;
    glm::vec4 TextureScale{1.0f};
    glm::vec4 DisplacementParams{};
    glm::vec4 WaveParams{};
    int TerrainVertexOffset = 0;
    int TerrainCellsPerSide = 0;
    int TerrainPad0 = 0;
    int TerrainPad1 = 0;
};
static_assert(sizeof(RayTracingInstanceGPU) == 288, "RayTracingInstance HLSL ABI drift");
static_assert(offsetof(RayTracingInstanceGPU, WaveParams) == 256, "RayTracingInstance WaveParams offset drift");
static_assert(offsetof(RayTracingInstanceGPU, TerrainVertexOffset) == 272, "Terrain vertex offset ABI drift");
static_assert(offsetof(RayTracingInstanceGPU, TerrainCellsPerSide) == 276, "Terrain cell-count ABI drift");

struct alignas(16) BVHNodeGPU
{
    glm::vec4 MinBounds{};
    glm::vec4 MaxBounds{};
};
static_assert(sizeof(BVHNodeGPU) == 32, "BVHNode HLSL ABI drift");
static_assert(offsetof(BVHNodeGPU, MaxBounds) == 16, "BVH node bounds offset drift");
static_assert(sizeof(Wasteland::TerrainNovaVertex) == 32, "Terrain vertex HLSL ABI drift");

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void RequireHr(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(hr) << ')';
        throw std::runtime_error(message.str());
    }
}

bool CollectD3D12Errors(ID3D12Device* device, std::vector<std::string>& errors)
{
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        return false;

    const UINT64 messageCount = infoQueue->GetNumStoredMessages();
    for (UINT64 index = 0; index < messageCount; ++index)
    {
        SIZE_T messageBytes = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &messageBytes)) || messageBytes == 0)
            continue;
        std::vector<unsigned char> storage(messageBytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageBytes)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            errors.emplace_back(message->pDescription);
    }
    return true;
}

void ProbeReadbackStagingCreation(ID3D12Device* device)
{
    // Reproduces DX12Nova::EnsureReadbackStaging's CreateCommittedResource call
    // so the debug layer's own message for the E_INVALIDARG is captured, then
    // retries with Alignment reset to 0 to confirm that is the only difference.
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sourceDesc = {};
    sourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sourceDesc.Width = 32;
    sourceDesc.Height = 32;
    sourceDesc.DepthOrArraySize = 1;
    sourceDesc.MipLevels = 1;
    sourceDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    sourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> source;
    RequireHr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &sourceDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&source)),
              "probe: source texture creation");

    const D3D12_RESOURCE_DESC runtimeDesc = source->GetDesc();
    std::cerr << "[DEBUG-probe] runtime source Alignment=" << runtimeDesc.Alignment
              << " Layout=" << static_cast<int>(runtimeDesc.Layout) << '\n';

    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    // Variant A: exactly what EnsureReadbackStaging does today (Alignment copied).
    {
        D3D12_RESOURCE_DESC desc = runtimeDesc;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> staging;
        const HRESULT hr = device->CreateCommittedResource(
            &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&staging));
        std::cerr << "[DEBUG-probe] copied-Alignment staging hr=0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec
                  << " alignment=" << desc.Alignment << '\n';
    }

    // Variant B: identical but with Alignment reset to 0.
    {
        D3D12_RESOURCE_DESC desc = runtimeDesc;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Alignment = 0;
        ComPtr<ID3D12Resource> staging;
        const HRESULT hr = device->CreateCommittedResource(
            &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&staging));
        std::cerr << "[DEBUG-probe] zero-Alignment staging hr=0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << '\n';
    }

    std::vector<std::string> probeErrors;
    if (CollectD3D12Errors(device, probeErrors))
    {
        std::cerr << "[DEBUG-probe] infoqueue messages=" << probeErrors.size() << '\n';
        for (const std::string& message : probeErrors)
            std::cerr << "[DEBUG-probe] message: " << message << '\n';
    }
}

void RunWarpUavComputeControlImpl(ID3D12Device* device, ID3D12CommandQueue* queue,
                                 DXGI_FORMAT outputFormat, const char* shaderSource,
                                 const char* controlName)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> compileErrors;
    RequireHr(D3DCompile(shaderSource, std::strlen(shaderSource), controlName, nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &shader, &compileErrors),
              "D3DCompile(WARP UAV control)");

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameter.DescriptorTable.NumDescriptorRanges = 1;
    rootParameter.DescriptorTable.pDescriptorRanges = &range;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParameter;

    ComPtr<ID3DBlob> serializedRoot;
    ComPtr<ID3DBlob> rootErrors;
    RequireHr(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serializedRoot, &rootErrors),
              "D3D12SerializeRootSignature(WARP UAV control)");
    ComPtr<ID3D12RootSignature> rootSignature;
    RequireHr(device->CreateRootSignature(0, serializedRoot->GetBufferPointer(),
                                           serializedRoot->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature)),
              "CreateRootSignature(WARP UAV control)");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    RequireHr(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap)),
              "CreateDescriptorHeap(WARP UAV control)");

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = 32;
    textureDesc.Height = 32;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = outputFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> outputTexture;
    RequireHr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&outputTexture)),
              "CreateCommittedResource(WARP UAV control)");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = textureDesc.Format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc,
                                       descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS.pShaderBytecode = shader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = shader->GetBufferSize();
    ComPtr<ID3D12PipelineState> pipelineState;
    RequireHr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)),
              "CreateComputePipelineState(WARP UAV control)");

    ComPtr<ID3D12CommandAllocator> allocator;
    RequireHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)),
              "CreateCommandAllocator(WARP UAV control)");
    ComPtr<ID3D12GraphicsCommandList> commandList;
    RequireHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         pipelineState.Get(), IID_PPV_ARGS(&commandList)),
              "CreateCommandList(WARP UAV control)");
    ID3D12DescriptorHeap* heaps[] = { descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(rootSignature.Get());
    commandList->SetPipelineState(pipelineState.Get());
    commandList->SetComputeRootDescriptorTable(0, descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->Dispatch(1, 1, 1);
    RequireHr(commandList->Close(), "CloseCommandList(WARP UAV control)");
    ID3D12CommandList* submittedLists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, submittedLists);

    ComPtr<ID3D12Fence> fence;
    RequireHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "CreateFence(WARP UAV control)");
    HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(completionEvent != nullptr, "CreateEventW(WARP UAV control) failed");
    const HRESULT signalResult = queue->Signal(fence.Get(), 1);
    if (FAILED(signalResult))
    {
        CloseHandle(completionEvent);
        RequireHr(signalResult, "SignalFence(WARP UAV control)");
    }
    if (fence->GetCompletedValue() < 1)
    {
        const HRESULT eventResult = fence->SetEventOnCompletion(1, completionEvent);
        if (FAILED(eventResult))
        {
            CloseHandle(completionEvent);
            RequireHr(eventResult, "SetEventOnCompletion(WARP UAV control)");
        }
        const DWORD waitResult = WaitForSingleObject(completionEvent, 30000);
        CloseHandle(completionEvent);
        Require(waitResult == WAIT_OBJECT_0, "WARP UAV control dispatch did not complete within 30 seconds");
    }
    else
    {
        CloseHandle(completionEvent);
    }

    std::vector<std::string> validationErrors;
    Require(CollectD3D12Errors(device, validationErrors), "WARP control has no D3D12 info queue");
    Require(validationErrors.empty(), validationErrors.empty() ? "" : validationErrors.front());
    std::cerr << "[DEBUG-warp-control] " << controlName << " dispatch completed cleanly\n";
}

void RunWarpUavComputeControl(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    static constexpr char shaderSource[] =
        "RWTexture2D<float4> outputImage : register(u0);\n"
        "[numthreads(1, 1, 1)]\n"
        "void main() { uint width, height; outputImage.GetDimensions(width, height); }\n";
    RunWarpUavComputeControlImpl(device, queue, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 shaderSource, "R8G8B8A8_UNORM/1-thread UAV GetDimensions control");
}

static constexpr char kNovaPreludeShaderSource[] =
    "RWTexture2D<float4> img_Output : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 dispatchThreadID : SV_DispatchThreadID) {\n"
    "    uint width, height;\n"
    "    img_Output.GetDimensions(width, height);\n"
    "    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height) return;\n"
    "}\n";

void RunWarpNovaPreludeControl(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    RunWarpUavComputeControlImpl(device, queue, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 kNovaPreludeShaderSource,
                                 "R32G32B32A32_FLOAT/8x8 Nova-prelude UAV control");
}

void RunWarpNovaRootSignatureControlImpl(ID3D12Device* device,
                                         ID3D12CommandQueue* queue,
                                         ID3DBlob* shader,
                                         const std::vector<unsigned char>& cbufferData,
                                         const char* controlName)
{
    Require(shader != nullptr, "Nova control shader bytecode is null");
    Require(!cbufferData.empty(), "Nova control cbuffer data is empty");

    D3D12_DESCRIPTOR_RANGE1 ranges[4] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 32;
    ranges[0].BaseShaderRegister = 15;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 6;
    ranges[1].BaseShaderRegister = 48;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 10;
    ranges[2].BaseShaderRegister = 64;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors = 11;
    ranges[3].BaseShaderRegister = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER1 rootParameters[10] = {};
    const auto setRootDescriptor = [&rootParameters](UINT index,
                                                      D3D12_ROOT_PARAMETER_TYPE type,
                                                      UINT shaderRegister)
    {
        rootParameters[index].ParameterType = type;
        rootParameters[index].Descriptor.ShaderRegister = shaderRegister;
        rootParameters[index].Descriptor.RegisterSpace = 0;
        rootParameters[index].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        rootParameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    setRootDescriptor(0, D3D12_ROOT_PARAMETER_TYPE_CBV, 1); // b1
    setRootDescriptor(1, D3D12_ROOT_PARAMETER_TYPE_SRV, 1); // t1
    setRootDescriptor(2, D3D12_ROOT_PARAMETER_TYPE_SRV, 2); // t2
    setRootDescriptor(3, D3D12_ROOT_PARAMETER_TYPE_SRV, 3); // t3
    setRootDescriptor(8, D3D12_ROOT_PARAMETER_TYPE_SRV, 10); // t10
    setRootDescriptor(9, D3D12_ROOT_PARAMETER_TYPE_SRV, 4); // t4
    for (UINT index = 0; index < 4; ++index)
    {
        rootParameters[4 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[4 + index].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[4 + index].DescriptorTable.pDescriptorRanges = &ranges[index];
        rootParameters[4 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    sampler.MaxAnisotropy = 16;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSignatureDesc.Desc_1_1.NumParameters = 10;
    rootSignatureDesc.Desc_1_1.pParameters = rootParameters;
    rootSignatureDesc.Desc_1_1.NumStaticSamplers = 1;
    rootSignatureDesc.Desc_1_1.pStaticSamplers = &sampler;
    rootSignatureDesc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRoot;
    ComPtr<ID3DBlob> rootErrors;
    RequireHr(D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &serializedRoot,
                                                    &rootErrors),
              "D3D12SerializeVersionedRootSignature(Nova control)");
    ComPtr<ID3D12RootSignature> rootSignature;
    RequireHr(device->CreateRootSignature(0, serializedRoot->GetBufferPointer(),
                                          serializedRoot->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSignature)),
              "CreateRootSignature(Nova control)");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 63; // Nova's table bases 4/36/42/52 plus 11 UAVs.
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    RequireHr(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap)),
              "CreateDescriptorHeap(Nova control)");
    const UINT descriptorStride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpuHandleAt = [&descriptorHeap, descriptorStride](UINT index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorStride;
        return handle;
    };

    const UINT cbAllocationSize =
        (static_cast<UINT>(cbufferData.size()) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
        ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = cbAllocationSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> bindingBuffer;
    RequireHr(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&bindingBuffer)),
              "CreateCommittedResource(Nova root bindings)");
    void* mappedBindingBuffer = nullptr;
    const D3D12_RANGE noCpuReads = { 0, 0 };
    RequireHr(bindingBuffer->Map(0, &noCpuReads, &mappedBindingBuffer),
              "Map(Nova root bindings)");
    std::memset(mappedBindingBuffer, 0, cbAllocationSize);
    std::memcpy(mappedBindingBuffer, cbufferData.data(), cbufferData.size());
    bindingBuffer->Unmap(0, nullptr);
    const D3D12_GPU_VIRTUAL_ADDRESS bindingAddress = bindingBuffer->GetGPUVirtualAddress();
    Require((bindingAddress % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) == 0,
            "Nova root binding upload-buffer GPUVA is not CBV aligned");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = bindingAddress;
    cbvDesc.SizeInBytes = cbAllocationSize;
    device->CreateConstantBufferView(&cbvDesc, cpuHandleAt(0));

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Texture2D.MipLevels = 1;
    // Keep padding and all three SRV tables initialized with typed null views.
    // The two float2 shader resources are given matching formats; all other
    // texture SRVs in the shader use float4-compatible declarations.
    for (UINT index = 1; index < 52; ++index)
    {
        nullSrvDesc.Format = (index == 47 || index == 51)
            ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateShaderResourceView(nullptr, &nullSrvDesc, cpuHandleAt(index));
    }

    D3D12_RESOURCE_DESC outputDesc = {};
    outputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    outputDesc.Width = 32;
    outputDesc.Height = 32;
    outputDesc.DepthOrArraySize = 1;
    outputDesc.MipLevels = 1;
    outputDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> outputTexture;
    RequireHr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&outputTexture)),
              "CreateCommittedResource(Nova control UAV)");
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc, cpuHandleAt(52));
    for (UINT index = 53; index < 62; ++index)
    {
        uavDesc.Format = index == 55 ? DXGI_FORMAT_R8G8B8A8_UNORM
            : (index == 57 || index == 61) ? DXGI_FORMAT_R32G32_FLOAT
            : DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, cpuHandleAt(index));
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullReservoirUavDesc = {};
    nullReservoirUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    nullReservoirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    nullReservoirUavDesc.Buffer.NumElements = 1;
    nullReservoirUavDesc.Buffer.StructureByteStride = 32;
    device->CreateUnorderedAccessView(nullptr, nullptr, &nullReservoirUavDesc, cpuHandleAt(62));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS.pShaderBytecode = shader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = shader->GetBufferSize();
    ComPtr<ID3D12PipelineState> pipelineState;
    RequireHr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)),
              "CreateComputePipelineState(Nova control)");

    ComPtr<ID3D12CommandAllocator> allocator;
    RequireHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)),
              "CreateCommandAllocator(Nova control)");
    ComPtr<ID3D12GraphicsCommandList> commandList;
    RequireHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         pipelineState.Get(), IID_PPV_ARGS(&commandList)),
              "CreateCommandList(Nova control)");
    ID3D12DescriptorHeap* heaps[] = { descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(rootSignature.Get());
    commandList->SetPipelineState(pipelineState.Get());
    commandList->SetComputeRootConstantBufferView(0, bindingAddress);
    commandList->SetComputeRootShaderResourceView(1, bindingAddress);
    commandList->SetComputeRootShaderResourceView(2, bindingAddress);
    commandList->SetComputeRootShaderResourceView(3, bindingAddress);
    const D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart =
        descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    for (UINT tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        static constexpr UINT tableBases[] = { 4, 36, 42, 52 };
        D3D12_GPU_DESCRIPTOR_HANDLE table = heapGpuStart;
        table.ptr += static_cast<UINT64>(tableBases[tableIndex]) * descriptorStride;
        commandList->SetComputeRootDescriptorTable(4 + tableIndex, table);
    }
    commandList->SetComputeRootShaderResourceView(8, bindingAddress);
    commandList->SetComputeRootShaderResourceView(9, bindingAddress);
    commandList->Dispatch(1, 1, 1);
    RequireHr(commandList->Close(), "CloseCommandList(Nova control)");
    ID3D12CommandList* submittedLists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, submittedLists);

    ComPtr<ID3D12Fence> fence;
    RequireHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "CreateFence(Nova control)");
    HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(completionEvent != nullptr, "CreateEventW(Nova control) failed");
    const HRESULT signalResult = queue->Signal(fence.Get(), 1);
    if (FAILED(signalResult))
    {
        CloseHandle(completionEvent);
        RequireHr(signalResult, "SignalFence(Nova control)");
    }
    if (fence->GetCompletedValue() < 1)
    {
        const HRESULT eventResult = fence->SetEventOnCompletion(1, completionEvent);
        if (FAILED(eventResult))
        {
            CloseHandle(completionEvent);
            RequireHr(eventResult, "SetEventOnCompletion(Nova control)");
        }
        const DWORD waitResult = WaitForSingleObject(completionEvent, 30000);
        CloseHandle(completionEvent);
        Require(waitResult == WAIT_OBJECT_0,
                std::string(controlName) + " dispatch did not complete within 30 seconds");
    }
    else
    {
        CloseHandle(completionEvent);
    }

    std::vector<std::string> validationErrors;
    Require(CollectD3D12Errors(device, validationErrors),
            "Nova control has no D3D12 info queue");
    Require(validationErrors.empty(),
            validationErrors.empty() ? "" : validationErrors.front());
    std::cerr << "[DEBUG-warp-control] " << controlName << " dispatch completed cleanly\n";
}

void RunWarpNovaRootSignaturePreludeControl(ID3D12Device* device,
                                            ID3D12CommandQueue* queue)
{
    // This diagnostic clones DX12Nova's parameter/register and heap-table shape,
    // but deliberately keeps only the small GetDimensions/bounds-check shader.
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> compileErrors;
    RequireHr(D3DCompile(kNovaPreludeShaderSource, std::strlen(kNovaPreludeShaderSource),
                         "Nova root-signature prelude control", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &shader, &compileErrors),
              "D3DCompile(Nova root-signature prelude control)");
    std::vector<unsigned char> zeroCbuffer(256, 0);
    RunWarpNovaRootSignatureControlImpl(device, queue, shader.Get(), zeroCbuffer,
                                        "Nova root-signature/heap-shape prelude");
}

bool IsAsciiWhitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

std::vector<int> ParseNovaPassCaseIds(const char* caseList)
{
    Require(caseList != nullptr, "MainCS case filter was not provided");
    const std::string values(caseList);
    std::vector<int> caseIds;
    size_t tokenStart = 0;
    while (tokenStart <= values.size())
    {
        size_t tokenEnd = values.find(',', tokenStart);
        if (tokenEnd == std::string::npos)
            tokenEnd = values.size();
        size_t first = tokenStart;
        size_t last = tokenEnd;
        while (first < last && IsAsciiWhitespace(values[first]))
            ++first;
        while (last > first && IsAsciiWhitespace(values[last - 1]))
            --last;
        Require(first < last, "WASTELAND_NOVA_GPU_TEST_CASES contains an empty pass ID");

        int caseId = 0;
        for (size_t i = first; i < last; ++i)
        {
            Require(values[i] >= '0' && values[i] <= '9',
                    "WASTELAND_NOVA_GPU_TEST_CASES must be comma-separated non-negative decimal IDs");
            const int digit = values[i] - '0';
            Require(caseId <= (std::numeric_limits<int>::max() - digit) / 10,
                    "WASTELAND_NOVA_GPU_TEST_CASES contains an out-of-range pass ID");
            caseId = caseId * 10 + digit;
        }
        if (std::find(caseIds.begin(), caseIds.end(), caseId) == caseIds.end())
            caseIds.push_back(caseId);

        if (tokenEnd == values.size())
            break;
        tokenStart = tokenEnd + 1;
    }
    Require(!caseIds.empty(), "WASTELAND_NOVA_GPU_TEST_CASES must select at least one pass ID");
    return caseIds;
}

bool ParseNovaCaseLabel(const std::string& source, size_t lineStart,
                        size_t lineEnd, int& caseId)
{
    size_t position = lineStart;
    while (position < lineEnd && IsAsciiWhitespace(source[position]))
        ++position;
    if (source.compare(position, 4, "case") != 0)
        return false;
    position += 4;
    if (position >= lineEnd || !IsAsciiWhitespace(source[position]))
        return false;
    while (position < lineEnd && IsAsciiWhitespace(source[position]))
        ++position;
    if (position >= lineEnd || source[position] < '0' || source[position] > '9')
        return false;

    int parsedId = 0;
    while (position < lineEnd && source[position] >= '0' && source[position] <= '9')
    {
        const int digit = source[position] - '0';
        Require(parsedId <= (std::numeric_limits<int>::max() - digit) / 10,
                "MainCS contains an out-of-range case ID");
        parsedId = parsedId * 10 + digit;
        ++position;
    }
    while (position < lineEnd && IsAsciiWhitespace(source[position]))
        ++position;
    if (position >= lineEnd || source[position] != ':')
        return false;
    caseId = parsedId;
    return true;
}

bool IsCommentOrBlankLine(const std::string& source, size_t lineStart,
                          size_t lineEnd)
{
    size_t position = lineStart;
    while (position < lineEnd && IsAsciiWhitespace(source[position]))
        ++position;
    if (position == lineEnd || source.compare(position, 2, "//") == 0 ||
        source.compare(position, 2, "/*") == 0)
        return true;
    return source[position] == '*' &&
        (position + 1 == lineEnd || IsAsciiWhitespace(source[position + 1]) ||
         source[position + 1] == '/');
}

void StubHLSLFunctionBody(std::string& source, const std::string& signature,
                           const std::string& body)
{
    const size_t signatureStart = source.find(signature);
    Require(signatureStart != std::string::npos,
            "test-only stub could not find function: " + signature);
    Require(source.find(signature, signatureStart + signature.size()) == std::string::npos,
            "test-only stub found multiple functions: " + signature);
    const size_t openBrace = source.find('{', signatureStart + signature.size());
    Require(openBrace != std::string::npos,
            "test-only stub could not find body for: " + signature);
    size_t closeBrace = std::string::npos;
    unsigned int braceDepth = 0;
    for (size_t i = openBrace; i < source.size(); ++i)
    {
        if (source[i] == '{')
            ++braceDepth;
        else if (source[i] == '}')
        {
            Require(braceDepth > 0,
                    "test-only stub found unbalanced braces for: " + signature);
            if (--braceDepth == 0)
            {
                closeBrace = i;
                break;
            }
        }
    }
    Require(closeBrace != std::string::npos,
            "test-only stub could not match braces for: " + signature);
    source.replace(openBrace + 1, closeBrace - openBrace - 1, " " + body + " ");
}

void ReplaceTokenInFunctionBody(std::string& source, const std::string& signature,
                                const std::string& oldToken, const std::string& newToken)
{
    const size_t signatureStart = source.find(signature);
    Require(signatureStart != std::string::npos,
            "test-only rewrite could not find function: " + signature);
    Require(source.find(signature, signatureStart + signature.size()) == std::string::npos,
            "test-only rewrite found multiple functions: " + signature);
    const size_t openBrace = source.find('{', signatureStart + signature.size());
    Require(openBrace != std::string::npos,
            "test-only rewrite could not find body for: " + signature);
    size_t closeBrace = std::string::npos;
    unsigned int braceDepth = 0;
    for (size_t i = openBrace; i < source.size(); ++i)
    {
        if (source[i] == '{')
            ++braceDepth;
        else if (source[i] == '}')
        {
            Require(braceDepth > 0,
                    "test-only rewrite found unbalanced braces for: " + signature);
            if (--braceDepth == 0)
            {
                closeBrace = i;
                break;
            }
        }
    }
    Require(closeBrace != std::string::npos,
            "test-only rewrite could not match braces for: " + signature);

    size_t pos = openBrace;
    while ((pos = source.find(oldToken, pos)) != std::string::npos && pos < closeBrace)
    {
        source.replace(pos, oldToken.size(), newToken);
        pos += newToken.size();
    }
}

size_t FindTrailingCaseTriviaStart(const std::string& source,
                                   size_t rangeStart, size_t rangeEnd)
{
    size_t triviaStart = rangeEnd;
    while (triviaStart > rangeStart)
    {
        size_t lineEnd = triviaStart;
        if (lineEnd > rangeStart && source[lineEnd - 1] == '\n')
            --lineEnd;
        size_t lineStart = lineEnd;
        while (lineStart > rangeStart && source[lineStart - 1] != '\n')
            --lineStart;
        if (!IsCommentOrBlankLine(source, lineStart, lineEnd))
            break;
        triviaStart = lineStart;
    }
    return triviaStart;
}

void FilterNovaMainCsPassCases(std::string& source,
                               const std::vector<int>& selectedCaseIds)
{
    const std::string switchToken = "switch(u_PassID)";
    const size_t switchStart = source.find(switchToken);
    Require(switchStart != std::string::npos,
            "test-only case filter could not find MainCS u_PassID switch");
    Require(source.find(switchToken, switchStart + switchToken.size()) == std::string::npos,
            "test-only case filter found multiple MainCS u_PassID switches");
    const size_t openBrace = source.find('{', switchStart + switchToken.size());
    Require(openBrace != std::string::npos,
            "test-only case filter could not find the MainCS u_PassID switch body");

    size_t closeBrace = std::string::npos;
    unsigned int braceDepth = 0;
    for (size_t i = openBrace; i < source.size(); ++i)
    {
        if (source[i] == '{')
            ++braceDepth;
        else if (source[i] == '}')
        {
            Require(braceDepth > 0, "test-only case filter found unbalanced switch braces");
            if (--braceDepth == 0)
            {
                closeBrace = i;
                break;
            }
        }
    }
    Require(closeBrace != std::string::npos,
            "test-only case filter could not match the MainCS u_PassID switch braces");

    struct CaseLabel
    {
        int id;
        size_t lineStart;
        size_t lineAfter;
    };
    std::vector<CaseLabel> labels;
    for (size_t lineStart = openBrace + 1; lineStart < closeBrace;)
    {
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos || lineEnd > closeBrace)
            lineEnd = closeBrace;
        const size_t lineAfter = lineEnd < closeBrace ? lineEnd + 1 : closeBrace;
        int caseId = 0;
        if (ParseNovaCaseLabel(source, lineStart, lineEnd, caseId))
            labels.push_back({ caseId, lineStart, lineAfter });
        if (lineAfter <= lineStart)
            break;
        lineStart = lineAfter;
    }
    Require(!labels.empty(), "MainCS u_PassID switch contains no case labels");

    for (const int selectedId : selectedCaseIds)
    {
        const bool found = std::any_of(labels.begin(), labels.end(),
            [selectedId](const CaseLabel& label) { return label.id == selectedId; });
        Require(found, "WASTELAND_NOVA_GPU_TEST_CASES selected missing MainCS case " +
                       std::to_string(selectedId));
    }

    std::vector<size_t> caseChunkStarts(labels.size());
    caseChunkStarts[0] = labels[0].lineStart;
    for (size_t i = 1; i < labels.size(); ++i)
    {
        caseChunkStarts[i] = FindTrailingCaseTriviaStart(
            source, labels[i - 1].lineAfter, labels[i].lineStart);
    }
    const size_t trailingTriviaStart = FindTrailingCaseTriviaStart(
        source, labels.back().lineAfter, closeBrace);

    std::string filteredSwitch = source.substr(switchStart,
                                                caseChunkStarts.front() - switchStart);
    for (size_t i = 0; i < labels.size(); ++i)
    {
        const size_t chunkEnd = i + 1 < labels.size()
            ? caseChunkStarts[i + 1] : trailingTriviaStart;
        const bool selected = std::find(selectedCaseIds.begin(), selectedCaseIds.end(),
                                        labels[i].id) != selectedCaseIds.end();
        if (selected)
            filteredSwitch.append(source, caseChunkStarts[i], chunkEnd - caseChunkStarts[i]);
    }
    filteredSwitch.append(source, trailingTriviaStart, closeBrace - trailingTriviaStart);
    source.replace(switchStart, closeBrace - switchStart, filteredSwitch);
}

void RunWarpNovaFullShaderControl(ID3D12Device* device, ID3D12CommandQueue* queue,
                                  const fs::path& shaderPath)
{
    const bool terrainGridEnabled =
        std::getenv("WASTELAND_NOVA_GPU_TEST_NO_TERRAIN_GRID") == nullptr;
    const D3D_SHADER_MACRO terrainGridMacros[] = {
        { "WASTELAND_NOVA_TERRAIN_GRID", "1" },
        { nullptr, nullptr }
    };
    const D3D_SHADER_MACRO* shaderMacros = terrainGridEnabled ? terrainGridMacros : nullptr;
    const bool optimizeLevel0 =
        std::getenv("WASTELAND_NOVA_GPU_TEST_SHADER_O0") != nullptr;
    const UINT shaderCompileFlags = D3DCOMPILE_ENABLE_STRICTNESS |
        (optimizeLevel0 ? D3DCOMPILE_OPTIMIZATION_LEVEL0 : 0);
    const bool removePassSwitch =
        std::getenv("WASTELAND_NOVA_GPU_TEST_NO_PASS_SWITCH") != nullptr;
    const char* passCasesEnvironment =
        std::getenv("WASTELAND_NOVA_GPU_TEST_CASES");
    const bool filterPassCases = passCasesEnvironment != nullptr && !removePassSwitch;
    const bool stubTraceCase =
        std::getenv("WASTELAND_NOVA_GPU_TEST_STUB_TRACE_CASE") != nullptr;
    const bool stubTraversal =
        std::getenv("WASTELAND_NOVA_GPU_TEST_STUB_TRAVERSAL") != nullptr;
    const bool stubTraceScene =
        stubTraversal || std::getenv("WASTELAND_NOVA_GPU_TEST_STUB_TRACE_SCENE") != nullptr;
    const bool stubIsOccluded =
        stubTraversal || std::getenv("WASTELAND_NOVA_GPU_TEST_STUB_IS_OCCLUDED") != nullptr;
    const std::vector<int> selectedCaseIds = filterPassCases
        ? ParseNovaPassCaseIds(passCasesEnvironment) : std::vector<int>{};
    if (stubTraceCase)
    {
        Require(filterPassCases &&
                std::find(selectedCaseIds.begin(), selectedCaseIds.end(), 1) != selectedCaseIds.end(),
                "WASTELAND_NOVA_GPU_TEST_STUB_TRACE_CASE requires "
                "WASTELAND_NOVA_GPU_TEST_CASES to include case 1");
    }
    std::cerr << "[DEBUG-warp-control] full Nova shader terrain grid="
              << (terrainGridEnabled ? "on" : "off")
              << ", optimization=" << (optimizeLevel0 ? "O0" : "default")
              << ", pass switch=" << (removePassSwitch ? "removed"
                  : (filterPassCases ? "filtered" : "intact")) << '\n';
    if (filterPassCases)
    {
        std::cerr << "[DEBUG-warp-control] MainCS selected case IDs=";
        for (size_t i = 0; i < selectedCaseIds.size(); ++i)
        {
            if (i != 0)
                std::cerr << ',';
            std::cerr << selectedCaseIds[i];
        }
        std::cerr << '\n';
    }
    else if (passCasesEnvironment != nullptr)
    {
        std::cerr << "[DEBUG-warp-control] MainCS case filter ignored because "
                     "WASTELAND_NOVA_GPU_TEST_NO_PASS_SWITCH takes precedence\n";
    }
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> compileErrors;
    HRESULT compileResult = S_OK;
    if (removePassSwitch || filterPassCases)
    {
        std::ifstream shaderFile(shaderPath, std::ios::binary);
        Require(shaderFile.good(), "could not open full Nova shader for test-only source rewrite");
        std::ostringstream sourceStream;
        sourceStream << shaderFile.rdbuf();
        std::string source = sourceStream.str();
        if (removePassSwitch)
        {
            const std::string switchToken = "switch(u_PassID)";
            const size_t switchStart = source.find(switchToken);
            Require(switchStart != std::string::npos,
                    "test-only rewrite could not find MainCS u_PassID switch");
            Require(source.find(switchToken, switchStart + switchToken.size()) == std::string::npos,
                    "test-only rewrite found multiple u_PassID switches");
            const size_t openBrace = source.find('{', switchStart + switchToken.size());
            Require(openBrace != std::string::npos,
                    "test-only rewrite could not find the u_PassID switch body");
            size_t closeBrace = std::string::npos;
            unsigned int braceDepth = 0;
            for (size_t i = openBrace; i < source.size(); ++i)
            {
                if (source[i] == '{')
                    ++braceDepth;
                else if (source[i] == '}' && --braceDepth == 0)
                {
                    closeBrace = i;
                    break;
                }
            }
            Require(closeBrace != std::string::npos,
                    "test-only rewrite could not match the u_PassID switch braces");
            source.replace(switchStart, closeBrace - switchStart + 1, "return;");
        }
        else
        {
            FilterNovaMainCsPassCases(source, selectedCaseIds);
            if (stubTraceCase)
            {
                const std::string traceCase = "case 1: RunTraceAndDenoise(); break;";
                const size_t traceCaseStart = source.find(traceCase);
                Require(traceCaseStart != std::string::npos &&
                        source.find(traceCase, traceCaseStart + traceCase.size()) == std::string::npos,
                        "WASTELAND_NOVA_GPU_TEST_STUB_TRACE_CASE expected exactly one "
                        "'case 1: RunTraceAndDenoise(); break;' in the filtered MainCS source");
                source.replace(traceCaseStart, traceCase.size(), "case 1: break;");
                std::cerr << "[DEBUG-warp-control] MainCS case 1 RunTraceAndDenoise stubbed "
                             "for test-only compile\n";
            }
            if (stubTraceScene)
            {
                StubHLSLFunctionBody(source, "HitInfo TraceScene(Ray ray)",
                                      "return (HitInfo)0;");
                std::cerr << "[DEBUG-warp-control] TraceScene stubbed for test-only compile\n";
            }
            if (stubIsOccluded)
            {
                StubHLSLFunctionBody(source, "bool IsOccluded(Ray r, float maxDist, int skipIdx)",
                                      "return false;");
                std::cerr << "[DEBUG-warp-control] IsOccluded stubbed for test-only compile\n";
            }
        }
        const bool splitGsStack =
            std::getenv("WASTELAND_NOVA_GPU_TEST_SPLIT_GS_STACK") != nullptr;
        if (splitGsStack)
        {
            const std::string declaration = "groupshared int gs_trav_stack[64 * 32];";
            const size_t declStart = source.find(declaration);
            Require(declStart != std::string::npos,
                    "test-only split could not find gs_trav_stack declaration");
            Require(source.find(declaration, declStart + declaration.size()) == std::string::npos,
                    "test-only split found multiple gs_trav_stack declarations");

            ReplaceTokenInFunctionBody(source, "HitInfo TraceScene(Ray ray)",
                                       "gs_trav_stack", "gs_trav_stack_trace");
            ReplaceTokenInFunctionBody(source, "bool IsOccluded(Ray r, float maxDist, int skipIdx)",
                                       "gs_trav_stack", "gs_trav_stack_shadow");

            source.replace(declStart, declaration.size(),
                "groupshared int gs_trav_stack_trace[64 * 32];\n"
                "groupshared int gs_trav_stack_shadow[64 * 32];");
            std::cerr << "[DEBUG-warp-control] gs_trav_stack split into trace/shadow arrays "
                         "for test-only compile\n";
        }
        const std::string sourceName = shaderPath.string();
        compileResult = D3DCompile(source.data(), source.size(), sourceName.c_str(),
                                   shaderMacros, nullptr, "main", "cs_5_0",
                                   shaderCompileFlags, 0, &shader, &compileErrors);
    }
    else
    {
        compileResult = D3DCompileFromFile(
            shaderPath.c_str(), shaderMacros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "cs_5_0", shaderCompileFlags, 0, &shader, &compileErrors);
    }
    if (FAILED(compileResult))
    {
        const std::string detail = compileErrors
            ? static_cast<const char*>(compileErrors->GetBufferPointer())
            : "no compiler diagnostics";
        throw std::runtime_error("D3DCompileFromFile(Nova full-shader control) failed: " + detail);
    }

    ComPtr<ID3D12ShaderReflection> reflection;
    RequireHr(D3DReflect(shader->GetBufferPointer(), shader->GetBufferSize(),
                         IID_ID3D12ShaderReflection,
                         reinterpret_cast<void**>(reflection.GetAddressOf())),
              "D3DReflect(Nova full-shader control)");
    D3D12_SHADER_INPUT_BIND_DESC cbufferBinding{};
    RequireHr(reflection->GetResourceBindingDescByName("NovaConstants", &cbufferBinding),
              "GetResourceBindingDescByName(NovaConstants)");
    Require(cbufferBinding.Type == D3D_SIT_CBUFFER && cbufferBinding.BindPoint == 1,
            "reflected NovaConstants binding does not match the test root CBV at b1");

    ID3D12ShaderReflectionConstantBuffer* constants = nullptr;
    D3D12_SHADER_BUFFER_DESC constantsDesc{};
    for (UINT index = 0; ; ++index)
    {
        ID3D12ShaderReflectionConstantBuffer* candidate =
            reflection->GetConstantBufferByIndex(index);
        if (!candidate)
            break;
        D3D12_SHADER_BUFFER_DESC candidateDesc{};
        RequireHr(candidate->GetDesc(&candidateDesc),
                  "GetDesc(NovaConstants reflection candidate)");
        if (std::strcmp(candidateDesc.Name, "NovaConstants") == 0)
        {
            constants = candidate;
            constantsDesc = candidateDesc;
            break;
        }
    }
    Require(constants != nullptr && constantsDesc.Size > 0,
            "NovaConstants cbuffer not found in full-shader reflection");

    std::vector<unsigned char> cbufferData(constantsDesc.Size, 0);
    const auto writeReflectedInt = [&constants, &constantsDesc, &cbufferData](
        const char* variableName, int value)
    {
        ID3D12ShaderReflectionVariable* variable = constants->GetVariableByName(variableName);
        Require(variable != nullptr, std::string("NovaConstants variable missing: ") + variableName);
        D3D12_SHADER_VARIABLE_DESC variableDesc{};
        RequireHr(variable->GetDesc(&variableDesc), "GetDesc(NovaConstants variable)");
        Require(variableDesc.Size == sizeof(value) &&
                variableDesc.StartOffset <= constantsDesc.Size &&
                variableDesc.Size <= constantsDesc.Size - variableDesc.StartOffset,
                std::string("unexpected reflected layout for NovaConstants.") + variableName);
        std::memcpy(cbufferData.data() + variableDesc.StartOffset, &value, sizeof(value));
        std::cerr << "[DEBUG-warp-control] reflected " << variableName
                  << " offset=" << variableDesc.StartOffset << '\n';
    };
    writeReflectedInt("u_PassID", 99);
    writeReflectedInt("u_InstanceCount", 0);

    RunWarpNovaRootSignatureControlImpl(
        device, queue, shader.Get(), cbufferData,
        terrainGridEnabled ? "actual Nova HLSL/pass-99 full-shader with terrain grid"
                           : "actual Nova HLSL/pass-99 full-shader without terrain grid");
}

fs::path FindAncestorContaining(fs::path start, const fs::path& marker)
{
    std::error_code ec;
    start = fs::absolute(start, ec);
    if (ec)
        throw std::runtime_error("could not make the executable path absolute: " + ec.message());

    for (fs::path candidate = start; !candidate.empty(); candidate = candidate.parent_path())
    {
        if (fs::exists(candidate / marker, ec) && !ec)
            return candidate;
        if (candidate == candidate.root_path())
            break;
    }
    throw std::runtime_error("could not locate repository marker '" + marker.string() + "' from " + start.string());
}

std::uint8_t ToUnorm8(float value)
{
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

void SetRequiredUniforms(Wasteland::DX12Nova& nova, const glm::vec3& cameraPosition,
                         const glm::mat4& viewProjection, int instanceCount)
{
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
    const int frameIndex = 0;
    const int qualityLevel = 0;
    const int lightCount = 0;
    const int disabled = 0;
    const float zero = 0.0f;
    const float one = 1.0f;
    const glm::vec2 zero2(0.0f);

    nova.Set("u_CameraPosition", cameraPosition);
    nova.Set("u_InverseViewProjection", inverseViewProjection);
    nova.Set("u_PrevViewProjection", viewProjection);
    nova.Set("u_ViewProjection", viewProjection);
    nova.Set("u_InstanceCount", instanceCount);
    nova.Set("u_FrameIndex", frameIndex);
    nova.Set("u_QualityLevel", qualityLevel);
    nova.Set("u_LightCount", lightCount);
    nova.Set("u_MaxLights", lightCount);
    nova.Set("u_IndirectRays", lightCount);
    nova.Set("u_CameraMoved", one);
    nova.Set("u_Jitter", zero2);
    nova.Set("u_PrevJitter", zero2);
    nova.Set("u_AccumulationAlpha", one);
    nova.Set("u_RenderScale", one);
    nova.Set("u_ColorGamma", one);
    nova.Set("u_Time", zero);
    nova.Set("u_NeuralEnabled", disabled);
    nova.Set("u_FogEnabled", disabled);
    nova.Set("u_CloudEnabled", disabled);
    nova.Set("u_GlobalFogEnabled", disabled);
    nova.Set("u_GodRaysEnabled", disabled);
    nova.Set("u_BloomEnabled", disabled);
    nova.Set("u_ColorGradingEnabled", disabled);
    nova.Set("u_FilmGrainEnabled", disabled);
    nova.Set("u_ChromaticAberrationEnabled", disabled);
    nova.Set("u_VignetteEnabled", disabled);
    nova.Set("u_SkyBottomColor", glm::vec3(0.08f, 0.10f, 0.16f));
    nova.Set("u_SkyTopColor", glm::vec3(0.18f, 0.28f, 0.48f));
}

LONG CALLBACK LogWARPException(EXCEPTION_POINTERS* exception)
{
    static LONG logging = 0;
    const DWORD code = exception && exception->ExceptionRecord
        ? exception->ExceptionRecord->ExceptionCode : 0;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000374u)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&logging, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    void* faultAddress = exception->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "[CRASH-terrain-warp] first-chance SEH 0x%08lx at %p\n",
                 static_cast<unsigned long>(code), faultAddress);
#if defined(_M_X64)
    if (exception->ContextRecord)
        std::fprintf(stderr, "[CRASH-terrain-warp] RIP=%p RSP=%p\n",
                     reinterpret_cast<void*>(exception->ContextRecord->Rip),
                     reinterpret_cast<void*>(exception->ContextRecord->Rsp));
#endif

    void* frames[64] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]), &module))
        {
            wchar_t widePath[MAX_PATH] = {};
            char path[MAX_PATH * 3] = {};
            GetModuleFileNameW(module, widePath, MAX_PATH);
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path, static_cast<int>(sizeof(path)), nullptr, nullptr);
            const auto offset = reinterpret_cast<std::uintptr_t>(frames[i]) -
                                reinterpret_cast<std::uintptr_t>(module);
            std::fprintf(stderr, "[CRASH-terrain-warp] frame[%u]=%p %s+0x%llx\n",
                         static_cast<unsigned int>(i), frames[i], path,
                         static_cast<unsigned long long>(offset));
        }
        else
        {
            std::fprintf(stderr, "[CRASH-terrain-warp] frame[%u]=%p\n",
                         static_cast<unsigned int>(i), frames[i]);
        }
    }
    if (g_crashInfoQueue)
    {
        const UINT64 messageCount = g_crashInfoQueue->GetNumStoredMessages();
        const UINT64 firstMessage = messageCount > 12 ? messageCount - 12 : 0;
        std::fprintf(stderr, "[CRASH-terrain-warp] D3D12 infoqueue messages=%llu\n",
                     static_cast<unsigned long long>(messageCount));
        for (UINT64 i = firstMessage; i < messageCount; ++i)
        {
            alignas(D3D12_MESSAGE) char storage[8192] = {};
            SIZE_T messageBytes = sizeof(storage);
            const HRESULT messageResult = g_crashInfoQueue->GetMessage(
                i, reinterpret_cast<D3D12_MESSAGE*>(storage), &messageBytes);
            if (SUCCEEDED(messageResult))
            {
                const D3D12_MESSAGE* message = reinterpret_cast<const D3D12_MESSAGE*>(storage);
                std::fprintf(stderr, "[CRASH-terrain-warp] D3D12 message[%llu] severity=%u: %s\n",
                             static_cast<unsigned long long>(i),
                             static_cast<unsigned int>(message->Severity),
                             message->pDescription ? message->pDescription : "(no description)");
            }
            else
            {
                std::fprintf(stderr, "[CRASH-terrain-warp] GetMessage(%llu) failed: 0x%08lx size=%llu\n",
                             static_cast<unsigned long long>(i), static_cast<unsigned long>(messageResult),
                             static_cast<unsigned long long>(messageBytes));
            }
        }
    }
    std::fflush(stderr);
    InterlockedExchange(&logging, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

int RunTest(const char* executablePath)
{
    // Diagnostic control: submit an empty-scene no-op pass to distinguish shared
    // command/binding faults from pass-specific scene traversal or shader work.
    const bool emptySceneDiagnostic = std::getenv("WASTELAND_NOVA_GPU_TEST_EMPTY_SCENE") != nullptr;
    const bool controlOnly = std::getenv("WASTELAND_NOVA_GPU_TEST_CONTROL_ONLY") != nullptr;
    const bool zeroGroupsDiagnostic = std::getenv("WASTELAND_NOVA_GPU_TEST_ZERO_GROUPS") != nullptr;
    const bool singleGroupDiagnostic = std::getenv("WASTELAND_NOVA_GPU_TEST_ONE_GROUP") != nullptr;
    const bool disableGpuValidation = std::getenv("WASTELAND_NOVA_GPU_TEST_NO_GBV") != nullptr;
    const fs::path testRoot = FindAncestorContaining(executablePath, "TerrainNovaGpuTests.vcxproj");
    // DX12Nova and Project logging use the process CWD for their diagnostic
    // file. Keep every generated artifact inside this test-only directory.
    fs::current_path(testRoot);
    const fs::path repositoryRoot = FindAncestorContaining(testRoot, fs::path("DemonCore-Editor") / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl");
    const fs::path editorRoot = repositoryRoot / "DemonCore-Editor";

    std::string projectError;
    Require(Wasteland::Project::Get().AttachAssetRoot(editorRoot, &projectError),
            "AttachAssetRoot failed: " + projectError);
    std::cerr << "[DEBUG-terrain-warp] project attached\n";
    const fs::path shaderPath = Wasteland::Project::Get().GetAssetPath("shaders/Renderer3D_NovaRenderer.hlsl");
    Require(fs::is_regular_file(shaderPath), "DX12Nova shader does not resolve: " + shaderPath.string());

    ComPtr<ID3D12Debug> debugLayer;
    const bool debugLayerEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugLayer)));
    if (debugLayerEnabled)
        debugLayer->EnableDebugLayer();
    if (debugLayerEnabled && (emptySceneDiagnostic || controlOnly) && !disableGpuValidation)
    {
        ComPtr<ID3D12Debug1> gpuValidationLayer;
        RequireHr(debugLayer.As(&gpuValidationLayer), "QueryInterface(ID3D12Debug1)");
        gpuValidationLayer->SetEnableGPUBasedValidation(TRUE);
        std::cerr << "[DEBUG-terrain-warp] GPU-based validation enabled\n";
    }
    else if (debugLayerEnabled && (emptySceneDiagnostic || controlOnly) && disableGpuValidation)
    {
        std::cerr << "[DEBUG-terrain-warp] GPU-based validation disabled by test environment\n";
    }
    ComPtr<IDXGIFactory4> factory;
    RequireHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter> warpAdapter;
    ComPtr<IDXGIAdapter1> realAdapter;
    IDXGIAdapter* deviceAdapter = nullptr;
    const bool useRealGpu = std::getenv("WASTELAND_NOVA_GPU_TEST_REAL_GPU") != nullptr;
    if (useRealGpu)
    {
        // Hardware adapter enumeration: skip the software rasterizer and any
        // adapter that cannot create a D3D12 device, so the selected adapter is
        // a real GPU rather than WARP under another name.
        for (UINT index = 0; ; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 candidateDesc{};
            if (FAILED(candidate->GetDesc1(&candidateDesc)))
                continue;
            if ((candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;
            ComPtr<ID3D12Device> probe;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&probe))))
                continue;
            realAdapter = candidate;
            std::wcerr << L"[DEBUG-terrain-warp] real GPU adapter: " << candidateDesc.Description << L'\n';
            break;
        }
        Require(realAdapter != nullptr,
                "WASTELAND_NOVA_GPU_TEST_REAL_GPU found no hardware D3D12 adapter");
        deviceAdapter = realAdapter.Get();
    }
    else
    {
        RequireHr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "IDXGIFactory4::EnumWarpAdapter");
        deviceAdapter = warpAdapter.Get();
    }

    ComPtr<ID3D12Device> device;
    constexpr std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    HRESULT deviceResult = E_INVALIDARG;
    D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    for (const D3D_FEATURE_LEVEL featureLevel : featureLevels)
    {
        deviceResult = D3D12CreateDevice(deviceAdapter, featureLevel, IID_PPV_ARGS(&device));
        if (SUCCEEDED(deviceResult))
        {
            selectedFeatureLevel = featureLevel;
            break;
        }
    }
    RequireHr(deviceResult, useRealGpu ? "D3D12CreateDevice(real GPU)" : "D3D12CreateDevice(WARP)");
    ComPtr<ID3D12InfoQueue> crashInfoQueue;
    if (SUCCEEDED(device.As(&crashInfoQueue)))
        g_crashInfoQueue = crashInfoQueue.Get();
    std::cerr << (useRealGpu ? "real GPU" : "WARP") << " feature level=0x" << std::hex
              << static_cast<unsigned int>(selectedFeatureLevel) << std::dec << '\n';

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    RequireHr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    std::cerr << "[DEBUG-terrain-warp] command queue created\n";

    if (std::getenv("WASTELAND_NOVA_GPU_TEST_PROBE_STAGING") != nullptr)
        ProbeReadbackStagingCreation(device.Get());

    if (emptySceneDiagnostic)
    {
        RunWarpUavComputeControl(device.Get(), queue.Get());
        RunWarpNovaPreludeControl(device.Get(), queue.Get());
        RunWarpNovaRootSignaturePreludeControl(device.Get(), queue.Get());
    }
    else if (controlOnly)
    {
        RunWarpUavComputeControl(device.Get(), queue.Get());
        RunWarpNovaPreludeControl(device.Get(), queue.Get());
        RunWarpNovaRootSignaturePreludeControl(device.Get(), queue.Get());
        RunWarpNovaFullShaderControl(device.Get(), queue.Get(), shaderPath);
    }
    if (controlOnly)
        return 0;

    // DX12Nova binds a 32-entry scene-texture table for every pass. Populate its
    // full range with a valid 1x1 SRV resource even though this Terrain material
    // has TextureID=-1 and the smoke schedule does not sample scene textures.
    D3D12_HEAP_PROPERTIES sceneTextureHeap{};
    sceneTextureHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sceneTextureDesc{};
    sceneTextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sceneTextureDesc.Width = 1;
    sceneTextureDesc.Height = 1;
    sceneTextureDesc.DepthOrArraySize = 1;
    sceneTextureDesc.MipLevels = 1;
    sceneTextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sceneTextureDesc.SampleDesc.Count = 1;
    sceneTextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> placeholderTexture;
    RequireHr(device->CreateCommittedResource(&sceneTextureHeap, D3D12_HEAP_FLAG_NONE, &sceneTextureDesc,
        static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        nullptr, IID_PPV_ARGS(&placeholderTexture)), "Create 1x1 scene-texture placeholder");
    std::cerr << "[DEBUG-terrain-warp] placeholder texture created\n";

    Wasteland::Terrain::TerrainAsset terrainAsset;
    std::string terrainError;
    Require(Wasteland::Terrain::CreateDefaultTerrainAsset("terrain-nova-gpu-smoke", terrainAsset, &terrainError),
            "CreateDefaultTerrainAsset failed: " + terrainError);

    Wasteland::Terrain::TerrainChunkMesh terrainMesh;
    Require(Wasteland::Terrain::BuildTerrainChunkMesh(terrainAsset, 0, 0, 0, terrainMesh, nullptr, &terrainError),
            "BuildTerrainChunkMesh failed: " + terrainError);
    Wasteland::TerrainNovaChunkGeometry terrainGeometry;
    Require(Wasteland::BuildTerrainNovaChunkGeometry(terrainMesh, terrainGeometry, &terrainError),
            "BuildTerrainNovaChunkGeometry failed: " + terrainError);
    Require(!terrainGeometry.Vertices.empty() && terrainGeometry.CellsPerSide == terrainMesh.CellsPerSide,
            "terrain Nova packing produced no mesh vertices or changed the grid extent");
    std::cerr << "[DEBUG-terrain-warp] terrain mesh packed; vertices=" << terrainGeometry.Vertices.size() << '\n';

    RayTracingInstanceGPU instance{};
    instance.InvTransform = terrainGeometry.InvTransform;
    instance.WorldTransform = terrainGeometry.WorldTransform;
    instance.Albedo = glm::vec4(0.60f, 0.20f, 0.05f, 1.0f);
    instance.MaterialParams = glm::vec4(0.0f, 0.85f, 8.0f, 0.0f); // shape 8 = terrain heightfield
    instance.Min = terrainGeometry.BoundsMin;
    instance.Max = terrainGeometry.BoundsMax;
    instance.Emission = glm::vec4(0.45f, 0.14f, 0.035f, 1.0f);
    instance.MaxDistance = terrainGeometry.MaxDistance;
    instance.LODLevel = static_cast<int>(terrainGeometry.LodLevel);
    instance.TextureScale = glm::vec4(1.0f);
    instance.TerrainVertexOffset = 0;
    instance.TerrainCellsPerSide = static_cast<int>(terrainGeometry.CellsPerSide);

    const glm::vec3 localCenter = (glm::vec3(terrainGeometry.BoundsMin) + glm::vec3(terrainGeometry.BoundsMax)) * 0.5f;
    const glm::vec3 worldCenter = glm::vec3(terrainGeometry.WorldTransform * glm::vec4(localCenter, 1.0f));
    const glm::vec3 cameraPosition = worldCenter + glm::vec3(0.0f, 128.0f, 0.0f);
    const glm::mat4 view = glm::lookAtRH(cameraPosition, worldCenter, glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(45.0f),
        static_cast<float>(kImageWidth) / static_cast<float>(kImageHeight), 0.1f, 512.0f);
    const glm::mat4 viewProjection = projection * view;

    const glm::vec3 worldBoundsMin = glm::vec3(terrainGeometry.WorldTransform * glm::vec4(glm::vec3(terrainGeometry.BoundsMin), 1.0f));
    const glm::vec3 worldBoundsMax = glm::vec3(terrainGeometry.WorldTransform * glm::vec4(glm::vec3(terrainGeometry.BoundsMax), 1.0f));
    BVHNodeGPU rootNode;
    rootNode.MinBounds = glm::vec4(worldBoundsMin, -1.0f); // negative leaf sentinel: first instance 0
    rootNode.MaxBounds = glm::vec4(worldBoundsMax, 1.0f);  // one instance in this leaf

    Wasteland::DX12Nova nova;
    std::cerr << "[DEBUG-terrain-warp] DX12Nova Init begin\n";
    nova.Init(device.Get(), queue.Get());
    std::cerr << "[DEBUG-terrain-warp] DX12Nova Init complete\n";
    nova.Resize(kImageWidth, kImageHeight);
    std::cerr << "[DEBUG-terrain-warp] Resize/Clear complete\n";
    nova.Upload(0, &instance, 1, sizeof(instance));
    std::cerr << "[DEBUG-terrain-warp] instance upload complete\n";
    nova.Upload(1, &rootNode, 1, sizeof(rootNode));
    std::cerr << "[DEBUG-terrain-warp] BVH upload complete\n";
    nova.Upload(2, nullptr, 0, sizeof(int)); // valid empty LightIndices SRV
    std::cerr << "[DEBUG-terrain-warp] light-index upload complete\n";
    nova.Upload(3, terrainGeometry.Vertices.data(), static_cast<UINT>(terrainGeometry.Vertices.size()), sizeof(Wasteland::TerrainNovaVertex));
    std::cerr << "[DEBUG-terrain-warp] terrain vertex upload complete\n";
    SetRequiredUniforms(nova, cameraPosition, viewProjection, emptySceneDiagnostic ? 0 : 1);
    std::cerr << "[DEBUG-terrain-warp] uniforms set\n";

    // WARP executes the backend command stream in-process. Refuse to launch a
    // shader pass if the backend's resize/clear setup already contains invalid
    // D3D12 commands; this keeps the smoke test diagnostic instead of crashing
    // inside the software driver.
    if (!debugLayerEnabled)
    {
        std::cerr << "BLOCKED: D3D12 debug layer unavailable; cannot validate DX12Nova's WARP command stream before dispatch.\n";
        return 2;
    }
    std::vector<std::string> validationErrors;
    if (!CollectD3D12Errors(device.Get(), validationErrors))
    {
        std::cerr << "BLOCKED: ID3D12InfoQueue unavailable; cannot validate DX12Nova's WARP command stream before dispatch.\n";
        return 2;
    }
    if (!validationErrors.empty())
    {
        std::cerr << "BLOCKED: DX12Nova::Resize/Clear reported " << validationErrors.size()
                  << " D3D12 validation error(s); refusing WARP dispatch.\n";
        const std::size_t shown = validationErrors.size() < 12 ? validationErrors.size() : 12;
        for (std::size_t i = 0; i < shown; ++i)
            std::cerr << "  - " << validationErrors[i] << '\n';
        return 2;
    }

    std::cerr << "[DEBUG-terrain-warp] pre-dispatch validation clean\n";
    const std::vector<ID3D12Resource*> sceneTextures(32, placeholderTexture.Get());
    // Minimal production schedule: trace, derive visibility/velocity, accumulate,
    // commit history, then composite. ReSTIR/denoising/bloom are intentionally
    // disabled so this smoke test covers the required Terrain->Nova image path.
    const int initialPass = emptySceneDiagnostic ? 99 : 1;
    const UINT dispatchWidth = zeroGroupsDiagnostic ? 0 : (singleGroupDiagnostic ? 8 : kImageWidth);
    const UINT dispatchHeight = zeroGroupsDiagnostic ? 0 : (singleGroupDiagnostic ? 8 : kImageHeight);
    std::cerr << "[DEBUG-terrain-warp] pass " << initialPass << " begin (groups input "
              << dispatchWidth << 'x' << dispatchHeight << ")\n";
    nova.Dispatch(initialPass, dispatchWidth, dispatchHeight, sceneTextures);
    std::cerr << "[DEBUG-terrain-warp] pass " << initialPass << " complete\n";
    if (emptySceneDiagnostic)
    {
        std::vector<unsigned char> emptySceneOutput;
        UINT emptyWidth = 0;
        UINT emptyHeight = 0;
        Require(nova.Readback(0, emptySceneOutput, emptyWidth, emptyHeight),
                "DX12Nova::Readback(0) failed in empty-scene diagnostic");
        Require(emptyWidth == kImageWidth && emptyHeight == kImageHeight &&
                emptySceneOutput.size() == kImageWidth * kImageHeight * 4,
                "empty-scene readback extent/size mismatch");
        std::vector<std::string> emptySceneValidationErrors;
        Require(CollectD3D12Errors(device.Get(), emptySceneValidationErrors),
                "could not read the D3D12 info queue after empty-scene pass 1");
        if (!emptySceneValidationErrors.empty())
        {
            std::cerr << "FAIL: empty-scene pass 1/readback reported " << emptySceneValidationErrors.size()
                      << " D3D12 validation error(s).\n";
            for (const std::string& error : emptySceneValidationErrors)
                std::cerr << "  - " << error << '\n';
            return 3;
        }
        std::cerr << "[DEBUG-terrain-warp] empty-scene no-op pass and readback completed\n";
        return 0;
    }
    nova.Dispatch(0, kImageWidth, kImageHeight, sceneTextures);
    std::cerr << "[DEBUG-terrain-warp] pass 0 complete\n";
    nova.Dispatch(2, kImageWidth, kImageHeight, sceneTextures);
    std::cerr << "[DEBUG-terrain-warp] pass 2 complete\n";
    nova.CommitHistory();
    std::cerr << "[DEBUG-terrain-warp] history committed\n";
    nova.Dispatch(6, kImageWidth, kImageHeight, sceneTextures);
    std::cerr << "[DEBUG-terrain-warp] pass 6 complete\n";

    std::vector<unsigned char> albedoHit;
    UINT readWidth = 0;
    UINT readHeight = 0;
    std::cerr << "[DEBUG-terrain-warp] albedo readback begin\n";
    const bool albedoReadbackOk = nova.Readback(4, albedoHit, readWidth, readHeight);
    {
        // Test-only: surface the debug layer's own message for the readback
        // failure before Require throws and unwinds out of RunTest.
        std::vector<std::string> readbackErrors;
        if (CollectD3D12Errors(device.Get(), readbackErrors))
        {
            std::cerr << "[DEBUG-terrain-warp] readback infoqueue messages="
                      << readbackErrors.size() << '\n';
            for (const std::string& message : readbackErrors)
                std::cerr << "[DEBUG-terrain-warp] readback message: " << message << '\n';
        }
    }
    Require(albedoReadbackOk, "DX12Nova::Readback(4) failed for the terrain hit buffer");
    std::cerr << "[DEBUG-terrain-warp] albedo readback complete\n";
    Require(readWidth == kImageWidth && readHeight == kImageHeight && albedoHit.size() == kImageWidth * kImageHeight * 4,
            "terrain-hit readback extent/size mismatch");
    const std::size_t center = (static_cast<std::size_t>(kImageHeight / 2) * kImageWidth + kImageWidth / 2) * 4;
    const std::array<std::uint8_t, 4> centerHit = {
        albedoHit[center], albedoHit[center + 1], albedoHit[center + 2], albedoHit[center + 3]
    };
    const std::array<std::uint8_t, 3> expectedAlbedo = {
        ToUnorm8(instance.Albedo.r), ToUnorm8(instance.Albedo.g), ToUnorm8(instance.Albedo.b)
    };
    Require(centerHit[3] >= 250, "center ray missed the terrain (shader albedo-hit alpha was not 1)");
    for (std::size_t channel = 0; channel < expectedAlbedo.size(); ++channel)
    {
        const int difference = std::abs(static_cast<int>(centerHit[channel]) - static_cast<int>(expectedAlbedo[channel]));
        Require(difference <= 2, "center hit did not contain the uploaded shape-8 terrain albedo");
    }

    std::vector<unsigned char> finalImage;
    std::cerr << "[DEBUG-terrain-warp] composite readback begin\n";
    Require(nova.Readback(0, finalImage, readWidth, readHeight), "DX12Nova::Readback(0) failed for the composited image");
    std::cerr << "[DEBUG-terrain-warp] composite readback complete\n";
    Require(readWidth == kImageWidth && readHeight == kImageHeight && finalImage.size() == kImageWidth * kImageHeight * 4,
            "composite readback extent/size mismatch");

    std::vector<std::string> postDispatchValidationErrors;
    Require(CollectD3D12Errors(device.Get(), postDispatchValidationErrors),
            "could not read the D3D12 info queue after dispatch/readback");
    if (!postDispatchValidationErrors.empty())
    {
        std::cerr << "FAIL: DX12Nova dispatch/readback reported " << postDispatchValidationErrors.size()
                  << " D3D12 validation error(s).\n";
        for (const std::string& error : postDispatchValidationErrors)
            std::cerr << "  - " << error << '\n';
        return 3;
    }

    const std::array<std::uint8_t, 4> centerFinal = {
        finalImage[center], finalImage[center + 1], finalImage[center + 2], finalImage[center + 3]
    };
    // This is a composite-output sanity check only: the exact terrain-hit
    // assertion is the uploaded albedo check above, since sky can also be nonzero.
    Require(centerFinal[3] >= 250 && (centerFinal[0] > 0 || centerFinal[1] > 0 || centerFinal[2] > 0),
            "composited center pixel remained zero or transparent");

    std::cout << "PASS: WARP DX12Nova Terrain GPU smoke test; cells=" << terrainGeometry.CellsPerSide
              << ", vertices=" << terrainGeometry.Vertices.size()
              << ", shape=8, center albedo-hit RGBA=(" << static_cast<int>(centerHit[0]) << ','
              << static_cast<int>(centerHit[1]) << ',' << static_cast<int>(centerHit[2]) << ','
              << static_cast<int>(centerHit[3]) << "), composite RGBA=("
              << static_cast<int>(centerFinal[0]) << ',' << static_cast<int>(centerFinal[1]) << ','
              << static_cast<int>(centerFinal[2]) << ',' << static_cast<int>(centerFinal[3]) << ")\n";
    std::cerr << "[DEBUG-terrain-warp] assertions passed; leaving test scope\n";
    return 0;
}
}

int main(int argc, char** argv)
{
    PVOID exceptionHandler = AddVectoredExceptionHandler(1, LogWARPException);
    try
    {
        const int result = RunTest(argc > 0 ? argv[0] : "TerrainNovaGpuTests.exe");
        if (exceptionHandler)
            RemoveVectoredExceptionHandler(exceptionHandler);
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (exceptionHandler)
            RemoveVectoredExceptionHandler(exceptionHandler);
        return 1;
    }
}
