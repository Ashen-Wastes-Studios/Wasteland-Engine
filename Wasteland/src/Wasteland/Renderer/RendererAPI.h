// RendererAPI.h -- the abstract graphics backend the engine draws through.
//
// Two implementations exist: OpenGLRendererAPI (Platform/OpenGL) and
// NVRHIRendererAPI (Platform/NVRHI, which itself covers DX11/DX12/Vulkan).
// Callers never hold a backend directly -- they go through RenderCommand, the
// static facade in RenderCommand.h whose s_RendererAPI pointer this header's
// s_API records the identity of.
//
// Deliberately minimal: state setters and two draw calls. Anything richer
// (batching, sorting, render graphs) lives above this, in Renderer2D/3D.
#pragma once

// For the vec4 clear colour in the SetClearColor signature below.
#include <glm/glm.hpp>
#include <vector>

// VertexArray is used only as a Ref<> parameter type in DrawIndexed/DrawLines;
// Texture.h likewise, for the SetActiveTextures vector. Both could be forward
// declared, but including them keeps the Ref<> template argument complete.
#include "VertexArray.h"
#include "Texture.h"

namespace Wasteland {

	// Forward declaration rather than an include: RenderCommand.h includes THIS
	// header, so including it back would be a cycle. The friend declaration at the
	// bottom of the class is the only other place the name is needed.
	class RenderCommand; // Forward declaration

	// All-virtual with a defaulted virtual destructor, so deleting through a
	// RendererAPI* (which RenderCommand::SetAPI does) is well-defined.
	// No copy/move is deleted explicitly -- nothing in the engine copies a backend,
	// and the implicitly-declared copy operations are simply unused.
	class RendererAPI
	{
	public:
		// Which backend is live, recorded once in s_API by RenderCommand::SetAPI.
		//
		// Scoped enum so the members cannot collide with anything in namespace
		// Wasteland. Note the editor relies on the numeric values: EditorLayer.cpp:1242
		// does `(int)GetAPI() - 1` to index a combo box, so inserting a value in the
		// middle would silently shift every entry after it.
		//
		// The three NVRHI values are separate enumerators rather than one "NVRHI"
		// because the editor's API selector offers DX11/DX12/Vulkan separately, even
		// though all three construct the same NVRHIRendererAPI (RenderCommand.cpp:29-33).
		enum class API
		{
			None = 0,
			OpenGL = 1,
			NVRHI_DX11 = 2,
			NVRHI_DX12 = 3,
			NVRHI_Vulkan = 4
		};
	// A second `public:` immediately after the enum. Redundant, and it is the only
	// such artifact in this header -- cosmetic, no effect on access.
	public:
		// `= default` in the base is what makes the derived destructor safe to call
		// through this pointer: without a virtual destructor, `delete s_RendererAPI`
		// in RenderCommand::SetAPI would run only this body and leak the derived state
		// (NVRHIRendererAPI holds pipeline caches).
		virtual ~RendererAPI() = default;

		// Called once per backend by RenderCommand::Init(). Sets fixed-function state
		// only -- blend/depth/multisample -- never anything target-specific.
		virtual void Init() = 0;
		// x/y are honoured by OpenGL (glViewport takes all four) but IGNORED by NVRHI:
		// nvrhi::Viewport has no origin, so NVRHIRendererAPI builds a 0,0 viewport sized
		// from the bound framebuffer or, failing that, the context. Callers that pass a
		// non-zero x/y therefore see backend-dependent behaviour.
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
		// Stored by the backend, not applied here; it takes effect at the next Clear().
		virtual void SetClearColor(const glm::vec4& color) = 0;
		// Colour plus depth in both backends. Stencil is NOT cleared, which is why
		// Framebuffer::ClearAttachment exists for the stencil case.
		virtual void Clear() = 0;

		// indexCount is counted in INDICES, not vertices.
		//
		// The default of 0 is documented by NVRHIRendererAPI::DrawIndexed as "use the
		// whole index buffer" (it substitutes nvrhiIB->GetCount()).
		// DEFECT: OpenGLRendererAPI::DrawIndexed inverts that convention --
		// `indexCount ? buffer->GetCount() : indexCount` returns the FULL count when
		// indexCount is non-zero and 0 when it is zero. So on OpenGL an explicit
		// partial count draws the entire buffer, and the default 0 draws nothing at
		// all. Partial draws are only usable on the NVRHI backends.
		//
		// The Ref<> is by const reference and shared: DrawIndexed does not mutate the
		// vertex array, but NVRHI's implementation does Bind()-style work on it.
		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) = 0;
		// Non-indexed path: a vertexCount, interpreted as vertices, drawn as a line
		// list. Renderer2D's debug lines use this.
		// DEFECT: NVRHIRendererAPI::DrawLines is a stub -- it only logs
		// WL_CORE_WARN("...not yet implemented") and returns, so line rendering under
		// DX11/DX12/Vulkan draws nothing. Only OpenGL implements it (glDrawArrays).
		// Both parameters are unused there, so no geometry is produced.
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) = 0;

		// Real on OpenGL (glLineWidth). On NVRHI it is a no-op that only traces the
		// value -- DX has no variable line width and Vulkan needs the wideLines
		// feature, which this engine does not request. The platform also clamps
		// glLineWidth to [1, ALIASED_LINE_WIDTH_RANGE], so a width above 1 is not
		// honoured there either.
		virtual void SetLineWidth(float width) = 0;
		// Empty body with a virtual, so this is an optional hook: a backend that
		// overrides it binds the textures, one that does not silently keeps whatever
		// was bound before.
		//
		// NVRHI's override makes this the one place the bound-texture set is tracked,
		// because its pipeline cache is keyed on the shader alone and is therefore
		// invalidated here whenever the texture set changes.
		//
		// The parameter is unnamed, so it is unused by design rather than by omission.
		virtual void SetActiveTextures(const std::vector<Ref<Texture2D>>& textures) {}

		// Static and inline: reads the one process-wide backend identity. Callers use
		// it to branch on capability (Application.cpp:421 tests for NVRHI_DX11) or to
		// detect a live backend switch, as EditorLayer.cpp:645-661 does when it
		// rebuilds its framebuffers after a change.
		//
		// Reports identity only -- it does NOT say whether a backend instance exists.
		// RenderCommand::s_RendererAPI can be nullptr (SetAPI(API::None)) while
		// s_API is still whatever was last set, so this can return NVRHI_DX12 on a
		// session with no renderer at all.
		inline static API GetAPI() { return s_API; }
	// s_API is private so the only writer is RenderCommand::SetAPI, which is a
	// friend below. That keeps the value in step with s_RendererAPI by
	// construction rather than by convention.
	private:
		// Grants SetAPI write access to the private s_API. Note the friendship is to
		// RenderCommand itself, not to RenderCommand's own statics, so this is the
		// narrowest grant available.
		friend class RenderCommand;
		// Defined in RenderCommand.cpp (`RendererAPI::s_API = api;` at the end of
		// SetAPI). No in-class initialiser, so it is zero-initialised to API::None
		// before any SetAPI call -- which makes GetAPI() safe to call during static
		// initialisation.
		// DEFECT: SetAPI writes s_API unconditionally at the end, so after
		// SetAPI(API::None) s_API reports None while s_RendererAPI is nullptr, and
		// there is no state in which a caller can tell "OpenGL" from "no renderer"
		// except by calling GetRendererAPI().
		static API s_API;
	};

}