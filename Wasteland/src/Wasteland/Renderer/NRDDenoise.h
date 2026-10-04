#pragma once

#include <cstdint>

#include "Wasteland/Renderer/RendererAPI.h"

namespace nrd
{
    struct Instance;
}

namespace Wasteland
{
    // NRD (NVIDIA Real-time Denoiser) integration -- the Tier B denoiser from
    // the RTX Kit stack, and the component that brings DX11 up to parity with
    // DX12/Vulkan.
    //
    // NRD 4.x has no opinion about graphics APIs. The host creates an Instance
    // describing the denoisers it wants, NRD hands back a texture pool and a
    // list of compute dispatches per frame, and the host binds and executes
    // them on whatever backend it has. That is why this is NVRHI-based rather
    // than a D3D12-only wrapper: the same object serves DX11, DX12 and Vulkan.
    // (NRD's own Integration/NRDIntegration is bound to NRI, which this engine
    // does not use.)
    //
    // Capability gating lives in RenderCapabilities::nrdCapable -- OpenGL is
    // never NRD-capable and keeps the bilateral blur. Both paths stay live.
    class NRDDenoise
    {
    public:
        ~NRDDenoise();

        // Returns false when NRD cannot run here; the caller then keeps the
        // existing bilateral blur. Never throws.
        bool Init(RendererAPI::API api);
        void Shutdown();

        void Resize(uint32_t width, uint32_t height);

        bool IsValid() const;

        // What NRD needs from Nova's output each frame.
        struct NRDDenoiseInput
        {
            // img_TraceGBuffer (binding 6) already carries world-space packed
            // normal + depth, so no separate NRD G-buffer is required.
            uint32_t normalRoughnessMotion = 0;
            uint32_t depth = 0;
            uint32_t roughness = 0;
            uint32_t albedo = 0;
            uint32_t specular = 0;
            // UNJITTERED view/proj. Nova jitters for TAA; NRD must not.
            float view[16] = {0};
            float proj[16] = {0};
            float invViewProj[16] = {0};
            float prevView[16] = {0};
            float prevProj[16] = {0};
            uint32_t frameIndex = 0;
            uint32_t historyLength = 1;
        };

        NRDDenoiseInput DescribeInputs() const;

        // True when the texture pool must be (re)allocated. Set by Resize().
        bool PoolNeedsResize() const { return m_PoolDirty; }
        void ClearPoolDirty() { m_PoolDirty = false; }

    private:
        nrd::Instance *m_Instance = nullptr;
        bool m_Valid = false;
        uint32_t m_Width = 0;
        uint32_t m_Height = 0;
        bool m_PoolDirty = false;
    };

} // namespace Wasteland