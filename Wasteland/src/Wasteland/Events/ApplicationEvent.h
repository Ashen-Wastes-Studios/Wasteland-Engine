// ApplicationEvent.h -- window and frame lifecycle events.
//
// These are the engine talking to itself rather than to the user: a window
// resize, a close request, the three points in the frame. All are tagged
// EventCategoryApplication.
//
// EventCategoryApplication is deliberately distinct from EventCategoryInput --
// window events are not input, so a listener subscribed to input is not woken
// by a resize.

#pragma once

#include "wlpch.h"

#include "Event.h"

namespace Wasteland {

	// The window's drawable size changed.
	//
	// Width and height are unsigned int, matching the framebuffer/swapchain
	// dimensions the renderer works in rather than the signed dimensions some
	// windowing APIs report.
	class WL_API WindowResizeEvent : public Event
	{
	public:
		// Records the new size. Stored as given; nothing validates that the
		// values are non-zero or within the display's supported range, so a
		// minimized window can legitimately deliver 0x0.
		//
		// @param width   New width in pixels.
		// @param height  New height in pixels.
		WindowResizeEvent(unsigned int width, unsigned int height) 
			: m_Width(width), m_Height(height) {}

		// New width in pixels. Inline because it is a bare member read on a
		// hot path -- the renderer calls this whenever it resizes.
		inline unsigned int GetWidth() const { return m_Width; }

		// New height in pixels. Inline for the same reason as GetWidth().
		inline unsigned int GetHeight() const { return m_Height; }

		// "WindowResizeEvent: <width>, <height>"
		//
		// Builds the text through a local stringstream and returns it by
		// value, so this allocates on every call. Fine for logging and debug
		// overlays, which is what it exists for.
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowResizeEvent: " << m_Width << ", " << m_Height;
			return ss.str();
		}

		// Supplies GetStaticType / GetEventType / GetName for
		// EventType::WindowResize.
		EVENT_CLASS_TYPE(WindowResize)
		// No Input bit: a resize is not user input.
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	private:
		// New size in pixels. Both are always assigned by the constructor.
		unsigned int m_Width, m_Height;
	};

	// The user or the system asked to close the window.
	//
	// Carries no payload. The platform window callback translates this into
	// Application::Close(). A layer that wants to veto the close (an
	// unsaved-changes prompt) can listen for it -- but read the handled-flag
	// caveat in Event.h first, since Dispatch writes the protected flag rather
	// than the public one.
	class WL_API WindowCloseEvent : public Event
	{
	public:
		// Defaulted; this event has no state to initialize.
		WindowCloseEvent() {}

		EVENT_CLASS_TYPE(WindowClose)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	// One iteration of the frame loop has begun, before any update work.
	//
	// Distinct from AppUpdateEvent so a listener can observe frame boundaries
	// without inferring them from update cadence. Nothing in the engine
	// currently raises it, so handlers registered for it simply never fire.
	class WL_API AppTickEvent : public Event
	{
	public:
		// Defaulted; no payload.
		AppTickEvent() {}

		EVENT_CLASS_TYPE(AppTick)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	// The per-layer update pass is about to run for this frame.
	//
	// Also currently unraised: Application::Run calls Layer::OnUpdate directly
	// rather than posting this event, so the distinction from AppTick is
	// nominal at present.
	class WL_API AppUpdateEvent : public Event
	{
	public:
		// Defaulted; no payload.
		AppUpdateEvent() {}

		EVENT_CLASS_TYPE(AppUpdate)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	// The frame's rendering is about to begin.
	//
	// Like the other two frame-lifecycle events, not currently posted by the
	// engine -- the render pass in Application::Run runs unconditionally.
	class WL_API AppRenderEvent : public Event
	{
		// Defaulted; no payload.
		//
		// Note this class has no access-specifier: its members are therefore
		// PRIVATE by the class default, unlike every sibling above. Nothing
		// outside needs to reach the constructor or the macro-generated
		// accessors, and the dispatcher works through the Event base, so the
		// difference has no practical effect -- but it is inconsistent with
		// the rest of this file and will surprise anyone extending it.
		AppRenderEvent() {}

		EVENT_CLASS_TYPE(AppRender)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

}