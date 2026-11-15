#include "wlpch.h"
#include "Application.h"

#include "Wasteland/Core/Log.h"

#include "Wasteland/Renderer/Renderer.h"
#include "Wasteland/Renderer/Renderer2D.h"
#include "Wasteland/Renderer/Renderer3D.h"
#include "Wasteland/Renderer/RendererAPI.h"
#include "Wasteland/Scene/BehaviorTreeSystem.h"
#include "Wasteland/Scene/AnimationSystem.h"
#include "Wasteland/Scene/FlowGraphSystem.h"
#include "Wasteland/Scripting/ScriptEngine.h"
#include "Platform/NVRHI/NVRHIContext.h"

#include "Input.h"

#include <GLFW/glfw3.h>

#include <cstdlib>   // std::getenv, for the WL_FRAME_TIMING harness hook
#include <thread>    // std::this_thread::sleep_for, the WLFG frame cap
#include <chrono>    // std::chrono::duration, the WLFG frame cap

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
// destroyed automatically after this body runs. Python-backed global state must
// be released here, while the interpreter is alive, before it is finalized.
Application::~Application()
{
	WL_PROFILE_FUNCTION();

	if (Py_IsInitialized())
	{
		// Active graph instances hold py::object handles in a static cache;
		// release them before Py_Finalize rather than from static destruction.
		BehaviorTreeSystem::Shutdown();
		// Flow graphs follow the same lifetime rule.
		FlowGraphSystem::Shutdown();
		AnimationSystem::Shutdown();
		// Scripted entity instances use the same lifetime rule. The module's
		// atexit hook covers Python-hosted tests; this handles the embedded app.
		ScriptEngine::Shutdown();

		// Release the handler before finalizing: s_PythonEventHandler is a static
		// py::object, and its destructor runs at process exit — long after
		// Py_Finalize() — which would Py_DECREF into a dead interpreter.
		//
		// The assignment resets the static to a null handle, dropping its reference
		// count NOW while the interpreter is still alive.
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

	// Per-section frame timing, for finding what actually bounds the frame rate.
	//
	// Why this exists: the renderer's own GPU timer reads ~211-253 ms while the
	// wall-clock frame time sits at ~213 ms in every configuration, so the GPU
	// number cannot be the whole story and `cpuSubmit` is 0.0 ms. These four
	// samples split the loop into the only places the time can go:
	//
	//   begin   -- Window::BeginFrame
	//   update  -- every layer's OnUpdate (the editor's scene update AND render)
	//   imgui   -- ImGui Begin + every layer's OnImGuiRender + End (the present)
	//   window  -- Window::OnUpdate, the OS event pump
	//
	// Reported every 100 frames, and only when WL_FRAME_TIMING=1, so it is inert
	// unless asked for. Uses glfwGetTime because that is what the loop already
	// uses for the timestep -- no second clock, no drift between them.
	double ftBegin = 0.0, ftUpdate = 0.0, ftImGui = 0.0, ftWindow = 0.0;
	// ImGui is split three ways because the first cut showed it holding ~all of
	// the frame time, and the interesting question is which part: the ImGui draw
	// itself, or the wait for the GPU that End()'s present implies.
	double ftImGuiBegin = 0.0, ftImGuiDraw = 0.0, ftImGuiEnd = 0.0;
	int ftFrames = 0;
	double ftLogTime = 0.0;
	const bool ftOn = (std::getenv("WL_FRAME_TIMING") != nullptr);
	double ftT0 = glfwGetTime();

	// WLFG synthetic frame cap (Step 1): an optional present interval, read from
	// WL_FRAME_CAP (frames per second; 0 or absent = uncapped). It is
	// INDEPENDENT of vsync -- a sleep-until-target at the end of the loop, not a
	// swapchain change. m_NextFrameTime is the next present's target time,
	// seeded to now so the first frame is never throttled.
	if (const char *fps = std::getenv("WL_FRAME_CAP"))
		SetFrameInterval(1.0 / std::atof(fps));
	m_NextFrameTime = glfwGetTime();

	// INSTRUMENTATION ONLY -- sim-clock scale, for verifying the WLFG drift
	// column below. The plan's Step 2 asks for timescale = 1/factor; that is
	// deliberately NOT wired (the premise is disproved by measurement -- see the
	// comment at the timestep, and the WLFG clock note in the task8 acceptance
	// doc). This override exists so the drift column can be SHOWN to work: set
	// WL_SIM_TIMESCALE=0.5 and the simDrift reading must go non-zero, which is
	// the failure the column exists to catch. It is not a feature and must not be
	// used to drive the sim in normal operation.
	if (const char *ts = std::getenv("WL_SIM_TIMESCALE"))
		SetSimTimescale((float)std::atof(ts));
	// Separate from ftLogTime: the WLFG timing line has its own 1-second cadence
	// and must not depend on WL_FRAME_TIMING being on.
	double wlfgLogTime = glfwGetTime();

	// Condition re-tested every iteration, and it is what Close() and
	// OnWindowClose clear.
	while (m_Running)
	{
		// Names the whole iteration in the profiler.
		WL_PROFILE_SCOPE("RunLoop");
		static bool s_FirstFrameLogged = false;
		if (!s_FirstFrameLogged)
		{
			s_FirstFrameLogged = true;
			WL_CORE_INFO("Application RunLoop: FIRST FRAME START");
		}

		// Seconds since GLFW was initialized. Cast to float because Timestep
		// holds a float; a long-running session loses precision here, but the
		// delta between consecutive frames stays accurate for any realistic
		// uptime.
		float time = (float)glfwGetTime();

		// Delta against the previous frame. NOT clamped: a debugger break or a
		// long asset load produces an arbitrarily large value, which every layer
		// then integrates. m_LastFrameTime starts at 0.0f, so the first frame's
		// delta is the entire time since process start.
		//
		// WLFG (Step 2): the sim clock is rawDelta * m_SimTimescale. The plan
		// specifies m_SimTimescale = 1/factor at a factor of N, on the reasoning
		// that N presents per real frame each advance the sim 1/N of the real
		// time, so the interpolated presents show the game at the correct speed.
		//
		// THAT REASONING DOES NOT HOLD, and SetSimTimescale is deliberately left
		// uncalled by C++ (it is exposed to Python only, for scripts that want
		// slow-motion). Measured on this machine, in BOTH pacing regimes:
		//
		//   uncapped:  WL_WLFG=1 -> 62.6 presents/s, WL_WLFG=6 -> 153.1  (2.45x)
		//   WL_FRAME_CAP=60:  both -> 60.6 presents/s                    (1.00x)
		//
		// So the factor does NOT multiply the present rate: uncapped the traced
		// presents still dominate the frame cost, and paced the cap holds both
		// factors to the same rate. The 6x claim is about the present/render
		// RATIO, not the absolute present rate, and this step conflates the two.
		//
		// Applying 1/factor would therefore advance the sim at 25.5 sim-steps/s
		// against 62.6 at factor 1 uncapped (~41% of real speed), or at 10.1
		// against 60.6 when paced (~17%, i.e. 1/6). Both are wrong.
		//
		// With timescale 1.0 the invariant that actually matters holds exactly:
		// the timestep is rawDelta * 1.0, and sum(rawDelta) over any wall-clock
		// window IS that window, so simulated time tracks real time at every
		// factor and every pacing setting. Do NOT "fix" this by wiring 1/factor --
		// see the WLFG clock note in
		// docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task8/TASK8_ACCEPTANCE.md.
		Timestep timestep = (time - m_LastFrameTime) * m_SimTimescale;
		// rawDelta is the same expression the timestep is built from, before the
		// scale. Read it BEFORE m_LastFrameTime is advanced, or the drift measures
		// zero against itself.
		const double rawDelta = (double)(time - m_LastFrameTime);
		m_LastFrameTime = time;

		// WLFG drift (Step 3). The accumulated difference between simulated and
		// real time, which is what the `simDrift` column on the WLFG timing line
		// reports.
		//
		// This is computed here rather than left at its 0.0 default: nothing used
		// to write it (SetWlfgStats only writes when handed driftMs >= 0, and no
		// caller ever passed one), so the column printed 0.0ms on every line of
		// every run and the plan's "the drift must stay bounded" had no
		// measurement behind it. A default that happens to be the expected answer
		// is not evidence.
		//
		// At m_SimTimescale == 1.0 the term is identically zero, and that IS the
		// correct reading: simulated time tracks real time. It only goes non-zero
		// if something scales the clock -- which is the case this column exists to
		// catch, since a wrong scale is otherwise invisible.
		//
		// VERIFIED: with the shipped timescale the column reads 0.0ms on every
		// line; with WL_SIM_TIMESCALE=0.5 (instrumentation override, above) it
		// reads -3149.5ms growing to -6665.0ms across a run. Read the TREND, not
		// the absolute: m_LastFrameTime starts at 0.0, so the first frame's
		// rawDelta is the whole time since process start and contributes a large
		// one-off term whenever the scale is not 1.0. At the shipped 1.0 that
		// artifact cancels exactly, which is why 0.0 there is clean.
		m_SimDriftMs += ((double)timestep.GetSeconds() - rawDelta) * 1000.0;

		// A minimized window has a zero-sized drawable, so the whole update and
		// render body is skipped. The loop still spins below, which keeps
		// pumping OS events so the window can be restored.
		if (!m_Minimized)
		{
			m_Window->BeginFrame();
			if (ftOn) { ftBegin += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }

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
			if (ftOn) { ftUpdate += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }

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
			if (std::getenv("WL_FRAME_TRACE")) { fprintf(stderr, "FRAMETRACE imgui-begin-done\n"); fflush(stderr); }
			if (ftOn) { ftImGuiBegin += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }

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
			if (std::getenv("WL_FRAME_TRACE")) { fprintf(stderr, "FRAMETRACE layerstack-imgui-done\n"); fflush(stderr); }
			if (ftOn) { ftImGuiDraw += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }

			// Renders and presents. For the NVRHI backends this is where the
			// recorded ImGui draw data reaches the GPU; for OpenGL it is also
			// where the scene is drawn, which is why the NVRHI branch above
			// submits separately.
			m_ImGuiLayer->End();
			if (std::getenv("WL_FRAME_TRACE")) { fprintf(stderr, "FRAMETRACE imgui-end-done\n"); fflush(stderr); }
			if (ftOn) { ftImGuiEnd += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }
			ftImGui = ftImGuiBegin + ftImGuiDraw + ftImGuiEnd;
		}

		// Pump the OS event queue: retrieve platform events, convert them, and
		// route each through Application::OnEvent.
		//
		// Deliberately OUTSIDE the minimized guard -- this must run every frame
		// or a minimized window stops responding to being restored.
		m_Window->OnUpdate();
		if (std::getenv("WL_FRAME_TRACE")) { fprintf(stderr, "FRAMETRACE window-update-done\n"); fflush(stderr); }
		if (ftOn) { ftWindow += glfwGetTime() - ftT0; ftT0 = glfwGetTime(); }

		// Report the split. Averaged over the last 100 frames so a single slow
		// frame (a shader compile, a scene load) does not dominate the reading.
		//
		// ftFrames MUST be reset here. It is the divisor for every section total,
		// and the section totals are reset with it -- an earlier version forgot
		// this, so every sample after the first was divided by a cumulative frame
		// count and reported a total four times too small at frame 400.
		if (ftOn && ++ftFrames >= 100)
		{
			WL_CORE_INFO("Frame sections (ms/frame): begin {0:.1f}  update {1:.1f}  imguiBegin {2:.1f}  imguiDraw {3:.1f}  imguiEnd {4:.1f}  window {5:.1f}  total {6:.1f}",
			             ftBegin * 1000.0 / ftFrames, ftUpdate * 1000.0 / ftFrames,
			             ftImGuiBegin * 1000.0 / ftFrames, ftImGuiDraw * 1000.0 / ftFrames,
			             ftImGuiEnd * 1000.0 / ftFrames, ftWindow * 1000.0 / ftFrames,
			             (ftBegin + ftUpdate + ftImGui + ftWindow) * 1000.0 / ftFrames);
			ftBegin = ftUpdate = ftImGui = ftWindow = 0.0;
			ftImGuiBegin = ftImGuiDraw = ftImGuiEnd = 0.0;
			ftFrames = 0;
			ftLogTime = glfwGetTime();
		}
		(void)ftLogTime;

		// WLFG real-frame accounting (Step 3). This used to be a blind increment:
		// Task 6 had no renderer wiring, so "one present == one real frame" was the
		// only available reading. It is NOT true once the factor-gated schedule
		// exists -- a generated present does not trace -- and leaving it here would
		// add 1 per present on top of the renderer's own count, so the ratio the
		// Step 6 acceptance is built on would be wrong by exactly one frame per
		// present.
		//
		// The renderer now owns BOTH counts (it is the only component that knows
		// whether the trace actually ran) and pushes them through SetWlfgStats()
		// each frame. At factor 1 this still yields realFrames == presents and
		// warps == 0, i.e. the same inert baseline the bench tool used to see.
		//
		// On a backend with no WLFG wiring (the OpenGL path never calls
		// SetWlfgStats) the counts stay 0, which is the honest reading: there is no
		// WLFG there, and WlfgMaxFactor() already forces the factor to 0.

		// WLFG timing line. Gated on WL_WLFG_TIMING so it is inert unless the
		// bench harness asks for it; its own 1-second cadence is independent of
		// WL_FRAME_TIMING. Reports the factor in force, the generated-present
		// count, the traced-present count, and the accumulated sim-vs-real drift
		// in ms. The renderer pushes factor/warps/realFrames through
		// SetWlfgStats() every frame; simDrift is the Application's own reading.
		//
		// These two counts are what the Step 6 acceptance is computed from:
		// present rate / render rate == realFrames / (realFrames - warps).
		if ((std::getenv("WL_WLFG_TIMING") != nullptr) && (glfwGetTime() - wlfgLogTime) >= 1.0)
		{
			WL_CORE_INFO("WLFG timing: factor {0} warps {1} realFrames {2} simDrift {3:.1f}ms",
			             m_WlfgFactor, m_WlfgWarps, m_WlfgRealFrames, m_SimDriftMs);
			wlfgLogTime = glfwGetTime();
		}

		// WLFG synthetic frame cap (Step 1). Sleeps until the next target present
		// when a cap is set; uncapped (m_FrameInterval <= 0) leaves the loop
		// running unthrottled, exactly as before. Independent of vsync. If the
		// loop fell behind the schedule, the target resets to now so a stall does
		// not leave a backlog of catch-up sleeps.
		if (m_FrameInterval > 0.0)
		{
			double now = glfwGetTime();
			if (now < m_NextFrameTime)
				std::this_thread::sleep_for(std::chrono::duration<double>(m_NextFrameTime - now));
			else
				m_NextFrameTime = now;
			m_NextFrameTime += m_FrameInterval;
		}
	}

	// RELEASE THE RENDERER BEFORE THE WINDOW AND ITS GRAPHICS CONTEXT DIE.
	//
	// Nothing used to tear the renderer down here, so the Nova backends -- file-scope
	// statics in Renderer3D.cpp -- were destroyed by the C runtime at process exit,
	// long after the NVRHI device had gone. On Vulkan that is fatal: destroying a
	// nvrhi::vulkan::BindingSet calls into the destroyed device's dispatch table and
	// faults inside nvoglv64.dll, so every clean run of the editor ended in an access
	// violation after the last frame. Shutdown releases those objects while the device
	// is still alive, which is the only order in which they can be released.
	Renderer3D::Shutdown();
	Renderer2D::Shutdown();
}

// WLFG sim clock (Step 2). Clamps a non-positive scale back to 1.0 so a bad
// value cannot freeze the simulation; the render loop is unaffected either way.
void Application::SetSimTimescale(float scale)
{
	m_SimTimescale = (scale > 0.0f) ? scale : 1.0f;
}

// WLFG synthetic frame cap (Step 1). <= 0 disables it (uncapped).
void Application::SetFrameInterval(double seconds)
{
	m_FrameInterval = (seconds > 0.0) ? seconds : 0.0;
}

// WLFG accounting (Step 3). The renderer calls this every frame with the factor
// in force and the two counts; the drift stays the Application's to own.
void Application::SetWlfgStats(int factor, std::uint64_t warps,
                               std::uint64_t realFrames, double driftMs)
{
	m_WlfgFactor = factor;
	m_WlfgWarps = warps;
	m_WlfgRealFrames = realFrames;
	// A negative drift means "not reported" -- the renderer has no sim clock, so
	// writing it here would reset the Application's own reading to zero every
	// frame and the drift column would always print 0.0.
	if (driftMs >= 0.0)
		m_SimDriftMs = driftMs;
}

// WLFG interpolation-factor ceiling (Step 4). The factor is a function of the
// accumulation state, not a constant: Wasteland's history IS its quality, so
// re-projecting a converged frame is nearly free and re-projecting a 1-sample
// frame multiplies noise.
//
//   converged (alpha at its 0.02 floor) + static camera + Nova backend -> 6x
//   partially converged -> 2x
//   camera moved / scene changed, or a non-Nova backend -> off (0)
int Application::WlfgMaxFactor(bool cameraMoved, float accumAlpha, bool novaBackend) const
{
	if (!novaBackend)   // no velocity buffer the warp can reproject against
		return 0;
	if (cameraMoved)    // reprojection is undefined; the incumbent is the truth
		return 0;
	if (accumAlpha <= 0.02f)  // the accumulation's floor marks a converged history
		return 6;
	return 2;           // still accumulating
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