// Header guard.
#pragma once

// The ONLY include, and the reason this file has no GL, DX or Vulkan
        // dependency: every method here is a forwarding stub. RendererAPI
        // brings its own dependencies (glm, the Texture/VertexArray bases,
        // Ref) transitively.
//
// Note the quoted path: this header is consumed from both Wasteland and
// the editor, so it uses a relative include rather than the
// <Wasteland/...> angle-bracket form the Renderer headers use.
#include "RendererAPI.h"

namespace Wasteland {

	// All members static and all bodies inline. There is no instance and no
	// state of its own -- s_RendererAPI below is the only data, and it
	// lives here so the forwarding stubs can reach it.
	class RenderCommand
	{
	public:
		// inline static in that order: static is what matters for storage, inline
		// just suppresses an out-of-line definition requirement in a header.
		inline static void Init()
		{
			// ARROW, not dot: s_RendererAPI is a POINTER to the abstraction.
			// DEFECT (null dereference, documented not fixed): no null check on
			// s_RendererAPI. Every one of these stubs dereferences it
			// unconditionally, so calling anything before SetAPI has run is an
			// immediate crash rather than a diagnosable error. SetAPI below is
			// the only thing that installs it.
			s_RendererAPI->Init();
		}

		inline static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		// Note glm is used here but NOT included by this header -- it arrives via
		// RendererAPI.h. That works but couples this file to an include it
		// does not name, so adding a second include here later could change
		// what compiles.
		inline static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		// No colour argument: the clear colour was set by SetClearColor above,
		// and GL/Vulkan treat it as state that persists until changed. So
		// Clear's behaviour depends on a call made somewhere else entirely.
		inline static void Clear()
		{
			s_RendererAPI->Clear();
		}

		// indexCount defaults to 0, and DEFECT (semantics not documented here,
		// documented not fixed): whether 0 means "use the whole index buffer"
		// or "draw nothing" is decided in the backend, and neither is stated
		// in this header. Getting it backwards turns a deliberate partial
		// draw into an empty one. Worth confirming in RendererAPI.
		inline static void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount);
		}

		// vertexCount is NOT defaulted, unlike indexCount above: line drawing has
		// no meaningful "whole buffer" case because the vertex buffer may hold
		// non-line vertices, so the caller must state how many.
		inline static void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		inline static void SetLineWidth(float width)
		{
			s_RendererAPI->SetLineWidth(width);
		}

		// Refs, not raw pointers, so the textures cannot be destroyed between
		// binding and drawing. The vector is taken by const reference and
		// presumably iterated by the backend to assign sequential slots.
		inline static void SetActiveTextures(const std::vector<Ref<Texture2D>>& textures)
		{
			s_RendererAPI->SetActiveTextures(textures);
		}

		// NOT inline, unlike everything above: this is defined in a .cpp because
		// it constructs a backend implementation, which needs the concrete
		// OpenGL/DX/Vulkan classes that this header must not include.
		// DEFECT (no null-out on failure, documented not fixed): the return type
		// is void, so a caller cannot tell whether a requested API was
		// available. If the implementation cannot create the backend, it
		// either leaves s_RendererAPI null -- making every stub above crash
		// -- or falls back silently. Either way the caller has no signal.
		static void SetAPI(RendererAPI::API api);
		
		// Returns the raw pointer, so a caller can reach backend-specific methods
		// the abstraction does not declare -- useful and also an escape hatch
		// out of the portability this class exists to provide.
		// DEFECT (dangling, documented not fixed): the pointer is returned
		// without lifetime information. A second SetAPI call presumably
		// replaces and deletes the previous implementation, so any caller
		// still holding the pointer from before has a dangling reference.
		inline static RendererAPI* GetRendererAPI()
		{
			return s_RendererAPI;
		}
	private:
		// The single piece of state. A static member DEFINED in a .cpp, not
		// initialised inline -- correct: an inline initialiser here would need
		// a complete RendererAPI type at every use, and would construct
		// nothing useful anyway since no backend can be chosen without a
		// parameter.
		//
		// DEFECT (uninitialised by default, documented not fixed): a static pointer
		// is zero-initialised, so before SetAPI runs it is null rather than
		        // pointing at a null-checked dummy -- hence the crash note above.
		static RendererAPI* s_RendererAPI;
	};

}