#include "wlpch.h"
#include "ScriptEngine.h"

#include "Wasteland/Scene/Components.h"

#include <filesystem>
#include <unordered_set>
#include <algorithm>

namespace Wasteland
{
    // These MUST be outside the function to persist across frames
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;
    static std::unordered_set<UUID> s_FailedScripts;

    // Helper to remove invisible characters or whitespace
    static std::string CleanPath(std::string path)
    {
        path.erase(std::remove(path.begin(), path.end(), '\r'), path.end());
        path.erase(std::remove(path.begin(), path.end(), '\n'), path.end());
        path.erase(std::remove(path.begin(), path.end(), '\t'), path.end());
        size_t first = path.find_first_not_of(' ');
        size_t last = path.find_last_not_of(' ');
        return (first == std::string::npos) ? "" : path.substr(first, (last - first + 1));
    }

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        if (s_FailedScripts.find(id) != s_FailedScripts.end())
            return;

        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            // Clean the path before doing anything else
            std::string cleanedPath = CleanPath(sc.ScriptPath);
            std::filesystem::path scriptFullPath(cleanedPath);

            // If path is relative, attempt to fix it
            if (!scriptFullPath.is_absolute())
            {
                std::filesystem::path projectRoot = std::filesystem::current_path().parent_path();
                scriptFullPath = projectRoot / scriptFullPath;
            }

            // Check existence
            if (!std::filesystem::exists(scriptFullPath))
            {
                WL_CORE_ERROR("Script file NOT FOUND at: {0}", scriptFullPath.string());
                s_FailedScripts.insert(id); // Block further attempts
                return;
            }

            try
            {
                // Import Setup
                std::string directory = scriptFullPath.parent_path().string();
                py::module_ sys = py::module_::import("sys");

                // Add to sys.path if missing
                py::list pathList = sys.attr("path").cast<py::list>();
                bool alreadyInPath = false;
                for (auto item : pathList)
                {
                    if (item.cast<std::string>() == directory)
                    {
                        alreadyInPath = true;
                        break;
                    }
                }
                if (!alreadyInPath)
                    sys.attr("path").attr("append")(directory);

                // Instantiate
                std::string moduleName = scriptFullPath.stem().string();
                py::module_ scriptModule = py::module_::import(moduleName.c_str());
                py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                s_EntityInstances[id] = scriptClass(entity);

                WL_CORE_INFO("Successfully initialized: {0}", moduleName);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id); // Mark as failed on Python error
                return;
            }
        }

        auto it = s_EntityInstances.find(id);
        if (it != s_EntityInstances.end())
        {
            try
            {
                it->second.attr("OnUpdate")(ts);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during update: {0}", e.what());
                PyErr_Clear();
            }
        }
    }
}