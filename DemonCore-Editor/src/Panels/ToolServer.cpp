// First by convention: the platform and GL set every editor translation unit
// needs before anything else is parsed.
#include "wlpch.h"
// The tool surface itself. Declares thirteen tools in the JSON schema built
// below and dispatches them in ExecuteTool; it owns no editor state beyond the
// two callbacks and the scene pointer.
#include "ToolServer.h"
// Argument extraction. A tool's arguments arrive as a raw JSON string from the
// model, and every handler pulls its own fields out of it.
#include "JsonMini.h"
// The entity view used by the three scene tools.
#include "Wasteland/Scene/Scene.h"
// Entity is a handle wrapper; constructing one per view entry is how components
// are reached below.
#include "Wasteland/Scene/Entity.h"
// Every component type this file reports on: ID, Tag, Transform, Script,
// Camera, SpriteRenderer, Rigidbody2D, BoxCollider2D.
#include "Wasteland/Scene/Components.h"
// Only for GetInstance: the live py::object holding a running script's
// instance, which get_script_state reads attributes out of.
#include "Wasteland/Scripting/ScriptEngine.h"
// Project is the single source of truth for the open project, its root, and the
// path containment test every file tool depends on.
#include "Wasteland/Project/Project.h"
#include "../AI/MemoryStore.h"
// Graph-file tools use the same Graph loaded by GraphSerializer and validated by
// Validator; Graph must precede GraphSerializer's forward declaration.
#include "Wasteland/VisualBlocks/Graph.h"
#include "Wasteland/VisualBlocks/GraphSerializer.h"
#include "Wasteland/VisualBlocks/NodeRegistry.h"
#include "Wasteland/VisualBlocks/Validator.h"

// HttpClient is the transport for the web_search tool. WinHTTP-backed and already
// part of the editor, so search adds no dependency; UrlEncode is reused rather than
// writing a second encoder for the query string.
#include "HttpClient.h"

// py::exec and py::eval live in eval.h, which pybind11.h does NOT pull in. The
// python_exec tool needs exec; without this include the only way to run a string
// would be the raw C API.
#include <pybind11/eval.h>

// py::object, py::list and error_already_set for the script-state tool. The
// interpreter is already running by the time a tool call arrives.
#include <pybind11/pybind11.h>

// Every file tool reads and writes through ifstream/ofstream rather than through
// the project or asset system, because these tools are meant to see the files on
// disk exactly as they are.
#include <fstream>
// The rdbuf slurp: the read tools return whole file contents as one string.
#include <sstream>
// Standard string helpers.
#include <algorithm>
// tolower for the case-insensitive fallback in symbol lookup, and isspace for
// stripping a line's trailing whitespace in search output.
#include <cctype>
// The directory walk in the codebase tools accumulates directory names in a set
// so the engine tree is emitted in sorted order without a second sort pass.
#include <set>
// strtol, for the percent-decoding of DuckDuckGo's redirect URLs, and strchr for
// the base64 alphabet lookup in the Bing redirect unwrapper.
#include <cstdlib>
#include <cstring>

// Short alias, so the script-state code below reads as py:: rather than
// pybind11:: on every line.
namespace py = pybind11;

// Engine-side namespace even though this file lives in the editor: the tool
// server is reusable by any host that wants to expose the same tools.
namespace Wasteland
{

    // The singleton. Its constructor is private and defaulted, so this is the only
    // way to reach it, which is what makes the callbacks single-writer.
    ToolServer &ToolServer::Instance()
    {
        // Function-local static: initialised on first use, and thread-safe to first
        // touch under C++11. DEFECT: the two callbacks and m_Screen below have no
        // synchronisation at all. They are written by the editor thread and read by
        // whichever thread the AI chat loop dispatches a tool call on, so installing a
        // callback or swapping a scene mid-call is a data race, not merely a stale
        // value.
        static ToolServer s;
        // The same object for the whole process, which is why the memory file and the
        // open-project identity are consistent from one tool call to the next.
        return s;
    }

    // Where persistent AI memory lives. Static because it is a property of the
    // engine install rather than of any project or window.
    std::filesystem::path ToolServer::GetMemoryPath()
    {
        // Engine-level state, so it lives with the user rather than inside
        // any one project.
        // %APPDATA%/Wasteland, or ~/.config/Wasteland. Deliberately NOT inside the
        // project: memory and preferences should survive switching projects.
        return Project::GetUserConfigDirectory() / "ai_memory.txt";
    }

    // Resolves a tool-supplied path inside the active project and refuses
    // anything that escapes it. The AI is told paths are "relative to the
    // project root", but a model will still occasionally send an absolute
    // path or a "../" escape; without this check, write_file could rewrite
    // anything the process can reach.
    // The single choke point every file tool goes through. The model is told paths
    // are project-relative and it does occasionally send an absolute path or a
    // parent-directory escape; without this, write_file could rewrite anything the
    // process can reach.
    std::filesystem::path ToolServer::ResolveProjectPath(const std::string &path, std::string &error)
    {
        // One reference for the whole function: the project cannot change underneath a
        // single tool call.
        Project &project = Project::Get();

        // Parsed from the raw string. Nothing is normalised here; deciding whether the
        // result is usable is the job of the containment test below.
        std::filesystem::path candidate(path);
        // An absolute path from the model is taken at face value and then subjected to
        // the same containment test as a relative one. That is the important part: the
        // check is not skipped for absolute paths, it is simply applied to them too.
        std::filesystem::path resolved = candidate.is_absolute()
                                             ? candidate
                                             // Relative paths join the project root WITHOUT the implied assets/ prefix, so a
                                             // model asking for assets/scenes/x.wastescene and one asking for
                                             // scenes/x.wastescene both resolve and an asset path is not doubled up.
                                             : project.GetProjectPath(path);

        // The non-throwing form. Every filesystem call in this function uses it, so a
        // bad path comes back as an error string instead of an exception escaping into
        // the tool dispatcher.
        std::error_code ec;
        // Made absolute WITHOUT resolving symlinks or normalising dot segments, and the
        // name says absolute, not canonical. That is sufficient: the containment test
        // below delegates normalisation to Project::MakeProjectRelative, which uses
        // std::filesystem::relative and therefore collapses the parent-directory
        // segments itself.
        const std::filesystem::path abs = std::filesystem::absolute(resolved, ec);
        // absolute() failed, and it returns its input unmodified in that case, so the
        // value is not safe to use and the call is refused.
        if (ec)
        {
            // The offending path is echoed back so the model can see exactly what was
            // rejected rather than having to guess which of its arguments was at fault.
            error = "cannot resolve path '" + path + "'";
            // An empty path is the universal 'refused' signal; every caller tests
            // .empty() first.
            return {};
        }

        // MakeProjectRelative yields empty for paths that climb out of the
        // project, which is precisely the containment test needed here.
        // No project open. Checked here even though MakeProjectRelative also returns
        // empty for an empty root, because the caller deserves the specific message
        // rather than being told its path was outside the project.
        if (project.GetRoot().empty())
        {
            // The wording names the fix: open or create a project first.
            error = "no project is open; refusing file access";
            return {};
        }

        // The containment test itself. Project::MakeProjectRelative returns an empty
        // string for anything that cannot be expressed as a project-relative path, which
        // per Project.cpp includes any path whose first relative component is a parent
        // reference. Reusing that function means the containment rule and the path format
        // the model is told to use cannot drift apart.
        if (project.MakeProjectRelative(abs).empty())
        {
            // Explicit rather than a generic failure: a model that keeps sending absolute
            // paths learns the rule from this message.
            error = "path '" + path + "' is outside the project";
            return {};
        }

        // The resolved ABSOLUTE path, not the relative form. Every caller hands this
        // straight to an ifstream or ofstream; the ones that need relative text for the
        // editor callback call MakeProjectRelative again.
        return abs;
    }

    // Installed by the editor so a tool-written file can be opened or re-imported
    // without the tool server knowing anything about the editor.
    void ToolServer::SetFileWrittenCallback(FileWrittenCallback cb)
    {
        // Unconditional overwrite. A null callback disables the hook, which is how the
        // tests use this class.
        m_FileWrittenCallback = cb;
    }

    // Installed so panels can re-root themselves after create_project or
    // open_project.
    void ToolServer::SetProjectChangedCallback(ProjectChangedCallback cb)
    {
        // Same overwrite semantics as the file callback.
        m_ProjectChangedCallback = cb;
    }

    // The scene the entity tools inspect. Stored raw, not owned.
    void ToolServer::SetScene(Scene *scene)
    {
        // DEFECT: a raw pointer with no notification when the scene is destroyed. The
        // entity tools check only for null, so that guards against 'never set', not
        // against 'no longer valid': a pointer to a closed scene is dereferenced rather
        // than reported.
        m_Screen = scene;
    }

    // A prose summary of the active project for the system prompt. Deliberately
    // text rather than JSON: this is read by the model, not parsed by it.
    std::string ToolServer::GetProjectInfo()
    {
        Project &project = Project::Get();
        // The no-project answer is still information: it names the asset root and says
        // which tool to call next.
        if (!project.IsOpen())
        {
            // Built by concatenation rather than a stream, so no iostream include has to be
            // justified here.
            return "No project is open. Asset root: " + project.GetAssetRoot().string() +
                   "\nUse create_project to make one.";
        }

        // Assembled as one growing string; every line ends in an explicit newline so
        // the model sees a clean list.
        std::string result = "Project: " + project.GetName() + "\n";
        // The root is the anchor for every path the model is subsequently told is
        // relative, which is what makes its guesses checkable.
        result += "Root: " + project.GetRoot().string() + "\n";
        // Read from the project settings rather than a compiled-in constant, so the
        // model learns the version of the file it is editing.
        result += "Engine version: " + project.GetSettings().EngineVersion + "\n";
        // Optional: a project may legitimately have no start scene yet.
        if (!project.GetSettings().StartScene.empty())
            // Shown only when set, rather than as an empty field the model might mistake
            // for a real path.
            result += "Start scene: " + project.GetSettings().StartScene + "\n";

        // Count the asset tree so the model has a sense of project size
        // without having to list every directory.
        // Counts rather than a listing: the model needs a sense of project size, not an
        // inventory of every asset.
        size_t files = 0, directories = 0;
        // One error_code shared by the iterator construction, the increment and both
        // the is_directory and is_regular_file probes below. It works because each call
        // overwrites it, but it means a failing stat and a failing increment are
        // indistinguishable by the time the check at the top of the loop runs.
        std::error_code ec;
        // Walked with an explicit error_code so an unreadable subdirectory ends the
        // traversal instead of throwing. Symlinks are not followed, so a link cycle
        // cannot hang the tool call.
        for (auto it = std::filesystem::recursive_directory_iterator(project.GetAssetRoot(), ec);
             // Compared against a default-constructed end iterator, which is also the
             // terminating state after a failed construction.
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            // Checked at the TOP of the body rather than in the loop condition, because
            // increment()'s error code has nowhere else to be examined.
            if (ec)
                // A partial count beats no answer at all: whatever was counted before the
                // failure is reported, with no indication that it is partial.
                break;
            // Counted separately from files, because the directory count is a better
            // signal of project shape than the file count alone.
            if (it->is_directory(ec))
                // One per directory entry visited, including empty ones.
                ++directories;
            // Regular files only. Symlinks and devices are counted as none of the above and
            // contribute to neither total.
            else if (it->is_regular_file(ec))
                // The other half of the same tally.
                ++files;
        }
        // Sizes are the point of this line; the plural is handled by spelling out the
        // suffix rather than by any real pluralisation.
        result += "Assets: " + std::to_string(files) + " file(s) in " +
                  // Second half of that line.
                  std::to_string(directories) + " director(ies)\n";

        // Copied by value: the list is small and the singleton's getter may hand back a
        // reference that a project-changed callback could invalidate.
        const std::vector<std::string> recents = Project::GetRecentProjects();
        // Optional, since a first run has no recents.
        if (!recents.empty())
        {
            // A header, then one indented line per entry.
            result += "Recent projects:\n";
            // Absolute paths, so the model can pass one straight to open_project.
            for (const std::string &r : recents)
                // Two-space indent, which distinguishes the list from the fields above it.
                result += "  " + r + "\n";
        }

        // Free-form text, never an error string: the caller cannot tell a successful
        // report from a failure by prefix, only by reading the content.
        return result;
    }

    // Creates AND opens. There is no create-without-open, so a model cannot leave
    // the editor with a project on disk that nothing is rooted to.
    std::string ToolServer::CreateProject(const std::string &name)
    {
        // Refuses rather than closing the current project. Silently swapping the open
        // project out from under open editors and unsaved scenes would be far worse.
        if (Project::Get().IsOpen())
            // Names the open project so the model can close or switch it instead of
            // repeating the same call.
            return "Error: a project is already open ('" + Project::Get().GetName() +
                   // Second half of that message, including the instruction.
                   "'). Close or switch it before creating another.";

        // Project::Create writes its own message here, so this tool adds no wording of
        // its own beyond the prefix below.
        std::string error;
        // Seed from the engine's own assets so the project starts with the
        // built-in shaders the renderer needs.
        // Seeded from the current asset root, which is the engine's own assets
        // directory when no project is open. That is what gives a new project the
        // built-in shaders the renderer needs instead of an empty tree.
        if (!Project::Create(name, Project::Get().GetAssetRoot(), &error))
            // Project::Create's own wording, passed through unchanged.
            return "Error: " + error;

        // Fired on success only. The editor uses it to re-root its panels onto the new
        // project.
        if (m_ProjectChangedCallback)
            // Runs synchronously inside the tool call, on whichever thread dispatched it.
            // See the note on Instance() about the unsynchronised state it touches.
            m_ProjectChangedCallback();

        // Reports the resolved name and root, so the model does not have to follow up
        // with get_project_info just to learn where the project landed.
        return "Created and opened project '" + Project::Get().GetName() + "' at " +
               // Second half of that message.
               Project::Get().GetRoot().string();
    }

    // Opens an existing project by its project.wasteland file or by its folder.
    std::string ToolServer::OpenProject(const std::string &path)
    {
        std::string error;
        // No path containment here, unlike the file tools: naming an absolute path
        // outside the current project is exactly how a project is switched.
        if (!Project::Get().Open(path, &error))
            // Project::Open's own wording.
            return "Error: " + error;

        // The same callback as create_project, for the same reason.
        if (m_ProjectChangedCallback)
            // Fires after a successful open only.
            m_ProjectChangedCallback();

        // Same shape as the create_project confirmation, so the model sees a consistent
        // pattern for project results.
        return "Opened project '" + Project::Get().GetName() + "' at " +
               // Second half of that message.
               Project::Get().GetRoot().string();
    }

    // Points the project at the scene the editor opens next time, and persists it.
    std::string ToolServer::SetStartScene(const std::string &path)
    {
        // Needs a project: the setting lives in project.wasteland.
        if (!Project::Get().IsOpen())
            // The one tool here whose message is not prefixed with the offending path,
            // because there is no path to report.
            return "Error: no project is open";

        // Reused by ResolveProjectPath below.
        std::string error;
        // Contained like every other path: the start scene has to live inside the
        // project being configured.
        const std::filesystem::path resolved = ResolveProjectPath(path, error);
        // Rejected by the containment test.
        if (resolved.empty())
            // The reason, verbatim from ResolveProjectPath.
            return "Error: " + error;

        // Must exist and be a plain file. DEFECT: the throwing overload is used here,
        // with no try/catch on this path, so a filesystem-level failure escapes rather
        // than coming back as an error string -- unlike every call inside
        // ResolveProjectPath, which all pass an error_code.
        if (!std::filesystem::is_regular_file(resolved))
            // The message for the ordinary case, where the file simply is not there.
            return "Error: '" + path + "' is not a file";

        // A reference into the live settings, so the mutation below needs no setter.
        Project::Settings &settings = Project::Get().GetSettings();
        // Stored RELATIVE, which is correct for portability. DEFECT: the live settings
        // are overwritten before the save is attempted, so if Save fails the running
        // editor will open the new scene while the file on disk still names the old one
        // -- and the error returned below does not mention the in-memory half of that.
        settings.StartScene = Project::Get().MakeProjectRelative(resolved);

        // Persists the whole settings block, not just this field.
        if (!Project::Get().Save())
            // Names the file that failed, but not the fact that the in-memory setting has
            // already been changed.
            return "Error: could not write project.wasteland";

        // Read back from settings rather than from the resolved path, so the
        // confirmation is in the same project-relative form that was stored.
        return "Start scene set to '" + settings.StartScene + "' and saved.";
    }

    // The JSON schema array injected into the system prompt. Hand-built by string
    // concatenation rather than serialised from a table, so the exact wording the
    // model sees is visible in this function and can be tuned without any schema
    // machinery. The cost of that choice is the defect noted below.
    std::string ToolServer::GetToolDefinitionsJson() const
    {
        // The one piece of prose that depends on editor state, computed once and
        // interpolated into the file tools that need it.
        const std::string rootNote = Project::Get().IsOpen()
                                         // With a project open the model is told the concrete root, which is what makes
                                         // the paths it invents checkable rather than guessable.
                                         ? ("Paths are relative to the project root: " + Project::Get().GetRoot().string() + ". Paths outside the project are rejected.")
                                         : "No project is open. Call create_project first; file tools will fail until then.";

        // Opened here and closed below. No trailing comma is emitted anywhere because
        // every entry carries its own structure and the last one is closed first.
        std::string json = "[";
        // Fifteen tools are declared in this function and fifteen are dispatched in
        // ExecuteTool. The two lists are kept in step BY HAND: nothing compares them, so
        // a tool added to one and not the other is either invisible to the model or
        // reachable only by guessing its name. Adding a sixteenth means editing both
        // lists and remembering there is no test.
        json += "{\"name\":\"get_project_info\",\"description\":\"Get the active project's name, root path, start scene, and asset counts.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        // create_project. Its description tells the model to use it when no project is
        // open, which is exactly the condition CreateProject's guard enforces.
        json += "{\"name\":\"create_project\",\"description\":\"Create a new project in the user's Documents folder and open it. Use when no project is open.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Project name; illegal path characters are stripped\"}},\"required\":[\"name\"]}},";
        // open_project, documented as taking either the project.wasteland path or its
        // folder, matching what Project::Open accepts.
        json += "{\"name\":\"open_project\",\"description\":\"Open an existing project by its project.wasteland path or its folder.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path to project.wasteland or its folder\"}},\"required\":[\"path\"]}},";
        // set_start_scene, described as a project-relative .wastescene path. Note the
        // handler does not enforce the .wastescene extension, so the description is
        // narrower than the behaviour.
        json += "{\"name\":\"set_start_scene\",\"description\":\"Set the scene the editor opens when this project loads, and save it to project.wasteland.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Project-relative path to a .wastescene file\"}},\"required\":[\"path\"]}},";
        // The first contained file tool. Every file tool's description ends with
        // rootNote, so the containment rule is repeated to the model at each point where
        // it could be tempted to ignore it.
        json += "{\"name\":\"read_file\",\"description\":\"Read the full contents of a file. " + rootNote + " Engine source files are read-only and addressed repo-relative: prefix the path with Wasteland/ or DemonCore-Editor/ (for example Wasteland/src/Wasteland/Scene/Scene.h).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\", \"description\":\"Path relative to the project root, or a repo-relative engine path starting with Wasteland/ or DemonCore-Editor/\"}},\"required\":[\"path\"]}},";
        // write_file is the tool that makes the rule load-bearing: it is the one that can
        // destroy work, so its description spells out that paths outside the project are
        // rejected rather than merely discouraged.
        json += "{\"name\":\"write_file\",\"description\":\"Write content to a file, creating or overwriting. " + rootNote + "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path relative to the project root\"},\"content\":{\"type\":\"string\",\"description\":\"The full file content to write\"}},\"required\":[\"path\",\"content\"]}},";
        // edit_file, an exact unique string replacement, which is why the handler
        // refuses a non-unique match rather than patching the first occurrence.
        json += "{\"name\":\"edit_file\",\"description\":\"Replace exact text in a file. " + rootNote + "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path relative to the project root\"},\"old_text\":{\"type\":\"string\",\"description\":\"The exact text to find and replace (must be unique)\"},\"new_text\":{\"type\":\"string\",\"description\":\"The replacement text\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}},";
        // DEFECT: this entry marks dir as required, while the dispatch branch defaults it
        // to the asset root when it is absent, and ListFiles applies that same default a
        // second time. A model that follows the schema and always sends dir is never
        // harmed, but the schema describes a stricter contract than the handler has.
        json += "{\"name\":\"list_files\",\"description\":\"List files in a directory recursively, as project-relative paths. " + rootNote + "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\",\"description\":\"Project-relative directory (default: assets)\"}},\"required\":[\"dir\"]}},";
        // Graph tools are read-only and stricter than the ordinary file tools:
        // the path must resolve inside this project's assets tree and have a
        // graph extension. ResolveGraphAssetPath repeats those checks in code.
        json += "{\"name\":\"validate_graph\",\"description\":\"Load a .wlblocks or .wlbtree from the active project's assets tree and return the real validator report, including every finding unchanged. The resolved path must remain under this project's assets root; project-relative paths under assets/ are preferred. " + rootNote + "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to a .wlblocks or .wlbtree under the active project's assets root (project-relative preferred)\"}},\"required\":[\"path\"]}},";
        json += "{\"name\":\"list_graph_nodes\",\"description\":\"Load a .wlblocks or .wlbtree from the active project's assets tree and list its node instance ids, type ids, ordered tree children, and registry-declared pin ids/types/directions. The resolved path must remain under this project's assets root; project-relative paths under assets/ are preferred. " + rootNote + "\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to a .wlblocks or .wlbtree under the active project's assets root (project-relative preferred)\"}},\"required\":[\"path\"]}},";
        // memory_read, documented as holding lessons and user preferences. No path
        // argument: the location is fixed in GetMemoryPath, so the model cannot move it.
        json += "{\"name\":\"memory_read\",\"description\":\"Read the AI persistent memory. Contains lessons and user preferences.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        // memory_append, the only write that is NOT contained in the project: it goes to
        // the engine-level memory file, which no path check applies to.
        json += "{\"name\":\"memory_append\",\"description\":\"Append a note to persistent memory for future sessions.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The note to append to memory\"}},\"required\":[\"text\"]}},";
        json += "{\"name\":\"memory_search\",\"description\":\"Search persistent memory for a query string.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}},";
        json += "{\"name\":\"memory_record_tool\",\"description\":\"Record tool usage telemetry to memory.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"tool\":{\"type\":\"string\",\"description\":\"Tool name\"},\"status\":{\"type\":\"string\",\"description\":\"Tool status\"},\"duration_ms\":{\"type\":\"integer\",\"description\":\"Duration in milliseconds\"}},\"required\":[\"tool\"]}},";
        json += "{\"name\":\"memory_forget\",\"description\":\"Delete a memory entry by key.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory entry key\"}},\"required\":[\"key\"]}},";
        // DEFECT, and a functional one rather than a documentation one: the handler
        // views the scene as IDComponent AND TagComponent, so an entity with no tag is
        // not merely unnamed here, it is absent. In a scene where nothing is tagged this
        // reports an empty scene while the scene is full.
        json += "{\"name\":\"list_entities\",\"description\":\"List all entities in the current scene with UUID, name, and components.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        // get_entity_info. Its id is documented as the entity UUID from list_entities,
        // which is the coupling that makes the missing untagged entities unrecoverable:
        // an id the model never saw listed cannot be asked about.
        json += "{\"name\":\"get_entity_info\",\"description\":\"Get detailed info about an entity: transform, tag, and all components.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"The entity UUID from list_entities\"}},\"required\":[\"id\"]}},";
        // get_script_state, the only tool that reads live interpreter state and
        // therefore the only one that can raise rather than return an error string.
        json += "{\"name\":\"get_script_state\", \"description\":\"Get the live state of a script instance variables.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\", \"description\":\"The entity UUID\"}},\"required\":[\"id\"]}},";
        // get_component_info. The per-component inspector the header has declared
        // since the capability flags were written; it complements get_entity_info,
        // which reports the components it happens to know about and omits the rest.
        json += "{\"name\":\"get_component_info\", \"description\":\"Get one component's fields for an entity. Use after get_entity_info to inspect a component that get_entity_info does not report (SpriteRenderer, Rigidbody2D, BoxCollider2D, Camera, Script).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\", \"description\":\"The entity UUID from list_entities\"},\"component\":{\"type\":\"string\", \"description\":\"Component type name, e.g. Transform, SpriteRenderer, Rigidbody2D, BoxCollider2D, Camera, Script\"}},\"required\":[\"id\",\"component\"]}},";
        // The codebase tools. They read the engine source tree, which is why they
        // resolve against the repo root rather than the open project; a model asked
        // to explain the renderer has to be able to open Renderer3D.cpp.
        json += "{\"name\":\"search_files\", \"description\":\"Search file contents for a literal string across the engine source tree (or a subdirectory of it). Returns matching lines as path:line: text. Paths in results are repo-relative and can be fed straight to read_file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\", \"description\":\"Literal text to find (case-sensitive)\"},\"dir\":{\"type\":\"string\", \"description\":\"Optional directory to search: repo-relative like Wasteland/src, DemonCore-Editor/src, or a project-relative path. Defaults to the whole repo\"}},\"required\":[\"query\"]}},";
        json += "{\"name\":\"list_dir\", \"description\":\"List the entries of a directory. Accepts a repo-relative path (Wasteland/src, DemonCore-Editor/src/Panels) or a project-relative path. Non-recursive.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\", \"description\":\"Directory to list, e.g. Wasteland/src/Wasteland/Scene\"}},\"required\":[\"dir\"]}},";
        json += "{\"name\":\"get_symbol_info\", \"description\":\"Find where a C++ symbol (class, struct, function, method) is declared or defined. Returns the file, line number and the declaration text.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\", \"description\":\"Symbol name, e.g. ScriptEngine, TransformComponent, ResolveProjectPath\"}},\"required\":[\"symbol\"]}},";
        json += "{\"name\":\"get_engine_tree\", \"description\":\"Print the engine source tree: every directory under Wasteland/src and DemonCore-Editor/src with its file count. Use to discover where a subsystem lives before reading it.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}},";
        // python_exec. Runs in the embedded interpreter with stdout/stderr captured.
        json += "{\"name\":\"python_exec\", \"description\":\"Execute Python code in the engine's embedded interpreter and return its stdout and stderr. Use for arithmetic, formatting, data transformation and inspecting the Python side of the engine. The code runs with the engine modules already imported.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\", \"description\":\"Python source to execute\"}},\"required\":[\"code\"]}},";
        // web_search. The last entry: no trailing comma after it.
        json += "{\"name\":\"web_search\", \"description\":\"Search the web and return the top results as markdown (title, URL, snippet). Use for anything that needs current information: release dates, API changes, documentation. No API key required.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\", \"description\":\"Search query\"},\"max_results\":{\"type\":\"integer\", \"description\":\"Maximum number of results to return (default 8, max 20)\"}},\"required\":[\"query\"]}}";
        // Closes the array. There is no trailing-comma problem because the last entry
        // was closed by its own literal.
        json += "]";
        // The assembled schema, handed to the AI layer for inclusion in the prompt.
        return json;
    }

    // The single entry point the AI layer calls. Name dispatch is a flat else-if
    // chain rather than a lookup table, which keeps each tool's argument names next
    // to the handler that consumes them.
    //
    // DEFECT: nothing here catches an exception. Two handlers call the THROWING
    // std::stoull on a model-supplied string, one calls the throwing
    // std::filesystem::create_directories, and SetStartScene calls a throwing
    // is_regular_file. A hallucinated id or an unwritable config directory therefore
    // escapes this function instead of coming back as an error string, while every
    // MISSING argument below is reported politely. That asymmetry is the defect.
    std::string ToolServer::ExecuteTool(const std::string &name, const std::string &argsJson)
    {
        // The one tool that takes no arguments at all.
        if (name == "get_project_info")
        {
            // Passed through unchanged; the result is prose, not JSON.
            return GetProjectInfo();
        }
        // Every tool from here on extracts its own arguments first, so a missing one is
        // reported as an error string rather than silently defaulted.
        else if (name == "create_project")
        {
            // Declared inside the branch rather than once at the top, so no handler can read
            // a value another branch left behind when one is added or removed.
            std::string projectName;
            // ExtractString returns false for a missing key AND for a null or non-string
            // value, leaving the out-parameter untouched. The two are indistinguishable to
            // the model here, which is acceptable because both mean the same thing: no
            // usable name was supplied.
            if (!JsonMini::ExtractString(argsJson, "name", projectName))
                // Names the argument that is missing, so the model can retry with it.
                return "Error: create_project requires 'name' argument";
            return CreateProject(projectName);
        }
        // The next project tool.
        else if (name == "open_project")
        {
            // A local again, for the same independence reason.
            std::string projectPath;
            // Required here even though open_project is the one path argument that is NOT
            // contained, because it still has to be present.
            if (!JsonMini::ExtractString(argsJson, "path", projectPath))
                // The missing-argument message.
                return "Error: open_project requires 'path' argument";
            return OpenProject(projectPath);
        }
        // Unlike open_project, this one IS contained by ResolveProjectPath inside
        // SetStartScene: a start scene has to live in the project being configured.
        else if (name == "set_start_scene")
        {
            // Extracted under the same argument name as open_project, which is worth noting
            // because the two tools accept quite different path spaces.
            std::string scenePath;
            // Same key, different containment downstream.
            if (!JsonMini::ExtractString(argsJson, "path", scenePath))
                // The missing-argument message.
                return "Error: set_start_scene requires 'path' argument";
            return SetStartScene(scenePath);
        }
        // First of the three contained file tools.
        else if (name == "read_file")
        {
            // The same variable name is reused by three sibling branches, each in its own
            // scope, so the branches stay independent of each other's locals.
            std::string path;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: read_file requires 'path' argument";
            return ReadFile(path);
        }
        // Two arguments, both mandatory.
        else if (name == "write_file")
        {
            // Two locals on one line, unlike the three-name form in the edit_file branch
            // below. Inconsistent, and only visible because the branches sit next to each
            // other.
            std::string path, content;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: write_file requires 'path' argument";
            // Checked separately from path so the model is told which of the two is absent.
            if (!JsonMini::ExtractString(argsJson, "content", content))
                // An empty content string is a legal request to empty the file; only an absent
                // or non-string value is refused here.
                return "Error: write_file requires 'content' argument";
            return WriteFile(path, content);
        }
        // Three mandatory strings.
        else if (name == "edit_file")
        {
            // All three on one line. An empty newText is meaningful, since it deletes the
            // match, so it is accepted; only a missing key is refused.
            std::string path, oldText, newText;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: edit_file requires 'path' argument";
            // The text to find. The snake_case key matches the schema property name exactly,
            // which matters because ExtractString matches keys literally.
            if (!JsonMini::ExtractString(argsJson, "old_text", oldText))
                // The missing-argument message.
                return "Error: edit_file requires 'old_text' argument";
            // The replacement text.
            if (!JsonMini::ExtractString(argsJson, "new_text", newText))
                // The missing-argument message.
                return "Error: edit_file requires 'new_text' argument";
            return EditFile(path, oldText, newText);
        }
        // The only branch with a default rather than a required argument.
        else if (name == "list_files")
        {
            // Pre-seeded, so the default survives an extraction that fails. The asset root
            // is what a model asking to see the project means.
            std::string dir = "assets";
            // DEFECT: the return value is discarded. Because ExtractString leaves the
            // out-parameter untouched on failure, a missing key and a non-string value (a
            // number, say) both leave dir as the asset root with no diagnostic at all.
            JsonMini::ExtractString(argsJson, "dir", dir);
            // So the model is shown a different directory than it asked for, and is not told
            // that its argument was ignored.
            return ListFiles(dir);
        }
        // Graph inspection stays under the project's asset root, and each handler
        // loads the real Graph model before it validates or lists registry pins.
        else if (name == "validate_graph")
        {
            std::string path;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: validate_graph requires 'path' argument";
            return ValidateGraph(path);
        }
        else if (name == "list_graph_nodes")
        {
            std::string path;
            if (!JsonMini::ExtractString(argsJson, "path", path))
                return "Error: list_graph_nodes requires 'path' argument";
            return ListGraphNodes(path);
        }
        // No arguments, and not project-scoped.
        else if (name == "memory_read")
        {
            // The engine-level memory file, readable with no project open.
            return ReadMemory();
        }
        // The write counterpart.
        else if (name == "memory_append")
        {
            // One argument: the note itself.
            std::string text;
            // Required, so an append with no text is refused rather than writing a bare
            // timestamp line into memory.
            if (!JsonMini::ExtractString(argsJson, "text", text))
                return "Error: memory_append requires 'text' argument";
            return AppendMemory(text);
        }
        else if (name == "memory_search")
        {
            std::string query;
            if (!JsonMini::ExtractString(argsJson, "query", query))
                return "Error: memory_search requires 'query' argument";
            return SearchMemory(query);
        }
        else if (name == "memory_record_tool")
        {
            std::string toolName;
            std::string status = "success";
            int64_t durationMs = 0;
            if (!JsonMini::ExtractString(argsJson, "tool", toolName))
                return "Error: memory_record_tool requires 'tool' argument";
            JsonMini::ExtractString(argsJson, "status", status);
            JsonMini::ExtractInt64(argsJson, "duration_ms", durationMs);
            return RecordToolTelemetry(toolName, status, (int)durationMs);
        }
        else if (name == "memory_forget")
        {
            std::string key;
            if (!JsonMini::ExtractString(argsJson, "key", key))
                return "Error: memory_forget requires 'key' argument";
            return ForgetMemory(key);
        }
        // No arguments, so nothing is extracted and the branch is a single call.
        else if (name == "list_entities")
        {
            return ListEntities();
        }
        // Takes the id printed by list_entities.
        else if (name == "get_entity_info")
        {
            // The raw string; the numeric conversion happens in the handler.
            std::string id;
            if (!JsonMini::ExtractString(argsJson, "id", id))
                return "Error: get_entity_info requires 'id' argument";
            return GetEntityInfo(id);
        }
        // The entity-detail tools reject a missing id up front, because neither has a
        // sensible default to fall back to.
        else if (name == "get_script_state")
        {
            std::string id;
            if (!JsonMini::ExtractString(argsJson, "id", id))
                return "Error: get_script_state requires 'id' argument";
            // The only call in this chain that can raise rather than return.
            return GetEntityScriptState(id);
        }
        // get_component_info takes a second required argument, which is what makes it
        // narrower than get_entity_info rather than a subset of it.
        else if (name == "get_component_info")
        {
            std::string id;
            std::string component;
            if (!JsonMini::ExtractString(argsJson, "id", id))
                return "Error: get_component_info requires 'id' argument";
            if (!JsonMini::ExtractString(argsJson, "component", component))
                return "Error: get_component_info requires 'component' argument";
            return GetComponentInfo(id, component);
        }
        // The codebase tools. search_files takes an optional dir, the other three a
        // required one, so the optional/required split is visible in the argument
        // handling rather than only in the schema.
        else if (name == "search_files")
        {
            std::string query;
            // Optional: an empty dir means the whole repository.
            std::string dir;
            if (!JsonMini::ExtractString(argsJson, "query", query))
                return "Error: search_files requires 'query' argument";
            JsonMini::ExtractString(argsJson, "dir", dir);
            return SearchFiles(query, dir);
        }
        else if (name == "list_dir")
        {
            std::string dir;
            if (!JsonMini::ExtractString(argsJson, "dir", dir))
                return "Error: list_dir requires 'dir' argument";
            return ListDir(dir);
        }
        else if (name == "get_symbol_info")
        {
            std::string symbol;
            if (!JsonMini::ExtractString(argsJson, "symbol", symbol))
                return "Error: get_symbol_info requires 'symbol' argument";
            return GetSymbolInfo(symbol);
        }
        // No arguments, so the branch is a single call.
        else if (name == "get_engine_tree")
        {
            return GetEngineTree();
        }
        else if (name == "python_exec")
        {
            std::string code;
            if (!JsonMini::ExtractString(argsJson, "code", code))
                return "Error: python_exec requires 'code' argument";
            return PythonExec(code);
        }
        else if (name == "web_search")
        {
            std::string query;
            if (!JsonMini::ExtractString(argsJson, "query", query))
                return "Error: web_search requires 'query' argument";
            // Optional, clamped in the handler rather than here: the clamp is a
            // property of the search, not of the argument's presence.
            int64_t maxResults = 8;
            JsonMini::ExtractInt64(argsJson, "max_results", maxResults);
            return WebSearch(query, (int)maxResults);
        }
        // Unknown names land here rather than throwing. This is also where a schema or
        // dispatch mismatch shows up, which is why the two hand-maintained lists in this
        // file have to be kept in step.
        return "Error: unknown tool '" + name + "'";
    }

    // Whole-file read. There is no range or offset form, so a model cannot page
    // through a large file: it either gets all of it or an error.
    std::string ToolServer::ReadFile(const std::string &path)
    {
        std::string error;
        // Contained first. Nothing is opened until the path has been proven to be
        // readable, so the refusal is a decision rather than a failed open.
        //
        // This is the one file tool that resolves engine paths as well as project
        // ones: read_file is how a model reads the engine's own source when it is
        // asked about the renderer or the scripting API, and no project root
        // contains those files. WriteFile and EditFile deliberately keep the
        // project-only resolver, so the widened read path is not a widened write path.
        const std::filesystem::path fullPath = ResolveProjectOrEnginePath(path, error);
        // The universal refusal signal.
        if (fullPath.empty())
            // The containment failure message, verbatim.
            return "Error: " + error;

        // Opened in text mode, so where the platform translates line endings a CRLF
        // file on disk comes back with LF. Binary would have been more faithful; the
        // model is editing source text, so the translation is mostly harmless.
        std::ifstream file(fullPath);
        // The ordinary failure: the path was legal but nothing is there.
        if (!file.is_open())
            // Reports the path AS SENT, not the resolved absolute one, which is the form the
            // model can act on.
            return "Error: cannot open file '" + path + "'";
        // The slurp buffer. There is no size limit anywhere in this file, so read_file on
        // a large file is a real allocation.
        std::stringstream ss;
        // Inserting the stream buffer is the cheapest whole-file read there is; a loop
        // over get() would be per character.
        ss << file.rdbuf();
        // The contents as-is. An empty file yields an empty string, which the model
        // cannot distinguish from a tool that returned nothing.
        return ss.str();
    }

    // Whole-file overwrite. The model sends the complete new content, so this is a
    // truncate-and-write rather than an edit.
    std::string ToolServer::WriteFile(const std::string &path, const std::string &content)
    {
        std::string error;
        const std::filesystem::path fullPath = ResolveProjectPath(path, error);
        if (fullPath.empty())
            return "Error: " + error;

        std::error_code ec;
        // Creates intermediate directories, so a model can write a path several levels
        // deep without creating each level in a separate call.
        std::filesystem::create_directories(fullPath.parent_path(), ec);

        // The error_code above is deliberately ignored: a failure there surfaces as this
        // open failing on the next line, which produces the more useful message anyway.
        std::ofstream file(fullPath, std::ios::trunc);
        // Refused before any content is written.
        if (!file.is_open())
            // Names the path as sent.
            return "Error: cannot write file '" + path + "'";
        // DEFECT: neither this write nor the close below is checked. A full disk, a
        // revoked permission or a path that became a directory mid-call all leave the
        // stream in a failed state, and the function still reports success at the end.
        file << content;
        // Closed explicitly so the flush happens before the callback fires; its result
        // is discarded, which is where a failed write would have surfaced.
        file.close();

        // The editor hook, fired unconditionally after the write attempt.
        if (m_FileWrittenCallback)
            // The editor receives a project-relative path, the form it stores and displays.
            // DEFECT: because the write was never verified, this also fires for a write that
            // failed, so a panel will try to open a file that does not exist.
            m_FileWrittenCallback(Project::Get().MakeProjectRelative(fullPath));

        // The byte count is the length of the model-supplied content, which is the right
        // number for the text that was requested and unrelated to what reached disk.
        return "Successfully wrote " + std::to_string(content.size()) + " bytes to '" + path + "'";
    }

    // Exact-string replacement: the tool an agent uses for surgical changes, where a
    // line-number edit would be wrong the moment anything above it moved.
    std::string ToolServer::EditFile(const std::string &path, const std::string &oldText, const std::string &newText)
    {
        std::string error;
        const std::filesystem::path fullPath = ResolveProjectPath(path, error);
        if (fullPath.empty())
            return "Error: " + error;

        // Read-modify-write in memory. The whole file is slurped, edited and written
        // back; for a file larger than memory that is a problem, but for the text files
        // an agent edits it is the only way to keep the replacement exact.
        std::ifstream inFile(fullPath);
        if (!inFile.is_open())
            // Distinguished from read_file's message so the model knows the file was
            // rejected as an edit target rather than as a read.
            return "Error: cannot open file '" + path + "' for editing";
        std::stringstream ss;
        // The same slurp idiom as ReadFile.
        ss << inFile.rdbuf();
        // Closed before the ofstream reopens the same path for truncation, which would
        // otherwise fail on Windows.
        inFile.close();

        // The whole file as one string; every offset below indexes into this.
        std::string content = ss.str();
        // The first occurrence.
        size_t pos = content.find(oldText);
        // No match at all is an error rather than a no-op, because a silent no-op looks
        // to the model exactly like a successful edit.
        if (pos == std::string::npos)
            // The model is told the text was not found rather than that nothing changed.
            return "Error: old_text not found in '" + path + "'";
        // Searches from just past the first match. DEFECT, minor: for an EMPTY oldText
        // this searches from index 0 again and finds the same position, so an empty
        // old_text is rejected below with 'appears multiple times' rather than with a
        // message about the argument being empty.
        size_t secondPos = content.find(oldText, pos + oldText.size());
        // This is what makes 'must be unique' in the schema real rather than
        // aspirational, and it also means overlapping occurrences count separately.
        if (secondPos != std::string::npos)
            // Refusing is the right call: picking the first would silently edit the wrong
            // place in a file the model believes it fully understands.
            return "Error: old_text appears multiple times in '" + path + "'";

        // A single replacement, in memory. Nothing has touched the disk yet at this
        // point, which is the last moment a failure is still harmless.
        content.replace(pos, oldText.size(), newText);

        // Truncating reopen. DEFECT: from here on the ORIGINAL CONTENT IS GONE, so a
        // write failure below destroys the file rather than leaving it as it was.
        std::ofstream outFile(fullPath, std::ios::trunc);
        // The one failure that is reported, and by this point the open attempt has
        // already truncated the file to zero.
        if (!outFile.is_open())
            // Wording differs from WriteFile's so the model can tell which operation
            // failed.
            return "Error: cannot write edited file '" + path + "'";
        // DEFECT: unchecked, exactly as in WriteFile, and worse here because the file
        // has already been truncated.
        outFile << content;
        // Discarded result; this is where a short write would have been detectable.
        outFile.close();

        if (m_FileWrittenCallback)
            // Same hook, same unverified-success caveat.
            m_FileWrittenCallback(Project::Get().MakeProjectRelative(fullPath));

        // No byte count and no diff, so the model cannot tell from the reply what
        // actually changed and has to read the file back to find out.
        return "Successfully edited '" + path + "'";
    }

    // Recursive listing as project-relative paths, one per line.
    std::string ToolServer::ListFiles(const std::string &dir)
    {
        // Default to the asset root so a bare list_files is useful without
        // the model having to know the internal layout.
        // The default is applied twice, here and in the dispatch branch above. The one
        // here is unreachable in practice, because the dispatcher never passes an empty
        // string: it pre-seeds dir with the same default.
        const std::string target = dir.empty() ? "assets" : dir;

        std::string error;
        // Contained like every other path tool, so the listing cannot walk out of the
        // project.
        const std::filesystem::path fullPath = ResolveProjectPath(target, error);
        if (fullPath.empty())
            return "Error: " + error;

        // Must be a directory. The throwing overload is used, as in SetStartScene, and a
        // filesystem-level failure here escapes rather than being reported.
        if (!std::filesystem::is_directory(fullPath))
            // Reports the target as sent.
            return "Error: '" + target + "' is not a directory";

        // Reported project-relative, so the model can feed a path straight
        // back into read_file or write_file.
        // Accumulated as one string, so a large project means one large allocation.
        std::string result;
        std::error_code ec;
        // The same non-throwing walk as GetProjectInfo, for the same reason.
        for (auto it = std::filesystem::recursive_directory_iterator(fullPath, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            // Checked at the top of the body, which is where increment()'s error code has to
            // be examined.
            if (ec)
                // A truncated listing is still more useful than none, so it is returned rather
                // than turned into an error -- though the model is not told it is truncated.
                break;
            // Files only: directories are not listed as entries, which keeps the output free
            // of a trailing slash per folder.
            if (!it->is_regular_file(ec))
                // Anything that is not a regular file, symlinks included, is skipped silently.
                continue;

            // Re-projected through MakeProjectRelative, which normalises separator style and
            // guarantees forward slashes, so the output matches the form the tools accept.
            const std::string rel = Project::Get().MakeProjectRelative(it->path());
            // Guards against a path that somehow cannot be expressed relative to the project
            // root, which would otherwise emit a bare blank line.
            if (!rel.empty())
                // Newline-delimited, no header and no count. The model parses this as text.
                result += rel + "\n";
        }
        // An empty directory is a valid answer, but an empty string is indistinguishable
        // from a tool that produced nothing at all.
        if (result.empty())
            // Spelled out in parentheses so it cannot be confused with an error.
            return "(empty directory)";
        return result;
    }

    // Resolve graph paths through the existing project-root check, then apply the
    // narrower assets-root boundary to the symlink-resolved target. Pass that
    // canonical target to GraphSerializer so the checked link path is not reopened.
    std::filesystem::path ToolServer::ResolveGraphAssetPath(const std::string &path,
                                                            std::string &error)
    {
        const std::filesystem::path projectPath = ResolveProjectPath(path, error);
        if (projectPath.empty())
            return {};

        std::error_code ec;
        const std::filesystem::path assetRoot =
            std::filesystem::weakly_canonical(Project::Get().GetAssetRoot(), ec);
        if (ec)
        {
            error = "cannot resolve the project's asset root";
            return {};
        }

        const std::filesystem::path canonicalPath =
            std::filesystem::weakly_canonical(projectPath, ec);
        if (ec)
        {
            error = "cannot resolve graph path '" + path + "'";
            return {};
        }

        const std::filesystem::path assetRelative =
            std::filesystem::relative(canonicalPath, assetRoot, ec);
        if (ec || assetRelative.empty() || assetRelative.is_absolute() ||
            assetRelative == ".")
        {
            error = "graph path '" + path + "' is outside the project's assets";
            return {};
        }
        for (const std::filesystem::path &part : assetRelative)
        {
            if (part == "..")
            {
                error = "graph path '" + path + "' is outside the project's assets";
                return {};
            }
        }

        const std::string extension = canonicalPath.extension().string();
        if (extension != ".wlblocks" && extension != ".wlbtree")
        {
            error = "graph path '" + path +
                    "' must name a .wlblocks or .wlbtree file";
            return {};
        }
        if (!std::filesystem::is_regular_file(canonicalPath, ec) || ec)
        {
            error = "graph path '" + path + "' is not a regular file";
            return {};
        }
        return canonicalPath;
    }

    // Load through GraphSerializer into the same Graph shape consumed by the
    // editor and Validator. The path is both project-confined and assets-confined
    // before any graph file is opened.
    bool ToolServer::LoadGraphAsset(const std::string &path,
                                    std::filesystem::path &fullPath,
                                    VisualBlocks::Graph &graph,
                                    std::string &error)
    {
        fullPath = ResolveGraphAssetPath(path, error);
        if (fullPath.empty())
            return false;

        try
        {
            VisualBlocks::GraphError loadError;
            if (!VisualBlocks::GraphSerializer::Load(fullPath.string(), graph, loadError))
            {
                error = loadError.Message;
                return false;
            }
        }
        catch (const std::exception &exception)
        {
            error = "graph load failed: ";
            error += exception.what();
            return false;
        }
        return true;
    }

    // Return Validator's own Report output. In particular, do not filter,
    // rewrite, or re-order Findings: each finding's Message is preserved by the
    // validator's Format/Report path.
    std::string ToolServer::ValidateGraph(const std::string &path)
    {
        VisualBlocks::Graph graph;
        std::filesystem::path fullPath;
        std::string error;
        if (!LoadGraphAsset(path, fullPath, graph, error))
            return "Error: " + error;

        VisualBlocks::ValidationResult validation;
        VisualBlocks::Validator::Validate(graph, validation);
        return validation.Report(Project::Get().MakeProjectRelative(fullPath));
    }

    // List graph instances alongside the actual registry signature. Registry::Find
    // resolves aliases, so legacy node ids report the pins the validator/emitter
    // actually use rather than an empty alias stub.
    std::string ToolServer::ListGraphNodes(const std::string &path)
    {
        VisualBlocks::Graph graph;
        std::filesystem::path fullPath;
        std::string error;
        if (!LoadGraphAsset(path, fullPath, graph, error))
            return "Error: " + error;

        std::ostringstream output;
        output << Project::Get().MakeProjectRelative(fullPath) << ": "
               << graph.Nodes.size() << " node(s)\n";
        VisualBlocks::NodeRegistry &registry = VisualBlocks::NodeRegistry::Get();
        for (const VisualBlocks::GraphNode &node : graph.Nodes)
        {
            output << "- id `" << node.Id << "` type `" << node.TypeId << "`";
            if (!node.Title.empty())
                output << " title `" << node.Title << "`";
            output << "\n";

            if (graph.Kind == "tree" && !node.ChildIds.empty())
            {
                output << "  children (priority order):";
                for (const std::string &childId : node.ChildIds)
                    output << " `" << childId << "`";
                output << "\n";
            }

            const VisualBlocks::NodeDef *definition = registry.Find(node.TypeId);
            if (definition == nullptr)
            {
                output << "  (node type is not registered)\n";
                continue;
            }
            for (const VisualBlocks::PinDef &pin : definition->Pins)
            {
                output << "  pin `" << pin.Id << "`: "
                       << (pin.Dir == VisualBlocks::PinDir::In ? "input" : "output")
                       << " " << VisualBlocks::PinTypeName(pin.Type);
                if (pin.Required)
                    output << " required";
                if (!pin.DefaultLiteral.empty())
                    output << " default `" << pin.DefaultLiteral << "`";
                output << "\n";
            }
        }
        return output.str();
    }

    // The persistent memory the editor's AI panel shows and the model reads back.
    std::string ToolServer::ReadMemory()
    {
        MemoryStore::Initialize();
        if (!MemoryStore::IsAvailable())
            return "(memory is empty)";

        auto entries = MemoryStore::ProjectRead("");
        if (entries.empty())
            return "(memory is empty)";

        std::string result = "## Persistent Memory\n\n";
        for (const auto &entry : entries)
        {
            char timeBuf[64] = {};
            tm timeInfo = {};
            localtime_s(&timeInfo, &entry.Timestamp);
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &timeInfo);
            result += "- [" + entry.Category + "] " + entry.Text + "\n";
        }
        return result;
    }

    // Append-only. There is no edit or delete tool for memory, so a wrong note can
    // only be corrected by editing the file by hand.
    std::string ToolServer::AppendMemory(const std::string &text)
    {
        MemoryStore::Initialize();
        if (!MemoryStore::IsAvailable())
            return "Error: memory is not available";

        MemoryStore::ProjectAppend(text, "lesson", "");
        return "Memory updated. Note saved.";
    }

    std::string ToolServer::SearchMemory(const std::string &query)
    {
        MemoryStore::Initialize();
        if (!MemoryStore::IsAvailable())
            return "(memory is empty)";

        std::vector<MemoryEntry> results;
        MemoryStore::ProjectSearch(query, results);
        if (results.empty())
            return "(no memory matches for: " + query + ")";

        std::string out = "## Memory Search Results\n\n";
        for (const auto &entry : results)
        {
            char timeBuf[64] = {};
            tm timeInfo = {};
            localtime_s(&timeInfo, &entry.Timestamp);
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &timeInfo);
            out += "- [" + entry.Category + "] " + entry.Text + "\n";
        }
        return out;
    }

    std::string ToolServer::RecordToolTelemetry(const std::string &toolName, const std::string &status, int durationMs)
    {
        MemoryStore::Initialize();
        if (!MemoryStore::IsAvailable())
            return "Error: memory is not available";

        std::string json = "{\"tool\":\"" + toolName + "\",\"status\":\"" + status + "\",\"duration_ms\":" + std::to_string(durationMs) + "}";
        MemoryStore::ProjectAppend(json, "tool_telemetry", toolName);
        return "Tool telemetry recorded.";
    }

    std::string ToolServer::ForgetMemory(const std::string &key)
    {
        MemoryStore::Initialize();
        if (!MemoryStore::IsAvailable())
            return "Error: memory is not available";

        MemoryStore::ProjectForget(key);
        return "Memory entry forgotten.";
    }

    std::string ToolServer::ListEntities()
    {
        if (!m_Screen)
            return "Error: no active scene";

        // DEFECT, and the most misleading failure in this file: an EnTT view yields only
        // entities holding EVERY listed component, so an entity with no TagComponent is
        // absent, not merely unnamed. A scene where nothing is tagged reports itself
        // empty while being full.
        auto view = m_Screen->GetAllEntitiesWith<IDComponent, TagComponent>();
        std::string result;
        // Linear over the view; the scene registry is small enough that this is not
        // worth an index.
        for (auto entityHandle : view)
        {
            // A lightweight handle wrapper per entry. It owns nothing: the scene keeps the
            // storage alive for the duration of the loop.
            Entity entity(entityHandle, m_Screen);
            // The entity id, rendered as text on the next line.
            auto &id = entity.GetComponent<IDComponent>();
            // Safe to dereference unconditionally, because the view guarantees the component
            // exists for every entity it yields.
            auto &tag = entity.GetComponent<TagComponent>();
            // The UUID is cast to its underlying 64-bit value and printed as a decimal
            // number. That printed form is the ONLY id get_entity_info and get_script_state
            // accept, so the model has to copy this exact text back.
            result += "UUID: " + std::to_string((uint64_t)id.ID) + " | Tag: " + tag.Tag + "\n";
        }
        // Same reasoning as ListFiles: an empty scene is spelled out rather than returned
        // as a string the model cannot interpret.
        if (result.empty())
            // DEFECT: this is returned both for a genuinely empty scene and for a full scene
            // whose entities have no TagComponent. The model has no way to tell the two.
            return "(scene has no entities)";
        return result;
    }

    // Full component dump for one entity, addressed by the id list_entities printed.
    std::string ToolServer::GetEntityInfo(const std::string &id)
    {
        if (!m_Screen)
            return "Error: no active scene";

        // DEFECT: the throwing overload, on a model-supplied string, with no try/catch
        // anywhere on this path. A hallucinated id or a quoted numeric string raises
        // std::invalid_argument out of ExecuteTool instead of returning an error the
        // model could act on; an id past 2^64 raises std::out_of_range the same way.
        uint64_t entityId = std::stoull(id);
        // Distinguishes 'the id was valid but names nothing' from 'here is the dump'.
        bool found = false;
        std::string result;
        // The same pair as ListEntities, so the two tools agree on which entities exist.
        // GetEntityScriptState uses a different pair, which is how the three tools end up
        // disagreeing about the same scene.
        auto view = m_Screen->GetAllEntitiesWith<IDComponent, TagComponent>();
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            // The id to compare against, as the 64-bit value rather than the UUID wrapper.
            auto &compId = entity.GetComponent<IDComponent>();
            // A linear scan comparing 64-bit values. Fine at scene sizes where a whole-scene
            // walk is noise, but it makes every call O(entities).
            if ((uint64_t)compId.ID != entityId)
                continue;

            // Reached only after the id matched, and safe because the view guarantees the
            // component.
            auto &tag = entity.GetComponent<TagComponent>();
            // Set before any optional component is read, so a match carrying none of them
            // still produces a successful answer.
            found = true;
            // The id string is echoed exactly as it was sent rather than reformatted from the
            // component, so the model sees its own text back.
            result += "Entity: " + tag.Tag + " (UUID: " + id + ")\n";

            // Every optional component is guarded by HasComponent before GetComponent, so
            // an entity without one is omitted rather than asserting.
            if (entity.HasComponent<TransformComponent>())
            {
                // Fetched only after the presence check.
                auto &tc = entity.GetComponent<TransformComponent>();
                // A header, because the three vectors below are printed one per line and would
                // otherwise be unreadable.
                result += "  Transform:\n";
                // glm components are floats and std::to_string on a float gives six decimal
                // places: readable for the model, lossy if it copies a number back into a file.
                result += "    Translation: (" + std::to_string(tc.Translation.x) + ", " + std::to_string(tc.Translation.y) + ", " + std::to_string(tc.Translation.z) + ")\n";
                // Euler angles in whatever unit the component stores. Nothing converts them here,
                // and nothing states the unit, so the model cannot tell degrees from radians.
                result += "    Rotation: (" + std::to_string(tc.Rotation.x) + ", " + std::to_string(tc.Rotation.y) + ", " + std::to_string(tc.Rotation.z) + ")\n";
                // Scale last, completing the triple.
                result += "    Scale: (" + std::to_string(tc.Scale.x) + ", " + std::to_string(tc.Scale.y) + ", " + std::to_string(tc.Scale.z) + ")\n";
            }

            // The next optional block. Its VALUES are reported, unlike every block after it.
            if (entity.HasComponent<ScriptComponent>())
            {
                // Fetched after the presence check, as above.
                auto &sc = entity.GetComponent<ScriptComponent>();
                // Name and path together: the path is what the model needs in order to edit the
                // file, and the name is what identifies it in the editor.
                result += "  Script: " + sc.ScriptName + " (" + sc.ScriptPath + ")\n";
            }

            // DEFECT (capability): from here on the tool reports PRESENCE only. A model can
            // learn that an entity has a camera but nothing about it: no frustum, no
            // projection, no near or far plane.
            if (entity.HasComponent<CameraComponent>())
            {
                // The word 'present' is the whole report. Every block from here to the end of
                // the function has the same shape.
                result += "  CameraComponent: present\n";
            }

            // Notably, the sprite's texture path is not printed, even though that is the one
            // value a model would most plausibly want to change.
            if (entity.HasComponent<SpriteRendererComponent>())
            {
                // Presence only.
                result += "  SpriteRenderer: present\n";
            }

            // Same for the physics components below.
            if (entity.HasComponent<Rigidbody2DComponent>())
            {
                // Presence only. Mass, damping and body type are all unreadable through this
                // tool.
                result += "  Rigidbody2D: present\n";
            }

            // Presence only.
            if (entity.HasComponent<BoxCollider2DComponent>())
            {
                // The last block. A model can confirm a collider exists but not its size.
                result += "  BoxCollider2D: present\n";
            }

            // Ids are unique, so the first match is the only match and the rest of the view
            // does not need scanning.
            break;
        }

        // A well-formed id that names nothing, which is a different error from a
        // malformed one above.
        if (!found)
            // Except that the malformed case never reaches here: it threw on the way past.
            return "Error: entity with UUID " + id + " not found";
        // The accumulated text, which for a bare entity is a single line.
        return result;
    }

    // Dumps an entity's live Python instance variables. The only tool that reads
    // interpreter state, and the only one that can raise through several layers.
    std::string ToolServer::GetEntityScriptState(const std::string &id)
    {
        if (!m_Screen)
            return "Error: no active scene";

        // DEFECT: the same unguarded throwing stoull as GetEntityInfo. Nothing on this
        // path catches it either, so a hallucinated id takes the whole tool call out
        // rather than producing an error string.
        uint64_t entityId = std::stoull(id);

        // A DIFFERENT component pair from the two tools above: this one needs a script
        // rather than a tag. An entity with a script and no tag is therefore visible here
        // while being invisible to list_entities and get_entity_info.
        auto view = m_Screen->GetAllEntitiesWith<IDComponent, ScriptComponent>();
        // The same linear walk, over a different view.
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            auto &compId = entity.GetComponent<IDComponent>();
            if ((uint64_t)compId.ID != entityId)
                continue;

            // The script's path, used only for the not-initialised message below.
            auto &sc = entity.GetComponent<ScriptComponent>();
            // A raw pointer into the engine's instance map, valid only while the scene is
            // running. Nothing keeps it alive across this call.
            py::object *instance = ScriptEngine::GetInstance(compId.ID);
            // The tool's only protection against a map that has already been torn down. A
            // dangling-but-non-null pointer is not covered, so a scene stopped mid-dump is
            // a use-after-free rather than an error.
            if (!instance)
                // Tells the model both why it is empty and which file the instance would come
                // from, which is the one piece of context it can act on.
                return "Script instance not initialized. Run the scene first.\nScript path: " + sc.ScriptPath;

            // The whole dump sits inside one try, because a single bad attribute can raise
            // and there is no per-attribute fallback for the __dict__ access itself.
            try
            {
                // Reads the instance dictionary rather than dir() or a class walk, so it sees
                // instance attributes only. An object using __slots__ has no instance __dict__ at
                // all, and the whole call fails with an AttributeError for that reason.
                py::list attrs = instance->attr("__dict__").attr("keys")().cast<py::list>();
                // Prefixed with the numeric id, so a multi-entity dump is unambiguous.
                std::string result = "Script variables for entity " + std::to_string(entityId) + ":\n";
                // Keys, not values: each one is fetched separately below so a property that
                // raises can be skipped without losing the rest of the dump.
                for (auto attrObj : attrs)
                {
                    // Attribute names in Python are always strings, so the cast cannot fail.
                    std::string attrName = attrObj.cast<std::string>();
                    // Underscore-prefixed names are dropped. That removes dunder methods and also any
                    // conventionally private attribute the author might have wanted to inspect; an
                    // empty name cannot occur in a real __dict__ but is guarded anyway.
                    if (attrName.empty() || attrName[0] == '_')
                        // Skipped silently, with no marker in the output that anything was filtered.
                        continue;

                    // Declared outside the inner try so the catch below can leave it untouched when
                    // the attribute access raises.
                    py::object value;
                    // A per-attribute try, because a property or descriptor can raise on access and
                    // one bad attribute should not lose the whole dump.
                    try
                    {
                        // Fetched from the instance, so a class attribute is reported too if the
                        // instance dictionary happens to shadow it. Raises are handled next.
                        value = instance->attr(attrName.c_str());
                    }
                    // Caught by non-const reference, which is what pybind11 requires for
                    // error_already_set.
                    catch (py::error_already_set &)
                    {
                        // The Python error indicator MUST be cleared, or the next call into the
                        // interpreter sees a pending exception and fails for an unrelated reason. This
                        // clearing is the reason the catch swallows rather than propagates.
                        PyErr_Clear();
                        // The attribute is skipped entirely. DEFECT (observability): nothing in the
                        // output says an attribute existed and could not be read, so the model sees a
                        // complete-looking variable list that is quietly missing entries -- including
                        // entries that raised on purpose, which is how a script's own guard properties
                        // disappear.
                        continue;
                    }

                    // Bound methods are py::function, so imported helpers and nested functions are
                    // filtered here too. The list is meant to be a variable dump, not an API.
                    if (py::isinstance<py::function>(value) || py::isinstance<py::module_>(value))
                        // Skipped, again with no marker in the output.
                        continue;

                    // The stringified value. DEFECT (naming): the variable is called repr but
                    // py::str calls PyObject_Str, which is Python's str(), not repr(). The two agree
                    // for str and int and differ for everything else.
                    std::string repr;
                    try
                    {
                        // An object with no __str__ falls back to its default repr, which embeds its
                        // address, so two identical calls in the same session print different text for
                        // the same value.
                        repr = py::str(value).cast<std::string>();
                    }
                    catch (py::error_already_set &)
                    {
                        PyErr_Clear();
                        // DEFECT: a value whose __str__ raised is reported as the literal string
                        // <error>, which is indistinguishable from a variable that genuinely holds that
                        // text. The model has no way to tell the two apart.
                        repr = "<error>";
                    }
                    // Two-space indent, matching the indentation used by get_entity_info so the two
                    // tools produce similarly shaped output.
                    result += "  " + attrName + " = " + repr + "\n";
                }
                // Success. Note this is inside the outer try, so a return from here still runs
                // no cleanup, which is correct: py::list and py::object are RAII.
                return result;
            }
            // The outer handler, for anything raised outside the per-attribute blocks -- in
            // practice the __dict__ access above.
            catch (py::error_already_set &e)
            {
                PyErr_Clear();
                // e.what() carries Python's own message and traceback text, which is the most
                // useful diagnostic this tool produces and the only place a Python traceback is
                // ever surfaced to the model.
                return "Error reading script state: " + std::string(e.what());
            }
        }

        // Reached when the loop ends without a match. Two different situations collapse
        // into this one message: an id that names no entity, and an entity with no
        // ScriptComponent at all. GetComponentInfo, which the header declares and this
        // file previously did not define, now exists below; it is a narrower inspector
        // than this one rather than a replacement, because it reports one component's
        // fields while this reports the ones it happens to know about.
        return "Error: entity with UUID " + id + " not found or has no script";
    }

    // ---- Repository root and engine-path resolution ---------------------
    //
    // The codebase tools read the engine source tree, which is not inside any open
    // project: a model asked to explain the renderer has to be able to open
    // Renderer3D.cpp, and no project root contains it. These two functions are the
    // whole mechanism, and both are read-only -- nothing here can be reached by a
    // write tool.

    std::filesystem::path ToolServer::FindRepoRoot()
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::current_path(ec);
        if (ec || dir.empty())
            return {};

        // Walk up looking for a repository marker. Eight levels is far past any
        // sane checkout depth while still bounded, so a checkout on a deep path
        // cannot walk to the filesystem root and stop there.
        for (int depth = 0; depth < 8; ++depth)
        {
            // Separate error codes per probe: exists() sets its code on failure, and
            // reusing one across the two calls would carry a stale failure into the
            // second.
            std::error_code probe;
            const bool hasMakefile = std::filesystem::exists(dir / "Makefile", probe);
            probe.clear();
            const bool hasSolution = std::filesystem::exists(dir / "Wasteland.sln", probe);
            if (hasMakefile || hasSolution)
                return dir;

            const std::filesystem::path parent = dir.parent_path();
            // parent_path of a root path returns empty, and of a drive root returns
            // the drive itself; both mean "no further up".
            if (parent.empty() || parent == dir)
                break;
            dir = parent;
        }

        // No marker. Falling back to the CWD rather than failing: every caller is
        // read-only, so a wrong root produces a "not found" the model can act on
        // rather than a wrong answer.
        return std::filesystem::current_path(ec);
    }

    std::filesystem::path ToolServer::ResolveProjectOrEnginePath(const std::string &path, std::string &error)
    {
        // Engine source is addressed repo-relative, with the leading directory naming
        // which half of the repository is meant. The prefix test is what keeps an
        // absolute path or a "../" escape from being reinterpreted as a repo path.
        const bool isEnginePath = path.rfind("Wasteland/", 0) == 0 ||
                                  path.rfind("DemonCore-Editor/", 0) == 0;
        if (!isEnginePath)
            return ResolveProjectPath(path, error);

        const std::filesystem::path root = FindRepoRoot();
        if (root.empty())
        {
            error = "cannot locate the repository root";
            return {};
        }

        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(root / path, ec);
        if (ec)
        {
            error = "cannot resolve path '" + path + "'";
            return {};
        }

        // Containment, by the same relative-path trick ResolveProjectPath uses: a
        // path that climbs out of the root cannot be expressed relative to it.
        std::error_code relEc;
        const std::filesystem::path rel = std::filesystem::relative(abs, root, relEc);
        if (relEc || rel.empty() || *rel.begin() == "..")
        {
            error = "path '" + path + "' is outside the repository";
            return {};
        }
        return abs;
    }

    // ---- Codebase tools -------------------------------------------------

    std::string ToolServer::SearchFiles(const std::string &query, const std::string &dir)
    {
        // An empty query would match every line of every file.
        if (query.empty())
            return "Error: search_files requires a non-empty 'query'";

        std::error_code ec;
        std::filesystem::path root;
        if (dir.empty())
        {
            root = FindRepoRoot();
            if (root.empty())
                return "Error: cannot locate the repository root";
        }
        else
        {
            std::string error;
            root = ResolveProjectOrEnginePath(dir, error);
            if (root.empty())
                return "Error: " + error;
        }

        if (!std::filesystem::is_directory(root, ec))
            return "Error: '" + (dir.empty() ? std::string("<repository root>") : dir) + "' is not a directory";

        // The base for repo-relative output. When the search is scoped to a project
        // subdirectory this is still the repository, so a result's path is stable
        // regardless of how the search was narrowed.
        const std::filesystem::path repoRoot = FindRepoRoot();

        std::string result;
        int hits = 0;
        // A cap, because an unbounded search over the whole repository can produce
        // more text than the context window holds.
        const int kMaxHits = 200;

        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator() && hits < kMaxHits;
             it.increment(ec))
        {
            if (ec)
                break;
            const std::filesystem::path &entry = it->path();
            const std::string name = entry.filename().string();
            // VCS metadata, build output and vendored third-party code. None of them
            // is engine source the model can act on, and bin-int alone is tens of
            // thousands of files that would dominate the walk.
            if (name == ".git" || name == "bin" || name == "bin-int" ||
                name == ".buildlogs" || name == "vendor")
            {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;

            // Extension filter. Without it the search would match byte sequences in
            // binary and asset files and report them as text.
            const std::string ext = entry.extension().string();
            const bool isText = ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".c" ||
                                ext == ".cc" || ext == ".inl" || ext == ".md" || ext == ".txt" ||
                                ext == ".py" || ext == ".cmake" || ext == ".json" || ext == ".yml" ||
                                ext == ".yaml" || ext == ".ini" || ext == ".cfg" || ext == ".slang" ||
                                ext == ".hlsl" || ext == ".bat" || ext == ".ps1" || ext == ".sh";
            if (!isText)
                continue;

            std::ifstream file(entry);
            if (!file.is_open())
                continue;

            std::string line;
            int lineNo = 0;
            while (std::getline(file, line) && hits < kMaxHits)
            {
                ++lineNo;
                if (line.find(query) == std::string::npos)
                    continue;

                // Repo-relative, so the model can feed it straight back into
                // read_file; the fallback keeps a usable path if the relative
                // computation fails.
                std::string display = entry.string();
                std::error_code relEc;
                const std::filesystem::path rel = std::filesystem::relative(entry, repoRoot, relEc);
                if (!relEc && !rel.empty())
                    display = rel.string();
                std::replace(display.begin(), display.end(), '\\', '/');
                result += display + ":" + std::to_string(lineNo) + ": " + line + "\n";
                ++hits;
            }
        }

        if (result.empty())
            return "No matches for '" + query + "'";
        // The cap is reported rather than silently applied: a model that needs more
        // has to know the answer is incomplete.
        if (hits >= kMaxHits)
            result += "... (truncated at " + std::to_string(kMaxHits) + " matches)\n";
        return result;
    }

    std::string ToolServer::ListDir(const std::string &dir)
    {
        std::string error;
        const std::filesystem::path fullPath = ResolveProjectOrEnginePath(dir, error);
        if (fullPath.empty())
            return "Error: " + error;

        std::error_code ec;
        if (!std::filesystem::is_directory(fullPath, ec))
            return "Error: '" + dir + "' is not a directory";

        const std::filesystem::path repoRoot = FindRepoRoot();
        std::vector<std::string> entries;
        std::error_code iterEc;
        for (auto it = std::filesystem::directory_iterator(fullPath, iterEc);
             it != std::filesystem::directory_iterator(); it.increment(iterEc))
        {
            if (iterEc)
                break;
            const std::filesystem::path &entry = it->path();
            std::string display = entry.string();
            std::error_code relEc;
            const std::filesystem::path rel = std::filesystem::relative(entry, repoRoot, relEc);
            if (!relEc && !rel.empty())
                display = rel.string();
            std::replace(display.begin(), display.end(), '\\', '/');
            // A trailing slash marks directories, which is the only way to tell them
            // from files in a flat listing.
            std::error_code typeEc;
            if (it->is_directory(typeEc))
                display += "/";
            entries.push_back(display);
        }
        // The filesystem's order is unspecified; sorted output is what makes two
        // consecutive calls comparable.
        std::sort(entries.begin(), entries.end());

        if (entries.empty())
            return "(empty directory)";
        std::string result;
        for (const std::string &entry : entries)
            result += entry + "\n";
        return result;
    }

    std::string ToolServer::GetSymbolInfo(const std::string &symbol)
    {
        if (symbol.empty())
            return "Error: get_symbol_info requires a non-empty 'symbol'";

        const std::filesystem::path root = FindRepoRoot();
        if (root.empty())
            return "Error: cannot locate the repository root";

        // Headers first: a symbol's declaration is what the model needs, and the
        // header set is a small fraction of the file count.
        const char *searchRoots[] = {"Wasteland/src", "DemonCore-Editor/src"};
        std::string result;
        int hits = 0;
        const int kMaxHits = 20;
        std::error_code ec;

        for (const char *sub : searchRoots)
        {
            const std::filesystem::path searchRoot = root / sub;
            if (!std::filesystem::is_directory(searchRoot, ec))
                continue;

            for (auto it = std::filesystem::recursive_directory_iterator(searchRoot, ec);
                 it != std::filesystem::recursive_directory_iterator() && hits < kMaxHits;
                 it.increment(ec))
            {
                if (ec)
                    break;
                const std::filesystem::path &entry = it->path();
                const std::string name = entry.filename().string();
                if (name == ".git" || name == "bin" || name == "bin-int" || name == "vendor")
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec))
                    continue;
                const std::string ext = entry.extension().string();
                if (ext != ".h" && ext != ".hpp" && ext != ".inl")
                    continue;

                std::ifstream file(entry);
                if (!file.is_open())
                    continue;

                std::string line;
                int lineNo = 0;
                while (std::getline(file, line) && hits < kMaxHits)
                {
                    ++lineNo;
                    // An identifier-boundary match, not a substring match: a bare
                    // find would also hit comments, call sites and unrelated
                    // identifiers that merely contain the name.
                    size_t pos = line.find(symbol);
                    while (pos != std::string::npos)
                    {
                        const char before = pos > 0 ? line[pos - 1] : '\0';
                        const bool boundaryBefore =
                            (pos == 0) || !(std::isalnum((unsigned char)before) || before == '_');
                        const size_t end = pos + symbol.size();
                        const char after = end < line.size() ? line[end] : '\0';
                        const bool boundaryAfter =
                            (end >= line.size()) || !(std::isalnum((unsigned char)after) || after == '_');
                        if (boundaryBefore && boundaryAfter)
                            break;
                        pos = line.find(symbol, pos + 1);
                    }
                    if (pos == std::string::npos)
                        continue;

                    std::string trimmed = line;
                    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
                        trimmed.pop_back();
                    std::string display = entry.string();
                    std::error_code relEc;
                    const std::filesystem::path rel = std::filesystem::relative(entry, root, relEc);
                    if (!relEc && !rel.empty())
                        display = rel.string();
                    std::replace(display.begin(), display.end(), '\\', '/');
                    result += display + ":" + std::to_string(lineNo) + ": " + trimmed + "\n";
                    ++hits;
                }
            }
        }

        if (result.empty())
            return "No declaration found for '" + symbol + "' in headers under Wasteland/src and DemonCore-Editor/src";
        if (hits >= kMaxHits)
            result += "... (truncated at " + std::to_string(kMaxHits) + " declarations)\n";
        return result;
    }

    std::string ToolServer::GetEngineTree()
    {
        const std::filesystem::path root = FindRepoRoot();
        if (root.empty())
            return "Error: cannot locate the repository root";

        std::string result;
        std::error_code ec;
        const char *searchRoots[] = {"Wasteland/src", "DemonCore-Editor/src"};

        for (const char *sub : searchRoots)
        {
            const std::filesystem::path searchRoot = root / sub;
            if (!std::filesystem::is_directory(searchRoot, ec))
            {
                result += std::string(sub) + ": (not found)\n";
                continue;
            }
            result += std::string(sub) + ":\n";

            // Collected then sorted: the walk order is unspecified, and an
            // alphabetical tree is the one a model can scan for a subsystem name.
            std::vector<std::pair<std::string, int>> rows;
            for (auto it = std::filesystem::recursive_directory_iterator(searchRoot, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (ec)
                    break;
                const std::filesystem::path &entry = it->path();
                const std::string name = entry.filename().string();
                if (name == ".git" || name == "bin" || name == "bin-int" || name == "vendor")
                {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code typeEc;
                if (!it->is_directory(typeEc))
                    continue;

                // Files directly inside this directory, not recursively: the
                // recursive total would be dominated by a handful of deep
                // directories and say nothing about where a file lives.
                int files = 0;
                std::error_code innerEc;
                for (auto inner = std::filesystem::directory_iterator(entry, innerEc);
                     inner != std::filesystem::directory_iterator(); inner.increment(innerEc))
                {
                    if (innerEc)
                        break;
                    std::error_code fileEc;
                    if (inner->is_regular_file(fileEc))
                        ++files;
                }

                std::string display = entry.string();
                std::error_code relEc;
                const std::filesystem::path rel = std::filesystem::relative(entry, root, relEc);
                if (!relEc && !rel.empty())
                    display = rel.string();
                std::replace(display.begin(), display.end(), '\\', '/');
                rows.emplace_back(display, files);
            }
            std::sort(rows.begin(), rows.end());
            for (const auto &row : rows)
                result += "  " + row.first + " (" + std::to_string(row.second) + " files)\n";
        }
        return result;
    }

    // ---- Python execution ----------------------------------------------

    std::string ToolServer::PythonExec(const std::string &code)
    {
        if (code.empty())
            return "Error: python_exec requires a non-empty 'code'";
        // The interpreter is started by the engine, not by this tool. Before it is,
        // there is nothing to run in.
        if (!Py_IsInitialized())
            return "Error: the Python interpreter is not initialised";

        // Tool calls run on the AI worker thread; the interpreter belongs to the
        // engine thread. Every entry into Python from here has to take the GIL
        // first, and gil_scoped_acquire is RAII, so the release happens even when
        // the code raises.
        py::gil_scoped_acquire gil;

        py::module_ sys = py::module_::import("sys");
        py::module_ io = py::module_::import("io");
        // Both saved before either is replaced, and restored before the buffers are
        // read back -- reading them through the redirected stream would append the
        // getvalue() call's own output to the buffer it reads.
        py::object oldStdout = sys.attr("stdout");
        py::object oldStderr = sys.attr("stderr");
        py::object out = io.attr("StringIO")();
        py::object err = io.attr("StringIO")();

        std::string error;
        bool ran = false;
        sys.attr("stdout") = out;
        sys.attr("stderr") = err;
        try
        {
            // A fresh globals dict per call: a model that assigns x = 1 in one call
            // must not have that x leak into the next. builtins is put in
            // explicitly because CPython only auto-inserts it for exec(), and this
            // interpreter may be embedded in a host that does not.
            py::dict globals;
            globals["__builtins__"] = py::module_::import("builtins");
            py::exec(code, globals);
            ran = true;
        }
        catch (py::error_already_set &e)
        {
            // e.what() carries Python's own message plus traceback text, which is
            // the most useful diagnostic this tool produces.
            error = e.what();
            // The error indicator is cleared so a later Python call from another
            // tool does not inherit it.
            PyErr_Clear();
        }
        catch (std::exception &e)
        {
            error = e.what();
        }
        sys.attr("stdout") = oldStdout;
        sys.attr("stderr") = oldStderr;

        const std::string outText = py::str(out.attr("getvalue")());
        const std::string errText = py::str(err.attr("getvalue")());

        std::string result;
        if (!outText.empty())
            result += outText;
        if (!errText.empty())
        {
            if (!result.empty())
                result += "\n";
            result += "[stderr]\n" + errText;
        }
        if (!error.empty())
        {
            if (!result.empty())
                result += "\n";
            result += "[error]\n" + error;
        }
        // Distinguishes "ran and printed nothing" from "did not run", which an
        // empty string would collapse.
        if (result.empty())
            return ran ? "(executed, no output)" : "Error: python_exec produced no result";
        return result;
    }

    // ---- Component inspection ------------------------------------------

    std::string ToolServer::GetComponentInfo(const std::string &id, const std::string &component)
    {
        if (!m_Screen)
            return "Error: no active scene";

        // stoull is wrapped, unlike GetEntityInfo: this tool is reached by the same
        // model output, and a hallucinated id should come back as a message rather
        // than unwind through ExecuteTool.
        uint64_t entityId;
        try
        {
            entityId = std::stoull(id);
        }
        catch (...)
        {
            return "Error: invalid entity id '" + id + "'";
        }

        // The same view pair as GetEntityInfo, so the two tools agree on which
        // entities exist -- the disagreement GetEntityScriptState has with them is
        // noted at its own site.
        auto view = m_Screen->GetAllEntitiesWith<IDComponent, TagComponent>();
        for (auto entityHandle : view)
        {
            Entity entity(entityHandle, m_Screen);
            auto &compId = entity.GetComponent<IDComponent>();
            if ((uint64_t)compId.ID != entityId)
                continue;

            const std::string name = entity.GetComponent<TagComponent>().Tag;
            std::string result = "Entity: " + name + " (UUID: " + id + ")\n";

            // The supported set is the components a scene inspector actually needs.
            // Everything else falls through to the presence report below rather
            // than being silently omitted.
            if (component == "Transform")
            {
                if (!entity.HasComponent<TransformComponent>())
                    return "Error: entity '" + name + "' has no TransformComponent";
                auto &tc = entity.GetComponent<TransformComponent>();
                result += "  Translation: (" + std::to_string(tc.Translation.x) + ", " + std::to_string(tc.Translation.y) + ", " + std::to_string(tc.Translation.z) + ")\n";
                // Radians, as stored. Nothing converts to degrees here, and the
                // unit is stated so the model does not have to guess.
                result += "  Rotation (radians): (" + std::to_string(tc.Rotation.x) + ", " + std::to_string(tc.Rotation.y) + ", " + std::to_string(tc.Rotation.z) + ")\n";
                result += "  Scale: (" + std::to_string(tc.Scale.x) + ", " + std::to_string(tc.Scale.y) + ", " + std::to_string(tc.Scale.z) + ")\n";
                return result;
            }
            if (component == "SpriteRenderer")
            {
                if (!entity.HasComponent<SpriteRendererComponent>())
                    return "Error: entity '" + name + "' has no SpriteRendererComponent";
                auto &sc = entity.GetComponent<SpriteRendererComponent>();
                result += "  Color: (" + std::to_string(sc.Color.r) + ", " + std::to_string(sc.Color.g) + ", " + std::to_string(sc.Color.b) + ", " + std::to_string(sc.Color.a) + ")\n";
                result += "  TilingFactor: " + std::to_string(sc.TilingFactor) + "\n";
                result += std::string("  Texture: ") + (sc.Texture ? "bound" : "none") + "\n";
                return result;
            }
            if (component == "CircleRenderer")
            {
                if (!entity.HasComponent<CircleRendererComponent>())
                    return "Error: entity '" + name + "' has no CircleRendererComponent";
                auto &cc = entity.GetComponent<CircleRendererComponent>();
                result += "  Color: (" + std::to_string(cc.Color.r) + ", " + std::to_string(cc.Color.g) + ", " + std::to_string(cc.Color.b) + ", " + std::to_string(cc.Color.a) + ")\n";
                result += "  Thickness: " + std::to_string(cc.Thickness) + "\n";
                result += "  Fade: " + std::to_string(cc.Fade) + "\n";
                return result;
            }
            if (component == "Rigidbody2D")
            {
                if (!entity.HasComponent<Rigidbody2DComponent>())
                    return "Error: entity '" + name + "' has no Rigidbody2DComponent";
                auto &rb = entity.GetComponent<Rigidbody2DComponent>();
                const char *typeName = rb.Type == Rigidbody2DComponent::BodyType::Static ? "Static"
                                    : rb.Type == Rigidbody2DComponent::BodyType::Dynamic ? "Dynamic" : "Kinematic";
                result += std::string("  Type: ") + typeName + "\n";
                result += std::string("  FixedRotation: ") + (rb.FixedRotation ? "true" : "false") + "\n";
                result += std::string("  Simulated: ") + (rb.RuntimeBody ? "yes" : "no") + "\n";
                return result;
            }
            if (component == "BoxCollider2D")
            {
                if (!entity.HasComponent<BoxCollider2DComponent>())
                    return "Error: entity '" + name + "' has no BoxCollider2DComponent";
                auto &bc = entity.GetComponent<BoxCollider2DComponent>();
                result += "  Offset: (" + std::to_string(bc.Offset.x) + ", " + std::to_string(bc.Offset.y) + ")\n";
                result += "  Size: (" + std::to_string(bc.Size.x) + ", " + std::to_string(bc.Size.y) + ")\n";
                result += "  Density: " + std::to_string(bc.Density) + "\n";
                result += "  Friction: " + std::to_string(bc.Friction) + "\n";
                result += "  Restitution: " + std::to_string(bc.Restitution) + "\n";
                return result;
            }
            if (component == "CircleCollider2D")
            {
                if (!entity.HasComponent<CircleCollider2DComponent>())
                    return "Error: entity '" + name + "' has no CircleCollider2DComponent";
                auto &cc = entity.GetComponent<CircleCollider2DComponent>();
                result += "  Offset: (" + std::to_string(cc.Offset.x) + ", " + std::to_string(cc.Offset.y) + ")\n";
                result += "  Radius: " + std::to_string(cc.Radius) + "\n";
                result += "  Density: " + std::to_string(cc.Density) + "\n";
                result += "  Friction: " + std::to_string(cc.Friction) + "\n";
                return result;
            }
            if (component == "Camera")
            {
                if (!entity.HasComponent<CameraComponent>())
                    return "Error: entity '" + name + "' has no CameraComponent";
                auto &cam = entity.GetComponent<CameraComponent>();
                const char *projection = cam.Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective
                                             ? "Perspective" : "Orthographic";
                result += std::string("  Projection: ") + projection + "\n";
                result += "  PerspectiveFOV (radians): " + std::to_string(cam.Camera.GetPerspectiveVerticalFOV()) + "\n";
                result += "  OrthographicSize: " + std::to_string(cam.Camera.GetOrthographicSize()) + "\n";
                result += std::string("  Primary: ") + (cam.Primary ? "true" : "false") + "\n";
                result += std::string("  FixedAspectRatio: ") + (cam.FixedAspectRatio ? "true" : "false") + "\n";
                return result;
            }
            if (component == "Script")
            {
                if (!entity.HasComponent<ScriptComponent>())
                    return "Error: entity '" + name + "' has no ScriptComponent";
                auto &sc = entity.GetComponent<ScriptComponent>();
                result += "  ScriptName: " + sc.ScriptName + "\n";
                result += "  ScriptPath: " + sc.ScriptPath + "\n";
                return result;
            }
            if (component == "Material")
            {
                if (!entity.HasComponent<MaterialComponent>())
                    return "Error: entity '" + name + "' has no MaterialComponent";
                auto &mat = entity.GetComponent<MaterialComponent>();
                result += "  Albedo: (" + std::to_string(mat.Albedo.r) + ", " + std::to_string(mat.Albedo.g) + ", " + std::to_string(mat.Albedo.b) + ", " + std::to_string(mat.Albedo.a) + ")\n";
                result += "  Metallic: " + std::to_string(mat.Metallic) + "\n";
                result += "  Roughness: " + std::to_string(mat.Roughness) + "\n";
                result += "  NormalStrength: " + std::to_string(mat.NormalStrength) + "\n";
                result += "  EmissionIntensity: " + std::to_string(mat.EmissionIntensity) + "\n";
                return result;
            }

            // The fallback: list the inspectable components the entity carries, so
            // an unsupported name still tells the model something it can act on
            // instead of only reporting a refusal.
            std::string list;
            if (entity.HasComponent<TransformComponent>()) list += "Transform ";
            if (entity.HasComponent<SpriteRendererComponent>()) list += "SpriteRenderer ";
            if (entity.HasComponent<CircleRendererComponent>()) list += "CircleRenderer ";
            if (entity.HasComponent<Rigidbody2DComponent>()) list += "Rigidbody2D ";
            if (entity.HasComponent<BoxCollider2DComponent>()) list += "BoxCollider2D ";
            if (entity.HasComponent<CircleCollider2DComponent>()) list += "CircleCollider2D ";
            if (entity.HasComponent<CameraComponent>()) list += "Camera ";
            if (entity.HasComponent<ScriptComponent>()) list += "Script ";
            if (entity.HasComponent<MaterialComponent>()) list += "Material ";

            result += "  Components present: " +
                      (list.empty() ? std::string("(none of the inspected types)") : list) + "\n";
            result += "  '" + component + "' is not one of the inspectable types: Transform, SpriteRenderer, "
                      "CircleRenderer, Rigidbody2D, BoxCollider2D, CircleCollider2D, Camera, Script, Material.\n";
            return result;
        }

        return "Error: entity with UUID " + id + " not found";
    }

    // ---- Web search -----------------------------------------------------

    // File-local helpers. Static rather than members: they are implementation
    // details of WebSearch and need no per-instance state.

    static std::string HtmlDecode(const std::string &text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '&')
            {
                out += text[i];
                continue;
            }
            // The entities DuckDuckGo emits in result titles and snippets. Anything
            // else is left as a literal '&': dropping it would corrupt text that
            // merely contains an ampersand.
            if (text.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
            else if (text.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
            else if (text.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
            else if (text.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
            else if (text.compare(i, 5, "&#39;") == 0) { out += '\''; i += 4; }
            else out += '&';
        }
        return out;
    }

    static std::string StripHtml(const std::string &text)
    {
        std::string out;
        out.reserve(text.size());
        bool inTag = false;
        for (char c : text)
        {
            if (c == '<') { inTag = true; continue; }
            if (c == '>') { inTag = false; continue; }
            if (!inTag)
                out += c;
        }
        return HtmlDecode(out);
    }

    // Pulls the href and the anchor text out of the <a ...>...</a> at or after
    // `from`. Returns false when there is no further anchor.
    static bool NextAnchor(const std::string &html, size_t from, std::string &href, std::string &text, size_t &end)
    {
        const size_t open = html.find("<a ", from);
        if (open == std::string::npos)
            return false;
        const size_t close = html.find("</a>", open);
        if (close == std::string::npos)
            return false;

        const size_t hrefPos = html.find("href=\"", open);
        if (hrefPos != std::string::npos && hrefPos < close)
        {
            const size_t start = hrefPos + 6;
            const size_t stop = html.find('"', start);
            if (stop != std::string::npos && stop < close)
                href = html.substr(start, stop - start);
        }
        const size_t textStart = html.find('>', open);
        if (textStart != std::string::npos && textStart < close)
            text = html.substr(textStart + 1, close - textStart - 1);
        end = close + 4;
        return true;
    }

    // DuckDuckGo's html endpoint wraps outbound links in a redirect whose uddg
    // parameter is the real target. The lite endpoint does not, so this is a
    // no-op there -- which is why it is applied unconditionally.
    static std::string UnwrapDuckDuckGoRedirect(const std::string &href)
    {
        const size_t uddg = href.find("uddg=");
        if (uddg == std::string::npos)
            return href;
        const size_t start = uddg + 5;
        const size_t stop = href.find('&', start);
        const std::string encoded = href.substr(start, (stop == std::string::npos ? href.size() : stop) - start);

        std::string decoded;
        decoded.reserve(encoded.size());
        for (size_t i = 0; i < encoded.size(); ++i)
        {
            if (encoded[i] == '%' && i + 2 < encoded.size())
            {
                const std::string hex = encoded.substr(i + 1, 2);
                const char *c = hex.c_str();
                char *end = nullptr;
                const long value = std::strtol(c, &end, 16);
                if (end && *end == '\0' && value > 0 && value < 256)
                {
                    decoded += static_cast<char>(value);
                    i += 2;
                    continue;
                }
            }
            decoded += encoded[i];
        }
        return decoded;
    }

    // DuckDuckGo's result markup. Each result title is an <a> carrying the
    // endpoint's title class; the snippet is the element carrying the snippet
    // class that follows it.
    static std::string ParseSearchResults(const std::string &html, const std::string &anchorClass,
                                          const std::string &snippetClass, int maxResults)
    {
        std::string out;
        int count = 0;
        size_t pos = 0;

        while (count < maxResults)
        {
            // The result anchor carries a class unique to result titles, which is
            // what separates them from navigation and ads.
            const std::string classAttr = "class=\"" + anchorClass + "\"";
            const size_t classPos = html.find(classAttr, pos);
            if (classPos == std::string::npos)
                break;

            // The class attribute sits inside the <a> tag, so rewind to the tag's
            // start rather than parsing from the attribute.
            const size_t tagStart = html.rfind("<a ", classPos);
            if (tagStart == std::string::npos)
            {
                pos = classPos + classAttr.size();
                continue;
            }

            std::string href, text;
            size_t end = 0;
            if (!NextAnchor(html, tagStart, href, text, end))
                break;

            const std::string title = StripHtml(text);
            // An anchor with no text is a layout artifact, not a result.
            if (title.empty())
            {
                pos = end;
                continue;
            }

            // The snippet follows the anchor inside an element carrying the snippet
            // class. The distance bound keeps a missing snippet from attaching the
            // next result's text to this one.
            std::string snippet;
            const std::string snippetAttr = "class=\"" + snippetClass + "\"";
            const size_t snippetPos = html.find(snippetAttr, end);
            if (snippetPos != std::string::npos && snippetPos - end < 2000)
            {
                const size_t textStart = html.find('>', snippetPos);
                if (textStart != std::string::npos)
                {
                    const size_t textEnd = html.find('<', textStart);
                    if (textEnd != std::string::npos)
                        snippet = StripHtml(html.substr(textStart + 1, textEnd - textStart - 1));
                }
            }

            out += "## " + std::to_string(count + 1) + ". " + title + "\n";
            out += UnwrapDuckDuckGoRedirect(href) + "\n";
            if (!snippet.empty())
                out += snippet + "\n";
            out += "\n";
            ++count;
            pos = end;
        }
        return out;
    }

    // DuckDuckGo's two HTML shapes. The full endpoint wraps every outbound link
    // in a redirect and uses <a> for snippets; the lite endpoint is a bare table
    // with no redirect.
    static std::string TryDuckDuckGo(const std::string &encodedQuery, int maxResults)
    {
        struct Endpoint
        {
            const char *url;
            const char *anchorClass;
            const char *snippetClass;
        };
        const Endpoint endpoints[] = {
            {"https://html.duckduckgo.com/html/?q=", "result__a", "result__snippet"},
            {"https://lite.duckduckgo.com/lite/?q=", "result-link", "result-snippet"},
        };

        // A browser-like User-Agent: the HTML endpoints answer a bare WinHTTP
        // client with a challenge page, and this header is what avoids it.
        const HttpHeaders headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"},
            {"Accept", "text/html,application/xhtml+xml"},
        };

        for (const Endpoint &ep : endpoints)
        {
            const HttpResult res = HttpClient::Get(std::string(ep.url) + encodedQuery, headers);
            // A transport failure, an empty body and a challenge page all move on
            // to the next endpoint; the parse simply yields nothing for them.
            if (!res.Succeeded || res.Body.empty())
                continue;
            const std::string parsed = ParseSearchResults(res.Body, ep.anchorClass, ep.snippetClass, maxResults);
            if (!parsed.empty())
                return parsed;
        }
        return {};
    }

    // Bing wraps outbound links in https://www.bing.com/ck/a?...&u=a1<base64url>.
    // The real target is that parameter, base64url-encoded behind a leading "a1".
    static std::string UnwrapBingRedirect(const std::string &href)
    {
        const size_t param = href.find("u=a1");
        if (param == std::string::npos)
            return href;

        // "u=a1" is a four-character prefix: the parameter name, '=', and a
        // two-character version tag. The base64url payload starts after it, and
        // including the tag in the decode corrupts every URL.
        std::string b64;
        b64.reserve(href.size() - param);
        for (size_t i = param + 4; i < href.size(); ++i)
        {
            const char c = href[i];
            // The value runs to the next separator. Bing emits it unpadded, so a
            // '=' inside it is padding and marks the end of the value.
            if (c == '&' || c == '=')
                break;
            // base64url to base64.
            b64 += (c == '-') ? '+' : (c == '_') ? '/' : c;
        }
        while (b64.size() % 4 != 0)
            b64 += '=';

        static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string decoded;
        decoded.reserve(b64.size());
        int buffer = 0;
        int bits = 0;
        for (char c : b64)
        {
            const char *found = std::strchr(kAlphabet, c);
            if (!found)
                continue;
            buffer = (buffer << 6) + static_cast<int>(found - kAlphabet);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                decoded += static_cast<char>((buffer >> bits) & 0xFF);
            }
        }
        // An empty decode means the parameter was not a URL; the original redirect
        // is more useful to the model than nothing.
        return decoded.empty() ? href : decoded;
    }

    // Bing's result list. Each result is an <li class="b_algo"> whose <h2> holds
    // the title link, and whose snippet is the first <p> after it.
    static std::string TryBing(const std::string &encodedQuery, int maxResults)
    {
        const HttpHeaders headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"},
            {"Accept", "text/html,application/xhtml+xml"},
        };
        const HttpResult res = HttpClient::Get("https://www.bing.com/search?q=" + encodedQuery, headers);
        if (!res.Succeeded || res.Body.empty())
            return {};

        std::string out;
        int count = 0;
        size_t pos = 0;
        while (count < maxResults)
        {
            const size_t blockPos = res.Body.find("class=\"b_algo\"", pos);
            if (blockPos == std::string::npos)
                break;
            // The next block bounds this one, so a missing snippet cannot run on
            // into the following result.
            const size_t nextBlock = res.Body.find("class=\"b_algo\"", blockPos + 1);
            const size_t limit = (nextBlock == std::string::npos) ? res.Body.size() : nextBlock;

            const size_t h2 = res.Body.find("<h2", blockPos);
            if (h2 == std::string::npos || h2 >= limit)
            {
                pos = blockPos + 11;
                continue;
            }
            std::string href, text;
            size_t end = 0;
            if (!NextAnchor(res.Body, h2, href, text, end) || end > limit)
            {
                pos = blockPos + 11;
                continue;
            }
            const std::string title = StripHtml(text);
            if (title.empty())
            {
                pos = blockPos + 11;
                continue;
            }

            std::string snippet;
            const size_t p = res.Body.find("<p", end);
            if (p != std::string::npos && p < limit)
            {
                const size_t textStart = res.Body.find('>', p);
                if (textStart != std::string::npos)
                {
                    const size_t textEnd = res.Body.find('<', textStart);
                    if (textEnd != std::string::npos && textEnd < limit)
                        snippet = StripHtml(res.Body.substr(textStart + 1, textEnd - textStart - 1));
                }
            }

            out += "## " + std::to_string(count + 1) + ". " + title + "\n";
            out += UnwrapBingRedirect(href) + "\n";
            if (!snippet.empty())
                out += snippet + "\n";
            out += "\n";
            ++count;
            pos = limit;
        }
        return out;
    }

    // Wikipedia's search API: JSON rather than HTML, so there is no layout to parse
    // and no redirect to unwrap. Encyclopedic rather than general web, which is why
    // it is the last resort rather than the primary.
    static std::string TryWikipedia(const std::string &query, int maxResults)
    {
        const HttpHeaders headers = {
            {"User-Agent", "WastelandEngine/1.0 (built-in web search)"},
        };
        const std::string url = "https://en.wikipedia.org/w/api.php?action=query&list=search&srsearch=" +
                                HttpClient::UrlEncode(query) +
                                "&format=json&srlimit=" + std::to_string(maxResults);
        const HttpResult res = HttpClient::Get(url, headers);
        if (!res.Succeeded || res.Body.empty())
            return {};

        std::string out;
        int count = 0;
        size_t pos = 0;
        while (count < maxResults)
        {
            const size_t item = res.Body.find("\"title\":", pos);
            if (item == std::string::npos)
                break;
            const size_t titleStart = res.Body.find('"', item + 8);
            if (titleStart == std::string::npos)
                break;
            const size_t titleEnd = res.Body.find('"', titleStart + 1);
            if (titleEnd == std::string::npos)
                break;
            const std::string title = res.Body.substr(titleStart + 1, titleEnd - titleStart - 1);

            // The snippet is the next "snippet" key after the title, and the API
            // wraps matches in <span class="searchmatch">, which StripHtml removes.
            std::string snippet;
            const size_t snippetKey = res.Body.find("\"snippet\":", titleEnd);
            if (snippetKey != std::string::npos && snippetKey - titleEnd < 2000)
            {
                const size_t snippetStart = res.Body.find('"', snippetKey + 10);
                if (snippetStart != std::string::npos)
                {
                    const size_t snippetEnd = res.Body.find('"', snippetStart + 1);
                    if (snippetEnd != std::string::npos)
                        snippet = StripHtml(res.Body.substr(snippetStart + 1, snippetEnd - snippetStart - 1));
                }
            }

            // The article URL is the title with spaces replaced by underscores.
            std::string path = title;
            std::replace(path.begin(), path.end(), ' ', '_');

            out += "## " + std::to_string(count + 1) + ". " + title + "\n";
            out += "https://en.wikipedia.org/wiki/" + path + "\n";
            if (!snippet.empty())
                out += snippet + "\n";
            out += "\n";
            ++count;
            pos = titleEnd;
        }
        return out;
    }

    std::string ToolServer::WebSearch(const std::string &query, int maxResults)
    {
        if (query.empty())
            return "Error: web_search requires a non-empty 'query'";

        // Clamped here rather than at the call site: the model can send any integer,
        // and the tool's own bound is the only place that can enforce it.
        if (maxResults <= 0)
            maxResults = 8;
        if (maxResults > 20)
            maxResults = 20;

        const std::string encoded = HttpClient::UrlEncode(query);

        // DuckDuckGo first: it is the endpoint this tool is specified to use, and
        // its HTML needs no key. It is also the provider most likely to answer a
        // datacentre IP with a challenge page, which is why it is not the only one.
        const std::string ddg = TryDuckDuckGo(encoded, maxResults);
        if (!ddg.empty())
            return ddg;

        // Bing: a general-web fallback over the same transport, same no-key rule.
        const std::string bing = TryBing(encoded, maxResults);
        if (!bing.empty())
            return bing;

        // Wikipedia last: no HTML to parse, but encyclopedic in scope.
        const std::string wiki = TryWikipedia(query, maxResults);
        if (!wiki.empty())
            return wiki;

        return "Error: web search returned no results (DuckDuckGo, Bing and Wikipedia all failed or were blocked)";
    }

}
