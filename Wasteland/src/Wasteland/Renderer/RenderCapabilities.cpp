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

        switch (caps.api)
        {
        case RendererAPI::API::OpenGL:
            // No DXR, no DXC, no compute denoiser. Everything resolves to TierC.
            caps.computeRaytracing = true;
            caps.hardwareRT = false;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_DX11:
            // No hardware RT (D3D11 predates DXR). A compute denoiser would run
            // here, but none ships, so denoising stays the bilateral blur.
            caps.computeRaytracing = true;
            caps.hardwareRT = false;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_DX12:
            // DXR support is not a Wasteland capability yet: there is no BLAS/
            // TLAS or RT dispatch path, and device feature support is not queried.
            // Keep these defaults false until both are implemented and probed.
            caps.computeRaytracing = true;
            caps.denoiseComputeCapable = false;
            break;

        case RendererAPI::API::NVRHI_Vulkan:
            // VulkanNova is wired through Renderer3D, but its live Vulkan
            // initialization/dispatch/presentation path is not yet validated.
            // Keep this capability false until runtime smoke checks pass.
            caps.computeRaytracing = false;
            caps.denoiseComputeCapable = false;
            break;
        }

        s_Capabilities = caps;

        WL_CORE_INFO("Render capability map: API={0} hardwareRT={1} inlineRT={2} rayQuery={3} denoiseCompute={4} -> denoise={5}",
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