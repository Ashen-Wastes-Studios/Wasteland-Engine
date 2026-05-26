#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>

#include <Wasteland/Scene/Entity.h>
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Scene.h>
#include "Wasteland/Core/Input.h"

// Define this globally before including pybind11 headers if possible
#define PYBIND11_DETAILED_ERROR_MESSAGES

namespace py = pybind11;

namespace Wasteland
{
    // A simple proxy struct to ensure the C++ type system
    // doesn't conflict with GLM internals.
    struct Vec3Proxy
    {
        float x, y, z;
    };

    PYBIND11_MODULE(Wasteland, m)
    {
        // 1. Bind the proxy type instead of glm::vec3 directly to avoid ODR/ABI conflicts
        py::class_<Vec3Proxy>(m, "Vec3")
            .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
            .def_readwrite("x", &Vec3Proxy::x)
            .def_readwrite("y", &Vec3Proxy::y)
            .def_readwrite("z", &Vec3Proxy::z);

        // 2. Bind components using the proxy for all vec3 fields
        py::class_<TransformComponent>(m, "TransformComponent")
            .def(py::init<>())
            .def_property("Translation", [](TransformComponent &t)
                          { return Vec3Proxy{t.Translation.x, t.Translation.y, t.Translation.z}; }, [](TransformComponent &t, const Vec3Proxy &v)
                          { t.Translation = {v.x, v.y, v.z}; })
            .def_property("Rotation", [](TransformComponent &t)
                          { return Vec3Proxy{t.Rotation.x, t.Rotation.y, t.Rotation.z}; }, [](TransformComponent &t, const Vec3Proxy &v)
                          { t.Rotation = {v.x, v.y, v.z}; })
            .def_property("Scale", [](TransformComponent &t)
                          { return Vec3Proxy{t.Scale.x, t.Scale.y, t.Scale.z}; }, [](TransformComponent &t, const Vec3Proxy &v)
                          { t.Scale = {v.x, v.y, v.z}; });

        // 3. Bind other components
        py::class_<TagComponent>(m, "TagComponent")
            .def(py::init<const std::string &>())
            .def_readwrite("Tag", &TagComponent::Tag);

        // 4. Bind Entity
        py::class_<Entity>(m, "Entity")
            .def(py::init<>())
            .def("HasTransform", [](Entity &e)
                 { return e.HasComponent<TransformComponent>(); })
            .def("GetTransform", [](Entity &e) -> TransformComponent &
                 { return e.GetComponent<TransformComponent>(); }, py::return_value_policy::reference)
            .def("HasTag", [](Entity &e)
                 { return e.HasComponent<TagComponent>(); })
            .def("GetTag", [](Entity &e) -> TagComponent &
                 { return e.GetComponent<TagComponent>(); }, py::return_value_policy::reference);

        // 5. Bind Scene
        py::class_<Scene, std::shared_ptr<Scene>>(m, "Scene")
            .def("CreateEntity", &Scene::CreateEntity, py::arg("name") = std::string())
            .def("DestroyEntity", &Scene::DestroyEntity);

        // 6. Utility
        m.def("IsKeyPressed", [](int keyCode)
              { return Input::IsKeyPressed(keyCode); }, py::arg("keyCode"));
    }
}