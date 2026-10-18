#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Wasteland
{
    // Forward declarations rather than includes: the tools below only pass a
    // Scene* around, and including Scene.h would drag entt and every component into
    // each panel that includes this header.
    class Scene;
    class Entity;

    // The bridge between the LLM and the editor: a call arrives as a name plus a
    // JSON arguments object and returns a string, so the AI side needs a schema and no
    // knowledge of the editor.
    //
    // DEFECT (trust boundary, by design): ExecuteTool dispatches on `name`, which is
    // model output. The tool list is fixed in code, so an unknown name cannot reach an
    // arbitrary function -- but the ARGUMENTS are model output too, which is why
    // ResolveProjectPath exists and refuses to escape the project. Every string that
    // becomes a path must go through it.
    //
    // The list in the comment above is the whole attack surface: anything added to it
    // is a new capability invocable without a human in the loop.
    // Exposes tool definitions (for the system prompt) and executes tool calls
    // made by the AI. Tools: read_file, write_file, edit_file, list_files,
    // memory_read, memory_append, list_entities, get_entity_info.
    class ToolServer
    {
    public:
        // The only public construction path. A singleton because the registry and the
        // callback slots are editor-global: one AI session, one project, one scene.
        static ToolServer &Instance();

        // The tool SCHEMA, in the JSON shape a chat completions API expects. If it is
        // serialised from the same table ExecuteTool dispatches on, advertised and
        // implemented capability cannot drift; maintained separately, a tool can be
        // advertised without being implemented.
        // JSON array of tool definitions, injected into the system prompt.
        std::string GetToolDefinitionsJson() const;

        // Execute a tool by name with a JSON arguments object.
        // Executes BY NAME. The dispatcher is a chain of string comparisons in the .cpp,
        // so the set of callable tools is exactly the set of names handled there -- that
        // list is the security boundary, and nothing else guards it.
        //
        // Returns a string because the value goes straight back into the conversation as a
        // tool message. An error is indistinguishable from a result unless worded as one:
        // a convention, not a type.
        std::string ExecuteTool(const std::string &name, const std::string &argsJson);

        // Memory file location.
        // Static: the memory file location is a property of the active project, not of
        // the singleton, so it needs no instance to answer.
        static std::filesystem::path GetMemoryPath();

        // Callback when a file is written (for auto-opening in editor).
        // A std::function so the panel can capture `this`.
        // DEFECT (unowned): the slot holds whatever it is given and ToolServer never
        // clears it. If the registering panel dies first, the next file write calls
        // through a dangling capture. Set-once at startup keeps this safe today; nothing
        // in the type prevents a later re-registration from a closed panel. Not fixed.
        using FileWrittenCallback = std::function<void(const std::string &path)>;
        void SetFileWrittenCallback(FileWrittenCallback cb);

        // Callback fired after a tool creates or switches the active
        // project, so the editor can re-root its panels.
        // A tool can CREATE or OPEN a project, which invalidates every panel's notion
        // of where the root is. Firing this lets panels re-root rather than poll.
        using ProjectChangedCallback = std::function<void()>;
        void SetProjectChangedCallback(ProjectChangedCallback cb);

        // Scene access for entity inspection tools.
        // The scene pointer for the entity tools. DEFECT (dangling): a raw pointer with
        // no lifetime coupling to the server. A tool running after the scene is torn down
        // -- an in-flight model response is exactly that -- dereferences a destroyed
        // Scene. SetScene(nullptr) on shutdown would close it; nothing here requires it.
        void SetScene(Scene *scene);

    private:
        // A defaulted private constructor: Instance() is the only way in. No copy or
        // move is deleted because nothing that cannot call the ctor can copy the class.
        ToolServer() = default;

        // The security-relevant function. A tool-supplied path is model output, so it
        // must not name a file outside the project: "../../.ssh/id_rsa" and an absolute
        // path are both reachable if the check is a string prefix test.
        //
        // Empty path plus an error string rather than throwing or a sentinel, so a caller
        // cannot treat a rejected path as valid -- an empty path is falsy where used.
        // Resolves a tool-supplied path inside the active project, refusing
        // anything that escapes it. Fills error and returns an empty path on
        // rejection.
        // error_code-based rather than throwing, so a bad path yields a message the
        // model can read instead of unwinding through the tool dispatcher.
        static std::filesystem::path ResolveProjectPath(const std::string &path, std::string &error);

        // Project tools
        // Separated from the file tools by comment only. There is NO privilege boundary:
        // the model reaches all of them through ExecuteTool, and SetStartScene changes
        // what the project is.
        std::string GetProjectInfo();
        std::string CreateProject(const std::string &name);
        std::string OpenProject(const std::string &path);
        std::string SetStartScene(const std::string &path);

        // File tools
        // The dangerous capability in this class: the model can MODIFY the project.
        // DEFECT (no confirmation hook): FileWrittenCallback fires AFTER the write, so
        // there is no hook a panel could use to ask the user first. If a confirmation step
        // is wanted, it has to be added here. Not fixed; comments only.
        std::string ReadFile(const std::string &path);
        std::string WriteFile(const std::string &path, const std::string &content);
        std::string EditFile(const std::string &path, const std::string &oldText, const std::string &newText);
        // Both old and new text, so the caller must supply an exact substring. A
        // deliberate constraint rather than a line number: the model read the file, so it
        // can quote what it saw, and a non-matching edit fails loudly instead of
        // corrupting a different region.
        std::string ListFiles(const std::string &dir);

        // Memory tools
        // One file whose path GetMemoryPath() owns, so the model never supplies a path
        // -- which is why these skip the ResolveProjectPath check.
        std::string ReadMemory();
        std::string AppendMemory(const std::string &text);

        // Entity inspection tools
        std::string ListEntities();
        // Read-only by design: these expose state, and nothing here lets the model
        // MUTATE an entity -- which keeps the scene from being a write target for
        // arbitrary model output.
        std::string GetEntityInfo(const std::string &id);
        std::string GetEntityScriptState(const std::string &id);
        std::string GetComponentInfo(const std::string &id, const std::string &componentType);

        // Both callback slots, empty by default, so every call site must test before
        // invoking: an unset slot throws std::bad_function_call from inside a tool
        // handler. Nothing in this header enforces the test.
        FileWrittenCallback m_FileWrittenCallback;
        ProjectChangedCallback m_ProjectChangedCallback;
        // DEFECT (typo, harmless): the member is m_Screen, not m_Scene -- a one-character
        // slip that compiles because it is only assigned in the .cpp. Noted so a reader
        // does not hunt for the mismatch. Not fixed; comments only.
        Scene *m_Screen = nullptr;
    };

}
