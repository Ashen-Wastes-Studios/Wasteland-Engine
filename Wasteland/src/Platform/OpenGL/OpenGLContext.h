#pragma once

// The abstract base. It declares only two pure virtuals (Init,
// SwapBuffers) plus a defaulted BeginFrame, so an OpenGL subclass is
// forced to supply the two it cannot inherit.
#include "Wasteland/Renderer/GraphicsContext.h"

// Forward declaration, not an include. The ctor takes a GLFWwindow* but
// nothing in this header calls into GLFW, so naming the type is enough --
// the real header stays confined to OpenGLContext.cpp. Without this line
// the member below would be an incomplete type.
//
// Note it sits outside namespace Wasteland because that is where GLFW
// declares it; `struct` rather than `class` to match GLFW's own spelling,
// which keeps a mismatched-declaration diagnostic from firing.
struct GLFWwindow;

namespace Wasteland {

	// OpenGL/GLFW implementation of the graphics context: owns the binding
	// between a GLFW window and the GL function loader.
	//
	// Minimal by design. A Vulkan or D3D build gets a different subclass, and
	// Renderer.h dispatches on Renderer::GetAPI() the same way Texture2D::Create
	// does. There is no per-API switch inside this class -- the choice is made
	// once, where the context is constructed.
	class OpenGLContext : public GraphicsContext
	{
	public:
		// Takes ownership of nothing: the window is created and destroyed by the
		// platform window layer, and this class only borrows the handle. The
		// destructor therefore nulls the pointer rather than calling
		// glfwDestroyWindow, which would double-free against the real owner.
		//
		// No `override`/default member init on m_WindowHandle, so it must be set
		// through this one constructor -- there is no default ctor.
		OpenGLContext(GLFWwindow* windowHandle);
		// Virtual, matching GraphicsContext's virtual dtor, so deleting through a
		// GraphicsContext* is well-defined even though this class adds no new
		// owned resources of its own.
		//
		// Real work lives in the .cpp: it detaches the GL context from the calling
		// thread if current, which GLFW requires before the window is destroyed
		// (matters on the GL -> NO_API switch path).
		~OpenGLContext();

		// One-time setup, called after the window exists. Makes the context
		// current, loads GL entry points through glad, then logs vendor/renderer/
		// version and -- under WL_ENABLE_ASSERTS only -- asserts the context is at
		// least OpenGL 4.5.
		//
		// Note the version floor is a debug-only assertion, so a release build on
		// an older driver proceeds and fails later at draw time instead.
		virtual void Init() override;
		// Presents the back buffer. Called once per frame at the end of the
		// render loop; the profile macro here attributes its cost to the frame.
		//
		// Does not rebind or re-validate m_WindowHandle, so calling it before
		// Init() would hand GLFW a window with no current context.
		virtual void SwapBuffers() override;
	// Private rather than protected: no subclass needs to touch the handle,
	// and keeping it out of the derived interface stops callers from reaching
	// it through an OpenGLContext* and bypassing the facade.
	private:
		// The borrowed window. Raw pointer on purpose -- this class does not own
		// the window and must not free it.
		//
		// DEFECT: left uninitialized by any other path. There is exactly one
		// constructor and it takes the handle, so this is currently safe, but
		// adding a defaulted or default ctor later would read an indeterminate
		// value in the dtor's `m_WindowHandle && ...` test. Initialising it to
		// nullptr (as Input.h's neighbouring members do elsewhere in the codebase)
		// would remove that footgun.
		GLFWwindow* m_WindowHandle;
	// No BeginFrame override: the base's empty default is correct for GL, where
	// a frame needs no explicit begin compared with D3D11/D3D12 command-list
	// recording.
	};

}