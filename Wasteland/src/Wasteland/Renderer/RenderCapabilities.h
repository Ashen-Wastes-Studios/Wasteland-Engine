#pragma once

#include <cstdint>

#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland
{
    // Feature tiers. Every RTX-class feature resolves to exactly one, and the
    // resolution happens here so no feature code ever branches on the API enum.
    //
    //   HardwareRT     A Wasteland hardware-RT path backed by device support.
    //   DenoiseCompute A dedicated denoiser that runs as plain compute, so it
    //                  works on D3D11 (which has no hardware RT) but never on
    //                  OpenGL.
    //   ComputeEmulated Software tracing in plain compute shaders -- this is
    //                  the Nova path tracer, not a stub. It runs on GL/DX11/DX12;
    //                  Vulkan remains unavailable until Nova is instantiated.
    //   ReconstructSpatial   WLSR (EASU+RCAS), WLAA sharpen, reactive mask.
    //   ReconstructTemporal  + WLRR full (kernel family + residual head).
    //   ReconstructFull      + WLFG with the neural synthesis head.
    //   Unavailable    The feature cannot run; caller must use its raster path.
    //
    // The three Reconstruct levels are FEATURE levels, not tracing mechanisms:
    // a probe degrades to the best level the active backend runs. They are
    // gated on the API, never on a GPU vendor and never on tensorCores --
    // a vendor gate is the first thing that would make this RTX-only.
    enum class FeatureTier : uint8_t
    {
        Unavailable = 0,
        ComputeEmulated = 1,
        DenoiseCompute = 2,
        HardwareRT = 3,
        ReconstructSpatial = 4,
        ReconstructTemporal = 5,
        ReconstructFull = 6,
    };

    struct RenderCapabilities
    {
        RendererAPI::API api = RendererAPI::API::OpenGL;

        // Hardware ray tracing usable by an implemented Wasteland path, not just
        // a presumed driver capability.
        bool hardwareRT = false;
        // SM 6.x shader features: Inline Raytracing, RayQuery, wave intrinsics.
        bool inlineRaytracing = false;
        bool rayQuery = false;
        uint32_t maxRecursionDepth = 0;
        // Cooperative vectors / tensor cores (RTXNTC, RTXMU).
        bool tensorCores = false;

        // Mesh shaders (amplification + mesh). DX12 and Vulkan only -- DX11 and
        // OpenGL have no mesh stage. This is a DEVICE fact, queried by
        // MergeDeviceFeatureCapabilities() from nvrhi::Feature::Meshlets; it is
        // never a constant and never inferred from the API enum. A device can
        // report Meshlets and still be unusable, so a true value means "the device
        // advertises the feature", not "a Wasteland mesh path is wired".
        bool meshShaders = false;
        // The amplification (task) stage specifically. NVRHI allows the AS stage
        // to be null and dispatchMesh() to be called directly, so this is
        // reported separately from meshShaders rather than implied by it.
        bool amplificationShaders = false;

        // Software ray tracing via compute shaders (Nova), per active runtime
        // path. VulkanNova is compiled but not instantiated today.
        bool computeRaytracing = false;
        // A compute-based denoiser can be built for this backend (D3D11 /
        // D3D12 / Vulkan). False everywhere until one exists.
        bool denoiseComputeCapable = false;

        // Degrade a TierA/TierB probe down to whatever this backend supports.
        FeatureTier TierFor(FeatureTier probe) const
        {
            switch (probe)
            {
            case FeatureTier::HardwareRT:
                if (hardwareRT)
                    return FeatureTier::HardwareRT;
                if (computeRaytracing)
                    return FeatureTier::ComputeEmulated;
                return FeatureTier::Unavailable;

            case FeatureTier::DenoiseCompute:
                if (denoiseComputeCapable)
                    return FeatureTier::DenoiseCompute;
                if (computeRaytracing)
                    return FeatureTier::ComputeEmulated;
                return FeatureTier::Unavailable;

            case FeatureTier::ReconstructFull:
            case FeatureTier::ReconstructTemporal:
            case FeatureTier::ReconstructSpatial:
            {
                // The reconstruction stack runs on the software-tracing (Nova)
                // backends. The level is a property of the BACKEND, not of the
                // GPU: no vendor gate, no tensorCores gate.
                //
                // NOTE: this is deliberately NOT gated on computeRaytracing.
                // Vulkan's entry in RenderCapabilities.cpp still reads false
                // (a stale "not yet validated" note) while the app's primary
                // path IS Vulkan Nova -- WLRR passes 20/21 and the WLFG warp are
                // SPIR-V-only. Gating on that flag would exclude the one backend
                // the stack actually runs on.
                FeatureTier best = FeatureTier::Unavailable;
                switch (api)
                {
                case RendererAPI::API::OpenGL:
                    best = FeatureTier::ReconstructSpatial;
                    break;
                case RendererAPI::API::NVRHI_DX11:
                    best = FeatureTier::ReconstructTemporal;
                    break;
                case RendererAPI::API::NVRHI_DX12:
                case RendererAPI::API::NVRHI_Vulkan:
                    best = FeatureTier::ReconstructFull;
                    break;
                default:
                    best = FeatureTier::Unavailable;
                    break;
                }
                // Degrade a probe to the best the backend runs.
                if (best == FeatureTier::Unavailable)
                    return FeatureTier::Unavailable;
                return (probe > best) ? best : probe;
            }

            default:
                return probe;
            }
        }

        // Human-readable name for the ImGui tier readout.
        static const char *TierName(FeatureTier tier);
    };

    const RenderCapabilities &GetRenderCapabilities();

    // Re-probe the active backend. Call after Init and after any device /
    // rendering-API switch (Renderer3D::RecreateResources).
    void RefreshRenderCapabilities();

    // Merges device-feature probes into the map RefreshRenderCapabilities()
    // built. Declared here, defined in Renderer3D.cpp -- the NVRHI platform
    // headers (d3d11.h/d3d12.h/vulkan.h) stay out of RenderCapabilities.cpp,
    // which is compiled into every target including the headless tests.
    //
    // Called at the end of RefreshRenderCapabilities(), before the map is
    // published, so a reader never observes a half-probed capability.
    void MergeDeviceFeatureCapabilities(RenderCapabilities &caps);
}