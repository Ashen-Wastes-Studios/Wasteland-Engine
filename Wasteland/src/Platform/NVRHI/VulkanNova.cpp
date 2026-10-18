#include "wlpch.h"

#include "Platform/NVRHI/VulkanNova.h"
#include "Platform/NVRHI/NVRHIContext.h"
#include "Platform/NVRHI/NVRHIRendererAPI.h"
#include "Platform/NVRHI/NVRHITexture.h"

#include "Wasteland/Core/Log.h"
#include "Wasteland/Project/Project.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace Wasteland
{

    // NVRHI derives a Vulkan descriptor binding as offset + slot
    // (vulkan-resource-bindings.cpp:100). These must equal the -fvk-*-shift
    // values tools/compile_nova_spirv.py bakes into the SPIR-V; both the layout
    // (CreatePipelineAndLayout) and the module cross-check
    // (VerifySpirvBindings) are written against these, so they are defined once
    // here rather than repeated as literals.
    static constexpr uint32_t kBindingOffsetUav = 0;
    static constexpr uint32_t kBindingOffsetConstantBuffer = 7;
    static constexpr uint32_t kBindingOffsetShaderResource = 16;
    static constexpr uint32_t kBindingOffsetSampler = 200;

    // HLSL register slots for the read_* SRVs and the image each one snapshots.
    //
    // WHY ONLY FIVE OF THE EIGHT read_* DECLARATIONS IN THE HLSL:
    // the .hlsl declares read_img_Output/Accumulation/Bloom/MaterialPacked/
    // AlbedoHit/Velocity/TraceGBuffer/Bloom_Temp at t64..t71, but the compiled
    // module declares only five of them. DXC eliminates a global that nothing
    // reads, and the only read path is the IMG_LOAD(tex, pos) macro, which
    // token-pastes to read_##tex. Every IMG_LOAD call site in the shader passes
    // img_Output, img_Accumulation, img_Bloom, img_Velocity or img_Bloom_Temp --
    // img_MaterialPacked is only ever written (IMG_STORE) and sized (IMG_SIZE),
    // and img_AlbedoHit / img_TraceGBuffer are only ever written. So t67, t68 and
    // t70 have no OpDecorate in the .spv at all, and binding descriptors for them
    // would create Vulkan bindings the shader can never reference.
    //
    // This is an EXCLUSION, not an oversight, and it is checked rather than
    // assumed: VerifySpirvBindings() (called from Init) parses the .spv this
    // backend actually loads and throws if the module's binding set and this
    // table disagree in either direction. If a future shader edit starts reading
    // img_MaterialPacked, DXC emits binding 83, the sets stop matching, and Init
    // throws -- which the caller turns into a raster fallback -- instead of the
    // descriptor silently going missing.
    //
    // DX11Nova.h:465 and DX12Nova.cpp both bind all eight; binding an unused
    // SRV is legal there, so the extra three cost nothing and hide the fact that
    // the shader never reads them.
    const uint32_t VulkanNova::kReadImageSlot[kReadImageCount] = {64, 65, 66, 69, 71};
    const uint32_t VulkanNova::kReadImageIndex[kReadImageCount] = {0, 1, 2, 5, 7};

    // NovaConstants field offsets, from fxc /dumpbin. tools/compile_nova_spirv.py
    // asserts these equal the offsets DXC gave the SPIR-V for all 84 members;
    // if that check ever fails, regenerate this table rather than editing it.
    static const VulkanNova::ConstantField kConstantFields[] = {
        {"u_CameraPosition", 0, 0},
        {"u_InverseViewProjection", 16, 0},
        {"u_PrevViewProjection", 80, 0},
        {"u_InstanceCount", 144, 0},
        {"u_FrameIndex", 148, 0},
        {"u_SamplesPerPixel", 152, 0},
        {"u_PassID", 156, 0},
        {"u_DepthBuffer", 160, 0},
        {"u_Jitter", 164, 0},
        {"u_PrevJitter", 176, 0},
        {"u_CameraMoved", 184, 0},
        {"u_ViewProjection", 192, 0},
        {"u_SkyBottomColor", 256, 0},
        {"u_SkyTopColor", 272, 0},
        {"u_AccumulationAlpha", 284, 0},
        {"u_NormalStrength", 288, 0},
        {"u_RoughnessBias", 292, 0},
        {"u_AOIntensity", 296, 0},
        {"u_QualityLevel", 300, 0},
        {"u_BloomEnabled", 304, 0},
        {"u_BloomIntensity", 308, 0},
        {"u_BloomThreshold", 312, 0},
        {"u_VignetteEnabled", 316, 0},
        {"u_VignetteIntensity", 320, 0},
        {"u_VignetteRadius", 324, 0},
        {"u_VignetteSoftness", 328, 0},
        {"u_ChromaticAberrationEnabled", 332, 0},
        {"u_ChromaticAberrationIntensity", 336, 0},
        {"u_ColorGradingEnabled", 340, 0},
        {"u_ColorTemperature", 344, 0},
        {"u_ColorTint", 348, 0},
        {"u_ColorGamma", 352, 0},
        {"u_FilmGrainEnabled", 356, 0},
        {"u_FilmGrainIntensity", 360, 0},
        {"u_MaxBounces", 364, 0},
        {"u_MaxLights", 368, 0},
        {"u_IndirectRays", 372, 0},
        {"u_StepSize", 376, 0},
        {"u_RenderScale", 380, 0},
        {"u_LightCount", 384, 0},
        {"u_NeuralEnabled", 388, 0},
        {"u_NeuralTexStrength", 392, 0},
        {"u_NeuralLightStrength", 396, 0},
        {"u_NeuralMatStrength", 400, 0},
        {"u_SunDirection", 404, 0},
        {"u_Time", 416, 0},
        {"u_FogEnabled", 420, 0},
        {"u_SunLightColor", 432, 0},
        {"u_FogCount", 444, 0},
        {"u_FogMin", 448, 4},
        {"u_FogMax", 512, 4},
        {"u_FogColor", 576, 4},
        {"u_FogData", 640, 4},
        {"u_FogData2", 704, 4},
        {"u_CloudEnabled", 768, 0},
        {"u_CloudCount", 772, 0},
        {"u_CloudMin", 784, 2},
        {"u_CloudMax", 816, 2},
        {"u_CloudColor", 848, 2},
        {"u_CloudAmbient", 880, 2},
        {"u_CloudData", 912, 2},
        {"u_CloudData2", 944, 2},
        {"u_CloudData3", 976, 2},
        {"u_VolFast", 1008, 0},
        {"u_GlobalFogEnabled", 1012, 0},
        {"u_GlobalFogDensity", 1016, 0},
        {"u_GlobalFogHeightFalloff", 1020, 0},
        {"u_GlobalFogColor", 1024, 0},
        {"u_GlobalFogBaseHeight", 1036, 0},
        {"u_GlobalFogDistAtten", 1040, 0},
        {"u_GodRaysEnabled", 1044, 0},
        {"u_GodSunScreenPos", 1048, 0},
        {"u_GodSunValid", 1056, 0},
        {"u_GodRayIntensity", 1060, 0},
        {"u_GodRayDecay", 1064, 0},
        {"u_GodRaySamples", 1068, 0},
        {"u_GodRayDensity", 1072, 0},
        {"u_GodRayColor", 1076, 0},
        {"u_ALightCount", 1088, 0},
        {"u_ALightPos", 1104, 8},
        {"u_ALightDir", 1232, 8},
        {"u_ALightColor", 1360, 8},
        {"u_ALightData", 1488, 8},
        {"u_ALightData2", 1616, 8},
    };
    static constexpr uint32_t kConstantFieldCount =
        sizeof(kConstantFields) / sizeof(kConstantFields[0]);

    const VulkanNova::ConstantField *VulkanNova::FindConstant(const char *name)
    {
        if (!name)
            return nullptr;
        for (uint32_t i = 0; i < kConstantFieldCount; ++i)
        {
            if (std::strcmp(kConstantFields[i].name, name) == 0)
                return &kConstantFields[i];
        }
        return nullptr;
    }

    template <class T>
    void VulkanNova::Set(const char *name, const T &value, uint32_t index)
    {
        const ConstantField *field = FindConstant(name);
        if (!field)
        {
            WL_CORE_ERROR("VulkanNova: unknown constant '{0}'", name);
            return;
        }
        // Every NovaConstants array has a 16-byte stride under both D3D and
        // std140 packing (verified per-member by tools/compile_nova_spirv.py).
        const uint32_t stride = field->arraySize > 0 ? 16u : 0u;
        if (field->arraySize > 0 && index >= field->arraySize)
        {
            WL_CORE_ERROR("VulkanNova: {0}[{1}] out of range (size {2})",
                          name, index, field->arraySize);
            return;
        }
        if (field->arraySize == 0 && std::strcmp(name, "u_FrameIndex") == 0)
            m_CurrentFrame = static_cast<uint32_t>(value);

        const uint32_t offset = field->offset + index * stride;
        const uint32_t size = sizeof(T);
        if (offset + size > kConstantBufferSize)
        {
            WL_CORE_ERROR("VulkanNova: {0} writes past NovaConstants ({1}+{2} > {3})",
                          name, offset, size, kConstantBufferSize);
            return;
        }
        std::memcpy(m_ConstantBytes.data() + offset, &value, size);
    }

    // -------------------------------------------------------------------------
    // SPIR-V cross-check
    // -------------------------------------------------------------------------

    // Parses the OpDecorate/OpName stream of a SPIR-V module and returns the
    // binding number declared for each named resource.
    //
    // This is the same information tools/compile_nova_spirv.py gets from
    // spirv-dis, reimplemented here so the RUNTIME can check the tables it
    // actually binds against the module it actually loaded. The tool proves the
    // committed .spv is self-consistent at build time; it cannot catch a C++
    // table that has drifted away from that .spv, and nothing else in this file
    // could. That drift is the whole failure mode: a valid descriptor set with
    // the wrong texture in it, or a binding the shader needs that NVRHI never
    // allocates.
    static std::unordered_map<uint32_t, std::string> ParseSpirvBindings(
        const std::vector<char> &spirv)
    {
        std::unordered_map<uint32_t, std::string> result;
        if (spirv.size() < 20 || spirv.size() % 4 != 0)
            return result;

        const uint32_t *words = reinterpret_cast<const uint32_t *>(spirv.data());
        const size_t wordCount = spirv.size() / 4;
        if (words[0] != 0x07230203u) // SPIR-V magic, little-endian
            return result;

        // SPIR-V opcodes used here. OpName carries a debug string, OpDecorate
        // with the Binding decoration carries the descriptor number.
        constexpr uint32_t kOpName = 5;
        constexpr uint32_t kOpDecorate = 71;
        constexpr uint32_t kDecorationBinding = 33;

        std::unordered_map<uint32_t, std::string> names;
        std::unordered_map<uint32_t, uint32_t> bindings;

        size_t i = 5; // skip the 5-word header
        while (i < wordCount)
        {
            const uint32_t instruction = words[i];
            const uint32_t opcode = instruction & 0xFFFFu;
            const uint32_t length = instruction >> 16;
            if (length == 0 || i + length > wordCount)
                break; // malformed; stop rather than run off the end

            if (opcode == kOpName && length >= 3)
            {
                std::string name;
                for (size_t w = i + 2; w < i + length; ++w)
                {
                    for (uint32_t byte = 0; byte < 4; ++byte)
                    {
                        const char c =
                            static_cast<char>((words[w] >> (8 * byte)) & 0xFFu);
                        if (c == '\0')
                        {
                            w = i + length; // terminate the outer loop too
                            break;
                        }
                        name.push_back(c);
                    }
                }
                names.emplace(words[i + 1], name);
            }
            else if (opcode == kOpDecorate && length >= 4 &&
                     words[i + 2] == kDecorationBinding)
            {
                bindings.emplace(words[i + 1], words[i + 3]);
            }

            i += length;
        }

        for (const auto &entry : bindings)
        {
            const auto name = names.find(entry.first);
            result.emplace(entry.second,
                           name != names.end() ? name->second
                                               : "<unnamed id " +
                                                     std::to_string(entry.first) + ">");
        }
        return result;
    }

    void VulkanNova::VerifySpirvBindings(const std::vector<char> &spirv) const
    {
        const std::unordered_map<uint32_t, std::string> module = ParseSpirvBindings(spirv);
        if (module.empty())
        {
            WL_CORE_ERROR("VulkanNova: SPIR-V declares no bindings (bad magic or "
                          "truncated module); cannot verify the binding contract");
            throw std::runtime_error("VulkanNova: unreadable SPIR-V binding table");
        }

        // The bindings this class's layout will create, from the same tables the
        // layout is built from. t15 is one descriptor of kSceneTextureCount
        // elements, so it contributes a single binding number.
        std::vector<uint32_t> expected;
        expected.reserve(kImageCount + 3 + 2 + 1 + kSampledCount + kReadImageCount);
        for (uint32_t i = 0; i < kImageCount; ++i)
            expected.push_back(kBindingOffsetUav + i);
        for (uint32_t i = 1; i <= 3; ++i)
            expected.push_back(kBindingOffsetShaderResource + i);
        expected.push_back(kBindingOffsetSampler);
        expected.push_back(kBindingOffsetConstantBuffer + 1);
        expected.push_back(kBindingOffsetShaderResource + 15);
        for (uint32_t slot : kSampledSlot)
            expected.push_back(kBindingOffsetShaderResource + slot);
        for (uint32_t i = 0; i < kReadImageCount; ++i)
            expected.push_back(kBindingOffsetShaderResource + kReadImageSlot[i]);

        std::sort(expected.begin(), expected.end());

        // Both directions are errors. A binding we allocate that the module does
        // not declare wastes a descriptor and hides a stale table entry; a
        // binding the module declares that we do not allocate is the descriptor
        // the shader reads and gets nothing.
        std::string missing;   // in the layout, absent from the module
        std::string unexpected; // in the module, absent from the layout
        size_t moduleIndex = 0;
        std::vector<uint32_t> moduleBindings;
        moduleBindings.reserve(module.size());
        for (const auto &entry : module)
            moduleBindings.push_back(entry.first);
        std::sort(moduleBindings.begin(), moduleBindings.end());

        size_t expectedIndex = 0;
        while (expectedIndex < expected.size() || moduleIndex < moduleBindings.size())
        {
            const bool haveExpected = expectedIndex < expected.size();
            const bool haveModule = moduleIndex < moduleBindings.size();
            if (haveExpected && (!haveModule || expected[expectedIndex] < moduleBindings[moduleIndex]))
            {
                if (!missing.empty())
                    missing += ", ";
                missing += std::to_string(expected[expectedIndex]);
                ++expectedIndex;
            }
            else if (haveModule && (!haveExpected || moduleBindings[moduleIndex] < expected[expectedIndex]))
            {
                if (!unexpected.empty())
                    unexpected += ", ";
                unexpected += std::to_string(moduleBindings[moduleIndex]) + " (" +
                              module.at(moduleBindings[moduleIndex]) + ")";
                ++moduleIndex;
            }
            else
            {
                if (expected.size() > 1 &&
                    std::count(expected.begin(), expected.end(), expected[expectedIndex]) > 1)
                {
                    WL_CORE_ERROR("VulkanNova: binding {0} is claimed twice by the "
                                  "layout tables", expected[expectedIndex]);
                    throw std::runtime_error("VulkanNova: duplicate descriptor binding");
                }
                ++expectedIndex;
                ++moduleIndex;
            }
        }

        if (!missing.empty() || !unexpected.empty())
        {
            WL_CORE_ERROR("VulkanNova: SPIR-V binding table does not match the C++ "
                          "tables. Unused by the shader: [{0}]. Read by the shader "
                          "but never bound: [{1}]. Re-run "
                          "tools/compile_nova_spirv.py and update the slot tables.",
                          missing, unexpected);
            throw std::runtime_error("VulkanNova: SPIR-V/C++ binding table mismatch");
        }

        WL_CORE_INFO("VulkanNova: SPIR-V binding cross-check passed ({0} bindings, "
                     "offsets u={1} b={2} t={3} s={4})",
                     module.size(), kBindingOffsetUav, kBindingOffsetConstantBuffer,
                     kBindingOffsetShaderResource, kBindingOffsetSampler);
    }

    // -------------------------------------------------------------------------
    // Initialisation
    // -------------------------------------------------------------------------

    bool VulkanNova::EnsureCommandList()
    {
        auto *ctx = NVRHIRendererAPI::GetCurrentContext();
        if (!ctx)
            return false;
        m_Context = ctx;
        m_Device = ctx->GetDevice();
        m_CommandList = ctx->GetCommandList();
        return m_Device != nullptr && m_CommandList != nullptr;
    }

    void VulkanNova::Init()
    {
        if (!EnsureCommandList())
            throw std::runtime_error("VulkanNova: no NVRHI device or command list");

        // The SPIR-V is a build artifact, not a source file. Regenerate with
        // tools/compile_nova_spirv.py before running the Vulkan backend.
        const std::filesystem::path spvPath =
            Project::Get().GetAssetPath("shaders/Renderer3D_NovaRenderer.spv");

        std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("VulkanNova: SPIR-V not found: " + spvPath.string());
        const std::streamsize size = file.tellg();
        file.seekg(0);
        std::vector<char> spirv(static_cast<size_t>(size));
        if (!file.read(spirv.data(), size))
            throw std::runtime_error("VulkanNova: failed to read SPIR-V");

        // Check the tables in this file against the module that was just read,
        // BEFORE any Vulkan object is created from them. See VerifySpirvBindings.
        VerifySpirvBindings(spirv);

        nvrhi::ShaderDesc shaderDesc;
        shaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shaderDesc.debugName = "Nova_SPIRV";
        m_ComputeShader = m_Device->createShader(shaderDesc, spirv.data(), spirv.size());
        if (!m_ComputeShader)
            throw std::runtime_error("VulkanNova: createShader failed");

        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
        samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
        samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
        samplerDesc.maxAnisotropy = 1.f;
        m_Sampler = m_Device->createSampler(samplerDesc);

        m_ConstantBytes.assign(kConstantBufferSize, 0);
        nvrhi::BufferDesc cbDesc;
        cbDesc.byteSize = kConstantBufferSize;
        cbDesc.isConstantBuffer = true;
        cbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        cbDesc.keepInitialState = true;
        cbDesc.debugName = "NovaConstants";
        m_ConstantBuffer = m_Device->createBuffer(cbDesc);

        for (int i = 0; i < 3; ++i)
            m_StructuredBuffers[i] = nullptr;

        CreatePipelineAndLayout();
        m_Valid = true;
    }

    void VulkanNova::CreatePipelineAndLayout()
    {
        // The offsets must equal the -fvk-*-shift values baked into the SPIR-V.
        // See the header comment and tools/compile_nova_spirv.py.
        nvrhi::VulkanBindingOffsets offsets;
        offsets.setShaderResourceOffset(kBindingOffsetShaderResource)
            .setConstantBufferOffset(kBindingOffsetConstantBuffer)
            .setUnorderedAccessViewOffset(kBindingOffsetUav)
            .setSamplerOffset(kBindingOffsetSampler);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindingOffsets = offsets;

        // UAV images, u0..u7.
        for (uint32_t i = 0; i < kImageCount; ++i)
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(i));
        // Structured buffers: Instances t1, BVHNodes t2, LightIndices t3.
        for (uint32_t i = 1; i <= 3; ++i)
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(i));
        // Sampler s0 and NovaConstants b1.
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(0));
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(1));
        // Scene texture array t15[32].
        {
            nvrhi::BindingLayoutItem item = nvrhi::BindingLayoutItem::Texture_SRV(15);
            item.size = kSceneTextureCount;
            layoutDesc.bindings.push_back(item);
        }
        // Sampled snapshots t48..t52.
        for (uint32_t slot : kSampledSlot)
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        // Read-only snapshots of our own images, t64/65/66/69/71.
        for (uint32_t i = 0; i < kReadImageCount; ++i)
            layoutDesc.bindings.push_back(
                nvrhi::BindingLayoutItem::Texture_SRV(kReadImageSlot[i]));

        m_BindingLayout = m_Device->createBindingLayout(layoutDesc);
        if (!m_BindingLayout)
            throw std::runtime_error("VulkanNova: createBindingLayout failed");

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = m_ComputeShader;
        pipelineDesc.addBindingLayout(m_BindingLayout);
        m_Pipeline = m_Device->createComputePipeline(pipelineDesc);
        if (!m_Pipeline)
            throw std::runtime_error("VulkanNova: createComputePipeline failed");
    }

    void VulkanNova::LogBindingContract() const
    {
        if (!m_BindingLayout)
            return;
        WL_CORE_INFO("VulkanNova binding contract: offsets u={0} b={1} t={2} s={3}; "
                     "{4} UAV + 3 SSBO + 1 CB + 1 sampler + 1x{5} scene SRV + "
                     "{6} sampled SRV (t48 = history) + {7} read SRV = {8} bindings, "
                     "all distinct; t67/t68/t70 are declared in the .hlsl but "
                     "eliminated by DXC, and Init cross-checked that against the "
                     "loaded module",
                     kBindingOffsetUav, kBindingOffsetConstantBuffer,
                     kBindingOffsetShaderResource, kBindingOffsetSampler, kImageCount,
                     kSceneTextureCount, kSampledCount, kReadImageCount,
                     kImageCount + 3 + 1 + 1 + 1 + kSampledCount + kReadImageCount);
    }

    // -------------------------------------------------------------------------
    // Size-dependent images
    // -------------------------------------------------------------------------

    void VulkanNova::Resize(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
            return;
        if (w == m_Width && h == m_Height && m_Valid)
            return;
        m_Width = w;
        m_Height = h;
        CreateImages(w, h);
        CreateBindingSet();
        if (!m_LoggedBindingContract)
        {
            LogBindingContract();
            m_LoggedBindingContract = true;
        }
    }

    void VulkanNova::CreateImages(uint32_t w, uint32_t h)
    {
        static const nvrhi::Format formats[kImageCount] = {
            nvrhi::Format::RGBA32_FLOAT, // img_Output        u0
            nvrhi::Format::RGBA32_FLOAT, // img_Accumulation  u1
            nvrhi::Format::RGBA32_FLOAT, // img_Bloom         u2
            nvrhi::Format::RGBA8_UNORM,     // img_MaterialPacked u3
            nvrhi::Format::RGBA32_FLOAT, // img_AlbedoHit     u4
            nvrhi::Format::RG32_FLOAT,       // img_Velocity      u5
            nvrhi::Format::RGBA32_FLOAT, // img_TraceGBuffer  u6
            nvrhi::Format::RGBA32_FLOAT, // img_Bloom_Temp    u7
        };

        for (uint32_t i = 0; i < kImageCount; ++i)
        {
            nvrhi::TextureDesc desc;
            desc.width = w;
            desc.height = h;
            desc.depth = 1;
            desc.arraySize = 1;
            desc.mipLevels = 1;
            desc.sampleCount = 1;
            desc.format = formats[i];
            desc.isShaderResource = true;
            desc.isRenderTarget = false;
            // Without initialState + keepInitialState NVRHI reports an unknown
            // prior state, which on D3D12 can cascade into device removal.
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.keepInitialState = true;
            desc.debugName = "NovaImage";
            m_Images[i].texture = m_Device->createTexture(desc);

            // A texture bound as a UAV cannot also be read as an SRV in the
            // same dispatch, so the read_* bindings sample a copy. This mirrors
            // DX12Nova's snapshot textures.
            nvrhi::TextureDesc snapDesc = desc;
            snapDesc.initialState = nvrhi::ResourceStates::CopyDest;
            snapDesc.debugName = "NovaSnapshot";
            m_Images[i].snapshot = m_Device->createTexture(snapDesc);

            if (!m_Images[i].texture || !m_Images[i].snapshot)
                throw std::runtime_error("VulkanNova: failed to create pass images");
        }

        // Temporal history. This is the texture the temporal reprojection pass
        // reads at t48 (s_Accumulation), and the only thing CommitHistory()
        // writes -- see the comment on that method for why it is a separate
        // texture and not m_Images[1].
        //
        // It replaces a second "m_HistoryImage" that used to sit here: nothing
        // ever copied INTO it, nothing bound it, and Clear() zeroed a texture
        // no pass could observe. It was a create/clear/destroy cycle with no
        // reader, i.e. the same class of dead resource this binding fixes, so it
        // is gone rather than left behind.
        nvrhi::TextureDesc historyDesc;
        historyDesc.width = w;
        historyDesc.height = h;
        historyDesc.depth = 1;
        historyDesc.arraySize = 1;
        historyDesc.mipLevels = 1;
        historyDesc.sampleCount = 1;
        historyDesc.format = nvrhi::Format::RGBA32_FLOAT;
        historyDesc.isShaderResource = true;
        // Both bits, not just CopyDest: CommitHistory() copies into this texture
        // (needs CopyDest) and CreateBindingSet() binds it as an SRV (needs
        // ShaderResource). keepInitialState makes NVRHI pin it to exactly these
        // bits and verify every use against them via
        // verifyPermanentResourceState, so declaring only one of the two makes
        // the other use an error at binding-set creation.
        historyDesc.initialState = nvrhi::ResourceStates::CopyDest |
                                   nvrhi::ResourceStates::ShaderResource;
        historyDesc.keepInitialState = true;
        historyDesc.debugName = "NovaHistory";
        m_HistorySnapshot = m_Device->createTexture(historyDesc);
    }

    void VulkanNova::CreateBindingSet()
    {
        nvrhi::BindingSetDesc desc;

        // UAV images, u0..u7.
        for (uint32_t i = 0; i < kImageCount; ++i)
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i, m_Images[i].texture));

        // Instances t1, BVHNodes t2, LightIndices t3. There is no
        // StructuredBuffer_SRV factory, so build the item directly.
        for (uint32_t i = 0; i < 3; ++i)
        {
            nvrhi::BindingSetItem item{};
            item.resourceHandle = m_StructuredBuffers[i];
            item.slot = i + 1;
            item.type = nvrhi::ResourceType::StructuredBuffer_SRV;
            desc.bindings.push_back(item);
        }

        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, m_Sampler));
        desc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer));

        // u_SceneTextures is t15[32]: one binding, arrayElement walks the
        // array, so push all 32 rather than resizing the layout.
        for (uint32_t i = 0; i < kSceneTextureCount; ++i)
        {
            nvrhi::BindingSetItem item = nvrhi::BindingSetItem::None(15);
            item.resourceHandle = i < m_SceneSRVs.size() ? m_SceneSRVs[i] : nullptr;
            item.type = nvrhi::ResourceType::Texture_SRV;
            item.arrayElement = i;
            desc.bindings.push_back(item);
        }

        // t48 is s_Accumulation, and the shader uses it as the TEMPORAL HISTORY,
        // not as this frame's accumulation: the reprojection pass reads
        //     float3 historyLight = TEX(s_Accumulation, histUV).rgb;
        // at a reprojected UV, so it must be LAST frame's committed
        // accumulation. CommitHistory() is what puts it there, and
        // DX11Nova.h:456 binds t48 to exactly that texture (history.srv, an SRV
        // over history.snapshot) for the same reason.
        //
        // It cannot be m_Images[1].texture: that is the img_Accumulation UAV
        // (u1) in this same descriptor set, and Vulkan forbids a resource being
        // written and sampled in one dispatch. m_Images[1].snapshot would be
        // legal but is a different contract -- it holds whatever the pass order
        // happened to leave there, and nothing guarantees it is the previous
        // frame's result.
        //
        // This binding did not exist before: the slot was filled from
        // m_SceneSRVs[0], i.e. the caller's first scene texture, which has no
        // relationship to Nova's accumulation. That is the defect.
        if (m_HistorySnapshot)
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                kSampledSlot[0], m_HistorySnapshot));
        }

        // s_DepthBuffer / s_Output / s_NormalBuffer / s_InputAlbedo (t49..t52)
        // still come from the caller's scene texture list. k starts at 1 so t48
        // is left to the history binding above; the remaining four keep exactly
        // the source they had before this change.
        for (uint32_t k = 1; k < kSampledCount; ++k)
        {
            const uint32_t slot = kSampledSlot[k];
            nvrhi::BindingSetItem item = nvrhi::BindingSetItem::None(slot);
            item.resourceHandle = k < m_SceneSRVs.size() ? m_SceneSRVs[k] : nullptr;
            item.type = nvrhi::ResourceType::Texture_SRV;
            if (item.resourceHandle)
                desc.bindings.push_back(item);
        }

        // Read-only snapshots of our own images, t64/65/66/69/71. These are
        // separate textures: a texture bound as a UAV cannot also be sampled.
        for (uint32_t i = 0; i < kReadImageCount; ++i)
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                kReadImageSlot[i], m_Images[kReadImageIndex[i]].snapshot));
        }

        m_BindingSet = m_Device->createBindingSet(desc, m_BindingLayout);
        if (!m_BindingSet)
            throw std::runtime_error("VulkanNova: createBindingSet failed");
    }

    // -------------------------------------------------------------------------
    // Per-frame work
    // -------------------------------------------------------------------------

    void VulkanNova::EnsureStructuredBuffer(uint32_t slot, uint32_t byteSize, uint32_t stride)
    {
        if (slot > 2 || byteSize == 0)
            return;
        if (m_StructuredBuffers[slot] && m_StructuredBufferSize[slot] >= byteSize &&
            m_StructuredBufferStride[slot] == stride)
            return;

        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.structStride = stride;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "NovaStructured";
        m_StructuredBuffers[slot] = m_Device->createBuffer(desc);
        m_StructuredBufferSize[slot] = byteSize;
    }

    void VulkanNova::Upload(uint32_t slot, const void *data, uint32_t count, uint32_t stride)
    {
        if (slot > 2 || !data || count == 0)
            return;
        const uint32_t byteSize = count * stride;
        m_StructuredBufferStride[slot] = stride;
        EnsureStructuredBuffer(slot, byteSize, stride);
        if (!m_StructuredBuffers[slot])
            return;
        if (!EnsureCommandList())
            return;

        // The engine's BeginFrame()/ExecuteNVRHICommandList() own the list in
        // the batched path. Opening or closing per upload would overwrite the
        // active list and drop earlier draws, so only record transiently when
        // we are genuinely outside a frame.
        const bool batched = m_Context && m_Context->IsCommandListOpen();
        if (!batched)
        {
            m_CommandList->open();
            m_CommandList->writeBuffer(m_StructuredBuffers[slot], data, byteSize, 0);
            m_CommandList->close();
            if (m_Device)
                m_Device->executeCommandList(m_CommandList);
            return;
        }
        m_CommandList->writeBuffer(m_StructuredBuffers[slot], data, byteSize, 0);
    }

    void VulkanNova::RecordCopy(nvrhi::ITexture *dst, nvrhi::ITexture *src)
    {
        if (!dst || !src || !EnsureCommandList())
            return;
        if (!m_Context->IsCommandListOpen())
            return;
        m_CommandList->copyTexture(dst, nvrhi::TextureSlice(), src, nvrhi::TextureSlice());
    }

    void VulkanNova::CopyImagesToSnapshots()
    {
        for (uint32_t i = 0; i < kReadImageCount; ++i)
        {
            const uint32_t img = kReadImageIndex[i];
            RecordCopy(m_Images[img].snapshot, m_Images[img].texture);
        }
    }

    nvrhi::ITexture *VulkanNova::ResolveSceneTexture(Texture2D *texture)
    {
        auto *nvrhiTex = dynamic_cast<NVRHITexture2D *>(texture);
        if (!nvrhiTex || !nvrhiTex->GetTexture())
            return nullptr;
        nvrhi::ITexture *handle = nvrhiTex->GetTexture();
        auto it = m_SceneSRVCache.find(handle);
        if (it != m_SceneSRVCache.end())
            return it->second;
        m_SceneSRVCache[handle] = nvrhiTex->GetTexture();
        return handle;
    }

    void VulkanNova::Dispatch(int pass, uint32_t w, uint32_t h,
                              const std::vector<Ref<Texture2D>> &textures)
    {
        if (!m_Valid || !m_Pipeline || w == 0 || h == 0)
            return;
        if (!EnsureCommandList())
            return;

        // The pass id is selected by uniform, not by pipeline.
        Set("u_PassID", pass);

        // See Upload(): never open/close inside the batched frame.
        if (!m_Context->IsCommandListOpen())
        {
            WL_CORE_WARN("VulkanNova: dispatch recorded outside a frame; "
                         "is BeginFrame() running?");
            return;
        }

        // Once per frame: rebind the scene textures and refresh the snapshots.
        if (m_BoundFrame != m_CurrentFrame)
        {
            bool sceneChanged = false;
            for (uint32_t i = 0; i < textures.size() && i < kSceneTextureCount; ++i)
            {
                nvrhi::ITexture *texture = ResolveSceneTexture(textures[i].get());
                if (i < m_SceneSRVs.size() && m_SceneSRVs[i] != texture)
                {
                    m_SceneSRVs[i] = texture;
                    sceneChanged = true;
                }
            }
            if (sceneChanged || !m_BindingSet)
            {
                CreateBindingSet();
                CopyImagesToSnapshots();
            }
            m_BoundFrame = m_CurrentFrame;
        }

        m_CommandList->writeBuffer(m_ConstantBuffer, m_ConstantBytes.data(),
                                   m_ConstantBytes.size(), 0);

        nvrhi::ComputeState state;
        state.pipeline = m_Pipeline;
        state.bindings.push_back(m_BindingSet);
        m_CommandList->setComputeState(state);
        m_CommandList->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    void VulkanNova::CommitHistory()
    {
        // Fold this frame's accumulation into the history texture that t48 reads,
        // so next frame's pass 2 reprojects it. This is the write half of the
        // t48 binding in CreateBindingSet, and it is why the history is a
        // separate texture: m_Images[1] is the img_Accumulation UAV in the same
        // descriptor set, so it cannot also be the sampled history.
        // Mirrors DX11Nova.h:496, which copies images[1] into history.snapshot.
        RecordCopy(m_HistorySnapshot, m_Images[1].texture);
    }

    void VulkanNova::CommitDenoise()
    {
        // Fold the bilateral-blur result back into accumulation.
        RecordCopy(m_Images[1].texture, m_Images[7].texture);
    }

    void VulkanNova::Clear()
    {
        if (!EnsureCommandList() || !m_Context->IsCommandListOpen())
            return;
        for (uint32_t i = 0; i < kImageCount; ++i)
        {
            if (!m_Images[i].texture)
                continue;
            m_CommandList->clearTextureFloat(m_Images[i].texture,
                                             nvrhi::AllSubresources,
                                             nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
        }
        if (m_HistorySnapshot)
        {
            // This is now the texture bound at t48, so it is the one a reset has
            // to reach: zeroing some other history texture would leave the
            // reprojection pass blending against the previous view's
            // accumulation and smearing it across the cut.
            m_CommandList->clearTextureFloat(m_HistorySnapshot,
                                             nvrhi::AllSubresources,
                                             nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
        }
        // Snapshots and the scene bindings are refreshed on the next frame's
        // first Dispatch, so no per-slot state has to be reset here.
        m_BoundFrame = 0xFFFFFFFFu;
    }

    void *VulkanNova::Output(uint32_t index) const
    {
        if (index >= kImageCount || !m_Images[index].texture)
            return nullptr;
        return m_Images[index].texture;
    }

    VulkanNova::~VulkanNova()
    {
        m_BindingSet = nullptr;
        m_Pipeline = nullptr;
        m_BindingLayout = nullptr;
        m_ComputeShader = nullptr;
        for (auto &image : m_Images)
        {
            image.snapshot = nullptr;
            image.texture = nullptr;
        }
        m_HistorySnapshot = nullptr;
        for (int i = 0; i < 3; ++i)
            m_StructuredBuffers[i] = nullptr;
        m_ConstantBuffer = nullptr;
        m_Sampler = nullptr;
        m_Valid = false;
    }

} // namespace Wasteland