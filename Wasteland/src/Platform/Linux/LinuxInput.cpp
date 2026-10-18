// Precompiled header first, by engine convention.
#include "wlpch.h"
// This class's declaration, including the Input base whose s_Instance is set
// two lines below the namespace opens.
#include "LinuxInput.h"

// Platform guard, matching the one in LinuxWindow.cpp. The whole translation
// unit becomes empty off Linux -- which also means LinuxInput.cpp is the file
// that defines Input::s_Instance, so on any other platform that static has no
// definition here and must come from the platform's own input source.
#if defined(WL_PLATFORM_LINUX)

#include "Wasteland/Core/Application.h"
#include <GLFW/glfw3.h>

// Brace on its own line, matching LinuxWindow.cpp rather than Renderer/
// Shader.cpp which puts it on the declaration line.
namespace Wasteland
{

    // Construction of the process-wide input singleton, at STATIC INITIALISATION
    // time -- before main() runs.

    // Three things worth knowing about this line:

    // 1. `new` with no matching delete, and stored as a raw pointer. The Input
    //    singleton lives for the whole process and is never freed, which is a
    //    deliberate trade (guaranteed-available input) but does mean the vtable
    //    and the object are a static-lifetime leak by construction.

    // 2. This initialiser runs Application-independent. That is what makes the
    //    IsInitialized() guards in the three Impl methods below necessary: input
    //    can be queried before any window exists.

    // 3. DEFECT: this is static init, and the object it creates has no state
    //    beyond the vtable, so nothing here is actually unsafe today. But if
    //    LinuxInput ever grows a member initialised from another translation
    //    unit's static, the order of initialisation across TUs is unspecified
    //    and this would become a real static-initialisation-order fiasco.
    Input *Input::s_Instance = new LinuxInput();

    // The Impl half of the Input interface. The public IsKeyPressed() on the base
    // class is what callers use; this is the platform half of that template.

    // Every one of the three methods below guards on Application::IsInitialized()
    // and/or a null native window. That is not defensive padding: this singleton
    // is constructed before main(), so a query during static init or after
    // shutdown would otherwise dereference a null Application.
    bool LinuxInput::IsKeyPressedImpl(int keycode)
    {
        // First guard: is there an Application at all? Callers include UI code that
        // can run before the app is up and editor panels that can run after it is
        // down.

        // Returns false rather than throwing or asserting: "no key is down" is the
        // safe answer, and a query during teardown is not a programming error worth
        // breaking the debugger for.
        if (!Application::IsInitialized())
            return false;

        // Second guard: the window exists but has no native handle. Checked
        // SEPARATELY from IsInitialized() because the two fail independently -- the
        // Application can be constructed with a window that failed to create.

        // Calling GetWindow().GetNativeWindow() requires the Application to be alive,
        // hence the ordering: this line is only reached because the guard above
        // returned.
        if (!Application::Get().GetWindow().GetNativeWindow())
            return false;

        // The void* -> GLFWwindow* cast.

        // Two Application::Get() calls in a row here and one on each guard above: four
        // separate lookups of the singleton, plus the window, plus the native handle,
        // for one key query. Application::Get() is presumably a cheap static read, so
        // this is noise rather than a cost -- but it is repeated per query, and
        // IsKeyPressed is called once per key per frame by gameplay code.

        // The cast is unchecked: nothing verifies the native handle really is a
        // GLFWwindow*, so a window built by another backend would be handed to
        // glfwGetKey as-is.
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        // Queries GLFW directly rather than tracking state from KeyPressedEvent.
        // The event-driven alternative would miss keys pressed before the input
        // layer attached; polling asks the window manager directly.

        // `keycode` is passed through unmodified, so the caller is using engine key
        // codes that GLFW happens to share. GLFW's key constants are its own enum,
        // so this works only because Core/KeyCodes.h mirrors GLFW's values.
        auto state = glfwGetKey(window, keycode);
        // PRESS or REPEAT both count as "held".

        // Deliberate: this answers "is the key down", not "did the key go down this
        // frame". A press-and-hold reads true continuously, which is what gameplay
        // movement code wants. Code needing edge detection must use the
        // KeyPressedEvent path, because a polling caller cannot distinguish the first
        // frame of a hold from the hundredth.
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    // Mouse buttons. Same null-guard structure as the key method above.
    bool LinuxInput::IsMouseButtonPressedImpl(int button)
    {
        // Same "is there an Application" guard.

        // Same rationale for returning false rather than asserting: during static
        // init or teardown no button is down.
        if (!Application::IsInitialized())
            return false;

        // DEFECT (minor, inconsistency): the native handle is fetched here BEFORE
        // the null check, and checked on the next line. Compare IsKeyPressedImpl
        // above, which checks GetNativeWindow() for null first and only then casts.
        // Both orders are safe -- the cast of a null void* is still null -- but the
        // two halves of one class spell the same guard differently.
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        // The null-handle check, on the local rather than through the accessor chain
        // as the key method does. Same effect, different shape.
        if (!window)
            return false;

        // Poll rather than track events, for the reason given in the key method.

        // `button` is an engine mouse button code passed straight into GLFW; as with
        // keys, this only works because Core/MouseButtonCodes.h mirrors GLFW's
        // values (MOUSE_BUTTON_0 = 0, etc.).
        auto state = glfwGetMouseButton(window, button);
        // Only PRESS counts.

        // DEFECT (behavioural, small): unlike the key version this does NOT accept
        // GLFW_REPEAT. GLFW does not generate REPEAT for mouse buttons, so the
        // asymmetry is currently harmless -- but it reads as an oversight, and it
        // diverges from IsKeyPressedImpl on the line above for no stated reason.
        return state == GLFW_PRESS;
    }

    // Returns client-area cursor coordinates, in float.

    // float rather than int matches MouseMovedEvent's float fields, so a poll
    // and the event stream agree on the type and a consumer does not convert.
    std::pair<float, float> LinuxInput::GetMousePositionImpl()
    {
        // Guard: Application::Get() is null and the native window is null before the
        // app boots (and after it shuts down). Dereferencing either segfaults.
        // Same guard as the two methods above. The comment block on lines 42-43
        // already states the reasoning -- both nulls, either dereference segfaults --
        // so this is not repeated here.
        if (!Application::IsInitialized())
            // The origin as the "no cursor" answer.

            // This is a fabricated position rather than a sentinel: (0,0) is a real
            // client-area coordinate (the top-left corner), so a caller cannot tell
            // "no window" from "cursor is in the corner". Using an optional, or a bool
            // out-param, would make the difference observable. As written, UI code that
            // polls before startup will place the cursor at the top-left.
            return {0.0f, 0.0f};

        // Same unchecked cast as the two methods above, and the same repeated
        // Application::Get().GetWindow().GetNativeWindow() chain.
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        // Null-handle check on the local.
        if (!window)
            return {0.0f, 0.0f};

        // GLFW reports cursor position as double.

        // Uninitialised locals: safe only because glfwGetCursorPos on the next line
        // unconditionally writes both. If GLFW ever failed without writing them
        // (a documented error case for an invalid window), these would be read
        // uninitialised -- and the window is not re-checked between line 48 and here.
        double xpos, ypos;
        // Returns GLFW_FALSE on failure, which is ignored here. Combined with the
        // uninitialised locals above, a failure path would read indeterminate values.
        // Initialising to 0.0f would cost nothing and remove the window.
        glfwGetCursorPos(window, &xpos, &ypos);

        // Narrowing double -> float. Client-area coordinates are well within float
        // precision for any real window size, so this loses nothing in practice.

        // The brace-init pair matches the std::pair<float,float> return type.
        return {(float)xpos, (float)ypos};
    }

    // Convenience accessor: one component of the pair above.
    float LinuxInput::GetMouseXImpl()
    {
        // Structured binding on a returned pair.

        // Calls the full position query -- and therefore repeats both null guards
        // and the Application::Get() chain -- to obtain one float. Callers wanting
        // both axes should call GetMousePositionImpl once instead of calling
        // GetMouseXImpl and GetMouseYImpl separately, which do the whole work twice.
        auto [x, y] = GetMousePositionImpl();
        // The X component. `y` is unused here; structured bindings do not warn on
        // that by default, so the unused element is silent.
        return x;
    }

    // Mirror of GetMouseXImpl for the Y component.

    // Same duplicated-work caveat as GetMouseXImpl above: querying both
    // components separately costs two full position queries per frame.
    float LinuxInput::GetMouseYImpl()
    {
        auto [x, y] = GetMousePositionImpl();
        // The Y component, discarding X the same way GetMouseXImpl discards Y.
        return y;
    }

}

// Closes the platform guard. The trailing comment names the macro, which is
// what a reader jumping to the end of the file needs.
#endif // WL_PLATFORM_LINUX
