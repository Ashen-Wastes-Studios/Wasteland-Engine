#include "wlpch.h"
#include "ScriptEngine.h"
#include "Wasteland/Scene/Components.h"

#include <filesystem>
#include <unordered_set>
#include <algorithm>

namespace Wasteland
{
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;
    static std::unordered_set<UUID> s_FailedScripts;

    static std::string CleanPath(std::string path)
    {
        path.erase(std::remove_if(path.begin(), path.end(), [](char c)
                                  { return (unsigned char)c < 32; }),
                   path.end());
        size_t first = path.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = path.find_last_not_of(" \t\r\n");
        return path.substr(first, (last - first + 1));
    }

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        if (!entity.IsValid())
            return;

        if (!entity.HasComponent<ScriptComponent>())
            return;

        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        if (s_FailedScripts.find(id) != s_FailedScripts.end())
            return;

        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            std::string cleanedPath = CleanPath(sc.ScriptPath);

            WL_CORE_INFO("ScriptEngine: Initializing Entity ID {0} with Path: {1}, Class: {2}", id, cleanedPath, sc.ScriptName);

            std::filesystem::path assetRoot = std::filesystem::current_path() / "assets";
            std::filesystem::path scriptFullPath = assetRoot / cleanedPath;
            scriptFullPath.make_preferred();

            if (!std::filesystem::exists(scriptFullPath))
            {
                WL_CORE_ERROR("Script file NOT FOUND at: {0}. Please update the ScriptComponent in your Scene.", scriptFullPath.string());
                s_FailedScripts.insert(id);
                return;
            }

            try
            {
                static py::module_ sys = py::module_::import("sys");
                py::list sysPath = sys.attr("path").cast<py::list>();

                sysPath.append("DemonCore-Editor/assets/scripts");

                std::string scriptDir = scriptFullPath.parent_path().string();
                std::string engineDir = std::filesystem::current_path().string();

                auto AddToPath = [&](const std::string &path)
                {
                    for (auto item : sysPath)
                        if (item.cast<std::string>() == path)
                            return;
                    sysPath.append(path);
                };

                AddToPath(scriptDir);
                AddToPath(engineDir);

                std::string moduleName = scriptFullPath.stem().string();
                static py::module_ importlib = py::module_::import("importlib");
                py::dict modules = sys.attr("modules").cast<py::dict>();

                py::module_ scriptModule;
                if (modules.contains(moduleName.c_str()))
                    scriptModule = importlib.attr("reload")(modules[moduleName.c_str()]);
                else
                    scriptModule = py::module_::import(moduleName.c_str());

                try
                {
                    py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                    s_EntityInstances[id] = scriptClass(py::cast(entity));
                }
                catch (py::error_already_set &e)
                {
                    std::cout << "Python Error: " << e.what() << std::endl;
                    e.restore();
                }

                WL_CORE_INFO("Successfully initialized: {0}", moduleName);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
            }
        }

        auto it = s_EntityInstances.find(id);
        if (it != s_EntityInstances.end())
        {
            try
            {
                it->second.attr("OnUpdateEntity")(ts);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during update: {0}", e.what());
                PyErr_Clear();
            }
        }
    }

    void ScriptEngine::OnDestroyEntity(UUID id)
    {
        if (s_EntityInstances.find(id) != s_EntityInstances.end())
        {
            s_EntityInstances.erase(id);
        }
    }

    void ScriptEngine::Shutdown()
    {
        s_EntityInstances.clear();
        s_FailedScripts.clear();

        WL_CORE_INFO("ScriptEngine: Cleaned up instance cache.");
    }
}