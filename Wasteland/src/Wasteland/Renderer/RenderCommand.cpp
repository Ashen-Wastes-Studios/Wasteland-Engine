// PCH first, by house rule; the header below brings in RendererAPI.h, which is
// where the API enum this file switches on is declared.
#include "wlpch.h"
// Declares the static s_RendererAPI member and the inline forwarders (Init,
// Clear, DrawIndexed...) that dereference it with no null check.
#include "RenderCommand.h"

// The default backend, included unconditionally so the type is complete at the
// `new` on line 9.
#include "Platform/OpenGL/OpenGLRendererAPI.h"
// The NVRHI backend, covering DX11, DX12 and Vulkan alike. Both platform headers
// are included on every platform, so both must compile even when only one is used.
#include "Platform/NVRHI/NVRHIRendererAPI.h"

// Statics only, no per-instance state -- which is what lets every forwarder in
// the header be `inline static`.
namespace Wasteland {

	// Dynamic initialisation of a namespace-scope pointer: the OpenGL backend is
	// built before main. DEFECT (lifetime): this `new` is never paired with a delete
	// on the shutdown path, so the initial backend leaks at process exit. SetAPI does
	// delete the current one, so only the first allocation is orphaned -- harmless in
	// practice, and left as found.
	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI;

	// The only out-of-line member of an otherwise header-inline class: changing the
	// backend is not something a caller should be able to do inline.
	void RenderCommand::SetAPI(RendererAPI::API api)
	{
		// The old backend is destroyed BEFORE the new one is constructed, so two NVRHI
		// backends are never alive at the same time.
		// Clean up old API
		// The null check is redundant on the first call -- the member is initialised to
		// a valid pointer at line 9 -- but SetAPI(None) leaves it null, so a second call
		// does reach here with nothing to delete.
		if (s_RendererAPI)
		{
			// delete, not a smart pointer: the header forwarders hand the raw pointer out
			// via GetRendererAPI() with no ownership transfer. Nulling it straight after
			// matters -- a caller reaching RenderCommand::Clear() before the switch below
			// would dereference null rather than use a destroyed object.
			delete s_RendererAPI;
			s_RendererAPI = nullptr;
		}

		// Create new API
		// A switch over the enum, and it has no `default:` label on purpose: an
		// unhandled new enumerator then leaves s_RendererAPI null instead of silently
		// keeping the old backend. See the DEFECT note on the None case.
		switch (api)
		{
		// DEFECT (incomplete state): `None` is the only enumerator that leaves the
		// pointer null rather than replacing it, and every forwarder in RenderCommand.h
		// dereferences s_RendererAPI unconditionally -- so selecting None converts a
		// working renderer into a null-pointer crash on the next draw. The compiler
		// warning this relies on fires only when a new enumerator is added, not when
		// None is selected at runtime. Not fixed here; comments only.
		case RendererAPI::API::None:
			s_RendererAPI = nullptr;
			break;
		// Constructed here rather than looked up in a table, so the backend choice is
		// a link-time dependency rather than a runtime one.
		case RendererAPI::API::OpenGL:
			s_RendererAPI = new OpenGLRendererAPI;
			break;
		// Three enumerators, ONE constructed type: NVRHIRendererAPI takes the api value
		// and picks the graphics API itself. The fallthrough is deliberate -- three
		// near-identical cases would be three places to keep in sync.
		case RendererAPI::API::NVRHI_DX11:
		case RendererAPI::API::NVRHI_DX12:
		case RendererAPI::API::NVRHI_Vulkan:
			s_RendererAPI = new NVRHIRendererAPI(api);
			break;
		}

		// DEFECT (ordering): the static API tag is published AFTER the backend object is
		// built. During the switch above, Renderer::GetAPI() -- which forwards to
		// RendererAPI::GetAPI() -- still reports the PREVIOUS api, so any code reading
		// the tag to decide how to talk to the renderer sees a stale answer for the
		// duration of the switch. Benign today because SetAPI runs during startup
		// before anything draws and s_API is private with RenderCommand its only friend;
		// worth knowing before SetAPI is ever called later. Not fixed here.
		RendererAPI::s_API = api;
	}

}