// Precompiled header, then the GL loader header. glad must come after
// wlpch: the loader needs the platform-specific types the PCH sets up.
#include "wlpch.h"
#include "OpenGLVertexArray.h"

// glad rather than <GL/gl.h>: the extension-loader form exposes the 3.3+
// core functions (glCreateVertexArrays, glVertexAttribPointer) that
// this file calls and that a legacy system header would not.
#include <glad/glad.h>

// Braced namespace style, unlike the C++17 form used in the newer headers.
// Both spellings are valid; this one matches the rest of the Platform
// layer.
namespace Wasteland {

	// A pure mapping from the backend-agnostic ShaderDataType to the GL base
	// type, so VertexBuffer layouts stay backend-independent and this is
	// the only place the two vocabularies meet.
	// Internal linkage, hence file-static.
	static GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType type)
	{
		// A switch with NO default arm, and the assert sits AFTER it rather than
		// inside. That is deliberate and correct here: with every enumerator
		// handled and no default, adding a ShaderDataType later produces a
		// compiler warning about an unhandled case, which is the signal a new
		// type needs a new GL mapping.
		switch (type)
		{
			// One line per case, and the fully qualified Wasteland:: prefix is
			// redundant inside namespace Wasteland -- harmless, but inconsistent
			// with the other eleven cases, which are unqualified.
			case Wasteland::ShaderDataType::Float: return GL_FLOAT;
			// Float2/3/4 all collapse to GL_FLOAT: the BASE type is the component type,
			// not the vector width. The width is carried by the componentCount
			// argument to glVertexAttribPointer below, which is why these four
			// cases return the same value.
			case Wasteland::ShaderDataType::Float2: return GL_FLOAT;
			case Wasteland::ShaderDataType::Float3: return GL_FLOAT;
			case Wasteland::ShaderDataType::Float4: return GL_FLOAT;
			// GL_INT, not GL_UNSIGNED_INT, even though the corresponding
			// uniform-upload path uses signed ints too -- the two choices are
			// independent and both happen to be signed here.
			case Wasteland::ShaderDataType::Int: return GL_INT;
			case Wasteland::ShaderDataType::Int2: return GL_INT;
			case Wasteland::ShaderDataType::Int3: return GL_INT;
			case Wasteland::ShaderDataType::Int4: return GL_INT;
			// GL_BOOL as the base type is legal but conventionally supplied via
			// glVertexAttribIPointer for integer attributes; using
			// glVertexAttribPointer with GL_BOOL means the value is converted
			// rather than passed as an integer. That matters only for a bool
			// attribute wider than a byte -- appears unused in practice.
			case Wasteland::ShaderDataType::Bool: return GL_BOOL;
			// A matrix attribute is four (or three) consecutive floats, so the
			// mapping is the same as Float4/Float3. The matrix-ness is a
			// shader-side convention over the memory layout.
			case Wasteland::ShaderDataType::Mat3: return GL_FLOAT;
			case Wasteland::ShaderDataType::Mat4: return GL_FLOAT;
		}
		// Reached only when a value outside the enum arrives. In Release this
		// returns 0, which is not a valid GLenum -- glVertexAttribPointer
		// would then raise a GL_INVALID_ENUM. The compiler warning from the
		// missing default arm makes reaching this path unlikely.
		WL_CORE_ASSERT(false, "Unknown ShaderDataType!");
		return 0;
	}

	// Creates immediately rather than lazily, so every instance owns a real
	// GL name for its whole lifetime. No error check: glCreateVertexArrays
	// leaves the name 0 on failure and nothing here notices.
	OpenGLVertexArray::OpenGLVertexArray()
	{
		WL_PROFILE_FUNCTION();

		// The DSA (direct state access) form. Binding first and then calling
		// glGenVertexArrays+glBindVertexArray is the older idiom and is
		// unnecessary here.
		glCreateVertexArrays(1, &m_RendererID);
	}

	OpenGLVertexArray::~OpenGLVertexArray()
	{
		WL_PROFILE_FUNCTION();

		// Unconditional, with no check that m_RendererID is non-zero. Deleting
		// name 0 is an error rather than a no-op, so a failed creation in the
		// ctor produces an error in the dtor as well.
		glDeleteVertexArrays(1, &m_RendererID);
	}

	void OpenGLVertexArray::Bind() const
	{
		WL_PROFILE_FUNCTION();

		// const because binding is global GL state, not a change to this object.
		glBindVertexArray(m_RendererID);
	}

	void OpenGLVertexArray::Unbind() const
	{
		WL_PROFILE_FUNCTION();

		// Unbind is binding zero. Required before the other backends touch their
		// own VAOs: GL keeps VAO state (including attribute enables) in the
		// currently bound VAO, so leaving one bound corrupts the next
		// backend's setup.
		glBindVertexArray(0);
	}

	// A shared_ptr, and the buffer is STORED below. The shared_ptr is what
	// lets several VAOs reference one buffer without the caller managing
	// lifetimes -- though it also means a buffer added to many VAOs
	// outlives all of them.
	void OpenGLVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer)
	{
		WL_PROFILE_FUNCTION();

		// Asserting non-emptiness: a VAO with an enabled-but-unconfigured buffer
		// draws undefined data rather than failing. DEFECT (null deref,
		// documented not fixed): the shared_ptr is dereferenced BEFORE any
		// null check, so a default-constructed or null argument crashes here
		// instead of asserting.
		WL_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size(), "Vertex buffer has no layout!");

		// Rebinding is required because glVertexAttribPointer below records its
		// state into the bound VAO. Every buffer added must rebind, so this
		// line is not redundant with anything the caller did.
		glBindVertexArray(m_RendererID);
		// The ELEMENT ARRAY_BUFFER binding is part of VAO state, so this also has
		// to happen with our VAO bound.
		vertexBuffer->Bind();

		// The attribute location counter. DEFECT (location assumptions, documented
		// not fixed): locations are assigned implicitly by counting from 0
		// and there is no check against GL_MAX_VERTEX_ATTRIBS. A layout with
		// more attributes than the hardware supports silently overwrites the
		// lowest locations.
		uint32_t index = 0;
		// A reference, so the layout is fetched once for both the loop and the
		// stride passed below.
		const auto& layout = vertexBuffer->GetLayout();
		// Iterating the layout, not the buffer: the layout describes the
		// attributes, and each one becomes one enabled attribute array.
		for (const auto& element : layout)
		{
			// Enable BEFORE configuring. GL ignores the pointer call for a disabled
			// attribute, so this order matters and both lines are needed.
			glEnableVertexAttribArray(index);
			// The five-argument form (not the glVertexAttrib*Pointer integer variant):
			// floats must go through the float path or they arrive as garbage.
			glVertexAttribPointer(index,
				element.GetComponentCount(),
				ShaderDataTypeToOpenGLBaseType(element.Type),
				// The normalized flag converts integer attributes to [0,1] or [-1,1] in the
				// shader instead of passing them as raw integers. It is per-element,
				// so one buffer can mix normalised colours with plain float positions.
				element.Normalized ? GL_TRUE : GL_FALSE,
				layout.GetStride(),
				// The offset is cast to a byte pointer: GL interprets this argument as a
				// byte offset into the bound buffer. The C-style cast is required
				// here because size_t to void* is not an implicit conversion.
				(const void*)element.Offset);
			// Advance AFTER the pointer call so each attribute gets its own location.
			index++;
		}
		// Storing the shared_ptr is what keeps the buffer alive: the GL name alone
		// would not, since the buffer object is destroyed by the VertexBuffer
		// destructor calling glDeleteBuffers.
		// DEFECT (no re-entry guard, documented not fixed): calling this
		// twice with the SAME buffer re-runs the loop, re-enabling and
		// re-pointing every attribute starting again at 0 -- overwriting
		// unrelated attributes the buffer may not have set. Nothing tracks
		// which buffers are already attached.
		m_VertexBuffers.push_back(vertexBuffer);
	}

	// No assert and no null check, unlike AddVertexBuffer above.
	void OpenGLVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& indexBuffer)
	{
		WL_PROFILE_FUNCTION();

		// Same reason as above: the index buffer binding is captured into the
		// currently bound VAO.
		glBindVertexArray(m_RendererID);
		indexBuffer->Bind();

		// Retained so the index buffer outlives the VAO. Calling this twice
		// replaces the previous index buffer, which is the intended
		// behaviour and unlike the vertex-buffer case does not corrupt
		// anything.
		m_IndexBuffer = indexBuffer;
	}

}