// First by convention: the platform prefix, which is what makes the GLFW and
// Win32 declarations below available without this file naming them.
#include "wlpch.h"
// The class declaration, including the WindowData struct that every GLFW lambda
// below reaches through glfwGetWindowUserPointer.
#include "WindowsWindow.h"

// KeyPressed, KeyReleased and KeyTyped. GLFW reports a key as a code plus an
// action; the engine events split that into three types.
#include <Wasteland/Events/KeyEvent.h>
// Buttons, scroll and motion.
#include <Wasteland/Events/MouseEvent.h>
// Resize and close, the two window-lifetime events this file produces.
#include <Wasteland/Events/ApplicationEvent.h>
// Only for Application::IsInitialized() and Get().GetImGuiLayer(). Both are
// guarded at every use below, because this file runs during window construction,
// long before an Application exists.
#include <Wasteland/Core/Application.h>
// The owner of the ImGui renderer backend. Two methods are used here,
// ShutdownBackendForWindowRecreate and SwitchBackend, and the ordering of those
// calls relative to context destruction is the subject of the long comment in
// ExecuteSwitch.
#include <Wasteland/ImGui/ImGuiLayer.h>

// The OpenGL context, one of the two mutually exclusive contexts this window can
// own. Constructed with the GLFWwindow it will attach to.
#include "Platform/OpenGL/OpenGLContext.h"
// The NVRHI context, which covers Vulkan and both DirectX backends behind one
// class and takes the target API as a constructor argument.
#include "Platform/NVRHI/NVRHIContext.h"
// Needed for ClearCachedResources and SetContext. Both are reached through a
// dynamic_cast from the RenderCommand-held API pointer, so this include is what
// makes the cast a runtime question rather than a compile error.
#include "Platform/NVRHI/NVRHIRendererAPI.h"

// The process-wide renderer API. This window keeps m_CurrentAPI in step with it
// deliberately, and the mismatch that would follow from not doing so is the
// one-frame bug recorded at the SetAPI call in ExecuteSwitch.
#include <Wasteland/Renderer/RenderCommand.h>

// The OpenGL entry points. Loaded by glad inside OpenGLContext::Init, not here,
// which is why nothing in this file calls a GL function directly.
#include <glad/glad.h>
// The windowing backend. Every GLFW call is spelled out here rather than wrapped
// in a helper, because this class is itself the thin owner around GLFWwindow.
#include <GLFW/glfw3.h>

// The engine namespace, matching the rest of the platform layer.
namespace Wasteland
{

	// Process-wide GLFW state, deliberately file-static rather than a member: GLFW
	// is a singleton, so initialising it per window would be wrong. DEFECT: it is
	// never reset to false, because glfwTerminate is never called -- see the TODO in
	// Init. The two facts currently agree, so nothing breaks, but a future
	// glfwTerminate that did not clear this flag would make every later window skip
	// glfwInit.
	static bool s_GLFWInitialized = false;

	// Routes GLFW's own diagnostics into the engine log, so a GLFW failure appears
	// in the same place as an engine failure. DEFECT: it is installed inside the
	// s_GLFWInitialized block in Init, AFTER the glfwInit call, so any error raised
	// by glfwInit itself goes to GLFW's default handler instead and is lost.
	static void GLFWErrorCallback(int error, const char *description)
	{
		// Error level, which is appropriate: everything GLFW routes here is a failure the
		// application has to notice.
		WL_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

#if defined(WL_PLATFORM_WINDOWS)
	// The platform factory, compiled only on Windows so the other backends can
	// supply their own.
	Window *Window::Create(const WindowProps &props)
	{
		// A raw pointer to a class with a virtual destructor. Ownership passes to the
		// caller by the base class's contract; nothing here deletes it, and the only
		// teardown path is ~WindowsWindow calling Shutdown.
		return new WindowsWindow(props);
	}
#endif

	// Construction is a profile scope plus Init. There is no separate Create step,
	// so a half-built window is never observable by a caller.
	WindowsWindow::WindowsWindow(const WindowProps &props)
	{
		WL_PROFILE_FUNCTION();

		// Everything else. Init calls SwitchRendererAPI below, which calls back into
		// this object, so the vtable is already live at this point.
		Init(props);
	}

	// Destruction is a profile scope plus Shutdown.
	WindowsWindow::~WindowsWindow()
	{
		WL_PROFILE_FUNCTION();

		// Called exactly once, from here. Shutdown is private and there is no public
		// close path, which is what keeps the double-destroy noted in that function from
		// being reachable.
		Shutdown();
	}


	// WindowProps width/height of 0 means "use the primary monitor".
	//
	// GLFW is asked AFTER glfwInit, because glfwGetPrimaryMonitor returns
	// null before initialisation and the original ordering silently fell
	// through to the 1280x720 default.
	//
	// The video mode is the FULL monitor resolution, not the work area:
	// the request was "default to the resolution of the user's monitor", and
	// rcWork would shrink it by the taskbar height.
	//
	// Note on a DPI-scaled display: this returns PHYSICAL pixels. A 3840x2160
	// panel at 150% scaling reports 3840x2160 here while a DPI-aware tool
	// (PowerShell's Screen.AllScreens, for instance) reports the 2560x1440
	// logical size. Those are the same display, not a disagreement -- and
	// seeing the smaller number from a DPI-aware tool is not evidence that
	// GLFW picked the wrong monitor.
	// Fills in whichever dimensions the caller left at zero. File-static because it
	// touches no member state -- it needs only GLFW, which the block comment above
	// explains must already be initialised when this runs.
	static void ResolveMonitorSize(uint32_t &width, uint32_t &height)
	{
		// The PRIMARY monitor specifically, not the one a window would open on: there is
		// no window yet, so there is nothing to be on. A multi-monitor user gets the
		// primary panel's mode regardless of where their other displays are.
		if (GLFWmonitor *monitor = glfwGetPrimaryMonitor())
		{
			// The monitor's current video mode. Returns null for a monitor GLFW cannot
			// query, which is handled by falling through to the fixed defaults below.
			const GLFWvidmode *mode = glfwGetVideoMode(monitor);
			// Every field guarded, not just the pointer: a zero reported by the driver would
			// otherwise reach glfwCreateWindow and be rejected there.
			if (mode != nullptr && mode->width > 0 && mode->height > 0)
			{
				// The cast is from GLFW's int to uint32_t. No negative value is possible because
				// of the guard above.
				width = static_cast<uint32_t>(mode->width);
				// Full panel resolution rather than the work area, which is the decision the
				// block comment above this function records.
				height = static_cast<uint32_t>(mode->height);
				return;
			}
		}

		// No display (headless/CI): keep the historical default rather than
		// creating a zero-sized window, which is invalid and fails an assert
		// later.
		// DEFECT: the two halves are treated independently, but the only caller invokes
		// this function when EITHER dimension is zero and both are then overwritten. So a
		// request for 1920x0 is answered with the monitor's width, not 1920. A partially
		// specified size cannot be expressed through the current API.
		if (width == 0)
			// Applied only to the dimension still at zero, which is why the two guards are
			// separate -- but the early return above means both are normally zero together.
			width = 1280;
		if (height == 0)
			height = 720;
	}


	// Everything the constructor does. Private and non-virtual in the header, so
	// there is exactly one implementation of window creation on this platform.
	void WindowsWindow::Init(const WindowProps &props)
	{
		WL_PROFILE_FUNCTION();

		// Copied into the engine-side WindowData rather than read from props later.
		// The props argument is a by-value const reference owned by the caller, which
		// may be a temporary -- as it is at the Window::Create call site. Keeping only
		// the fields copied here is what makes that safe.
		m_Data.Title = props.Title;
		// Zero is the documented 'use the monitor' sentinel and is resolved a few lines
		// below, after GLFW exists.
		m_Data.Width = props.Width;
		// Same sentinel, same deferred resolution.
		m_Data.Height = props.Height;

		// 0 means "the user's monitor"; GLFW is initialised by the
		// caller before we get here, so the query is valid now.

		// GLFW is initialised here, by this class, and nowhere else. That is the fact the
		// stale comment above contradicts.
		if (!s_GLFWInitialized)
		{
			// The profiler scope is entered before the call and ends at the closing brace
			// below, so it also covers the error-callback installation.
			WL_PROFILE_SCOPE("glfwInit");
			// DEFECT, and a deliberate one: GLFW is never terminated, so s_GLFWInitialized and
			// the absence of a terminate call agree today. But it also means the GLFW library
			// stays initialised for the life of the process, and the join threads and loaded
			// driver DLLs that glfwTerminate would release are never released.
			// TODO: glfwTerminate on system shutdown
			// The result is captured rather than asserted inline, because the error callback
			// is installed immediately afterwards and the assert message below is more
			// informative than GLFW's own.
			int success = glfwInit();
			// Installed AFTER glfwInit, which is the defect noted on this function's
			// definition: an init failure is reported to GLFW's default handler.
			glfwSetErrorCallback(GLFWErrorCallback);
			// DEFECT, and a serious one in release: Core.h defines WL_CORE_ASSERT to nothing
			// when NDEBUG is set, and the header comment there warns that the arguments are
			// not even evaluated. So in a release build a failed glfwInit is ignored and
			// s_GLFWInitialized is still set to true below, leaving every later GLFW call in
			// this file running against an uninitialised library.
			WL_CORE_ASSERT(success, "Could not initialize GLFW!");

			// Set unconditionally, including after the assert above has done nothing in
			// release. This is the second half of the defect noted there.
			s_GLFWInitialized = true;
		}

		// Monitor query needs glfwInit to have run, so it goes here and not
		// above: before init glfwGetPrimaryMonitor returns null and the
		// window silently reverts to the 1280x720 fallback.
		// DEFECT: the OR is what makes a partial request impossible -- see the note on
		// the first guard inside ResolveMonitorSize. The AND that would preserve a
		// specified dimension is not what is written here.
		if (m_Data.Width == 0 || m_Data.Height == 0)
			// Called with both fields, and overwrites both, so the sentinel test and the
			// assignment disagree about whether the halves are independent.
			ResolveMonitorSize(m_Data.Width, m_Data.Height);

		// Logged here, not above: this line used to fire before the monitor
		// query and so printed the 0x0 sentinel rather than the real size.
		// Logged after the resolution on purpose, as the comment above explains: before
		// that reordering this line printed the 0x0 sentinel instead of the real size.
		WL_CORE_INFO("Creating window {0} ({1}, {2}){3}", m_Data.Title,
			m_Data.Width, m_Data.Height,
			// Compares against props to decide whether to append a ' (monitor)' marker. DEFECT
			// (cosmetic): the comparison is against the raw request, so a caller that asked
			// for exactly the monitor's resolution is still labelled as having used the
			// monitor.
			(m_Data.Width == props.Width && m_Data.Height == props.Height)
				? ""
				: " (monitor)"
			);

		{
			WL_PROFILE_SCOPE("glfwCreateWindow");
			// Set window hints based on initial renderer API
			// Vulkan and DirectX require NO_API, OpenGL requires OPENGL_API
			// Read before any hint is set, because GLFW_CLIENT_API is immutable after
			// glfwCreateWindow: that is the whole reason the recreate path exists at all.
			RendererAPI::API initialAPI = RendererAPI::GetAPI();
			// Reset first, because GLFW window hints are global and sticky: without this a
			// hint set by an earlier window -- or by the recreate path in ExecuteSwitch --
			// would leak into this one.
			glfwDefaultWindowHints();
			if (initialAPI == RendererAPI::API::NVRHI_Vulkan ||
				initialAPI == RendererAPI::API::NVRHI_DX11 ||
				initialAPI == RendererAPI::API::NVRHI_DX12)
			{
				// The Vulkan/DirectX branch. Creating an OpenGL context on a NO_API window fails,
				// so the hint has to be right the first time or the window must be destroyed and
				// rebuilt.
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			}
			else
			{
				// The OpenGL branch, with a 4.5 core-profile context requested immediately
				// after. DEFECT: the hint set is duplicated verbatim in the recreate path in
				// ExecuteSwitch, so the two copies can drift apart -- a version bump in one place
				// only would make a switch behave differently from startup.
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
			}
			// Always TRUE, and props.Resizable is never read anywhere in this file. DEFECT: a
			// caller that asks for a fixed-size window silently gets a resizable one; the
			// WindowProps field appears to be unimplemented.
			glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
			glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

			// No share parameter: the context this window owns is never shared with another.
			// The int casts are safe because the sizes were resolved from a GLFW video mode
			// and clamped to positive values.
			m_Window = glfwCreateWindow((int)m_Data.Width, (int)m_Data.Height, m_Data.Title.c_str(), nullptr, nullptr);
			// The second release-build hole: in a release build this expands to nothing, so
			// a null m_Window is carried forward into CreateContexts, SwitchRendererAPI and
			// every GLFW call below, and the first symptom will be a crash inside GLFW
			// rather than at this line.
			WL_CORE_ASSERT(m_Window, "Failed to create GLFW window!");
		}

		// Create all contexts
		// Contexts first, then the switch below. On the OpenGL path this means the GL
		// context exists before anything asks which API is current.
		CreateContexts();

		// Initialize the current API context
		// Re-read after CreateContexts rather than reused from the variable above, so the
		// value is the one in force at the moment of the switch.
		RendererAPI::API currentAPI = RendererAPI::GetAPI();
		// Goes through the public entry point, not straight to ExecuteSwitch, so the
		// deferral rule below applies. At startup Application is not initialised, so it
		// executes immediately -- which is the 'called during init' case named in that
		// function's comment.
		SwitchRendererAPI(currentAPI);

		// Load-bearing ordering: every lambda installed by SetupCallbacks dereferences
		// this pointer without a null check, so it must be set first. It is.
		glfwSetWindowUserPointer(m_Window, &m_Data);
		// DEFECT: hardcoded, so the caller's props.VSync is never read on this path and
		// the engine's saved vsync preference is ignored at startup. m_Data.VSync is set
		// unconditionally by SetVSync, which also means the window is created with vsync
		// on whatever the application asked for.
		SetVSync(true);

		// Set GLFW callbacks
		// Last, so the window cannot deliver an event to a callback before the user
		// pointer and the Engine-side data it dereferences are both in place.
		SetupCallbacks();
	}

	// Creates only the context for the current API. Kept separate from ExecuteSwitch
	// because startup needs it, while a switch needs the teardown half as well.
	void WindowsWindow::CreateContexts()
	{
		WL_PROFILE_FUNCTION();

		RendererAPI::API api = RendererAPI::GetAPI();
		// One boolean rather than a switch over the enum, because only two of the four
		// APIs use a context type declared in this file.
		bool needGL = (api == RendererAPI::API::OpenGL);
		// Only create OpenGL context if we are starting with OpenGL
		// For NO_API windows (Vulkan/DX), OpenGL context would fail
		// The OpenGL case.
		if (needGL)
		{
			// Constructed but not yet initialised; the next line does that, because
			// OpenGLContext::Init is what loads glad and makes the context current.
			m_OpenGLContext = new OpenGLContext(m_Window);
			// Also performs glfwMakeContextCurrent, which is why glfwSwapInterval works later
			// without this class ever calling it during Init.
			m_OpenGLContext->Init();
			WL_CORE_INFO("Created OpenGL context");
		}
		else
		{
			// Explicitly nulled rather than left alone, so a stale pointer from a previous
			// window cannot survive into DestroyContexts.
			m_OpenGLContext = nullptr;
			WL_CORE_INFO("Skipping OpenGL context creation for NO_API window");
		}

		// Create NVRHI context (will be initialized on first switch)
		// We don't initialize it here to avoid creating DX device when not needed
		// Always null here. The NVRHI context is created on demand in ExecuteSwitch so
		// that starting on OpenGL does not also create a DirectX device -- the cost this
		// line's comment above is avoiding.
		m_NVRHIContext = nullptr;
	}

	// Installs every GLFW callback the engine uses. All of them are captureless
	// lambdas, so they convert to plain function pointers; each recovers its state
	// from glfwGetWindowUserPointer, which is why the user pointer set at the end of
	// Init is a precondition rather than a convenience.
	void WindowsWindow::SetupCallbacks()
	{
		// The resize callback. Registered first because it is the only one that writes
		// back into WindowData.
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
								  {
				// DEFECT, repeated in all seven lambdas: the C-style cast and the missing null
				// check. Every one of these callbacks dereferences the user pointer
				// unconditionally, so a window that has no pointer yet -- or a null one, which is
				// what GLFW reports for a window with no pointer set -- is a null dereference
				// rather than a skipped event.
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				if (data.SuppressCloseEvent)
					return;
				// The engine-side size is updated from the event rather than from a
				// glfwGetWindowSize query, so it is exactly the size GLFW decided on.
				data.Width = width;
				data.Height = height;

				WindowResizeEvent event(width, height);
				// DEFECT (log level): an ordinary user resize is logged at WARNING. Dragging a
				// window edge fills the log with warnings that no one will ever read, which
				// trains everyone to ignore the level that does matter.
				WL_CORE_WARN("{0}, {1}", width, height);
				data.EventCallback(event); });

		// The close callback. Unlike the resize one it does not write back to WindowData:
		// closing is delivered as an event and the application decides what it means.
		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
								   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				// The guard behind the recreate path's m_Data.SuppressCloseEvent flag: while a
				// window is being torn down and rebuilt, glfwDestroyWindow would otherwise deliver
				// a close event and the application would quit mid-switch.
				if (data.SuppressCloseEvent)
					return;
				WindowCloseEvent event;
				data.EventCallback(event); });

		// Keys. The scancode and mods parameters are unused: the engine's KeyPressedEvent
		// carries a repeat flag and the caller queries modifier state separately.
		glfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
						   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				// GLFW_PRESS, GLFW_RELEASE and GLFW_REPEAT are the only values GLFW produces.
				switch (action)
				{
					case GLFW_PRESS:
					{
						// The second argument is the repeat flag, which is how the engine distinguishes a
						// genuine press from the auto-repeat the OS generates for a held key.
						KeyPressedEvent event(key, 0);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent event(key);
						data.EventCallback(event);
						break;
					}
					case GLFW_REPEAT:
					{
						// The same event with repeat set, so a held key produces a stream of presses
						// rather than one press and nothing.
						KeyPressedEvent event(key, 1);
						data.EventCallback(event);
						break;
					}
				} });

		// Text input, kept separate from the key callback because GLFW reports it as a
		// Unicode code point, which is what makes non-ASCII input work at all.
		glfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
							{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				// One event type with no repeat or modifier information, because neither applies
				// to a typed character.
				KeyTypedEvent event(keycode);
				data.EventCallback(event); });

		// Mouse buttons. Only press and release are handled; a double click is two press
		// events, and nothing here tries to detect it.
		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods)
								   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				switch (action)
				{
					case GLFW_PRESS:
					{
						// The button index is GLFW's own numbering (0 left, 1 right, 2 middle), passed
						// through unchanged rather than remapped.
						MouseButtonPressedEvent event(button);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent event(button);
						data.EventCallback(event);
						break;
					}
				} });

		// Scroll. GLFW reports both axes in doubles, and the narrowing cast below is
		// lossy only for scroll deltas far larger than any real input device produces.
		glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
							  {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				// Both components kept, because the engine's event carries a 2D offset and
				// horizontal scroll from a tilt wheel is a real thing.
				MouseScrolledEvent event((float)xOffset, (float)yOffset);
				data.EventCallback(event); });

		// The last callback. Cursor position in window coordinates, which GLFW delivers
		// in doubles; the cast is safe because a window is never wider than 2^24 in
		// practice.
		glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
								 {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				// Absolute position, not a delta. When the cursor is captured GLFW reports the
				// warped position instead, which is why SetMouseCaptured's comment below notes
				// that there is no absolute position to accumulate while captured.
				MouseMovedEvent event((float)xPos, (float)yPos);
				data.EventCallback(event); });
	}

	// Tears down whichever context exists. Called from both Shutdown and the recreate
	// path in ExecuteSwitch, so every ordering constraint it depends on has to hold in
	// both.
	void WindowsWindow::DestroyContexts()
	{
		WL_PROFILE_FUNCTION();

		// Never destroy a window while its GL context is current on this thread:
		// glfwDestroyWindow with a current context is undefined behavior and
		// corrupts nvoglv64.dll state (it also implements Vulkan), crashing later
		// Vulkan init. OpenGLContext's dtor detaches too; this covers ordering.
		// Detach BEFORE deleting anything. The comment above states the reason: destroying
		// a window with its context still current is undefined behaviour and, because the
		// NVIDIA OpenGL driver also implements Vulkan, corrupts state that a later Vulkan
		// init then trips over.
		glfwMakeContextCurrent(nullptr);

		// Null-checked, unlike m_Window in the callers. OpenGLContext's destructor also
		// detaches, so this line is belt and braces as the comment above says.
		if (m_OpenGLContext)
		{
			// Destroyed immediately rather than deferred, so the GL device is gone before any
			// NVRHI device is created on the same window.
			delete m_OpenGLContext;
			m_OpenGLContext = nullptr;
		}

		// The NVRHI half. Note the asymmetry with the GL half: nothing here calls
		// ClearCachedResources first, which is why every caller that has NVRHI resources
		// alive must do it itself.
		if (m_NVRHIContext)
		{
			// Only reached when a context exists, so the NVRHI-only shutdown path does not
			// have to check m_OpenGLContext.
			delete m_NVRHIContext;
			m_NVRHIContext = nullptr;
		}

		// Both context pointers and the current-context pointer are cleared together, so
		// no state survives into the next switch. m_CurrentAPI is deliberately NOT cleared
		// here -- each caller does that itself, at its own point.
		m_CurrentContext = nullptr;
	}

	// The public switch entry point. It never performs the switch itself: it records
	// the request and decides whether to run it now or on the next BeginFrame.
	void WindowsWindow::SwitchRendererAPI(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		// The no-op guard. The context check is load-bearing: at startup m_CurrentAPI is
		// None and a context is null, so switching to None is NOT short-circuited and
		// ExecuteSwitch runs, which is what creates the startup context.
		if (api == m_CurrentAPI && m_CurrentContext)
			return;
		// Coalescing: repeated requests for the same API while one is already queued are
		// ignored. A request for a DIFFERENT API overwrites the pending one, so only the
		// most recent request survives.
		if (m_PendingSwitch && m_PendingAPI == api)
			return;

		// The target, stored rather than acted on. Read back by ProcessPendingSwitch.
		m_PendingAPI = api;
		// The flag BeginFrame tests. Cleared by ProcessPendingSwitch before ExecuteSwitch
		// runs, so a switch that itself requests another switch will queue it rather than
		// recurse.
		m_PendingSwitch = true;

		// If called during init (no current API or app not fully initialized), switch immediately
		// Otherwise defer to next BeginFrame (safe, outside ImGui frame)
		// Three separate reasons to execute immediately rather than defer: before the
		// Application exists there is no BeginFrame to defer to, at startup there is no
		// current API yet, and without a window there is nothing to tear down. The
		// comment above names the first two; the third is the case this function cannot
		// otherwise make progress in.
		if (!Application::IsInitialized() || m_CurrentAPI == RendererAPI::API::None || m_Window == nullptr)
		{
			// Synchronous at startup, which is why Init can call this and then continue with a
			// live context.
			ProcessPendingSwitch();
		}
	}

	// The pump. Called from BeginFrame every frame and once from SwitchRendererAPI,
	// which is why the flag test is the first thing it does.
	void WindowsWindow::ProcessPendingSwitch()
	{
		if (!m_PendingSwitch)
			return;
		// Copied before the flag is cleared, because clearing it is what makes a nested
		// request queue instead of overwrite.
		RendererAPI::API api = m_PendingAPI;
		// Cleared BEFORE the work runs. DEFECT: if ExecuteSwitch throws or fails an
		// internal check, the request is lost with no record that it was ever made -- the
		// window is left on the old API and the caller that asked has no way to retry.
		m_PendingSwitch = false;
		// The actual work, in the one place it happens.
		ExecuteSwitch(api);
	}

	// The long one. Four phases in order: decide whether the GLFW window itself must
	// be recreated, bring the global RenderCommand API in line, shut the ImGui backend
	// down while the old context is still alive, then create the new context and
	// point m_CurrentContext at it. Every crash the comments below record came from
	// reordering two of those.
	void WindowsWindow::ExecuteSwitch(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		if (api == m_CurrentAPI && m_CurrentContext)
			return;
		// The cast to int is because the engine logger has no enum formatter; the numbers
		// are the RendererAPI::API values, which is why this line is only readable next to
		// that enum.
		WL_CORE_INFO("Switching renderer API to {0}", (int)api);
		// Check if window needs recreation due to GLFW_CLIENT_API mismatch
		// OpenGL requires OPENGL_API, Vulkan/DX require NO_API
		// At startup m_CurrentAPI is None, window was just created with correct hints for 'api', so no recreate needed
		bool isNewNoAPI = (api == RendererAPI::API::NVRHI_Vulkan ||
						   api == RendererAPI::API::NVRHI_DX11 ||
						   api == RendererAPI::API::NVRHI_DX12);
		bool isOldNoAPI = (m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan ||
						   m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
						   m_CurrentAPI == RendererAPI::API::NVRHI_DX12);
		// DEFECT: the three-way API test is written out in full four times in this file
		// -- here twice, once in Init and once below. Nothing factors it into a helper, so
		// adding a fourth NVRHI backend means finding all four sites.
		bool needWindowRecreate = false;
		// No recreate at startup: the window was created moments ago in Init with hints
		// chosen for this very API, so recreating it would be pure cost.
		if (m_CurrentAPI != RendererAPI::API::None && m_Window != nullptr)
			// The recreate is needed exactly when the NO_API-ness changes, which is the only
			// thing GLFW_CLIENT_API cares about. An OpenGL to Vulkan switch recreates; Vulkan to
			// DirectX does not.
			needWindowRecreate = (isNewNoAPI != isOldNoAPI);

		if (needWindowRecreate)
		{
			WL_CORE_INFO("Recreating GLFW window for API switch (GL <-> NO_API)");
			// Set before anything is torn down, and cleared again at the end of this block.
			// Every event-producing callback checks it, which is what stops the in-flight
			// glfwDestroyWindow from being reported to the application as a close request.
			m_Data.SuppressCloseEvent = true;
			if (m_Window)
			{
				// Explicitly detached even though SuppressCloseEvent would already suppress it.
				// The flag is the safety net for events already in flight; this removes the source
				// so nothing new arrives.
				glfwSetWindowCloseCallback(m_Window, nullptr);
				// Detached for the same reason. Note this also stops the resize callback that
				// keeps m_Data.Width in step, which is why the properties are saved explicitly on
				// the next line rather than read from m_Data at recreation time.
				glfwSetWindowSizeCallback(m_Window, nullptr);
			}
			// Save window properties
			// A three-argument WindowProps built from the live WindowData. DEFECT: this
			// synthesises a props object rather than keeping the original, so any field not
			// listed in that constructor -- resizable, for instance -- is lost across a
			// recreate.
			WindowProps props(m_Data.Title, m_Data.Width, m_Data.Height);

			// Clear NVRHI cached resources before destroying contexts
			if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
			{
				// A checked downcast of the global API. The dynamic_cast is what makes the single
				// RenderCommand accessor usable for four backends, and the null test below is what
				// keeps a GL-to-GL or GL-to-NVRHI switch from calling through a base pointer.
				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
					nvrhiAPI->ClearCachedResources();
			}

			// Shutdown ImGui backend before window destruction (it holds GLFW window references)
			// Guarded because the recreate path also runs during Init, before an Application
			// exists. GetImGuiLayer is separately null-checked inside, since the Application
			// can exist without the layer.
			if (Application::IsInitialized())
			{
				ImGuiLayer *imguiLayer = Application::Get().GetImGuiLayer();
				if (imguiLayer)
					imguiLayer->ShutdownBackendForWindowRecreate();
			}

			// Destroy contexts that are tied to old window
			// Old contexts gone BEFORE the window, which is the order the comment in
			// DestroyContexts requires.
			DestroyContexts();
			// The old GLFWwindow. Not null-checked here, unlike the guard two blocks up --
			// safe only because needWindowRecreate already required m_Window != nullptr.
			glfwDestroyWindow(m_Window);
			m_Window = nullptr;

			// Set hints for new API
			// Reset, because the hints are global and the previous window's GL context hints
			// would otherwise still be set when the new window is created.
			glfwDefaultWindowHints();
			if (isNewNoAPI)
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			}
			else
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
			}
			glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
			glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

			// Rebuilt at the saved size. The window is created VISIBLE, so it appears on
			// screen before the context below exists and before the callbacks are reinstalled:
			// for a frame or two it is a window that draws nothing and ignores input.
			m_Window = glfwCreateWindow((int)m_Data.Width, (int)m_Data.Height, props.Title.c_str(), nullptr, nullptr);
			// The same release-build hole as in Init, and worse here: a null window from this
			// line is immediately passed to new OpenGLContext below, so a release build
			// dereferences null rather than merely misbehaving.
			WL_CORE_ASSERT(m_Window, "Failed to recreate GLFW window for API switch!");

			// Recreate contexts for new window
			// Context creation is duplicated between this recreate path and CreateContexts
			// above, with one real difference: this version does not null m_NVRHIContext in
			// the OpenGL branch -- it relies on DestroyContexts having done it moments ago.
			if (api == RendererAPI::API::OpenGL)
			{
				m_OpenGLContext = new OpenGLContext(m_Window);
				m_OpenGLContext->Init();
				m_NVRHIContext = nullptr;
			}
			else
			{
				m_OpenGLContext = nullptr;
				m_NVRHIContext = nullptr; // will be created below
			}

			// Restores the size recorded before the teardown. Redundant in the common case,
			// since nothing resized the window in between, but it makes the intent explicit
			// after a period in which the user pointer was dangling.
			m_Data.Width = props.Width;
			m_Data.Height = props.Height;
			glfwSetWindowUserPointer(m_Window, &m_Data);
			SetupCallbacks();
			m_Data.SuppressCloseEvent = false;
			// Set VSync directly without calling glfwSwapInterval for NO_API windows
			// glfwSwapInterval is only valid for OpenGL contexts
			// DEFECT: hardcoded, and it overwrites whatever the user had chosen. The comment
			// above explains why glfwSwapInterval is not called for a NO_API window, but the
			// saved preference is still discarded -- the next call to SetVSync will be a no-op
			// because m_Data.VSync already says true.
			m_Data.VSync = true;
			if (!isNewNoAPI)
			{
				// Only call glfwSwapInterval for OpenGL windows
				// Needed before glfwSwapInterval on the line below: vendor/GLFW/src/context.c
				// raises GLFW_NO_CURRENT_CONTEXT when there is no current context, and OpenGL is
				// not on this path to make one.
				glfwMakeContextCurrent(m_Window);
				// Hardcoded 1, matching m_Data.VSync = true above. The two are set
				// independently, so they cannot disagree today -- but if the line above became
				// m_Data.VSync = m_Data.VSync this call would still force vsync on.
				glfwSwapInterval(1);
			}
			// DEFECT: a second call. The first is directly above, before the VSync block, and
			// this one re-registers all seven callbacks. It is redundant -- glfwSetXxxCallback
			// replaces rather than appends -- and it means the callbacks are re-installed
			// after the context became current, so nothing observes the intermediate state.
			SetupCallbacks();

			// Reset current context tracking so the switch below creates the right context
			// Cleared so the switch below cannot short-circuit on the context check at the top
			// of this function, and so no stale pointer survives if the switch fails midway.
			m_CurrentContext = nullptr;
			// Also cleared, which together with the line above makes the state indistinguishable
			// from a fresh window -- deliberately, so the code below takes its create-new-
			// context path rather than the switching-between-contexts path.
			m_CurrentAPI = RendererAPI::API::None;
		}

		// Ensure global RenderCommand API matches target api before creating contexts
		// This makes the switch atomic (previously EditorLayer called SetAPI separately causing 1-frame mismatch)
		// Bringing the process-wide API in line with the window's. The comment above
		// records why this has to happen here rather than being left to the caller.
		if (RendererAPI::GetAPI() != api)
		{
			// Clear cached resources from old NVRHI API before deleting it
			if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
			{
				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
					nvrhiAPI->ClearCachedResources();
			}
			// DEFECT: this replaces the global API object outright. The cached-resource clear
			// above is what makes that survivable; anything else holding the old API pointer
			// across this line is left dangling.
			RenderCommand::SetAPI(api);
		}
		else if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				 m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				 m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
		{
			// Clear cached resources when switching away from NVRHI but API enum already matches
			NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
				nvrhiAPI->ClearCachedResources();
			}

			// Shut down the ImGui renderer backend while the OLD context is still
			// alive. Its Shutdown releases backend device objects owned by the old
			// context (e.g. ImGui_ImplVulkan_Shutdown touches the descriptor pool /
			// render pass that die with the old NVRHIContext). Running it after the
			// old context is deleted is use-after-destroy (crashed in
			// PerformBackendSwitch after Vulkan -> DX switches). The deferred
			// SwitchBackend(api) below then only has to init the new backend
			// (ShutdownBackend is a no-op when no backend is current). The window-
			// recreate path above already did this; the None guard makes a second
			// call here harmless.
			if (Application::IsInitialized())
			{
			ImGuiLayer *preSwitchImgui = Application::Get().GetImGuiLayer();
			// The null check the window-recreate branch also performs, for the same reason:
			// the layer can be absent even when the Application exists.
			if (preSwitchImgui)
				// The second ImGui shutdown, reached on the non-recreate path (OpenGL to Vulkan
				// on a window that stays NO_API on both ends, or one NVRHI API to another). The
				// long comment above explains why this must precede context destruction, and the
				// note that a second call is harmless because the first path already ran it.
				preSwitchImgui->ShutdownBackendForWindowRecreate();
			}

			// Create NVRHI context if needed and not already created
		// Create-if-absent, which distinguishes the first NVRHI switch from a switch
		// between two NVRHI backends handled by the branch below.
		if ((api == RendererAPI::API::NVRHI_DX11 ||
			 api == RendererAPI::API::NVRHI_DX12 ||
			 api == RendererAPI::API::NVRHI_Vulkan) &&
			!m_NVRHIContext)
		{
			// Constructed with the target API rather than reading it back out of the global,
			// so the context and the global cannot be built for different backends.
			m_NVRHIContext = new NVRHIContext(m_Window, api);
			// The expensive step: it creates the native device, the swapchain and the queues.
			// Not wrapped in a profile scope, unlike the OpenGL path in CreateContexts, so
			// this cost is invisible in the profiler.
			m_NVRHIContext->Init();

			// Set the context on the renderer API
			NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
			{
				// Hands the context to the renderer API. The static_cast is safe because the
				// dynamic_cast a few lines above already proved the global API is an NVRHIRendererAPI
				// and this branch already proved api is one of the three NVRHI values.
				nvrhiAPI->SetContext(static_cast<NVRHIContext *>(m_NVRHIContext));
			}

			WL_CORE_INFO("Created NVRHI context");
		}
		else if (api == RendererAPI::API::NVRHI_DX11 ||
				 api == RendererAPI::API::NVRHI_DX12 ||
				 api == RendererAPI::API::NVRHI_Vulkan)
		{
			// Switching between NVRHI APIs - need to recreate context
			// Only recreate when one exists. DEFECT: if it does not -- which is the state
			// DestroyContexts just left -- this branch silently does nothing and m_CurrentContext
			// is left null for the rest of this function, with no diagnostic. The create path
			// above is supposed to guarantee non-null, so this is a second line of defence
			// that never reports when it is needed.
			if (m_NVRHIContext)
			{
				// DEFECT: no ClearCachedResources before this delete, unlike the three earlier
				// call sites in this file and in Shutdown. A Vulkan-to-DirectX switch therefore
				// destroys the device with the previous backend's cached pipelines and textures
				// still alive -- exactly the situation the Shutdown comment says produces VUID spam
				// and an access violation.
				delete m_NVRHIContext;
				m_NVRHIContext = new NVRHIContext(m_Window, api);
				m_NVRHIContext->Init();

				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
				{
					nvrhiAPI->SetContext(static_cast<NVRHIContext *>(m_NVRHIContext));
				}
			}
		}

		// Switch to the appropriate context
		// The final selection of m_CurrentContext. There is no else: an unknown or None
		// API leaves m_CurrentContext null and m_CurrentAPI set to that value, and the
		// caller sees a window with no context and no error.
		if (api == RendererAPI::API::OpenGL)
		{
			m_CurrentContext = m_OpenGLContext;
		}
		else if (api == RendererAPI::API::NVRHI_DX11 ||
				 api == RendererAPI::API::NVRHI_DX12 ||
				 api == RendererAPI::API::NVRHI_Vulkan)
		{
			m_CurrentContext = m_NVRHIContext;
		}

		// Recorded last, so the whole function above runs with the OLD value still in
		// m_CurrentAPI. The two ClearCachedResources tests earlier in this function rely on
		// exactly that.
		m_CurrentAPI = api;

		// Switch ImGui backend to match the new renderer API
		if (Application::IsInitialized())
		{
			ImGuiLayer *imguiLayer = Application::Get().GetImGuiLayer();
			if (imguiLayer != nullptr)
			{
				// Init for the new backend only. The matching Shutdown happened above while the
				// old context was still alive, which is the ordering the long comment insists on.
				// This call is skipped entirely when no Application exists, which is the startup
				// case -- and it means a switch performed during Init leaves ImGui with no backend
				// at all.
				imguiLayer->SwitchBackend(api);
			}
		}
	}

	// Called only from the destructor. Private, with no idempotence guard and no
	// m_Window null check on the destroy below -- safe today only because nothing else
	// calls it, as the comment on the destructor notes.
	void WindowsWindow::Shutdown()
	{
		WL_PROFILE_FUNCTION();

		// Release renderer-cached NVRHI resources BEFORE destroying contexts:
		// anything still referenced (pipelines, buffers, textures) keeps the
		// NVRHI Device alive past native teardown, and the driver then destroys
		// a device with live children (VUID spam + AV in nvoglv64). Switches
		// already do this in ExecuteSwitch; shutdown must too.
		if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
			m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
			m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
		{
			NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
				nvrhiAPI->ClearCachedResources();
		}

		// Contexts before the window, the ordering DestroyContexts' own comment requires.
		DestroyContexts();
		// DEFECT: m_Window is not set to null afterwards and there is no guard against a
		// null or already-destroyed handle. Shutdown is private and called once, so this is
		// currently unreachable -- but any future public Close() that calls it would make
		// the destructor's call a second destroy on a dangling pointer.
		glfwDestroyWindow(m_Window);
	}

	// Called once per frame before rendering.
	void WindowsWindow::BeginFrame()
	{
		WL_PROFILE_FUNCTION();
		// The deferred half of SwitchRendererAPI's decision. Running here rather than at
		/// the request site is what keeps a switch out of the middle of an ImGui frame.
		if (m_PendingSwitch)
			ProcessPendingSwitch();
		if (m_CurrentContext)
			// Null-checked, so a window that never had a context -- a None API, or a failed
			// switch -- skips its per-frame work instead of crashing.
			m_CurrentContext->BeginFrame();
	}

	void WindowsWindow::OnUpdate()
	{
		WL_PROFILE_FUNCTION();

		// DEFECT: called from OnUpdate rather than BeginFrame, so the two are coupled by
		// the base class's call order rather than by anything stated here. If a caller ever
		/// pumps OnUpdate without BeginFrame, queued events are dispatched while m_CurrentContext
		// still points at the context from the previous frame.
		glfwPollEvents();
		if (m_CurrentContext)
			// Null-checked for the same reason as BeginFrame above. This is where vsync is
			/// actually applied: glfwSwapInterval only changes the interval the driver uses
			/// when the buffers are presented.
			m_CurrentContext->SwapBuffers();
	}

	// Sets the desired state first, then pushes it to the driver where that is
	// possible. The recorded value is the authority; GLFW is best-effort.
	void WindowsWindow::SetVSync(bool enabled)
	{
		WL_PROFILE_FUNCTION();

		m_Data.VSync = enabled;
		// glfwSwapInterval is only valid for OpenGL contexts
		// For NO_API windows (Vulkan/DX), it's a no-op and may generate GLFW error
		// DEFECT (wrong test): this asks what client API the WINDOW was created for, not
		// whether a context is current. glfwSwapInterval reads the current context's TLS
		// slot (vendor/GLFW/src/context.c, _glfwPlatformGetTls) and raises
		// GLFW_NO_CURRENT_CONTEXT without one. On the OpenGL path OpenGLContext::Init
		// leaves one current so this passes, but the recreate path's NO_API case and any
		// caller that made another context current first would trip it.
		if (m_Window && glfwGetWindowAttrib(m_Window, GLFW_CLIENT_API) == GLFW_OPENGL_API)
		{
			if (enabled)
				glfwSwapInterval(1);
			else
				// The off case, as a literal 0 rather than a negation of the value above: GLFW's
				// interval is a count of vsyncs, so 'disabled' is 0 and 'enabled' is 1.
				glfwSwapInterval(0);
		}
	}

	// Reports the recorded intent, never the driver state.
	bool WindowsWindow::IsVSync() const
	{
		// DEFECT: for a NO_API window -- Vulkan or DirectX -- nothing in this file ever
		// calls glfwSwapInterval, so the answer is a stored boolean with no mechanism
		// behind it. A caller reading true here should not conclude the frame rate is
		// actually synchronised.
		return m_Data.VSync;
	}

	// Confine or release the cursor. One GLFW call covers both, as the comment block
	// below explains, and that is why there is no separate hide-only path.
	void WindowsWindow::SetMouseCaptured(bool captured)
	{
		WL_PROFILE_FUNCTION();

		// Null-checked here, unlike the GLFW calls in Init and Shutdown. This is the
		/// defensive shape the rest of the file should have used.
		if (!m_Window)
			return;

		// GLFW_CURSOR_DISABLED does two things at once: it hides the cursor
		// AND switches motion reporting to relative (warped) mode. Both are
		// required here -- hiding the cursor alone leaves an absolute pointer
		// that runs into a screen edge, and relative mode alone leaves a
		// visible cursor the player cannot aim with.
		//
		// GLFW_CURSOR_DISABLED also warps the cursor to the window centre as a
		// side effect, which is why a raw per-frame delta has no absolute
		// position to accumulate.
		glfwSetInputMode(m_Window, GLFW_CURSOR,
						 captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

		// DEFECT: recorded unconditionally, even if glfwSetInputMode failed. The header
		// comments explain the intent -- IsMouseCaptured reports intent rather than
		// querying GLFW -- so this is deliberate, and it means the flag can disagree with
		// the OS-level mode, which is the exact case that comment describes.
		m_Data.MouseCaptured = captured;
	}

	// The const reader for the flag above.
	bool WindowsWindow::IsMouseCaptured() const
	{
		// DEFECT: the property is private in WindowData, so only this class can write it
		// -- there is no setter for GLFW to call when the OS cancels capture, for instance
		// when the user presses Escape. The value can therefore be stale in exactly the
		// situation the header comment says it is meant to survive.
		return m_Data.MouseCaptured;
	}

}