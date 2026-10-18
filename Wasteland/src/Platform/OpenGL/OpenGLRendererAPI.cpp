#include "wlpch.h"
#include "OpenGLRendererAPI.h"

// glad rather than a system GL header: the loader has to be included
// before any GL entry point is referenced, or the calls below fail to
// link against the Microsoft OpenGL 1.1 stubs.
#include <glad/glad.h>

namespace Wasteland {

	void OpenGLRendererAPI::Init()
	{
		WL_PROFILE_FUNCTION();

		// Blend is enabled ONCE here and never disabled. The engine therefore
		// cannot draw an opaque pass without alpha blending active; that is
		// correct for this 2D renderer and would be a bug for a 3D one.
		glEnable(GL_BLEND);
		// Classic source-alpha over. The separate blend EQUATION is left at
		// its default (GL_FUNC_ADD), so src+dst*(1-srcA) -- not the premultiplied
		// variant, which would need GL_ONE instead of GL_SRC_ALPHA.
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Depth test on for a 2D renderer. The depth WRITE mask is never
		// touched, so it defaults to enabled and every drawn quad writes
		// depth -- which means a later quad at the same z can be rejected by
		// the test. Sprites therefore need deliberate z separation.
		glEnable(GL_DEPTH_TEST);
		// GL_LINE_SMOOTH is a legacy fixed-function feature removed from core
		// profile OpenGL, so on a core-profile context this is silently
		// ignored (or warns). Renderer2D's line width setting has the same
		// problem -- see SetLineWidth below.
		glEnable(GL_LINE_SMOOTH);
		// MSAA enabled globally rather than per-draw. There is no framebuffer
		// with samples disabled in this file, so it applies to every default
		// framebuffer including the UI and any render target.
		glEnable(GL_MULTISAMPLE);
	}

	void OpenGLRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		// Offset is x/y from the lower-left in GL, unlike D3D/NVRHI where the
		// viewport origin is top-left. Any code ported between backends has
		// to flip Y here.
		glViewport(x, y, width, height);
	}

	void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRendererAPI::Clear()
	{
		// Colour and depth only. Not the stencil bit, so a stencil mask set
		// elsewhere would survive a Clear() and accumulate.
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	// An INDEX count, matching Renderer2D::Flush's QuadIndexCount.
	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount)
	{
		// DEFECT (ignored parameter): the ternary resolves to indexCount
		// whenever it is non-zero -- which is every real call, since Flush
		// guards on `if (s_Data.QuadIndexCount)`. So the caller's batch
		// size is discarded and the buffer's FULL count is drawn instead.
		// A partial batch is drawn as though the whole index buffer were
		// filled. The intent was evidently "use the caller's count, else fall
		// back to the buffer's", which needs the tests the other way round.
		uint32_t count = indexCount ? vertexArray->GetIndexBuffer()->GetCount() : indexCount;
		vertexArray->Bind();
		// The offset is nullptr: drawing starts at the first index in the
		// bound element buffer. That is only correct because every batch
		// shares ONE static index buffer and relies on the vertex count to
		// limit the draw -- which the bug above defeats.
		glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
	}

	// Non-indexed: vertices are consumed in order, two per line. So the
	// count is a VERTEX count here, unlike DrawIndexed's index count.
	void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
	{
		vertexArray->Bind();
		// first=0 because the line batch always starts at the beginning of
		// the staging buffer; there is no partial-batch case here.
		glDrawArrays(GL_LINES, 0, vertexCount);
	}

    void OpenGLRendererAPI::SetLineWidth(float width)
    {
		// glLineWidth is clamped by the driver to its implementation range,
		// commonly [1, 1] on core profile, so Renderer2D::SetLineWidth's
		// 2.0 default silently has no effect and every line is 1 pixel.
		// The value is accepted and reported back unchanged, so
		// GetLineWidth lies about what is actually drawn.
		glLineWidth(width);
    }

}