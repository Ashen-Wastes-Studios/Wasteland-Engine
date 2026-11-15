#include "wlpch.h"

#include "Platform/NVRHI/VulkanNova.h"
#include "Platform/NVRHI/NVRHIContext.h"
#include "Platform/NVRHI/NVRHIRendererAPI.h"
#include "Platform/NVRHI/NVRHITexture.h"

#include "Wasteland/Core/Log.h"
#include "Wasteland/Project/Project.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

    // Internal image index -> HLSL UAV register. u8 is unused by the shader;
    // image 8 is the moments texture at u9. The WLFG reactive mask (img_Mask)
    // was moved from the u8 hole to u14 to avoid a Vulkan binding-space
    // collision with NovaConstants at b1+shift7=8.
    // index 10 is WLRR's pre-SVGF snapshot (img_WlrrInput) at u15.
    const uint32_t VulkanNova::kImageUavSlot[kImageCount] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 14, 15};

    // HLSL register slots for active read_* SRVs and the image each snapshots.
    // ReSTIR reads AlbedoHit (t68) and TraceGBuffer (t70); temporal/filter passes
    // also read the other listed images. MaterialPacked's t67 alias remains
    // declaration-only because the shader has no IMG_LOAD(img_MaterialPacked).
    // Keep this list synchronized with the loaded SPIR-V: VerifySpirvBindings()
    // checks both missing and unused bindings before Vulkan creates the pipeline.
    // t77 (image 10) is WLRR's pre-SVGF snapshot, read by pass 21 as the
    // "input" arm. t67 stays declaration-only (no IMG_LOAD of MaterialPacked).
    const uint32_t VulkanNova::kReadImageSlot[kReadImageCount] = {64, 65, 66, 68, 69, 70, 71, 73, 77};
    const uint32_t VulkanNova::kReadImageIndex[kReadImageCount] = {0, 1, 2, 4, 5, 6, 7, 8, 10};

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
        {"u_Jitter", 168, 0},
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
        {"u_ReconstructionFilter", 388, 0},
        {"u_SamplePattern", 392, 0},
        {"u_NeuralEnabled", 396, 0},
        {"u_NeuralTexStrength", 400, 0},
        {"u_NeuralLightStrength", 404, 0},
        {"u_NeuralMatStrength", 408, 0},
        {"u_SunDirection", 416, 0},
        {"u_Time", 428, 0},
        {"u_FogEnabled", 432, 0},
        {"u_SunLightColor", 448, 0},
        {"u_FogCount", 460, 0},
        {"u_FogMin", 464, 0},
        {"u_FogMax", 528, 0},
        {"u_FogColor", 592, 0},
        {"u_FogData", 656, 0},
        {"u_FogData2", 720, 0},
        {"u_CloudEnabled", 784, 0},
        {"u_CloudCount", 788, 0},
        {"u_CloudMin", 800, 0},
        {"u_CloudMax", 832, 0},
        {"u_CloudColor", 864, 0},
        {"u_CloudAmbient", 896, 0},
        {"u_CloudData", 928, 0},
        {"u_CloudData2", 960, 0},
        {"u_CloudData3", 992, 0},
        {"u_VolFast", 1024, 0},
        {"u_VolStepCap", 1028, 0},
        {"u_VolLightCap", 1032, 0},
        {"u_VolRangeCull", 1036, 0},
        {"u_GlobalFogEnabled", 1040, 0},
        {"u_GlobalFogDensity", 1044, 0},
        {"u_GlobalFogHeightFalloff", 1048, 0},
        {"u_GlobalFogColor", 1056, 0},
        {"u_GlobalFogBaseHeight", 1068, 0},
        {"u_GlobalFogDistAtten", 1072, 0},
        {"u_GodRaysEnabled", 1076, 0},
        {"u_GodSunScreenPos", 1080, 0},
        {"u_GodSunValid", 1088, 0},
        {"u_GodRayIntensity", 1092, 0},
        {"u_GodRayDecay", 1096, 0},
        {"u_GodRaySamples", 1100, 0},
        {"u_GodRayDensity", 1104, 0},
        {"u_GodRayColor", 1120, 0},
        {"u_ALightCount", 1132, 0},
        {"u_ALightPos", 1136, 0},
        {"u_ALightDir", 1648, 0},
        {"u_ALightColor", 2160, 0},
        {"u_ALightData", 2672, 0},
        {"u_ALightData2", 3184, 0},
        {"u_TSREnabled", 3696, 0},
        {"u_TSRSharpenStrength", 3700, 0},
        {"u_RCSEnabled", 3704, 0},
        // Phase 1 hybrid path. Vulkan has no raster visibility pass, so this is
        // always 0 here and the shader never reads the t78..t81 SRVs.
        {"u_UseRasterVisibility", 3708, 0},
        {"u_EffectsFlags", 3712, 0},
        {"u_DecalCount", 3716, 0},
        {"u_DecalPos", 3728, 0},
        {"u_DecalRight", 3856, 0},
        {"u_DecalUp", 3984, 0},
        {"u_DecalDir", 4112, 0},
        {"u_DecalSize", 4240, 0},
        {"u_DecalColor", 4368, 0},
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

    void VulkanNova::SetBytes(const char *name, const void *value, std::size_t size, uint32_t index)
    {
        // THE CALLER'S KEY IS NOT THE TABLE'S KEY.
        //
        // Renderer3D addresses cbuffer members the way Slang names them --
        // "NovaConstants_0.u_InstanceCount_0" -- while this table stores the source-level
        // "u_InstanceCount". An exact-match lookup therefore missed instance count, frame
        // index, camera matrices, pass id and resolution: every pass reported success over
        // a zeroed constant buffer and the frame came out flat. DX12Nova::Set has the same
        // normalisation for the same reason.
        std::string keyStorage;
        if (name)
        {
            keyStorage.assign(name);
            const std::size_t dot = keyStorage.rfind('.');
            if (dot != std::string::npos)
                keyStorage.erase(0, dot + 1);
            const std::size_t underscore = keyStorage.rfind('_');
            if (underscore != std::string::npos && underscore + 1 < keyStorage.size())
            {
                bool allDigits = true;
                for (std::size_t i = underscore + 1; i < keyStorage.size(); ++i)
                {
                    if (keyStorage[i] < '0' || keyStorage[i] > '9') { allDigits = false; break; }
                }
                // A bare identifier that ends in digits (u_Foo2) must survive, so the
                // suffix is only dropped when it is preceded by a letter and separated by
                // an underscore -- which is what an entry id looks like.
                if (allDigits)
                    keyStorage.erase(underscore);
            }
            name = keyStorage.c_str();
        }

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
        if (field->arraySize == 0 && std::strcmp(name, "u_FrameIndex") == 0 &&
            size == sizeof(uint32_t))
        {
            uint32_t frameIndex = 0;
            std::memcpy(&frameIndex, value, sizeof(frameIndex));
            m_CurrentFrame = frameIndex;
        }

        const uint32_t offset = field->offset + index * stride;
        if (offset + size > kConstantBufferSize)
        {
            WL_CORE_ERROR("VulkanNova: {0} writes past NovaConstants ({1}+{2} > {3})",
                          name, offset, size, kConstantBufferSize);
            return;
        }
        std::memcpy(m_ConstantBytes.data() + offset, value, size);
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
        expected.reserve(kImageCount + 3 + 2 + 1 + kSampledCount + kReadImageCount + 2);
        for (uint32_t i = 0; i < kImageCount; ++i)
            expected.push_back(kBindingOffsetUav + kImageUavSlot[i]);
        // Reservoir write at u10 (shader: RWStructuredBuffer : register(u10))
        expected.push_back(kBindingOffsetUav + 10);
        for (uint32_t i = 1; i <= 3; ++i)
            expected.push_back(kBindingOffsetShaderResource + i);
        // Reservoir read at t9 (was t10, changed to avoid SSBO namespace
        // collision with ReservoirWrite at u10 in GLSL with zero shifts)
        expected.push_back(kBindingOffsetShaderResource + 9);
        expected.push_back(kBindingOffsetSampler);
        expected.push_back(kBindingOffsetConstantBuffer + 1);
        expected.push_back(kBindingOffsetShaderResource + 15);
        for (uint32_t slot : kSampledSlot)
            expected.push_back(kBindingOffsetShaderResource + slot);
        for (uint32_t i = 0; i < kReadImageCount; ++i)
            expected.push_back(kBindingOffsetShaderResource + kReadImageSlot[i]);
        // The neural/terrain/WLFG inputs the module also reads. Offsets are the same
        // arithmetic NVRHI uses (offset + slot), so these are u12, u13, t4 and t72.
        expected.push_back(kBindingOffsetUav + kNeuralGlobalsSlot);
        expected.push_back(kBindingOffsetUav + kNeuralWeightsSlot);
        expected.push_back(kBindingOffsetShaderResource + kTerrainVertexSlot);
        expected.push_back(kBindingOffsetShaderResource + kWlfgHistorySlot);
        expected.push_back(kBindingOffsetShaderResource + kWlfgWeightsSlot);
        expected.push_back(kBindingOffsetShaderResource + kWlrrWeightsSlot);
        // Phase 1 hybrid visibility G-buffer (t78..t81). Declared by the module;
        // bound to stand-ins because this backend has no raster visibility pass.
        expected.push_back(kBindingOffsetShaderResource + kVisibilityIdsSlot);
        expected.push_back(kBindingOffsetShaderResource + kVisibilityAlbedoSlot);
        expected.push_back(kBindingOffsetShaderResource + kVisibilityNormalSrvSlot);
        expected.push_back(kBindingOffsetShaderResource + kVisibilityDepthSrvSlot);

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

        // Before the layout: the descriptor set is built from these, and the layout must
        // already contain their slots (VerifySpirvBindings enforces that both lists agree
        // with the module).
        CreateFeatureInputs();

        CreatePipelineAndLayout();
        m_Valid = true;

        // The WLFG synthesis head's weights (t75) are read from disk once, HERE rather
        // than lazily from Dispatch. Dispatch runs inside a frame, and the upload may
        // need to open, close and submit the command list -- which would overwrite the
        // list BeginFrame() owns and drop everything already recorded into it. At Init
        // no frame is in flight, so the transient upload path is the correct one.
        LoadWlfgWeights();
        LoadWlrrWeights();
    }

    // Resources for the bindings the module declares but no pass of this backend
    // populates (see the note on the slots in the header). Created once, at Init:
    // their size does not depend on the render target.
    void VulkanNova::CreateFeatureInputs()
    {
        // NeuralGlobals: the reduce stage writes sixteen floats into it (u12 is an
        // RWStructuredBuffer in the shader). One element of 16 floats is enough for a
        // disabled neural stage, and it keeps the UAV a real buffer.
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = 64;
            desc.structStride = 16;
            // No isShaderResource/isUAV on nvrhi::BufferDesc: a buffer's usage is
            // described by its state. UnorderedAccess covers the UAV write, and
            // ShaderResource the read side.
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess |
                                nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "NovaNeuralGlobals";
            m_NeuralGlobals = m_Device->createBuffer(desc);
            if (!m_NeuralGlobals)
                throw std::runtime_error("VulkanNova: could not create NeuralGlobals buffer");
        }

        // NeuralWeights: read-only float buffer. The real weights live in the neural
        // stage's upload path, which this backend does not run, so the descriptor points
        // at a single element that no enabled branch ever indexes past.
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = 4096;
            desc.structStride = 4;
            // Both states: the module declares NeuralWeights as a storage buffer, and
            // NVRHI rejects a descriptor whose permanent state does not cover the access
            // the layout item implies (validation: "Binding 13, NeuralWeights is
            // invalid" on every dispatch).
            desc.initialState = nvrhi::ResourceStates::ShaderResource |
                                nvrhi::ResourceStates::UnorderedAccess;
            desc.keepInitialState = true;
            desc.debugName = "NovaNeuralWeights";
            m_NeuralWeights = m_Device->createBuffer(desc);
            if (!m_NeuralWeights)
                throw std::runtime_error("VulkanNova: could not create NeuralWeights buffer");
        }

        // TerrainVertices: same shape as a real terrain upload (packed 4-float stride).
        // Zero instances of terrain are submitted to this backend, so the buffer only
        // has to exist and be shader-readable.
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = 64;
            desc.structStride = 16;
            desc.initialState = nvrhi::ResourceStates::ShaderResource |
                                nvrhi::ResourceStates::UnorderedAccess;
            desc.keepInitialState = true;
            desc.debugName = "NovaTerrainVertices";
            m_TerrainVertices = m_Device->createBuffer(desc);
            if (!m_TerrainVertices)
                throw std::runtime_error("VulkanNova: could not create terrain vertex buffer");
        }

        // WlfgWeights (t75): the pass-23 synthesis head's weights, kWlfgWeightCount
        // fp32 values. Created ZEROED and left that way unless LoadWlfgWeights() finds a
        // valid blob -- a zeroed weight vector makes every matmul zero, so gate*residual
        // is 0 and pass 23 is exactly the identity. That is the acceptance control:
        // WL_WLFG=1 with no blob must be byte-identical to WL_WLFG=0.
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = kWlfgWeightCount * sizeof(float);
            desc.structStride = sizeof(float);
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "NovaWlfgWeights";
            m_WlfgWeights = m_Device->createBuffer(desc);
            if (!m_WlfgWeights)
                throw std::runtime_error("VulkanNova: could not create WlfgWeights buffer");
        }

        // WlrrWeights (t76): the pass-21 residual-correcting denoiser's payload --
        // weights (524) | mean (9) | std (9) | blend alpha (3) = 545 fp32. Same
        // zeroed-by-default contract as t75: a zeroed payload makes every matmul
        // zero AND alpha zero, so pass 21 degenerates to `out == incumbent` (the
        // blend with alpha=0 IS the incumbent). That is the acceptance control.
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = kWlrrPayloadCount * sizeof(float);
            desc.structStride = sizeof(float);
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "NovaWlrrWeights";
            m_WlrrWeights = m_Device->createBuffer(desc);
            if (!m_WlrrWeights)
                throw std::runtime_error("VulkanNova: could not create WlrrWeights buffer");
        }

        // s_WlfgHistory: one RGBA8 texel. WLFG is off in this backend; the descriptor
        // still has to be a valid sampled image for the module's declaration.
        {
            nvrhi::TextureDesc desc;
            desc.width = 1;
            desc.height = 1;
            desc.depth = 1;
            desc.arraySize = 1;
            desc.mipLevels = 1;
            desc.sampleCount = 1;
            desc.format = nvrhi::Format::RGBA8_UNORM;
            desc.isShaderResource = true;
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            desc.debugName = "NovaWlfgHistory";
            m_WlfgHistory = m_Device->createTexture(desc);
            if (!m_WlfgHistory)
                throw std::runtime_error("VulkanNova: could not create WLFG history texture");

            // t78..t81 stand-ins (Phase 1 hybrid visibility). The SPIR-V declares
            // them, so they must exist and point at a real resource even though
            // u_UseRasterVisibility is always 0 on this backend. Formats match
            // the shader's declared sampled types exactly.
            auto makeVisDummy = [&](nvrhi::Format format, const char *name) {
                nvrhi::TextureDesc d;
                d.width = 1; d.height = 1; d.depth = 1; d.arraySize = 1;
                d.mipLevels = 1; d.sampleCount = 1;
                d.format = format;
                d.isShaderResource = true;
                d.initialState = nvrhi::ResourceStates::ShaderResource;
                d.keepInitialState = true;
                d.debugName = name;
                return m_Device->createTexture(d);
            };
            m_VisDummyInt = makeVisDummy(nvrhi::Format::R32_SINT, "NovaVisIds");
            m_VisDummyFloat4 = makeVisDummy(nvrhi::Format::RGBA8_UNORM, "NovaVisFloat4");
            m_VisDummyFloat = makeVisDummy(nvrhi::Format::R32_FLOAT, "NovaVisFloat");
            if (!m_VisDummyInt || !m_VisDummyFloat4 || !m_VisDummyFloat)
                throw std::runtime_error("VulkanNova: could not create visibility stand-in textures");
        }
    }

    bool VulkanNova::Readback(uint32_t index, std::vector<unsigned char> &rgba,
                              uint32_t &outWidth, uint32_t &outHeight)
    {
        // Entry trace for the dataset capture set: the only place that can say which
        // early-out a given image index took.
        std::fprintf(stderr, "[NOVARB] index=%u valid=%d tex=%d list=%d\n", index, (int)m_Valid,
                     (int)(m_Images[index].texture != nullptr), (int)(m_CommandList != nullptr));
        if (std::getenv("WL_NOVA_DIAGNOSTICS") && m_Valid && index < kImageCount && m_Images[index].texture)
        {
            const nvrhi::TextureDesc &d = m_Images[index].texture->getDesc();
            WL_CORE_INFO("VulkanNova DIAG readback image={0} {1}x{2}", index, d.width, d.height);
        }
        if (!m_Valid || index >= kImageCount)
            return false;
        nvrhi::ITexture *source = m_Images[index].texture;
        if (!source || !m_Device || !m_CommandList)
        {
            std::fprintf(stderr, "[NOVARB] index=%u no texture/device/list\n", index);
            return false;
        }

        const nvrhi::TextureDesc &sourceDesc = source->getDesc();
        const uint32_t w = sourceDesc.width;
        const uint32_t h = sourceDesc.height;
        if (w == 0 || h == 0)
        {
            std::fprintf(stderr, "[NOVARB] index=%u zero extent %ux%u\n", index, w, h);
            return false;
        }

        // The staging texture mirrors the source format; the conversion to 8-bit
        // happens below, not in the copy.
        nvrhi::TextureDesc stagingDesc = sourceDesc;
        stagingDesc.isUAV = false;
        stagingDesc.isRenderTarget = false;
        stagingDesc.isShaderResource = false;
        stagingDesc.initialState = nvrhi::ResourceStates::Common;
        stagingDesc.keepInitialState = false;
        stagingDesc.debugName = "NovaReadbackStaging";
        nvrhi::StagingTextureHandle staging =
            m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (!staging)
        {
            std::fprintf(stderr, "[NOVARB] index=%u createStagingTexture failed fmt=%d %ux%u\n", index, (int)sourceDesc.format, w, h);
            return false;
        }

        // A PRIVATE command list, not the context's.
        //
        // Using m_CommandList here executed and closed the shared list that the frame
        // recorder had opened in BeginFrame; NVRHIContext's m_CommandListOpen flag was
        // left set, so the next frame's first writeBuffer went into a closed list and
        // aborted inside nvrhi::vulkan::CommandList::writeBuffer. The capture path owns
        // its own submission and leaves the frame's list untouched.
        nvrhi::CommandListHandle copyList = m_Device->createCommandList();
        if (!copyList)
        {
            std::fprintf(stderr, "[NOVARB] index=%u createCommandList failed\n", index);
            return false;
        }
        copyList->open();
        // The source keeps a permanent CopySource state (see CreateImages), so this
        // copy needs no explicit transition and leaves the image usable next frame.
        copyList->copyTexture(staging, nvrhi::TextureSlice(), source,
                              nvrhi::TextureSlice());
        copyList->close();
        m_Device->executeCommandList(copyList);
        m_Device->waitForIdle();

        size_t rowPitch = 0;
        const uint8_t *mapped = static_cast<const uint8_t *>(m_Device->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        if (!mapped)
        {
            std::fprintf(stderr, "[NOVARB] index=%u mapStagingTexture failed fmt=%d\n", index, (int)sourceDesc.format);
            return false;
        }

        outWidth = w;
        outHeight = h;
        const bool isFloat = sourceDesc.format == nvrhi::Format::RGBA32_FLOAT;
        // RG32_FLOAT is 8 bytes per pixel, not 16: the velocity image needs its own
        // path, because the generic 4-byte memcpy below would read it as if it were
        // RGBA8 and scramble the two components into each other. X lands in R, Y in G,
        // and B/A are filled so the PNG carries the two channels unambiguously.
        const bool isRg32Float = sourceDesc.format == nvrhi::Format::RG32_FLOAT;
        const size_t bytesPerPixel = isFloat ? 16u : 4u;
        rgba.resize(static_cast<size_t>(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *row = mapped + static_cast<size_t>(y) * rowPitch;
            uint8_t *dst = rgba.data() + static_cast<size_t>(y) * w * 4u;
            if (isRg32Float)
            {
                const float *f = reinterpret_cast<const float *>(row);
                for (uint32_t x = 0; x < w; ++x)
                {
                    const float u = f[x * 2 + 0];
                    const float v = f[x * 2 + 1];
                    // Velocity is a signed, sub-pixel displacement. It is remapped from
                    // [-1,1] to [0,1] so it survives an 8-bit PNG, and the exporter
                    // documents that mapping; storing clamped magnitudes would throw
                    // away every negative component, which is half of them.
                    const float cu = 0.5f * (u < -1.0f ? -1.0f : (u > 1.0f ? 1.0f : u)) + 0.5f;
                    const float cv = 0.5f * (v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v)) + 0.5f;
                    dst[x * 4 + 0] = static_cast<uint8_t>(cu * 255.0f + 0.5f);
                    dst[x * 4 + 1] = static_cast<uint8_t>(cv * 255.0f + 0.5f);
                    dst[x * 4 + 2] = 0;
                    dst[x * 4 + 3] = 255;
                }
                continue;
            }
            if (isFloat)
            {
                // HDR radiance clamped to [0,1]: a PNG is 8-bit and a tone map belongs
                // in the dataset exporter, matching the GL path's choice.
                const float *f = reinterpret_cast<const float *>(row);
                for (uint32_t x = 0; x < w * 4u; ++x)
                {
                    const float v = f[x];
                    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    dst[x] = static_cast<uint8_t>(c * 255.0f + 0.5f);
                }
            }
            else
            {
                memcpy(dst, row, static_cast<size_t>(w) * bytesPerPixel);
            }
        }

        m_Device->unmapStagingTexture(staging);
        return true;
    }

    bool VulkanNova::ReadbackFloat(uint32_t index, std::vector<float> &out,
                                    uint32_t &width, uint32_t &height) const
{
    // Lossless float readback for the WLFG dataset: the head reads raw float32
    // (velocity RG32, depth RGBA32), never the 8-bit PNG the visual path emits.
    if (!m_Valid || index >= kImageCount)
        return false;
    nvrhi::ITexture *source = m_Images[index].texture;
    if (!source || !m_Device || !m_CommandList)
        return false;
    const nvrhi::TextureDesc &sourceDesc = source->getDesc();
    const uint32_t w = sourceDesc.width;
    const uint32_t h = sourceDesc.height;
    if (w == 0 || h == 0)
        return false;
    if (sourceDesc.format != nvrhi::Format::RGBA32_FLOAT &&
        sourceDesc.format != nvrhi::Format::RG32_FLOAT)
        return false;

    nvrhi::TextureDesc stagingDesc = sourceDesc;
    stagingDesc.isUAV = false;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isShaderResource = false;
    stagingDesc.initialState = nvrhi::ResourceStates::Common;
    stagingDesc.keepInitialState = false;
    stagingDesc.debugName = "NovaReadbackFloat";
    nvrhi::StagingTextureHandle staging =
        m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging)
        return false;
    nvrhi::CommandListHandle copyList = m_Device->createCommandList();
    if (!copyList)
        return false;
    copyList->open();
    nvrhi::TextureSlice srcSlice;
    copyList->copyTexture(staging, nvrhi::TextureSlice(), source, srcSlice);
    copyList->close();
    m_Device->executeCommandList(copyList);
    m_Device->waitForIdle();
    size_t rowPitch = 0;
    const uint8_t *mapped = static_cast<const uint8_t *>(m_Device->mapStagingTexture(
        staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (!mapped)
        return false;

    const size_t total = (size_t)w * h;
    out.resize(total * 4);
    if (sourceDesc.format == nvrhi::Format::RG32_FLOAT)
    {
        // u5 velocity: R = velX, G = velY, stride 8 bytes per pixel.
        for (uint32_t y = 0; y < h; ++y)
        {
            const float *row = reinterpret_cast<const float *>(mapped + (size_t)y * rowPitch);
            float *dst = out.data() + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; ++x)
            {
                dst[x * 4 + 0] = row[x * 2 + 0];
                dst[x * 4 + 1] = row[x * 2 + 1];
                dst[x * 4 + 2] = 0.0f;
                dst[x * 4 + 3] = 0.0f;
            }
        }
    }
    else
    {
        // u6 depth: RGBA32_FLOAT, R = NDC depth.
        for (uint32_t y = 0; y < h; ++y)
        {
            const float *row = reinterpret_cast<const float *>(mapped + (size_t)y * rowPitch);
            float *dst = out.data() + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; ++x)
                dst[x * 4 + 0] = row[x * 4 + 0];
        }
    }
    m_Device->unmapStagingTexture(staging);
    width = w;
    height = h;
    return true;
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

        // UAV images, u0..u7 and u9 (u8 is a deliberate gap).
        for (uint32_t i = 0; i < kImageCount; ++i)
            layoutDesc.bindings.push_back(
                nvrhi::BindingLayoutItem::Texture_UAV(kImageUavSlot[i]));
        // Structured buffers: Instances t1, BVHNodes t2, LightIndices t3.
        for (uint32_t i = 1; i <= 3; ++i)
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(i));
        // ReSTIR reservoir: the module reads it at t9 (binding 25 through the t offset)
        // and writes it at u10 (binding 10). The read side used to be declared at slot 10,
        // i.e. binding 26 -- a descriptor the shader never reads, while binding 25, the one
        // it does read, was never declared. Validation reported it both ways: the pipeline
        // layout was missing binding 25 "ReservoirRead", and the dispatch bound a set with
        // an unwritten binding.
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(10));
        // Sampler s0 and NovaConstants b1.
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(0));
        layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(1));
        // Scene texture array t15[32].
        {
            nvrhi::BindingLayoutItem item = nvrhi::BindingLayoutItem::Texture_SRV(15);
            item.size = kSceneTextureCount;
            layoutDesc.bindings.push_back(item);
        }
        // Sampled snapshots t48..t53.
        for (uint32_t slot : kSampledSlot)
            layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
        // Read-only snapshots of our own images, t64/65/66/68/69/70/71/73.
        for (uint32_t i = 0; i < kReadImageCount; ++i)
            layoutDesc.bindings.push_back(
                nvrhi::BindingLayoutItem::Texture_SRV(kReadImageSlot[i]));
        // NeuralGlobals (u12) is read and written by the reduce stage; NeuralWeights
        // (u13) and TerrainVertices (t4) are read-only buffers; s_WlfgHistory (t72) is
        // a sampled texture. Declared by the module, so they must exist in the layout
        // even though this path leaves the features disabled.
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(kNeuralGlobalsSlot));
        // UAV, not SRV: NVRHI derives the Vulkan binding as offset + slot, and slot 13
        // only lands on the module's binding 13 through the UAV offset (0). Declared as an
        // SRV it became binding 16 + 13 = 29, which the pipeline-layout check rejected and
        // which left "Binding 13, NeuralWeights" unwritten at every dispatch.
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(kNeuralWeightsSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(kTerrainVertexSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::Texture_SRV(kWlfgHistorySlot));
        // WlfgWeights (t75), the pass-23 synthesis head's weights. Read-only, so an
        // SRV; the module declares it, so it must exist in the layout even when no blob
        // was found (the buffer is then zeroed, which makes pass 23 the identity).
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(kWlfgWeightsSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(kWlrrWeightsSlot));
        // Phase 1 hybrid visibility G-buffer (t78..t81). See the header note.
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::Texture_SRV(kVisibilityIdsSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::Texture_SRV(kVisibilityAlbedoSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::Texture_SRV(kVisibilityNormalSrvSlot));
        layoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::Texture_SRV(kVisibilityDepthSrvSlot));

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
                     "{6} sampled SRV (t48 = history) + {7} read SRV + 2 reservoir = {8} bindings, "
                     "all distinct; t67 remains declaration-only, and Init "
                     "cross-checked the active read table against the loaded module",
                     kBindingOffsetUav, kBindingOffsetConstantBuffer,
                     kBindingOffsetShaderResource, kBindingOffsetSampler, kImageCount,
                     kSceneTextureCount, kSampledCount, kReadImageCount,
                     kImageCount + 3 + 1 + 1 + 1 + kSampledCount + kReadImageCount + 2);
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
        if (std::getenv("WL_NOVA_DIAGNOSTICS"))
            WL_CORE_INFO("VulkanNova DIAG Resize {0}x{1} (was {2}x{3} lastDispatchPass={4})",
                         w, h, m_Width, m_Height, m_LastDispatchPass);
        m_LastDispatchPass = -1;
        m_Width = w;
        m_Height = h;
        CreateImages(w, h);
        CreateBindingSet();
        m_BoundFrame = 0xFFFFFFFFu;
        if (!m_LoggedBindingContract)
        {
            LogBindingContract();
            m_LoggedBindingContract = true;
        }
    }

    void VulkanNova::CreateImages(uint32_t w, uint32_t h)
    {
        constexpr uint64_t kReservoirBytesPerPixel = 32;
        const uint64_t pixelCount = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
        if (pixelCount > std::numeric_limits<uint64_t>::max() / kReservoirBytesPerPixel)
            throw std::overflow_error("VulkanNova: reservoir buffer size overflow");
        const uint64_t reservoirByteSize = pixelCount * kReservoirBytesPerPixel;

        static const nvrhi::Format formats[kImageCount] = {
            nvrhi::Format::RGBA32_FLOAT, // img_Output        u0
            nvrhi::Format::RGBA32_FLOAT, // img_Accumulation  u1
            nvrhi::Format::RGBA32_FLOAT, // img_Bloom         u2
            nvrhi::Format::RGBA8_UNORM,     // img_MaterialPacked u3
            nvrhi::Format::RGBA32_FLOAT, // img_AlbedoHit     u4
            nvrhi::Format::RG32_FLOAT,       // img_Velocity      u5
            nvrhi::Format::RGBA32_FLOAT, // img_TraceGBuffer  u6
            nvrhi::Format::RGBA32_FLOAT, // img_Bloom_Temp    u7
            nvrhi::Format::RG32_FLOAT,       // img_Moments       u9
            nvrhi::Format::RGBA8_UNORM,     // img_Mask          u14
            nvrhi::Format::RGBA32_FLOAT, // img_WlrrInput     u15
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
            desc.isUAV = true;
            // Compute writes the images and snapshot copies read them as sources.
            // keepInitialState requires the permanent state to include both uses.
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess |
                                nvrhi::ResourceStates::CopySource;
            desc.keepInitialState = true;
            desc.debugName = "NovaImage";
            m_Images[i].texture = m_Device->createTexture(desc);

            // A texture bound as a UAV cannot also be read as an SRV in the
            // same dispatch, so the read_* bindings sample a copy. It needs
            // both states: copies write it, and the descriptor set samples it.
            nvrhi::TextureDesc snapDesc = desc;
            snapDesc.isUAV = false;
            snapDesc.initialState = nvrhi::ResourceStates::CopyDest |
                                    nvrhi::ResourceStates::ShaderResource;
            snapDesc.keepInitialState = true;
            snapDesc.debugName = "NovaSnapshot";
            m_Images[i].snapshot = m_Device->createTexture(snapDesc);
            m_Images[i].snapshotDirty = true;

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
        if (!m_HistorySnapshot)
            throw std::runtime_error("VulkanNova: failed to create temporal history texture");
        // Newly allocated storage is undefined until the renderer-owned list
        // clears it before the first dispatch.
        m_ImagesNeedClear = true;

        // WLFG previous-present history (t72). CreateFeatureInputs() allocates a
        // 1x1 RGBA8 placeholder so the descriptor is valid before the first
        // Resize; this replaces it with the real thing now that the size is
        // known. Full render size, not trace size: RunWlfgWarp addresses it as
        // histUV = prevUV * u_RenderScale, i.e. the trace-res region occupying
        // [0, RenderScale]^2 of a full-res texture.
        //
        // Both state bits for the same reason historyDesc needs them: it is
        // written by copy (CopyDest) and read as an SRV (ShaderResource), and
        // keepInitialState makes NVRHI verify every use against exactly these.
        {
            nvrhi::TextureDesc wlfgDesc;
            wlfgDesc.width = w;
            wlfgDesc.height = h;
            wlfgDesc.depth = 1;
            wlfgDesc.arraySize = 1;
            wlfgDesc.mipLevels = 1;
            wlfgDesc.sampleCount = 1;
            wlfgDesc.format = nvrhi::Format::RGBA32_FLOAT;
            wlfgDesc.isShaderResource = true;
            wlfgDesc.initialState = nvrhi::ResourceStates::CopyDest |
                                    nvrhi::ResourceStates::ShaderResource;
            wlfgDesc.keepInitialState = true;
            wlfgDesc.debugName = "NovaWlfgHistory";
            m_WlfgHistory = m_Device->createTexture(wlfgDesc);
            if (!m_WlfgHistory)
                throw std::runtime_error("VulkanNova: failed to create WLFG history texture");
            m_ImagesNeedClear = true;
        }

        // ReSTIR reservoir ping-pong buffers. Each is 32 bytes per pixel.
        m_ReservoirWidth = w;
        m_ReservoirHeight = h;
        m_ReservoirReadFlip = false;
        m_ReservoirsNeedClear = true;
        for (uint32_t i = 0; i < 2; ++i)
        {
            nvrhi::BufferDesc reservoirDesc;
            reservoirDesc.byteSize = reservoirByteSize;
            reservoirDesc.structStride = 32;
            reservoirDesc.canHaveUAVs = true;
            reservoirDesc.initialState = nvrhi::ResourceStates::UnorderedAccess |
                                         nvrhi::ResourceStates::ShaderResource;
            reservoirDesc.keepInitialState = true;
            reservoirDesc.debugName = "NovaReservoir";
            m_ReservoirBuffers[i] = m_Device->createBuffer(reservoirDesc);
            if (!m_ReservoirBuffers[i])
                throw std::runtime_error("VulkanNova: failed to create reservoir buffer");
        }
    }

    void VulkanNova::CreateBindingSet()
    {
        nvrhi::BindingSetDesc desc;
        // Keep sets alive until already-recorded command lists finish using them.
        desc.trackLiveness = true;

        // Every scene-array element needs a valid Texture2D SRV. History is an
        // existing compatible shader-resource texture and is used only as the
        // fallback for elements the caller did not supply.
        if (!m_HistorySnapshot)
            throw std::runtime_error("VulkanNova: no compatible fallback texture for scene descriptors");
        m_SceneSRVs.resize(kSceneTextureCount, m_HistorySnapshot);
        for (auto &sceneSRV : m_SceneSRVs)
            if (!sceneSRV)
                sceneSRV = m_HistorySnapshot;

        // UAV images, u0..u7 and u9 (u8 is a deliberate gap).
        for (uint32_t i = 0; i < kImageCount; ++i)
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(
                kImageUavSlot[i], m_Images[i].texture));

        // Instances t1, BVHNodes t2, LightIndices t3. There is no
        // StructuredBuffer_SRV factory, so build the item directly.
        // USE THE FACTORIES, NEVER a hand-filled BindingSetItem.
        //
        // nvrhi::BindingSetItem's default constructor initialises NOTHING ("for
        // performance: stored in large statically sized arrays"), and `item{}` calls that
        // constructor rather than zero-initialising. So a hand-built item carried an
        // uninitialised union where the buffer range lives, and on Vulkan that garbage
        // became pDescriptorWrites[].pBufferInfo.offset -- typically a value equal to or
        // larger than the buffer itself, which BufferRange::resolve clamps to give
        // offset == byteSize and range == 0. The descriptor was therefore invalid and the
        // shader bound no data: every ray missed, the frame came out as sky only, while
        // DX12 rendered the same scene correctly because it ignores that field.
        //
        // StructuredBuffer_SRV/UAV set the range to EntireBuffer, which is what the
        // resource is bound for in every case here.
        for (uint32_t i = 0; i < 3; ++i)
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                i + 1, m_StructuredBuffers[i]));

        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, m_Sampler));
        desc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(1, m_ConstantBuffer));

        // u_SceneTextures is t15[32]: one binding, arrayElement walks the
        // array. Every element has a real texture, including unused slots.
        for (uint32_t i = 0; i < kSceneTextureCount; ++i)
        {
            nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(
                15, m_SceneSRVs[i]);
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
        desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
            kSampledSlot[0], m_HistorySnapshot));

        // s_DepthBuffer / s_Output / s_NormalBuffer / s_InputAlbedo (t49..t52)
        // still come from the caller's scene texture list. k starts at 1 so t48
        // is left to the history binding above; the remaining four keep exactly
        // the source they had before this change.
        //
        // t53 (s_Moments) is different: it is NOT one of the caller's scene
        // textures. It is the moments snapshot, the previous frame's moments
        // that pass 12 samples at a reprojected UV. So it binds
        // m_Images[8].snapshot, not m_SceneSRVs[5].
        for (uint32_t k = 1; k < kSampledCount; ++k)
        {
            const uint32_t slot = kSampledSlot[k];
            if (slot == 53)
            {
                // s_Moments: the moments snapshot, not a scene texture.
                desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    slot, m_Images[8].snapshot));
            }
            else
            {
                desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                    slot, m_SceneSRVs[k]));
            }
        }

        // Read-only snapshots of our own images, t64/65/66/68/69/70/71/73. These are
        // separate textures: a texture bound as a UAV cannot also be sampled.
        for (uint32_t i = 0; i < kReadImageCount; ++i)
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(
                kReadImageSlot[i], m_Images[kReadImageIndex[i]].snapshot));
        }

        // ReSTIR reservoir ping-pong: read from one buffer (t10), write to the other (u10).
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                9, m_ReservoirBuffers[m_ReservoirReadFlip ? 1 : 0]));
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                10, m_ReservoirBuffers[m_ReservoirReadFlip ? 0 : 1]));
        }

        // The neural/terrain/WLFG inputs. Bound to real, minimal resources so the
        // descriptors are valid: with the feature flags off in NovaConstants the shader
        // never reads past element zero, and a null descriptor would be a driver fault
        // the moment one of those flags were switched on.
        {
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                kNeuralGlobalsSlot, m_NeuralGlobals));
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                kNeuralWeightsSlot, m_NeuralWeights));
            desc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                kTerrainVertexSlot, m_TerrainVertices));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(kWlfgHistorySlot, m_WlfgHistory));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::StructuredBuffer_SRV(kWlfgWeightsSlot, m_WlfgWeights));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::StructuredBuffer_SRV(kWlrrWeightsSlot, m_WlrrWeights));
            // Phase 1 hybrid visibility G-buffer (t78..t81). Never read on this
            // backend (u_UseRasterVisibility is always 0), but the SPIR-V declares
            // them, so they must be bound to real resources.
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(kVisibilityIdsSlot, m_VisDummyInt));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(kVisibilityAlbedoSlot, m_VisDummyFloat4));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(kVisibilityNormalSrvSlot, m_VisDummyFloat4));
            desc.bindings.push_back(
                nvrhi::BindingSetItem::Texture_SRV(kVisibilityDepthSrvSlot, m_VisDummyFloat));
        }

        m_BindingSet = m_Device->createBindingSet(desc, m_BindingLayout);
        if (!m_BindingSet)
            throw std::runtime_error("VulkanNova: createBindingSet failed");
        m_BindingSetDirty = false;
    }

    // -------------------------------------------------------------------------
    // Per-frame work
    // -------------------------------------------------------------------------

    void VulkanNova::EnsureStructuredBuffer(uint32_t slot, uint32_t byteSize, uint32_t stride)
    {
        if (slot > 2 || byteSize == 0 || stride == 0)
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
        nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
        if (!buffer)
            throw std::runtime_error("VulkanNova: failed to create structured buffer");

        m_StructuredBuffers[slot] = buffer;
        m_StructuredBufferSize[slot] = byteSize;
        m_StructuredBufferStride[slot] = stride;
        m_BindingSetDirty = true;
    }

    void VulkanNova::Upload(uint32_t slot, const void *data, uint32_t count, uint32_t stride)
    {
        if (slot > 2 || stride == 0 || (count != 0 && !data))
            return;
        // Keep a valid one-record SRV bound even for empty scenes. The shader
        // must still respect the corresponding count uniform before reading it.
        const uint64_t recordCount = count == 0 ? 1u : static_cast<uint64_t>(count);
        const uint64_t byteCount = recordCount * static_cast<uint64_t>(stride);
        if (byteCount > (std::numeric_limits<uint32_t>::max)())
            throw std::runtime_error("VulkanNova: structured upload exceeds 32-bit buffer limit");
        const uint32_t byteSize = static_cast<uint32_t>(byteCount);
        EnsureStructuredBuffer(slot, byteSize, stride);
        if (!m_StructuredBuffers[slot])
            return;
        if (!EnsureCommandList())
            return;

        std::vector<uint8_t> zeroRecord;
        const void *uploadData = data;
        if (count == 0)
        {
            zeroRecord.assign(stride, 0);
            uploadData = zeroRecord.data();
        }

        // The engine's BeginFrame()/ExecuteNVRHICommandList() own the list in
        // the batched path. Opening or closing per upload would overwrite the
        // active list and drop earlier draws, so only record transiently when
        // we are genuinely outside a frame.
        const bool batched = m_Context && m_Context->IsCommandListOpen();
        if (!batched)
        {
            m_CommandList->open();
            m_CommandList->writeBuffer(m_StructuredBuffers[slot], uploadData, byteSize, 0);
            m_CommandList->close();
            if (m_Device)
                m_Device->executeCommandList(m_CommandList);
            return;
        }
        m_CommandList->writeBuffer(m_StructuredBuffers[slot], uploadData, byteSize, 0);
    }

    void VulkanNova::RecordCopy(nvrhi::ITexture *dst, nvrhi::ITexture *src)
    {
        if (!dst || !src || !EnsureCommandList())
            return;
        if (!m_Context->IsCommandListOpen())
            return;
        m_CommandList->copyTexture(dst, nvrhi::TextureSlice(), src, nvrhi::TextureSlice());
    }

    void VulkanNova::CopyImageToSnapshot(uint32_t imageIndex)
    {
        if (imageIndex >= kImageCount || !m_Images[imageIndex].snapshotDirty ||
            !m_Images[imageIndex].texture || !m_Images[imageIndex].snapshot ||
            !m_CommandList || !m_Context || !m_Context->IsCommandListOpen())
            return;

        RecordCopy(m_Images[imageIndex].snapshot, m_Images[imageIndex].texture);
        m_Images[imageIndex].snapshotDirty = false;
    }

    void VulkanNova::CopyImagesToSnapshots()
    {
        for (uint32_t i = 0; i < kReadImageCount; ++i)
            CopyImageToSnapshot(kReadImageIndex[i]);
    }

    void VulkanNova::RefreshImageSnapshotsForPass(int pass)
    {
        switch (pass)
        {
        case 20: // WLRR snapshot copies the accumulation pass 2 just wrote. Its
                 // IMG_LOAD is served by snapshot 1, so without this refresh the
                 // "raw input" arm is the PREVIOUS ladder's output, ~0.08 LSB from
                 // the incumbent instead of the ~0.94 the raw frame really is.
            CopyImageToSnapshot(1);
            break;
        case 21: // WLRR composite reads the pre-SVGF snapshot pass 20 just wrote
                 // AND the accumulation the a-trous ladder just left. The shader's
                 // IMG_LOAD(img_Accumulation) is served by snapshot 1, which
                 // CommitDenoise() marks dirty: without this refresh pass 21 sees
                 // the accumulation from BEFORE the last ladder step and writes it
                 // back over the filtered result -- a pure-copy pass 21 changed the
                 // frame by 0.34 LSB, a no-op body by 0.002.
            CopyImageToSnapshot(10);
            CopyImageToSnapshot(1);
            break;
        case 2: // Temporal accumulation reads this frame's trace and velocity.
            CopyImageToSnapshot(2);
            CopyImageToSnapshot(5);
            break;
        case 3: // ReSTIR candidate generation reads this frame's G-buffer.
            CopyImageToSnapshot(4);
            CopyImageToSnapshot(6);
            break;
        case 4: // Bloom blur reads the trace/bloom image.
            CopyImageToSnapshot(2);
            break;
        case 5: // Vertical bloom reads horizontal output and composite color.
            CopyImageToSnapshot(0);
            CopyImageToSnapshot(7);
            break;
        case 6: // Composite reads accumulated lighting.
        case 7: // Bilateral filter reads accumulated lighting.
            CopyImageToSnapshot(1);
            break;
        case 8: // Upscale reads the filtered temporary image.
            CopyImageToSnapshot(7);
            break;
        case 10: // God rays read the current composite output.
            CopyImageToSnapshot(0);
            break;
        case 11: // Temporal reservoir merge reads current geometry and velocity.
            CopyImageToSnapshot(4);
            CopyImageToSnapshot(5);
            CopyImageToSnapshot(6);
            break;
        case 14: // Spatial reuse reads current bloom and receiver data.
            CopyImageToSnapshot(2);
            CopyImageToSnapshot(4);
            CopyImageToSnapshot(6);
            break;
        case 12: // Moments reads current trace/velocity; t53 remains prior-frame.
            CopyImageToSnapshot(2);
            CopyImageToSnapshot(5);
            CopyImageToSnapshot(8);
            break;
        case 13: // A-Trous reads current accumulation and moments.
            CopyImageToSnapshot(1);
            CopyImageToSnapshot(8);
            break;
        default:
            break;
        }
    }

    void VulkanNova::MarkImageSnapshotsDirtyAfterPass(int pass)
    {
        const auto markDirty = [this](uint32_t imageIndex)
        {
            if (imageIndex < kImageCount)
                m_Images[imageIndex].snapshotDirty = true;
        };

        switch (pass)
        {
        case 0: markDirty(5); break;
        case 1: markDirty(2); markDirty(4); markDirty(6); break;
        case 2: markDirty(1); break;
        case 4: markDirty(7); break;
        case 5: markDirty(0); break;
        case 6: markDirty(0); markDirty(2); break;
        case 7: markDirty(7); break;
        case 8: markDirty(0); break;
        case 9: markDirty(3); break;
        case 10: markDirty(0); break;
        case 14: markDirty(2); break;
        case 12: markDirty(8); break;
        case 13: markDirty(7); break;
        // WLRR. Pass 21 writes img_Accumulation (1) directly through the UAV;
        // nothing else marks it dirty. Without this entry the pass-7 refresh
        // SKIPS the copy (not dirty), the bilateral blurs the STALE pre-WLRR
        // snapshot, and CommitDenoise() folds that stale copy back over image 1
        // -- erasing pass 21's write every frame. Pass 20 writes image 10; the
        // dirty flag is set by CommitWlrrSnapshot() in the schedule.
        case 20: markDirty(10); break;
        case 21: markDirty(1); break;
        default: break;
        }
    }

    void VulkanNova::RecordImageHistoryClear()
    {
        for (uint32_t i = 0; i < kImageCount; ++i)
        {
            if (!m_Images[i].texture)
                continue;
            m_CommandList->clearTextureFloat(m_Images[i].texture,
                                             nvrhi::AllSubresources,
                                             nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            m_Images[i].snapshotDirty = true;
        }
        if (m_HistorySnapshot)
        {
            m_CommandList->clearTextureFloat(m_HistorySnapshot,
                                             nvrhi::AllSubresources,
                                             nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
        }
        // Same reason as the t48 history: freshly allocated storage is undefined,
        // and the first pass 22 would otherwise reproject whatever the driver left
        // in it. Zeroed, the warp's depth test rejects it and falls back to the
        // current render for the first frame.
        if (m_WlfgHistory)
        {
            m_CommandList->clearTextureFloat(m_WlfgHistory,
                                             nvrhi::AllSubresources,
                                             nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
        }
        m_ImagesNeedClear = false;
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

        // Clear requests made outside an open renderer-owned list are recorded
        // here, before any dispatch (or snapshot copy) can observe stale data.
        // This method never opens, closes, or submits the renderer's list.
        if (m_ImagesNeedClear)
            RecordImageHistoryClear();
        if (m_ReservoirsNeedClear)
        {
            for (uint32_t i = 0; i < 2; ++i)
                if (m_ReservoirBuffers[i])
                    m_CommandList->clearBufferUInt(m_ReservoirBuffers[i], 0);
            m_ReservoirsNeedClear = false;
        }

        // Resolve the full descriptor array on every dispatch. Missing or null
        // entries use a compatible real texture; incompatible supplied textures
        // are rejected. Shrinking/reordering also clears previously stale slots.
        if (!m_HistorySnapshot)
            throw std::runtime_error("VulkanNova: no compatible fallback texture for scene descriptors");
        const bool newFrame = m_BoundFrame != m_CurrentFrame;
        bool sceneChanged = false;
        if (m_SceneSRVs.size() != kSceneTextureCount)
        {
            m_SceneSRVs.resize(kSceneTextureCount, m_HistorySnapshot);
            sceneChanged = true;
        }
        for (uint32_t i = 0; i < kSceneTextureCount; ++i)
        {
            nvrhi::ITexture *texture = m_HistorySnapshot;
            if (i < textures.size() && textures[i])
            {
                texture = ResolveSceneTexture(textures[i].get());
                if (!texture)
                    throw std::runtime_error("VulkanNova: scene texture is not a compatible NVRHI Texture2D");
            }
            if (m_SceneSRVs[i] != texture)
            {
                m_SceneSRVs[i] = texture;
                sceneChanged = true;
            }
        }
        if (sceneChanged)
            m_BindingSetDirty = true;
        // Upload/reallocation and reservoir swaps only mark the descriptor set
        // dirty. Rebuild here at a dispatch boundary so recorded work retains
        // its prior set while this dispatch sees the current resources.
        if (m_BindingSetDirty || !m_BindingSet)
            CreateBindingSet();
        if (newFrame || sceneChanged)
            CopyImagesToSnapshots();
        RefreshImageSnapshotsForPass(pass);
        m_BoundFrame = m_CurrentFrame;

        // TEMPORARY DIAGNOSTIC (WL_NOVA_DIAGNOSTICS): what the constants actually hold
        // at dispatch, and how big the instance buffer is. A flat image means either the
        // camera/instance-count never arrived or the buffer is empty; this separates them.
        if (std::getenv("WL_NOVA_DIAGNOSTICS") && m_DiagDispatchCount < 400)
        {
            ++m_DiagDispatchCount;
            m_LastDispatchPass = pass;
            WL_CORE_INFO("VulkanNova DIAG dispatch #{0} pass={1} {2}x{3} clearPending={4}",
                         m_DiagDispatchCount, pass, w, h, m_ImagesNeedClear);
        }
        if (std::getenv("WL_NOVA_DIAGNOSTICS") && pass == 0)
        {
            uint32_t instanceCount = 0, passId = 0;
            float camera[3] = {0, 0, 0};
            std::memcpy(&instanceCount, m_ConstantBytes.data() + 144, 4);
            std::memcpy(&passId, m_ConstantBytes.data() + 156, 4);
            std::memcpy(camera, m_ConstantBytes.data() + 0, 12);
            static uint32_t logged = 0;
            if (logged < 6)
            {
                ++logged;
                WL_CORE_INFO("VulkanNova DIAG pass={0} {1}x{2} instances={3} u_PassID={4} "
                             "cam=({5:.2f},{6:.2f},{7:.2f}) instancesBuf={8}B stride={9} "
                             "cbBytes={10}",
                             pass, w, h, instanceCount, passId, camera[0], camera[1],
                             camera[2], m_StructuredBufferSize[0], m_StructuredBufferStride[0],
                             m_ConstantBytes.size());
            }
        }

        m_CommandList->writeBuffer(m_ConstantBuffer, m_ConstantBytes.data(),
                                   m_ConstantBytes.size(), 0);

        nvrhi::ComputeState state;
        state.pipeline = m_Pipeline;
        state.bindings.push_back(m_BindingSet);
        m_CommandList->setComputeState(state);
        m_CommandList->dispatch((w + 7) / 8, (h + 7) / 8, 1);
        MarkImageSnapshotsDirtyAfterPass(pass);
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

    void VulkanNova::CommitWlfgHistory()
    {
        // Write half of the t72 binding: capture THIS frame's present so next
        // frame's pass 22 can reproject it. m_Images[0] is img_Output, the
        // displayed frame.
        //
        // Deliberately not folded into CommitHistory(). That runs straight after
        // pass 2, and the composite (pass 6) has not run at that point -- so
        // m_Images[0] still holds the previous frame and the copy would land one
        // frame early, giving a warp that looks plausible and is wrong. The
        // schedule calls this immediately after pass(6,true).
        if (!m_Valid || !m_WlfgHistory || !m_Images[0].texture)
            return;
        RecordCopy(m_WlfgHistory, m_Images[0].texture);
    }

    void VulkanNova::LoadWlfgWeights()
    {
        // One-shot. Reads the blob tools/train_wlfg/export_weights.py writes and
        // uploads its payload to the t75 buffer. The header is validated in FULL
        // (magic, version, payload size, CRC32): a bit-flipped weight is a silent
        // visual corruption with no shader error, no link failure and no log line --
        // the same failure class the neural stage's NeuralStageBlob guards.
        //
        // ANY failure leaves the buffer zeroed, which makes pass 23 the identity:
        // every matmul is zero, so gate*residual is 0 and out == anchor. A missing or
        // corrupt blob therefore degrades to "no head", never to garbage -- and that
        // zeroed case is also the pass's acceptance control.
        m_WlfgWeightsLoaded = true;
        if (!m_WlfgWeights)
            return;

        // The payload starts ZEROED and only a fully-validated blob replaces it.
        // This matters: createBuffer() does not guarantee zeroed memory, so without
        // this the t75 buffer could hold uninitialised VRAM and pass 23 would emit
        // garbage instead of the identity. Every failure path below therefore still
        // uploads zeros -- "no head" is a DEFINED state, not whatever the allocator
        // happened to leave behind.
        std::vector<float> payload(kWlfgWeightCount, 0.0f);

        std::filesystem::path path;
        if (const char *env = getenv("WL_WLFG_WEIGHTS"))
            path = env;
        else
            path = Project::Get().GetAssetPath("wlfg/wlfg_weights.bin");

        constexpr size_t kHeaderBytes = 24;   // magic(4) version(4) payload(4) shapes(4) total(4) crc(4)
        const size_t payloadBytes = kWlfgWeightCount * sizeof(float);
        bool loaded = false;
        std::string reason;

        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec))
        {
            reason = "no blob at that path";
        }
        else
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            if (blob.size() != kHeaderBytes + payloadBytes)
                reason = "wrong byte size";
            else if (std::memcmp(blob.data(), "WLFG", 4) != 0)
                reason = "bad magic";
            else
            {
                auto readU32 = [&blob](size_t off) {
                    uint32_t v = 0;
                    std::memcpy(&v, blob.data() + off, sizeof(v));
                    return v;
                };
                if (readU32(4) != 1 || readU32(8) != payloadBytes)
                    reason = "version/payload mismatch";
                else
                {
                    // CRC32 over the PAYLOAD ONLY, matching export_weights.py's crc32().
                    uint32_t crc = 0xFFFFFFFFu;
                    for (size_t i = 0; i < payloadBytes; ++i)
                    {
                        crc ^= blob[kHeaderBytes + i];
                        for (int b = 0; b < 8; ++b)
                            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
                    }
                    crc ^= 0xFFFFFFFFu;
                    if (crc != readU32(20))
                        reason = "CRC32 mismatch";
                    else
                    {
                        std::memcpy(payload.data(), blob.data() + kHeaderBytes, payloadBytes);
                        loaded = true;
                    }
                }
            }
        }

        if (!EnsureCommandList())
            return;
        // PRIVATE list, always submitted -- same fix and same reason as
        // LoadWlrrWeights: a batched write into an ambient list that is never
        // submitted leaves t75 zeroed and pass 23 the identity.
        nvrhi::CommandListHandle uploadList = m_Device->createCommandList();
        if (!uploadList)
            return;
        uploadList->open();
        uploadList->writeBuffer(m_WlfgWeights, payload.data(), payloadBytes, 0);
        uploadList->close();
        m_Device->executeCommandList(uploadList);
        // Published for the editor's gate readout -- see LoadWlrrWeights.
        m_WlfgWeightsValid = loaded;

        if (loaded)
            WL_CORE_INFO("VulkanNova: loaded WLFG weights blob '{0}' ({1} floats, CRC32 ok)",
                         path.string(), kWlfgWeightCount);
        else
            WL_CORE_WARN("VulkanNova: no usable WLFG weights blob ({0}: '{1}'); t75 zeroed, "
                         "so pass 23 is the identity. Set WL_WLFG_WEIGHTS to load one.",
                         reason, path.string());
    }

    void VulkanNova::LoadWlrrWeights()
    {
        // Same contract as LoadWlfgWeights: a missing or corrupt blob leaves the
        // payload ZEROED, and a zeroed payload makes pass 21 degenerate to
        // `out == incumbent` (alpha=0 makes the anchor the incumbent and every
        // matmul zero makes gate*residual zero). So "no blob" is a SAFE default
        // that changes nothing -- it is not a silent wrong answer.
        static constexpr size_t kHeaderBytes = 24;
        m_WlrrWeightsLoaded = false;
        if (!m_Valid)
            return;

        std::vector<uint8_t> blob;
        std::string reason;
        if (const char *override = getenv("WL_WLRR_WEIGHTS"))
        {
            if (*override)
            {
                std::ifstream f(override, std::ios::binary);
                if (f)
                    blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                else
                    reason = "WL_WLRR_WEIGHTS unreadable";
            }
        }
        if (blob.empty())
        {
            std::ifstream f("D:/GitHub/Wasteland-Engine/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/assets/models/wlrr.bin", std::ios::binary);
            if (!f)
                f.open("assets/models/wlrr.bin", std::ios::binary);
            if (f)
                blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            else
                reason = reason.empty() ? "no wlrr.bin next to the executable" : reason;
        }
        if (blob.empty())
        {
            WL_CORE_WARN("VulkanNova: no WLRR blob ({0}); t76 zeroed, so pass 21 is the "
                         "identity (out == incumbent). Set WL_WLRR_WEIGHTS to load one.",
                         reason);
            return;
        }
        if (blob.size() < kHeaderBytes)
        {
            WL_CORE_WARN("VulkanNova: WLRR blob too short ({0} bytes); t76 left zeroed",
                         blob.size());
            return;
        }

        std::vector<uint8_t> payload(kWlrrPayloadCount * sizeof(float), 0);
        const uint32_t payloadBytes = kWlrrPayloadCount * static_cast<uint32_t>(sizeof(float));
        bool loaded = false;
        if (std::memcmp(blob.data(), "WLRR", 4) != 0)
            reason = "bad magic";
        else
        {
            auto readU32 = [&blob](size_t off) {
                uint32_t v = 0;
                std::memcpy(&v, blob.data() + off, sizeof(v));
                return v;
            };
            if (readU32(4) != 1 || readU32(8) != payloadBytes)
                reason = "version/payload mismatch";
            else
            {
                uint32_t crc = 0xFFFFFFFFu;
                for (size_t i = 0; i < payloadBytes; ++i)
                {
                    crc ^= blob[kHeaderBytes + i];
                    for (int b = 0; b < 8; ++b)
                        crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
                }
                crc ^= 0xFFFFFFFFu;
                if (crc != readU32(20))
                    reason = "CRC32 mismatch";
                else
                {
                    std::memcpy(payload.data(), blob.data() + kHeaderBytes, payloadBytes);
                    loaded = true;
                }
            }
        }

        if (!EnsureCommandList())
            return;
        // A PRIVATE command list, always submitted here. The old batched path
        // wrote into whatever list happened to be open at Init; when that ambient
        // list is never submitted the payload is lost, the buffer reads back as
        // zeros at dispatch, and EVERY blob -- including br=100 and alpha=1 --
        // degenerates to the identity. Same class as the Readback private list.
        nvrhi::CommandListHandle uploadList = m_Device->createCommandList();
        if (!uploadList)
            return;
        uploadList->open();
        uploadList->writeBuffer(m_WlrrWeights, payload.data(), payloadBytes, 0);
        uploadList->close();
        m_Device->executeCommandList(uploadList);
        // Published so the editor's gate readout can tell "no enhancement" from
        // "the head is off" -- the buffer being zeroed is what makes pass 21 the
        // identity, and that is indistinguishable from a disabled head without
        // this flag.
        m_WlrrWeightsValid = loaded;

        if (loaded)
            WL_CORE_INFO("VulkanNova: loaded WLRR blob ({0} floats, CRC32 ok)",
                         kWlrrPayloadCount);
        else
            WL_CORE_WARN("VulkanNova: no usable WLRR blob ({0}); t76 zeroed, "
                         "so pass 21 is the identity.", reason);
    }

    void VulkanNova::CommitWlrrSnapshot()
    {
        // Pass 20 wrote img_WlrrInput (image index 10); pass 21 reads it through
        // read_img_WlrrInput, which samples the SNAPSHOT rather than the live UAV.
        // Marking it dirty here lets RefreshImageSnapshotsForPass(21) copy it at
        // the pass-21 dispatch boundary -- after pass 20 has run, before pass 21.
        if (m_Valid && kImageCount > 10)
            m_Images[10].snapshotDirty = true;
    }

    void VulkanNova::CommitDenoise()
    {
        // Fold the bilateral-blur result back into accumulation.
        RecordCopy(m_Images[1].texture, m_Images[7].texture);
        m_Images[1].snapshotDirty = true;
    }

    void VulkanNova::SwapReservoir()
    {
        m_ReservoirReadFlip = !m_ReservoirReadFlip;
        // Refresh at the next dispatch boundary; do not replace the set while
        // already-recorded work is still using its current reservoir parity.
        m_BindingSetDirty = true;
    }

    void VulkanNova::Clear()
    {
        // Reset the temporal ping-pong history as well as the image history.
        m_ReservoirReadFlip = false;
        m_ReservoirsNeedClear = true;
        m_ImagesNeedClear = true;
        m_BindingSetDirty = true;
        if (!EnsureCommandList() || !m_Context->IsCommandListOpen())
        {
            // Defer all clears to the next renderer-owned list; never open,
            // close, or submit a list from this backend helper.
            m_BoundFrame = 0xFFFFFFFFu;
            return;
        }

        RecordImageHistoryClear();
        for (uint32_t i = 0; i < 2; ++i)
            if (m_ReservoirBuffers[i])
                m_CommandList->clearBufferUInt(m_ReservoirBuffers[i], 0);
        m_ReservoirsNeedClear = false;
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
        for (uint32_t i = 0; i < 2; ++i)
            m_ReservoirBuffers[i] = nullptr;
        for (int i = 0; i < 3; ++i)
            m_StructuredBuffers[i] = nullptr;
        m_ConstantBuffer = nullptr;
        m_Sampler = nullptr;
        m_Valid = false;
    }

} // namespace Wasteland