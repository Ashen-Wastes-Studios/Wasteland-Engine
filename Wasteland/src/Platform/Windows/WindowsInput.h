// Include guard by pragma once: the project's house style.
#pragma once

// Input.h, which declares the abstract facade, the five *Impl virtuals
// implemented below, and the static s_Instance the facade forwards through.
#include "Wasteland/Core/Input.h"

namespace Wasteland {

	// The Windows backend for the engine-level input facade. Everything public on
	// Input is static, so this class adds no public surface -- it exists only to
	// answer the five protected hooks.
	// Built and registered by the Windows platform layer, which owns it;
	// Input::s_Instance is a raw pointer precisely because Input neither creates nor
	// destroys it.
	class WindowsInput : public Input
	{
	// protected, matching Input: the facade's static entry points reach these
	// through s_Instance, and no external code may bypass the facade to ask the
	// backend directly.
	protected:
		// Level-triggered, matching Input::IsKeyPressed: true while the key is held,
		// so a per-frame poller sees it repeatedly. The edge-triggered counterpart is
		// KeyPressedEvent in Events/KeyEvent.h.
		// DEFECT (unguarded facade): Input::IsKeyPressed dereferences s_Instance with no
		// null check -- Input.h says so explicitly -- so a query before the platform
		// registers a backend is a null dereference, not a false. GetMouseDelta is the one
		// accessor that guards; the other four do not.
		virtual bool IsKeyPressedImpl(int keycode) override;

		// Returned BY VALUE so the facade can offer structured bindings:
		// `auto [x, y] = Input::GetMousePosition();`. Window coordinates, not NDC.
		virtual bool IsMouseButtonPressedImpl(int button) override;
		virtual std::pair<float, float> GetMousePositionImpl() override;
		// Single-axis convenience overrides, so a caller needing one axis need not
		// unpack a pair. Kept separate from GetMousePositionImpl rather than derived
		// from it so a backend can answer one axis without building a pair.
		// DEFECT (redundant virtuals): both are pure virtual in Input, so this class must
		// declare all five even though GetMousePositionImpl alone could answer both --
		// the cost of no default implementations in the base.
		virtual float GetMouseXImpl() override;
		virtual float GetMouseYImpl() override;
	};

}