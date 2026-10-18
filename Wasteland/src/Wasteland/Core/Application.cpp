#include "wlpch.h"
#include "Application.h"

#include "Wasteland/Core/Log.h"

#include "Wasteland/Renderer/Renderer.h"
#include "Wasteland/Renderer/RendererAPI.h"
#include "Platform/NVRHI/NVRHIContext.h"

#include "Input.h"

#include <GLFW/glfw3.h>

#include <pybind11/pybind11.h>
#include <Python.h>

namespace py = pybind11;

namespace Wasteland
{

// Local variant of WL_BIND_EVENT_FN (see Core/Core.h) that binds an
// Application member instead of a Layer member. The core macro hard-codes
// `this` but is otherwise identical, and it lives in Core.h which this file
// already includes -- this duplicate exists so the binding target is explicit
// at the call sites below.
#define BIND_EVENT_FN(x) std::bind(&Application::x, this, std::placeholders::_1)

// The Python callable registered to receive engine events, or a null handle
// when none is.
//
// Process-global rather than a member of Application: the setter is an
// extern "C" entry point called from the Python extension module, which has no
// handle on the Application instance.
static py::object s_PythonEventHandler;

// Registers (or clears) the Python event handler. Callable from Python via the
// module's C API.
//
// Passing None, or a null object, clears the handler -- see HasPythonEventHandler
// for why clearing needs more than resetting this to a default-constructed object.
//
// extern "C" so the name is not C++-mangled and can be found by dlsym/GetProcAddress
// from the Python side.
extern "C" void Wasteland_SetPythonEventHandler(py::object handler)
{
	// Move-assigned: py::object is a reference-counted handle, so the move
	// avoids a refcount bump-and-drop and leaves `handler` empty. Safe even if
	// the caller passes the current handler back in.
	s_PythonEventHandler = std::move(handler);
}

// Returns the registered handler, or None if there is not one.
//
// extern "C" for the same dlsym reason as the setter. The caller receives a
// new reference to the handler object; it does not own it.
extern "C" py::object Wasteland_GetPythonEventHandler()
{
	// The ternary converts a null handle to None. Note this tests truthiness
	// only, so a handler that was explicitly set to None comes back as None
	// here without HasPythonEventHandler's extra is_none() check.
	return s_PythonEventHandler ? s_PythonEventHandler : py::none();
}

// A default-constructed py::object is a NULL handle, not None. Testing only
// is_none() lets an unregistered handler through, and the first event then
// dies casting Event& to an unregistered type (cast.h:2301).
//
// All three conditions must hold before an event is handed to Python:
//   1. the handle is non-null (a handler was ever registered),
//   2. it is not None (so passing None to the setter really disables it), and
//   3. the interpreter is still up, since dispatch touches the GIL and
//      Python objects below.
static bool HasPythonEventHandler()
{
	return static_cast<bool>(s_PythonEventHandler) && !s_PythonEventHandler.is_none()
		&& Py_IsInitialized();
}

// The handler is invoked from inside GLFW's C window procedure
// (win32_window.c). No exception may escape OnEvent: unwinding a C++ exception
// through a C callback frame is undefined behaviour and kills the process.
//
// Every catch below exists to enforce that rule -- the function has no
// exception path out of it at all.
static void DispatchPythonEvent(Event &e)
{
	// Take the GIL for the duration. Scoped, so it is released on every exit
	// path including the catches. Required because this runs on the render
	// thread, which does not hold it.
	py::gil_scoped_acquire gil;
	try
	{
		// Event is abstract, so it has no copy constructor. The default
		// automatic_reference policy degrades to `copy` for an lvalue reference
		// (type_caster_base.h:1640) and throws. Pin it to `reference` so the
		// Python object aliases the real event and `Handled` writes land on it.
		py::object event = py::cast(&e, py::return_value_policy::reference);

		// cast() succeeding means the concrete Event type is registered in
		// Bindings.cpp. If it is not, the cast fails and sets a Python error.
		if (event)
		{
			// Invoke the Python callable with the event. No argument checking
			// beyond the cast above -- a handler with the wrong signature raises
			// TypeError here, which the catch below turns into a log line.
			s_PythonEventHandler(event);
		}
		else
		{
			// py::cast already set a Python TypeError ("Unregistered type").
			// Log which event failed, naming it via GetName() so the log is
			// actionable: it means the type is missing from the bindings.
			WL_CORE_ERROR("Python event handler: cannot bind {0} to Python "
						  "(type not registered)",
						  e.GetName());
			// Clear the pending error. Required: leaving it set makes the next
			// unrelated Python call fail with a confusing "SystemError: <built-in
			// function> returned a result with an error set".
			PyErr_Clear();
		}
	}
	// A Python exception (ValueError, TypeError, ...) propagating out of the
	// handler. Reported and swallowed -- see the note above about not letting
	// exceptions cross into C.
	catch (const py::error_already_set &err)
	{
		// The ctor used PyErr_Fetch, so the indicator is already clear and
		// what() is safe under the GIL we still hold here.
		WL_CORE_ERROR("Python event handler raised on {0}: {1}", e.GetName(), err.what());
	}
	// Any other standard exception. py::cast_error, py::type_error, ... — not a
	// Python exception, but a failed cast may still have left the error
	// indicator set.
	catch (const std::exception &err)
	{
		WL_CORE_ERROR("Python event dispatch failed on {0}: {1}", e.GetName(), err.what());
		PyErr_Clear();
	}
	// Anything else, including a non-std::exception type thrown from Python via
	// pybind11.
	catch (...)
	{
		WL_CORE_ERROR("Python event dispatch failed on {0}: unknown exception", e.GetName());
		PyErr_Clear();
	}
}

// The singleton pointer, defined here so the header's declaration has storage.
//
// Initialized to nullptr rather than left indeterminate, so Application::Get
// has a defined (if fatal) answer before construction rather than reading
// garbage.
Application *Application::s_Instance = nullptr;

// The Python extension module's init function, defined in Bindings.cpp.
// Declared here so the interpreter can be told about the "Wasteland" module
// before Py_Initialize runs.
extern "C" PyObject *PyInit_Wasteland(void);

// Constructs the application: embeds Python, creates the window, initializes
// the renderer, and installs the ImGui layer.
//
// The ordering below is load-bearing. Python comes up first because the
// renderer and window may consult it; the window before the renderer because
// the renderer needs a graphics context; ImGui last because it needs both.
Application::Application(const std::string &name)
{
	// Names this function in the profiler output, so a slow constructor is
	// attributable.
	WL_PROFILE_FUNCTION();

	// Register the engine's Python module in the inittab BEFORE Py_Initialize.
	// This is the only point at which the table can be modified: afterwards the
	// interpreter has already built its module cache, and an import of
	// "Wasteland" would fail.
	PyImport_AppendInittab("Wasteland", &PyInit_Wasteland);

	// Points the embedded interpreter at a Python installation.
	//
	// NOTE: this is a hard-coded absolute path into one specific user's home
	// directory. It will not resolve on any other machine, in CI, or for any
	// other developer, and the resulting failure is a silent fallback to
	// whatever Python the build happens to find. It should come from an
	// environment variable or a build setting.
	Py_SetPythonHome(L"C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64");

	// Starts the interpreter. Idempotent in practice -- a second call is a
	// no-op -- but the matching Py_Finalize in the destructor is not balanced
	// against it if construction throws between here and the end.
	Py_Initialize();

	// Left-in debug output: prints sys.path on every startup. Useful while
	// diagnosing import failures, noisy otherwise, and it runs before any
	// project or window setup so it cannot be suppressed by a quiet flag.
	PyRun_SimpleString("import sys; print('Python sys.path:', sys.path)");

	// Guards against a second Application. Assert rather than a runtime check:
	// the invariant is a programming error, not a recoverable condition, and in
	// release the assert compiles away leaving s_Instance simply overwritten.
	WL_CORE_ASSERT(!s_Instance, "Application already exists");

	// Publish this instance so Application::Get works from here on.
	s_Instance = this;

	// 0x0 means "size me to the user's primary monitor", which the
	// platform windows resolve once GLFW is up. The old default was a
	// fixed 1280x720, so on a 2560x1440 display the editor opened as a
	// letterboxed quarter of the screen with dead space around it.
	//
	// Wrapped in a unique_ptr because Window::Create returns a raw pointer and
	// Application owns the window for its whole lifetime.
	m_Window = std::unique_ptr<Window>(Window::Create(WindowProps(name, 0, 0)));

	// Route every platform event into this Application's OnEvent. Set before
	// the renderer is initialized, so an event raised during renderer startup
	// still arrives.
	m_Window->SetEventCallback(BIND_EVENT_FN(OnEvent));

	// Brings up the rendering backend. Must follow window creation: the
	// renderer acquires a graphics context from the window's current context.
	Renderer::Init();

	// The ImGui layer is allocated with new and pushed as an OVERLAY, not a
	// regular layer. Overlay means it sits after every other layer in
	// m_LayerStack, so its panels draw last and appear on top of the game.
	//
	// Ownership passes to the LayerStack, which deletes it -- hence the bare
	// new here is correct rather than a leak.
	m_ImGuiLayer = new ImGuiLayer();
	PushOverlay(m_ImGuiLayer);
}

// Tears down the application.
//
// The window and layer stack need no explicit cleanup: they are members and are
// destroyed automatically after this body runs. What this body must do is
// anything that has to happen while those members are still alive -- here,
// only the Python handler release.
Application::~Application()
{
	WL_PROFILE_FUNCTION();

	// Release the handler before finalizing: s_PythonEventHandler is a static
	// py::object, and its destructor runs at process exit — long after
	// Py_Finalize() — which would Py_DECREF into a dead interpreter.
	//
	// The assignment resets the static to a null handle, dropping its reference
	// count NOW while the interpreter is still alive.
	if (Py_IsInitialized())
	{
		// The GIL is needed to touch a py::object's refcount. Scoped, so it is
		// released before Py_Finalize below -- finalizing with the GIL held
		// deadlocks.
		py::gil_scoped_acquire gil;
		s_PythonEventHandler = py::object();
	}

	// Shuts the interpreter down. Note the ordering consequence: members are
	// destroyed AFTER this line, so any layer destructor that touches Python
	// runs against a finalized interpreter.
	Py_Finalize();
}

// Adds a regular layer and attaches it.
//
// @param layer  Layer to take ownership of; the stack deletes it.
void Application::PushLayer(Layer *layer)
{
	WL_PROFILE_FUNCTION();

	// Insert into the stack first, then attach. The order matters: a layer's
	// OnAttach may push further layers or query the stack, so it must already
	// be present when it runs.
	//
	// This is also why the hook lives here rather than in LayerStack::PushLayer
	// -- see the call-graph note in Layer.h.
	m_LayerStack.PushLayer(layer);
	layer->OnAttach();
}

// Adds an overlay and attaches it.
//
// Identical to PushLayer except for insertion position: overlays go after all
// regular layers, so they update and draw last.
//
// @param layer  Overlay to take ownership of; the stack deletes it.
void Application::PushOverlay(Layer *layer)
{
	WL_PROFILE_FUNCTION();

	m_LayerStack.PushOverlay(layer);
	layer->OnAttach();
}

// Requests that Run's loop exit after the current iteration.
//
// Sets the flag rather than calling exit() or throwing, so the caller returns
// to the frame loop normally and every layer still gets its OnDetach via the
// shutdown path. Safe to call from inside a layer's OnUpdate: the loop
// condition is re-tested after the current frame completes.
void Application::Close()
{
	m_Running = false;
}

// Routes one event: window events first, then layers in reverse order, then
// Python.
//
// @param e  The event to dispatch. May be marked handled to stop layer
//           propagation.
void Application::OnEvent(Event &e)
{
	WL_PROFILE_FUNCTION();

	// Typed routing for the two window events the engine itself acts on. Each
	// Dispatch call runs its handler only if the event is that type, and the
	// bool each returns is discarded here -- see the note on the layer loop
	// below about the handled flag.
	EventDispatcher dispatcher(e);
	dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(OnWindowClose));
	dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FN(OnWindowResize));

	// Offer the event to every layer, from the top of the stack downward, so
	// layers added later see an event first and can consume it before the
	// layers beneath.
	//
	// Written as a manual reverse iterator rather than a range-for because
	// rbegin/rend would need the stack to expose them.
	for (auto it = m_LayerStack.end(); it != m_LayerStack.begin();)
	{
		// Pre-decrement, then dereference: steps back one layer and yields it.
		// The loop condition guarantees the decrement cannot pass begin().
		(*--it)->OnEvent(e);

		// Stop once a layer has consumed the event.
		//
		// NOTE: as currently written this can never be true. The only place in
		// the engine that assigns Event::Handled is the second, unreachable
		// type-check block inside EventDispatcher::Dispatch, and no other code
		// assigns it. Dispatch's reachable block writes the protected
		// m_Handled instead, which nothing in the engine ever reads. So in
		// practice every layer receives every event, and no layer can consume
		// one. See Events/Event.h for the full explanation.
		if (e.Handled)
			break;
	}

	// Dispatch to Python event handler if registered
	//
	// Last, so a native layer always gets first refusal. Also outside the
	// reverse loop, so it runs once per event rather than once per layer.
	if (HasPythonEventHandler())
	{
		DispatchPythonEvent(e);
	}
}

// The main frame loop. Runs until Close() is called or the window is closed.
//
// Per iteration, in order:
//
//   1. compute the Timestep from glfwGetTime
//   2. if not minimized:
//        a. Window::BeginFrame
//        b. every layer's OnUpdate, forward order
//        c. if the backend is NVRHI: bail out on device loss, else submit the
//           recorded command list
//        d. ImGui Begin, every layer's OnImGuiRender, ImGui End
//   3. Window::OnUpdate (pump the OS event queue)
void Application::Run()
{
	WL_PROFILE_FUNCTION();

	// Condition re-tested every iteration, and it is what Close() and
	// OnWindowClose clear.
	while (m_Running)
	{
		// Names the whole iteration in the profiler.
		WL_PROFILE_SCOPE("RunLoop");

		// Seconds since GLFW was initialized. Cast to float because Timestep
		// holds a float; a long-running session loses precision here, but the
		// delta between consecutive frames stays accurate for any realistic
		// uptime.
		float time = (float)glfwGetTime();

		// Delta against the previous frame. NOT clamped: a debugger break or a
		// long asset load produces an arbitrarily large value, which every layer
		// then integrates. m_LastFrameTime starts at 0.0f, so the first frame's
		// delta is the entire time since process start.
		Timestep timestep = time - m_LastFrameTime;
		m_LastFrameTime = time;

		// A minimized window has a zero-sized drawable, so the whole update and
		// render body is skipped. The loop still spins below, which keeps
		// pumping OS events so the window can be restored.
		if (!m_Minimized)
		{
			m_Window->BeginFrame();

			{
				// Times the update pass as one block, so its cost is
				// attributable in the profile.
				WL_PROFILE_SCOPE("LayerStack OnUpdate");
				// Forward order, so layers update in the sequence they were
				// pushed -- a layer that produces state another layer reads must
				// be pushed first.
				for (Layer *layer : m_LayerStack)
					layer->OnUpdate(timestep);
			}

			// Flush NVRHI scene rendering before ImGui (scene must be behind UI)
			//
			// Only for the NVRHI backends. The OpenGL path renders during
			// ImGuiLayer::End() instead, so this whole block is skipped for it.
			//
			// The ordering is not incidental: the scene must be submitted and
			// presented before ImGui draws, or the UI ends up underneath the
			// scene and is invisible.
			if (RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX11 ||
				RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX12 ||
				RendererAPI::GetAPI() == RendererAPI::API::NVRHI_Vulkan)
			{
				// dynamic_cast rather than static_cast: GetCurrentContext is
				// declared to return a GraphicsContext*, and only the NVRHI
				// backend actually produces an NVRHIContext. Yields nullptr on a
				// backend mismatch, which the next test handles.
				auto *nvrhiCtx = dynamic_cast<NVRHIContext *>(m_Window->GetCurrentContext());
				if (nvrhiCtx)
				{
					if (nvrhiCtx->IsDeviceLost())
					{
						// Device is dead (TDR, driver crash, GPU reset). Skip
						// the entire frame — don't render scene or ImGui.
						// Attempt Vulkan recovery; DX11/DX12 need a restart.
						//
						// Attempt recovery, then still pump the OS event queue
						// before continuing -- a lost device must not also stall
						// window input, or the window becomes unresponsive on top
						// of being blank.
						nvrhiCtx->RecoverDevice();
						m_Window->OnUpdate();
						// Skip the rest of this frame, including the
						// m_Window->OnUpdate() at the loop's end, since it was
						// just called here.
						continue;
					}

					// Submits the command list the update pass recorded into.
					// Without this the scene never reaches the GPU.
					nvrhiCtx->ExecuteNVRHICommandList();
				}
			}

			// Open the ImGui frame. Must follow the scene submission above so
			// the UI composites on top of the already-rendered scene.
			m_ImGuiLayer->Begin();

			// Draw pass, gated on a backend actually existing. A headless or
			// backend-less build can construct the ImGuiLayer without one, and
			// calling the draw callbacks then would touch no valid context.
			if (m_ImGuiLayer->HasBackend())
			{
				WL_PROFILE_SCOPE("LayerStack OnImGuiRender");
				// Forward order -- the SAME direction as OnUpdate, not reverse.
				// So a panel drawn by an earlier layer is painted OVER by one
				// drawn later. See the ordering note in Layer.h.
				for (Layer *layer : m_LayerStack)
					layer->OnImGuiRender();
			}

			// Renders and presents. For the NVRHI backends this is where the
			// recorded ImGui draw data reaches the GPU; for OpenGL it is also
			// where the scene is drawn, which is why the NVRHI branch above
			// submits separately.
			m_ImGuiLayer->End();
		}

		// Pump the OS event queue: retrieve platform events, convert them, and
		// route each through Application::OnEvent.
		//
		// Deliberately OUTSIDE the minimized guard -- this must run every frame
		// or a minimized window stops responding to being restored.
		m_Window->OnUpdate();
	}
}

// Handles a close request by stopping the frame loop.
//
// Called through EventDispatcher, so it only runs for a WindowCloseEvent.
//
// @param e  The close event. Unused -- its existence is the signal.
// @returns true. The value is discarded by the caller in OnEvent, so it has no
//          effect today; it would matter if the layer early-out worked, since
//          this returning true is intended to consume the event before it
//          reaches the layers below.
bool Application::OnWindowClose(WindowCloseEvent &e)
{
	// The window is going away, so the loop must end; Close() is not used here
	// because this path is already inside the loop and only needs the flag.
	WL_CORE_INFO("OnWindowClose received! Shutting down app.");
	m_Running = false;

	// Always true, so a close is always treated as consumed.
	return true;
}

// Handles a window resize, tracking the minimized state and resizing the
// renderer.
//
// Called through EventDispatcher, so it only runs for a WindowResizeEvent.
//
// @param e  Carries the new size. A minimized window reports 0x0 on Windows,
//          which is the signal used below.
// @returns false, so the event is not consumed here and still reaches the
//          layers -- a panel may want to react to the resize too.
bool Application::OnWindowResize(WindowResizeEvent &e)
{
	WL_PROFILE_FUNCTION();

	// A 0x0 drawable means minimized. Record it and stop: there is nothing to
	// resize a renderer to, and passing 0 through would produce an invalid
	// framebuffer.
	//
	// Returning false lets the event continue to the layers regardless.
	if (e.GetWidth() == 0 || e.GetHeight() == 0)
	{
		m_Minimized = true;
		return false;
	}

	// Non-zero size: definitely not minimized. Set before the resize so a
	// failure inside the renderer cannot leave the flag wrongly true and the
	// frame loop permanently skipping its body.
	m_Minimized = false;

	// Rebuild the framebuffer-sized resources -- swapchain, render targets,
	// and the ImGui viewport. Expensive, and it happens on the platform event
	// callback rather than on the frame thread.
	Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());

	// Not consumed: layers below still see the resize.
	return false;
}

}