// The DX12 Nova path: a hand-written D3D12 compute backend for the
// ray-traced renderer, sitting beside VulkanNova.cpp and DX11Nova as
// the other implementations of the same Nova interface.
//
// Unlike NVRHIFramebuffer.cpp next door, nothing here goes through
// NVRHI: this is raw D3D12, so every descriptor, barrier and fence is
// spelled out rather than abstracted.
#include "wlpch.h"
#include "DX12Nova.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Renderer/NeuralStageBlob.h"

// glm is used for the typed Set() instantiations at the bottom of this
// file -- the CPU-side constant values are glm types so they match the
// HLSL declarations in the shader byte-for-byte.
#include <glm/glm.hpp>
// Both macros strip parts of the d3dx12.h helpers this file does not
// use. They must be defined BEFORE the include below, which is why they
// sit on their own lines rather than after it.
#define D3DX12_NO_STATE_OBJECT_HELPERS
#define D3DX12_NO_CHECK_FEATURE_SUPPORT_CLASS
#include <directx/d3dx12.h>

namespace Wasteland {

// Destruction order is the whole point of this function: the GPU may
// still be executing work that references every handle released below,
// so the drain has to happen first and everything else after.
DX12Nova::~DX12Nova()
{
    // Drain before releasing anything. Every image, buffer and descriptor heap
    // below is still referenced by in-flight submissions; the ComPtr destructors
    // would otherwise hand that memory back while the GPU is executing.
    WaitForGpuIdle();
    for (UINT i = 0; i < kFramesInFlight; i++)
        if (constantBufferMapped[i] && constantBuffers[i]) {
            constantBuffers[i]->Unmap(0, nullptr);
            constantBufferMapped[i] = nullptr;
        }
    // A full drain rather than a wait on just the current slot. Every slot
    // may have an in-flight submission, and the ComPtr destructors that
    // follow release resources ANY of them could be reading.
    // Unmap before releasing the buffer. A resource cannot be released
    // while it is still mapped, and the mapped pointer is cleared so the
    // later Reset cannot hand back a dangling address.
    // The fence event handle is closed AFTER the drain, never before:
    // WaitForGpuIdle waits on this very handle, so closing it first would
    // make the wait invalid.
    // The fence event must outlive every SetEventOnCompletion wait, so it is
    // closed after the drain, never before.
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    // The structured buffers are released here but NOT unmapped or
    // synchronised individually -- the drain above already covered them.
    // Resettable on a per-slot basis in Upload, since each upload replaces
    // the whole buffer rather than appending.
    for (auto& buf : structuredBuffers) buf.Reset();
}

// Full pipeline setup: device, ring of frame slots, shader compilation,
// reflection, root signature, PSO and descriptor heaps.
// Re-entrant by guard, not by design for partial re-init: the guard
// below returns early only when the device is unchanged AND a pipeline
// state already exists.
void DX12Nova::Init(ID3D12Device* d, ID3D12CommandQueue* q)
{
    // Idempotence guard. Both halves must hold: the same device AND a live
    // pipeline state. A device change with a surviving pipeline state falls
    // through and rebuilds everything against the new device.
    if (device.Get() == d && pipelineState) return;
    device = d;
    commandQueue = q;

    // The fence starts at value 0 with no flags. A monotonically increasing
    // value is signalled after each submission (see FlushCommandList), so
    // "completed value >= N" means "everything up to submission N is done".
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    Check(hr);
    // Manual-reset, initially non-signalled event, reused for every wait in
    // this file. Safe to reuse because each wait arms it with a fresh
    // SetEventOnCompletion before waiting.
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // One allocator + command list per frame slot. These are created once and
    // reused for the lifetime of the device; per-submission they are reset in
    // BeginSubmit() only after the fence proves the slot retired.
    for (UINT i = 0; i < kFramesInFlight; i++)
    {
        // The frame ring: one allocator and one command list per slot, so three
        // submissions can be recorded back-to-back without waiting. The slot is
        // recycled only after its fence proves the previous use retired.
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&frames[i].allocator));
        Check(hr);
        // The command list is created CLOSED (Close() on the next line).
        // A freshly created command list is in the recording state, and this
        // file only ever records after an explicit Reset in BeginSubmit.
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            frames[i].allocator.Get(), nullptr, IID_PPV_ARGS(&frames[i].commandList));
        Check(hr);
        hr = frames[i].commandList->Close();
        Check(hr);
    }

    Com<ID3DBlob> error;
    // Resolved through Project so the compute shader comes from the active
    // project's assets rather than the process working directory.
    // The shader path is resolved through the active PROJECT, not the
    // process working directory. That matters for an editor: the CWD is
    // whatever directory the process was launched from, which has nothing to
    // do with which project the user has open.
    const std::wstring novaShaderPath =
        Project::Get().GetAssetPath("shaders/Renderer3D_NovaRenderer.hlsl").wstring();
    // Compiled at runtime rather than through the offline shader pipeline.
    // That is a deliberate trade: it keeps one .hlsl as the single source of
    // truth, at the cost of a compile at startup and no precompiled bytecode.
    const D3D_SHADER_MACRO shaderMacros[] = {
        { "WASTELAND_NOVA_TERRAIN_GRID", "1" },
        { nullptr, nullptr }
    };
    hr = D3DCompileFromFile(novaShaderPath.c_str(), shaderMacros,
        // cs_5_0: shader model 5.0 compute. The entry point is "main", so the
        // shader needs no [numthreads] entry naming in the build system --
        // it is named here instead.
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &computeShader, &error);
    if (FAILED(hr)) {
        // A failed compile throws with the compiler's own message, which is
        // the difference between "DX12 Nova shader compile failed" and a
        // readable syntax error with a line number.
        throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Nova shader compile failed");
    }

    // Reflection, not a hand-written constant layout. The offsets below
    // are whatever the HLSL compiler decided, which is why no magic
    // numbers appear in Set() and why a shader edit cannot silently
    // desynchronise the CPU-side writes.
    Com<ID3D12ShaderReflection> reflection;
    hr = D3DReflect(computeShader->GetBufferPointer(), computeShader->GetBufferSize(),
        IID_ID3D12ShaderReflection, (void**)reflection.GetAddressOf());
    Check(hr);

    ID3D12ShaderReflectionConstantBuffer* cb = nullptr;
    // Scans every constant buffer for one named NovaConstants. The shader
    // may declare more than one, and picking by name rather than index 0
    // means the layout of the others cannot break this.
    for (UINT i = 0; ; i++) {
        cb = reflection->GetConstantBufferByIndex(i);
        if (!cb) break;
        D3D12_SHADER_BUFFER_DESC desc;
        cb->GetDesc(&desc);
        // Exact strcmp, so a rename of the cbuffer in the shader fails loudly
        // here rather than producing a silent zero-filled constant buffer.
        if (strcmp(desc.Name, "NovaConstants") == 0) break;
        cb = nullptr;
    }
    if (!cb) throw std::runtime_error("NovaConstants cbuffer not found");

    D3D12_SHADER_BUFFER_DESC cbDesc;
    cb->GetDesc(&cbDesc);
    // The staging byte array is sized to the shader's declared size, NOT
    // the 256-aligned allocation size below. The difference is padding the
    // Set() bounds check must not write past.
    bytes.resize(cbDesc.Size);
    for (UINT j = 0; j < cbDesc.Variables; j++) {
        ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
        D3D12_SHADER_VARIABLE_DESC vdesc;
        var->GetDesc(&vdesc);
        // Records every variable's name and byte offset. This map is what Set()
        // looks names up in, which is why an unknown name is a silent no-op
        // rather than a crash -- see Set() below.
        variables[vdesc.Name] = vdesc;
    }

    // Creation order matters: the constant buffer needs its size (from
    // reflection), the root signature and PSO need the shader, and the
    // descriptor heaps need the CBV to exist so slot 0 can point at it.
    CreateConstantBuffer(cbDesc.Size);
    CreateRootSignature();
    CreatePSO();
    CreateDescriptorHeaps();

    // Dispatch binds all four structured-buffer SRVs before the first scene
    // upload may exist (including after backend recreation). Upload's zero-count
    // path allocates one stride and clears it; queue, fence, and event are ready
    // above, so its retirement wait is valid here.
    Upload(0, nullptr, 0, 288); // RayTracingInstance: 288-byte shader ABI record.
    Upload(1, nullptr, 0, 32);  // BVHNodeGPU: 32-byte shader ABI record.
    Upload(2, nullptr, 0, sizeof(int)); // Light index: int.
    Upload(3, nullptr, 0, 32);  // TerrainNovaVertex: 32-byte shader ABI record.
}

// 256-byte alignment is a D3D12 requirement for constant buffers, not
// an optimisation: the shader is compiled with cbuffer offsets on that
// boundary, so an unaligned allocation would not match.
void DX12Nova::CreateConstantBuffer(UINT size)
{
    // Round UP to 256: `(size + 255) & ~255`. This is the standard
    // round-up-to-mask idiom -- adding 255 then clearing the low bits. On
    // overflow this would wrap, but the size comes from shader reflection
    // and is small.
    // UPLOAD heap for the constant buffer: CPU-writable, GPU-readable.
    // That is exactly what a per-frame uniform block needs, and it is why
    // the buffer is kept mapped for the object's lifetime instead of being
    // mapped and unmapped per frame.
    constantBufferSize = (size + 255) & ~255u;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // Built field by field rather than with a helper, so every field that
    // D3D12 requires is visibly set. Width is the only one that varies.
    desc.Width = constantBufferSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // One upload buffer PER SLOT, each kept mapped. See the constantBuffers
    // declaration for why this cannot be a single shared buffer: a slot's
    // command list may still be reading its uniforms when the next slot's
    // Dispatch overwrites them, and since u_PassID is one of those uniforms,
    // that is a wrong-pass-number bug rather than a stale float.
    for (UINT i = 0; i < kFramesInFlight; i++)
    {
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            // Created in GENERIC_READ, which is the correct initial state for a
            // buffer read by a shader. Combined with the persistent mapping
            // below, a Dispatch is just a memcpy into mapped memory.
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffers[i]));
        Check(hr);

        D3D12_RANGE readRange = { 0, 0 };
        // An empty read range tells D3D12 the CPU will not read this resource,
        // so the driver may skip any write-combining/cache coherency handling.
        // Upload() does the same for its buffers.
        hr = constantBuffers[i]->Map(0, &readRange, &constantBufferMapped[i]);
        Check(hr);
    }
}

// The root signature is where the binding layout is fixed. Everything
// below -- descriptor table bases, register numbers -- is decided here
// and must agree with the shader's register declarations and with the
// heap layout constants in the header.
void DX12Nova::CreateRootSignature()
{
    // Fixed-size arrays sized by the constants that use them. Changing
    // kUavCount or adding a root parameter without resizing these is an
    // out-of-bounds write in _countof() below.
    CD3DX12_ROOT_PARAMETER1 rootParams[10];
    CD3DX12_DESCRIPTOR_RANGE1 ranges[4];

    // Root parameters 0..3 are single descriptors, not tables: the constant
    // buffer and the first three structured buffers (instances, BVH nodes,
    // light indices). The terrain buffer is bound separately at root parameter
    // 9 below. Each declares a count of 1, which for a non-table root parameter
    // is the "how many descriptors" field and must be 1.
    rootParams[0].InitAsConstantBufferView(1);
    rootParams[1].InitAsShaderResourceView(1);
    rootParams[2].InitAsShaderResourceView(2);
    rootParams[3].InitAsShaderResourceView(3);

    // The three SRV ranges: 32 scene textures, 6 sampled snapshots, 10
    // readable snapshots. The third argument is the SHADER REGISTER, not a
    // heap offset -- heap offsets are the kXxxBase constants used in
    // Dispatch. The two numbering schemes are independent and both must
    // match the shader.
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 15);
    // Root parameter 4: the 32 scene-texture SRVs. Descriptor tables
    // rather than a descriptor array because 32 descriptors exceed the 64
    // root-descriptor register limit a non-table parameter could use.
    rootParams[4].InitAsDescriptorTable(1, &ranges[0]);

    // Six, not five: t53 is s_Moments, the previous frame's moments that pass 12
    // samples at a reprojected UV. A descriptor range is contiguous, so covering
    // t53 from a base of 48 means the range is 6 wide even though t48..t52 were
    // the only registers used before.
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 48);
    rootParams[5].InitAsDescriptorTable(1, &ranges[1]);

    // Eleven, not ten: t74 is NeuralWeights, the StructuredBuffer<float> the
    // neural eval pass reads. Contiguity from a base of 64 forces the range to
    // 11 to reach t74. Same reason as the sampled block above, and the reason
    // the readable block ends at heap index 52 -- which is why the UAV block
    // below had to move to 53.
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11, 64);
    rootParams[6].InitAsDescriptorTable(1, &ranges[2]);

    // UAV count is 13, not 12. The shader declares u0..u7, u9, u10, u11 and --
    // since the neural stage -- u12 (NeuralGlobals, an RWStructuredBuffer). u8
    // is still a hole nothing declares. The history image is the u11 one (its
    // own descriptor so Clear() can clear it without aliasing an image the
    // compute passes write), the moments image is u9, and the ReSTIR reservoir
    // write is u10. Getting this wrong silently writes a clear into a pass
    // output. UAV descriptor table: kUavCount entries, covering u0..u12. The
    // range is contiguous, so the hole at u8 still costs a slot. Sizing this
    // from anything other than the shader's highest UAV register is what
    // produced the E_INVALIDARG on CreateComputePipelineState.
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0);
    rootParams[7].InitAsDescriptorTable(1, &ranges[3]);

    // Root parameter 8: the ReSTIR reservoir read SRV at t10. This is a single
    // descriptor (not a table), bound by GPU virtual address like the scene
    // structured buffers above and the terrain buffer at root parameter 9.
    // The reservoir is a ping-pong pair: two buffers,
    // read from one and write to the other, swapping each frame.
    rootParams[8].InitAsShaderResourceView(10);

    // Root parameter 9: DX12-only terrain-grid vertices at t4. The HLSL
    // declaration is enabled only for this D3DCompileFromFile path.
    rootParams[9].InitAsShaderResourceView(4);

    // One static sampler for the whole signature, at register 0 of space 0.
    // ShaderVisibility ALL rather than COMPUTE, so it is also valid if a
    // graphics pipeline is ever built against this signature.
    D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    // MaxAnisotropy 16 is the hardware maximum on every shipping D3D12
    // adapter; a higher value is not clamped by the API.
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ShaderRegister = 0;
    samplerDesc.RegisterSpace = 0;
    samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // A VERSIONED root signature descriptor, serialised as version 1.1.
    // The version is stated twice -- here and in the Serialize call below --
    // and the two must agree, which is the usual cause of a versioned-
    // signature creation failure.
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(_countof(rootParams), rootParams, 1, &samplerDesc,
        // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT on a COMPUTE-only pipeline is
        // unnecessary. Harmless, and it would be needed if a graphics PSO were
        // ever built from this signature.
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Com<ID3DBlob> sig, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &error);
    if (FAILED(hr)) {
        throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Root signature serialize failed");
    }

    hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    Check(hr);
}

// The compute PSO. Only two fields of the descriptor are set: the
// root signature and the compiled bytecode. Everything else in
// D3D12_COMPUTE_PIPELINE_STATE_DESC is zero-initialised by the {} above,
// which is required -- D3D12 validates the whole struct.
void DX12Nova::CreatePSO()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS.pShaderBytecode = computeShader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = computeShader->GetBufferSize();

    // The PSO owns a reference to both the root signature and the shader
    // bytecode, so this is the point after which those may be released.
    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    Check(hr);
}

// Descriptor heap creation. The per-slot decision below is the
// load-bearing part of this function; everything else is bookkeeping.
void DX12Nova::CreateDescriptorHeaps()
{
    // ONE HEAP PER FRAME SLOT. Not a shared heap: the scene-texture SRVs are
    // rewritten before every dispatch, and a single heap would be mutated while
    // a previously-submitted command list is still reading it. Per-slot heaps
    // are what make the scene-texture rewrite safe without stalling.
    // Per-slot heaps, NOT one shared heap. The comment above explains
    // why: scene-texture descriptors are rewritten every dispatch, and a
    // shared heap would be mutated while an in-flight command list still
    // reads it. Three slots means up to two in-flight submissions can be
    // reading different heaps at once.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // The heap size is kDescriptorsPerHeap, which is derived in the header
    // as kUavBase + kUavCount so the layout and the allocation cannot drift
    // apart by hand-editing one constant.
    heapDesc.NumDescriptors = kDescriptorsPerHeap;
    // SHADER_VISIBLE is required here because the root signature binds
    // descriptor TABLES. A CPU-visible-only heap cannot back a root
    // descriptor table.
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    // Descriptor handles are STRIDE-based, not byte-sized: every offset in
    // this file is `handle.ptr += n * cbvSrvUavDescriptorSize`. Hardcoding a
    // byte size would be wrong on any device whose descriptors are not
    // exactly N bytes.
    cbvSrvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT i = 0; i < kFramesInFlight; i++)
    {
        HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&frames[i].descriptorHeap));
        Check(hr);
        frames[i].heapCpuStart = frames[i].descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // One CBV descriptor per slot, all naming the same buffer. The buffer
    // is shared; the DESCRIPTORS cannot be, because each slot's heap is
    // written independently.
    // Written into slot 0 of each heap -- index 0 because the heap starts
    // at heapCpuStart and the CBV is the first descriptor in the layout.
    // No offset arithmetic is needed here, unlike the UAV writes below.
    // The CBV at index 0 of every slot points at the same upload buffer. It is
    // rewritten per slot in Dispatch() (the constant bytes change every pass),
    // so each slot needs its own descriptor even though the resource is shared.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    // Slot i's descriptor names slot i's buffer. A single cbvDesc reused
    // across the loop would point every slot at whichever buffer was last
    // written, which is the shared-buffer bug this per-slot layout removes.
    for (UINT i = 0; i < kFramesInFlight; i++) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC slotCbv = cbvDesc;
        slotCbv.BufferLocation = constantBuffers[i]->GetGPUVirtualAddress();
        slotCbv.SizeInBytes = constantBufferSize;
        device->CreateConstantBufferView(&slotCbv, frames[i].heapCpuStart);
    }
}

// (Re)creates every render image and snapshot at a new size. Called
// from Init and from Resize, so it must be safe to run when the images
// already exist -- which is why the drain at the top is not optional.
void DX12Nova::CreateImages(UINT w, UINT h)
{
    // The GPU may still be reading the existing images (and the UAVs that name
    // them) from submissions in flight, so destroy nothing until it is done.
    // Without this, a resize or a render-scale change frees resources the GPU
    // is actively sampling.
    WaitForGpuIdle();

    D3D12_HEAP_PROPERTIES heapProps = {};
    // A FULL GPU drain on every resize. Not per-slot: the images and the
    // UAVs that name them are about to be replaced, and any in-flight
    // submission may be reading them. A resize is rare enough that the
    // stall is the right trade.
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    // ALLOW_UNORDERED_ACCESS on the render target. These images are both
    // written by compute and copied from, so they must be UAV-capable --
    // but see the snapshot below, which must NOT be.
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Per-image formats, matching the shader's u0..u7 declarations one
    // for one. Index 3 is RGBA8 (the composited colour) and index 5 is
    // R32G32_FLOAT (a two-channel intermediate), so these are not all the
    // same type and a single shared descriptor would be wrong.
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

    // The UAV descriptors are written once here and then never rewritten
    // until the next CreateImages, because the images themselves do not
    // change identity between resizes of OTHER sizes.
    // One UAV descriptor per image PER SLOT. Every slot's heap needs its own
    // descriptor even though they all name the same resource, because the
    // descriptor table the GPU reads lives in whichever slot submitted it.
    for (int i = 0; i < 8; i++) {
        // Hardcoded 8, matching the formats[] array above and the shader's
        // u0..u7. The nine UAVs are NOT created here -- the ninth is history,
        // created below.
        desc.Format = formats[i];

        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&images[i].texture));
        Check(hr);

        D3D12_RESOURCE_DESC snapDesc = desc;
        // A COPY of the descriptor, not the same one: the snapshot differs
        // from the render target in exactly one flag, and mutating desc after
        // the UAV create would retroactively describe the UAV resource.
        // Must allow shader-resource sampling: this snapshot is what ImGui
        // samples to draw the viewport. Without the flag the SRV is invalid and
        // the draw reads garbage.
        snapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &snapDesc,
            // The snapshot loses ALLOW_UNORDERED_ACCESS. A D3D12 resource cannot
            // be both a render target for compute and something ImGui samples
            // through a pixel shader in the same binding, and this is the copy that
            // exists purely to be sampled.
            // Created in COPY_DEST because the only thing that ever writes it is
            // CopyResource. Dispatch and Output track and bracket its combined
            // pixel/non-pixel shader-readable state.
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&images[i].snapshot));
        Check(hr);
        images[i].snapshotState = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format = formats[i];
        for (UINT s = 0; s < kFramesInFlight; s++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = frames[s].heapCpuStart;
            // The UAV descriptor is written into EVERY slot's heap. The reason is
            // not that the views differ -- they are all views of the same texture --
            // but that the heap a command list reads must contain the descriptor
            // when that list executes.
            // `kUavBase + i` is the heap INDEX; multiplying by the stride converts
            // it to a byte offset within the heap. This is the one place where a
            // stride error would be silent -- every other heap access has the same
            // shape, so a wrong stride looks consistent.
            uavHandle.ptr += (kUavBase + (UINT)i) * cbvSrvUavDescriptorSize;
            device->CreateUnorderedAccessView(images[i].texture.Get(), nullptr, &uavDesc, uavHandle);
            images[i].uavCpu[s] = uavHandle;
        }
    }

    // The history image is a UAV created outside the loop, which is why it does
    // not appear in formats[]. It binds at the shader's u11 -- see below -- not
    // at the next index after the image block.
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
    history.snapshotState = D3D12_RESOURCE_STATE_COPY_DEST;

    // The history image needs a UAV of its own: Clear() clears it directly.
    // It cannot share one of images[i]'s slots because that would clobber the
    // descriptor the compute passes read as an output.
    //
    // `kUavBase + 11` because that is the shader's u11 -- `img_HistoryBuffer`
    // is declared `register(u11)`. This is deliberately NOT "the slot after the
    // eight image UAVs": u8 is a hole the shader never declares, so binding
    // history there put a descriptor at a register nothing reads and left u11 --
    // the one the shader actually samples -- with no descriptor at all. Combined
    // with the UAV range stopping at u10, that is why every DX12 Nova init failed
    // with E_INVALIDARG. The index must track the SHADER's register, not the
    // position after the image block.
    D3D12_UNORDERED_ACCESS_VIEW_DESC histUavDesc = {};
    histUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    histUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (UINT s = 0; s < kFramesInFlight; s++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = frames[s].heapCpuStart;
        uavHandle.ptr += (kUavBase + 11) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(history.texture.Get(), nullptr, &histUavDesc, uavHandle);
        history.uavCpu[s] = uavHandle;
    }

    // The SVGF moments image: RG32F (mean, mean-of-squares), a tenth UAV at
    // u9. RG32F matches the shader's `RWTexture2D<float2>` and the DX11/RG32F
    // and GL/RG32F choices, so all four backends agree on the format.
    //
    // Its snapshot exists for the same reason the others do: pass 12 reads the
    // PREVIOUS frame's moments through t53, and reading the live texture while a
    // pass writes it is a read-write hazard. The snapshot is the pre-write copy.
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&moments.texture));
    Check(hr);
    D3D12_RESOURCE_DESC momSnapDesc = desc;
    momSnapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &momSnapDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&moments.snapshot));
    Check(hr);
    moments.snapshotState = D3D12_RESOURCE_STATE_COPY_DEST;

    // kUavBase + 9 is the moments slot, matching the shader's register(u9). The
    // slot at kUavBase + 8 is the hole at u8 -- the shader never declares it, so
    // it is simply left empty; a slot nothing binds costs nothing but the slot.
    // History, which used to sit there, is now at kUavBase + 11 (see above).
    D3D12_UNORDERED_ACCESS_VIEW_DESC momUavDesc = {};
    momUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    momUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    for (UINT s = 0; s < kFramesInFlight; s++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = frames[s].heapCpuStart;
        uavHandle.ptr += (kUavBase + 9) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(moments.texture.Get(), nullptr, &momUavDesc, uavHandle);
        moments.uavCpu[s] = uavHandle;
    }

    // ==========================================================================
    // Trained end-of-frame neural stage (docs/NEURAL_RENDERING.md)
    // ==========================================================================
    // Two resources, uploaded once here and then read-only:
    //
    //   neuralGlobals -- the u12 UAV. A raw uint buffer: the four frame means as
    //   raw float bits in elements 0..3, then one uint32 fixed-point partial per
    //   workgroup block. This is byte-for-byte the GLSL's NeuralGlobals SSBO
    //   (float g_Mean[4]; uint g_Partial[]), which is what lets the same shader
    //   source describe both backends. The GLSL's std430 SSBO and HLSL's
    //   StructuredBuffer<uint> both lay out elements at 4-byte strides from
    //   offset 0, so element i of the HLSL buffer is byte 4i of the SSBO -- and
    //   g_Partial[i] sits at byte 16+4i, which is element 4+i. Same thing.
    //
    //   neuralWeights -- the t74 SRV. The 1140 fp32 weights in the exporter's
    //   LAYER_SHAPES order, exactly the blob the GL path loads.
    //
    // Neither is per-frame: both are created once and their descriptors written
    // into every slot's heap, which is why this loop runs outside the per-slot
    // loop below.
    neuralGlobalsUavSlot = kUavBase + 12;
    neuralWeightsSrvSlot = kNeuralWeightsSrvSlot;
    {
        // Same byte count as the GL SSBO, from the shared constant rather than a
        // literal, so the two backends cannot disagree about the buffer size.
        D3D12_RESOURCE_DESC gdesc = CD3DX12_RESOURCE_DESC::Buffer(Neural::kGlobalsBytes);
        gdesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &gdesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&neuralGlobals));
        Check(hr);

        // The weights buffer is created here but FILLED in UploadNeuralWeights().
        // A resource cannot be memcpy'd into from a DEFAULT heap, so the upload
        // has to stage through an UPLOAD buffer and issue a CopyResource -- and
        // CopyResource is a command-list operation, which means it can only be
        // recorded from inside a submission. Resize() calls CreateImages() and
        // then Clear(), and Clear() opens the only command list that exists at
        // init time, so the upload is recorded there.
        //
        // Sized from the blob rather than a round number: a network that grows
        // past a guessed size would silently truncate, and the descriptor below
        // is built from the same number.
        Neural::WeightBlob blob;
        const bool haveBlob = Neural::LoadWeightBlob("assets/neural/neural_weights.bin", blob);
        const UINT64 weightsBytes = haveBlob ? (UINT64)blob.payload.size() : 0;
        if (haveBlob)
        {
            hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(weightsBytes),
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&neuralWeights));
            Check(hr);
        }
    }

    // ReSTIR reservoir UAV shape: one 32-byte structured element per pixel.
    D3D12_UNORDERED_ACCESS_VIEW_DESC resUavDesc = {};
    resUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    resUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    resUavDesc.Buffer.NumElements = w * h;
    resUavDesc.Buffer.StructureByteStride = 32;

    // Allocate both resources before writing any descriptor that names them.
    // CreateImages has drained the queue above, so releasing old resources and
    // replacing all of their descriptors is safe.
    reservoirClearCpuHeap.Reset();
    reservoirClearCpu = {};
    for (auto& heap : reservoirClearGpuHeaps) heap.Reset();
    for (int i = 0; i < 2; ++i)
    {
        reservoir[i].Reset();
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width = (UINT64)w * h * 32;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // UAV for write, SRV for read. The ping-pong swaps which is which.
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr2 = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reservoir[i]));
        Check(hr2);
        reservoirStates[i] = D3D12_RESOURCE_STATE_COMMON;
    }
    reservoirParity = 0;

    // The per-slot shader-visible u10 descriptor is rebuilt for the current
    // write target in Dispatch(), but initialize it only after both resources
    // exist so every heap begins with a valid descriptor.
    for (UINT s = 0; s < kFramesInFlight; s++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = frames[s].heapCpuStart;
        uavHandle.ptr += (kUavBase + 10) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(reservoir[1 - reservoirParity].Get(), nullptr,
                                          &resUavDesc, uavHandle);
    }

    // ClearUnorderedAccessView* requires the CPU descriptor to come from a
    // non-shader-visible heap. Keep one matching CPU descriptor per resource.
    D3D12_DESCRIPTOR_HEAP_DESC clearCpuDesc = {};
    clearCpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    clearCpuDesc.NumDescriptors = 2;
    clearCpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT clearHr = device->CreateDescriptorHeap(&clearCpuDesc, IID_PPV_ARGS(&reservoirClearCpuHeap));
    Check(clearHr);
    D3D12_CPU_DESCRIPTOR_HANDLE clearCpuStart = reservoirClearCpuHeap->GetCPUDescriptorHandleForHeapStart();

    // Each immutable shader-visible clear heap places its resource's view in
    // the u10 slot. Separate heaps keep the two GPU handles valid until the
    // submitted clears execute, while the CPU descriptors above match them.
    D3D12_DESCRIPTOR_HEAP_DESC clearGpuDesc = {};
    clearGpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    clearGpuDesc.NumDescriptors = kUavBase + kUavCount;
    clearGpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for (UINT i = 0; i < 2; ++i)
    {
        reservoirClearCpu[i] = clearCpuStart;
        reservoirClearCpu[i].ptr += i * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(reservoir[i].Get(), nullptr, &resUavDesc, reservoirClearCpu[i]);

        clearHr = device->CreateDescriptorHeap(&clearGpuDesc, IID_PPV_ARGS(&reservoirClearGpuHeaps[i]));
        Check(clearHr);
        D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpu = reservoirClearGpuHeaps[i]->GetCPUDescriptorHandleForHeapStart();
        gpuHeapCpu.ptr += (kUavBase + 10) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(reservoir[i].Get(), nullptr, &resUavDesc, gpuHeapCpu);
    }
    reservoirUavDesc = resUavDesc;
}

// Resize is guarded, so a same-size call is free. The zero checks
// matter because CreateImages would create a zero-extent texture, which
// D3D12 rejects.
void DX12Nova::Resize(UINT w, UINT h)
{
    if (!w || !h || (width == w && height == h)) return;
    Unbind();
    // Unbind before rebuilding. No-op on this backend (see Unbind below),
    // but kept so the DX11/DX12 pair read the same and a future backend
    // that does bind state has the right order.
    width = w;
    height = h;
    // Clear after CreateImages, because freshly created UAVs contain
    // undefined data and the first accumulation frame would read garbage.
    // This is also why the Drain inside CreateImages is safe to do first.
    CreateImages(w, h);
    Clear();
}

// Clear() records its own submission: it is not folded into the next
// Dispatch, because Clear must complete before any pass reads the
// images it just zeroed.
// The zero-width guard: every method that touches images checks this
// first, because before the first Resize the images do not exist and a
// barrier or copy against a null resource is undefined.
// Adopts a frame slot and opens its command list. Clear is a full
// submission like Dispatch, so it goes through the same ring rather
// than recording into whatever list happens to be open.
void DX12Nova::Clear()
{
    // D4: THIS USED TO CLEAR NOTHING. The old body called FlushCommandList()
    // twice with no commands recorded between them -- two empty submissions.
    // DX11Nova::Clear() does clear its UAVs; this now matches, so DX12
    // accumulation actually resets when a resize or render-scale change
    // demands it (previously stale history survived and bled into the new
    // resolution).
    if (!width) return;
    Unbind();

    BeginSubmit();
    // Recorded here, inside the submission this function already opens: the
    // weight upload is a CopyResource plus a state transition, and both are
    // command-list operations. Doing it in CreateImages() would record into a
    // command list that does not exist yet (Resize calls CreateImages before any
    // submission), which is a null-pointer call rather than a subtle bug.
    UploadNeuralWeights();
    FrameContext &slot = frames[currentFrame];

    // ClearUnorderedAccessViewFloat needs the GPU descriptor in a shader-visible
    // heap and a matching CPU descriptor in a non-shader-visible heap. The slot's
    // image CPU handles belong to its shader-visible heap, so build CPU-only views.
    D3D12_DESCRIPTOR_HEAP_DESC clearCpuDesc = {};
    clearCpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    clearCpuDesc.NumDescriptors = 10; // images[0..7], history, moments
    clearCpuDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Com<ID3D12DescriptorHeap> imageClearCpuHeap;
    Check(device->CreateDescriptorHeap(&clearCpuDesc, IID_PPV_ARGS(&imageClearCpuHeap)));

    std::array<ID3D12Resource*, 10> clearTargets = {
        images[0].texture.Get(), images[1].texture.Get(), images[2].texture.Get(), images[3].texture.Get(),
        images[4].texture.Get(), images[5].texture.Get(), images[6].texture.Get(), images[7].texture.Get(),
        history.texture.Get(), moments.texture.Get()
    };
    D3D12_CPU_DESCRIPTOR_HANDLE clearCpu = imageClearCpuHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < static_cast<UINT>(clearTargets.size()); ++i)
    {
        D3D12_RESOURCE_DESC targetDesc = clearTargets[i]->GetDesc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC clearUavDesc = {};
        clearUavDesc.Format = targetDesc.Format;
        clearUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        clearUavDesc.Texture2D.MipSlice = 0;
        clearUavDesc.Texture2D.PlaneSlice = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE targetCpu = clearCpu;
        targetCpu.ptr += static_cast<SIZE_T>(i) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(clearTargets[i], nullptr, &clearUavDesc, targetCpu);
    }

    commandList->SetComputeRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { slot.descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart = slot.descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = heapGpuStart;
    uavTable.ptr += kUavBase * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(7, uavTable);

    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Order any earlier UAV writes to these resources before clearing them.
    // The fence wait below orders later consumers; it cannot replace this
    // resource-level barrier before the clear operation.
    for (UINT i = 0; i < static_cast<UINT>(clearTargets.size()); ++i)
        UavBarrier(clearTargets[i]);

    for (UINT i = 0; i < static_cast<UINT>(clearTargets.size()); ++i)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE viewGpu = uavTable;
        viewGpu.ptr += i * cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE viewCpu = clearCpu;
        viewCpu.ptr += static_cast<SIZE_T>(i) * cbvSrvUavDescriptorSize;
        commandList->ClearUnorderedAccessViewFloat(viewGpu, viewCpu, clearTargets[i], zero, 0, nullptr);
    }

    // Structured buffers cannot be cleared with ClearUnorderedAccessViewUint.
    // Copy zero bytes from a bounded upload resource instead, transitioning each
    // reservoir through COPY_DEST and back to UAV.
    std::array<UINT64, 2> reservoirBytes = {};
    for (UINT i = 0; i < 2; ++i)
    {
        if (!reservoir[i])
            throw std::runtime_error("DX12 Nova reservoir buffer is missing during Clear");
        reservoirBytes[i] = reservoir[i]->GetDesc().Width;
        if (!reservoirBytes[i])
            throw std::runtime_error("DX12 Nova reservoir buffer has zero size during Clear");
    }

    constexpr UINT64 kReservoirZeroChunkBytes = 1024u * 1024u;
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC zeroUploadDesc = {};
    zeroUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    zeroUploadDesc.Width = kReservoirZeroChunkBytes;
    zeroUploadDesc.Height = 1;
    zeroUploadDesc.DepthOrArraySize = 1;
    zeroUploadDesc.MipLevels = 1;
    zeroUploadDesc.SampleDesc.Count = 1;
    zeroUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> reservoirZeroUpload;
    Check(device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &zeroUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&reservoirZeroUpload)));
    void* zeroData = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    Check(reservoirZeroUpload->Map(0, &noRead, &zeroData));
    std::memset(zeroData, 0, static_cast<std::size_t>(kReservoirZeroChunkBytes));
    D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(kReservoirZeroChunkBytes) };
    reservoirZeroUpload->Unmap(0, &writtenRange);

    for (UINT i = 0; i < 2; ++i)
    {
        if (reservoirStates[i] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            UavBarrier(reservoir[i].Get());
        Transition(reservoir[i].Get(), reservoirStates[i], D3D12_RESOURCE_STATE_COPY_DEST);
        reservoirStates[i] = D3D12_RESOURCE_STATE_COPY_DEST;
        for (UINT64 offset = 0; offset < reservoirBytes[i];)
        {
            UINT64 copyBytes = reservoirBytes[i] - offset;
            if (copyBytes > kReservoirZeroChunkBytes)
                copyBytes = kReservoirZeroChunkBytes;
            commandList->CopyBufferRegion(reservoir[i].Get(), offset, reservoirZeroUpload.Get(), 0, copyBytes);
            offset += copyBytes;
        }
        Transition(reservoir[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        reservoirStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    reservoirParity = 0;

    TransitionSnapshot(history, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(history.snapshot.Get(), images[1].texture.Get());
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    TransitionSnapshot(moments, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(moments.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(moments.snapshot.Get(), moments.texture.Get());
    Transition(moments.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    FlushCommandList();
    // The CPU descriptor heap and staging buffer are local to Clear; retain them
    // through GPU execution. This reset path already requires completion before
    // later passes consume the freshly initialized image and reservoir data.
    WaitForGpuIdle();
}

// Uploads the trained weights once. Called from Clear(), which is the first
// thing that opens a command list, because the copy itself is a command-list
// operation.
//
// The buffer was created in CreateImages() in COPY_DEST state; here it is filled
// from a staging upload buffer and promoted to NON_PIXEL_SHADER_RESOURCE, which
// is the state an SRV read requires. Leaving it in COPY_DEST is not a warning --
// it is a real hazard that only shows up as a debug-layer error or a wrong image
// on a driver that enforces it.
void DX12Nova::UploadNeuralWeights()
{
    if (neuralWeightsValid)
        return;
    if (!neuralWeights)
    {
        // No blob was present when CreateImages ran. Nothing to upload, and the
        // stage stays disabled -- the safe default. This is not retried per
        // frame; the blob's presence is a property of the run directory.
        WL_CORE_WARN("Neural stage (DX12): assets/neural/neural_weights.bin missing or "
                     "invalid -- the stage is disabled");
        return;
    }

    Neural::WeightBlob blob;
    if (!Neural::LoadWeightBlob("assets/neural/neural_weights.bin", blob))
        return;

    const UINT64 bytes = blob.payload.size();
    if (!bytes || bytes != neuralWeights->GetDesc().Width)
    {
        WL_CORE_ERROR("Neural stage (DX12): blob is {} bytes but the buffer is {} -- "
                      "the stage is disabled rather than truncated",
                      bytes, (UINT)neuralWeights->GetDesc().Width);
        neuralWeights.Reset();
        return;
    }

    // A DEFAULT-heap buffer cannot be memcpy'd into, so stage through an UPLOAD
    // buffer. The staging resource is a local Com<>, released at the end of this
    // function -- which is safe because FlushCommandList() and WaitForGpuIdle()
    // in Clear() retire the submission before the function's stack unwinds.
    Com<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES uploadProps = {};
    uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    HRESULT hr = device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bytes),
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
    Check(hr);

    void *mapped = nullptr;
    staging->Map(0, nullptr, &mapped);
    if (mapped)
    {
        memcpy(mapped, blob.payload.data(), blob.payload.size());
        staging->Unmap(0, nullptr);
    }

    commandList->CopyResource(neuralWeights.Get(), staging.Get());
    D3D12_RESOURCE_BARRIER promote = CD3DX12_RESOURCE_BARRIER::Transition(
        neuralWeights.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &promote);

    neuralWeightsValid = true;
    WL_CORE_INFO("Neural stage (DX12): loaded {} weights ({} bytes)",
                 blob.nWeights, blob.payload.size());
}

// Dispatches the trained end-of-frame neural stage: pass 17 reduces the frame
// into per-block partials, pass 19 folds them into the four frame means, pass 18
// evaluates the MLP. The same three passes the GL path runs, in the same order,
// from the same shader source.
//
// Pass 19 is dispatched with w = h = 8 rather than the frame size: the shader
// declares [numthreads(8,8,1)] and Dispatch derives its group count as
// ((w+7)/8, (h+7)/8), so (8,8) yields exactly one block -- which is what
// RunNeuralReduceFinal is written for. It derives its own block count from
// g_dims, so it does not care how many blocks the dispatch used; the dispatch
// size only has to be one. This mirrors the GL path's glDispatchCompute(1,1,1),
// and matters because the first version of the reduction read gl_NumWorkGroups
// here and summed one block's partials out of 129600.
//
// Each pass is its own submission (Dispatch calls BeginSubmit), so the ordering
// between 17, 19 and 18 is guaranteed by submission order rather than by a
// barrier inside one list. A UAV barrier is still recorded before each, because
// D3D12 does not order writes to the same UAV across dispatches without one.
void DX12Nova::DispatchNeuralStage(UINT w, UINT h)
{
    if (!neuralWeightsValid || !neuralGlobals)
        return;

    for (int pass = 17; pass <= 19; ++pass)
    {
        UavBarrier(neuralGlobals.Get());
        // Pass 19 is the single-block fold; 17 and 18 are per-pixel. BOTH
        // dimensions have to be 8, because Dispatch derives the group count as
        // ((w+7)/8, (h+7)/8) from each independently -- setting only w would give
        // 1 x (h+7)/8 blocks, which still computes the right answer (every block
        // would fold the same partials) but wastes the whole frame's worth of
        // threads to do it.
        const bool singleBlock = (pass == 19);
        Dispatch(pass, singleBlock ? 8u : w, singleBlock ? 8u : h, {});
    }
}

// A full-pipeline drain, used before destroying GPU resources. Safe to
// call when nothing has been submitted yet -- both early-outs below
// handle the uninitialised case.
// INFINITE rather than a timeout: a bounded wait here would let the
// caller proceed to free resources the GPU is still reading, which is
// the exact bug the drain exists to prevent.
void DX12Nova::WaitForGpuIdle()
{
    if (!fence) return;
    UINT64 target = 0;
    for (UINT i = 0; i < kFramesInFlight; i++)
        // The wait target is the HIGHEST submitted value across all slots,
        // not the current slot's. Fence values are monotonic, so waiting for
        // the maximum retires every earlier submission too.
        if (frames[i].submittedValue > target)
            target = frames[i].submittedValue;
    // Value 0 is the fence's initial state and means "nothing was ever
    // signalled", so it is also the "nothing in flight" sentinel.
    if (target == 0) return;
    // Nothing beyond 'target' has been queued, so waiting for it retires every
    // in-flight slot regardless of which one is current.
    // Already-complete check before arming the event, so the common case
    // -- the fence has already passed this value -- does not block at all.
    if (fence->GetCompletedValue() >= target) return;
    HRESULT hr = fence->SetEventOnCompletion(target, fenceEvent);
    Check(hr);
    WaitForSingleObject(fenceEvent, INFINITE);
}

// The ring advance. Everything else in this file that records
// commands goes through here, so the "is this slot free" check is made
// in exactly one place.
// The slot-free proof. A slot with submittedValue == 0 has never been
// used, so there is nothing to wait for; otherwise the previous
// submission must have retired before its allocator and list are
// touched.
void DX12Nova::BeginSubmit()
{
    // Advance to the next slot and prove it is free. A slot's previous
    // submission must have retired before its allocator and command list are
    // touched -- this is the wait the old code never performed.
    currentFrame = (currentFrame + 1) % kFramesInFlight;
    FrameContext &slot = frames[currentFrame];

    if (slot.submittedValue != 0 && fence->GetCompletedValue() < slot.submittedValue)
    {
        HRESULT hr = fence->SetEventOnCompletion(slot.submittedValue, fenceEvent);
        Check(hr);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    // Resetting an allocator that the GPU is still executing is the
    // classic ring-buffer bug: the command list's commands become
    // undefined. The wait above is what makes this safe.
    HRESULT hr = slot.allocator->Reset();
    Check(hr);
    // The PSO is passed to Reset because it re-establishes it as the
    // command list's current pipeline state. Recording could otherwise
    // assume it and a state-tracking-aware backend would be a fair way to
    // find this missing.
    hr = slot.commandList->Reset(slot.allocator.Get(), pipelineState.Get());
    Check(hr);

    // The member aliases, so the rest of this file can record through
    // `commandList` without threading the slot through every call. Both
    // point into frames[]; nothing else may reassign them.
    commandAllocator = slot.allocator;
    commandList = slot.commandList;
}

// A single-transition barrier helper. D3D12 has no implicit
// resource state tracking: every state change must be recorded as a
// barrier, and the caller must know the exact before-state.
void DX12Nova::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    // ALL_SUBRESOURCES because none of these resources are mipped or
    // arrayed -- all are created with 1 mip and 1 slice. A per-subresource
    // transition would be needed only for a partially-state resource.
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

// Snapshot resources are recreated in COPY_DEST and then move between copy
// destination and shader-readable states. Track the actual state per resource
// so first use does not claim a false before-state.
void DX12Nova::TransitionSnapshot(Image& image, D3D12_RESOURCE_STATES after)
{
    if (!image.snapshot || image.snapshotState == after)
        return;
    Transition(image.snapshot.Get(), image.snapshotState, after);
    image.snapshotState = after;
}

// Orders UAV accesses to one resource without changing its state. Pass 11
// reads and rewrites the buffer pass 3 just generated into; Clear also brackets
// its unordered UAV clear commands with this barrier.
void DX12Nova::UavBarrier(ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = res;
    commandList->ResourceBarrier(1, &barrier);
}

// Close, submit, and signal. These three steps are the atomic unit of
// a submission in this file; nothing else calls ExecuteCommandLists.
void DX12Nova::FlushCommandList()
{
    HRESULT hr = commandList->Close();
    Check(hr);
    ID3D12CommandList* lists[] = { commandList.Get() };
    // A one-element array for the same reason as the heap binding above.
    commandQueue->ExecuteCommandLists(1, lists);

    // Signal the fence for THIS slot and remember the value, so BeginSubmit
    // can prove later that the slot has retired before recycling it. The old
    // code created a fence and never signalled it, then reset the allocator
    // immediately -- the GPU could still be executing the list being
    // invalidated.
    // The fence value is incremented and stored on the SLOT before being
    // signalled. Storing it first is what lets BeginSubmit and Output wait
    // for this specific submission later.
    frames[currentFrame].submittedValue = ++fenceValue;
    // Signal is queued on the command queue AFTER the command lists, so
    // the fence reaches this value only once the work it covers has
    // finished. Signalling before the submission would be a no-op wait.
    hr = commandQueue->Signal(fence.Get(), fenceValue);
    Check(hr);
}

// Grows the constant buffer on demand. Called when the shader
// reflection reports a larger NovaConstants than the current buffer
// holds.
void DX12Nova::EnsureConstantBufferSize(UINT size)
{
    // Only ever grows. Shrinking would be a realloc with no benefit, and
    // a constant buffer that got smaller means the reflection changed in a
    // way this does not expect.
    if (size <= constantBufferSize) return;
    // Destroying the buffer the GPU is reading out from under in-flight
    // submissions is the same class of bug as the allocator reset this file
    // previously had: wait for everything submitted to retire first.
    WaitForGpuIdle();
    for (UINT i = 0; i < kFramesInFlight; i++) {
        if (constantBufferMapped[i] && constantBuffers[i]) {
            constantBuffers[i]->Unmap(0, nullptr);
            constantBufferMapped[i] = nullptr;
        }
        // Same reasoning as the drain in CreateImages: the buffer about to be
        // released may still be bound as the CBV by an in-flight command list.
        constantBuffers[i].Reset();
    }
    CreateConstantBuffer(size);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    // Every slot's heap carries its own CBV, and each now names its OWN buffer
    // rather than a shared one -- so each needs a different BufferLocation and
    // cannot be written with a single descriptor reused across the loop.
    for (UINT i = 0; i < kFramesInFlight; i++) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC slotCbv = cbvDesc;
        slotCbv.BufferLocation = constantBuffers[i]->GetGPUVirtualAddress();
        slotCbv.SizeInBytes = constantBufferSize;
        device->CreateConstantBufferView(&slotCbv, frames[i].heapCpuStart);
    }
}

template<class T>
// Templated on the value type so the caller writes Set("u_CameraPos",
// someGlmVec3) and the correct byte count is memcpy'd. The explicit
// instantiations at the bottom of this file are what make the template
// usable from Renderer3D.cpp.
void DX12Nova::Set(const char* name, const T& value, UINT index)
{
    // A name lookup in the reflection map. A name the shader does not declare
    // used to be SILENTLY DROPPED, which is how a whole class of Nova bug
    // reaches shipping: a name typo, or a constant removed from the shader
    // while the call stayed, leaves the shader reading whatever stale value is
    // in the staging buffer -- typically zero from the last resize -- with no
    // diagnostic at all. It produces a silently wrong render rather than a
    // failure.
    //
    // Warned rather than thrown. Dead uniforms are a legitimate call pattern
    // here: u_MaxBounces and u_SamplesPerPixel are uploaded to hold cbuffer
    // layout while the shader ignores them, and the calls must keep compiling.
    // Throwing would turn a cosmetic no-op into a startup crash. Once per name
    // because Set is called every frame, so an unconditional warn would flood
    // the log at 60 lines a second.
    auto it = variables.find(name);
    if (it == variables.end()) {
        static std::unordered_set<std::string> warned;
        if (warned.insert(name).second)
            WL_CORE_WARN("Nova constant '{0}' is not declared by the shader -- the upload is "
                         "dropped and the shader keeps its stale value (typo, or the uniform "
                         "was removed from the shader while the call stayed)", name);
        return;
    }
    // Arrays are addressed by `index * 16`: a hardcoded 16-byte stride,
    // which is D3D12's cbuffer array element alignment. A caller writing
    // an array element must pass the element index, and the stride is not
    // sizeof(T) -- a vec3 array still strides by 16.
    UINT offset = it->second.StartOffset + (index * 16);
    // The bounds check throws rather than truncating. It is a `>` and not
    // a `>=` because writing exactly up to the end of the variable is
    // legal: offset + sizeof(T) == end is in bounds.
    if (offset + sizeof(T) > it->second.StartOffset + it->second.Size)
        throw std::runtime_error("Nova constant size mismatch");
    memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Uploads one of four structured buffers (instances, BVH nodes, light indices,
// or terrain vertices). Replaces the buffer outright rather than updating it in
// place, which is why a full drain is needed.
void DX12Nova::Upload(UINT slot, const void* data, UINT count, UINT stride)
{
    // Hardcoded 4, matching the structuredBuffers array's four shader ABI
    // slots. Keep this guard in sync with that array so an invalid slot cannot
    // reach its unchecked index below.
    if (slot >= 4) return;
    // structuredBuffers[slot].Reset() below releases a buffer that every
    // in-flight submission still has bound as a root SRV. Draining first is the
    // difference between "the GPU finished reading it" and "the driver will
    // eventually unmap memory the GPU is walking."
    WaitForGpuIdle();
    structuredBuffers[slot].Reset();

    // A zero count still allocates one stride, because a zero-width
    // buffer is invalid. The upload below then memsets that single element,
    // so a caller with no instances gets a valid, defined buffer.
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
    // The empty-data case memsets rather than skipping, so the buffer has
    // defined contents even when there is nothing to upload. This is the
    // companion to the minimum-size allocation above.
    if (data && count > 0) {
        memcpy(mapped, data, count * stride);
    } else {
        memset(mapped, 0, stride);
    }
    // Unmapped immediately: unlike the constant buffer, these are written
    // once per upload and read by the GPU, so there is no reason to keep
    // them mapped.
    structuredBuffers[slot]->Unmap(0, nullptr);
}

// Empty by design. On the DX11 path this releases the bound SRVs;
// there are no bound views to release on D3D12, so the body is empty
// rather than deleted -- the interface and the call sites in Resize,
// Clear and Output are shared across backends.
void DX12Nova::Unbind()
{
}

// The main per-pass entry point: record one compute dispatch into a
// fresh frame slot. Everything between BeginSubmit and FlushCommandList
// is one command list.
void DX12Nova::Dispatch(int pass, UINT w, UINT h, const std::vector<ID3D12Resource*>& textures)
{
    // Adopt the next frame slot. This is where the old inline
    // allocator->Reset()/commandList->Reset() used to be -- and where the wait
    // Adopting a slot here rather than inline resetting the allocator is
    // the fix for the missing-wait bug: the wait the old code lacked is now
    // inside BeginSubmit, so every path that records gets it.
    // that was missing now lives.
    BeginSubmit();

    // t10 is the previous-frame read buffer and u10 is the current-frame write
    // buffer. Track their actual states because SwapReservoir reverses these
    // roles: transition both at the start of a dispatch, then keep the read side
    // in NON_PIXEL_SHADER_RESOURCE and the write side in UAV through the pass.
    const UINT reservoirReadIndex = reservoirParity;
    const UINT reservoirWriteIndex = 1u - reservoirParity;
    if (reservoirStates[reservoirReadIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Transition(reservoir[reservoirReadIndex].Get(), reservoirStates[reservoirReadIndex],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        reservoirStates[reservoirReadIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (reservoirStates[reservoirWriteIndex] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        Transition(reservoir[reservoirWriteIndex].Get(), reservoirStates[reservoirWriteIndex],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        reservoirStates[reservoirWriteIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    // Pass 11 consumes pass 3's UAV writes from the same current-frame target.
    // The explicit UAV barrier is required even though both passes are queued
    // in order and the resource remains in UAV state.
    if (pass == 11)
        UavBarrier(reservoir[reservoirWriteIndex].Get());

    // Upload u_PassID. WAS MISSING ENTIRELY: DX11Nova::Dispatch does this
    // (DX11Nova.h:413) and the Nova shader switches on u_PassID to pick its
    // pass, but the DX12 body never referenced the `pass` argument. Every
    // dispatch therefore ran with u_PassID == 0, so the whole schedule
    // (visibility, trace, composite, bloom, god rays) collapsed to whichever
    // branch handles pass 0 -- which is why DX12 rendered nothing useful
    // while DX11 was correct. Set it here so the argument is finally used.
    // u_PassID is what the shader switches on to select its pass. The
    // comment above records that this was previously never set, which made
    // every dispatch run pass 0 -- the single highest-value line in this
    // file, since it is what makes the `pass` argument do anything.
    Set("u_PassID", pass);

    commandList->SetComputeRootSignature(rootSignature.Get());

    FrameContext &slot = frames[currentFrame];
    ID3D12DescriptorHeap* heaps[] = { slot.descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    // THIS slot's constant buffer, not a shared one. Paired with the memcpy
    // below, which writes the same slot: the bytes a command list reads are the
    // bytes written for the slot that recorded it.
    commandList->SetComputeRootConstantBufferView(0, constantBuffers[currentFrame]->GetGPUVirtualAddress());
    // The three scene-buffer root SRVs are bound by GPU VIRTUAL ADDRESS rather
    // than by descriptor, because they are root parameters 1..3 (non-table).
    // The terrain-buffer SRV is bound separately at root parameter 9 below.
    commandList->SetComputeRootShaderResourceView(1, structuredBuffers[0]->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(2, structuredBuffers[1]->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(3, structuredBuffers[2]->GetGPUVirtualAddress());
    // DX12 terrain vertex rows at t4, uploaded by the producer in slot 3.
    commandList->SetComputeRootShaderResourceView(9, structuredBuffers[3]->GetGPUVirtualAddress());
    // ReSTIR reservoir read SRV at root param 8 (t10). The ping-pong parity
    // selects the read buffer for this phase; swap after pass 11 to expose its
    // result to pass 14, then again after pass 14 for next-frame history.
    commandList->SetComputeRootShaderResourceView(8, reservoir[reservoirReadIndex]->GetGPUVirtualAddress());

    // Rebuild the u10 UAV descriptor against the LIVE write target. This must
    // happen per-dispatch, not once at creation: after SwapReservoir the write
    // target changes. The state transitions above ensure this view names a UAV-
    // state resource while t10 names the other buffer in SRV state.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavSlot = slot.heapCpuStart;
        uavSlot.ptr += (kUavBase + 10) * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(reservoir[reservoirWriteIndex].Get(), nullptr,
                                          &reservoirUavDesc, uavSlot);
    }

    // All descriptor-table base addresses come from THIS slot's heap. They are
    // laid out identically in every slot, so the same offsets apply -- but they
    // must come from the slot being recorded, not a shared heap another
    // in-flight submission is reading.
    // The CPU handle base is cached here for the three descriptor-writing
    // loops below. Each loop derives its own offset from it into THIS
    // slot's heap.
    auto cpuHandle = slot.heapCpuStart;

    // The staging bytes are uploaded ONCE here, after all the root
    // bindings are recorded. It is a memcpy into persistently-mapped
    // memory, so the GPU sees the new values when it eventually executes
    // this list.
    // Into the CURRENT slot's mapping. Writing a single shared buffer here is
    // what the per-slot ring cannot protect against: BeginSubmit has already
    // advanced currentFrame by the time this runs, so the destination must be
    // the same slot whose command list will read it.
    memcpy(constantBufferMapped[currentFrame], bytes.data(), bytes.size());

    // Descriptor writes below go into slot's heap only. Safe now that this
    // slot's previous submission has retired (BeginSubmit waited for it) and no
    // other in-flight slot shares these descriptors.
    // Scene-texture descriptors, capped at 32 -- which matches the range
    // declared in the root signature and the 32 entries reserved between
    // kSceneTextureBase and kSamplerBase. The `i < 32` cap is what stops
    // a longer textures vector from overrunning into the sampler table.
    // A null entry is skipped rather than creating a descriptor for it, so
    // the shader reads whatever the slot previously held.
    for (size_t i = 0; i < textures.size() && i < 32; i++) {
        if (!textures[i]) continue;
        // Format from the resource. A zero Format makes D3D12 infer
        // R32G32B32A32_FLOAT + UNORM, which mismatches the R8G8B8A8_UNORM
        // textures the engine loads (including its 1x1 white fallback).
        // A format that does not match the view's resource is undefined.
        D3D12_RESOURCE_DESC texDesc = textures[i]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (kSceneTextureBase + (UINT)i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(textures[i], &srvDesc, dest);
    }

    const D3D12_RESOURCE_STATES snapshotReadState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The snapshot may still be COPY_DEST on first use, or shader-readable
    // after a prior dispatch/Output. Track its actual state, copy into it, then
    // transition it for compute SRV reads and ImGui's pixel-shader read.
    for (int i = 0; i < 8; i++) {
        TransitionSnapshot(images[i], D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(images[i].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(images[i].snapshot.Get(), images[i].texture.Get());
        Transition(images[i].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionSnapshot(images[i], snapshotReadState);
    }

    // The moments snapshot is refreshed HERE, at the top of every dispatch, and
    // not in CommitHistory. That placement is load-bearing and non-obvious:
    //
    //   - pass 12 samples t53 (the moments snapshot) to blend against the
    //     PREVIOUS frame, then writes the live moments texture.
    //   - pass 13 reads t73 (the same snapshot) and must see what pass 12 JUST
    //     wrote, not the previous frame.
    //
    // So the snapshot has to be re-copied between those two dispatches. Putting
    // the copy in CommitHistory does not work: CommitHistory runs once per frame,
    // BEFORE pass 12 (see RunNovaPassSchedule), so it would refresh the snapshot
    // to the previous frame's value and pass 13 would read stale moments. Doing it
    // per-dispatch gives each pass exactly the "what the last pass left" view the
    // DX11 backend gets from Unbind(), which refreshes every snapshot the same way.
    //
    // Dispatch returns the moments snapshot to COPY_DEST after sampling it.
    // The tracked transition below enforces the required copy destination state
    // and is a no-op in the current schedule.
    TransitionSnapshot(moments, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(moments.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(moments.snapshot.Get(), moments.texture.Get());
    Transition(moments.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    for (int i = 0; i < 8; i++) {
        // One SRV per image, for the shader's own reads of previous-pass
        // outputs. These occupy kImageSrvBase..kImageSrvBase+7.
        // Format must come from the resource. A zero Format makes D3D12 infer
        // R32G32B32A32_FLOAT and UNORM, which does not match several of these
        // images (R32G32_FLOAT, R8G8B8A8_UNORM).
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = images[i].snapshot->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = images[i].snapshot->GetDesc().MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (kImageSrvBase + i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(images[i].snapshot.Get(), &srvDesc, dest);
    }

    // The five sampled snapshots, and the indirection is the point: the
    // sampler table maps five shader-visible slots onto an ARBITRARY set
    // of sources, in this order. Index 6 appears twice, so two samplers read
    // the same image under different register numbers.
    //
    // Mirrors DX11Nova.h:456 (srvs[48..51]). t48 is the dedicated history
    // image, NOT images[1]: the shader reads t48 (s_Accumulation) as the
    // TEMPORAL HISTORY --
    //     float3 historyLight = TEX(s_Accumulation, histUV).rgb;
    // at a REPROJECTED uv, so it must be LAST frame's committed accumulation.
    // CommitHistory() is what puts it there, and DX11 binds t48 to exactly
    // that texture (history.srv, an SRV over history.snapshot) for the same
    // reason.
    //
    // It cannot be images[1].texture: that is the img_Accumulation UAV (u1)
    // in this same dispatch, and sampling a resource the dispatch writes is
    // undefined. images[1].snapshot would be legal but is a different
    // contract -- it holds whatever this frame's earlier passes left there,
    // not last frame's result.
    //
    // HISTORY (fixed 2026-10-05; do not "simplify" this back). Before the
    // sentinel below, sampIndices was {1,6,0,6,4}, which bound t48 to
    // images[1].snapshot -- so the accumulation pass reprojected against the
    // CURRENT frame's own output, and history.snapshot was bound to no slot
    // at all. kHistorySlot is a SENTINEL, not an images[] index: it is resolved
    // to history.snapshot when the descriptor is built below, which is why the
    // -1 can never index images[].
    const int kHistorySlot = -1;
    // kMomentsSlot is the second sentinel, for the same reason: t53 is
    // s_Moments, the previous frame's moments, which is not one of the eight
    // pass images. -2 can never collide with kHistorySlot (-1) or an images[]
    // index (0..7).
    const int kMomentsSlot = -2;
    int sampIndices[] = { kHistorySlot, 6, 0, 6, 4, kMomentsSlot };
    // Pass 9 is the material-map generator. DX11 binds textures[0] to t52 for
    // that pass only (DX11Nova.h:447); DX12 had no pass handling at all, so
    // GenerateMaterialMaps read an unrelated internal image as its input.
    // t52 is one past the five entries above, hence the 6 - index.
    // t48..t52 is FIVE registers (48,49,50,51,52), so t52 is kSamplerBase+4 --
    // the last entry of the existing range, matching DX11Nova.h:447. An earlier
    // attempt used kSamplerBase+5, which is kImageSrvBase and would have
    // overwritten the first internal-image SRV.
    const UINT kMaterialMapInputSlot = kSamplerBase + 4;
    if (pass == 9 && !textures.empty() && textures[0])
    {
        D3D12_RESOURCE_DESC inDesc = textures[0]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC inDesc2 = {};
        inDesc2.Format = inDesc.Format;
        inDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        inDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        inDesc2.Texture2D.MipLevels = inDesc.MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += kMaterialMapInputSlot * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(textures[0], &inDesc2, dest);
    }
    // Compute reads require NON_PIXEL_SHADER_RESOURCE. Use a combined shader
    // read state so image snapshots also remain valid for ImGui pixel reads;
    // history and moments return to COPY_DEST after dispatch for CopyResource.
    TransitionSnapshot(history, snapshotReadState);
    // Moments are sampled through t53/t73 by compute passes as well.
    TransitionSnapshot(moments, snapshotReadState);
    for (int i = 0; i < 6; i++) {
        // The descriptor is built from the RESOURCE's own descriptor rather
        // than from the formats[] array, so it cannot disagree with what was
        // actually created. The source is resolved THROUGH sampIndices, and
        // kHistorySlot / kMomentsSlot are the entries that name a resource
        // outside images[] -- so the format is read from whichever resource is
        // actually bound, not from images[i].
        ID3D12Resource *source = sampIndices[i] == kHistorySlot
            ? history.snapshot.Get()
            : (sampIndices[i] == kMomentsSlot
                ? moments.snapshot.Get()
                : images[sampIndices[i]].snapshot.Get());
        D3D12_RESOURCE_DESC snapDesc = source->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = snapDesc.Format; // from the resource; see the kImageSrvBase loop
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = snapDesc.MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (kSamplerBase + i) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(source, &srvDesc, dest);
    }

    // t73, read_img_Moments: the IMG_LOAD twin pass 13 reads the moments through.
    // It lives in the readable-snapshot block (kImageSrvBase) because that block's
    // range is what covers t64..t73. Its slot is kImageSrvBase + 9 -- t73 minus the
    // block's base register of 64. t72 at +8 is unused by the shader, but still
    // needs a valid descriptor because the whole static range must be initialized.
    {
        D3D12_RESOURCE_DESC momDesc = moments.snapshot->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = momDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = momDesc.MipLevels;
        D3D12_CPU_DESCRIPTOR_HANDLE paddingDest = cpuHandle;
        paddingDest.ptr += (kImageSrvBase + 8) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(moments.snapshot.Get(), &srvDesc, paddingDest);

        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += (kImageSrvBase + 9) * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(moments.snapshot.Get(), &srvDesc, dest);
    }

    // ==========================================================================
    // Neural stage descriptors
    // ==========================================================================
    // u12 is NeuralGlobals. The slot at kUavBase + 12 is the last entry of the
    // UAV table, which is why kUavCount is 13: the table is contiguous from u0,
    // so u12 cannot be addressed without sizing the range to cover it.
    //
    // A buffer UAV needs a StructuredBuffer-style view: Format UNKNOWN plus a
    // StructureByteStride. A typed view here would be a format/stride mismatch
    // and the debug layer would reject it.
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ngUavDesc = {};
        ngUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ngUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        // g_Mean[4] + g_Partial[MAX*4] + g_GateMean + g_GatePartial[MAX], in
        // uints -- the same element count the GL SSBO is sized with
        // (Neural::kGlobalsBytes). A UAV view that is smaller than the buffer
        // would let the shader write past the view's end, which the D3D12 debug
        // layer flags as UAV-0 and which silently corrupts the gate partials on
        // a release build.
        ngUavDesc.Buffer.NumElements = Neural::kGlobalsUints;
        ngUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += neuralGlobalsUavSlot * cbvSrvUavDescriptorSize;
        device->CreateUnorderedAccessView(neuralGlobals.Get(), nullptr, &ngUavDesc, dest);
    }

    // t74 is NeuralWeights, the last entry of the readable-snapshot block. Same
    // StructuredBuffer shape as above, as an SRV.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nwSrvDesc = {};
        nwSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nwSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        nwSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nwSrvDesc.Buffer.NumElements = Neural::kWeightCount;
        nwSrvDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE dest = cpuHandle;
        dest.ptr += neuralWeightsSrvSlot * cbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(neuralWeights.Get(), &nwSrvDesc, dest);
    }

    // These root-signature ranges use static descriptors. Finish all writes to
    // their heap slots before binding the tables; binding first is invalid even
    // when the writes happen before Dispatch is recorded.
    D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart = slot.descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = heapGpuStart;
    srvHandle.ptr += kSceneTextureBase * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(4, srvHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE sampHandle = heapGpuStart;
    sampHandle.ptr += kSamplerBase * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(5, sampHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE imgHandle = heapGpuStart;
    imgHandle.ptr += kImageSrvBase * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(6, imgHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = heapGpuStart;
    uavHandle.ptr += kUavBase * cbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(7, uavHandle);

    // 8x8 thread groups, computed from the requested size. The shader
    // declares [numthreads(8,8,1)], so the group count must be ceil(w/8) by
    // ceil(h/8); a group count computed by dividing would drop the
    // remainder pixels at the right and bottom edges.
    commandList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    // Image snapshots remain shader-readable for Output()/ImGui. History and
    // moments return to COPY_DEST for their next CopyResource refresh.
    TransitionSnapshot(history, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionSnapshot(moments, D3D12_RESOURCE_STATE_COPY_DEST);
    FlushCommandList();
}

// Commits the accumulation image into the history snapshot, as its
// own submission. Own submission because it must retire before the
// next frame's accumulation reads the history.
// Its own BeginSubmit. Recording into the list the previous Dispatch
// left open would be recording into a closed or in-flight list -- the
// bug the comment above describes.
void DX12Nova::CommitHistory()
{
    if (!width) return;
    // These three used to record into whatever command list the previous
    // Dispatch() left open -- so a Commit could be appended to a slot already
    // submitted and closed (recording into a closed list is an error), or to
    // one the GPU was still executing. Each is its own submission, so each
    // adopts its own slot.
    BeginSubmit();
    TransitionSnapshot(history, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(history.snapshot.Get(), images[1].texture.Get());
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The moments snapshot is NOT copied here. It is refreshed per-dispatch in
    // Dispatch(), because pass 13 must see what pass 12 just wrote, not the
    // previous frame's value. See the comment at the moments copy in Dispatch.
    FlushCommandList();
}

// The denoised result is copied back into images[1], the
// accumulation image. Copying denoise output into the accumulation
// target is what lets the next frame accumulate denoised samples
// rather than raw ones.
void DX12Nova::CommitDenoise()
{
    if (!width) return;
    BeginSubmit();
    // BOTH endpoints of a CopyResource need COPY_SOURCE / COPY_DEST. Only the
    // source was bracketed before, leaving images[1] in UNORDERED_ACCESS as the
    // copy destination -- which is undefined, and it is the denoised-accumulation
    // path, so it is reachable whenever bilateral blur is on (the High and Ultra
    // presets set it). images[1] returns to UNORDERED_ACCESS afterwards because
    // the compute passes read and write it as img_Accumulation.
    Transition(images[7].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(images[1].texture.Get(), images[7].texture.Get());
    Transition(images[7].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(images[1].texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushCommandList();
}

// Flip the parity at a ReSTIR phase boundary; this is bookkeeping only and
// records no GPU work. The first flip follows pass 11 so pass 14 reads its
// temporal output and writes the old-history buffer. The second follows pass 14
// so the next frame reads the spatial result. Calling before pass 11 would make
// temporal reuse read the buffer pass 3 did not write.
void DX12Nova::SwapReservoir()
{
    if (!reservoir[0] || !reservoir[1]) return;
    reservoirParity ^= 1u;
}

// Produces the texture the renderer will show. Records its own
// submission and then WAITS for it, because the caller binds the
// returned pointer immediately.
void* DX12Nova::Output(UINT index)
{
    if (!width) return nullptr;
    Unbind();
    BeginSubmit();
    const D3D12_RESOURCE_STATES snapshotReadState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Track the true before-state on first use (COPY_DEST) and later uses
    // (combined shader-readable). The returned resource is valid for both
    // ImGui's pixel shader and Nova's compute SRVs.
    TransitionSnapshot(images[index], D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyResource(images[index].snapshot.Get(), images[index].texture.Get());
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionSnapshot(images[index], snapshotReadState);
    FlushCommandList();

    // The caller binds this snapshot as a texture immediately. The GPU copy
    // that fills it may not have retired yet, so wait for exactly this
    // submission before handing the pointer out -- otherwise the consumer
    // samples a snapshot the copy is still writing.
    FrameContext &slot = frames[currentFrame];
    if (slot.submittedValue != 0 && fence->GetCompletedValue() < slot.submittedValue)
    {
        HRESULT hr = fence->SetEventOnCompletion(slot.submittedValue, fenceEvent);
        Check(hr);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    // A targeted wait on THIS slot's submission -- not a full drain. The
    // other slots' work is irrelevant to whether this snapshot is filled,
    // and waiting on all of them would stall for no reason.
    // Returns the raw ID3D12Resource*, NOT something ImGui can bind directly.
    //
    // ImGui's DX12 backend does
    //     texture_handle.ptr = (UINT64)pcmd->GetTexID();
    //     ctx->SetGraphicsRootDescriptorTable(1, texture_handle);
    // so TexID must be a GPU DESCRIPTOR HANDLE VALUE. This returns a resource
    // pointer on purpose: converting it needs the NVRHIContext that owns the
    // ImGui SRV heap, which this class does not have. The conversion happens in
    // Renderer3D::GetRayTraceTargetNativeSRV.
    return images[index].snapshot.Get();
}

// Creates the staging texture a CPU readback lands in, or leaves the existing
// one alone when it already matches. See the readbackStaging members in the
// header for why this is cached rather than created per call.
void DX12Nova::EnsureReadbackStaging(const D3D12_RESOURCE_DESC& source)
{
    // The match test includes the FORMAT, not just the dimensions. A staging
    // texture whose dimensions agree with the source but whose format does not
    // makes CopyTextureRegion undefined behaviour, and a format change without a
    // size change is exactly what a render-scale or format switch produces.
    if (readbackStaging && readbackWidth == source.Width && readbackHeight == source.Height &&
        readbackFormat == source.Format)
        return;

    // Releasing the old staging texture while a previous readback's copy is still
    // executing would hand the driver back memory it is writing. Readback already
    // waits for its own fence before returning, so in practice nothing is in
    // flight here -- but this is the same destroy-behind-the-GPU rule CreateImages
    // and Upload follow, and being wrong about it is a device-removed bug rather
    // than a slow one.
    WaitForGpuIdle();
    readbackStaging.Reset();
    // Explicit narrowing from the descriptor's UINT64. A texture dimension beyond
    // UINT is not representable as a 2D image on any shipping adapter, and the
    // cast makes the lossy step a decision here rather than four implicit
    // warnings the build log then trains everyone to ignore.
    readbackWidth = (UINT)source.Width;
    readbackHeight = (UINT)source.Height;
    readbackFormat = source.Format;
    readbackRowPitch = 0;
    readbackTotalBytes = 0;

    // READBACK heap, and NO resource flags.
    //
    // This is the D3D12 spelling of "usage staging + CPU access read": a texture
    // with no ALLOW_UNORDERED_ACCESS, no RENDER_TARGET and no DEPTH_STENCIL on the
    // READBACK heap is what D3D12 requires in order to be mappable for reading. It
    // cannot be rendered to, sampled by a shader, or written by anything except a
    // copy, which is exactly the guarantee wanted from a readback landing zone.
    //
    // A BUFFER, not a TEXTURE2D, and this is a hard D3D12 rule rather than a
    // preference: the debug layer rejects a texture created on a READBACK or
    // UPLOAD heap outright ("A texture resource cannot be created on a
    // D3D12_HEAP_TYPE_UPLOAD or D3D12_HEAP_TYPE_READBACK heap"). A readback
    // landing zone therefore has to be a buffer, and a buffer means the row
    // pitch is no longer something the copy can hide -- GetCopyableFootprints
    // reports it, and Readback walks rows by it.
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    // The copy destination is a PLACED_FOOTPRINT built from this footprint, so
    // the size it reports is the size the buffer has to be. Asked of the SOURCE
    // descriptor: the destination is required to agree with the source on
    // dimensions and format, so the source's footprint is the destination's.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0, totalBytes = 0;
    device->GetCopyableFootprints(&source, 0, 1, 0, &layout, &numRows,
        &rowSizeInBytes, &totalBytes);
    if (!numRows || numRows != source.Height || !totalBytes)
    {
        WL_CORE_ERROR("DX12 Nova readback: footprint query failed for {0}x{1} (rows={2}, bytes={3})",
            (int)source.Width, (int)source.Height, (int)numRows, (unsigned long long)totalBytes);
        readbackStaging.Reset();
        readbackWidth = readbackHeight = 0;
        readbackFormat = DXGI_FORMAT_UNKNOWN;
        readbackRowPitch = 0;
        readbackTotalBytes = 0;
        return;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = totalBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Alignment = 0;
    // Born in COPY_DEST and kept there, matching NVRHIFramebuffer's staging
    // resource, which has the same lifetime and the same "only a copy ever writes
    // me" property. A staging resource cannot be transitioned out of COPY_DEST
    // and back, and does not need to be -- which is exactly why the copy in
    // Readback brackets only the SOURCE and leaves this side alone.
    WL_CORE_INFO("DX12 Nova readback staging buffer: {0}x{1} fmt={2} rowPitch={3} total={4}",
        (int)source.Width, (int)source.Height, (int)source.Format,
        (unsigned long long)layout.Footprint.RowPitch, (unsigned long long)totalBytes);
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackStaging));
    // Reported rather than Check()'d: this function runs from the editor's
    // screenshot hook, and Check throws. The contract on that path is that a
    // failed capture shows up in the log instead of taking the editor down
    // mid-frame, so every failure below returns false instead of throwing.
    if (FAILED(hr)) {
        WL_CORE_ERROR("DX12 Nova readback: staging buffer creation failed (0x{0:x}) for {1}x{2}",
            (unsigned long)hr, (int)source.Width, (int)source.Height);
        readbackStaging.Reset();
        readbackWidth = readbackHeight = 0;
        readbackFormat = DXGI_FORMAT_UNKNOWN;
        readbackRowPitch = 0;
        readbackTotalBytes = 0;
        return;
    }

    readbackRowPitch = layout.Footprint.RowPitch;
    readbackTotalBytes = totalBytes;
}

// GPU -> CPU capture of one render image.
//
// This is the DX12 counterpart of the OpenGL glReadPixels call in
// EditorLayer::CaptureFramebufferPNG, and it exists for the same reason: a clean
// log proves nothing about pixels (a deliberately broken shader logs no error at
// all), so the only way to know the viewport rendered is to look at an image.
//
// The whole method is one extra submission plus one fence wait. That is inherent
// to a CPU readback -- the copy cannot be read until the GPU has finished it --
// and it is why this is a capture path rather than something called per frame.
bool DX12Nova::Readback(UINT index, std::vector<unsigned char>& rgba, UINT& outWidth, UINT& outHeight)
{
    // The same guards every other method here carries, and for the same reason:
    // before the first Resize the images do not exist, and an out-of-range index
    // is a caller bug rather than something to clamp.
    if (!width || !height || !device || index >= images.size()) return false;
    if (!images[index].texture) return false;

    D3D12_RESOURCE_DESC sourceDesc = images[index].texture->GetDesc();
    // Only two formats have an unambiguous 8-bit-RGBA reading: the float
    // pass images, whose channels are converted below, and the RGBA8 image,
    // whose bytes are already what the caller wants. The R32G32_FLOAT image has
    // no alpha and no blue, so reading it as colour would invent both -- it is
    // rejected rather than guessed at.
    const bool isFloat = sourceDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    const bool isUnorm8 = sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!isFloat && !isUnorm8) {
        WL_CORE_WARN("DX12 Nova readback: image {0} has format {1}, which has no 8-bit RGBA reading",
            (int)index, (int)sourceDesc.Format);
        return false;
    }

    EnsureReadbackStaging(sourceDesc);

    // Guard the whole recording+readback body. The frame ring, the fence and the
    // staging map are all stateful, and a throw from any of them (a failed
    // HRESULT on a fence or a copy) would otherwise unwind straight out of the
    // editor's screenshot hook and terminate the process. The DX11/DX12 flush
    // paths wrap their Nova calls for exactly this reason.
    struct ReadbackGuard {
        DX12Nova *self;
        bool ok = false;
        ~ReadbackGuard() {
            if (!ok) {
                WL_CORE_ERROR("DX12 Nova readback: aborted, leaving the image and staging texture consistent");
            }
        }
    } guard{ this };

    // A submission of its own, through the ring, like every other recorded unit
    // of work in this file. Recording into whatever list the last Dispatch left
    // open would be recording into a closed or in-flight list.
    BeginSubmit();

    // The source image is in UNORDERED_ACCESS between frames: every pass that
    // reads or copies it transitions it there and back, and Output() leaves it
    // there. So COPY_SOURCE is the correct "before" state on entry, and the
    // copy is bracketed so the image is back in UNORDERED_ACCESS for whatever
    // records next -- leaving it in COPY_SOURCE would make the next pass's
    // barrier describe a state the resource is not in.
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // CopyTextureRegion takes D3D12_TEXTURE_COPY_LOCATION descriptors in this SDK,
    // not bare ID3D12Resource* pointers, and the two copy endpoints are named
    // rather than positional -- which is what makes it explicit that each side is
    // addressing subresource 0.
    //
    // PLACED_FOOTPRINT on the destination because it is now a buffer: a buffer
    // has no subresource layout of its own, so the copy has to be told exactly
    // where each row lands. The footprint is the one EnsureReadbackStaging
    // derived from the source descriptor, so source and destination agree by
    // construction. The source side stays SUBRESOURCE_INDEX: it is a texture,
    // and subresource 0 is the whole image (see CreateImages).
    D3D12_TEXTURE_COPY_LOCATION dstCopy = {};
    dstCopy.pResource = readbackStaging.Get();
    dstCopy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstCopy.PlacedFootprint.Offset = 0;
    dstCopy.PlacedFootprint.Footprint.Format = sourceDesc.Format;
    dstCopy.PlacedFootprint.Footprint.Width = (UINT)sourceDesc.Width;
    dstCopy.PlacedFootprint.Footprint.Height = (UINT)sourceDesc.Height;
    dstCopy.PlacedFootprint.Footprint.Depth = 1;
    dstCopy.PlacedFootprint.Footprint.RowPitch = (UINT)readbackRowPitch;

    D3D12_TEXTURE_COPY_LOCATION srcCopy = {};
    srcCopy.pResource = images[index].texture.Get();
    srcCopy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcCopy.SubresourceIndex = 0;

    // The destination coordinates are all zero -- the top-left texel of a
    // same-sized destination -- and a null source box means the whole image.
    commandList->CopyTextureRegion(&dstCopy, 0, 0, 0, &srcCopy, nullptr);
    Transition(images[index].texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushCommandList();

    // Wait on THIS slot's fence value, not a full drain. This is the same
    // targeted wait Output() performs, and for the same reason: the copy into
    // the staging texture is the only thing standing between here and reading
    // the pixels, so retiring that submission is sufficient and draining the
    // other two slots would stall for work whose result nobody is waiting on.
    //
    // The queue ordering matters too: because this submission is recorded after
    // every frame's Dispatch, the copy is ordered after the render that filled
    // the image, so the fence retirement implies the image was fully written.
    FrameContext &slot = frames[currentFrame];
    if (slot.submittedValue != 0 && fence->GetCompletedValue() < slot.submittedValue) {
        HRESULT hr = fence->SetEventOnCompletion(slot.submittedValue, fenceEvent);
        if (FAILED(hr)) {
            WL_CORE_ERROR("DX12 Nova readback: fence arm failed (0x{0:x})", (unsigned long)hr);
            return false;
        }
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    // THE ROW PITCH, asked for rather than assumed.
    //
    // D3D12 has no D3D12_MAPPED_SUBRESOURCE: that type belongs to D3D11, and
    // Map() here hands back a bare void** with no pitch at all. GetCopyableFootprints
    // is the D3D12 way to learn the layout the driver chose for a subresource, and
    // it is the same query every D3D12 readback uses.
    //
    // The pitch is almost never width*4. D3D12 pads every row out to a 256-byte
    // boundary, so a 160-texel-wide RGBA8 image has a 640-byte row in a 768-byte
    // stride. Reading width*4 bytes per row regardless would shear the image and
    // shear it MORE the further down it gets -- a wrong-pixel readback that looks
    // exactly like a rendering bug, which is worse than no readback at all.
    //
    // Read from the cache EnsureReadbackStaging filled in from the source
    // descriptor. Re-querying here would be the same answer for the same
    // dimensions and format, and the staging resource is now a buffer whose
    // descriptor carries no image layout to ask about. The lower bound is the
    // tightest row the image could possibly have; a pitch below it cannot hold
    // a row and would make the copy below walk off the end of the mapping.
    if (!readbackRowPitch ||
        readbackRowPitch < (UINT64)sourceDesc.Width * (isFloat ? 16 : 4))
    {
        WL_CORE_ERROR("DX12 Nova readback: cached row pitch {0} is too small for {1}x{2}",
            (unsigned long long)readbackRowPitch, (int)sourceDesc.Width, (int)sourceDesc.Height);
        return false;
    }
    const size_t rowPitch = (size_t)readbackRowPitch;

    // A NULL read range, which is the opposite of the constant buffer's: here the
    // CPU intends to read the whole resource, so D3D12 must not be told the
    // range is empty. Passing an empty D3D12_RANGE here is legal but leaves the
    // contents formally undefined on some drivers, which is exactly the class of
    // bug that makes a readback return plausible-looking garbage.
    //
    // void** rather than a mapped-subresource struct: that is the actual D3D12
    // Map signature. Map returns HRESULT in this SDK, so the result is checked
    // like any other call here.
    void *mapped = nullptr;
    HRESULT hr = readbackStaging->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped)
    {
        WL_CORE_ERROR("DX12 Nova readback: staging Map failed (0x{0:x})",
            (unsigned long)hr);
        return false;
    }

    // Bytes per texel in the SOURCE, which is not always 4. The float pass images
    // are R32G32B32A32_FLOAT -- 16 bytes per texel -- and the RGBA8 image is 4.
    // Hardcoding 4 would stride four times too slowly across a float row, reading
    // the first quarter of each row and mixing fragments from neighbouring rows
    // into every output row: an image that looks like a rendering bug and is not.
    // GetCopyableFootprints reported the row pitch from the real descriptor, and
    // this is the same fact expressed per texel.
    const size_t srcBytesPerTexel = isFloat ? 16 : 4;
    // OUTPUT rows are always tightly packed RGBA8, so the destination stride is
    // Width*4 regardless of the source format.
    const size_t rowBytes = (size_t)sourceDesc.Width * 4;
    rgba.resize(rowBytes * sourceDesc.Height);

    for (UINT y = 0; y < sourceDesc.Height; y++) {
        const unsigned char *src = (const unsigned char *)mapped + (size_t)y * rowPitch;
        unsigned char *dst = rgba.data() + (size_t)y * rowBytes;
        if (isUnorm8) {
            // Already bytes, and the same layout on both sides. memcpy rather than
            // a per-pixel loop, but bounded by the SOURCE row: rowBytes is the
            // destination stride, and reading a float-sized width*4 out of an
            // RGBA8 row would run past the row into the next one.
            memcpy(dst, src, rowBytes);
        } else {
            // Twelve bytes of padding at the end of a float row are skipped
            // entirely: the source row is read as texel-indexed floats, never as a
            // flat byte run.
            // Float -> 8-bit by a plain clamp and scale. No tone map and no
            // sRGB encode: this reports what the Nova passes actually wrote to
            // the image, which is the thing being verified. Post-processing here
            // would hide exactly the errors a capture is meant to expose.
            const float *in = (const float *)src;
            unsigned char *out = dst;
            const size_t texels = sourceDesc.Width;
            (void)srcBytesPerTexel; // documents the 16 above; the indexing below is texel-based
            for (size_t x = 0; x < texels; x++) {
                // Clamped, not wrapped: the pass images are HDR intermediates
                // and a channel above 1.0 is normal. Clamping maps every
                // over-range channel to white, so a blown-out highlight reads as
                // a highlight rather than wrapping to a saturated colour.
                float a = in[x * 4 + 3];
                float r = in[x * 4 + 0];
                float g = in[x * 4 + 1];
                float b = in[x * 4 + 2];
                r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
                g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
                b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
                a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
                out[x * 4 + 0] = (unsigned char)(r * 255.0f + 0.5f);
                out[x * 4 + 1] = (unsigned char)(g * 255.0f + 0.5f);
                out[x * 4 + 2] = (unsigned char)(b * 255.0f + 0.5f);
                out[x * 4 + 3] = (unsigned char)(a * 255.0f + 0.5f);
            }
        }
    }

    readbackStaging->Unmap(0, nullptr);
    // Narrowed for the same reason as the cached staging dimensions above.
    outWidth = (UINT)sourceDesc.Width;
    outHeight = (UINT)sourceDesc.Height;
    // Reached only when the pixels were actually copied out, so the guard's
    // destructor stays quiet on the success path.
    guard.ok = true;
    WL_CORE_INFO("DX12 Nova readback: captured {0}x{1} from image {2} (row pitch {3} bytes, {4} bytes/texel)",
        (int)sourceDesc.Width, (int)sourceDesc.Height, (int)index,
        (int)rowPitch, (int)(isFloat ? 16 : 4));
    return true;
}

// Explicit template instantiations. The declaration in the header is
// a template definition, so without these the linker has nothing to
// resolve calls from another translation unit.
// The list is the contract with the shader: every type written into
// NovaConstants must appear here and have a matching HLSL declaration.
// Adding a type to the shader without adding it here is a link error.
template void DX12Nova::Set<int>(const char*, const int&, UINT);
template void DX12Nova::Set<unsigned int>(const char*, const unsigned int&, UINT);
template void DX12Nova::Set<float>(const char*, const float&, UINT);
template void DX12Nova::Set<glm::vec2>(const char*, const glm::vec2&, UINT);
template void DX12Nova::Set<glm::vec3>(const char*, const glm::vec3&, UINT);
template void DX12Nova::Set<glm::vec4>(const char*, const glm::vec4&, UINT);
template void DX12Nova::Set<glm::mat4>(const char*, const glm::mat4&, UINT);

// Closing the namespace opened at the top of the file.
} // namespace Wasteland
