// Include guard for the OpenGL buffer implementations.

// This header declares the two GL-backed buffer classes; their bodies are in
// OpenGLBuffer.cpp in the same directory.
#pragma once

// Supplies BufferLayout (the vertex attribute description), the VertexBuffer
// and IndexBuffer base classes, and the Create() factories declared here but
// defined in the renderer sources.

// Note the include is by full path from the include root rather than a
// relative "../Renderer/Buffer.h" -- the engine uses project-root-relative
// includes throughout.
#include "Wasteland/Renderer/Buffer.h"

// Same single top-level namespace as the base classes in Buffer.h; these
// subclasses are not namespaced under an OpenGL sub-namespace.
namespace Wasteland {

	// GL 4.5 DSA implementation of the vertex buffer interface.

	// Note what is NOT here: no element array, no VAO. The pairing of vertex
	// buffers into a drawable unit is OpenGLVertexArray's job, which is why this
	// class exposes only Bind/Unbind and never a draw call.
	class OpenGLVertexBuffer : public VertexBuffer
	{
	public:
		// Allocate without uploading: reserves `size` BYTES and fills later via
		// SetData. The parameter is named `size` and typed uint32_t, which reads like
		// a vertex count but is a byte count -- the implementation passes it
		// straight to glBufferData as the byte size.
		OpenGLVertexBuffer(uint32_t size);
		// Allocate and upload in one step.

		// Non-const float* although the data is only read, so a caller with a
		// `const float[]` cannot use this overload and must fall back to the
		// size-only constructor plus SetData().
		OpenGLVertexBuffer(float* vertices, uint32_t size);
		// Declared here, defined in the .cpp, because it must call glDeleteBuffers.

		// Spelled `virtual` rather than `override`: the base destructor is already
		// virtual, so `override` would also be legal and would catch a base-signature
		// change. The engine is inconsistent on this (GetCount() below does use
		// override).
		virtual ~OpenGLVertexBuffer();

		// Binds to GL_ARRAY_BUFFER. Const: binding changes GL global state, not the
		// buffer, so a const handle is still bindable.
		virtual void Bind() const override;
		// Unbinds by binding 0.
		virtual void Unbind() const override;

		// Overwrites the buffer contents from offset 0. `size` is in BYTES and is
		// NOT checked against the size given to the constructor, so an oversized
		// update is a GL error that nothing in this class detects.
		virtual void SetData(const void* data, uint32_t size) override;

		// Inline in the header -- a bare member read.

		// Returns by const reference, so the caller must not hold it past a call to
		// SetLayout.
		virtual const BufferLayout& GetLayout() const override { return m_Layout; }
		// Also inline; assigns a whole BufferLayout by value, which copies the
		// underlying std::vector of BufferElements.

		// DEFECT: assigning the layout does not re-specify the vertex attributes in
		// GL. In a DSA setup the attribute layout is fixed by the vertex array's
		// glVertexAttribPointer calls; changing m_Layout here updates the engine's
		// record of the layout without telling the GPU. Whether that matters depends
		// entirely on whether OpenGLVertexArray re-reads GetLayout() and re-applies
		// it; nothing in this class suggests it does.
		virtual void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
	private:
		// The GL buffer name returned by glCreateBuffers.

		// DEFECT: no in-class initialiser. The constructor assigns it, so a normally
		// constructed object is fine, but if the constructor throws before the
		// assignment the destructor still runs and passes this indeterminate value to
		// glDeleteBuffers. An in-class `= 0` would cost nothing and remove the case.

		// Not zeroed after deletion either, so the value dangles post-destruction.
		uint32_t m_RendererID;
		// Engine-side record of the attribute layout, as opposed to whatever GL
		// actually has bound. See the SetLayout note above for how these can diverge.
		BufferLayout m_Layout;
	};

	// GL implementation of the index buffer. Holds only a name and a count --
	// the index format is fixed at 32 bits, consistent with the "only supports
	// 32-bit index buffers" note on IndexBuffer in Buffer.h.
	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		// `count` is the ELEMENT count, and it is what m_Count stores, so GetCount()
		// reports a draw size rather than a byte size.

		// `indices` may be null, which the implementation treats as "allocate now,
		// upload later" and picks GL_DYNAMIC_DRAW on that basis. m_Count is set to
		// `count` either way.
		OpenGLIndexBuffer(uint32_t* indices, uint32_t count);
		// Defined in the .cpp so it can call glDeleteBuffers. Same missing-in-class-
		// initialiser caveat as the vertex buffer's handle.
		virtual ~OpenGLIndexBuffer();

		// DEFECT (minor): no `override`. The signature does override IndexBuffer's
		// Bind today, so behaviour is correct -- but if the base signature ever
		// changed, this would silently become a new overload instead of an override
		// and the class would fail to compile as abstract, or worse, not compile at
		// all with a confusing error. The sibling SetData/GetCount below DO carry
		// override, so the omission looks like an oversight rather than a policy.
		virtual void Bind() const;
		virtual void Unbind() const;

		virtual void SetData(const void* data, uint32_t size) override;
		// The element count captured at construction. Inline: a bare member read.

		// DEFECT: this is set once in the constructor and never updated, so a
		// SetData() that changes the number of elements leaves GetCount() reporting
		// the ORIGINAL count. A draw call then uses a stale index count -- reading
		// past the end of the new index data, or ignoring the tail. There is no
		// SetCount() and SetData() has no way to update it, so a dynamic index buffer
		// cannot be resized safely through this interface.
		virtual uint32_t GetCount() const { return m_Count; }
	private:
		// GL buffer name. No in-class initialiser -- see the note on the vertex
		// buffer's handle.
		uint32_t m_RendererID;
		// Element count, set by the constructor's member initialiser list and never
		// revised. This is the field behind the stale-count defect noted at
		// GetCount() above.
		uint32_t m_Count;
	};

}