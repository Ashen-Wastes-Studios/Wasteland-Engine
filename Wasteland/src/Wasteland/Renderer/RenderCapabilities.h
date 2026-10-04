#pragma once

#include <cstdint>

#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland
{
    // Feature tiers. Every RTX-class feature resolves to exactly one, and the
    // resolution happens here so no feature code ever branches on the API enum.
    //
    //   HardwareRT     DXR / VK_KHR_ray_tracing: real RTX Kit SDKs.
    //   DenoiseCompute NRD-class spatio-temporal denoising. Works on D3D11
    //                  (which has no hardware RT) but never on OpenGL.
    //   ComputeEmulated Software tracing in plain compute shaders -- this is
    //                  the Nova path tracer, not a stub. It runs everywhere.
    //   Unavailable    The feature cannot run; caller must use its raster path.
    enum class FeatureTier : uint8_t
    {
        Unavailable = 0,
        ComputeEmulated = 1,
        DenoiseCompute = 2,
        HardwareRT = 3,
    };

    struct RenderCapabilities
    {
        RendererAPI::API api = RendererAPI::API::OpenGL;

        // Hardware ray tracing (DXR 1.1 / VK_KHR_ray_tracing).
        bool hardwareRT = false;
        // SM 6.x shader features: Inline Raytracing, RayQuery, wave intrinsics.
        bool inlineRaytracing = false;
        bool rayQuery = false;
        uint32_t maxRecursionDepth = 0;
        // Cooperative vectors / tensor cores (RTXNTC, RTXMU).
        bool tensorCores = false;

        // Software ray tracing via compute shaders (Nova). True on all four
        // backends today -- this is what keeps OpenGL feature-complete
        // without hardware RT.
        bool computeRaytracing = false;
        // NRD integration is possible (D3D11 / D3D12 / Vulkan).
        bool nrdCapable = false;

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
                if (nrdCapable)
                    return FeatureTier::DenoiseCompute;
                if (computeRaytracing)
                    return FeatureTier::ComputeEmulated;
                return FeatureTier::Unavailable;

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
}