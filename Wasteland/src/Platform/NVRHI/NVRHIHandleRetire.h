#pragma once

// Retiring GPU handles whose device has been destroyed -------------------------
//
// A backend switch (DX12 -> Vulkan, Vulkan -> DX12, anything -> OpenGL) destroys a
// whole nvrhi::Device. Every GPU wrapper created under that device still holds
// nvrhi handles, and releasing one of them afterwards dispatches through the
// object's vtable into driver code whose device is gone. In this tree that showed
// up as:
//
//   nvrhi::d3d12::Framebuffer::`vector deleting destructor'
//     <- nvrhi::RefCounter<nvrhi::rt::IAccelStruct>::Release
//     <- nvrhi::RefCountPtr<nvrhi::IFramebuffer>::operator=
//     <- Wasteland::NVRHIFramebuffer::~NVRHIFramebuffer
//     <- EditorLayer::OnUpdate (the m_FramebufferAPI != RendererAPI::GetAPI() block)
//
// i.e. a Vulkan run destroying a DX12 framebuffer that outlived its device.
//
// The GPU memory is already gone with the device, so the only thing left to
// preserve is the CPU-side reference object. These helpers park it on the heap and
// never touch it again: a bounded leak of a few dozen bytes per switch, in exchange
// for not calling into freed driver state.
//
// Usage: stamp `m_Generation = NVRHIContext::GetCurrentGeneration();` when the
// wrapper creates (or recreates) its handles, then route every release through
// ReleaseOrAbandon(handle, m_Generation).

#include <nvrhi/nvrhi.h>

#include <utility>
#include <vector>

#include "NVRHIContext.h"

namespace WastelandHandleRetire
{
	template <typename T>
	void Abandon(nvrhi::RefCountPtr<T> &handle)
	{
		if (!handle)
			return;

		// Deliberately never freed: destroying this RefCountPtr would be the very
		// Release call that crashes.
		static std::vector<nvrhi::RefCountPtr<T> *> *s_Abandoned =
			new std::vector<nvrhi::RefCountPtr<T> *>();
		s_Abandoned->push_back(new nvrhi::RefCountPtr<T>(std::move(handle)));
	}

	// Drops a handle the normal way when its device is still alive, and abandons it
	// when the device that created it has been destroyed.
	template <typename T>
	void ReleaseOrAbandon(nvrhi::RefCountPtr<T> &handle, uint32_t generation)
	{
		if (Wasteland::NVRHIContext::IsGenerationRetired(generation))
			Abandon(handle);
		else
			handle = nullptr;
	}
}
