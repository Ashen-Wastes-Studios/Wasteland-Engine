// PCH first, by house rule.
#include "wlpch.h"
// The abstract base: Bind/Unbind, AddVertexBuffer, SetIndexBuffer and the two
// getters. All pure virtual, so both backends must implement every one.
#include "VertexArray.h"

// Renderer::GetAPI() forwards to RendererAPI::GetAPI() and returns the static
// tag last published by RenderCommand::SetAPI -- so the factory below keys off
// that tag rather than off the live object.
#include "Renderer.h"
// The OpenGL implementation, included unconditionally: the make_shared below
// needs the complete type whether or not OpenGL is the selected backend.
#include "Platform/OpenGL/OpenGLVertexArray.h"
// The NVRHI implementation, shared by DX11, DX12 and Vulkan -- so both backends
// are a compile-time dependency of this file on every platform.
#include "Platform/NVRHI/NVRHIVertexArray.h"

// One namespace holding three translation units' worth of backend, the same
// shape as RenderCommand.cpp, which owns the API object this dispatches beside.
namespace Wasteland {

	// The factory every mesh path goes through. Returns a Ref (shared_ptr) so the
	// array outlives the mesh that created it; takes no parameters because
	// construction is empty and buffers are added afterwards.
	Ref<VertexArray> VertexArray::Create()
	{
		// Switch on the static API tag, which is why the tag and the live object must
		// agree: SetAPI publishes it, this reads it.
		//
		// DEFECT (no default label): every enumerator has a case today, so the switch is
		// exhaustive -- but a new enumerator would warn and fall out to the assert below.
		// That is the intended shape, which is why the assert sits outside the switch
		// rather than in a `default:` case. Noted so it is not "tidied" into one.
		switch (Renderer::GetAPI())
		{
			// `None` is a programming error, not a state to handle: there is no vertex array
			// to build without a backend, so the answer is no object plus a loud message.
			//
			// DEFECT (release behaviour): WL_CORE_ASSERT expands to nothing in release
			// (Core.h), so a release build silently returns nullptr here and the caller has
			// a null Ref to dereference. Same for the assert below. Not fixed here.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			// make_shared rather than new: one allocation instead of two, and the control
			// block cannot be separated from the object -- which is what lets Create return
			// Ref<VertexArray> safely, the deleter knowing the dynamic type.
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLVertexArray>();
			// Three enumerators, one constructed type, falling through deliberately:
			// NVRHIVertexArray is not told which NVRHI backend it is for, unlike
			// NVRHIRendererAPI which does receive the api value.
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return std::make_shared<NVRHIVertexArray>();
		}

		// Outside the switch on purpose. With no `default:` label the compiler demands
		// a return on every path, and in release -- where the asserts vanish -- this is
		// the only thing between an unknown API and undefined behaviour.
		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		// The release-build fallback, reachable only when an enumerator is unhandled.
		// Null rather than throw, matching the None case above.
		return nullptr;
	}

}
