#include "wlpch.h"
#include "ScriptEngine.h"

#include "Wasteland/Scene/Components.h"

#include <filesystem>

namespace Wasteland
{
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;

    void ScriptEngine::OnUpdate(Timestep ts)
    {
        py::module_ scriptModule;
        try
        {
            scriptModule.attr("OnUpdate")(ts);
        }
        catch (const std::exception &e)
        {
            WL_CORE_ERROR("Python Script Error: {0}", e.what());
        }
    }

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            std::filesystem::path scriptPath = sc.ScriptPath;
            std::string directory = scriptPath.parent_path().string();
            std::string moduleName = scriptPath.stem().string(); // "MyScript" from "MyScript.py"

            // This allows Python to "see" your files
            py::module_ sys = py::module_::import("sys");
            sys.attr("path").attr("append")(directory);

            py::module_ scriptModule = py::module_::import(moduleName.c_str());

            py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
            s_EntityInstances[id] = scriptClass(entity);
        }

        s_EntityInstances[id].attr("OnUpdate")(ts);
    }
}