#include "wlpch.h"
#include "DX12Nova.h"
#include "Wasteland/Core/Log.h"

#include <glm/glm.hpp>
#define D3DX12_NO_STATE_OBJECT_HELPERS
#define D3DX12_NO_CHECK_FEATURE_SUPPORT_CLASS
#include <directx/d3dx12.h>

namespace Wasteland {

DX12Nova::~DX12Nova()
{
    if (constantBufferMapped && constantBuffer) {
        constantBuffer->Unmap(0, nullptr);
    }
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    for (auto& buf : structuredBuffers) buf.Reset();
}

void DX12Nova::Init(ID3D12Device* d, ID3D12CommandQueue* q)
{
    if (device.Get() == d && pipelineState) return;
    device = d;
    commandQueue = q;

    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    Check(hr);

    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
    Check(hr);
    hr = commandList->Close();
    Check(hr);

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    Check(hr);
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    Com<ID3DBlob> error;
    hr = D3DCompileFromFile(L"assets/shaders/Renderer3D_NovaRenderer.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &computeShader, &error);
    if (FAILED(hr)) {
        throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Nova shader compile failed");
    }

    Com<ID3D12ShaderReflection> reflection;
    hr = D3DReflect(computeShader->GetBufferPointer(), computeShader->GetBufferSize(),
        IID_ID3D12ShaderReflection, (void**)reflection.GetAddressOf());
    Check(hr);

    ID3D12ShaderReflectionConstantBuffer* cb = nullptr;
    for (UINT i = 0; ; i++) {
        cb = reflection->GetConstantBufferByIndex(i);
        if (!cb) break;
        D3D12_SHADER_BUFFER_DESC desc;
        cb->GetDesc(&desc);
        if (strcmp(desc.Name, "NovaConstants") == 0) break;
        cb = nullptr;
    }
    if (!cb) throw std::runtime_error("NovaConstants cbuffer not found");

    D3D12_SHADER_BUFFER_DESC cbDesc;
    cb->GetDesc(&cbDesc);
    bytes.resize(cbDesc.Size);
    for (UINT j = 0; j < cbDesc.Variables; j++) {
        ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
        D3D12_SHADER_VARIABLE_DESC vdesc;
        var->GetDesc(&vdesc);
        variables[vdesc.Name] = vdesc;
    }

    CreateConstantBuffer(cbDesc.Size);
    CreateRootSignature();
    CreatePSO();
    CreateDescriptorHeaps();
}

void DX12Nova::CreateConstantBuffer(UINT size)
{
    constantBufferSize = (size + 255) & ~255u;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = constantBufferSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer));
    Check(hr);

    D3D12_RANGE readRange = { 0, 0 };
    hr = constantBuffer->Map(0, &readRange, &constantBufferMapped);
    Check(hr);
}

void DX12Nova::CreateRootSignature()
{
    CD3DX12_ROOT_PARAMETER1 rootParams[8];
    CD3DX12_DESCRIPTOR_RANGE1 ranges[4];

    rootParams[0].InitAsConstantBufferView(1);
    rootParams[1].InitAsShaderResourceView(1);
    rootParams[2].InitAsShaderResourceView(2);
    rootParams[3].InitAsShaderResourceView(3);

    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 15);
    rootParams[4].InitAsDescriptorTable(1, &ranges[0]);

    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 48);
    rootParams[5].InitAsDescriptorTable(1, &ranges[1]);

    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 64);
    rootParams[6].InitAsDescriptorTable(1, &ranges[2]);

    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0);
    rootParams[7].InitAsDescriptorTable(1, &ranges[3]);

    D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ShaderRegister = 0;
    samplerDesc.RegisterSpace = 0;
    samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(_countof(rootParams), rootParams, 1, &samplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Com<ID3DBlob> sig, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &error);
    if (FAILED(hr)) {
        throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Root signature serialize failed");
    }

    hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    Check(hr);
}

void DX12Nova::CreatePSO()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS.pShaderBytecode = computeShader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = computeShader->GetBufferSize();

    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    Check(hr);
}

void DX12Nova::CreateDescriptorHeaps()
{
    const UINT numDescriptors = 57;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = numDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cbvSrvUavHeap));
    Check(hr);

    cbvSrvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = constantBufferSize;
    device->CreateConstantBufferView(&cbvDesc, cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart());
}

void DX12Nova::CreateImages(UINT w, UINT h)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT
    };

    auto cpuHandle = cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    UINT uavIndex = 49;

    for (int i = 0; i < 8; i++) {
        desc.Format = formats[i];

        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&images[i].texture));
        Check(hr);

        D3D12_RESOURCE_DESC snapDesc = desc;
        snapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &snapDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&images[i].snapshot));
        Check(hr);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format = formats[i];
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuHandle;
        uavHandle.ptr += uavIndex * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(images[i].texture.Get(), nullptr, &uavDesc, uavHandle);
        images[i].uavCpu = uavHandle;
        uavIndex++;
    }

    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&history.texture));
    Check(hr);
    D3D12_RESOURCE_DESC snapDesc = desc;
    snapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &snapDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&history.snapshot));
    Check(hr);
}

void DX12Nova::Resize(UINT w, UINT h)
{
    if (!w || !h || (width == w && height == h)) return;
    Unbind();
    width = w;
    height = h;
    CreateImages(w, h);
    Clear();
}

void DX12Nova::Clear()
{
    if (!width) return;
    Unbind();
    FlushCommandList();
    FlushCommandList();
}

void DX12Nova::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

void DX12Nova::FlushCommandList()
{
    commandList->Close();
    ID3D12CommandList* lists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), nullptr);
}

void DX12Nova::EnsureConstantBufferSize(UINT size)
{
    if (size <= constantBufferSize) return;
    if (constantBufferMapped && constantBuffer) {
        constantBuffer->Unmap(0, nullptr);
        constantBufferMapped = nullptr;
    }
    constantBuffer.Reset();
    CreateConstantBuffer(size);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = constantBufferSize;
    auto cpuHandle = cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateConstantBufferView(&cbvDesc, cpuHandle);
}

template<class T>
void DX12Nova::Set(const char* name, const T& value, UINT index)
{
    auto it = variables.find(name);
    if (it == variables.end()) return;
    UINT offset = it->second.StartOffset + (index * 16);
    if (offset + sizeof(T) > it->second.StartOffset + it->second.Size)
        throw std::runtime_error("Nova constant size mismatch");
    memcpy(bytes.data() + offset, &value, sizeof(T));
}

void DX12Nova::Upload(UINT slot, const void* data, UINT count, UINT stride)
{
    if (slot >= 3) return;
    structuredBuffers[slot].Reset();

    UINT bufferSize = (count ? count : 1) * stride;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bufferSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&structuredBuffers[slot]));
    Check(hr);

    void* mapped;
    D3D12_RANGE readRange = { 0, 0 };
    hr = structuredBuffers[slot]->Map(0, &readRange, &mapped);
    Check(hr);
    if (data && count > 0) {
        memcpy(mapped, data, count * stride);
    } else {
        memset(mapped, 0, stride);
    }
    structuredBuffers[slot]->Unmap(0, nullptr);
}

void DX12Nova::Unbind()
{
}

void DX12Nova::Dispatch(int pass, UINT w, UINT h, const std::vector<ID3D12Resource*>& textures)
{
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), pipelineState.Get());
    commandList->SetComputeRootSignature(rootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { cbvSrvUavHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    commandList->SetComputeRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(1, structuredBuffers[0]->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(2, structuredBuffers[1]->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(3, structuredBuffers[2]->GetGPUVirtualAddress());

    auto cpuHandle = cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += 4 * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(4, srvHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE sampHandle = cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    sampHandle.ptr += 36 * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(5, sampHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE imgHandle = cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    imgHandle.ptr += 41 * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(6, imgHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += 49 * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(7, uavHandle);

    memcpy(constantBufferMapped, bytes.data(), bytes.size());

    for (size_t i = 0; i < textures.size() && i < 32; i++) {
        if (!textures[i]) continue;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (4 + (UINT)i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(textures[i], &srvDesc, dest);
    }

    for (int i = 0; i < 8; i++) {
        Transition(images[i].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(images[i].snapshot.Get(), images[i].texture.Get());
        Transition(images[i].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    for (int i = 0; i < 8; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (41 + i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(images[i].snapshot.Get(), &srvDesc, dest);
    }

    int sampIndices[] = { 1, 6, 0, 6, 4 };
    for (int i = 0; i < 5; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (36 + i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(images[sampIndices[i]].snapshot.Get(), &srvDesc, dest);
    }

    commandList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    FlushCommandList();
}

void DX12Nova::CommitHistory()
{
    if (!width) return;
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(history.snapshot.Get(), images[1].texture.Get());
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushCommandList();
}

void DX12Nova::CommitDenoise()
{
    if (!width) return;
    Transition(images[7].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(images[1].texture.Get(), images[7].texture.Get());
    Transition(images[7].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushCommandList();
}

void* DX12Nova::Output(UINT index)
{
    if (!width) return nullptr;
    Unbind();
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(images[index].snapshot.Get(), images[index].texture.Get());
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushCommandList();
    return images[index].snapshot.Get();
}

template void DX12Nova::Set<int>(const char*, const int&, UINT);
template void DX12Nova::Set<unsigned int>(const char*, const unsigned int&, UINT);
template void DX12Nova::Set<float>(const char*, const float&, UINT);
template void DX12Nova::Set<glm::vec2>(const char*, const glm::vec2&, UINT);
template void DX12Nova::Set<glm::vec3>(const char*, const glm::vec3&, UINT);
template void DX12Nova::Set<glm::vec4>(const char*, const glm::vec4&, UINT);
template void DX12Nova::Set<glm::mat4>(const char*, const glm::mat4&, UINT);

} // namespace Wasteland
