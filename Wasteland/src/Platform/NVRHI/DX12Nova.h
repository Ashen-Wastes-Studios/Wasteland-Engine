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
    std::unordered_map<std::string, D3D12_SHADER_VARIABLE_DESC> variables;
    std::vector<unsigned char> bytes;

    std::array<Image, 8> images;
    Image history;
    // The SVGF temporal moments (img_Moments at u9, read_img_Moments at t73,
    // s_Moments at t53). A separate member rather than a ninth entry in `images`
    // for the same reason `history` is separate: `images` is indexed by the
    // shader's u0..u7 and by formats[] one-for-one, and growing it would
    // renumber the format table and every images[N] reference.
    Image moments;
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

    // Twelve UAVs. The shader declares u0..u7, u9, u10 and u11 -- NOT u8.
    // u8 is a hole: the moments UAV moved from u8 to u9 because on Vulkan the
    // NVRHI binding for u8 (offset 0 + 8) collides with NovaConstants (offset
    // 7 + 1), and the history image was later moved from u8 to u11 for the same
    // reason. A D3D12 descriptor range is contiguous, so u8 still costs a
    // descriptor even though nothing binds it: the range has to run to u11 to
    // cover u11, which is why kUavCount is 12 and not 10.
    //
    // This constant MUST be >= (highest shader UAV index + 1). The root
    // signature's UAV table is sized from it, so a shader that gains a UAV
    // without bumping this fails CreateComputePipelineState with E_INVALIDARG
    // (0x80070057) during Init -- the backend then silently falls back to raster
    // and logs "DX12 Nova init failed". That is exactly what happened when
    // img_HistoryBuffer moved to u11 while this stayed 11: every DX12 Nova init
    // failed. When adding a UAV, bump this AND the heap layout comment below.
    static constexpr UINT kUavCount = 13;
    UINT cbvSrvUavDescriptorSize = 0;

    // Heap layout, shared by every slot so the index arithmetic has one
    // definition. Offsets are descriptor-table indices within a slot's heap:
    //   0        CBV (NovaConstants)
    //   4..35    scene-texture SRVs (32)
    //   36..41   sampled-snapshot SRVs (6: t48..t53)
    //   42..52   readable-snapshot SRVs (11: t64..t74)
    //   53..65   UAVs (kUavCount = 13)
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
    static constexpr UINT kSceneTextureBase = 4;
    static constexpr UINT kSamplerBase = 36;
    static constexpr UINT kImageSrvBase = 42;
    static constexpr UINT kUavBase = 53;
    static constexpr UINT kDescriptorsPerHeap = kUavBase + kUavCount;
    // t74 -- NeuralWeights, the last entry of the readable-snapshot block.
    static constexpr UINT kNeuralWeightsSrvSlot = kImageSrvBase + 10;


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

    // Adopts the next slot as the recording target, blocking first if that
    // slot's previous submission has not retired. Must be called before each
    // unit of recorded work (Dispatch / CommitHistory / CommitDenoise /
    // Output / Clear).
    void BeginSubmit();

public:
    ~DX12Nova();

    // Introspection for the Renderer3D init diagnostic: lets the log state the
    // real ring/heap configuration instead of leaving the DX12 Nova path silent.
    // Cheap, static, and read once at init.
    static UINT GetFramesInFlight() { return kFramesInFlight; }
    static UINT GetDescriptorsPerSlot() { return kDescriptorsPerHeap; }
    static UINT GetUavCount() { return kUavCount; }

    void Init(ID3D12Device* d, ID3D12CommandQueue* q);
    void Resize(UINT w, UINT h);
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
    void CommitDenoise();
    // Flips the SRV/UAV roles at a ReSTIR phase boundary. Call after pass 11 to
    // expose its temporal result as pass 14's immutable SRV, then after pass 14
    // to promote the spatial output as next-frame history.
    void SwapReservoir();
    void* Output(UINT index = 0);

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
};

} // namespace Wasteland
