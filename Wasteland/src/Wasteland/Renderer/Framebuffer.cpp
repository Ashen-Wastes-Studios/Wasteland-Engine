// Precompiled header first, by engine convention (standard library, core
// types, Ref, WL_CORE_ASSERT, platform macros).
#include "wlpch.h"
#include "Framebuffer.h"

// Supplies Renderer::GetAPI(), the dispatch key in the factory below.
#include "Wasteland/Renderer/Renderer.h"

#include "Platform/OpenGL/OpenGLFramebuffer.h"
// Both backend subclasses are included rather than forward-declared: each is
// constructed by value inside CreateRef<>, which needs the complete type.
#include "Platform/NVRHI/NVRHIFramebuffer.h"

// Single top-level namespace; brace on the same line as the declaration.
namespace Wasteland {

	// The one and only factory. Every framebuffer in the engine is built here,
	// which is what makes the backend selection a single-point decision.

	// `spec` is forwarded by const reference and copied into the concrete
	// framebuffer, so the caller's specification is never aliased.
	Ref<Framebuffer> Framebuffer::Create(const FramebufferSpecification& spec)
	{
		// Same dispatch shape as Shader::Create in Renderer/Shader.cpp: one switch,
		// no default label, backend-specific subclasses selected here.

		// No default label means a future RendererAPI::API enumerator compiles
		// without a -Wswitch warning and falls through to the assert below instead.
		switch (Renderer::GetAPI())
		{
			// No backend selected yet. Returning nullptr lets the caller decide how to
			// fail; the assert makes the mistake loud while developing.

			// In release WL_CORE_ASSERT expands to nothing (Core.h), so this arm becomes
			// a bare `return nullptr;` and the diagnostic is silently lost.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			// CreateRef<T> is the engine helper from Core.h: std::make_shared wrapped so
			// the return type is spelled Ref<T> rather than a bare shared_ptr.

			// Contrast with Renderer/Shader.cpp, which calls std::make_shared directly
			// for the same job -- the two factories are not stylistically consistent.
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLFramebuffer>(spec);
			// The three NVRHI backends share one implementation; only the underlying RHI
			// differs, so the cases stack and share a single return.
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				return CreateRef<NVRHIFramebuffer>(spec);
		}

		// Unreachable for any value the enum currently defines. Debug-only: in
		// release the macro vanishes and control simply falls through.
		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		// The last-resort return, so no path through this function can fall off the
		// end -- which for a non-void function would otherwise be undefined
		// behaviour and a compiler warning.
		return nullptr;
	}

}
