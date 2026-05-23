#include "wlpch.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <Wasteland/Core/Log.h>
#include <Wasteland/Scene/Entity.h>

namespace py = pybind11;

PYBIND11_MODULE(WastelandCore, m)
{
    m.doc() = "Wasteland Engine Python API";

    m.def("log_info", &Wasteland::Log::Init, "Log a message to the console");

    py::class_<Wasteland::Entity>(m, "Entity")
        .def(py::init()) // Constructor
        .def("get_name", &Wasteland::Entity::GetName)
        .def("get_transform", [](Wasteland::Entity &e)
             { return e.GetComponent<Wasteland::TransformComponent>().GetTransform(); });
}