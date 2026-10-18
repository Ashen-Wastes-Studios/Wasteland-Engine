#pragma once

// Layer.h -- base class for a unit of engine/game behaviour that has an
// explicit attach/detach lifetime and receives frame + event callbacks.
//
// A Layer is the primary extension point of the engine. Anything that needs
// to run every frame (the editor, a scripting host, a debug overlay) is
// expressed as a Layer rather than being special-cased inside Application.
//
// Who calls what (verified against Application.cpp and LayerStack.cpp):
//
//   Application::PushLayer / PushOverlay
//       -> LayerStack::PushLayer / PushOverlay   (insert into m_Layers)
//       -> layer->OnAttach()                     <-- called by Application,
//                                                    NOT by LayerStack
//
//   Application::Run(), every frame, while the window is not minimized:
//       -> for each layer, forward order:  layer->OnUpdate(timestep)
//       -> for each layer, forward order:  layer->OnImGuiRender()
//                                            (only if the ImGui layer
//                                             reports HasBackend())
//
//   Application::OnEvent:
//       -> for each layer, REVERSE order: (*--it)->OnEvent(e)
//          stopping early once e.Handled is set
//
//   LayerStack::PopLayer / PopOverlay
//       -> layer->OnDetach()                     <-- called by LayerStack
//       -> erase from m_Layers
//
//   LayerStack::~LayerStack
//       -> delete layer, WITHOUT calling OnDetach()
//
// Two asymmetries in that list are worth knowing before relying on them:
//
//   1. OnAttach is invoked by Application, not by LayerStack::PushLayer. If
//      you push straight onto a LayerStack, the hook never runs.
//
//   2. ~LayerStack() deletes its layers without calling OnDetach first. A
//      layer torn down via stack destruction, rather than an explicit pop,
//      gets no detach notification and so cannot rely on OnDetach to release
//      what OnAttach acquired. Prefer popping over letting the stack die.
//
// Note that OnImGuiRender runs in the SAME direction as OnUpdate, not in
// reverse, so a panel drawn by a layer earlier in m_Layers is painted over by
// one drawn later. Reverse ordering applies to OnEvent only.

#include "Wasteland/Core/Core.h"  // WL_API export macro
#include "Wasteland/Core/Timestep.h"  // Timestep: the frame delta handed to OnUpdate
#include "Wasteland/Events/Event.h"  // Event base type forwarded to OnEvent

namespace Wasteland {

	// Base class for engine behaviour with an attach/detach lifetime.
	//
	// Every callback is virtual with an empty default body, so a derived
	// layer overrides only the hooks it cares about. The defaults are
	// deliberately no-ops rather than pure virtual: that keeps Layer concrete
	// (instantiable as a placeholder) and lets a layer be added to the stack
	// before its behaviour is written.
	//
	// Lifetime:
	//   1. Constructed.
	//   2. Inserted into a LayerStack, after which Application calls
	//      OnAttach() -- but only if the push went through
	//      Application::PushLayer / PushOverlay.
	//   3. OnUpdate() and OnImGuiRender() run every frame while attached.
	//   4. OnDetach() runs on an explicit pop, or not at all if the stack is
	//      simply destroyed.
	//
	// All five hooks are called from the main thread, from inside the frame
	// loop or the event dispatch of Application. A layer that needs background
	// work must own the thread itself and hand results back through a
	// synchronized queue -- the engine does no implicit threading on a
	// layer's behalf.
	class WL_API Layer
	{
	public:
		// Constructs a layer and records a name for it.
		//
		// Note: this constructor is intentionally NOT explicit, so a
		// std::string converts implicitly to a Layer. Preserved as-is.
		//
		// @param name  Debug/display identifier, stored verbatim and returned
		//              by GetName(). Used in profiler dumps and log lines, so
		//              it should be human-readable rather than a type name.
		//              The declaration spells this parameter `name` while the
		//              definition in Layer.cpp spells it `debugName`; the two
		//              refer to the same value and callers pass it
		//              positionally, so the mismatch is inert.
		Layer(const std::string& name = "Layer");

		// Virtual destructor so a Layer held by pointer (which is how the
		// LayerStack stores them) destroys correctly through the base pointer.
		// Empty: a layer is responsible for its own cleanup in OnDetach, and
		// note that ~LayerStack does not give it the chance to run.
		virtual ~Layer();

		// Called once, by Application::PushLayer / PushOverlay, immediately
		// after the layer is inserted into the stack.
		//
		// This is the place to acquire graphics resources, register event
		// handlers, and open files. Anything created here should be released
		// in OnDetach() so a layer can be pushed and popped repeatedly
		// without leaking. Default: no-op.
		virtual void OnAttach() {}

		// Called once by LayerStack::PopLayer / PopOverlay, before the layer
		// is erased from the stack.
		//
		// Must undo everything OnAttach() acquired. Be aware this is NOT
		// called when the LayerStack itself is destroyed -- see the file
		// header. Default: no-op.
		virtual void OnDetach() {}

		// Per-frame update, called for every layer in forward order each tick
		// while the window is not minimized.
		//
		// @param ts  Frame delta time. Use it rather than reading a clock
		//            directly, so every layer in a frame agrees on the same
		//            dt. Note that Application::Run computes it as a raw
		//            glfwGetTime() difference and does NOT clamp it, so a
		//            debugger break, a long asset load, or a stalled frame
		//            hands you an arbitrarily large dt. Anything integrating
		//            velocity or draining a timer should clamp ts itself
		//            rather than assuming it is already bounded.
		// Default: no-op.
		virtual void OnUpdate(Timestep ts) {}

		// ImGui draw pass, called once per frame for every attached layer.
		//
		// Runs after OnUpdate() for all layers, so panels can display the
		// state this frame's update just produced. Only invoked when the
		// ImGui layer reports HasBackend(), so a headless or server build
		// that never creates a backend will not have this called. Iterates in
		// the same forward order as OnUpdate, so later layers draw over
		// earlier ones. Default: no-op.
		virtual void OnImGuiRender() {}

		// Event sink, dispatched in REVERSE layer order with an early-out
		// once the event is marked handled.
		//
		// @param event  The event being dispatched. Inspect event.GetType()
		//               and query the typed payload to decide whether to
		//               handle it, then set event.Handled to consume it;
		//               leaving it false passes the event to the next layer
		//               down. The base Layer ignores everything.
		// Default: no-op.
		virtual void OnEvent(Event& event) {}

		// Returns the layer's display name.
		//
		// Returns a const reference to the member rather than a copy, so this
		// is cheap to call in a log line. The reference is invalidated if the
		// layer is destroyed, and by nothing else -- the name is set once at
		// construction and never mutated.
		inline const std::string& GetName() const { return m_DebugName; }

	protected:
		// Display/debug name for this layer, set by the constructor and
		// exposed through GetName(). Protected rather than public so that
		// renaming is a deliberate act by a subclass rather than something
		// the outside world can do at any time.
		std::string m_DebugName;
	};

}
