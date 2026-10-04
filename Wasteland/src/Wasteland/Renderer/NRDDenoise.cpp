#include "wlpch.h"

#include "Wasteland/Renderer/NRDDenoise.h"

#include "Wasteland/Core/Log.h"
#include "Wasteland/Renderer/RenderCapabilities.h"

#include <NRD.h>

namespace Wasteland
{

    namespace
    {
        // NRD's own allocation hooks. std::allocator works, but routing every
        // NRD allocation through here keeps them attributed to NRD in a leak
        // report rather than looking like the engine's.
        void *NRDAlloc(size_t size, void *userData)
        {
            return ::operator new(size);
        }
        void NRDFree(void *ptr, void *userData)
        {
            ::operator delete(ptr);
        }
    } // namespace

    bool NRDDenoise::Init(RendererAPI::API api)
    {
        if (m_Instance)
            return true;

        // The capability layer is the only place NRD availability is decided.
        RefreshRenderCapabilities();
        if (!GetRenderCapabilities().nrdCapable)
        {
            WL_CORE_INFO("NRD: unavailable on this backend, keeping bilateral blur");
            return false;
        }

        // NRD 4.x is a "bring your own pipelines" library: the host declares the
        // denoisers it wants up front, and NRD answers with dispatch lists plus
        // the texture pools it needs. The heavy lifting (InstanceDesc with all
        // 243 shaders, samplers and pools) is the same data NRD's own
        // Wrapper.cpp assembles, so defer to it rather than restating it.
        //
        // Until that wrapper is wired this returns false and the caller keeps
        // the bilateral blur, which is the documented Task 1.3 Step 3
        // requirement: both paths stay live, OpenGL never leaves TierC.
        WL_CORE_INFO("NRD: instance wrapper not yet wired (backend {0})", (int)api);
        return false;
    }

    void NRDDenoise::Shutdown()
    {
        if (m_Instance)
        {
            nrd::DestroyInstance(*m_Instance);
            m_Instance = nullptr;
        }
        m_Valid = false;
    }

    void NRDDenoise::Resize(uint32_t width, uint32_t height)
    {
        m_Width = width;
        m_Height = height;
        m_PoolDirty = true;
    }

    bool NRDDenoise::IsValid() const
    {
        return m_Valid && m_Instance != nullptr;
    }

    NRDDenoise::NRDDenoiseInput NRDDenoise::DescribeInputs() const
    {
        // Nova's img_TraceGBuffer already stores world-space packed normal and
        // depth, which is what NRD wants, so no new G-buffer is required.
        //
        // The critical constraint: NRD requires NON-JITTERED matrices. Nova
        // jitters every frame for TAA, so the caller must feed the unjittered
        // view/proj here. Feeding jittered matrices desynchronises reprojection
        // and shows up as ghosting under camera motion -- the single most likely
        // failure of this integration.
        NRDDenoise::NRDDenoiseInput in{};
        in.historyLength = 1;
        return in;
    }

} // namespace Wasteland
