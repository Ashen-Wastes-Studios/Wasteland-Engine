// Application.h -- the engine's owning object: owns the window, drives the
// frame loop, and holds the layer stack.
//
// This is the root of the engine's object graph. A client defines its own
// subclass (see SandboxApp.cpp, where `class Sandbox : public Application`),
// supplies a CreateApplication() factory, and EntryPoint.h calls it to obtain
// the instance that then runs the frame loop.
//
// Singleton: exactly one Application exists per process, tracked in s_Instance
// and reachable from anywhere via Application::Get(). This is what lets deeply
// nested code -- a render pass, an ImGui panel, a script binding -- reach the
// window without every layer in between having to thread a reference down.

#pragma once

#include "Core.h"

#include "Wasteland/Core/Window.h"
#include "Wasteland/Core/LayerStack.h"
#include "Wasteland/Events/Event.h"
#include "Wasteland/Events/ApplicationEvent.h"

#include "Wasteland/Core/Timestep.h"

#include "Wasteland/ImGui/ImGuiLayer.h"

namespace Wasteland
{

	// Owns the window and the layer stack, and runs the main loop.
	//
	// NOT marked WL_API, unlike every other public engine type (Layer, Window,
	// Event, Timestep). That is safe only because both consumers -- Sandbox and
	// DemonCore-Editor -- are executables that reference Wasteland through a
	// ProjectReference and therefore link the static Wasteland.lib directly.
	// A DLL consumer would need this exported; moving either consumer to
	// dynamic linkage would turn that into a link error on the vtable.
	//
	// Thread-confined: every method is called from the main thread. There is no
	// locking anywhere in this class.
	class Application
	{
	public:
		// Constructs the application and creates its window.
		//
		// @param name  Application name, used in log lines and as the default
		//              window title. Defaults to "Wasteland App".
		Application(const std::string &name = "Wasteland App");

		// Tears down the ImGui layer and destroys the window.
		//
		// Virtual because clients subclass this. Order matters: the ImGui
		// layer is deleted before the window, since ImGui's graphics resources
		// belong to the window's context.
		virtual ~Application();

		// Runs the frame loop until Close() is called or the window is closed
		// by the user.
		//
		// Each iteration computes a Timestep, drives Layer::OnUpdate across the
		// stack, renders, then drives Layer::OnImGuiRender. This is the engine's
		// main loop and does not return until shutdown. See Application.cpp for
		// the exact per-frame sequence, including the NVRHI scene flush that has
		// to happen between update and ImGui.
		void Run();

		// Central event sink. Every event the window produces arrives here.
		//
		// Handles window close and resize itself, then offers the event to each
		// layer in REVERSE stack order, stopping early once the event is marked
		// handled. Finally, if a Python handler is registered, dispatches to it.
		//
		// @param e  The event to route. Marking it handled stops propagation to
		//           the remaining layers. See the handled-flag caveat in
		//           Events/Event.h before relying on that.
		void OnEvent(Event &e);

		// Adds a regular layer and calls its OnAttach().
		//
		// Note this is the caller that fires OnAttach -- LayerStack::PushLayer
		// only inserts the pointer.
		//
		// @param layer  Layer to take ownership of. The stack deletes it.
		void PushLayer(Layer *layer);

		// Adds an overlay and calls its OnAttach().
		//
		// Overlays sit after all regular layers, so they update and draw last.
		//
		// @param layer  Overlay to take ownership of. The stack deletes it.
		void PushOverlay(Layer *layer);

		// Returns the window by reference.
		//
		// By reference rather than pointer so callers cannot accidentally null-
		// check a window that is always present, and so `Get().GetWindow()` reads
		// naturally. Inline because it is a bare member read.
		inline Window &GetWindow() { return *m_Window; }

		// Requests that the frame loop exit after the current iteration.
		//
		// Sets m_Running false rather than throwing or calling exit(), so
		// cleanup still runs normally.
		void Close();

		// Returns the ImGui layer, or nullptr if it was never created.
		//
		// Null is a real possibility in a headless build, so callers should
		// check. Used by the editor and by panels that need to enqueue draw
		// commands outside a Layer callback.
		ImGuiLayer *GetImGuiLayer() { return m_ImGuiLayer; }

		// Returns the one live Application.
		//
		// Dereferences s_Instance unconditionally, so calling this before the
		// Application is constructed is a null deref. Test IsInitialized()
		// first if that is a possibility -- static initializers and early
		// script bindings both hit this.
		inline static Application &Get() { return *s_Instance; }

		// Whether an Application instance exists yet.
		//
		// Cheap and safe at any point in startup, unlike Get(). This is the
		// check to use from code that might run before CreateApplication().
		static bool IsInitialized() { return s_Instance != nullptr; }

	private:
		// Handler for WindowCloseEvent, invoked from OnEvent.
		//
		// @returns true if the close was consumed, which stops the event
		//          reaching the layers below.
		bool OnWindowClose(WindowCloseEvent &e);

		// Handler for WindowResizeEvent, invoked from OnEvent.
		//
		// Records the new size and rebuilds the framebuffer-sized resources the
		// renderer and ImGui need.
		//
		// @returns true if the resize was consumed.
		bool OnWindowResize(WindowResizeEvent &e);

	private:
		// The platform window, owned via unique_ptr because Application creates
		// it and must destroy it. Held by pointer semantics rather than by
		// value so GetWindow() can hand out a reference.
		std::unique_ptr<Window> m_Window;

		// The ImGui layer, owned by the LayerStack (it is pushed as an overlay),
		// so this is a non-owning observer pointer kept for convenience.
		//
		// Starts null and stays null in a build that never creates ImGui, which
		// is why GetImGuiLayer() can return null.
		ImGuiLayer *m_ImGuiLayer = nullptr;

		// Whether the frame loop should keep running.
		//
		// True from construction, so Run() enters its loop immediately without
		// an explicit start call. Close() clears it.
		bool m_Running = true;

		// The layers and overlays, in update order. Owns everything pushed into
		// it. Declared after m_Window so it is destroyed FIRST, which means
		// layers shut down while the window and its graphics context are still
		// alive -- the order a layer needs in order to release GPU resources.
		LayerStack m_LayerStack;

		// glfwGetTime() value from the previous frame, used to compute this
		// frame's Timestep.
		//
		// Initialized to 0 rather than to the current time, so the very first
		// frame's delta is the time since process start -- a large value. The
		// timestep is not clamped anywhere, so a layer integrating velocity on
		// frame one will see that spike. Pre-warming this in the constructor
		// would remove it.
		float m_LastFrameTime = 0.0f;

		// Whether the window is minimized.
		//
		// When true, Run() skips the entire update/render/ImGui body for the
		// frame. A minimized window's drawable size is 0x0 on most platforms,
		// and rendering into a zero-sized swapchain is at best wasted work and
		// at worst a validation error.
		bool m_Minimized = false;

	private:
		// The live Application, set by the constructor and cleared by the
		// destructor. Null whenever no instance exists.
		//
		// Raw pointer, not a smart pointer: the instance is owned by whoever
		// called CreateApplication (EntryPoint.h deletes it after Run returns),
		// so Application must not also own it.
		static Application *s_Instance;
	};

	// To be defined in CLIENT
	//
	// Factory the client implements to produce its Application subclass.
	// EntryPoint.h declares it extern and calls it, so the engine never names a
	// concrete application type.
	//
	// @returns A newly constructed Application. The caller owns it and is
	//          responsible for deleting it.
	Application *CreateApplication();

}