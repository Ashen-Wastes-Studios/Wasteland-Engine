#pragma once

// The abstract Window interface. This class implements it, so the full
// definition is needed -- not just a forward declaration.
#include "Wasteland/Core/Window.h"
// Needed because m_OpenGLContext / m_NVRHIContext / m_CurrentContext
// are held as pointers to this type.
#include "Wasteland/Renderer/GraphicsContext.h"
// The RendererAPI::API enum, stored in m_CurrentAPI and m_PendingAPI.
#include "Wasteland/Renderer/RendererAPI.h"

// Platform windowing library. Needed for the GLFWwindow* member.
// Third-party and therefore angle-bracketed, unlike the project
// headers above.
#include <GLFW/glfw3.h>

namespace Wasteland
{

	// Public inheritance from the abstract Window: the application holds
	// Window pointers, so anything else would make this unusable there.
	// This is the Windows backend of a set that also has a MacWindow
	// (Wasteland/src/Platform/Mac/MacWindow.cpp).
	class WindowsWindow : public Window
	{
	public:
		// props carries title, size and the initial API preference.
		// The & binds to const, which is looser than the file's prevailing
		// const-ref-by-convention style but matches other members here.
		WindowsWindow(const WindowProps &props);
		// Not defaulted and not inline: the body must call Shutdown()/
		// DestroyContexts(), so it is defined in WindowsWindow.cpp.
		// virtual because Window is polymorphic and instances are deleted
		// through a Window pointer.
		virtual ~WindowsWindow();

		// Per-frame pump. Takes no timestep, so anything time-based inside
		// has to get time from somewhere else.
		void OnUpdate() override;
		// Marks the start of a frame for the platform layer.
		void BeginFrame() override;

		// Reads the cached m_Data.Width rather than querying GLFW. That is a
		// deliberate cache: the value is updated on resize, so this can be
		// stale for the duration of a resize event.
		// inline in the class body: these are one-liners, so a separate
		// out-of-line definition would be pure overhead.
		inline unsigned int GetWidth() const override { return m_Data.Width; }
		inline unsigned int GetHeight() const override { return m_Data.Height; }

		// Existing section comment introducing the property setters below.
		// Window attributes
		// Stores the callback in m_Data rather than registering it directly
		// with GLFW. GLFW callbacks are set up once in SetupCallbacks() and
		// they funnel into this stored function.
		// Inline because it is a one-line assignment.
		inline void SetEventCallback(const EventCallbackFn &callback) override { m_Data.EventCallback = callback; }
		// Out-of-line because it reaches into GLFW, and because toggling
		// vsync on some drivers forces a surface/context rebuild.
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;
		// Records intent in m_Data.MouseCaptured and (per the .cpp) applies
		// the GLFW cursor mode. The cached flag is what IsMouseCaptured reads.
		void SetMouseCaptured(bool captured) override;
		bool IsMouseCaptured() const override;

		// Type-erased escape hatch: returns the GLFWwindow* as void* so the
		// platform backends (WindowsInput.cpp, MacInput.cpp) can static_cast it
		// back without Core/Window.h knowing about GLFW.
		// Returns the member directly, so before Init() runs -- or after
		// Shutdown() nulls it -- this hands back null. Both input backends
		// null-check the result for exactly that reason.
		// `inline virtual` is redundant (a virtual member function defined in
		// the class body is implicitly inline) but harmless.
		inline virtual void *GetNativeWindow() const { return m_Window; }

		// Existing section comment: the API can be switched at runtime, which
		// is why the context members below exist as a set rather than as one
		// context field.
		// Multi-backend support
		// Requests a backend switch. Given the m_PendingAPI/m_PendingSwitch
		// pair below, this only records the request -- ExecuteSwitch does the
		// work later, at a point the .cpp considers safe to recreate a
		// graphics context.
		void SwitchRendererAPI(RendererAPI::API api) override;
		// Returns a mutable GraphicsContext* from a const method, because the
		// pointer member is const but the pointee is not. Intentional (callers
		// need to issue commands through it) but it does let a const
		// WindowsWindow be mutated from outside.
		// May be null when m_CurrentAPI is None.
		GraphicsContext *GetCurrentContext() const override { return m_CurrentContext; }

	// Implementation surface. Everything below is called from the .cpp or
	// from the public methods here.
	private:
		// virtual so a derived window class can extend startup without
		// hiding it. DEFECT-ish: nothing in this repo derives from
		// WindowsWindow, so the virtual is currently speculative.
		virtual void Init(const WindowProps &props);
		// Out-of-line because it must destroy GLFW resources in a specific
		// order (contexts before window, window before glfwTerminate).
		virtual void Shutdown();
		// Builds whichever GraphicsContext m_CurrentAPI selected. Not virtual,
		// unlike its counterpart below -- an asymmetry in this private block.
		void CreateContexts();
		// The mirror of CreateContexts. Order relative to glfwDestroyWindow is
		// the kind of thing that has to be right; that ordering lives in the
		// .cpp, not here.
		void DestroyContexts();
		// Registers the GLFW window/focus/cursor/key/mouse/scroll callbacks
		// once at startup. Those callbacks are what ultimately invoke the
		// m_Data.EventCallback set on line 25.
		void SetupCallbacks();
		// Does the real work for SwitchRendererAPI: tears down the current
		// context, creates the new one, updates m_CurrentAPI. Split from the
		// request method so the switch can be deferred to a safe point.
		void ExecuteSwitch(RendererAPI::API api);
		// Called from the frame pump (OnUpdate): if m_PendingSwitch is set,
		// run ExecuteSwitch and clear the flag. This is the deferred half of
		// the two-phase API switch.
		void ProcessPendingSwitch();

	// Second consecutive private: label, directly after line 37.
	// Redundant but harmless -- it does visually separate methods from data.
	private:
		// DEFECT: no in-class initialiser, so it is indeterminate until Init
		// assigns it. Every other pointer member below defaults to nullptr.
		// GetNativeWindow() (line 31) can therefore return garbage rather than
		// null if called on a not-yet-initialised window. A `= nullptr` here
		// would make the class safe to construct and then query.
		GLFWwindow *m_Window;

		// Multiple contexts for different backends
		// One slot per backend, all initialised to null. Holding both at once
		// means a switch back does not have to rebuild from source.
		GraphicsContext *m_OpenGLContext = nullptr;
		// The second backend slot. DEFECT: there is no destructor on this class
		// that deletes either pointer, so whichever context is live at exit
		// leaks unless the .cpp Shutdown() handles it. Not verifiable from
		// this header alone.
		GraphicsContext *m_NVRHIContext = nullptr;
		// Points at whichever of the two slots is active. It is a second level
		// of indirection rather than a member of an array or a std::variant,
		// so adding a third backend means adding another field AND another
		// branch in every dispatch site.
		GraphicsContext *m_CurrentContext = nullptr;
		// Which backend is live. None is the honest initial state and is what
		// GetCurrentContext() returning null would correspond to.
		RendererAPI::API m_CurrentAPI = RendererAPI::API::None;
		// The requested-but-not-yet-applied backend. Separated from
		// m_CurrentAPI precisely so a switch requested mid-frame does not
		// take effect until ProcessPendingSwitch runs.
		RendererAPI::API m_PendingAPI = RendererAPI::API::None;
		// Separate flag rather than comparing m_PendingAPI against m_CurrentAPI,
		// because "request the API that is already active" is a legitimate
		// no-op that would otherwise look identical to a real request.
		bool m_PendingSwitch = false;

		// POD-ish bundle of the window's mutable properties. It exists so the
		// accessors can be inline one-liners over a struct member instead of
		// out-of-line calls into GLFW.
		struct WindowData
		{
			// DEFECT: no default initialiser, but unlike the other uninitialised
			// members here std::string constructs itself, so it is safe empty.
			std::string Title;
			// DEFECT: plain unsigned ints with no initialiser, so a WindowsWindow
			// that has not run Init() reports garbage from GetWidth()/GetHeight().
			// Two values on one line, unlike the one-per-line style of the
			// pointer members above.
			unsigned int Width, Height;
			// DEFECT: uninitialised bool. Reading it before Init() gives an
			// indeterminate value, and an uninitialised bool that is read is
			// undefined behaviour in the strict sense -- IsVSync() does exactly
			// that read.
			bool VSync;
			// Whether the caller currently wants the cursor confined to the
			// window. Tracked so IsMouseCaptured() reflects intent rather than
			// querying GLFW, which would report the OS-level mode and can differ
			// after the user presses Escape.
			// In-class initialiser, unlike VSync and Width/Height above.
			// This is the one member of the three that is actually safe to read
			// before Init -- see the comment above it for why the cached value is
			// the right thing to consult instead of asking GLFW.
			bool MouseCaptured = false;
			// Lets the application veto the close request (used for unsaved-changes
			// prompts). Initialised false, so the default is "let it close".
			bool SuppressCloseEvent = false;

			// DEFECT: NOT initialised, unlike its neighbours MouseCaptured and
			// SuppressCloseEvent. If any GLFW callback fires before SetupCallbacks
			// installs a target and something calls the stored callback, this is
			// an empty std::function and calling it throws std::bad_function_call.
			// In-class `= nullptr` or `{}` would close that gap.
			EventCallbackFn EventCallback;
		};

		// The one instance of the struct above; every inline accessor reads
		// through it. Value-initialised as an aggregate, but that does NOT
		// initialise its scalar members -- which is exactly the defect noted
		// on Width, Height, VSync and EventCallback above.
		WindowData m_Data;
	};

}
