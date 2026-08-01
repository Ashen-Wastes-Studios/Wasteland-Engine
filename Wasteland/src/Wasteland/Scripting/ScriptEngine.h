#include <unordered_map>

#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"
#include "Wasteland/Core/Timestep.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Wasteland
{
    class ScriptEngine
    {
    public:
        static void OnUpdateEntity(Entity entity, Timestep ts);
        static void OnDestroyEntity(UUID id);

        static void Shutdown();

        static py::object* GetInstance(UUID id);

    private:
        static std::unordered_map<UUID, py::object> s_EntityInstances;
    };
}