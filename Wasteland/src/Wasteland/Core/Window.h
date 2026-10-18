// Window.h -- the platform window abstraction and its creation properties.
//
// Window is the engine's view of a desktop window. The concrete
// implementations live under src/Platform/<OS>/ and differ in how they talk to
// GLFW or the native API. Everything above this layer -- renderer, ImGui layer,
// Application -- works against the interface only.
//
// Multi-backend: SwitchRendererAPI and GetCurrentContext exist because the
// engine can be running on Vulkan, D3D11, D3D12 or OpenGL within the same
// window, with the swapchain re-created when that changes.

#pragma once

#include "wlpch.h"

#include "Wasteland/Core/Core.h"
#include "Wasteland/Events/Event.h"
#include "Wasteland/Renderer/RendererAPI.h"

namespace Wasteland {

	class GraphicsContext; // Forward declaration

	// Construction parameters for a window: what to call it and how big it
	// starts.
	//
	// A struct with public members and a constructor rather than a class with
	// accessors -- the engine treats these as a plain value bag, and every
	// field has a sensible default so `WindowProps()` alone is valid.
	struct WindowProps
	{
		// Window title bar text. Default "Wasteland Engine".
		std::string Title;

		// Initial width in pixels. Default 1280.
		uint32_t Width;

		// Initial height in pixels. Default 720.
		uint32_t Height;

		// Sets all three fields from arguments, each with a default so a
		// caller can override only what it cares about:
		//   WindowProps p;                        // all defaults
		//   WindowProps p("Game", 1920, 1080);    // all explicit
		//
		// Member-init list rather than assignment in the body, so each field is
		// initialized exactly once.
		WindowProps(const std::string& title = "Wasteland Engine",
			uint32_t width = 1280,
			uint32_t height = 720)
			: Title(title), Width(width), Height(height)
		{
		}
	};

	// Interface representing a desktop system based Window
	class WL_API Window
	{
	public:
		// Callback invoked when the platform delivers an event.
		//
		// Takes the event by mutable reference because a handler may mark it
		// handled to stop propagation. std::function rather than a raw
		// function pointer so a capturing lambda can be registered.
		using EventCallbackFn = std::function<void(Event&)>;

		// Virtual so a backend deleted through a Window* is destroyed
		// correctly. Empty body -- concrete backends release their own handles.
		virtual ~Window() {}

		// Pumps the platform event queue: retrieves pending OS events, converts
		// them to engine Events, and invokes the registered callback for each.
		//
		// Called once per frame from Application::Run, after the layer update
		// pass. Pure virtual because every backend must pump its own queue.
		virtual void OnUpdate() = 0;

		// Signals the start of a frame to the backend (device-lost checks,
		// swapchain presentation, and so on).
		//
		// Non-pure with a do-nothing default, so a backend that has no
		// per-frame start work can ignore it.
		virtual void BeginFrame() {}

		// Current window width in pixels.
		//
		// Preferred over the WindowProps values, which are only the initial
		// size -- this reflects whatever the window is now, including
		// user-driven resizes. May return 0 for a minimized window.
		virtual uint32_t GetWidth() const = 0;

		// Current window height in pixels. See GetWidth for the caveats.
		virtual uint32_t GetHeight() const = 0;

		// Window attributes
		//
		// Registers the callback that receives every event this window
		// produces. Typically wired to Application::OnEvent, so platform
		// events enter the layer stack.
		//
		// @param callback  Invoked per event; must outlive the window.
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;

		// Enables or disables vertical sync, which caps the frame rate at the
		// display refresh. Turning it off removes the frame cap, which is what
		// a benchmark or an uncapped-gameplay setting wants.
		virtual void SetVSync(bool enabled) = 0;

		// Returns whether VSync is currently enabled. Lets the UI show the
		// current setting rather than a stale copy of the last request.
		virtual bool IsVSync() const = 0;

		// Confine the cursor to the window and report it as relative motion,
		// which is what a first-person camera needs: an absolute cursor leaves
		// it drifting toward a screen edge and eventually unable to turn
		// further. Releasing capture restores a normal visible cursor.
		//
		// Capture must be explicitly released. A game that grabs the cursor and
		// exits or crashes while holding it strands the user with an invisible
		// pointer and no way to get it back without killing the process, so the
		// caller owns the paired release.
		//
		// Non-pure with a do-nothing default: the other platform backends
		// (Linux, Mac) are compiled by the Makefile but not by the MSVC
		// project, so a new pure virtual here would break those builds while
		// leaving the Windows build green and the breakage unnoticed.
		//
		// @param captured  true to grab and hide the cursor, false to release.
		virtual void SetMouseCaptured(bool captured) { (void)captured; }

		// Whether the cursor is currently captured. Always false on backends
		// that ignore SetMouseCaptured, so a caller cannot use this to detect
		// whether capture is actually supported.
		virtual bool IsMouseCaptured() const { return false; }

		// Returns the platform's own window handle, cast to void*.
		//
		// Untyped because the concrete type (HWND, X11 Window, NSWindow) is
		// platform-specific and cannot appear in this header. Callers cast it
		// back, which is why this is unsafe to use across a platform boundary.
		virtual void* GetNativeWindow() const = 0;

		// Multi-backend support
		//
		// Re-creates the swapchain and graphics resources on a different
		// backend, keeping the window itself alive. A no-op if the backend is
		// already current.
		//
		// Expensive: everything bound to the old context is invalidated, so
		// textures, pipelines and the ImGui backend all need rebuilding
		// afterwards. Anything holding a device resource across this call will
		// be left dangling.
		//
		// @param api  Target backend.
		virtual void SwitchRendererAPI(RendererAPI::API api) = 0;

		// Returns the graphics context for the backend currently in use.
		//
		// Ownership stays with the window; the pointer is invalidated by
		// SwitchRendererAPI. Callers typically dynamic_cast it to a concrete
		// context type (NVRHIContext, OpenGLContext) to reach backend-specific
		// operations.
		virtual GraphicsContext* GetCurrentContext() const = 0;

		// Creates the platform's window implementation.
		//
		// A static factory rather than a constructor so the caller never names
		// a platform class -- the #ifdef lives inside the implementation. The
		// returned pointer is owned by the caller and must be deleted.
		//
		// @param props  Title and initial size. Defaults are used if omitted.
		// @returns A new Window, or nullptr if the platform refused to create
		//          one (display unavailable, unsupported resolution).
		static Window* Create(const WindowProps& props = WindowProps());
	};

}