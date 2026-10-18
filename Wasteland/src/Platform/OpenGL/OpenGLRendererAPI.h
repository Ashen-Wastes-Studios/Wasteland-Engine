#pragma once

// Engine-side abstract base. This header therefore compiles on any
// platform, including the ones with no GL at all -- which is what
// lets the same renderer code sit above a backend switch.
#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland {

	// One subclass per backend, each instantiated once by the Renderer
	// when it selects an API. There is no per-frame polymorphism here:
	// calls go through a RendererAPI* and each is a virtual dispatch.
	class OpenGLRendererAPI : public RendererAPI
	{
	public:
		// Set the global GL state (blend, depth, MSAA) once. Idempotent, so
		// calling it again after a context switch is harmless.
		virtual void Init() override;
		// x/y offset is included even though no caller in the engine uses
		// it -- GL's viewport origin is bottom-left, so a nonzero y is the
		// one case where a ported D3D value needs flipping.
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

		virtual void SetClearColor(const glm::vec4& color) override;
		virtual void Clear() override;

		// indexCount DEFAULTS TO 0, which is what makes the draw caller's
		// bug in the .cpp reachable in a second way: a caller that omits the
		// argument gets count 0, and the .cpp's ternary then substitutes
		// the index buffer's full count instead of drawing nothing.
		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) override;
		// A VERTEX count, deliberately not defaulted. Contrast with
		// DrawIndexed above: the two counts are different units and the
		// asymmetry in the defaults is what stops them being confused at a
		// call site.
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) override;

		// Last, and the only stateful one -- glLineWidth has no equivalent
		// in the NVRHI path, which is why Renderer2D::SetLineWidth behaves
		// differently per backend.
		virtual void SetLineWidth(float width) override;
	};

}