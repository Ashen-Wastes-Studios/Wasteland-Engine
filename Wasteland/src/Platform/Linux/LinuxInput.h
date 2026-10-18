#pragma once

// The engine-level facade this class plugs into. Input.h declares the five
// *Impl hooks below as pure virtual and owns the static s_Instance pointer
// that the public static accessors dispatch through.
//
// Deliberately the only include: the header exposes no GLFW, X11 or
// platform types, so a translation unit can name LinuxInput without
// pulling in platform headers. The GLFW types stay confined to the .cpp.
#include "Wasteland/Core/Input.h"

// Engine namespace, matching the one Input lives in -- the base class is
// unreachable unqualified from inside this namespace without it.
namespace Wasteland
{

    // Linux/GLFW implementation of the input backend.
    //
    // One instance per process, created in LinuxInput.cpp as a function-local
    // static initializer (`Input *Input::s_Instance = new LinuxInput();`) that
    // runs at static-init time -- so it exists before main, well before any
    // GLFW window does. The methods therefore all have to tolerate the window
    // not existing yet; the .cpp guards each one with Application::IsInitialized().
    //
    // The whole public surface is inherited. Everything declared here is
    // protected, so callers cannot bypass Input's static facade and ask the
    // backend directly -- the guard rails are enforced by access, not by
    // convention.
    class LinuxInput : public Input
    {
    // All five overrides are protected because Input.h declares them protected
    // too: the static facades reach them through s_Instance, which is inside
    // the class, while outside code is meant to use Input::IsKeyPressed and
    // friends. Pure virtual in the base, so this class is concrete and all
    // five must appear -- one missing would leave LinuxInput abstract and
    // `new LinuxInput()` at line 12 of the .cpp would not compile.
    //
    // DEFECT: `override` is written on a `virtual` that adds nothing beyond it.
    // Harmless, but the style differs from Input.h, which declares these
    // pure virtual with no `virtual` keyword at all.
    protected:
        virtual bool IsKeyPressedImpl(int keycode) override;

        // Level-triggered: true for as long as the button is held, so a caller
        // polling each frame sees it true repeatedly. KeyEvent.h carries the
        // edge-triggered counterpart. The .cpp maps GLFW_PRESS to true and
        // ignores GLFW_RELEASE, so no release edge is reported here.
        //
        // Unlike IsKeyPressedImpl, this one guards on Application::IsInitialized()
        // and then fetches the native window itself rather than reading it once.
        virtual bool IsMouseButtonPressedImpl(int button) override;
        // Cursor position in window coordinates, returned by value so callers can
        // use structured bindings. This is also the sampling point for
        // Input::GetMouseDelta, which diffs consecutive calls to this method.
        //
        // The .cpp casts the native window to GLFWwindow* itself; no GLFW type is
        // needed here, which is what lets this header stay platform-agnostic.
        virtual std::pair<float, float> GetMousePositionImpl() override;
        // Convenience axis accessor. The base class keeps this separate from
        // GetMousePositionImpl rather than deriving it, so a backend can answer
        // X without constructing a pair; the Linux .cpp just destructures the
        // pair and discards Y.
        virtual float GetMouseXImpl() override;
        // Same role as GetMouseXImpl for the vertical axis.
        virtual float GetMouseYImpl() override;
    };

}
