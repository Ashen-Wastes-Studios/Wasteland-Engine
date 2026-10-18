#include "wlpch.h"
#include "Buffer.h"

// Only for the Renderer singleton -- every factory below dispatches on
// Renderer::GetAPI(), so including the backend headers here is what
// lets the switch name the concrete classes.
#include "Renderer.h"

// Engine side. The platform headers are pulled in only because the
// factory functions below must name their types -- the dependency is
// deliberate and one-directional, and the OpenGL/DX backends do not
// include this file.
#include "Platform/OpenGL/OpenGLBuffer.h"
#include "Platform/NVRHI/NVRHIBuffer.h"

namespace Wasteland {

	// THE FACTORY PATTERN. Engine code asks for a VertexBuffer and gets
	// whichever backend is active; nothing above this file knows that
	// NVRHI even exists. Returns a Ref to the ABSTRACT base, so the
	// returned object is destroyed correctly through the base's virtual
	// destructor.
	Ref<VertexBuffer> VertexBuffer::Create(uint32_t size)
	{
		// Read once per call, not cached. That is what lets the renderer API
		// change at runtime -- every resource created after the switch comes
		// from the new backend.
		switch (Renderer::GetAPI())
		{
			// One line per case because all four failure paths are identical.
			// DEFECT: whether the assert ABORTS depends on NDEBUG. In a release
			// build WL_CORE_ASSERT is normally compiled out, so this returns a
			// null Ref and the caller dereferences it -- a null-deref crash far
			// from the cause, with no log. A hard error return or a throw would
			// be safer than relying on the assert.
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			// Ref<OpenGLVertexBuffer> implicitly converts to Ref<VertexBuffer>
			// on return, so the caller never learns which backend it got.
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLVertexBuffer>(size);
			// The three NVRHI variants deliberately share one branch: all three
			// are serviced by the same nvrhi backend, and the API enum names
			// which native API underneath. Collapsing them keeps the three
			// labels from needing identical bodies.
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// Same shape as the size-only overload. Note the duplication across
				// the three factories: adding a backend means editing three switches
				// here plus a new Create overload, and forgetting one is a compile
				// error rather than a silent gap -- which is the saving grace of the
				// fallthrough-to-assert pattern.
				return CreateRef<NVRHIVertexBuffer>(size);
		}

		// Reached when the enum holds a value with no case above. The assert
		// is the only guard: adding an API to the enum without adding a case
		// here turns every resource creation into a null return.
		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

	// The data-taking overload. DEFECT (signature): `float*` is
	// non-const and vertex-specific, and it is forwarded verbatim to
	// both backends. Renderer2D::Init only uses the size-only form, but a
	// caller wanting to seed an index buffer or a byte blob has to cast
	// past the type. The parameter is also `size` where the backends want
	// BYTES -- passing a vertex count allocates four times too small.
	Ref<VertexBuffer> VertexBuffer::Create(float* vertices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
			case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
			case RendererAPI::API::OpenGL: return CreateRef<OpenGLVertexBuffer>(vertices, size);
			case RendererAPI::API::NVRHI_DX11:
			case RendererAPI::API::NVRHI_DX12:
			case RendererAPI::API::NVRHI_Vulkan:
				// DEFECT (wrong type at this boundary): this factory takes
				// `uint32_t* indices` but forwards to NVRHIIndexBuffer, whose
				// constructor is `NVRHIIndexBuffer(uint32_t* indices, uint32_t
				// count)` -- a COUNT, which the backend multiplies by 4. The GL
				// backend expects a BYTE count in the same position. So the same
				// value cannot be correct for both: whichever the caller passes,
				// one backend is wrong. Renderer2D::Init passes MaxIndices (an
				// index COUNT), so the GL path over-reads by 4x here.
				return CreateRef<NVRHIVertexBuffer>(vertices, size);
		}

		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

	// Note the return type differs from the two above -- Ref<IndexBuffer>
	// -- so the polymorphism is per abstract base, not one shared
	// factory.
	Ref<IndexBuffer> IndexBuffer::Create(uint32_t* indices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None: WL_CORE_ASSERT(false, "RendererApi::None is currently not supported!"); return nullptr;
		case RendererAPI::API::OpenGL: return CreateRef<OpenGLIndexBuffer>(indices, size);
		case RendererAPI::API::NVRHI_DX11:
		case RendererAPI::API::NVRHI_DX12:
		case RendererAPI::API::NVRHI_Vulkan:
			return CreateRef<NVRHIIndexBuffer>(indices, size);
		}

		WL_CORE_ASSERT(false, "Unknown RendererApi!");
		return nullptr;
	}

}
