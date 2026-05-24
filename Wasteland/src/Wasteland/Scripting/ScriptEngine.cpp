#include "wlpch.h"
#include "ScriptEngine.h"

#include "Wasteland/Scene/Components.h"

#include <filesystem>
#include <unordered_set>
#include <algorithm>

namespace Wasteland
{
    // Persistence containers
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;
    static std::unordered_set<UUID> s_FailedScripts;

    static std::string CleanPath(std::string path)
    {
        // 1. Remove non-printable control characters
        path.erase(std::remove_if(path.begin(), path.end(), [](char c)
                                  { return (unsigned char)c < 32; }),
                   path.end());

        // 2. Trim whitespace from start and end
        size_t first = path.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = path.find_last_not_of(" \t\r\n");

        return path.substr(first, (last - first + 1));
    }

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        // Skip if this script already failed to load
        if (s_FailedScripts.find(id) != s_FailedScripts.end())
            return;

        // --- 1. INITIALIZATION: Load script if not already instance-held ---
        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            std::string cleanedPath = CleanPath(sc.ScriptPath);
            std::filesystem::path scriptFullPath(cleanedPath);

            // Bulletproof Resolution:
            // Try raw path first (if it's absolute, this works immediately)
            bool found = std::filesystem::exists(scriptFullPath);

            // Fallback: Try resolving relative to Project Root
            if (!found)
            {
                std::filesystem::path relativePath = std::filesystem::current_path().parent_path() / cleanedPath;
                if (std::filesystem::exists(relativePath))
                {
                    scriptFullPath = relativePath;
                    found = true;
                }
            }

            if (!found)
            {
                WL_CORE_ERROR("Script file NOT FOUND at: {0}", cleanedPath);
                s_FailedScripts.insert(id);
                return;
            }

            // --- 2. PYTHON LOADING ---
            try
            {
                std::string moduleName = scriptFullPath.stem().string();

                // Cache these imports so they only happen once
                static py::module_ sys = py::module_::import("sys");
                static py::module_ importlib = py::module_::import("importlib");
                py::dict modules = sys.attr("modules").cast<py::dict>();

                py::module_ scriptModule;

                if (modules.contains(moduleName.c_str()))
                {
                    // Reload if already exists (allows hot-reloading)
                    scriptModule = importlib.attr("reload")(modules[moduleName.c_str()]);
                }
                else
                {
                    // Import fresh
                    scriptModule = py::module_::import(moduleName.c_str());
                }

                // Instantiate class
                py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                s_EntityInstances[id] = scriptClass(entity);

                WL_CORE_INFO("Successfully initialized: {0}", moduleName);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
                return;
            }
        }

        // --- 3. RUNTIME UPDATE ---
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