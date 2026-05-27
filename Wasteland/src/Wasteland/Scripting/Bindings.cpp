#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>

#include <Wasteland/Scene/Entity.h>
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Scene.h>
#include "Wasteland/Core/Input.h"

namespace py = pybind11;

PYBIND11_MODULE(Wasteland, m)
{
    // 1. Bind glm::vec3 as a standard Python object.
    // No custom type_caster needed, which resolves the cast_error.
    py::class_<glm::vec3>(m, "Vec3")
        .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
        .def_readwrite("x", &glm::vec3::x)
        .def_readwrite("y", &glm::vec3::y)
        .def_readwrite("z", &glm::vec3::z);

    // 2. Bind Components using properties to bridge C++ memory to Python
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

    py::class_<Wasteland::TagComponent>(m, "TagComponent")
        .def(py::init<const std::string &>())
        .def_readwrite("Tag", &Wasteland::TagComponent::Tag);

    // 3. Bind Entity
    py::class_<Wasteland::Entity>(m, "Entity")
        .def(py::init<>())
        .def("HasTransform", [](Wasteland::Entity &e)
             { return e.HasComponent<Wasteland::TransformComponent>(); })
        .def("GetTransform", [](Wasteland::Entity &e) -> Wasteland::TransformComponent &
             { return e.GetComponent<Wasteland::TransformComponent>(); }, py::return_value_policy::reference)
        .def("HasTag", [](Wasteland::Entity &e)
             { return e.HasComponent<Wasteland::TagComponent>(); })
        .def("GetTag", [](Wasteland::Entity &e) -> Wasteland::TagComponent &
             { return e.GetComponent<Wasteland::TagComponent>(); }, py::return_value_policy::reference);

    // 4. Bind Scene
    py::class_<Wasteland::Scene, std::shared_ptr<Wasteland::Scene>>(m, "Scene")
        .def("CreateEntity", &Wasteland::Scene::CreateEntity, py::arg("name") = std::string())
        .def("DestroyEntity", &Wasteland::Scene::DestroyEntity);

    // 5. Utilities
    m.def("IsKeyPressed", [](int keyCode)
          { return Wasteland::Input::IsKeyPressed(keyCode); }, py::arg("keyCode"));
}