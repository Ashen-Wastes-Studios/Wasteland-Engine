#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <Wasteland/Core/Log.h>
#include <Wasteland/Scene/Entity.h>
#include <Wasteland/Scene/Components.h>
#include "Wasteland/Core/Input.h"
#include "Wasteland/Core/KeyCodes.h"
#include "Wasteland/Core/MouseButtonCodes.h"

namespace py = pybind11;

namespace Wasteland
{
    // Helper to bind a component to an entity
    template <typename T>
    void BindComponent(py::class_<Entity> &entity_class, const std::string &name)
    {
        entity_class.def(("Has" + name).c_str(), [](Entity &e)
                         { return e.HasComponent<T>(); });
        entity_class.def(("Get" + name).c_str(), [](Entity &e) -> T &
                         { return e.GetComponent<T>(); }, py::return_value_policy::reference);
    }

    PYBIND11_MODULE(Wasteland, m)
    {
        // 1. Bind Components (Safely)
        if (!py::hasattr(m, "TransformComponent"))
        {
            py::class_<TransformComponent>(m, "TransformComponent")
                .def(py::init<>())
                .def_readwrite("Translation", &TransformComponent::Translation)
                .def_readwrite("Rotation", &TransformComponent::Rotation)
                .def_readwrite("Scale", &TransformComponent::Scale);
        }

        if (!py::hasattr(m, "TagComponent"))
        {
            py::class_<TagComponent>(m, "TagComponent")
                .def(py::init<const std::string &>())
                .def_readwrite("Tag", &TagComponent::Tag);
        }

        // 2. Bind Entity
        if (!py::hasattr(m, "Entity"))
        {
            // First-time initialization
            py::class_<Entity> entity_cls(m, "Entity");

            BindComponent<TransformComponent>(entity_cls, "Transform");
            BindComponent<TagComponent>(entity_cls, "Tag");

            entity_cls.def("HasComponentTransform", [](Entity &e)
                           { return e.HasComponent<TransformComponent>(); })
                .def("GetTransform", [](Entity &e) -> TransformComponent &
                     { return e.GetComponent<TransformComponent>(); }, py::return_value_policy::reference)
                .def("AddTransform", [](Entity &e)
                     { return e.AddComponent<TransformComponent>(); }, py::return_value_policy::reference);
        }

        // 3. Bind Scene
        if (!py::hasattr(m, "Scene"))
        {
            py::class_<Scene, Ref<Scene>>(m, "Scene")
                .def("CreateEntity", &Scene::CreateEntity, py::arg("name") = std::string())
                .def("DestroyEntity", &Scene::DestroyEntity);
        }

        // 4. Utility functions
        m.def("IsKeyPressed", [](const std::string &keyName)
              {
                  return Input::IsKeyPressed(WL_KEY_W); // Note: Keep your key mapping logic here
              });
    }
}