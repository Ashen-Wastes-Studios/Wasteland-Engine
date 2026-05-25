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

    // Helper to bind a component to an entity in one line
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
        py::class_<Entity> entity_cls(m, "Entity");
        BindComponent<TransformComponent>(entity_cls, "Transform");
        BindComponent<TagComponent>(entity_cls, "Tag");

        // 1. Bind Components
        py::class_<TransformComponent>(m, "TransformComponent")
            .def(py::init<>())
            .def_readwrite("Translation", &TransformComponent::Translation)
            .def_readwrite("Rotation", &TransformComponent::Rotation)
            .def_readwrite("Scale", &TransformComponent::Scale);

        py::class_<TagComponent>(m, "TagComponent")
            .def(py::init<const std::string &>())
            .def_readwrite("Tag", &TagComponent::Tag);

        // 2. Bind Entity
        // Note: Since Entity is a wrapper, we bind it by value or reference
        py::class_<Entity>(m, "Entity")
            .def("HasComponentTransform", [](Entity &e)
                 { return e.HasComponent<TransformComponent>(); })
            .def("GetTransform", [](Entity &e) -> TransformComponent &
                 { return e.GetComponent<TransformComponent>(); }, py::return_value_policy::reference)
            .def("AddTransform", [](Entity &e)
                 { return e.AddComponent<TransformComponent>(); }, py::return_value_policy::reference);

        // 3. Bind Scene
        py::class_<Scene, Ref<Scene>>(m, "Scene")
            .def("CreateEntity", &Scene::CreateEntity, py::arg("name") = std::string())
            .def("DestroyEntity", &Scene::DestroyEntity);

        m.def("IsKeyPressed", [](const std::string &keyName)
              {
            // Replace 'Wasteland::Input' with your actual engine input class
            return Input::IsKeyPressed(WL_KEY_W); });
    }

}