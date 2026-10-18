#pragma once

// Gamepad.h -- gamepad connection state, input queries, and deadzone shaping.
//
// Layering: this sits beside Input rather than inside it. Keyboard and mouse
// are singletons bound to the one window; gamepads are a hot-pluggable list of
// slots that can appear and disappear mid-session, and they need a per-frame
// snapshot rather than a live query. Keeping them apart means the keyboard path
// never carries state it does not use.
//
// The axis conventions matter when reading any of this: sticks rest at 0 and
// span [-1, 1]; triggers rest at -1 and span [-1, 1]. See
// GamepadButtonCodes.h for the full note and GetTrigger for the conversion.

#include "Wasteland/Core/GamepadButtonCodes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland {

	// One frame of gamepad state, snapshotted from GLFW.
	//
	// A struct rather than live queries so that a whole frame reads consistent
	// values. GLFW's state is already per-frame, but copying it means a
	// gameplay script polling buttons, then axes, then a trigger cannot
	// straddle a device connect or disconnect and get a torn read.
	//
	// Default-constructible and cheaply copyable: GetState resets one of these
	// by assigning a fresh GamepadState() on every poll.
	struct GamepadState
	{
		// Whether the pad was present when this snapshot was taken.
		//
		// False means every other field is meaningless and should be treated as
		// zero -- which it will be, since GetState resets before filling.
		bool Connected = false;

		// The pad's reported name, e.g. "Xbox Controller".
		//
		// Empty when the device exposes no name, or when the slot is
		// unconnected. Diagnostic only; nothing should branch on its content.
		std::string Name;

		// Digital buttons, indexed by WL_GAMEPAD_BUTTON_*.
		//
		// Sized 16 rather than 15 so it covers every valid index plus one:
		// WL_GAMEPAD_BUTTON_LAST is 14, so indices 0..14 are meaningful and 15
		// is slack. All-false by default, so a disconnected pad reports nothing
		// pressed rather than uninitialised memory.
		bool Buttons[16] = {false};

		// Analog axes, indexed by WL_GAMEPAD_AXIS_*. Triggers rest at -1 and
		// are remapped to 0..1 by the caller; sticks stay in -1..1.
		//
		// Sized 6 to match WL_GAMEPAD_AXIS_LAST (5) inclusive. Zero-initialised,
		// which for a trigger axis is NOT its resting value -- another reason
		// the disconnected case must be caught by Connected rather than by
		// inspecting axis values.
		float Axes[6] = {0.0f};

		// Bounds-checked read of a button.
		//
		// Returns false for an out-of-range index rather than reading past the
		// array, so a bad constant from a script degrades to "not pressed"
		// instead of corrupting memory.
		//
		// @param button  A WL_GAMEPAD_BUTTON_* value.
		// @returns Whether that button is currently held, false if the index is
		//          out of range.
		bool IsButtonPressed(int button) const
		{
			// Upper bound is WL_GAMEPAD_BUTTON_LAST, so the final array slot is
			// deliberately unreachable -- no WL_GAMEPAD_BUTTON_* constant names
			// it.
			if (button < 0 || button > WL_GAMEPAD_BUTTON_LAST)
				return false;
			return Buttons[button];
		}

		// Bounds-checked read of an axis.
		//
		// @param axis  A WL_GAMEPAD_AXIS_* value.
		// @returns The raw axis reading, or 0.0f if the index is out of range.
		//          Note 0.0f is also the sticks' resting value, so an
		//          out-of-range trigger index is indistinguishable from a
		//          centred stick -- check the index, not the value, if that
		//          matters.
		float GetAxis(int axis) const
		{
			if (axis < 0 || axis > WL_GAMEPAD_AXIS_LAST)
				return 0.0f;
			return Axes[axis];
		}
	};

	// Platform gamepad access.
	//
	// Separate from Input rather than folded into it: the mouse and keyboard
	// are singletons bound to the one window, while gamepads are a
	// hot-pluggable list of up to N devices. Mixing them would force the
	// keyboard path to carry state it never uses.
	//
	// All-static, like Input: there is no gamepad instance, only slots. Every
	// method is main-thread only and does no caching -- each call re-polls the
	// device, so a script calling several of these per frame performs several
	// driver round-trips. Prefer one GetState() call and read the struct when
	// a frame needs several values.
	class Gamepad
	{
	public:
		// Highest joystick slot GLFW will probe, and the practical number of
		// controllers a player will ever have plugged in. Polling all 16
		// costs a driver call each, so the count is capped well below the
		// API maximum.
		//
		// Every method that takes a `gamepad` slot index treats this as the
		// exclusive upper bound, so valid slots are 0 .. MaxGamepads-1.
		static constexpr int MaxGamepads = 4;

		// The stick value below which input is treated as zero.
		//
		// Not optional. An unpowered stick rests around 0.02-0.05 rather
		// than exactly 0, so without a deadzone the player drifts slowly
		// sideways whenever the stick is released -- the single most common
		// gamepad complaint there is.
		//
		// This is the default for every deadzone parameter in this class, so
		// callers get it without asking. It is a magnitude threshold, applied
		// to |value|, so it applies in all four directions.
		static constexpr float DefaultDeadzone = 0.15f;

		// Above this the stick is treated as fully deflected, which is how a
		// stick with a worn gate that never quite reaches 1.0 still lets the
		// player reach top speed.
		//
		// Consulted by ApplyDeadzone, which clamps the rescaled result to 1.0.
		// Note the constant is defined but the implementation saturates on the
		// literal 1.0f rather than reading this -- so changing
		// FullDeflectionPoint alone would not change behaviour.
		static constexpr float FullDeflectionPoint = 0.95f;

		// ---- Connection ----

		// True if a gamepad is present in the given slot. Does not require
		// the device to expose a mapping; an unmapped pad still reports
		// axes and buttons on GLFW.
		//
		// Cheap enough to poll every frame, and it is the guard every other
		// query below performs internally.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1. Out-of-range returns
		//                 false.
		static bool IsConnected(int gamepad);

		// The pad's name, or an empty string if the slot is unconnected or the
		// device reports no name.
		//
		// Calls IsConnected internally, so an out-of-range slot is safe.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1.
		static std::string GetName(int gamepad);

		// ---- Live state ----

		// Fills `outState` for the given slot and returns true if the pad is
		// connected. False means the caller should treat every field as
		// zero -- a disconnected pad must read as "nothing pressed", never as
		// stale data.
		//
		// The struct is reset to defaults before the read, so a false return
		// leaves it all-zero rather than untouched. That guarantee is what
		// lets a caller use the result without clearing it first.
		//
		// Note the return is false both for "not connected" and for "connected
		// but the state read failed" -- the two are not distinguished.
		//
		// @param gamepad    Slot index, 0 .. MaxGamepads-1.
		// @param outState   Filled in place. Must not be null; it is a
		//                   reference, so it cannot be.
		// @returns Whether a live snapshot was taken.
		static bool GetState(int gamepad, GamepadState &outState);

		// Snapshot of every connected pad. Returns the number written.
		//
		// Scans all MaxGamepads slots and appends only the connected ones, so
		// the output vector is compacted: it holds no placeholder for an empty
		// slot. That means the index of a pad in `outStates` is NOT its slot
		// number -- use State.Connected and Name, or IsButtonPressed with an
		// explicit slot, rather than assuming slot == index.
		//
		// Allocates as it pushes. For a per-frame poll, consider reusing a
		// vector; this clears rather than reallocates the caller's capacity
		// only if the vector is passed by reference and already large enough.
		//
		// @param outStates  Cleared, then filled with one entry per connected
		//                   pad.
		// @returns The number of entries written, which is also
		//          outStates.size().
		static int GetConnectedGamepads(std::vector<GamepadState> &outStates);

		// ---- Queries ----

		// Whether the given button is held on the given pad.
		//
		// Takes a full snapshot internally and returns false for a
		// disconnected pad, so it is safe but not cheap -- it performs a driver
		// read per call. Level-triggered: true for as long as the button is
		// down.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1.
		// @param button   A WL_GAMEPAD_BUTTON_* value.
		static bool IsButtonPressed(int gamepad, int button);

		// Raw axis with no deadzone applied. Use this only when the caller
		// wants the literal device value.
		//
		// Prefer GetAxisDeadzone for movement: an un-deadzoned stick includes
		// its resting drift. Returns 0.0f for a disconnected pad, which is
		// indistinguishable from a centred stick -- check IsConnected if that
		// distinction matters.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1.
		// @param axis     A WL_GAMEPAD_AXIS_* value.
		static float GetAxisRaw(int gamepad, int axis);

		// Trigger axes, remapped from GLFW's -1..1 resting value to 0..1, so
		// "how far is the trigger pulled" is a plain 0..1 with no caller-side
		// arithmetic to get wrong.
		//
		// Returns 0.0 for a disconnected pad, which is correct for a trigger
		// and is why the disconnect check exists before the remap: a
		// disconnected pad's raw 0.0 would otherwise map to 0.5, a
		// half-pulled trigger.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1.
		// @param axis     A trigger axis (left or right).
		// @returns Trigger pull in 0..1, or 0.0 for a disconnected pad or an
		//          out-of-range axis.
		static float GetTrigger(int gamepad, int axis);

		// Stick axes with the deadzone applied and the remaining range
		// rescaled, so the first responsive pixel of input maps to a
		// non-zero output instead of a dead 0.
		//
		// Without that rescale, pushing a stick just past the deadzone yields
		// a value near zero, and the player has to shove it a long way before
		// the character responds at all.
		//
		// Trigger axes are routed to GetTrigger instead of the stick curve:
		// a resting trigger reads -1, whose magnitude is 1, so the stick curve
		// would report it as fully deflected.
		//
		// @param gamepad   Slot index, 0 .. MaxGamepads-1.
		// @param axis      A WL_GAMEPAD_AXIS_* value.
		// @param deadzone  Magnitude below which input reads as zero. Clamped
		//                  to [0, 1) internally.
		// @returns The shaped axis value, or 0.0 for a disconnected pad.
		static float GetAxisDeadzone(int gamepad, int axis, float deadzone = DefaultDeadzone);

		// Convenience: a direction vector from the left stick, already
		// deadzoned and rescaled, with magnitude <= 1.0. This is the shape a
		// movement system wants, so it lives here rather than being
		// reassembled in every gamepad-aware script.
		//
		// Y is returned un-inverted: GLFW reports up as positive, and so does
		// this. The caller maps +Y onto its own forward vector.
		//
		// @param gamepad   Slot index, 0 .. MaxGamepads-1.
		// @param outX      Receives the X component. Must not be null.
		// @param outY      Receives the Y component, positive for up. Must not
		//                  be null.
		// @param deadzone  Passed through to GetAxisDeadzone.
		static void GetLeftStick(int gamepad, float &outX, float &outY,
								 float deadzone = DefaultDeadzone);

		// ---- Rumble ----

		// Currently a no-op: the vendored GLFW in this tree exposes no
		// haptic API, so there is nothing to call. Declared anyway because
		// the signature is the contract gameplay scripts will code against,
		// and it makes the missing capability explicit instead of leaving
		// callers to discover it at the point they need it.
		//
		// Calling it is safe and does nothing.
		//
		// @param gamepad        Slot index, currently ignored.
		// @param lowFrequency   Low-frequency motor strength, intended 0..1.
		//                       Currently ignored.
		// @param highFrequency  High-frequency motor strength, intended 0..1.
		//                       Currently ignored.
		static void SetRumble(int gamepad, float lowFrequency, float highFrequency);

		// Stops rumble on the given pad. Implemented as SetRumble with both
		// strengths zero, so it inherits that function's no-op status.
		//
		// @param gamepad  Slot index, 0 .. MaxGamepads-1. Currently ignored.
		static void StopRumble(int gamepad);

		// ---- Curve, exposed for testing ----

		// The stick deadzone curve as a free function. Public rather than
		// private so a headless test can assert against the exact shape the
		// live path uses. Were it private, the test would have to reimplement
		// the curve to drive it, which would prove the reimplementation
		// rather than the engine.
		//
		// Takes a raw value and returns it with |value| below `deadzone`
		// snapped to zero and the surviving range rescaled onto 0..1, sign
		// preserved. Independent of any device, so it is testable without
		// hardware.
		//
		// @param value     A raw stick reading in [-1, 1].
		// @param deadzone  Magnitude threshold. Clamped to [0, 0.999] so the
		//                  rescale cannot divide by zero.
		// @returns The shaped value in [-1, 1], or 0.0f when |value| is within
		//          the deadzone.
		static float ApplyDeadzone(float value, float deadzone = DefaultDeadzone);
	};

} // namespace Wasteland