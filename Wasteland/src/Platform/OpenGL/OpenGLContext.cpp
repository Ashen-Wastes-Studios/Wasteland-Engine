// Precompiled header first: it establishes the glad GL loader and the
// WL_PROFILE_FUNCTION / WL_CORE_* macros before anything else uses them.
#include "wlpch.h"
// The class declaration. Note this header forward-declares GLFWwindow and does
// NOT include glfw3.h, so the real GLFW types only become available here.
#include "OpenGLContext.h"

// Needed for glfwMakeContextCurrent, glfwGetCurrentContext, glfwSwapBuffers and
// glfwGetProcAddress -- all called below.
//
// Including it in the .cpp rather than the header is deliberate: the header's
// comment says keeping GLFW confined here means callers cannot reach through
// this class to the platform layer.
#include <GLFW/glfw3.h>
// GL loader. Must come AFTER any real GL header inclusion and BEFORE any GL
// call: glad declares the gl* entry points this file uses (glGetString,
// glGetIntegerv) via the loader rather than linking against opengl32.lib.
#include <glad/glad.h>

namespace Wasteland {

	// Stores the borrowed window in the initialiser list. There is exactly one
	// constructor and no default, so m_WindowHandle is never indeterminate -- the
	// header flags this as a footgun that would return if a defaulted constructor
	// were ever added.
	//
	// Takes ownership of NOTHING. The platform window layer creates and destroys
	// the GLFWwindow; this class only points at it.
	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		// Debug-only. In a Release build this compiles away entirely and a null handle
		// is stored, which Init() below would then hand to glfwMakeContextCurrent --
		// GLFW tolerates a null window but the context never becomes current, and the
		// failure surfaces much later as every GL call going nowhere.
		//
		// An assert in a constructor body rather than in the initialiser list: by the
		// time it runs the member is already set, so a debugger shows the stored value.
		// It also means the object is constructed (and would be destructible) even in
		// the failing case.
		WL_CORE_ASSERT(windowHandle, "Window handle is null!");
	}


	// Virtual, matching GraphicsContext's, so deleting through the base pointer is
	// well-defined.
	//
	// Does NOT call glfwDestroyWindow: this class does not own the window, and the
	// platform layer will destroy it. Calling it here would be a double free.
	OpenGLContext::~OpenGLContext()
	{
		// Detach our context from the calling thread if it is current.
		// Destroying a window whose context is current (glfwDestroyWindow in
		// the GL -> NO_API switch path) is forbidden by GLFW and corrupts
		// driver state in nvoglv64.dll — the same binary that implements
		// Vulkan — crashing later Vulkan calls (seen as an AV in
		// nvrhi::vulkan::Queue::Queue's semaphore creation).
		// Two conditions, both necessary. The null test is defensive: see the header's
		// note that m_WindowHandle has no member initialiser.
		//
		// The identity test is the important one -- glfwMakeContextCurrent(nullptr) on
		// a thread that has some OTHER window's context current would detach that
		// context, breaking a context this object knows nothing about. Comparing against
		// the handle first means the detach happens only when it is ours to undo.
		//
		// `==` on two pointers is the documented GLFW idiom for context identity, not
		// a value comparison.
		if (m_WindowHandle && glfwGetCurrentContext() == m_WindowHandle)
			// Detaches the GL context from the CALLING thread. This is a per-thread
			// property in GL, not per-window, which is why the guard above had to compare
			// against the current thread's context.
			//
			// Required before glfwDestroyWindow in the editor's GL -> NO_API switch path.
			// The comment already in the file records the observed consequence of getting
			// this wrong: nvoglv64.dll (NVIDIA's combined GL/Vulkan ICD) ends up with
			// corrupted state and a later Vulkan semaphore creation faults.
			//
			// The context itself is not destroyed or freed -- only made non-current. GLFW
			// releases it when the window is destroyed.
			glfwMakeContextCurrent(nullptr);
		// Poisoning the member so a second destruction, or any use after this object
		// is gone, cannot reach a dangling window pointer. Assigning rather than
		// clearing is all that is available -- the type is a bare pointer.
		m_WindowHandle = nullptr;
	}

	// One-time setup, called after the window exists. Does three things: make the
	// context current, load GL entry points, then report and validate the version.
	//
	// Not idempotent in any meaningful sense -- calling it twice reloads glad and
	// re-prints the info block, which is harmless but noisy.
	void OpenGLContext::Init()
	{
		// RAII timing scope for everything below, attributed to this function's name.
		// Placed first so it covers the loader call, which is typically the expensive
		// part of startup.
		WL_PROFILE_FUNCTION();

		// Establishes the GL context on the CALLING thread. Must precede the glad load
		// below, because glad resolves entry points through glfwGetProcAddress and
		// GLFW will only answer that for a current context.
		//
		// No null check on the handle -- see the constructor's assert note; in Release a
		// null handle here means no current context and the loader call below fails.
		//
		// Also not here: any glfwSwapInterval call. Vsync is therefore whatever the
		// window layer configured, and this class has no say in it.
		glfwMakeContextCurrent(m_WindowHandle);
		// The loader entry point. gladLoadGLLoader takes a function pointer used to
		// resolve each gl* symbol, and GLFW supplies one.
		//
		// The C-style cast to GLADloadproc is required rather than optional:
		// glfwGetProcAddress has signature `GLFWglproc(const char*)` and GLADloadproc
		// is `void*(*)(const char*)`. The signatures differ only in return type, so
		// this cast is a reinterpretation of the function pointer -- correct here
		// because both return a pointer-sized value from the same driver table, but
		// it is not a type-safe conversion and a static_cast would not compile.
		//
		// `status` is an int, not bool: glad returns non-zero on success.
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		// Debug-only, and the second line of defence. A Release build with a failed
		// loader keeps going and every subsequent gl* call dereferences a null
		// function pointer -- the process dies on the first glGetString with no
		// diagnostic at all.
		//
		// This is the only check on loader success anywhere in the class; there is no
		// runtime error path.
		WL_CORE_ASSERT(status, "Failed to initialize Glad!");

		// Header for the driver identification block below. INFO rather than WARN so a
		// normal log level still shows which GPU the run picked up -- useful when
		// diagnosing a wrong-driver or software-rasteriser outcome.
		WL_CORE_INFO("OpenGL Info:");
		// glGetString returns a `const GLubyte*` -- a byte pointer to driver-owned
		// memory. The cast to const char* is the conventional spelling, and the engine's
		// format placeholder expects a char*.
		//
		// These three calls are unguarded: glGetString can return nullptr if there is no
		// current context, and passing that to a {0} formatter is undefined. The
		// preceding assert covers the Debug case only.
		WL_CORE_INFO(" Vendor: {0}", (const char*)glGetString(GL_VENDOR));
		WL_CORE_INFO(" Renderer: {0}", (const char*)glGetString(GL_RENDERER));
		WL_CORE_INFO(" Version: {0}", (const char*)glGetString(GL_VERSION));

// The version floor check is compiled out entirely in Release. That is a real
// limitation, documented in the header: a Release build on a pre-4.5 driver
// proceeds and fails later, at the first draw call that needs a 4.5 feature,
// with a much less obvious message.
//
// Preprocessor guard rather than a runtime check on WL_CORE_ASSERT's own
// behaviour, so the glGetIntegerv calls are not compiled either.
#ifdef WL_ENABLE_ASSERTS
		// Deliberately NOT initialised. Both are written by the glGetIntegerv pair
		// below before the assert reads them, so the uninitialised read cannot happen
		// -- but that makes the code fragile to reordering, and it would be free to
		// initialise them.
		int versionMajor;
		int versionMinor;
		// GL_MAJOR_VERSION / GL_MINOR_VERSION are core-profile-only tokens added in
		// GL 3.0. On a legacy context that does not support them the call generates
		// GL_INVALID_ENUM and leaves the variable untouched -- so the assert below would
		// then read an uninitialised value, which is the failure mode the missing
		// initialisers above make possible.
		//
		// &versionMajor is an int* matching GLint*, which is a 32-bit int on every GL
		// implementation, so this is the correct pointer type in practice.
		glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
		glGetIntegerv(GL_MINOR_VERSION, &versionMinor);

		// The version floor: 4.5, with the comparison written longhand because there is
		// no lexicographic compare on (major, minor) pairs.
		//
		// 4.5 is not arbitrary -- it is what Renderer2D_Quad.glsl's `#version 450 core`
		// demands, so this assert is the runtime half of that shader requirement.
		//
		// OpenGL 4.5 is also the first version with compute shaders and with
		// named texture units, both of which the engine uses.
		//
		// Debug-only, per the #ifdef above.
		WL_CORE_ASSERT(versionMajor > 4 || (versionMajor == 4 && versionMinor >= 5), "Wasteland requires at least OpenGL version 4.5!");
#endif
	}

	// The present. Called once per frame from the render loop's tail.
	//
	// Overridden rather than inherited because the base declares it pure virtual:
	// GL's present is a real API call, whereas D3D11/DX12 would need a present on
	// the swap chain object and Vulkan a queue present.
	void OpenGLContext::SwapBuffers()
	{
		WL_PROFILE_FUNCTION();

		// Hands the back buffer to the window system. Blocks when vsync is enabled,
		// which is the main reason this is wrapped in a profile scope -- a vsync-bound
		// frame shows up entirely as time in this call.
		//
		// No guard on m_WindowHandle or on there being a current context. As the header
		// notes, calling this before Init() hands GLFW a window with no current
		// context, which GLFW reports as an error rather than crashing.
		//
		// Does not touch the GL context binding, so a swap never detaches the context
		// that the destructor goes on to clean up.
		glfwSwapBuffers(m_WindowHandle);
	}

}