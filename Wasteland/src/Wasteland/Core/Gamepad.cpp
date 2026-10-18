#include "wlpch.h"
#include "Wasteland/Core/Gamepad.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

namespace Wasteland {

	// Whether a pad is present in the given slot.
	//
	// @param gamepad  Slot index. Out-of-range returns false rather than
	//                 reaching GLFW.
	// @returns Whether GLFW reports a joystick in that slot.
	bool Gamepad::IsConnected(int gamepad)
	{
		// Slot bounds check first. glfwJoystickPresent on an out-of-range index
		// is an error return from GLFW, not a crash, but returning false here
		// keeps every caller safe without them each checking.
		//
		// Upper bound is MaxGamepads (4), NOT GLFW's own 16-slot limit: this
		// engine deliberately polls only the first four.
		if (gamepad < 0 || gamepad >= MaxGamepads)
			return false;

		// GLFW_TRUE is 1; comparing against it explicitly rather than testing
		// the raw value documents that a zero return means "absent".
		return glfwJoystickPresent(gamepad) == GLFW_TRUE;
	}

	// The pad's name, or empty when the slot is unconnected or unnamed.
	//
	// @param gamepad  Slot index, 0 .. MaxGamepads-1.
	// @returns The device name, or an empty string.
	std::string Gamepad::GetName(int gamepad)
	{
		// Reuses IsConnected, so an out-of-range slot is handled by the same
		// bounds check rather than being duplicated here.
		if (!IsConnected(gamepad))
			return std::string();

		// glfwGetGamepadName returns null for a device with no name string, and
		// also null when the pad lacks a mapping. The null test covers both.
		const char *name = glfwGetGamepadName(gamepad);
		return name ? std::string(name) : std::string();
	}

	// Snapshots one pad's full state.
	//
	// @param gamepad   Slot index, 0 .. MaxGamepads-1.
	// @param outState  Reset then filled. Never left partially written.
	// @returns Whether a live snapshot was taken.
	bool Gamepad::GetState(int gamepad, GamepadState &outState)
	{
		// Reset before every read. A pad that disconnected since the last
		// poll must read as all-zero, not retain the previous frame's
		// pressed buttons -- otherwise a player who yanks the controller out
		// mid-fire keeps firing forever.
		//
		// Value-assignment of a default-constructed temporary, which relies on
		// GamepadState's default member initializers to produce the all-zero
		// state (Connected false, empty Name, arrays cleared).
		outState = GamepadState();

		if (!IsConnected(gamepad))
			return false;

		// The presence check above is necessary but not sufficient: the pad can
		// disappear between the two calls, so the state read below is the
		// authoritative one.
		outState.Connected = true;
		outState.Name = GetName(gamepad);

		// GLFW's own snapshot struct. Allocated on the stack; small and fixed
		// size, so no allocation is involved.
		GLFWgamepadstate state;

		// Returns GLFW_CONNECTED, or GLFW_DISCONNECTED / GLFW_UNKNOWN if the
		// pad went away or has no mapping. Both non-connected cases return false
		// here, leaving outState as the all-zero reset -- note that Connected
		// was already set true above and is NOT rolled back, so a caller that
		// ignores the return value could see Connected true with all-zero
		// buttons.
		if (glfwGetGamepadState(gamepad, &state) != GLFW_CONNECTED)
			return false;

		// Copy the buttons, bounded by BOTH the last valid index and the array
		// size. The `i < 16` half is a compile-time-known belt-and-braces check
		// against the Buttons[16] declaration: if GLFW ever adds a button past
		// 15, the loop stops at the array bound instead of writing out of range.
		for (int i = 0; i <= WL_GAMEPAD_BUTTON_LAST && i < 16; ++i)
			// GLFW_PRESS is 1; compare rather than coerce so a pad reporting an
			// unexpected value reads as released rather than pressed.
			outState.Buttons[i] = (state.buttons[i] == GLFW_PRESS);

		// Same double-bounded pattern for the axes, against Axes[6].
		for (int i = 0; i <= WL_GAMEPAD_AXIS_LAST && i < 6; ++i)
			// C-style cast from GLFW's float; spdlog/fmt are not involved here.
			outState.Axes[i] = (float)state.axes[i];

		return true;
	}

	// Snapshots every connected pad.
	//
	// @param outStates  Cleared, then appended to -- one entry per connected
	//                   pad, so indices are compacted and do NOT correspond to
	//                   slot numbers.
	// @returns The number of entries written.
	int Gamepad::GetConnectedGamepads(std::vector<GamepadState> &outStates)
	{
		// Clear first so a caller reusing the vector across frames does not
		// accumulate stale pads that have since been unplugged. clear() keeps
		// the allocated capacity, so this does not free.
		outStates.clear();

		// Every slot in the engine's supported range, not GLFW's 16.
		for (int i = 0; i < MaxGamepads; ++i)
		{
			// Declared inside the loop so each iteration gets a fresh,
			// zero-initialized snapshot; GetState would reset it anyway, but a
			// per-iteration object makes that independence obvious.
			GamepadState state;

			// Only connected pads are pushed, which is what compacts the output.
			if (GetState(i, state))
				outStates.push_back(state);
		}

		// The size is already the count; returning it saves a second traversal
		// and lets the caller use the result without reading the vector.
		return (int)outStates.size();
	}

	// Whether a button is held on a pad.
	//
	// Takes a full snapshot per call, so this is a driver round-trip -- see
	// the header note on preferring one GetState per frame.
	//
	// @param gamepad  Slot index, 0 .. MaxGamepads-1.
	// @param button   A WL_GAMEPAD_BUTTON_* value.
	// @returns false for a disconnected pad or an out-of-range button.
	bool Gamepad::IsButtonPressed(int gamepad, int button)
	{
		GamepadState state;
		if (!GetState(gamepad, state))
			return false;
		// Bounds-checked in GamepadState, so a bad button index is handled there.
		return state.IsButtonPressed(button);
	}

	// The unmodified axis reading.
	//
	// @param gamepad  Slot index, 0 .. MaxGamepads-1.
	// @param axis     A WL_GAMEPAD_AXIS_* value.
	// @returns The device value in [-1, 1], or 0.0f if the pad is absent.
	float Gamepad::GetAxisRaw(int gamepad, int axis)
	{
		GamepadState state;
		if (!GetState(gamepad, state))
			return 0.0f;
		return state.GetAxis(axis);
	}

	// A trigger's pull, converted from GLFW's -1..1 to 0..1.
	//
	// @param gamepad  Slot index, 0 .. MaxGamepads-1.
	// @param axis     A trigger axis.
	// @returns Pull in 0..1; 0.0 for a disconnected pad or bad axis.
	float Gamepad::GetTrigger(int gamepad, int axis)
	{
		// A disconnected or missing pad must read as FULLY RELEASED, which is
		// 0.0 on the trigger's 0..1 scale.
		//
		// This check is load-bearing, not defensive. The remap below turns a
		// raw axis of -1 (a released trigger on real hardware) into 0.0 --
		// but a pad that is NOT CONNECTED returns a raw 0.0, which the same
		// arithmetic turns into 0.5, i.e. a half-pulled trigger. Without this
		// guard a player with the controller unplugged gets a weapon that
		// fires constantly.
		if (!IsConnected(gamepad))
			return 0.0f;

		// Bounds check before the arithmetic. Rejects a stick axis passed here
		// by mistake, which would otherwise be silently remapped as though it
		// were a trigger.
		if (axis < 0 || axis > WL_GAMEPAD_AXIS_LAST)
			return 0.0f;

		float value = GetAxisRaw(gamepad, axis);

		// Triggers rest at -1 and travel to +1, unlike sticks which rest at
		// 0. Remap to 0..1 so "how hard is this pulled" needs no
		// caller-side arithmetic.
		//
		// The vendored GLFW 3.4 header confirms the range: "The states of each
		// gamepad axis, in the range -1.0 to 1.0 inclusive."
		//
		// A trigger that rests slightly above -1 on real hardware reports a
		// small non-zero value; without clamping to 0, "released" would read
		// as a light pull and the player would creep.
		float remapped = (value + 1.0f) * 0.5f;
		// Clamped to 0..1 to absorb hardware overshoot at both ends.
		return std::max(0.0f, std::min(1.0f, remapped));
	}

	// Applies the stick deadzone curve to one raw value.
	//
	// Free-standing and device-independent so a test can assert the exact shape
	// the live path uses.
	//
	// @param value     A raw stick reading in [-1, 1].
	// @param deadzone  Magnitude threshold; clamped to [0, 0.999].
	// @returns The shaped value in [-1, 1], or 0.0f inside the deadzone.
	float Gamepad::ApplyDeadzone(float value, float deadzone)
	{
		// Guard the degenerate zone. A deadzone at or above 1 would make the
		// rescale divide by zero, and a negative one is nonsense.
		if (deadzone < 0.0f)
			deadzone = 0.0f;
		if (deadzone >= 1.0f)
			deadzone = 0.999f;

		// Magnitude, so the threshold applies symmetrically in all four
		// directions.
		float magnitude = std::abs(value);

		// Inside the deadzone: report exactly zero rather than a small residual,
		// which is what stops a resting stick from drifting the player.
		if (magnitude <= deadzone)
			return 0.0f;

		// Rescale the surviving range back onto 0..1. Without this, input
		// just past the deadzone produces a value near zero, and the player
		// has to push the stick most of the way before anything happens -- the
		// "sticky aim" feel that makes cheap pads unusable.
		//
		// (1 - deadzone) cannot be zero because of the clamp above, so this
		// division is always safe.
		float rescaled = (magnitude - deadzone) / (1.0f - deadzone);
		// Saturate: the clamp above permits deadzone 0.999, where the division
		// can still slightly exceed 1 for a value at the very top.
		rescaled = std::min(1.0f, rescaled);

		// Preserve direction, including the inverted Y that sticks need to
		// feel natural (pushing up must move forward, not back).
		//
		// Sign taken from the ORIGINAL value, not the magnitude, so a stick at
		// -1 still returns -1 and not +1.
		return (value < 0.0f) ? -rescaled : rescaled;
	}

	// A stick axis with the deadzone applied, or a trigger remapped to 0..1.
	//
	// @param gamepad   Slot index, 0 .. MaxGamepads-1.
	// @param axis      A WL_GAMEPAD_AXIS_* value.
	// @param deadzone  Passed to ApplyDeadzone; ignored for trigger axes.
	// @returns The shaped value, or 0.0 for a disconnected pad.
	float Gamepad::GetAxisDeadzone(int gamepad, int axis, float deadzone)
	{
		// Triggers get their own remapping and are never deadzoned by the
		// stick path: a trigger at rest reads -1, whose magnitude is 1, so
		// the stick curve would map it to full deflection.
		//
		// The `deadzone` argument is deliberately ignored on this branch --
		// a trigger has no neutral point to be dead around.
		if (axis == WL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == WL_GAMEPAD_AXIS_RIGHT_TRIGGER)
			return GetTrigger(gamepad, axis);

		// Stick path: shape the raw value. No bounds check here, because
		// GamepadState::GetAxis inside GetAxisRaw already returns 0.0f for an
		// out-of-range index -- and 0 is inside any deadzone, so it maps to 0.
		return ApplyDeadzone(GetAxisRaw(gamepad, axis), deadzone);
	}

	// The left stick as a deadzoned direction vector.
	//
	// @param gamepad   Slot index, 0 .. MaxGamepads-1.
	// @param outX      Receives X. Must not be null.
	// @param outY      Receives Y, positive for up. Must not be null.
	// @param deadzone  Passed through to GetAxisDeadzone.
	void Gamepad::GetLeftStick(int gamepad, float &outX, float &outY, float deadzone)
	{
		outX = GetAxisDeadzone(gamepad, WL_GAMEPAD_AXIS_LEFT_X, deadzone);

		// Y is returned AS-IS: GLFW reports up as positive, and so does this.
		//
		// Inverting here would be wrong. An earlier version inverted to suit
		// screen space, but a gamepad stick is a world-space direction
		// control, not a 2D UI cursor: the caller maps +Y onto its own
		// forward vector, and the engine's forward is -Z. Inverting made
		// "push the stick up" walk the player backwards.
		//
		// The two components are shaped independently, so the result's
		// magnitude can exceed 1 (up to sqrt(2)) when the stick is pushed
		// diagonally -- it is NOT normalized to a unit disc. A movement system
		// wanting analog magnitude should scale by the vector's own length.
		outY = GetAxisDeadzone(gamepad, WL_GAMEPAD_AXIS_LEFT_Y, deadzone);
	}

	// Sets rumble strength. Currently a no-op -- see the body.
	//
	// @param gamepad        Slot index, currently ignored.
	// @param lowFrequency   Intended 0..1, currently ignored.
	// @param highFrequency  Intended 0..1, currently ignored.
	void Gamepad::SetRumble(int gamepad, float lowFrequency, float highFrequency)
	{
		// No-op: the vendored GLFW in this tree exposes no haptic API
		// (glfwSetGamepadHapticStrength and the glfwHaptic* family are both
		// absent), so there is nothing to call.
		//
		// Kept as a declared, silently-ignored function rather than removed,
		// because the signature is the contract a gamepad-aware script will
		// code against. When GLFW haptic support lands, this is the one place
		// that needs to change: validate the slot, then call the haptic API
		// with both strengths clamped to 0..1.
		//
		// The (void) casts mark each parameter as deliberately unused, which
		// keeps a warning-enabled build clean without commenting the names out.
		(void)gamepad;
		(void)lowFrequency;
		(void)highFrequency;
	}

	// Stops rumble on a pad.
	//
	// @param gamepad  Slot index, currently ignored.
	void Gamepad::StopRumble(int gamepad)
	{
		// Routed through SetRumble rather than duplicating the no-op, so when
		// rumble is implemented only one function needs changing.
		SetRumble(gamepad, 0.0f, 0.0f);
	}

} // namespace Wasteland