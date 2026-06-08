#include "wlpch.h"
#include "MacInput.h"

#if defined(WL_PLATFORM_MACOS)

#include "Wasteland/Core/Application.h"
#include <GLFW/glfw3.h>

namespace Wasteland
{

    Input *Input::s_Instance = new MacInput();

    bool MacInput::IsKeyPressedImpl(int keycode)
    {
        if (!Application::IsInitialized())
            return false;

        if (!Application::Get().GetWindow().GetNativeWindow())
            return false;

        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        auto state = glfwGetKey(window, keycode);
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    bool MacInput::IsMouseButtonPressedImpl(int button)
    {
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        auto state = glfwGetMouseButton(window, button);
        return state == GLFW_PRESS;
    }

    std::pair<float, float> MacInput::GetMousePositionImpl()
    {
        auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        return {(float)xpos, (float)ypos};
    }

    float MacInput::GetMouseXImpl()
    {
        auto [x, y] = GetMousePositionImpl();
        return x;
    }

    float MacInput::GetMouseYImpl()
    {
        auto [x, y] = GetMousePositionImpl();
        return y;
    }

}

#endif // WL_PLATFORM_MACOS
