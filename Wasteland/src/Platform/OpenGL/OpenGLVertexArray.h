// Include guard only: this is a plain class declaration, and a matching
// header exists per backend, so there is no #ifdef around the body.
#pragma once

// The abstract base this implements. Included rather than forward-declared
// because the override signatures below must match it exactly.
#include "Wasteland/Renderer/VertexArray.h"

// Engine root, as every header here is.
namespace Wasteland {

		// The OpenGL vertex array object, a.k.a. a VAO.
	//
		// A VAO is the GL state object that binds a set of vertex buffers to a set
		// of attribute locations AND remembers those bindings between draw calls.
		// That memory is the point of the class: without one, every draw would have
		// to re-issue every glVertexAttribPointer call.
	//
		// Public inheritance is required. VertexArray::Create() returns a
		// Ref<VertexArray> built from std::make_shared<OpenGLVertexArray>, and the
		// renderer uses the result through base pointers.
	class OpenGLVertexArray : public VertexArray
	{
		// The public section. Bind/Unbind and the buffer-attachment setters are what
		// the renderer calls; the getters are for introspection and for the editor.
	public:
		OpenGLVertexArray();
				// Declared here, defined in the .cpp because it issues glGenVertexArrays --
				// a GL call that must not happen during static initialisation.
		virtual ~OpenGLVertexArray();

				// const on a mutating-sounding operation: binding VAO state is global GL
				// state, not object state, so it does not change this object. The same
				// reasoning applies to Unbind below.
		virtual void Bind() const override;
		virtual void Unbind() const override;

				// Takes a Ref, not a raw pointer: the VAO stores a shared handle, so the
				// buffer stays alive for as long as the VAO could still be bound and drawn
				// from. A raw pointer would dangle the moment the caller released it.
		virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
				// The index buffer is stored separately from the vertex buffers because GL
				// models it as a separate binding (ELEMENT_ARRAY_BUFFER), and storing it in
				// the same vector would obscure that.
		virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

				// Returns a const reference, so a caller cannot empty the vector and leave
				// the VAO bound to nothing. std::vector would reallocate on a copy anyway.
		virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const { return m_VertexBuffers; }
		virtual const Ref<IndexBuffer>& GetIndexBuffer() const { return m_IndexBuffer; }
		// No access specifier, so this section is private by class default. Note
		// this is inconsistent with the rest of the engine, which tends to spell
		// `private:` out -- the file has one `public:` above and no `private:` at
		// all. It compiles identically; a reader looking for an explicit marker
		// will not find one here.
	private:
				// The GL object name. DEFECT (documented, not fixed): it is a raw uint32_t
				// with no default initialiser, so a default-constructed OpenGLVertexArray
				// would carry an indeterminate handle. Nothing in the engine default-
				//constructs one -- VertexArray::Create() returns std::make_shared<> of the
				//constructor, which initialises it -- so the exposure is theoretical.
		uint32_t m_RendererID;
				// The bound buffers, in the order they were added. That order is what the
				// attribute locations were assigned against, so it must not be reordered
				// or cleared after binding.
		std::vector<Ref<VertexBuffer>> m_VertexBuffers;
		Ref<IndexBuffer> m_IndexBuffer;
		// Closing the class; see the note on the missing `private:` above.
	};

}