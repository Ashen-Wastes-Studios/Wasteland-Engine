#include "wlpch.h"
#include "ScriptEngine.h"

#include "Wasteland/Scene/Components.h"

#include <filesystem>

namespace Wasteland
{
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        // 1. Define your project root
        std::filesystem::path projectRoot = "C:/dev/Wasteland/DemonCore-Editor/assets/";

        // 2. Check the path
        std::filesystem::path scriptPath = sc.ScriptPath;
        if (!std::filesystem::exists(scriptPath))
        {
            // Try appending the project root if it wasn't found
            scriptPath = projectRoot / scriptPath;
        }

        if (std::filesystem::exists(scriptPath))
        {
            if (s_EntityInstances.find(id) == s_EntityInstances.end())
            {
                try
                {
                    // 1. Get the absolute directory
                    std::filesystem::path scriptPath = sc.ScriptPath;
                    std::string directory = std::filesystem::absolute(scriptPath.parent_path()).string();

                    // 2. Access sys.path
                    py::module_ sys = py::module_::import("sys");
                    py::list pathList = sys.attr("path").cast<py::list>();

                    // 3. Check if path is already there before adding
                    bool alreadyAdded = false;
                    for (auto item : pathList)
                    {
                        if (item.cast<std::string>() == directory)
                        {
                            alreadyAdded = true;
                            break;
                        }
                    }

                    if (!alreadyAdded)
                    {
                        sys.attr("path").attr("append")(directory);
                    }

                    // 4. Proceed with import...
                    std::string moduleName = scriptPath.stem().string();
                    py::module_ scriptModule = py::module_::import(moduleName.c_str());

                    py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                    s_EntityInstances[id] = scriptClass(entity);
                }
                catch (const std::exception &e)
                {
                    WL_CORE_CRITICAL("CATCH-ALL ERROR: ", e.what());
                }
                catch (...)
                {
                    WL_CORE_CRITICAL("CRITICAL ERROR: Unknown exception occured!", nullptr);
                }
            }
        }
        else
        {
            WL_CORE_ERROR("Could not find script at: {0}", scriptPath.string());
        }

        s_EntityInstances[id].attr("OnUpdate")(ts);
    }
}