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
        auto &sc = entity.GetComponent<ScriptComponent>();
        auto id = entity.GetComponent<IDComponent>().ID;

        if (s_FailedScripts.find(id) != s_FailedScripts.end())
            return;

        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            std::string cleanedPath = CleanPath(sc.ScriptPath);

            // 1. SANITIZE: Strip absolute path if present
            std::filesystem::path p(cleanedPath);
            if (p.is_absolute())
                cleanedPath = p.parent_path().filename().string() + "/" + p.filename().string();

            // 2. RESOLVE: Build absolute path in assets/
            std::filesystem::path assetRoot = std::filesystem::current_path() / "assets";
            std::filesystem::path scriptFullPath = assetRoot / cleanedPath;
            scriptFullPath.make_preferred(); // Fixes slash issues

            if (!std::filesystem::exists(scriptFullPath))
            {
                WL_CORE_ERROR("Script file NOT FOUND at: {0}", scriptFullPath.string());
                s_FailedScripts.insert(id);
                return;
            }

            // 3. PYTHON LOADING
            try
            {
                static py::module_ sys = py::module_::import("sys");
                py::list sysPath = sys.attr("path").cast<py::list>();

                // Add script folder to sys.path
                std::string scriptDir = scriptFullPath.parent_path().string();
                // Add engine module folder (where the Wasteland module lives)
                std::string engineDir = std::filesystem::current_path().string();

                auto AddToPath = [&](const std::string &path)
                {
                    for (auto item : sysPath)
                        if (item.cast<std::string>() == path)
                            return;
                    sysPath.append(path);
                };

                AddToPath(scriptDir);
                AddToPath(engineDir); // Ensures "import Wasteland" works

                std::string moduleName = scriptFullPath.stem().string();
                static py::module_ importlib = py::module_::import("importlib");
                py::dict modules = sys.attr("modules").cast<py::dict>();

                py::module_ scriptModule;
                if (modules.contains(moduleName.c_str()))
                    scriptModule = importlib.attr("reload")(modules[moduleName.c_str()]);
                else
                    scriptModule = py::module_::import(moduleName.c_str());

                py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                s_EntityInstances[id] = scriptClass(entity);

                WL_CORE_INFO("Successfully initialized: {0}", moduleName);
            }
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
            }
        }

        // 4. RUNTIME UPDATE
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