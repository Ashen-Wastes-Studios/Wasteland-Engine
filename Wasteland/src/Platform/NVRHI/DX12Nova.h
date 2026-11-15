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

// Generated constant-buffer layout (tools/gen_nova_cb_layout.py).
//
// D3DReflect is the DXBC/SM5.x reflection entry point and returns E_INVALIDARG
// on the SM6 DXIL container Slang emits, which is why the NovaConstants layout
// is extracted at shader-build time and compiled in here instead.
#include "NovaShaderReflection.h"

namespace Wasteland {

// Native DX12 compute resources for the Nova ray-tracer. Mirrors DX11Nova
// but uses explicit D3D12 command lists, descriptor heaps, and resource
// barriers. Every dispatch reads immutable snapshots, never its UAVs.
class DX12Nova {
    template<class T> using Com = Microsoft::WRL::ComPtr<T>;

    // FRAMES IN FLIGHT.
    //
    // Every Dispatch/CommitX call submits its own command list, so a frame is
    // up to ~13 separate ExecuteCommandLists (3 passes + history + 2 per blur
    // iteration + composite + 2 bloom + god rays). The allocator and command
    // list therefore cannot be single objects reset inline: recycling one
    // while the GPU is still executing it is undefined behaviour, and the old
    // code did exactly that (FlushCommandList reset the allocator immediately
    // after ExecuteCommandLists, with a fence that was created and never
    // signalled).
    //
    // Each slot is a complete independent submission context -- allocator,
    // command list, descriptor heap AND its own fence value -- so recording
    // into slot N+1 never touches anything slot N's GPU work still reads.
    // The descriptor heap must be per-slot too: the scene-texture SRVs are
    // rewritten before every dispatch, and a shared heap would be mutated
    // underneath an in-flight command list.
    //
    // 3 slots is the standard depth: enough that the GPU has a full frame of
    // work queued while the CPU records the next.
    static constexpr UINT kFramesInFlight = 3;

    struct FrameContext {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> commandList;
        Com<ID3D12DescriptorHeap> descriptorHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE heapCpuStart{};
        // Fence value this slot's work was submitted with; 0 = never
        // submitted. Compared against ID3D12Fence::GetCompletedValue.
        UINT64 submittedValue = 0;
    };

    struct Image {
        Com<ID3D12Resource> texture, snapshot;
        D3D12_RESOURCE_STATES snapshotState = D3D12_RESOURCE_STATE_COPY_DEST;
        // Per-slot UAV handles (kFramesInFlight entries).
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFramesInFlight> uavCpu{};
    };

    // The staging copy a CPU readback lands in, created lazily on the first
    // Readback and rebuilt only when the source image changes size or format.
    //
    // NOT recreated per call. A readback already stalls for a full GPU round
    // trip, so adding a committed-resource allocation (and the descriptor-less
    // heap pressure that comes with it) on top of that would make every capture
    // materially more expensive for no benefit.
    //
    // The dimensions and format are cached separately because a resource does
    // not report them back cheaply enough to compare on every capture, and
    // because comparing only the dimensions -- the mistake NVRHIFramebuffer's
    // version of this makes -- would silently keep a staging texture whose
    // format no longer matches its source after a resize.
    Com<ID3D12Resource> readbackStaging;
    UINT readbackWidth = 0, readbackHeight = 0;
    DXGI_FORMAT readbackFormat = DXGI_FORMAT_UNKNOWN;
    // The staging resource is a BUFFER, so the image's row pitch and total size
    // are not recoverable from its descriptor the way a texture's dimensions are.
    // Both are cached here so Readback can build its PLACED_FOOTPRINT copy
    // destination and walk rows without re-deriving them.
    UINT64 readbackRowPitch = 0;
    UINT64 readbackTotalBytes = 0;

    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> commandQueue;

    // The slot currently being recorded into. All commandList->... calls in
    // this class operate on this.
    Com<ID3D12GraphicsCommandList> commandList;
    // Mesh-shader path (Task 2.5): DispatchMesh lives on ID3D12GraphicsCommandList6.
    // It is QI'd from the current frame's command list at each dispatch — never
    // cached — because commandList is reassigned to a different per-frame slot's
    // list by every BeginSubmit.
    // TEMP DIAGNOSTIC [probepso]: the probe's exact working PSO + its empty sig.
    Com<ID3D12PipelineState> visProbePipelineState;
    Com<ID3D12RootSignature> visProbeRootSignature;
    Com<ID3D12CommandAllocator> commandAllocator;

    std::array<FrameContext, kFramesInFlight> frames;
    UINT currentFrame = 0;

    Com<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    Com<ID3D12RootSignature> rootSignature;
    Com<ID3D12PipelineState> pipelineState;
    Com<ID3DBlob> computeShader;

    // PER-SLOT, one upload buffer per frame slot, not one shared buffer.
    //
    // This was the one part of the frame ring that was NOT made per-slot:
    // kFramesInFlight gave each slot its own allocator, command list,
    // descriptor heap and fence value, but all three CBVs pointed at a
    // single constantBuffer that Dispatch overwrote via memcpy. A frame is
    // up to ~13 submissions (RunNovaPassSchedule), so slot 0's list could
    // still be executing when slot 1's Dispatch rewrote the bytes -- and
    // because u_PassID is uploaded inside Dispatch, the corruption is not
    // merely a stale float: an in-flight list can read a DIFFERENT PASS
    // NUMBER than the one it was recorded for.
    //
    // Cost is kFramesInFlight x one upload heap each, which is trivial
    // against the per-slot descriptor heaps already allocated.
    std::array<Com<ID3D12Resource>, kFramesInFlight> constantBuffers;
    UINT constantBufferSize = 0;
    std::array<void*, kFramesInFlight> constantBufferMapped = {};
    std::unordered_map<std::string, NovaReflection::CbVariable> variables;
    std::vector<unsigned char> bytes;

    std::array<Image, 8> images;
    Image history;
    // The SVGF temporal moments (img_Moments at u9, read_img_Moments at t73,
    // s_Moments at t53). A separate member rather than a ninth entry in `images`
    // for the same reason `history` is separate: `images` is indexed by the
    // shader's u0..u7 and by formats[] one-for-one, and growing it would
    // renumber the format table and every images[N] reference.
    Image moments;
    // WLFG reactive mask: single-channel (RGBA8) output of pass 24
    // (RunReactiveMask). Written once per present, read back by the dataset
    // exporter so the 7-feature model never needs the shader's alpha channel.
    // Separate member rather than a ninth entry in `images` for the same reason
    // `history` is separate: it is not one of the eight u0..u7 images.
    Image mask;
    // The WLFG previous-present history (read_img_WlfgHistory at t72). Separate
    // member for the same reason `history`/`moments` are: it is not one of the
    // eight u0..u7 images. It holds the last displayed frame so the warp (pass
    // 22) reprojects it; at factor 1 / zero velocity that is the bit-identical
    // acceptance. Not yet allocated or bound -- declared here so the addition is
    // one contiguous change.
    Image wlfgHistory;
    std::array<Com<ID3D12Resource>, 4> structuredBuffers;
    // Trained end-of-frame neural stage. `neuralGlobals` is the u12 UAV the
    // reduce and eval passes share; `neuralWeights` is the t74 SRV the eval pass
    // reads. Both are uploaded once at init from the same blob the GL path
    // loads, so the two backends cannot silently disagree about the weights.
    //
    // `neuralGlobals` is 16 + NEURAL_MAX_BLOCKS*4 bytes: the four frame means
    // (as raw float bits) followed by one uint32 partial per workgroup block.
    // See NeuralGlobals in the shader for why the layout matches the GLSL's SSBO
    // byte-for-byte.
    Com<ID3D12Resource> neuralGlobals;
    Com<ID3D12Resource> neuralWeights;
    bool neuralWeightsValid = false;
    // t75 -- WlfgWeights, the WLFG head's weights. Declaration-only on this
    // backend (see the descriptor site): the root signature's contiguous
    // readable range has to cover t75, so the slot needs a valid descriptor,
    // but no pass of this backend reads it.
    Com<ID3D12Resource> wlfgWeights;
    UINT neuralGlobalsUavSlot = 0;   // kUavBase + 12
    UINT neuralWeightsSrvSlot = 0;   // kNeuralWeightsSrvSlot
    // ReSTIR reservoir ping-pong buffers. Two buffers, each 32 bytes per pixel.
    // Swap after pass 11 to expose temporal output to pass 14, then after pass 14
    // to promote spatial output as next-frame history.
    Com<ID3D12Resource> reservoir[2];
    UINT reservoirParity = 0;
    std::array<D3D12_RESOURCE_STATES, 2> reservoirStates = {
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON
    };
    // Dispatch heaps expose one live u10 descriptor. Clear uses dedicated,
    // immutable shader-visible heaps (one per reservoir) plus CPU-only matching
    // descriptors, since a descriptor cannot be changed between queued clears.
    D3D12_UNORDERED_ACCESS_VIEW_DESC reservoirUavDesc = {};
    Com<ID3D12DescriptorHeap> reservoirClearCpuHeap;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> reservoirClearCpu{};
    std::array<Com<ID3D12DescriptorHeap>, 2> reservoirClearGpuHeaps;

    // Thirteen UAVs. The shader declares u0..u7, u9, u10, u11 and u14 -- NOT u8.
    // u8 is a hole: the moments UAV moved from u8 to u9 because on Vulkan the
    // NVRHI binding for u8 (offset 0 + 8) collides with NovaConstants (offset
    // 7 + 1), and the history image was later moved from u8 to u11 for the same
    // reason. The WLFG reactive mask (img_Mask) was moved from u8 to u14 for the
    // same collision. A D3D12 descriptor range is contiguous, so u8 still costs a
    // descriptor even though nothing binds it: the range has to run to u14 to
    // cover it, which is why kUavCount is 15 and not 13.
    //
    // This constant MUST be >= (highest shader UAV index + 1). The root
    // signature's UAV table is sized from it, so a shader that gains a UAV
    // without bumping this fails CreateComputePipelineState with E_INVALIDARG
    // (0x80070057) during Init -- the backend then silently falls back to raster
    // and logs "DX12 Nova init failed". That is exactly what happened when
    // img_HistoryBuffer moved to u11 while this stayed 11: every DX12 Nova init
    // failed. When adding a UAV, bump this AND the heap layout comment below.
    // 15, NOT 16: the DXIL excludes WLRR (WASTELAND_DXIL), so the shader
    // declares no u15 and the UAV table only needs u0..u14. Bumping this
    // to 16 without a u15 in the shader made CreateComputePipelineState
    // fail with E_INVALIDARG (0x80070057) -- DX12 Nova init failed, the
    // backend fell back to raster, and the later switch to Vulkan lost the
    // device. Vulkan's own UAV count is independent (VulkanNova.h).
    static constexpr UINT kUavCount = 15;
    UINT cbvSrvUavDescriptorSize = 0;

    // Heap layout, shared by every slot so the index arithmetic has one
    // definition. Offsets are descriptor-table indices within a slot's heap:
    //   0        CBV (NovaConstants)
    //   4..35    scene-texture SRVs (32)
    //   36..41   sampled-snapshot SRVs (6: t48..t53)
    //   42..53   readable-snapshot SRVs (12: t64..t75)
    //   54..68   UAVs (kUavCount = 15)
    // Indices 1..3 are unused padding left from the original fixed layout.
    //
    // The two SRV blocks grew for the SVGF moments resource, which the shader
    // reads twice: as `s_Moments : register(t53)` in pass 12 and as
    // `read_img_Moments : register(t73)` in pass 13. Both are sampled snapshots,
    // so both live in the SRV region -- but a D3D12 descriptor range must be
    // CONTIGUOUS and start at a fixed register, so covering t53 and t73 forced
    // each block wider than the registers actually used:
    //   - sampled: t48..t52 became t48..t53 (6), taking the block to 36..41
    //   - readable: t64..t71 became t64..t73 (10). t72 is never declared in the
    //     shader, so its slot is padding; t73 is the moments SRV.
    // kImageSrvBase and kUavBase therefore both shift, which is why they are
    // derived here rather than left as the old literals. Every relative offset
    // inside a block is unchanged (image i is still kImageSrvBase+i, history is
    // still kUavBase+11), so only the block bases moved.
    //
    // NEURAL STAGE. u12 is NeuralGlobals, the RWStructuredBuffer the reduce and
    // eval passes (17/18/19 in the GLSL) share. It was empty before: nothing in
    // the shader claimed it, and the binding reservation
    // (docs/NEURAL_RENDERING_BINDING_PLAN.md) assigns it to this stage. The
    // weights are an SRV, not a UAV -- the stage only reads them -- and they sit
    // in the readable-snapshot block at t74, the first register past that block's
    // old t64..t73 range.
    //
    // Both blocks grew by one, and the UAV block had to MOVE to make room:
    //   - readable SRVs 42..51 (10) -> 42..52 (11), so t74 is at heap index 52
    //   - UAVs 52..63 (12) -> 53..65 (13), so kUavBase moved 52 -> 53
    // kUavBase is referenced everywhere as a relative base (kUavBase + i,
    // +9, +10, +11), so moving it shifts all of them consistently -- which is
    // exactly why it is a named constant and not a literal. Grep for a bare 52
    // or 63 in this file before trusting this layout.
    //
    // WLFG STAGE. t75 is WlfgWeights, the pass-23 synthesis head's weights: a
    // StructuredBuffer<float> the shader only reads, so an SRV like the neural
    // weights beside it. It is the next register past the readable block's
    // t64..t74 range, so the block grew a second time and the UAV block moved
    // again -- this is the SAME E_INVALIDARG trap documented on kUavCount above,
    // and it fired exactly as described the first time t75 was declared without
    // extending ranges[2]: DX12 Nova init failed, the backend fell back to
    // raster, and the later switch to Vulkan lost the device.
    //   - readable SRVs 42..52 (11) -> 42..53 (12), so t75 is at heap index 53
    //   - UAVs 53..67 (15) -> 54..68 (15), so kUavBase moved 53 -> 54
    static constexpr UINT kSceneTextureBase = 4;
    static constexpr UINT kSamplerBase = 36;
    static constexpr UINT kImageSrvBase = 42;
    // PHASE 1 HYBRID VISIBILITY. t78..t81 are the raster visibility G-buffer
    // (meshId, albedo, normal, depth), read by the compute passes when
    // u_UseRasterVisibility is set. Contiguity from a base of 64 forces the
    // readable range to 18, which moves the UAV block again -- the SAME
    // E_INVALIDARG trap documented above, and the third time it has fired.
    //   - readable SRVs 42..53 (12) -> 42..59 (18), so t81 is at heap index 59
    //   - UAVs 54..68 (15) -> 60..74 (15), so kUavBase moved 54 -> 60
    static constexpr UINT kUavBase = 60;
    static constexpr UINT kDescriptorsPerHeap = kUavBase + kUavCount;
    // t74 -- NeuralWeights, the last entry of the readable-snapshot block.
    static constexpr UINT kNeuralWeightsSrvSlot = kImageSrvBase + 10;
    // t75 -- WlfgWeights, the next entry.
    static constexpr UINT kWlfgWeightsSrvSlot = kImageSrvBase + 11;
    // t78..t81 -- the raster visibility G-buffer SRVs.
    static constexpr UINT kVisibilityIdsSrvSlot    = kImageSrvBase + 14;
    static constexpr UINT kVisibilityAlbedoSrvSlot = kImageSrvBase + 15;
    static constexpr UINT kVisibilityNormalSrvSlot = kImageSrvBase + 16;
    static constexpr UINT kVisibilityDepthSrvSlot  = kImageSrvBase + 17;
    // Floats in the WLFG blob payload: W1(7x16) b1(16) W2(16x16) b2(16)
    // Wr(16x3) br(3) Wg(16x1) bg(1) = 468. Must match model.py's N_WEIGHTS, the
    // shader's WLFG_N_WEIGHTS and VulkanNova.h's kWlfgWeightCount.
    static constexpr UINT kWlfgWeightCount = 468;


    UINT width = 0, height = 0;

    // VISIBILITY G-BUFFER (Phase 1, Tasks 1.2/1.2b).
    //
    // Raw D3D12, like everything else in this class: DX12Nova does not use nvrhi, so the
    // engine's generic Framebuffer/Texture2D abstractions cannot create these. The
    // visibility pass renders primary hits into three colour targets + depth; the compute
    // schedule later reads them as SRVs.
    //
    // Formats follow the plan's Phase 1 resolution: RT0 is R32_SINT (the engine's
    // RED_INTEGER) because the analytic path only needs meshId -- primId is always 0 -- and
    // a two-channel format would be dead weight until Phase 2's mesh shaders need it.
    struct VisibilityTargets {
        Com<ID3D12Resource> color0;       // R32_SINT        -- meshId
        Com<ID3D12Resource> color1;       // R8G8B8A8_UNORM  -- albedo
        Com<ID3D12Resource> color2;       // R8G8B8A8_UNORM  -- normal
        Com<ID3D12Resource> depth;        // D24_UNORM_S8_UINT
        UINT width = 0, height = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        // Tracked separately because the depth target moves DEPTH_WRITE ->
        // shader-readable -> DEPTH_WRITE every frame: Task 1.3 samples it as an
        // SRV, which is undefined while it is in DEPTH_WRITE. The colours share
        // `state`; the depth cannot, because its write state differs.
        D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    };
    VisibilityTargets vis;
    Com<ID3D12DescriptorHeap> visRtvHeap;
    Com<ID3D12DescriptorHeap> visDsvHeap;
    // Descriptor indices within the two heaps. RTVs are contiguous from visRtvBase
    // (color0, color1, color2); the DSV is at visDsvBase.
    UINT visRtvBase = 0;
    UINT visDsvBase = 0;

    // The visibility pipeline reuses the compute root signature: it already exposes b1
    // (rootParams[0], the CBV) and t1 (rootParams[1], the Instances SRV) and was built
    // with ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT precisely so a graphics PSO could use it.
    // Its comment says so. A second signature would duplicate a working one.
    Com<ID3D12PipelineState> visPipelineState;
    Com<ID3DBlob> visVertexShader;
    Com<ID3DBlob> visPixelShader;

    // --- Mesh-shader path (Phase 2, Task 2.5) ---
    // The same G-buffer, produced by a mesh pipeline instead of a VS/FS pair. Both
    // are live: DrawVisibility picks one per frame, and the compute schedule cannot
    // tell them apart.
    //
    // A DEDICATED root signature, not the compute one. The compute signature binds
    // t2 and t3 as BVH nodes and light indices, which the mesh shader's t2 (mesh
    // vertices) and t3 (meshlets) would collide with. The mesh path declares six
    // root descriptors: b1 MeshConstants, t1 Instances, t2 MeshVertices,
    // t3 Meshlets, t4 MeshletVertices, t5 MeshletIndices.
    Com<ID3D12RootSignature> visMeshRootSignature;
    // MS+PS and AS+MS+PS. The AS variant is the default: the AS decides which
    // meshlets to run and dispatches one mesh group per meshlet. The MS+PS variant
    // exists so the mesh path can be tested without the AS.
    Com<ID3D12PipelineState> visMeshPipelineState;
    Com<ID3D12PipelineState> visMeshASPipelineState;
    Com<ID3DBlob> visMeshShader;
    Com<ID3DBlob> visMeshPixelShader;
    Com<ID3DBlob> visMeshAS;

    // Per-shape meshlet data, indexed by PrimitiveShape. `visMeshletBuffer` holds
    // MeshletData, `visMeshletVertexBuffer` the remapped mesh vertex indices,
    // `visMeshletIndexBuffer` the meshlet-LOCAL triangle indices, and
    // `visMeshVertexBuffer` the packed MeshVertex the mesh shader reads.
    Com<ID3D12Resource> visMeshletBuffer[8];
    Com<ID3D12Resource> visMeshletVertexBuffer[8];
    Com<ID3D12Resource> visMeshletIndexBuffer[8];
    Com<ID3D12Resource> visMeshVertexBuffer[8];
    UINT visMeshletCount[8] = {};
    // Concatenated tables live in slot 0 of each array; the dispatch table
    // (per-shape meshlet offset/count) lets the mesh shader derive
    // (shape, instance, meshlet) from its group index, so one DispatchMesh
    // draws the whole scene with no per-dispatch CPU state.
    Com<ID3D12Resource> visDispatchBuffer;
    UINT visMeshTotal = 0;
    // The meshlet vertex/index buffers are built once from ProceduralMeshes, which
    // is why this is a creation-time step rather than a per-frame one.
    bool visMeshletsBuilt = false;

    // u_ViewProjection, uploaded per frame. Separate from the compute constant buffer:
    // NovaConstants' offsets are generated by tools/gen_nova_cb_layout.py and start at
    // zero, so appending a matrix would shift every one of them.
    // PER-SLOT, matching constantBuffers above and for the same reason: the
    // view-projection changes every frame, so a single shared buffer would be
    // rewritten by the CPU while an earlier submission is still reading it.
    std::array<Com<ID3D12Resource>, kFramesInFlight> visConstantBuffer;
    std::array<void*, kFramesInFlight> visConstantBufferMapped = {};
    Com<ID3D12Resource> visInstanceBuffer;

    // Per-shape meshes, indexed by the PrimitiveShape ordinal (ProceduralMeshes.h).
    // The scene is drawn once per shape and the VS clips every instance belonging to
    // a different shape, so a handful of instanced draws covers the whole scene while
    // meshId stays SV_InstanceID -- an index into the SAME reordered instance buffer
    // the compute path binds at t1. Compacting the buffer per shape would renumber
    // the instances and silently break that contract.
    Com<ID3D12Resource> visShapeVertexBuffer[8];
    Com<ID3D12Resource> visShapeIndexBuffer[8];
    // 1x1 stand-ins for the t78..t81 slots, so the descriptors are ALWAYS valid
    // even before the raster path has created its targets (the whole visibility
    // experiment is opt-in). The shader gates the read on u_UseRasterVisibility,
    // so these are never actually sampled -- a null descriptor in a bound range
    // would be undefined behaviour rather than a harmless no-op.
    Com<ID3D12Resource> visDummyInt;
    Com<ID3D12Resource> visDummyFloat4;
    Com<ID3D12Resource> visDummyFloat;
    UINT visShapeIndexCount[8] = {};
    UINT visShapeVertexCount[8] = {};
    // The visibility constant buffer is split into one 256-byte region per shape.
    // The GPU may execute a recorded draw long after the CPU has moved on, so a
    // single shared region would make every draw see whichever shape was written
    // last -- the per-shape region is written once at creation and never again.
    static const UINT kVisShapeCount    = 8;
    static const UINT kVisCbRegionSize = 256;   // D3D12 constant buffer alignment
    static const UINT kVisShapeOffset  = 64;    // sizeof(float4x4)

    // Scene-driven draw state, set per frame by Renderer3D. When
    // visSceneInstanceCount > 0 the pass draws the real scene from
    // structuredBuffers[0]; otherwise it draws the synthetic two-cube gate.
    UINT visSceneInstanceCount = 0;
    float visViewProjection[16] = {};
    UINT visInstanceCount = 0;
    bool visReady = false;

    void CreateVisibilityResources(UINT w, UINT h);
    void CreateVisibilityPipeline();
    void CreateVisibilityGeometry();
    // Mesh path (Task 2.5): root signature, pipeline, meshlet upload, draw.
    void CreateMeshRootSignature();
    void CreateVisibilityMeshPipeline();
    void CreateVisibilityMeshlets();
    void DrawMeshlets();
    void ReleaseVisibilityResources();
    // Points the t78..t81 SRV slots at the live visibility targets, or at 1x1
    // stand-ins before they exist. Every slot's heap needs its own descriptor
    // because the table the GPU reads lives in whichever slot submitted it.
    void WriteVisibilitySrvs();
    // One slot's copy, called per dispatch: the readable-snapshot SRVs use this
    // pattern and a descriptor written once at init is not reliably visible to a
    // later slot's command list.
    void WriteVisibilitySrvs(FrameContext& slot, UINT index);
    void DrawVisibility();

    void Check(HRESULT hr, const char* step = nullptr) {
        if (FAILED(hr)) {
            std::string msg = "DX12 Nova HRESULT " + std::to_string((unsigned long)hr);
            if (step) msg += " at " + std::string(step);
            // TEMP DIAGNOSTIC [rmreason]: when the device is removed, the reason
            // code names the fault (hung / page fault / driver internal). Logged
            // once, on the first failure. Removed once the driver issue is
            // settled.
            if (hr == 0x887A0005 && device) {
                WL_CORE_ERROR("DX12 device removed, reason 0x{:08x}",
                              (unsigned)device->GetDeviceRemovedReason());
            }
            throw std::runtime_error(msg);
        }
    }

    void CreateImages(UINT w, UINT h);
    void CreateConstantBuffer(UINT size);
    void CreateRootSignature();
    void CreatePSO();
    void CreateDescriptorHeaps();
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void TransitionSnapshot(Image& image, D3D12_RESOURCE_STATES after);
    void UavBarrier(ID3D12Resource* res);
    void FlushCommandList();
    void EnsureConstantBufferSize(UINT size);

    // Creates or rebuilds readbackStaging so it matches the source image's
    // extent and format, which is what CopyTextureRegion requires of both ends.
    // Leaves readbackStaging null and the cached dimensions zeroed on failure --
    // it reports rather than throws, because the only caller is the editor's
    // screenshot hook, which must not take the process down.
    void EnsureReadbackStaging(const D3D12_RESOURCE_DESC& source);

    // Blocks until the GPU has finished all previously submitted work in every
    // slot. Needed before destroying GPU resources (CreateImages on resize,
    // Upload replacing a structured buffer), because the GPU may still be
    // reading them.
    void WaitForGpuIdle();

    // Capture the reactive-mask (pass 24 output) to a PNG on disk. The mask is a
    // separate D3D12 resource from the main image array, so this is the only
    // way to pull it off the GPU. Writes an 8-bit RGBA PNG (mask value as
    // luma), returns true on success.

    // Adopts the next slot as the recording target,
    // slot's previous submission has not retired. Must be called before each
    // unit of recorded work (Dispatch / CommitHistory / CommitDenoise /
    // Output / Clear).
    void BeginSubmit();

public:
    ~DX12Nova();

    bool CaptureMaskPNG(const std::string& path);

    // real ring/heap configuration instead of leaving the DX12 Nova path silent.
    // Cheap, static, and read once at init.
    static UINT GetFramesInFlight() { return kFramesInFlight; }
    static UINT GetDescriptorsPerSlot() { return kDescriptorsPerHeap; }
    static UINT GetUavCount() { return kUavCount; }

    void Init(ID3D12Device* d, ID3D12CommandQueue* q);
    void Resize(UINT w, UINT h);
    // Diagnostic wrapper that catches structured exceptions around Resize.
    // Removed once the 0xC0000007B root cause is identified.
    void ResizeSEH(UINT w, UINT h);
    void Clear();
    // Fills neuralWeights from assets/neural/neural_weights.bin and promotes it to
    // a shader-readable state. No-op once neuralWeightsValid is true.
    //
    // Called from Clear() rather than CreateImages(), because the copy has to be
    // recorded on a command list and Clear() is what opens one at init time.
    // CreateImages() runs before any submission exists.
    void UploadNeuralWeights();
    bool IsNeuralWeightsValid() const { return neuralWeightsValid; }
    void DispatchNeuralStage(UINT w, UINT h);
    template<class T> void Set(const char* name, const T& value, UINT index = 0);
    void Upload(UINT slot, const void* data, UINT count, UINT stride);
    void Unbind();
    void Dispatch(int pass, UINT w, UINT h, const std::vector<ID3D12Resource*>& textures);
    void CommitHistory();
    // WLFG previous-present history. No-op on DX12: `wlfgHistory` is declared but
    // never allocated, so pass 22 has nothing to reproject from here. Declared so
    // the shared RunNovaPassSchedule can call it on every backend.
    void CommitWlfgHistory() {}
    // WLRR is Vulkan-only for now: passes 20/21 live in the Nova
    // schedule, and neither the DX11 nor the DX12 backend binds the
    // t76/t77 payload or img_WlrrInput. The no-op keeps the shared
    // schedule in Renderer3D.cpp compilable for every backend.
    void CommitWlrrSnapshot() {}
    void CommitDenoise();
    // Creates or rebuilds the dedicated RGBA8 reactive-mask image used to
    // capture pass 24's output for the dataset pipeline. Shared with the
    // Resize path, so a mid-session resolution change recreates it safely.
    void CreateMask(UINT w, UINT h);
    // Flips the SRV/UAV roles at a ReSTIR phase boundary. Call after pass 11 to
    // expose its temporal result as pass 14's immutable SRV, then after pass 14
    // to promote the spatial output as next-frame history.
    void SwapReservoir();
    void* Output(UINT index = 0);

    // Records and submits the visibility pass in one call. A wrapper rather than
    // exposing BeginSubmit/FlushCommandList, which are private: the frame ring is
    // an implementation detail and Renderer3D should not drive it directly.
    void SubmitVisibility();

    // Scene-driven visibility draw: the instance count comes from the reordered scene
    // instance list and the matrix is the live camera's view-projection. Without this
    // the pass renders the synthetic gate geometry from a fixed camera, which proves
    // the pipeline but not the scene.
    void SetVisibilityScene(UINT instanceCount, const float* viewProjection);

    // Reads one visibility target back to the CPU as 8-bit RGBA:
    //   0 = meshId (R32_SINT, expanded to greyscale)
    //   1 = albedo (RGBA8, direct)
    //   2 = normal (RGBA8, direct)
    // Returns false -- leaving `rgba` untouched -- when the pass has not run or the
    // index is out of range. Stalls for a full GPU round trip, like Readback.
    bool ReadbackVisibility(UINT index, std::vector<unsigned char>& rgba,
                            UINT& outWidth, UINT& outHeight);

    // Copies the render image `index` to the CPU as tightly packed, top-down
    // 8-bit RGBA in `rgba`, which is resized to width*height*4 on success.
    // Returns false -- leaving `rgba` untouched -- when there is nothing to read
    // (no device yet, no images yet, an out-of-range index) or when the image's
    // format has no 8-bit-RGBA reading.
    //
    // This is the capture/verification path, so it stalls: it submits its own
    // copy through the frame ring and waits for that slot's fence. It is NOT on
    // the per-frame path and must not become one.
    bool Readback(UINT index, std::vector<unsigned char>& rgba, UINT& outWidth, UINT& outHeight);

    // Lossless float readback for the WLFG dataset state inputs (velocity RG32, depth
    // RGBA32). DX12's staging copy is done in the source format, but the 8-bit path
    // quantizes; the float path must be verified before it ships. Returns false until
    // verified.
    bool ReadbackFloat(uint32_t index, std::vector<float> &out, UINT &width, UINT &height) const;
    // Read the dedicated RGBA8 reactive-mask image (pass 24 output) to the CPU,
    // same convention as Readback. The mask is a separate resource, so it is not
    // indexed by the main image array.
    void ReadMaskStaging(std::vector<unsigned char>& rgba, UINT& outWidth, UINT& outHeight);
};

} // namespace Wasteland
