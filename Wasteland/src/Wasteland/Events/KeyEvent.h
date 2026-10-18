// KeyEvent.h -- keyboard events: press, release, and text entry.
//
// Three distinct events rather than one with a state flag, because the three
// answer different questions: "is it held" (pressed/released edges), "what did
// the user just type" (KeyTyped, which carries a character and is layout- and
// IME-aware), and repeat handling for held keys.

#pragma once

// The precompiled header, in an EVENT header.
//
// DEFECT, and a notable one: a header that every translation unit including
// keyboard events must include cannot itself include the project's PCH.
// wlpch.h is meant to be force-included per translation unit, so depending on
// it here couples this header to the project's build setup and to whatever
// wlpch happens to pull in. <string> and <sstream> are what this file
// actually needs for ToString() and should be included directly; if wlpch is
// force-included the compiler will not need the include at all.
#include "wlpch.h"

// The Event base, for the category/type macros and the ToString interface.
#include "Event.h"

namespace Wasteland {

	// Shared base for the three keyboard events.
	//
	// Abstract in practice: its constructor is protected, so KeyEvent itself
	// cannot be instantiated and only the three subclasses below exist. That
	// keeps a stray bare KeyEvent from ever entering a dispatch chain, where it
	// would have no EventType.
	//
	// Declares EVENT_CLASS_CATEGORY but NOT EVENT_CLASS_TYPE, because the
	// category is common to all keyboard events while the type differs.
	// The shared base for all three keyboard events.
	//
	// WL_API is the export/import macro, needed because these classes cross a
	// shared-library boundary -- the concrete event types are constructed by the
	// platform layer and consumed by game code.
	//
	// Abstract in practice rather than in the language sense: the constructor is
	// protected below, so no bare KeyEvent can be constructed. That is a softer
	// guarantee than an abstract class, and it means the class is still
	// instantiable-in-principle if a future refactor makes the constructor
	// public.
	class WL_API KeyEvent : public Event
	{
	public:
		// The key that changed, as an engine key code -- not a platform code
		// and not a character. See Core/KeyCodes.h for the enumeration.
		// The key involved, as an ENGINE key code.
		//
		// inline in the header returning the member directly. Note it returns int,
		// not a scoped enum type -- so a caller can compare against any int, and a
		// typo compiles. The alternative would have caught it.
		//
		// Deliberately not a character: for KeyTypedEvent this is still the PHYSICAL
		// key, which is what makes layout-independent behaviour possible.
		inline int GetKeyCode() const { return m_KeyCode; }

		// Keyboard events are both "keyboard" and "input", so a listener can
		// subscribe to either granularity.
		// Two categories at once, so a listener can subscribe to keyboard events
		// specifically or to input as a whole.
		//
		// The macro is what gives the class a static EventCategory value and the
		// type-trait machinery EventDispatcher uses to match. OR-ing the categories
		// means a dispatcher asking for EventCategoryInput sees these too.
		EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	protected:
		// Protected because KeyEvent is a base, never constructed directly.
		//
		// @param keycode  Engine key code for the key involved.
		// Protected constructor.
		//
		// @param keycode  Engine key code for the key involved.
		//
		// Protected rather than private precisely so the three subclasses below can
		// call it while no outside code can construct a bare KeyEvent. That is what
		// the class comment means by 'abstract in practice'.
		KeyEvent(int keycode)
			// Brace on the same line, unlike the subclasses below which put the opening
			// brace on the next line -- a cosmetic inconsistency the KeyTypedEvent
			// comment acknowledges.
			: m_KeyCode(keycode) { }

		// Engine key code, as passed to the constructor. Protected rather than
		// private so subclasses can read it directly when building their
		// ToString() without going back through the accessor.
		// The key code, as passed to the constructor.
		//
		// DEFECT: NOT initialised. Every current subclass initialises it through the
		// base constructor, so it is always set in practice -- but a member with no
		// in-class initialiser in a class whose subclasses chain to it is one
		// incomplete constructor call away from reading indeterminate memory.
		// `= 0` would cost nothing.
		int m_KeyCode;
	};

	// A key went down, or is repeating because it is held.
	//
	// The repeat count distinguishes a fresh press from auto-repeat, which a UI
	// needs: a button should fire once on the initial press and a text field
	// should accept the held key repeating.
	// Key down, or repeating because it is held.
	//
	// One event for both the initial press and auto-repeat, distinguished by the
	// repeat count -- rather than separate events, which would force every
	// listener to handle two types for one physical action.
	class WL_API KeyPressedEvent : public KeyEvent
	{
	public:
		// @param keycode     Engine key code for the key.
		// @param repeatCount 0 for the initial press, 1+ for each auto-repeat
		//                   while held. The convention is 0-based; see
		//                   GetRepeatCount().
		// @param keycode      Engine key code.
		// @param repeatCount  0 for the initial press, 1+ per auto-repeat.
		//
		// No default on repeatCount, so a caller must state which kind of press this
		// is. That is deliberate: defaulting it would make 'forgot to pass it'
		// indistinguishable from 'initial press'.
		KeyPressedEvent(int keycode, int repeatCount)
			: KeyEvent(keycode), m_RepeatCount(repeatCount) { }

		// Auto-repeat ordinal: 0 for the initial press, then 1, 2, ... for each
		// repeat. Zero-based, so "is this the initial press" is
		// `GetRepeatCount() == 0`.
		// The auto-repeat ordinal, zero-based.
		//
		// Zero-based rather than one-based, so 'is this the initial press' is a
		// simple `== 0`. The convention is stated in both the constructor comment and
		// here, which is the right amount of repetition for something that is easy to
		// get backwards.
		inline int GetRepeatCount() const { return m_RepeatCount; }

		// "KeyPressedEvent: <keycode> (<n>repeats)"
		//
		// Note the format quirk: the count is appended with no separating
		// space, and the singular case reads "(0repeats)". Left as written.
		// The debug representation.
		//
		// override because Event declares it virtual. Returning std::string by value
		// rather than a const char* is the better choice -- no static buffer to
		// share between calls.
		std::string ToString() const override
		{
			std::stringstream ss;
			// The format quirk the comment above names: no space before the count, so
			// the singular case reads '(0repeats)'.
			//
			// The comment says 'left as written', so it is a known-and-accepted quirk
			// rather than an oversight. Worth noting it is the only one of the three
			// ToString implementations with a parenthesised suffix, so the three do not
			// share a format.
			ss << "KeyPressedEvent: " << m_KeyCode << " (" << m_RepeatCount << "repeats)";
			return ss.str();
		}

		EVENT_CLASS_TYPE(KeyPressed)
	private:
		// Auto-repeat ordinal, as supplied to the constructor.
		// The repeat ordinal, as supplied.
		//
		// DEFECT: NOT initialised, and unlike m_KeyCode above there is no base-class
		// constructor setting it -- the only constructor takes it as a parameter. So
		// it is always initialised in practice, but only because the class has
		// exactly one constructor.
		int m_RepeatCount;
	};

	// A key went up.
	//
	// Fires once per press, so a key held through several KeyPressedEvents
	// still produces exactly one release.
	// Key up.
	//
	// Fires once per press, so a key held through several KeyPressedEvents still
	// produces exactly one release -- the property that makes press/release
	// pairing work without a counter.
	class WL_API KeyReleasedEvent : public KeyEvent
	{
	public:
		// @param keycode  Engine key code for the key being released.
		KeyReleasedEvent(int keycode) 
			: KeyEvent(keycode) { }

		// "KeyReleasedEvent: <keycode>"
		std::string ToString() const override
		{
			std::stringstream ss;
			// The simplest of the three formats: name and key code, no suffix.
			ss << "KeyReleasedEvent: " << m_KeyCode;
			return ss.str();
		}

		EVENT_CLASS_TYPE(KeyReleased)
	};

	// The user produced a character.
	//
	// Distinct from KeyPressedEvent because text entry needs the resolved
	// character -- accounting for shift, caps lock, keyboard layout, and IME
	// composition. GetKeyCode() still returns the physical key, so a handler
	// wanting layout-independent behaviour should read the key code, not treat
	// this as a character event.
	// The user produced a character.
	//
	// This is the event that carries text entry. The comment above explains why
	// it is separate from KeyPressedEvent: typing needs the RESOLVED character
	// (shift, caps lock, layout, IME composition), which the platform layer
	// computes and a key-down event does not have.

	// The key code, and the caution that goes with it here: on THIS
	// event GetKeyCode() returns the physical key, not the character.
	// So a handler wanting layout-independent behaviour reads this;
	// one wanting the character reads the platform's resolved value.

	// DEFECT worth noting: KeyTypedEvent carries no CHARACTER
	// member, which is the entire reason the event exists. Its
	// class comment says text entry needs the resolved character,
	// yet the class stores only m_KeyCode from the base. Either the
	// resolved character arrives by a separate mechanism, or this
	// event cannot do the job it was introduced for.

	// Its constructor therefore takes only a key code, consistent
	// with that absence -- and its brace placement differs from both
	// siblings, which the comment above apologises for as cosmetic.
	class WL_API KeyTypedEvent : public KeyEvent
	{
	public:
		// Note the brace placement differs from the sibling constructors: the
		// body starts on the same line here rather than the next. Cosmetic
		// only.
		//
		// @param keycode  Engine key code of the key that produced text.
		// @param keycode  Engine key code of the key that produced text.
		//
		// Takes no character parameter -- consistent with the absence noted above.
		// Whatever resolved the character did so elsewhere.
		KeyTypedEvent(int keycode)
			// The brace placement differs from its siblings, which the comment above
			// acknowledges as cosmetic.
			//
			// Worth noting this is the ONLY formatting difference between the three
			// constructors, and it sits directly under a comment apologising for it -- a
			// reader spends time on a difference that carries no meaning.
			: KeyEvent(keycode) {
		}

		// "KeyTypedEvent: <keycode>"
		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "KeyTypedEvent: " << m_KeyCode;
			return ss.str();
		}

		EVENT_CLASS_TYPE(KeyTyped)
	};

}