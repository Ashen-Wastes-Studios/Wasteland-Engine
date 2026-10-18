// Precompiled header first: supplies the STL and project-wide defines.
#include "wlpch.h"
#include "MacInput.h"

// Platform gate at the top of the FILE, before any code -- unlike
// WindowsInput.cpp where the #if sits inside the namespace. Here the
// whole translation unit, include block included, is conditional.
// The alternative (empty object file) is what lets the same
// Input::s_Instance symbol be claimed by exactly one backend.
#if defined(WL_PLATFORM_MACOS)

// Inside the #if, so it is only pulled in on a Mac build.
// Supplies Application::IsInitialized()/Get() to reach the window.
#include "Wasteland/Core/Application.h"
// Same GLFW API as the other backends. GLFW abstracts macOS windowing,
// so the bodies below are character-for-character comparable with the
// Windows ones.
#include <GLFW/glfw3.h>

namespace Wasteland
{

    // Same singleton-claim pattern as WindowsInput.cpp line 11.
    // DEFECT: identical -- raw new, never deleted, and a second backend
    // linking in would leak this one.
    Input *Input::s_Instance = new MacInput();

    // Body is identical to WindowsInput::IsKeyPressedImpl.
    bool MacInput::IsKeyPressedImpl(int keycode)
    {
        // Guard 1 of 2, matching WindowsInput.cpp line 16 exactly.
        if (!Application::IsInitialized())
            return false;

        // Guard 2 of 2: the Application exists but the window does not.
        if (!Application::Get().GetWindow().GetNativeWindow())
            return false;

        // static_cast because GLFW is not a polymorphic/RTTI type here.
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        // Same caveat as the Windows version: no focus check, so keys still
        // report pressed while the window is in the background.
        auto state = glfwGetKey(window, keycode);
        // Level-triggered: true for as long as the key is held.
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    // Same as the Windows mouse-button hook.
    bool MacInput::IsMouseButtonPressedImpl(int button)
    {
        if (!Application::IsInitialized())
            return false;

        // DEFECT (minor, same as WindowsInput.cpp line 32): the null check on
        // line 33 happens after this cast, while lines 19-20 check the handle
        // first. Two different orderings of the same guard in one file.
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        if (!window)
            return false;

        // No repeat state for mouse buttons, so only GLFW_PRESS counts.
        auto state = glfwGetMouseButton(window, button);
        return state == GLFW_PRESS;
    }

    // Window-coordinate cursor position, returned by value for structured
    // bindings at the call sites.
    std::pair<float, float> MacInput::GetMousePositionImpl()
    {
        // Guard: Application::Get() is null and the native window is null before the
        // app boots (and after it shuts down). Dereferencing either segfaults.
        // The comment above this line is inherited verbatim from
        // WindowsInput.cpp (same wording, same claim about both pointers being
        // null) -- strong evidence these two files were maintained as a pair,
        // and also that any fix here needs making in both.
        if (!Application::IsInitialized())
            // Cannot be distinguished from a real cursor at the origin.
            return {0.0f, 0.0f};

        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        if (!window)
            return {0.0f, 0.0f};

        // GLFW uses double for cursor coordinates; declared uninitialised here.
        double xpos, ypos;
        // DEFECT: return value ignored. If GLFW reports failure, xpos/ypos keep
        // their uninitialised values and line 54 returns garbage. Identical to
        // WindowsInput.cpp line 52.
        glfwGetCursorPos(window, &xpos, &ypos);

        // Narrowing double -> float; acceptable for pixel coordinates.
        return {(float)xpos, (float)ypos};
    }

    // Same hook shape as the Windows backend.
    float MacInput::GetMouseXImpl()
    {
        // DEFECT (minor): y is bound and unused in this function, same as
        // WindowsInput.cpp line 59.
        auto [x, y] = GetMousePositionImpl();
        // Goes through the full pair rather than querying X alone, so this
        // hook saves nothing over GetMousePosition.
        return x;
    }

    // Mirror of GetMouseXImpl.
    float MacInput::GetMouseYImpl()
    {
        // x bound and unused, same as line 59.
        auto [x, y] = GetMousePositionImpl();
        // X sample discarded.
        return y;
    }

}

// Closes namespace Wasteland above and then the platform gate. This
// is the one place in the file where a guard is named at its close;
// the #if on line 4 carries no matching comment.
// Formatting note: bodies here use 4 spaces while WindowsInput.cpp uses
// tabs, so the two sibling backends are not even indented consistently
// despite being near-identical copies.
#endif // WL_PLATFORM_MACOS
