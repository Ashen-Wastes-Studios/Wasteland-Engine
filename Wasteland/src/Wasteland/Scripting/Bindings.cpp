#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>

#include <Wasteland/Core/Log.h>
#include <Wasteland/Scene/Entity.h>
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Scene.h>
#include "Wasteland/Core/Input.h"
#include "Wasteland/Core/KeyCodes.h"
#include "Wasteland/Core/MouseButtonCodes.h"

#include <glm/glm.hpp>

namespace py = pybind11;

// Type casters for glm::vec3 to support Python tuples
namespace pybind11::detail
{
    template <>
    struct type_caster<glm::vec3>
    {
    public:
        PYBIND11_TYPE_CASTER(glm::vec3, _("glm.vec3"));

        bool load(handle src, bool)
        {
            if (py::isinstance<py::tuple>(src))
            {
                if (PyTuple_Size(src.ptr()) != 3)
                    return false;

                value.x = py::cast<float>(PyTuple_GetItem(src.ptr(), 0));
                value.y = py::cast<float>(PyTuple_GetItem(src.ptr(), 1));
                value.z = py::cast<float>(PyTuple_GetItem(src.ptr(), 2));
                return true;
            }
            return false;
        }

        static handle cast(const glm::vec3 &src, return_value_policy policy, handle parent)
        {
            return py::cast(std::make_tuple(src.x, src.y, src.z)).release();
        }
    };
}

namespace Wasteland
{
    PYBIND11_MODULE(Wasteland, m)
    {
        // 1. Bind glm::vec3 as a Python tuple-like type
        py::class_<glm::vec3>(m, "Vec3")
            .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
            .def_readwrite("x", &glm::vec3::x)
            .def_readwrite("y", &glm::vec3::y)
            .def_readwrite("z", &glm::vec3::z)
            .def("__repr__", [](const glm::vec3 &v)
                 { return "Vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")"; });

        // 2. Bind Components
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

        // 3. Bind Entity
        if (!py::hasattr(m, "Entity"))
        {
            py::class_<Entity> entity_cls(m, "Entity");

            // Constructor - allow Entity to be passed from C++
            entity_cls.def(py::init<>())
                .def(py::init<const Entity &>());

            // Basic entity methods
            entity_cls.def("HasTransform", [](Entity &e)
                           { return e.HasComponent<TransformComponent>(); })
                .def("GetTransform", [](Entity &e) -> TransformComponent &
                     { return e.GetComponent<TransformComponent>(); }, py::return_value_policy::reference_internal)
                .def("AddTransform", [](Entity &e) -> TransformComponent &
                     { return e.AddComponent<TransformComponent>(); }, py::return_value_policy::reference_internal)
                .def("HasTag", [](Entity &e)
                     { return e.HasComponent<TagComponent>(); })
                .def("GetTag", [](Entity &e) -> TagComponent &
                     { return e.GetComponent<TagComponent>(); }, py::return_value_policy::reference_internal)
                .def("AddTag", [](Entity &e, const std::string &tag) -> TagComponent &
                     { return e.AddComponent<TagComponent>(tag); }, py::return_value_policy::reference_internal);
        }

        // 4. Bind Scene
        if (!py::hasattr(m, "Scene"))
        {
            py::class_<Scene, Ref<Scene>>(m, "Scene")
                .def("CreateEntity", &Scene::CreateEntity, py::arg("name") = std::string())
                .def("DestroyEntity", &Scene::DestroyEntity);
        }

        // 5. Utility functions
        m.def("IsKeyPressed", [](int keyCode)
              { return Input::IsKeyPressed(keyCode); }, py::arg("keyCode"));
    }
}