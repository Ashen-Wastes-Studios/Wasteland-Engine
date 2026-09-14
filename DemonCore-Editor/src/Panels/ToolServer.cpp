#include "wlpch.h"
#include "ToolServer.h"
#include "JsonMini.h"
#include "Wasteland/Scene/Scene.h"
#include "Wasteland/Scene/Entity.h"
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Scripting/ScriptEngine.h"

#include <pybind11/pybind11.h>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace py = pybind11;

namespace Wasteland
{

    ToolServer &ToolServer::Instance()
    {
        static ToolServer s;
        return s;
    }

    std::filesystem::path ToolServer::GetMemoryPath()
    {
#if defined(_WIN32)
        const char *appData = std::getenv("APPDATA");
        std::filesystem::path dir = appData ? std::filesystem::path(appData) / "Wasteland" : std::filesystem::path("assets");
#else
        const char *home = std::getenv("HOME");
        std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "Wasteland" : std::filesystem::path("assets");
#endif
        return dir / "ai_memory.txt";
    }

    void ToolServer::SetFileWrittenCallback(FileWrittenCallback cb)
    {
        m_FileWrittenCallback = cb;
    }

    void ToolServer::SetScene(Scene *scene)
    {
        m_Screen = scene;
    }

    std::string ToolServer::GetToolDefinitionsJson() const
    {
        std::string json = "[";
        json += "{\"name\":\"read_file\",\"description\":\"Read the full contents of a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative path from the project root\"}},\"required\":[\"path\"]}},";
        json += "{\"name\":\"write_file\",\"description\":\"Write content to a file, creating or overwriting.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative path from the project root\"},\"content\":{\"type\":\"string\",\"description\":\"The full file content to write\"}},\"required\":[\"path\",\"content\"]}},";
        json += "{\"name\":\"edit_file\",\"description\":\"Replace exact text in a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative path from the project root\"},\"old_text\":{\"type\":\"string\",\"description\":\"The exact text to find and replace (must be unique)\"},\"new_text\":{\"type\":\"string\",\"description\":\"The replacement text\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}},";
        json += "{\"name\":\"list_files\",\"description\":\"List files in a directory recursively.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\",\"description\":\"Relative directory path (default: assets)\"}},\"required\":[\"dir\"]}},";
        json += "{\"name\":\"memory_read\",\"description\":\"Read the AI persistent memory. Contains lessons and user preferences.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        json += "{\"name\":\"memory_append\",\"description\":\"Append a note to persistent memory for future sessions.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The note to append to memory\"}},\"required\":[\"text\"]}},";
        json += "{\"name\":\"list_entities\",\"description\":\"List all entities in the current scene with UUID, name, and components.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        json += "{\"name\":\"get_entity_info\",\"description\":\"Get detailed info about an entity: transform, tag, and all components.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"The entity UUID from list_entities\"}},\"required\":[\"id\"]}},";
        json += "{\"name\":\"get_script_state\",\"description\":\"Get the live state of a script instance variables.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"The entity UUID\"}},\"required\":[\"id\"]}}";
        json += "]";
        return json;
    }

    std::string ToolServer::ExecuteTool(const std::string &name, const std::string &argsJson)
    {
        if (name == "read_file")
        {
            std::string path;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: read_file requires 'path' argument";
            return ReadFile(path);
        }
        else if (name == "write_file")
        {
            std::string path, content;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: write_file requires 'path' argument";
            if (!JsonMini::ExtractString(argsJson, "content", content))
                return "Error: write_file requires 'content' argument";
            return WriteFile(path, content);
        }
        else if (name == "edit_file")
        {
            std::string path, oldText, newText;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: edit_file requires 'path' argument";
            if (!JsonMini::ExtractString(argsJson, "old_text", oldText))
                return "Error: edit_file requires 'old_text' argument";
            if (!JsonMini::ExtractString(argsJson, "new_text", newText))
                return "Error: edit_file requires 'new_text' argument";
            return EditFile(path, oldText, newText);
        }
        else if (name == "list_files")
        {
            std::string dir = "assets";
            JsonMini::ExtractString(argsJson, "dir", dir);
            return ListFiles(dir);
        }
        else if (name == "memory_read")
        {
            return ReadMemory();
        }
        else if (name == "memory_append")
        {
            std::string text;
            if (!JsonMini::ExtractString(argsJson, "text", text))
                return "Error: memory_append requires 'text' argument";
            return AppendMemory(text);
        }
        else if (name == "list_entities")
        {
            return ListEntities();
        }
        else if (name == "get_entity_info")
        {
            std::string id;
            if (!JsonMini::ExtractString(argsJson, "id", id))
                return "Error: get_entity_info requires 'id' argument";
            return GetEntityInfo(id);
        }
        else if (name == "get_script_state")
        {
            std::string id;
            if (!JsonMini::ExtractString(argsJson, "id", id))
                return "Error: get_script_state requires 'id' argument";
            return GetEntityScriptState(id);
        }
        return "Error: unknown tool '" + name + "'";
    }

    std::string ToolServer::ReadFile(const std::string &path)
    {
        std::filesystem::path fullPath = std::filesystem::absolute(path);
        std::ifstream file(fullPath);
        if (!file.is_open())
            return "Error: cannot open file '" + path + "'";
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::string ToolServer::WriteFile(const std::string &path, const std::string &content)
    {
        std::filesystem::path fullPath = std::filesystem::absolute(path);
        std::filesystem::create_directories(fullPath.parent_path());

        std::ofstream file(fullPath, std::ios::trunc);
        if (!file.is_open())
            return "Error: cannot write file '" + path + "'";
        file << content;
        file.close();

        if (m_FileWrittenCallback)
            m_FileWrittenCallback(path);

        return "Successfully wrote " + std::to_string(content.size()) + " bytes to '" + path + "'";
    }

    std::string ToolServer::EditFile(const std::string &path, const std::string &oldText, const std::string &newText)
    {
        std::filesystem::path fullPath = std::filesystem::absolute(path);
        std::ifstream inFile(fullPath);
        if (!inFile.is_open())
            return "Error: cannot open file '" + path + "' for editing";
        std::stringstream ss;
        ss << inFile.rdbuf();
        inFile.close();

        std::string content = ss.str();
        size_t pos = content.find(oldText);
        if (pos == std::string::npos)
            return "Error: old_text not found in '" + path + "'";
        size_t secondPos = content.find(oldText, pos + oldText.size());
        if (secondPos != std::string::npos)
            return "Error: old_text appears multiple times in '" + path + "'";

        content.replace(pos, oldText.size(), newText);

        std::ofstream outFile(fullPath, std::ios::trunc);
        if (!outFile.is_open())
            return "Error: cannot write edited file '" + path + "'";
        outFile << content;
        outFile.close();

        if (m_FileWrittenCallback)
            m_FileWrittenCallback(path);

        return "Successfully edited '" + path + "'";
    }

    std::string ToolServer::ListFiles(const std::string &dir)
    {
        std::filesystem::path fullPath = std::filesystem::absolute(dir);
        if (!std::filesystem::is_directory(fullPath))
            return "Error: '" + dir + "' is not a directory";

        std::string result;
        for (auto it = std::filesystem::recursive_directory_iterator(fullPath);
             it != std::filesystem::recursive_directory_iterator(); ++it)
        {
            if (it->is_regular_file())
            {
                std::string rel = std::filesystem::relative(it->path(), std::filesystem::absolute("")).string();
                result += rel + "\n";
            }
        }
        if (result.empty())
            return "(empty directory)";
        return result;
    }

    std::string ToolServer::ReadMemory()
    {
        std::filesystem::path memPath = GetMemoryPath();
        std::ifstream file(memPath);
        if (!file.is_open())
            return "(memory is empty)";
        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        if (content.empty())
            return "(memory is empty)";
        return content;
    }

    std::string ToolServer::AppendMemory(const std::string &text)
    {
        std::filesystem::path memPath = GetMemoryPath();
        std::filesystem::create_directories(memPath.parent_path());

        std::ofstream file(memPath, std::ios::app);
        if (!file.is_open())
            return "Error: cannot write to memory file";
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char timeBuf[64];
        tm timeInfo;
        localtime_s(&timeInfo, &time);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &timeInfo);
        file << "[" << timeBuf << "] " << text << "\n";
        file.close();

        return "Memory updated. Note saved.";
    }

    std::string ToolServer::ListEntities()
    {
        if (!m_Screen)
            return "Error: no active scene";

        auto view = m_Screen->GetAllEntitiesWith<IDComponent, TagComponent>();
        std::string result;
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            auto &id = entity.GetComponent<IDComponent>();
            auto &tag = entity.GetComponent<TagComponent>();
            result += "UUID: " + std::to_string((uint64_t)id.ID) + " | Tag: " + tag.Tag + "\n";
        }
        if (result.empty())
            return "(scene has no entities)";
        return result;
    }

    std::string ToolServer::GetEntityInfo(const std::string &id)
    {
        if (!m_Screen)
            return "Error: no active scene";

        uint64_t entityId = std::stoull(id);
        bool found = false;
        std::string result;
        auto view = m_Screen->GetAllEntitiesWith<IDComponent, TagComponent>();
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            auto &compId = entity.GetComponent<IDComponent>();
            if ((uint64_t)compId.ID != entityId)
                continue;

            auto &tag = entity.GetComponent<TagComponent>();
            found = true;
            result += "Entity: " + tag.Tag + " (UUID: " + id + ")\n";

            if (entity.HasComponent<TransformComponent>())
            {
                auto &tc = entity.GetComponent<TransformComponent>();
                result += "  Transform:\n";
                result += "    Translation: (" + std::to_string(tc.Translation.x) + ", " + std::to_string(tc.Translation.y) + ", " + std::to_string(tc.Translation.z) + ")\n";
                result += "    Rotation: (" + std::to_string(tc.Rotation.x) + ", " + std::to_string(tc.Rotation.y) + ", " + std::to_string(tc.Rotation.z) + ")\n";
                result += "    Scale: (" + std::to_string(tc.Scale.x) + ", " + std::to_string(tc.Scale.y) + ", " + std::to_string(tc.Scale.z) + ")\n";
            }

            if (entity.HasComponent<ScriptComponent>())
            {
                auto &sc = entity.GetComponent<ScriptComponent>();
                result += "  Script: " + sc.ScriptName + " (" + sc.ScriptPath + ")\n";
            }

            if (entity.HasComponent<CameraComponent>())
            {
                result += "  CameraComponent: present\n";
            }

            if (entity.HasComponent<SpriteRendererComponent>())
            {
                result += "  SpriteRenderer: present\n";
            }

            if (entity.HasComponent<Rigidbody2DComponent>())
            {
                result += "  Rigidbody2D: present\n";
            }

            if (entity.HasComponent<BoxCollider2DComponent>())
            {
                result += "  BoxCollider2D: present\n";
            }

            break;
        }

        if (!found)
            return "Error: entity with UUID " + id + " not found";
        return result;
    }

    std::string ToolServer::GetEntityScriptState(const std::string &id)
    {
        if (!m_Screen)
            return "Error: no active scene";

        uint64_t entityId = std::stoull(id);

        auto view = m_Screen->GetAllEntitiesWith<IDComponent, ScriptComponent>();
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            auto &compId = entity.GetComponent<IDComponent>();
            if ((uint64_t)compId.ID != entityId)
                continue;

            auto &sc = entity.GetComponent<ScriptComponent>();
            py::object *instance = ScriptEngine::GetInstance(compId.ID);
            if (!instance)
                return "Script instance not initialized. Run the scene first.\nScript path: " + sc.ScriptPath;

            try
            {
                py::list attrs = instance->attr("__dict__").attr("keys")().cast<py::list>();
                std::string result = "Script variables for entity " + std::to_string(entityId) + ":\n";
                for (auto attrObj : attrs)
                {
                    std::string attrName = attrObj.cast<std::string>();
                    if (attrName.empty() || attrName[0] == '_')
                        continue;

                    py::object value;
                    try
                    {
                        value = instance->attr(attrName.c_str());
                    }
                    catch (py::error_already_set &)
                    {
                        PyErr_Clear();
                        continue;
                    }

                    if (py::isinstance<py::function>(value) || py::isinstance<py::module_>(value))
                        continue;

                    std::string repr;
                    try
                    {
                        repr = py::str(value).cast<std::string>();
                    }
                    catch (py::error_already_set &)
                    {
                        PyErr_Clear();
                        repr = "<error>";
                    }
                    result += "  " + attrName + " = " + repr + "\n";
                }
                return result;
            }
            catch (py::error_already_set &e)
            {
                PyErr_Clear();
                return "Error reading script state: " + std::string(e.what());
            }
        }

        return "Error: entity with UUID " + id + " not found or has no script";
    }

}
