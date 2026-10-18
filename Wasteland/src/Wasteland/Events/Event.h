// Event.h -- the engine's event type, its type/category tags, and the
// dispatcher that routes an event to a handler.
//
// Model: events are plain value objects with virtual accessors for their type,
// name and category. Concrete events subclass Event and use the
// EVENT_CLASS_TYPE / EVENT_CLASS_CATEGORY macros to supply those accessors, so
// adding a new event is a class definition plus one enum entry, nothing more.
//
// Dispatch: EventDispatcher wraps a reference to one event and offers a
// templated Dispatch<T>() that invokes a handler only when the event is
// actually a T. Application::OnEvent walks the layer stack in reverse, calling
// Dispatch<T> once per event type of interest.

#pragma once

#include "wlpch.h"

#include "Wasteland/Core/Core.h"

namespace Wasteland {

	// Events in Wasteland are currently blocking, meaning when an event occurs it
	// immediately gets dispatched and must be dealt with right then and there.
	// For the future, a better strategy might be to buffer events in an event
	// bus and process the during the "event" part of the update stage.

	// Discriminator identifying the concrete kind of an event.
	//
	// enum class rather than a plain enum, so the enumerators do not leak into
	// the enclosing namespace and cannot be confused with EventCategory's
	// similarly named members. None is the sentinel for "no event" and is 0 so
	// that a zero-initialized EventType is a safe default.
	//
	// Each enumerator must have a matching EVENT_CLASS_TYPE declaration on the
	// concrete event class; the two lists are kept in step by hand, so adding a
	// value here without adding the class (or vice versa) fails to compile at
	// the dispatch site rather than silently mismatching.
	enum class EventType
	{
		None = 0,
		// Window lifecycle, raised by the platform window layer.
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved,
		// Frame lifecycle, raised by Application::Run.
		AppTick, AppUpdate, AppRender,
		// Keyboard, from KeyEvent.h.
		KeyPressed, KeyReleased, KeyTyped,
		// Mouse, from MouseEvent.h.
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled
	};

	// Bit flags describing which categories an event belongs to.
	//
	// Plain (unscoped) enum, not enum class -- the values are combined with BIT
	// and then OR'd together, and the resulting int is what
	// Event::GetCategoryFlags() returns. Note that this makes the enumerators
	// visible in the Wasteland namespace, so EventCategory::None sits alongside
	// EventType::None; they are unrelated and both mean "nothing".
	//
	// Application is the window/frame group; Input is the umbrella category
	// shared by every keyboard and mouse event, so a handler that wants "any
	// input at all" tests only EventCategoryInput.
	enum EventCategory
	{
		None = 0,
		EventCategoryApplication = BIT(0),
		EventCategoryInput = BIT(1),
		EventCategoryKeyboard = BIT(2),
		EventCategoryMouse = BIT(3),
		EventCategoryMouseButton = BIT(4)
	};

// Boilerplate every concrete event uses to satisfy Event's pure virtuals.
//
// Expands to three members: a static type tag used by the dispatcher for
// compile-time matching, an override that returns it for runtime queries, and a
// GetName() that stringifies the enumerator name via the # operator -- so the
// debug name can never drift out of sync with the type.
//
// The trailing backslashes continue the macro across lines; note this makes the
// macro fragile to edit, since inserting a comment on an interior line without
// its own continuation will truncate it.
#define EVENT_CLASS_TYPE(type) static EventType GetStaticType() { return EventType::type; }\
				virtual EventType GetEventType() const override { return GetStaticType(); }\
				virtual const char* GetName() const override { return #type; }

// Boilerplate for the category bitmask, overriding Event::GetCategoryFlags.
// OR several categories together to place an event in more than one group:
//   EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
#define EVENT_CLASS_CATEGORY(category) virtual int GetCategoryFlags() const override { return category; }

	// Abstract base for everything the engine dispatches.
	//
	// Lifetime is stack-scoped by convention: a caller constructs the concrete
	// event, dispatches it, and lets it die at the end of scope. Nothing
	// retains an Event past the dispatch, so the reference held by
	// EventDispatcher is valid only for that window.
	class WL_API Event
	{
		// Grants EventDispatcher access to the protected m_Handled below.
		// Needed because Dispatch() writes the handled flag directly rather
		// than going through a public setter.
		friend class EventDispatcher;

	public:
		// Virtual so an Event deleted through a base pointer is destroyed
		// correctly; defaulted because there is nothing to release.
		virtual ~Event() = default;

		// Runtime identity of this event, supplied by EVENT_CLASS_TYPE.
		// Compared against T::GetStaticType() by the dispatcher.
		virtual EventType GetEventType() const = 0;

		// Human-readable name for logs and debug overlays, also supplied by
		// EVENT_CLASS_TYPE. Returns a pointer to a string literal, so it is
		// valid for the life of the program and must not be freed.
		virtual const char* GetName() const = 0;

		// Bitmask of EventCategory values this event belongs to, supplied by
		// EVENT_CLASS_CATEGORY. Returned as int rather than EventCategory
		// because it is a combination of flags, not a single enumerator.
		virtual int GetCategoryFlags() const = 0;

		// Text form of the event, defaulting to just GetName().
		//
		// Concrete events that carry a payload (a resize's dimensions, a key's
		// code) override this to append that payload, which is what makes
		// `std::cout << event` useful in a debugger.
		virtual std::string ToString() const { return GetName(); }

		// Tests whether this event belongs to the given category.
		//
		// The bitwise AND yields an int, which is then implicitly converted to
		// bool by the return type -- any nonzero overlap is true. Declared
		// non-const even though it only reads, so it cannot be called on a
		// const Event reference.
		//
		// @param category  A single category flag, e.g. EventCategoryKeyboard.
		//                   Passing a combined mask tests for ANY of those
		//                   categories, which is usually what a caller wants.
		inline bool IsInCategory(EventCategory category)
		{
			return GetCategoryFlags() & category;
		}

		// Public handled flag. This is the one dispatchers actually set and the
		// one callers test to stop further propagation.
		bool Handled = false;

	protected:
		// A second handled flag, shadowing the public Handled above.
		//
		// Two flags for one piece of state is a latent bug, not a design: code
		// that sets or reads the wrong one will not see the other's value.
		// EventDispatcher::Dispatch touches both (see the duplicated type check
		// there), which is the symptom. Nothing outside Event and
		// EventDispatcher should be using this member.
		bool m_Handled = false;
	};

	// Routes one event to typed handlers.
	//
	// Holds a reference to an event for the duration of a dispatch and offers
	// Dispatch<T>(), which runs a handler only if that event is a T. This is
	// what lets a listener write `dispatcher.Dispatch<KeyPressedEvent>(...)`
	// and have the handler receive a properly typed reference without any
	// casting at the call site.
	//
	// Short-lived by design -- construct one around an event, dispatch, discard.
	class EventDispatcher
	{
		// Handler signature: takes the event by mutable reference and returns
		// whether it consumed the event.
		template<typename T>
		using EventFn = std::function<bool(T&)>;

	public:
		// Binds the dispatcher to the event to be routed.
		//
		// Stored by reference, not copied: the dispatcher does not own the
		// event, and the reference must outlive the dispatcher.
		//
		// @param event  The event to dispatch. Must outlive this dispatcher.
		EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		// Invokes func if, and only if, the bound event is a T.
		//
		// @param func  Handler receiving the event as a T&. Its return value
		//              becomes the event's handled flag, so returning true
		//              stops the event propagating to further listeners.
		//
		// @returns true if the event was of type T and func was therefore run,
		//          false if the type did not match and func was not called.
		//          Callers use the false case to fall through to the next
		//          type they care about.
		template<typename T>
		bool Dispatch(EventFn<T> func)
		{
			// Matches on the STATIC type tag rather than the virtual
			// GetEventType(), so the comparison happens without a vtable call
			// and T is known to be complete here.
			//
			// The C-style cast to T* is an unchecked downcast from Event&, which
			// is sound only because the static type comparison above has just
			// established that the dynamic type really is T.
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.m_Handled = func(*(T*)&m_Event);
				return true;
			}
			// NOTE: this block repeats the exact same condition as the one
			// above, which always returns true when it is taken -- so every
			// statement here is unreachable. The effect is that the public
			// Handled flag is never written by Dispatch, and only the
			// protected m_Handled is. Callers that test `event.Handled` (the
			// documented way to detect consumption) therefore never see it set.
			// Left as written because this pass is comments-only; collapsing
			// the duplicate and settling on one flag is a behaviour change that
			// belongs in its own commit.
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.Handled = func(*(T*)&m_Event);
				return true;
			}
			// Type did not match: func was not invoked and the event is
			// untouched.
			return false;
		}

	private:
		// The event being routed. A reference, so no ownership and no copy;
		// the referent must outlive the dispatcher.
		Event& m_Event;
	};

	// Streams an event using its ToString(), so `std::cout << event` prints the
	// payload-carrying form rather than just the type name.
	inline std::ostream& operator<<(std::ostream& os, const Event& e)
	{
		return os << e.ToString();
	}

}