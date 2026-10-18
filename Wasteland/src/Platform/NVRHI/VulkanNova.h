#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "Wasteland/Renderer/Texture.h"

namespace Wasteland
{
    class NVRHIContext;


    // NVRHI/Vulkan compute resources for the Nova path tracer.
    //
    // Mirrors the DX11Nova/DX12Nova interface exactly so the shared
    // RunNovaPassSchedule() schedule drives all three backends. The shader is
    // the same HLSL the DX12 path uses, compiled to SPIR-V by
    // tools/compile_nova_spirv.py.
    //
    // WHY SPIR-V AND NOT THE GLSL
    // --------------------------
    // Vulkan GLSL forbids loose (non-opaque, non-block) uniforms, and Nova
    // declares ~50 of them. The HLSL already packs them into one
    // 'cbuffer NovaConstants', so DXC -> SPIR-V is the route that works, and it
    // reuses the existing shader instead of adding a third variant.
    //
    // BINDING CONTRACT
    // ---------------
    // NVRHI derives a Vulkan descriptor binding as 'offset + slot'
    // (vulkan-resource-bindings.cpp:100), mirroring DXC's register numbering.
    // Left alone the two collide at four points, so tools/compile_nova_spirv.py
    // shifts the register spaces into disjoint ranges at compile time
    // (u=0, b=7, t=16, s=200) and the same values go into bindingOffsets below.
    // The tool verifies the binding contract stays collision-free.
    //
    // The slots below are the HLSL register numbers. Changing a register in the
    // shader without re-running that tool will produce an invalid descriptor set
    // (or, worse, a valid one binding the wrong texture).
    class VulkanNova
    {
    public:
        ~VulkanNova();

        // All failures throw; the caller catches and falls back to raster.
        void Init();
        void Resize(uint32_t w, uint32_t h);
        void Clear();

        // Writes a constant buffer field by name. Array fields (u_FogMin etc.)
        // take an element index; every array in NovaConstants has a 16-byte
        // stride under both D3D and std140 packing, which
        // tools/compile_nova_spirv.py verifies member-by-member.
        template <class T>
        void Set(const char *name, const T &value, uint32_t index = 0)
        {
            static_assert(std::is_trivially_copyable<T>::value,
                          "VulkanNova constants must be trivially copyable");
            SetBytes(name, &value, sizeof(T), index);
        }

        // slot: 0 = Instances, 1 = BVHNodes, 2 = LightIndices.
        void Upload(uint32_t slot, const void *data, uint32_t count, uint32_t stride);
        void Unbind() {}

        // Mirrors DX11Nova/DX12Nova so RunNovaPassSchedule() drives all three.
        // For DX the vector carries native SRV pointers; here the caller passes
        // scene textures and VulkanNova resolves them to nvrhi textures, since
        // this NVRHI has no separate SRV object.
        void Dispatch(int pass, uint32_t w, uint32_t h,
                      const std::vector<Ref<Texture2D>> &textures);
        void CommitHistory();
        void CommitDenoise();
        void SwapReservoir();

        // Native handle for compositing the result into the swapchain.
        void *Output(uint32_t index = 0) const;

        bool IsValid() const { return m_Valid; }

        // First-dispatch self-check. The binding map is verified at build time
        // by tools/compile_nova_spirv.py, but this confirms at runtime that
        // NVRHI actually built the expected layout with the right
        // offset+slot arithmetic -- if that ever disagrees the frame is wrong
        // in a way no exception reports.
        void LogBindingContract() const;

    public:
        struct ConstantField
        {
            const char *name;
            uint32_t offset;
            uint32_t arraySize;
        };

    private:
        struct Image
        {
            nvrhi::TextureHandle texture;
            nvrhi::TextureHandle snapshot; // copy target; read_* SRVs bind this
            bool snapshotDirty = true;
        };

        // Nine images: the eight pass images (u0..u7) plus the SVGF moments
        // image at u9. u8 is a deliberate gap -- the moments UAV was moved off
        // u8 to u9 because on Vulkan the NVRHI binding for u8 (offset 0 + 8)
        // collides with NovaConstants (offset 7 + 1). The gap costs one
        // descriptor and keeps the HLSL/GLSL twins identical.
        static constexpr uint32_t kImageCount = 9;
        // Internal image index -> HLSL UAV register. Index 8 is moments at u9;
        // u8 is intentionally unbound.
        static const uint32_t kImageUavSlot[kImageCount];
        static constexpr uint32_t kSceneTextureCount = 32;
        // NovaConstants, from spirv-cross: block_size 1744.
        static constexpr uint32_t kConstantBufferSize = 1744;

        // Active read_* SRVs. ReSTIR adds AlbedoHit (t68) and TraceGBuffer
        // (t70); MaterialPacked's t67 alias remains unused. Moments is t73 and
        // maps to image index 8; t72 is intentionally absent. VulkanNova.cpp
        // defines the ordered slot/index maps and VerifySpirvBindings() checks
        // them against the module loaded at runtime.
        static constexpr uint32_t kReadImageCount = 8;
        static const uint32_t kReadImageSlot[kReadImageCount];  // 64,65,66,68,69,70,71,73
        static const uint32_t kReadImageIndex[kReadImageCount]; // 0,1,2,4,5,6,7,8
        // Sampled SRV slots t48..t53 -> (s_Accumulation, s_DepthBuffer,
        // s_Output, s_NormalBuffer, s_InputAlbedo, s_Moments). t48 is NOT one of
        // the caller's scene textures: it is the temporal history texture
        // written by CommitHistory() and read by the reprojection pass. See the
        // t48 binding in VulkanNova::CreateBindingSet. t53 is s_Moments, the
        // previous frame's moments that pass 12 samples at a reprojected UV.
        static constexpr uint32_t kSampledCount = 6;
        static constexpr uint32_t kSampledSlot[kSampledCount] = {48, 49, 50, 51, 52, 53};

        nvrhi::IDevice *m_Device = nullptr;
        nvrhi::ICommandList *m_CommandList = nullptr;
        NVRHIContext *m_Context = nullptr;
        bool m_Valid = false;

        nvrhi::ShaderHandle m_ComputeShader;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::ComputePipelineHandle m_Pipeline;
        nvrhi::SamplerHandle m_Sampler;

        Image m_Images[kImageCount];
        nvrhi::BufferHandle m_StructuredBuffers[3];
        uint32_t m_StructuredBufferSize[3] = {0, 0, 0};
        // HLSL stride per slot; Upload supplies it, and it is baked into
        // structStride when the buffer is (re)created.
        uint32_t m_StructuredBufferStride[3] = {0, 0, 0};

        nvrhi::BufferHandle m_ConstantBuffer;
        std::vector<unsigned char> m_ConstantBytes;

        uint32_t m_Width = 0, m_Height = 0;
        bool m_LoggedBindingContract = false;
        // Dispatch is called several times per frame. Compare all scene texture
        // slots on each call, but rebuild bindings only when resources change.
        // Copy internal image snapshots when dirty, immediately before a pass
        // reads an image written earlier in the frame. Keep old binding sets
        // alive for already-recorded work. u_FrameIndex is the engine's frame
        // token; Renderer3D sets it once before the pass schedule.
        uint32_t m_CurrentFrame = 0xFFFFFFFFu;
        uint32_t m_BoundFrame = 0xFFFFFFFFu;

        // Scene texture handles are cached by their nvrhi texture pointer.
        std::unordered_map<nvrhi::ITexture *, nvrhi::TextureHandle> m_SceneSRVCache;
        std::vector<nvrhi::TextureHandle> m_SceneSRVs;

        // The temporal history: written by CommitHistory() from the accumulation
        // image and bound as an SRV at t48 (s_Accumulation). It is a separate
        // texture from m_Images[1] because that one is the img_Accumulation UAV
        // in the same descriptor set, and a resource cannot be written and
        // sampled in one dispatch.
        nvrhi::TextureHandle m_HistorySnapshot;

        // ReSTIR reservoir ping-pong buffers. Two buffers, each 32 bytes per pixel.
        // Read is t10 (SPIR-V binding 26), write is u10 (binding 10); swap after
        // the consumer pass so next frame reads this frame's combined result.
        nvrhi::BufferHandle m_ReservoirBuffers[2];
        uint32_t m_ReservoirWidth = 0;
        uint32_t m_ReservoirHeight = 0;
        bool m_ReservoirReadFlip = false;
        bool m_ReservoirsNeedClear = true;
        bool m_ImagesNeedClear = false;
        // Swaps only mark this dirty. The replacement set is built on the next
        // Dispatch, leaving the current command list's recorded set alive.
        bool m_BindingSetDirty = false;

        void CreateImages(uint32_t w, uint32_t h);
        void CreateStaticResources();
        void CreatePipelineAndLayout();
        void CreateBindingSet();
        void EnsureStructuredBuffer(uint32_t slot, uint32_t byteSize, uint32_t stride);
        void CopyImageToSnapshot(uint32_t imageIndex);
        void CopyImagesToSnapshots();
        void RefreshImageSnapshotsForPass(int pass);
        void MarkImageSnapshotsDirtyAfterPass(int pass);
        nvrhi::ITexture *ResolveSceneTexture(Texture2D *texture);
        void RecordCopy(nvrhi::ITexture *dst, nvrhi::ITexture *src);
        void RecordImageHistoryClear();
        bool EnsureCommandList();

        // Parses the SPIR-V module and throws unless its set of OpDecorate
        // Binding numbers matches the set this class's layout creates, in both
        // directions. Called from Init() before any Vulkan object is built from
        // these tables, so a table that has drifted from the .spv fails loudly
        // instead of producing a valid descriptor set holding the wrong
        // texture. See the comment on the implementation for why the build-time
        // tool cannot cover this.
        void VerifySpirvBindings(const std::vector<char> &spirv) const;

        static const ConstantField *FindConstant(const char *name);
        void SetBytes(const char *name, const void *value, std::size_t size, uint32_t index);
    };

} // namespace Wasteland