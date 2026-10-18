// MouseEvent.h -- mouse movement, scroll, and button events.
//
// Four events: two positional (move, scroll) and two edge-based (button
// pressed/released) sharing a common base. Positions are float rather than int
// because some backends and high-DPI paths deliver sub-pixel coordinates.

#pragma once

#include "wlpch.h"

#include "Event.h"

namespace Wasteland {

	// The mouse cursor moved to a new position.
	//
	// The position is relative to the window's client area, not the screen.
	class WL_API MouseMovedEvent : public Event
	{
	public:
		// @param x  New cursor X in window coordinates.
		// @param y  New cursor Y in window coordinates.
		MouseMovedEvent(float x, float y)
			: m_MouseX(x), m_MouseY(y) { }

		// Cursor X in window coordinates. Inline: a bare member read, and this
		// fires on every mouse motion.
		inline float GetX() const { return m_MouseX; }

		// Cursor Y in window coordinates. Inline for the same reason as GetX().
		inline float GetY() const { return m_MouseY; }

		// "MouseMovedEvent: <x>, <y>"
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseMovedEvent: " << m_MouseX << ", " << m_MouseY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseMoved)
		// Categorized as both Mouse and Input.
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		// Cursor position in window coordinates, as supplied to the
		// constructor. Stored by value; the event is a snapshot of where the
		// cursor was when the event was raised, not a live query.
		float m_MouseX, m_MouseY;
	};

	// A scroll wheel or trackpad reported movement.
	//
	// Two axes because trackpads and high-resolution wheels report horizontal
	// scroll alongside vertical, and a consumer that ignored it would lose that
	// input.
	class WL_API MouseScrolledEvent : public Event
	{
	public:
		// @param xOffset  Horizontal scroll delta.
		// @param yOffset  Vertical scroll delta.
		//
		// Magnitude is in whatever units the backend reports -- typically
		// "notches" or raw device units, not pixels -- and the sign gives
		// direction. No unit normalization happens here.
		MouseScrolledEvent(float xOffset, float yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset) { }

		// Horizontal scroll delta. Inline bare member read.
		inline float GetXOffset() const { return m_XOffset; }

		// Vertical scroll delta. Inline bare member read.
		inline float GetYOffset() const { return m_YOffset; }

		// "MouseScrolledEvent: <xOffset>, <yOffset>"
		//
		// Reads through the accessors rather than the members directly --
		// equivalent here, but it keeps the printed values honest if either
		// accessor ever gains clamping or conversion.
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		// Scroll deltas as supplied to the constructor.
		float m_XOffset, m_YOffset;
	};

	// Shared base for the two mouse button events.
	//
	// Abstract in practice: the constructor is protected, so only the pressed
	// and released subclasses exist. Declares the category but not the type,
	// since those differ per subclass.
	class WL_API MouseButtonEvent : public Event
	{
	public:
		// Which button changed, as an engine button code -- see
		// Core/MouseButtonCodes.h.
		//
		// Note this is NOT the same thing as EventCategoryMouseButton. That
		// category flag is declared in Event.h and exported to Python in
		// Bindings.cpp, but no event class ever includes it in its
		// GetCategoryFlags() -- the concrete subclasses below use
		// EventCategoryMouse | EventCategoryInput instead. So a C++ listener
		// testing IsInCategory(EventCategoryMouseButton) matches nothing.
		inline int GetMouseButton() const { return m_Button; }

		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	protected:
		// Protected because MouseButtonEvent is a base only.
		//
		// @param button  Engine mouse button code.
		MouseButtonEvent(int button)
			: m_Button(button) { }

		// Engine mouse button code, as supplied to the constructor. Protected
		// so subclasses can read it directly when building ToString().
		int m_Button;
	};

	// A mouse button went down.
	//
	// Note there is no double-click or repeat-count information on this event;
	// multi-click detection is left to the consumer.
	class WL_API MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		// @param button  Engine mouse button code for the button pressed.
		MouseButtonPressedEvent(int button)
			: MouseButtonEvent(button) { }

		// "MouseButtonPressedEvent: <button>"
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonPressedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	// A mouse button went up.
	class WL_API MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		// @param button  Engine mouse button code for the button released.
		MouseButtonReleasedEvent(int button)
			: MouseButtonEvent(button) { }

		// "MouseButtonReleasedEvent: <button>"
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonReleasedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonReleased)
	};

}