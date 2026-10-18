// Precompiled header first, by engine convention: standard library, core
// types, Ref, WL_CORE_ASSERT, platform macros. This translation unit needs
// almost none of it -- it exists to keep the include prologue uniform.
#include "wlpch.h"
// The interface this file provides the one definition for.
#include "RendererAPI.h"

// Brace on its own line here, unlike Renderer/Shader.cpp which puts it on
// the declaration line. The codebase is not consistent about this.
// Immaterial to the compiler; only a readability difference.
namespace Wasteland
{

	// The single mutable bit of global renderer state in the engine: which
	// backend every Renderer::GetAPI() call reports, and therefore what each
	// Create() factory dispatches to.

	// This is the OUT-OF-CLASS DEFINITION of the static member declared at
	// RendererAPI.h:41. Without the `RendererAPI::` qualifier here it would be a
	// different variable with the same name.

	// Windows starts with NVRHI/DX12, the supported Nova path. Other platforms
	// retain OpenGL until a Nova-capable default is proven there. RenderCommand's
	// initial concrete backend follows this same platform selection; startup may
	// subsequently select the active API through SetAPI.

	// Not a thread-local, so it is shared mutable state with no synchronisation.
	// Benign today only because backend selection happens once during startup
	// before worker threads exist; a Create() from a thread while another
	// switches backends is a data race.

	// Being a plain enum rather than std::atomic<API> also means this write is
	// not synchronised with the reads in Renderer::GetAPI(), which is an
	// `inline static` accessor in the header returning s_API by value.
#if defined(WL_PLATFORM_WINDOWS)
	RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;
#else
	RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;
#endif

}