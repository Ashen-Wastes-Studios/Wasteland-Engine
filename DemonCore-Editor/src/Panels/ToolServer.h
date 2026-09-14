#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace Wasteland
{
    class Scene;
    class Entity;

    // Exposes tool definitions (for the system prompt) and executes tool calls
    // made by the AI. Tools: read_file, write_file, edit_file, list_files,
    // memory_read, memory_append, list_entities, get_entity_info.
    class ToolServer
    {
    public:
        static ToolServer &Instance();

        // JSON array of tool definitions, injected into the system prompt.
        std::string GetToolDefinitionsJson() const;

        // Execute a tool by name with a JSON arguments object.
        std::string ExecuteTool(const std::string &name, const std::string &argsJson);

        // Memory file location.
        static std::filesystem::path GetMemoryPath();

        // Callback when a file is written (for auto-opening in editor).
        using FileWrittenCallback = std::function<void(const std::string &path)>;
        void SetFileWrittenCallback(FileWrittenCallback cb);

        // Scene access for entity inspection tools.
        void SetScene(Scene *scene);

    private:
        ToolServer() = default;

        // File tools
        std::string ReadFile(const std::string &path);
        std::string WriteFile(const std::string &path, const std::string &content);
        std::string EditFile(const std::string &path, const std::string &oldText, const std::string &newText);
        std::string ListFiles(const std::string &dir);

        // Memory tools
        std::string ReadMemory();
        std::string AppendMemory(const std::string &text);

        // Entity inspection tools
        std::string ListEntities();
        std::string GetEntityInfo(const std::string &id);
        std::string GetEntityScriptState(const std::string &id);
        std::string GetComponentInfo(const std::string &id, const std::string &componentType);

        FileWrittenCallback m_FileWrittenCallback;
        Scene *m_Screen = nullptr;
    };

}
