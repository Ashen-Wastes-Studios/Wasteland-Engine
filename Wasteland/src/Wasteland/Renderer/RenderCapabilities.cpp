#include "wlpch.h"
#include "Wasteland/Renderer/RenderCapabilities.h"

#include "Wasteland/Core/Log.h"

namespace Wasteland
{
    static RenderCapabilities s_Capabilities;

    const char *RenderCapabilities::TierName(FeatureTier tier)
    {
        switch (tier)
        {
        case FeatureTier::HardwareRT:
            return "TierA (hardware RT)";
        case FeatureTier::DenoiseCompute:
            return "TierB (denoise compute)";
        case FeatureTier::ComputeEmulated:
            return "TierC (compute)";
        case FeatureTier::Unavailable:
        default:
            return "unavailable";
        }
    }

    void RefreshRenderCapabilities()
    {
        RenderCapabilities caps{};
        caps.api = RendererAPI::GetAPI();

        // Nova traces in software on every backend, so this is unconditionally
        // true. It is what keeps OpenGL and DX11 rendering at all: neither has
        // hardware ray tracing, and neither can ever run RTX Kit SDKs (no DXR,
        // no VK_KHR_ray_tracing, no DXC).
        caps.computeRaytracing = true;

        switch (caps.api)
        {
        case RendererAPI::API::OpenGL:
            // No DXR, no DXC, no compute denoiser. Everything resolves to TierC.
            caps.hardwareRT = false;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_DX11:
            // No hardware RT (D3D11 predates DXR). A compute denoiser would run
            // here, but none ships, so denoising stays the bilateral blur.
            caps.hardwareRT = false;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_DX12:
            caps.hardwareRT = true;
            caps.inlineRaytracing = true;
            caps.rayQuery = true;
            caps.maxRecursionDepth = 31;
            caps.tensorCores = true;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_Vulkan:
            caps.hardwareRT = true;
            caps.inlineRaytracing = true;
            caps.rayQuery = true;
            caps.maxRecursionDepth = 31;
            caps.tensorCores = true;
            caps.denoiseComputeCapable = false;
            break;
        }

        s_Capabilities = caps;

        WL_CORE_INFO("RTX Kit tier map: API={0} hardwareRT={1} inlineRT={2} rayQuery={3} denoiseCompute={4} -> denoise={5}",
                     caps.api == RendererAPI::API::OpenGL         ? "OpenGL"
                     : caps.api == RendererAPI::API::NVRHI_DX11  ? "DX11"
                     : caps.api == RendererAPI::API::NVRHI_DX12  ? "DX12"
                     : caps.api == RendererAPI::API::NVRHI_Vulkan ? "Vulkan"
                                                                   : "?",
                     caps.hardwareRT ? "yes" : "no",
                     caps.inlineRaytracing ? "yes" : "no",
                     caps.rayQuery ? "yes" : "no",
                     caps.denoiseComputeCapable ? "yes" : "no",
                     RenderCapabilities::TierName(caps.TierFor(FeatureTier::DenoiseCompute)));
    }

    const RenderCapabilities &GetRenderCapabilities()
    {
        return s_Capabilities;
    }
}