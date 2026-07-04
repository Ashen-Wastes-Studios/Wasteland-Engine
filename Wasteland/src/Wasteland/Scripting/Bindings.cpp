#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>

#include <Wasteland/Scene/Entity.h>
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Scene.h>
#include "Wasteland/Core/Input.h"
#include "Wasteland/Core/KeyCodes.h"
#include "Wasteland/Core/MouseButtonCodes.h"

namespace py = pybind11;

PYBIND11_MODULE(Wasteland, m)
{
    py::class_<glm::vec2>(m, "Vec2")
        .def(py::init<float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f)
        .def_readwrite("x", &glm::vec2::x)
        .def_readwrite("y", &glm::vec2::y);

    py::class_<glm::vec3>(m, "Vec3")
        .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
        .def_readwrite("x", &glm::vec3::x)
        .def_readwrite("y", &glm::vec3::y)
        .def_readwrite("z", &glm::vec3::z);

    py::class_<glm::vec4>(m, "Vec4")
        .def(py::init<float, float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f, py::arg("w") = 0.0f)
        .def_readwrite("x", &glm::vec4::x)
        .def_readwrite("y", &glm::vec4::y)
        .def_readwrite("z", &glm::vec4::z)
        .def_readwrite("w", &glm::vec4::w);

    py::class_<Wasteland::TagComponent>(m, "TagComponent")
        .def(py::init<const std::string &>())
        .def_readwrite("Tag", &Wasteland::TagComponent::Tag);

    py::class_<Wasteland::TransformComponent>(m, "TransformComponent")
        .def(py::init<>())
        .def_property("Translation", [](Wasteland::TransformComponent &t)
                      { return t.Translation; }, [](Wasteland::TransformComponent &t, const glm::vec3 &v)
                      { t.Translation = v; })
        .def_property("Rotation", [](Wasteland::TransformComponent &t)
                      { return t.Rotation; }, [](Wasteland::TransformComponent &t, const glm::vec3 &v)
                      { t.Rotation = v; })
        .def_property("Scale", [](Wasteland::TransformComponent &t)
                      { return t.Scale; }, [](Wasteland::TransformComponent &t, const glm::vec3 &v)
                      { t.Scale = v; });

    py::class_<Wasteland::Entity>(m, "Entity")
        .def(py::init<>())
        .def("IsValid", [](Wasteland::Entity &e)
             { return e.IsValid(); })
        .def("HasTransform", [](Wasteland::Entity &e)
             { return e.HasComponent<Wasteland::TransformComponent>(); })
        .def("GetTransform", [](Wasteland::Entity &e) -> Wasteland::TransformComponent &
             { return e.GetComponent<Wasteland::TransformComponent>(); }, py::return_value_policy::reference)
        .def("HasTag", [](Wasteland::Entity &e)
             { return e.HasComponent<Wasteland::TagComponent>(); })
        .def("GetTag", [](Wasteland::Entity &e) -> Wasteland::TagComponent &
             { return e.GetComponent<Wasteland::TagComponent>(); }, py::return_value_policy::reference);

    py::class_<Wasteland::Scene, std::shared_ptr<Wasteland::Scene>>(m, "Scene")
        .def("CreateEntity", &Wasteland::Scene::CreateEntity, py::arg("name") = std::string())
        .def("DestroyEntity", &Wasteland::Scene::DestroyEntity);

    py::class_<Wasteland::Timestep>(m, "Timestep")
        .def(py::init<float>(), py::arg("time") = 0.0f)
        .def("GetSeconds", &Wasteland::Timestep::GetSeconds)
        .def("GetMilliseconds", &Wasteland::Timestep::GetMilliseconds)
        .def("__mul__", [](const Wasteland::Timestep &ts, float scalar)
             { return ts.GetSeconds() * scalar; })
        .def("__rmul__", [](const Wasteland::Timestep &ts, float scalar)
             { return ts.GetSeconds() * scalar; });

    m.attr("WL_KEY_UNKNOWN") = WL_KEY_UNKNOWN;
    m.attr("WL_KEY_SPACE") = WL_KEY_SPACE;
    m.attr("WL_KEY_APOSTROPHE") = WL_KEY_APOSTROPHE;
    m.attr("WL_KEY_COMMA") = WL_KEY_COMMA;
    m.attr("WL_KEY_MINUS") = WL_KEY_MINUS;
    m.attr("WL_KEY_PERIOD") = WL_KEY_PERIOD;
    m.attr("WL_KEY_SLASH") = WL_KEY_SLASH;
    m.attr("WL_KEY_0") = WL_KEY_0;
    m.attr("WL_KEY_1") = WL_KEY_1;
    m.attr("WL_KEY_2") = WL_KEY_2;
    m.attr("WL_KEY_3") = WL_KEY_3;
    m.attr("WL_KEY_4") = WL_KEY_4;
    m.attr("WL_KEY_5") = WL_KEY_5;
    m.attr("WL_KEY_6") = WL_KEY_6;
    m.attr("WL_KEY_7") = WL_KEY_7;
    m.attr("WL_KEY_8") = WL_KEY_8;
    m.attr("WL_KEY_9") = WL_KEY_9;
    m.attr("WL_KEY_SEMICOLON") = WL_KEY_SEMICOLON;
    m.attr("WL_KEY_EQUAL") = WL_KEY_EQUAL;
    m.attr("WL_KEY_A") = WL_KEY_A;
    m.attr("WL_KEY_B") = WL_KEY_B;
    m.attr("WL_KEY_C") = WL_KEY_C;
    m.attr("WL_KEY_D") = WL_KEY_D;
    m.attr("WL_KEY_E") = WL_KEY_E;
    m.attr("WL_KEY_F") = WL_KEY_F;
    m.attr("WL_KEY_G") = WL_KEY_G;
    m.attr("WL_KEY_H") = WL_KEY_H;
    m.attr("WL_KEY_I") = WL_KEY_I;
    m.attr("WL_KEY_J") = WL_KEY_J;
    m.attr("WL_KEY_K") = WL_KEY_K;
    m.attr("WL_KEY_L") = WL_KEY_L;
    m.attr("WL_KEY_M") = WL_KEY_M;
    m.attr("WL_KEY_N") = WL_KEY_N;
    m.attr("WL_KEY_O") = WL_KEY_O;
    m.attr("WL_KEY_P") = WL_KEY_P;
    m.attr("WL_KEY_Q") = WL_KEY_Q;
    m.attr("WL_KEY_R") = WL_KEY_R;
    m.attr("WL_KEY_S") = WL_KEY_S;
    m.attr("WL_KEY_T") = WL_KEY_T;
    m.attr("WL_KEY_U") = WL_KEY_U;
    m.attr("WL_KEY_V") = WL_KEY_V;
    m.attr("WL_KEY_W") = WL_KEY_W;
    m.attr("WL_KEY_X") = WL_KEY_X;
    m.attr("WL_KEY_Y") = WL_KEY_Y;
    m.attr("WL_KEY_Z") = WL_KEY_Z;
    m.attr("WL_KEY_LEFT_BRACKET") = WL_KEY_LEFT_BRACKET;
    m.attr("WL_KEY_BACKSLASH") = WL_KEY_BACKSLASH;
    m.attr("WL_KEY_RIGHT_BRACKET") = WL_KEY_RIGHT_BRACKET;
    m.attr("WL_KEY_GRAVE_ACCENT") = WL_KEY_GRAVE_ACCENT;
    m.attr("WL_KEY_WORLD_1") = WL_KEY_WORLD_1;
    m.attr("WL_KEY_WORLD_2") = WL_KEY_WORLD_2;
    m.attr("WL_KEY_ESCAPE") = WL_KEY_ESCAPE;
    m.attr("WL_KEY_ENTER") = WL_KEY_ENTER;
    m.attr("WL_KEY_TAB") = WL_KEY_TAB;
    m.attr("WL_KEY_BACKSPACE") = WL_KEY_BACKSPACE;
    m.attr("WL_KEY_INSERT") = WL_KEY_INSERT;
    m.attr("WL_KEY_DELETE") = WL_KEY_DELETE;
    m.attr("WL_KEY_RIGHT") = WL_KEY_RIGHT;
    m.attr("WL_KEY_LEFT") = WL_KEY_LEFT;
    m.attr("WL_KEY_DOWN") = WL_KEY_DOWN;
    m.attr("WL_KEY_UP") = WL_KEY_UP;
    m.attr("WL_KEY_PAGE_UP") = WL_KEY_PAGE_UP;
    m.attr("WL_KEY_PAGE_DOWN") = WL_KEY_PAGE_DOWN;
    m.attr("WL_KEY_HOME") = WL_KEY_HOME;
    m.attr("WL_KEY_END") = WL_KEY_END;
    m.attr("WL_KEY_CAPS_LOCK") = WL_KEY_CAPS_LOCK;
    m.attr("WL_KEY_SCROLL_LOCK") = WL_KEY_SCROLL_LOCK;
    m.attr("WL_KEY_NUM_LOCK") = WL_KEY_NUM_LOCK;
    m.attr("WL_KEY_PRINT_SCREEN") = WL_KEY_PRINT_SCREEN;
    m.attr("WL_KEY_PAUSE") = WL_KEY_PAUSE;
    m.attr("WL_KEY_F1") = WL_KEY_F1;
    m.attr("WL_KEY_F2") = WL_KEY_F2;
    m.attr("WL_KEY_F3") = WL_KEY_F3;
    m.attr("WL_KEY_F4") = WL_KEY_F4;
    m.attr("WL_KEY_F5") = WL_KEY_F5;
    m.attr("WL_KEY_F6") = WL_KEY_F6;
    m.attr("WL_KEY_F7") = WL_KEY_F7;
    m.attr("WL_KEY_F8") = WL_KEY_F8;
    m.attr("WL_KEY_F9") = WL_KEY_F9;
    m.attr("WL_KEY_F10") = WL_KEY_F10;
    m.attr("WL_KEY_F11") = WL_KEY_F11;
    m.attr("WL_KEY_F12") = WL_KEY_F12;
    m.attr("WL_KEY_F13") = WL_KEY_F13;
    m.attr("WL_KEY_F14") = WL_KEY_F14;
    m.attr("WL_KEY_F15") = WL_KEY_F15;
    m.attr("WL_KEY_F16") = WL_KEY_F16;
    m.attr("WL_KEY_F17") = WL_KEY_F17;
    m.attr("WL_KEY_F18") = WL_KEY_F18;
    m.attr("WL_KEY_F19") = WL_KEY_F19;
    m.attr("WL_KEY_F20") = WL_KEY_F20;
    m.attr("WL_KEY_F21") = WL_KEY_F21;
    m.attr("WL_KEY_F22") = WL_KEY_F22;
    m.attr("WL_KEY_F23") = WL_KEY_F23;
    m.attr("WL_KEY_F24") = WL_KEY_F24;
    m.attr("WL_KEY_F25") = WL_KEY_F25;
    m.attr("WL_KEY_KP_0") = WL_KEY_KP_0;
    m.attr("WL_KEY_KP_1") = WL_KEY_KP_1;
    m.attr("WL_KEY_KP_2") = WL_KEY_KP_2;
    m.attr("WL_KEY_KP_3") = WL_KEY_KP_3;
    m.attr("WL_KEY_KP_4") = WL_KEY_KP_4;
    m.attr("WL_KEY_KP_5") = WL_KEY_KP_5;
    m.attr("WL_KEY_KP_6") = WL_KEY_KP_6;
    m.attr("WL_KEY_KP_7") = WL_KEY_KP_7;
    m.attr("WL_KEY_KP_8") = WL_KEY_KP_8;
    m.attr("WL_KEY_KP_9") = WL_KEY_KP_9;
    m.attr("WL_KEY_KP_DECIMAL") = WL_KEY_KP_DECIMAL;
    m.attr("WL_KEY_KP_DIVIDE") = WL_KEY_KP_DIVIDE;
    m.attr("WL_KEY_KP_MULTIPLY") = WL_KEY_KP_MULTIPLY;
    m.attr("WL_KEY_KP_SUBTRACT") = WL_KEY_KP_SUBTRACT;
    m.attr("WL_KEY_KP_ADD") = WL_KEY_KP_ADD;
    m.attr("WL_KEY_KP_ENTER") = WL_KEY_KP_ENTER;
    m.attr("WL_KEY_KP_EQUAL") = WL_KEY_KP_EQUAL;
    m.attr("WL_KEY_LEFT_SHIFT") = WL_KEY_LEFT_SHIFT;
    m.attr("WL_KEY_RIGHT_SHIFT") = WL_KEY_RIGHT_SHIFT;
    m.attr("WL_KEY_LEFT_CONTROL") = WL_KEY_LEFT_CONTROL;
    m.attr("WL_KEY_RIGHT_CONTROL") = WL_KEY_RIGHT_CONTROL;
    m.attr("WL_KEY_RIGHT_ALT") = WL_KEY_RIGHT_ALT;
    m.attr("WL_KEY_RIGHT_SUPER") = WL_KEY_RIGHT_SUPER;
    m.attr("WL_KEY_MENU") = WL_KEY_MENU;
    m.attr("WL_KEY_LAST") = WL_KEY_LAST;

    m.def("IsKeyPressed", [](int keyCode)
          { return Wasteland::Input::IsKeyPressed(keyCode); }, py::arg("keyCode"));

    m.attr("WL_MOUSE_BUTTON_1") = WL_MOUSE_BUTTON_1;
    m.attr("WL_MOUSE_BUTTON_2") = WL_MOUSE_BUTTON_2;
    m.attr("WL_MOUSE_BUTTON_3") = WL_MOUSE_BUTTON_3;
    m.attr("WL_MOUSE_BUTTON_4") = WL_MOUSE_BUTTON_4;
    m.attr("WL_MOUSE_BUTTON_5") = WL_MOUSE_BUTTON_5;
    m.attr("WL_MOUSE_BUTTON_6") = WL_MOUSE_BUTTON_6;
    m.attr("WL_MOUSE_BUTTON_LAST") = WL_MOUSE_BUTTON_LAST;
    m.attr("WL_MOUSE_BUTTON_LEFT") = WL_MOUSE_BUTTON_LEFT;
    m.attr("WL_MOUSE_BUTTON_RIGHT") = WL_MOUSE_BUTTON_RIGHT;
    m.attr("WL_MOUSE_BUTTON_MIDDLE") = WL_MOUSE_BUTTON_MIDDLE;

    m.def("IsMouseButtonPressed", [](int button)
          { return Wasteland::Input::IsMouseButtonPressed(button); }, py::arg("button"));

    m.def("GetMousePosition", []()
          { return Wasteland::Input::GetMousePosition(); });

    m.def("GetMouseDelta", []()
          {
              static std::pair<float, float> lastMousePos = Wasteland::Input::GetMousePosition();
              std::pair<float, float> currentMousePos = Wasteland::Input::GetMousePosition();
              std::pair<float, float> delta = {currentMousePos.first - lastMousePos.first, currentMousePos.second - lastMousePos.second};
              lastMousePos = currentMousePos;
              return delta; });
}