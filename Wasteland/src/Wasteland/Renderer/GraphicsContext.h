#pragma once

namespace Wasteland {

	// A pure interface: three capabilities and no platform includes at
	// all. MacWindow holds a GraphicsContext* with nothing more than this
	// forward declaration, which is why that header parses without any
	// graphics backend present.
	class GraphicsContext
	{
	public:
		// virtual + defaulted rather than pure: contexts are owned by value
		// inside each platform window and destroyed directly, so the vtable
		// is for the polymorphic Init/Swap calls, not for ownership.
		virtual ~GraphicsContext() = default;

		// Pure virtual: there is no meaningful default. Every backend must
		// create its device and swapchain here, which is also the natural
		// place for the API switch to have already happened.
		virtual void Init() = 0;
		virtual void SwapBuffers() = 0;
		// NOT pure, and empty -- a deliberate no-op default. This is the
		// hook the NVRHI backend fills in (acquire/present on a swapchain);
		// the GL backend has no per-frame acquire step and leaves it alone.
		// DEFECT (silent): because the default does nothing, a backend that
		// forgets to override it gets no compile error and no diagnostic --
		// frames simply are not presented, which reads as a hung window.
		virtual void BeginFrame() {}
	};

}