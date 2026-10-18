// Precompiled header first, by engine convention. This is also where glad
// enters the process -- glad.h is included at line 4.
#include "wlpch.h"
#include "OpenGLBuffer.h"

// The GL function loader. glad 2 (`glCreateBuffers` below is the direct
// state-access form added in GL 4.5) rather than GLEW, so the whole engine
// shares one loader; see ImGui/ImGuiBuild.cpp for a note on how that
// interacts with ImGui's own loader.
#include <glad/glad.h>

// Single top-level namespace; brace on the declaration line here.
namespace Wasteland {

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// The banner comments in this file are runs of slashes with the section name
	// in the middle line. They are the file's only visual structure and they
	// carry no semantic weight.
	// Vertex Buffer /////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// Allocate-only constructor: reserves `size` bytes of GPU memory without
	// uploading anything.

	// Paired with SetData() -- construct empty, then fill when the data exists.
	OpenGLVertexBuffer::OpenGLVertexBuffer(uint32_t size)
	{
		// Scoped profiler timer named after the enclosing function. Expands to
		// nothing when profiling is compiled out (Instrumentor.h defines both arms),
		// so it is free in release.
		WL_PROFILE_FUNCTION();

		// Direct State Access (GL 4.5+). One call that both creates the buffer and
		// returns its name, replacing the glGenBuffers + glCreateBuffers pairing.

		// So this requires a 4.5 context. Note the Linux window path
		// (Platform/Linux/LinuxWindow.cpp) never calls glfwWindowHint with
		// GLFW_CONTEXT_VERSION or GL_OPENGL_PROFILE at all, unlike the Windows
		// backend which requests GLFW_OPENGL_CORE_PROFILE. On a Linux box whose
		// driver advertises an older default context this DSA call has no entry
		// point; glad resolves it but the driver call fails at runtime. Appears to
		// work in practice only because modern Linux compositors hand out 4.5+ by
		// default.

		// The buffer is left UNBOUND after this call, despite the explicit
		// glBindBuffer on the next line.
		glCreateBuffers(1, &m_RendererID);
		// Binds to the ARRAY_BUFFER target, which is what makes the following
		// glBufferData apply to this buffer.

		// The binding is a global GL state that is never restored in this function,
		// so it leaks to whatever runs next. Benign in practice because a
		// VertexArray rebinds its own buffers before drawing.
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		// Allocates `size` bytes with no initial data (nullptr), and hints that the
		// contents will change often.

		// GL_DYNAMIC_DRAW is a usage HINT, not behaviour: it tells the driver where to
		// optimise for, and a driver is free to ignore it. Pairing it with
		// SetData's glBufferSubData is the intended pattern.

		// DEFECT: no error check. If the allocation fails (out of memory, or a too-
		// large size) glBufferData sets GL_OUT_OF_MEMORY and returns; `size` came
		// from the caller unvalidated, and the object is left in a state where every
		// later SetData writes into storage that does not exist. Every gl* call in
		// this file is unchecked, which is consistent with the rest of the OpenGL
		// backend but means GL errors are invisible without an external debug context.
		glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
	}

	// Allocate-and-upload constructor for statically known vertex data.

	// `vertices` is a non-const float* even though it is only read. A caller
	// holding a `const float[]` cannot use this overload, and has to fall back to
	// the size-only constructor plus SetData().

	// `size` is in BYTES, not vertex count, despite the type looking like a
	// vertex tally. A caller passing a vertex count gets a buffer sized for
	// `count` bytes rather than count*12-or-so, and the mismatch is silent.
	OpenGLVertexBuffer::OpenGLVertexBuffer(float* vertices, uint32_t size)
	{
		WL_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		// Uploads the pointer's contents at creation time.

		// GL_STATIC_DRAW matches the intent: this data will not be rewritten. The
		// sibling constructor uses GL_DYNAMIC_DRAW for the same allocation size, so
		// the usage hint follows the constructor chosen rather than any property of
		// the data.
		glBufferData(GL_ARRAY_BUFFER, size, vertices, GL_STATIC_DRAW);
	}

	// Releases the GL buffer name.

	// DEFECT: m_RendererID is not reset to 0 afterwards. Not a double-delete risk
	// -- the destructor runs once per object -- but it leaves a dangling name in
	// a destroyed object, which matters for any diagnostic code that reads it.
	OpenGLVertexBuffer::~OpenGLVertexBuffer()
	{
		WL_PROFILE_FUNCTION();

		// glDeleteBuffers silently ignores 0, so a construction that failed before
		// assigning m_RendererID would still be safe here -- except m_RendererID is
		// not initialised in the header (OpenGLBuffer.h declares it with no default),
		// so an object whose constructor threw before line 16 deletes whatever
		// indeterminate value the member held.

		// Note also that deleting does not unbind: if this buffer is still bound to
		// GL_ARRAY_BUFFER, GL keeps the name alive until it is unbound, so the
		// memory is not actually freed at this point.
		glDeleteBuffers(1, &m_RendererID);
	}

	// Const because binding mutates GL global state, not engine state: the same
	// buffer can be bound from a const handle.
	void OpenGLVertexBuffer::Bind() const
	{
		WL_PROFILE_FUNCTION();

		// The one call that matters. Binding the ARRAY_BUFFER is half of making a
		// vertex buffer usable -- the other half is the vertex array's attribute
		// setup, which lives in OpenGLVertexArray.
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
	}

	// Unbinds by binding 0, the conventional way to say "no vertex buffer".
	void OpenGLVertexBuffer::Unbind() const
	{
		WL_PROFILE_FUNCTION();

		// Deliberately binds the sentinel 0 rather than a stored previous binding.
		// Nothing here saves and restores a prior binding, so this clobbers whatever
		// was bound by an outer scope -- fine in a renderer that binds explicitly
		// before every draw, wrong for any code that assumed nesting.
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	// Partial update of an existing allocation.

	// DEFECT (minor, cosmetic): unlike every other function in this class, this
	// one omits WL_PROFILE_FUNCTION(). Both buffers' SetData methods are the
	// only entries without a profiler scope, so per-frame buffer uploads do not
	// appear in a capture -- which is usually exactly when you want them.
	void OpenGLVertexBuffer::SetData(const void* data, uint32_t size)
	{
		// Binds before updating. This is required: glBufferSubData operates on the
		// currently bound ARRAY_BUFFER, and the constructor's binding is long gone.

		// Which also means calling SetData on an unbound context -- or after some
		// other code left a different buffer bound -- writes into the wrong buffer.
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		// Offset 0, so this overwrites from the start of the buffer rather than
		// appending or updating a subrange. A partial-range update is not supported.

		// `size` is in bytes and is not clamped against the original allocation, so a
		// caller passing more than the constructor received triggers a GL_INVALID_
		//_VALUE, which -- as above -- nothing checks.
		glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Section banner for the index buffer half, matching the style of the
	// vertex buffer banner above.
	// Index Buffer //////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// Construct-and-upload. Unlike the vertex buffer there is no allocate-only
	// overload, so an index buffer is either created with data or updated later.

	// `indices == nullptr` is explicitly tolerated: see the usage-hint choice
	// two lines below. `count` is the ELEMENT count, and m_Count remembers it so
	// GetCount() can report the draw size without re-querying GL.
	OpenGLIndexBuffer::OpenGLIndexBuffer(uint32_t* indices, uint32_t count)
		// Member initialiser list. m_Count is captured at construction and never
		// revised, so a later SetData with a different element count leaves GetCount()
		// reporting the ORIGINAL count -- a real hazard for a dynamic mesh, which
		// would draw using a stale index count.
		: m_Count(count)
	{
		WL_PROFILE_FUNCTION();

		// Same DSA creation as the vertex buffer.
		glCreateBuffers(1, &m_RendererID);
		// The ELEMENT_ARRAY_BUFFER target, not ARRAY_BUFFER. This distinction is the
		// one that matters: an element-array binding is captured into a vertex array
		// object at glVertexAttribDivisor-free link time, whereas ARRAY_BUFFER is
		// pure global state.
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
		// Byte size derived from the element count. IndexBuffer is documented in
		// Buffer.h as 32-bit-only, so sizeof(uint32_t) is fixed at 4 and there is no
		// 16-bit index path.
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(uint32_t), indices,
			// The usage hint is chosen from whether data was supplied, not from how the
			// buffer will be used.

			// A null `indices` therefore means "allocate now, upload later via SetData",
			// which is exactly the dynamic case -- so the inference happens to be right.
			// It is still indirect: the hint is derived from the constructor argument
			// rather than stated by the caller, so a caller that allocates empty and
			// then uploads once-and-for-all gets a DYNAMIC hint for static data.

			// DEFECT: when indices is null the buffer is allocated but left uninitialised,
			// and m_Count is set to `count` regardless. If SetData is never called,
			// drawing uses uninitialised index data. Nothing guards that path.
			indices ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
	}

	// Mirrors the vertex buffer destructor; see the note there about the
	// unreset handle and the un-unbound deletion.
	OpenGLIndexBuffer::~OpenGLIndexBuffer()
	{
		WL_PROFILE_FUNCTION();

		glDeleteBuffers(1, &m_RendererID);
	}

	// NOTE: the header declares Bind and Unbind without `override` (unlike
	// SetData and GetCount, which do carry it). They still override -- the
	// signatures match IndexBuffer -- but a signature drift in the base class
	// would silently create an overload instead of an override, with no compiler
	// error.
	void OpenGLIndexBuffer::Bind() const
	{
		WL_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
	}

	void OpenGLIndexBuffer::Unbind() const
	{
		WL_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	// Same omission of WL_PROFILE_FUNCTION() as the vertex buffer's SetData.
	void OpenGLIndexBuffer::SetData(const void* data, uint32_t size)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
		// Offset-0 overwrite, same as the vertex buffer. Since the size argument is
		// bytes while GetCount() reports elements, a caller updating N elements must
		// remember to pass N*4 here -- the two numbers are not interchangeable and
		// nothing converts between them.
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, size, data);
	}

}