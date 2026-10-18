// Input.h -- engine-level input queries, fronted by a platform backend.
//
// Shape: a static facade over a single polymorphic backend instance. Callers
// write Input::IsKeyPressed(Key::A) with no platform knowledge; the concrete
// backend (WindowsInput, LinuxInput, MacInput) answers through the protected
// *Impl virtuals.

#pragma once

#include "Wasteland/Core/Core.h"

namespace Wasteland
{

	// Static input query facade.
	//
	// Not intended to be constructed or subclassed by callers -- the static
	// entry points forward to whichever backend has claimed s_Instance, and the
	// *Impl methods are protected so only that backend can answer them.
	class WL_API Input
	{
	public:
		// Virtual so a backend deleted through an Input* is destroyed
		// correctly; defaulted because there is nothing to release here.
		virtual ~Input() = default;

		// True while the given key is held down.
		//
		// Level-triggered, not edge-triggered: this stays true for as long as
		// the key is down, so a caller polling every frame sees it true
		// repeatedly. The KeyPressedEvent in Events/KeyEvent.h is the
		// edge-triggered counterpart.
		//
		// NOTE: unlike GetMouseDelta below, this dereferences s_Instance with
		// no null check. Calling it before a platform backend has registered
		// itself is a null-pointer dereference. The same caveat applies to
		// IsMouseButtonPressed, GetMousePosition, GetMouseX and GetMouseY.
		//
		// @param keycode  Engine key code, from Core/KeyCodes.h.
		inline static bool IsKeyPressed(int keycode) { return s_Instance->IsKeyPressedImpl(keycode); }

		// True while the given mouse button is held down.
		//
		// Same level-triggered semantics as IsKeyPressed, and the same
		// unguarded s_Instance dereference.
		//
		// @param button  Engine mouse button code, from
		//                Core/MouseButtonCodes.h.
		inline static bool IsMouseButtonPressed(int button) { return s_Instance->IsMouseButtonPressedImpl(button); }

		// Current cursor position in window coordinates.
		//
		// Returned as a pair by value rather than filling out-parameters, so
		// structured bindings work: `auto [x, y] = Input::GetMousePosition();`
		inline static std::pair<float, float> GetMousePosition() { return s_Instance->GetMousePositionImpl(); }

		// Per-frame mouse motion, derived by diffing consecutive GetMousePosition samples.
		// NOTE: stateful — the first call ever returns {0,0}, and a second call within the
		// same frame also returns {0,0} (the cache is already consumed). Call exactly once
		// per frame, at the end of input processing.
		//
		// Mechanics: holds the previous sample in function-local statics and
		// subtracts. Because the cache is function-static rather than a member,
		// it is process-wide -- there is exactly one mouse-delta budget per
		// frame no matter how many callers ask. A second consumer in the same
		// frame gets {0,0} rather than the same delta, which is what the NOTE
		// above warns about. There is also no per-frame reset hook, so the
		// state carries across a frame boundary on its own; correctness depends
		// on the cursor position being sampled after the frame's motion events
		// have been applied.
		//
		// This is the one accessor that guards s_Instance, returning {0,0} when
		// no backend is registered rather than crashing.
		inline static std::pair<float, float> GetMouseDelta()
		{
			// Function-local statics rather than members of Input: they must
			// persist across calls without any instance to hang off, since the
			// entry point is static.
			static float s_LastX = 0.0f;
			static float s_LastY = 0.0f;
			// Tracks whether a previous sample exists at all. Without it, the
			// first call would report the difference from (0,0), which is the
			// cursor's position in window space rather than any real motion.
			static bool s_HasLast = false;

			// No backend yet: report no motion. The other accessors in this
			// class omit this check and would dereference null here.
			if (!s_Instance)
				return {0.0f, 0.0f};

			// Structured binding on the returned pair (C++17).
			auto [x, y] = s_Instance->GetMousePositionImpl();

			// First sample ever: seed the cache and report zero motion, since
			// there is no previous position to diff against.
			if (!s_HasLast)
			{
				s_LastX = x;
				s_LastY = y;
				s_HasLast = true;
				return {0.0f, 0.0f};
			}

			// Motion since the previous sample, computed before the cache is
			// overwritten below.
			float dx = x - s_LastX;
			float dy = y - s_LastY;
			// Update the cache unconditionally, so every call consumes its own
			// sample and a second call in the same frame sees no change.
			s_LastX = x;
			s_LastY = y;
			return {dx, dy};
		}

		// Current cursor X in window coordinates. Convenience wrapper over
		// GetMousePosition, for call sites that only need one axis.
		inline static float GetMouseX() { return s_Instance->GetMouseXImpl(); }

		// Current cursor Y in window coordinates. See GetMouseX.
		inline static float GetMouseY() { return s_Instance->GetMouseYImpl(); }

	protected:
		// Backend hooks. Pure virtual, so a concrete platform backend must
		// supply all five.
		//
		// Protected rather than private so the static facades above can reach
		// them through s_Instance while external code cannot bypass the
		// facade and call them directly.

		// Backend's answer for IsKeyPressed.
		virtual bool IsKeyPressedImpl(int keycode) = 0;

		// Backend's answer for IsMouseButtonPressed.
		virtual bool IsMouseButtonPressedImpl(int button) = 0;

		// Backend's answer for GetMousePosition.
		virtual std::pair<float, float> GetMousePositionImpl() = 0;

		// Backend's answer for GetMouseX. Separate from GetMousePositionImpl
		// rather than derived from it, so a backend can report X without
		// building a pair.
		virtual float GetMouseXImpl() = 0;

		// Backend's answer for GetMouseY. See GetMouseXImpl.
		virtual float GetMouseYImpl() = 0;

	private:
		// The active backend, owned by whoever created it (the platform
		// window, in practice).
		//
		// Raw pointer, not a smart pointer: Input does not create the backend
		// and does not destroy it either, so it has no business owning it.
		//
		// Uninitialized to null implicitly, which is why the dereferences above
		// are only safe once a platform window has registered itself.
		static Input *s_Instance;
	};

}