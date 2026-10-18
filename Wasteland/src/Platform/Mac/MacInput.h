// Header guard. One of the per-platform Input headers, and the smallest
        // file in this batch: five declarations and nothing else.
#pragma once

// The backend-agnostic base. Note it is NOT <...> angle-bracket form even
        // though this sits under src/Platform -- this header is included from
        // the engine's own source tree, where the quoted include path
        // resolves relative to it.
#include "Wasteland/Core/Input.h"

namespace Wasteland
{

    // A platform input backend and nothing else: five queries. No state, no
    // event translation, no key mapping table -- all of that lives in the
    // base or in the .cpp.
    // The name is Mac rather than MacOS or Darwin; this class carries no
    // platform #ifdefs at all, so it compiles anywhere. The platform
    // dependency is entirely in the implementation file.
    class MacInput : public Input
    {
    // PROTECTED, not private or public. That is the template-method pattern:
    // the base declares public IsKeyPressed/IsMouseButtonPressed wrappers
    // that callers use, and those call the *Impl methods below. Naming
    // the convention is what makes the pairing recognisable.
    //
    // This also means a MacInput instance can be subclassed to intercept a
    // query, since the methods are reachable from a derived class.
    protected:
        // `virtual ... override` together: override already implies virtual, so
        // the keyword is redundant but matches the style of the surrounding
        // codebase.
        // The keycode is an int rather than a Key enum, which keeps the base
        // header from having to include an enum header and lets a platform
        // backend pass a native scancode through unchanged.
        // DEFECT (no enum documentation, documented not fixed): nothing here
        // says what the int means -- a GLFW keycode, a macOS virtual keycode,
        // or the engine's own enum. Each per-platform header has to match the
        // base's convention, and a mismatch is a silent wrong-key bug.
        virtual bool IsKeyPressedImpl(int keycode) override;

        // Same shape as the key query: an int button code passed straight through.
        // Pressed, not down -- so this is an edge query (this frame the button
        // went down), distinct from an IsMouseButtonDown held-state query the
        // base may also offer. The distinction matters for gameplay input.
        virtual bool IsMouseButtonPressedImpl(int button) override;
        // Returned by value in a std::pair rather than through out-params: a pair of
        // floats is small enough that the copy is free, and it cannot be
        // misused by forgetting one of two out-params.
        // FLOAT, not int: this reports a continuous position, so the backend
        // must convert from whatever the windowing system returns (typically
        // device pixels) to float.
        virtual std::pair<float, float> GetMousePositionImpl() override;
        // The single-axis queries exist separately from GetMousePositionImpl
        // above, so a caller that needs only X does not pay for both halves.
        // DEFECT (possible inconsistency, documented not fixed): nothing
        // forces these to agree with the pair-returning version. If the
        // implementations are not written from one source of truth, a
        // cursor-moved this frame could report X and Y from different moments.
        virtual float GetMouseXImpl() override;
        // See the note above. Note also that there is no visible Y flip here, which
        // is the correct place for it to be absent -- the base class decides
        // the coordinate convention and each backend must match it.
        virtual float GetMouseYImpl() override;
    };

}
