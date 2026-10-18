// NVRHIVertexArray.h -- the NVRHI implementation of the engine's abstract
// VertexArray.
//
// Concept difference from OpenGL that explains most of this file: in GL a
// VAO owns the vertex layout binding (glVertexAttribPointer), so Bind() is
// meaningful. In NVRHI the layout is part of the GraphicsState built per draw,
// so this class only holds bookkeeping and Bind()/Unbind() are no-ops. The
// real work happens in NVRHIRendererAPI::DrawIndexed, which builds
// VertexBufferBindings from GetVertexBuffers().
//
// The NVRHI-specific addition is the cached input layout: an NVRHI pipeline
// needs an array of VertexAttributeDesc at creation time, and that array is
// derived from the CPU-side BufferLayouts attached here.
#pragma once

// The abstract base, plus transitively Buffer.h -- which is where VertexBuffer,
// IndexBuffer, BufferLayout and ShaderDataType are declared. The .cpp needs all
// of those and includes this header rather than Buffer.h directly.
#include "Wasteland/Renderer/VertexArray.h"
// The vendor API. This is the only place the engine's renderer abstraction
// meets NVRHI types in a header -- everything above the platform layer speaks
// only the engine's own interfaces.
//
// Pulled in bare rather than through wlpch, because this header is part of the
// platform layer's public surface and other platform headers include it.
#include <nvrhi/nvrhi.h>

namespace Wasteland {

	// `virtual` on the overrides below rather than `override` -- consistent with
	// the rest of this backend, but it means a signature drift from the base would
	// silently create an overload instead of a compile error.
	class NVRHIVertexArray : public VertexArray
	{
	public:
		// User-provided and empty, though the base class has no data and every member
		// below has an initialiser. `= default` would do the same thing; declaring it
		// explicitly is redundant.
		//
		// Called through VertexArray::Create(), which returns a Ref<NVRHIVertexArray>
		// upcast to Ref<VertexArray>.
		NVRHIVertexArray();
		// Also empty in the .cpp. There is nothing native to release: this object owns
		// no nvrhi::BufferHandle of its own, only Ref<>s to the buffers that do. Their
		// refcounts drop here, and the nvrhi handles are released when the last Ref
		// goes -- which is why destruction order relative to the device matters.
		//
		// `virtual` rather than `override`, same reason as the constructor note above.
		virtual ~NVRHIVertexArray();

		// NO-OP, and that is the single most surprising thing in this class.
		//
		// The OpenGL implementation calls glBindVertexArray. Here the equivalent would
		// be a GraphicsState, and NVRHIGraphicsState is a value built per draw rather
		// than a persistent object to bind. NVRHIRendererAPI::DrawIndexed therefore
		// reads GetVertexBuffers() directly and never calls Bind().
		//
		// Keeping the override means the base's pure virtual is satisfied, so code
		// written against VertexArray compiles unchanged on both backends -- but a
		// caller relying on Bind() to take effect would see nothing happen on NVRHI.
		virtual void Bind() const override;
		virtual void Unbind() const override;

		// APPENDS, does not replace. That ordering is semantic: the index into
		// m_VertexBuffers becomes desc.bufferIndex in the input layout, and the same
		// index becomes the vertex buffer SLOT in the GraphicsState. So the order of
		// AddVertexBuffer calls must match the slot numbers the shader's input layout
		// expects -- for Renderer2D's quad that is the a_Position..a_EntityID order.
		//
		// The Ref<> is copied, so the array shares ownership and the buffers outlive
		// it.
		//
		// Invalidation of the cached layout happens inside; the flag is not this
		// method's concern.
		virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
		virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

		// Inline in the header, unlike the OpenGL equivalent, because it is a bare
		// member read with no logic.
		//
		// By const reference: NVRHIRendererAPI::DrawIndexed iterates this per draw call,
		// and a copy of the vector (and a refcount bump per buffer) on every draw
		// would be a measurable cost in a batched renderer.
		//
		// The reference aliases internal state, so a caller holding it across an
		// AddVertexBuffer would see the vector reallocate underneath it. In practice
		// the only caller uses it within one function call.
		virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
		// By const reference for the same no-copy reason. Returns the Ref itself, not
		// a raw IndexBuffer*, so the caller keeps the buffer alive while using it --
		// which is what makes NVRHIRendererAPI's two-step downcast to NVRHIIndexBuffer
		// safe (see its DrawIndexed).
		//
		// Default-constructs to a null Ref when SetIndexBuffer was never called; the
		// .cpp of the renderer checks for that and errors rather than dereferencing.
		virtual const Ref<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

		// NVRHI-specific: create input layout from vertex buffer layouts
		// NVRHI-specific, and NOT part of the abstract VertexArray -- so it is
		// unreachable without downcasting, which is exactly what
		// NVRHIRendererAPI::GetOrCreatePipeline does before calling
		// device->createInputLayout().
		//
		// Returns a raw pointer INTO this object's m_InputLayout vector, with the
		// length written to the out-parameter. The NVRHI C-style out-param is what
		// forces this shape; there is no way to return a span of the right type.
		//
		// The pointer is valid only until the next AddVertexBuffer on this array (the
		// layout vector is cleared and rebuilt, which can reallocate). Callers copy
		// what they need inside createInputLayout rather than holding it.
		//
		// `const` because the layout is logically a read-only view; the mutable
		// members below are the mechanism that lets it be computed lazily.
		nvrhi::VertexAttributeDesc* GetInputLayout(uint32_t& count) const;

	private:
		// No NSDMI needed -- std::vector default-constructs empty.
		// One entry per AddVertexBuffer call; the position is the NVRHI buffer slot.
		std::vector<Ref<VertexBuffer>> m_VertexBuffers;
		// No initialiser, so this is default-constructed to a null Ref, which is the
		// "no index buffer" state the renderer checks for before downcasting.
		//
		// The trailing whitespace on this line is pre-existing and left alone.
		Ref<IndexBuffer> m_IndexBuffer;
		
		// Why the next two members are mutable: GetInputLayout is a const method (it is
		// conceptually a read), but the first call has to COMPUTE the layout. Marking
		// them mutable lets the cache fill in without the method lying about its
		// constness.
		//
		// The trade-off is that the cache is now shared mutable state with no
		// synchronisation -- two threads calling GetInputLayout on the same array
		// would race on the vector. Single-threaded rendering makes this safe today.
		//
		// These two lines are the last in the class, so they follow the members they
		// describe rather than preceding them -- the reverse of the convention used
		// elsewhere in this header.
		// Cached input layout (mutable to allow lazy generation in const method)
		// The cache itself: built on first use, rebuilt when a buffer is added.
		// Each desc is a POD-ish value with a `const char*` name pointing into the
		// BufferLayout's strings, which is why the comment at the assignment in the
		// .cpp stresses that the buffer must outlive this vector.
		mutable std::vector<nvrhi::VertexAttributeDesc> m_InputLayout;
		// Starts DIRTY, so a vertex array with no buffers added still produces a
		// (zero-length) layout on first query rather than an uninitialised read.
		//
		// Set true by AddVertexBuffer and false after a rebuild in the .cpp. Nothing
		// else touches it -- notably SetIndexBuffer does not, correctly, since the
		// index buffer plays no part in the vertex input layout.
		mutable bool m_InputLayoutDirty = true;
	};

}
