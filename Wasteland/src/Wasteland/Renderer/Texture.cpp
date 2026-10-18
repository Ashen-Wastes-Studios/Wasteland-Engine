#include "wlpch.h"
#include "Texture.h"

// For Renderer::GetAPI(), the single source of truth for which backend is
// live. Texture.cpp is deliberately dumb about graphics APIs -- it only
// asks this one question.
#include "Renderer.h"
// GL backend concrete type. Included unconditionally, not behind an
// #ifdef, so the OpenGLTexture2D name always resolves and the switch below
// compiles in every configuration.
#include "Platform/OpenGL/OpenGLTexture.h"
// NVRHI backend concrete type, covering DX11, DX12 and Vulkan with one
// implementation. Same unconditional-include reasoning as above.
//
// Note these two includes pull in the platform headers for every build,
// including the SDL/GLFW or D3D dependencies each carries -- a build-time
// cost traded for a switch with no preprocessor in it.
#include "Platform/NVRHI/NVRHITexture.h"

namespace Wasteland {

	// Blank-texture factory: allocates an uninitialised 2D texture of the given
	// size on whichever backend is live.
	//
	// @param width   Texture width in texels.
	// @param height  Texture height in texels.
	// @return        Shared owning pointer to the backend-specific Texture2D.
	//
	// This and the path-taking overload below are the only two ways to build a
	// Texture2D; the class has no public constructor, so the backend choice can
	// never be made at a call site.
	Ref<Texture2D> Texture2D::Create(uint32_t width, uint32_t height)
	{
		// The dispatch. One runtime query per creation decides between two
		// implementations, so there is no per-draw backend branching later --
		// callers only ever hold the abstract Ref<Texture2D>.
		switch (Renderer::GetAPI())
		{
			// Two statements sharing one line, deliberately: assert then return. The
			// explicit `return nullptr` is required, because falling out of the switch
			// would reach the 'Unknown RendererAPI!' assert below and be reported as
			// an unrecognised API when the real problem is that none was selected.
			//
			// In a release build with asserts compiled out, this silently returns null
			// and the caller dereferences it.
			//
			// The identical line in the second overload below is why the anchors for
			// these entries are full lines rather than `case ...None:` prefixes: the
			// inserter matches exact stripped text first, so a full-line anchor stays
			// unique via its forward-only cursor, while a prefix would be ambiguous.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			// Straight to the GL concrete type, forwarding width and height.
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLTexture2D>(width, height);
			// Three NVRHI cases share ONE return statement below. That is the point of
			// the fall-through: NVRHI is a single abstraction over three native APIs,
			// and NVRHITexture2D internally dispatches again. DX12 and Vulkan are
			// listed but carry no body of their own.
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// make_shared, not new: Ref<T> is a shared_ptr, so this both allocates the
				// control block in one shot and expresses the ownership transfer. The
				// returned Ref extends the raw pointer implicitly.
				//
				// Last statement of the switch -- every enumerated case returns, so control
				// cannot fall out of it.
				return std::make_shared<NVRHITexture2D>(width, height);
		}

		// Unreachable for any value RendererAPI::s_API actually holds, since all five
		// enumerators are handled above. It exists to catch a sixth enumerator being
		// added without updating this switch -- the compiler will not warn about an
		// unhandled enum case here, so this assert is the only tripwire.
		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		// Required to satisfy the compiler: a switch over an enum is not treated as
		// exhaustive for control-flow purposes here, so a return must follow it.
		// Returns null in the assert-compiled-out case -- a caller that did not
		// check the result gets a null Ref.
		return nullptr;
	}

	// File-loading factory. Structurally identical to the size overload -- the
	// same switch, the same cases, the same fall-through -- with `path` threaded
	// through instead of width/height.
	//
	// @param path  Image file path; decoding is the backend's job.
	// @return      Shared owning pointer to the backend-specific Texture2D.
	//
	// DEFECT: the load result is unchecked. Neither backend's path-taking
	// constructor reports failure here (no bool out-param, no exception), so a
	// missing or malformed file yields a texture object rather than a null Ref
	// or an error the caller can see.
	Ref<Texture2D> Texture2D::Create(const std::string& path)
	{
		switch (Renderer::GetAPI())
		{
			// Same combined assert-and-return as the first overload; the duplication
			// between the two functions is the price of the parameter type differing.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
			// GL concrete type again, this time handed the path to decode from.
			case RendererAPI::API::OpenGL: return std::make_shared<OpenGLTexture2D>(path);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// The shared return for the three NVRHI cases, mirroring the fall-through
				// in the size overload. Staging of the file through a staging texture is
				// NVRHITexture2D's business, not this file's.
				return std::make_shared<NVRHITexture2D>(path);
		}

		// Same tripwire as the first overload -- kept in step deliberately, so the
		// two factories fail the same way when the enum grows a new member.
		WL_CORE_ASSERT(false, "Unknown RendererAPI!");
		// Same tail-return rationale. In a no-assert release build a genuinely
		// unknown API is indistinguishable from a successful-but-empty texture.
		return nullptr;
	}

}
