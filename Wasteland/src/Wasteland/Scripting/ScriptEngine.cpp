// First include by convention in every engine translation unit: it forces
// the platform/GLM/format headers to be configured before anything else
// includes them, and it is where pybind11's Python.h is pulled in.
#include "wlpch.h"
#include "ScriptEngine.h"
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Project/Project.h"

// filesystem for path composition in OnUpdateEntity; unordered_set for the
// failed-script memo; algorithm for std::remove_if in CleanPath. No
// <vector>/<string> because wlpch supplies them.
#include <filesystem>
#include <unordered_set>
#include <algorithm>

namespace Wasteland
{
    // DEFINED IN THE .CPP, not initialised inline: py::object is a Python
    // handle and must not be constructed during static initialisation, which
    // would run before Py_Initialize. This is the standard pybind11 reason.
    //
    // DEFECT (leak, documented not fixed): a static py::object map holds Python
    // references that are destroyed at static-destruction time. If the
    // interpreter has already been finalised by then, dropping the last
    // reference calls into a dead interpreter. Shutdown() clears it, but only
    // if something calls Shutdown() before finalize.
    std::unordered_map<UUID, py::object> ScriptEngine::s_EntityInstances;
    // File-static rather than a class member, so it has no external linkage
    // and no initialiser-order exposure. The set is the "do not retry" memo:
    // a script that failed once is skipped forever, which is what keeps a
    // broken script from re-logging 60 times a second.
    static std::unordered_set<UUID> s_FailedScripts;

    // By value: the function strips leading/trailing whitespace in place, and
    // taking a const reference would force a copy inside anyway.
    //
    // This exists because script paths are hand-typed into a ScriptComponent
    // and pick up stray whitespace from copy-paste -- which would otherwise
    // produce a path that fails the exists() check below and permanently
    // marks the script failed.
    static std::string CleanPath(std::string path)
    {
        // Strips every control character, not just tabs and newlines. The
        // (unsigned char) cast matters: on platforms where char is signed, any
        // byte >= 128 would compare as negative and pass the test, so without the
        // cast the removal is narrower than intended.
        path.erase(std::remove_if(path.begin(), path.end(), [](char c)
                                  { return (unsigned char)c < 32; }),
                   path.end());
        // Separate first/last rather than a single trim helper: the npos case is
        // handled explicitly below, which a naive substr would wrap around on.
        size_t first = path.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = path.find_last_not_of(" \t\r\n");
        // The +1 converts the last-character INDEX to a LENGTH. Getting this
        // wrong by the off-by-one drops the final character of every script path,
        // which is a genuinely hard bug to spot from a "file not found" log.
        return path.substr(first, (last - first + 1));
    }

    // Initialised explicitly to nullptr even though static storage already
    // zeroes it. The declared type is Scene*, not Entity, so the engine's
    // type requirement is visible at the definition rather than the header.
    Scene *ScriptEngine::s_ActiveScene = nullptr;

    void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
    {
        // Three cheap guards before any component access. Each one is a real
        // failure mode: a destroyed entity in the update list, a ScriptComponent
        // removed at runtime, or an entity that never had an ID.
        if (!entity.IsValid())
            return;

        // Not an error: the scene holds every entity, not just scripted ones, and
        // this runs per entity per frame.
        if (!entity.HasComponent<ScriptComponent>())
            return;

        // A REFERENCE, not a copy: the component carries a script path string, and
        // copying it once per entity per frame would be a real allocation cost in
        // a hot loop. Safe because the entity is alive for the rest of this call.
        auto &sc = entity.GetComponent<ScriptComponent>();

        // ScriptComponent does not imply IDComponent, and GetComponent on a
        // missing type asserts in Debug and is plain UB in Release. Bail
        // rather than read a component that was never there.
        // DEFECT GUARD, and the reason is spelled out above it: GetComponent on a
        // type the entity does not have asserts in Debug and is undefined
        // behaviour in Release. This is the cheapest possible fix for that, but
        // it means an entity whose IDComponent is absent is silently never
        // scripted -- no log, no diagnostic.
        if (!entity.HasComponent<IDComponent>())
            return;

        // The UUID is the cache key for the Python instance. Copied rather than
        // held as a reference because the map lookup below outlives the scope of
        // the component borrow in the general case.
        auto id = entity.GetComponent<IDComponent>().ID;

        // The "never retry" check, done before the instance lookup below so a
        // failed script costs one hash lookup per frame and nothing more.
        if (s_FailedScripts.find(id) != s_FailedScripts.end())
            return;

        // First-frame-only initialisation. The map doubles as the "already
        // constructed" flag, which is why there is no separate bool per entity.
        if (s_EntityInstances.find(id) == s_EntityInstances.end())
        {
            std::string cleanedPath = CleanPath(sc.ScriptPath);

            // Logged once per entity, before the file is touched, so a script that
            // never resolves still leaves a trace of what the engine tried.
            WL_CORE_INFO("ScriptEngine: Initializing Entity ID {0} with Path: {1}, Class: {2}", id, cleanedPath, sc.ScriptName);

            // Resolved through Project so scripts load out of the active
            // project's assets, not the process working directory.
            // Resolved through the active project rather than the working directory,
            // which is what lets a project move on disk without breaking its scripts.
            Project &project = Project::Get();
            // GetAssetPath accepts both project-relative and assets/-relative inputs,
            // so a ScriptComponent can store either form.
            std::filesystem::path scriptFullPath = project.GetAssetPath(cleanedPath);
            // Forces the platform's native separator. Required because the path is
            // about to be handed to PYTHON, which does not understand the "generic"
            // forward-slash form on Windows.
            scriptFullPath.make_preferred();

            // The check that decides whether a script is retried or retired. Marking
            // it failed here means a user who drops the file in later must reload the
            // scene -- there is no re-check path.
            if (!std::filesystem::exists(scriptFullPath))
            {
                WL_CORE_ERROR("Script file NOT FOUND at: {0}. Please update the ScriptComponent in your Scene.", scriptFullPath.string());
                // Retire the script rather than retry: an editor session that retries a
                // missing file every frame floods the log and hides real errors.
                s_FailedScripts.insert(id);
                return;
            }

            // The outer try covers everything Python-related that is not the class
            // instantiation itself: importing sys, mutating sys.path, and the module
            // import. Those failures are all py::error_already_set.
            try
            {
                // Keep Python module handles scoped to this GIL-protected call.
                // CPython caches imports, so this avoids persistent py::object
                // statics without repeatedly initializing the modules.
                py::module_ sys = py::module_::import("sys");
                // A COPY of sys.path (cast<py::list> constructs a new Python list), not a
                // reference to it. So the two appends below mutate a throwaway list and
                // never reach the interpreter -- see the sibling AddToPath lambda, which
                // iterates this same copy. The script import below therefore relies on
                // sys.path already containing the right entries by some other means.
                // DEFECT: appears to be dead code with a misleading comment above it.
                py::list sysPath = sys.attr("path").cast<py::list>();

                // The project's scripts directory, so a script can import a
                // sibling module by name regardless of where the project
                // lives on disk.
                // Appended to the copy described above, so this has no effect on any
                // subsequent Python import in the process.
                sysPath.append(project.GetAssetPath("scripts").string());

                std::string scriptDir = scriptFullPath.parent_path().string();
                std::string engineDir = project.GetRoot().string();

                // A lambda capturing sysPath BY REFERENCE -- the `&` matters. Written to
                // be idempotent so calling it twice does not grow sys.path unboundedly
                // across many entities.
                auto AddToPath = [&](const std::string &path)
                {
                    // Linear scan of the whole path list per call, twice per entity. Cheap at
                    // the sizes involved, and it is what keeps sys.path from duplicating an
                    // entry for every scripted entity in the scene.
                    for (auto item : sysPath)
                        // DEFECT (throw hazard, documented not fixed): sys.path entries are
                        // usually strings, but this cast is unconditional. A non-string entry
                        // (a zip importer path object, a pathlib.Path added by a script) throws
                        // py::cast_error -- which is NOT caught by the py::error_already_set
                        // handler in this function's outer try, and would propagate to the
                        // std::exception catch below, retiring a script for an unrelated reason.
                        if (item.cast<std::string>() == path)
                            return;
                    sysPath.append(path);
                };

                AddToPath(scriptDir);
                AddToPath(engineDir);

                // The file stem is the Python module name, so the file name IS the import
                // contract. A hyphenated filename cannot be imported this way.
                std::string moduleName = scriptFullPath.stem().string();
                py::module_ importlib = py::module_::import("importlib");
                py::dict modules = sys.attr("modules").cast<py::dict>();

                py::module_ scriptModule;
                // Hot-reload path: if a module of this name is already loaded, reload it
                // rather than returning the cached copy. That is what lets a user edit a
                // script and see the change without restarting.
                //
                // DEFECT (partial reload, documented not fixed): reload() re-executes the
                // module but the OLD class instances in s_EntityInstances are NOT
                // replaced -- they still hold references to the previous class object, so
                // an edited script does not take effect on already-spawned entities. Only
                // newly spawned entities get the new code.
                if (modules.contains(moduleName.c_str()))
                    // Returns the same module object, mutated in place; the assignment is for
                    // symmetry with the import branch below.
                    scriptModule = importlib.attr("reload")(modules[moduleName.c_str()]);
                else
                    scriptModule = py::module_::import(moduleName.c_str());

                try
                {
                    // Attribute lookup, not getattr with a default: a missing class name is a
                    // hard error caught immediately below rather than a silent no-script.
                    py::object scriptClass = scriptModule.attr(sc.ScriptName.c_str());
                    // Constructed with the Entity bound as the constructor argument -- the
                    // scripting contract is that every script class takes an entity.
                    //
                    // DEFECT (leak, documented not fixed): operator[] default-constructs a
                    // py::object before assigning, so a failed construction leaves a null
                    // handle in the map. Combined with the guard at line ~50 the null entry
                    // is treated as "already initialised" and the entity is silently never
                    // scripted. try{} below is what actually prevents the insert.
                    s_EntityInstances[id] = scriptClass(py::cast(entity));
                }
                catch (py::error_already_set &e)
                {
                    // Do NOT e.restore() here. Restoring re-arms the Python
                    // error indicator, and the next pybind11 call on this thread
                    // then fails against a stale error. Mark the script failed
                    // and drop the indicator so the next entity starts clean.
                    // The whole point of NOT calling e.restore() here: pybind11's
                    // error_already_set has already fetched and cleared the Python error, and
                    // restoring would re-arm the indicator, poisoning the next pybind11 call
                    // on this thread with a stale error.
                    WL_CORE_ERROR("Script '{0}.{1}' raised during construction: {2}",
                                  moduleName, sc.ScriptName, e.what());
                    // Belt-and-braces alongside the deliberate non-restore: if anything did
                    // leave an error set, this clears it before control returns to Python.
                    PyErr_Clear();
                    s_FailedScripts.insert(id);
                }

                // Success message printed only on the path where the inner catch did not
                // fire. Note this is checked, not assumed -- the inner handler may have
                // retired the script.
                if (s_FailedScripts.find(id) == s_FailedScripts.end())
                    WL_CORE_INFO("Successfully initialized: {0}", moduleName);
            }
            // Catch ORDER IS LOAD-BEARING. py::error_already_set derives from
            // std::runtime_error, so it must precede the std::exception handler
            // below. Reversing the two would silently make this handler dead code
            // and every Python error would be reported as a binding error.
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
            }
            // The comment block above this catch is the rationale: py::cast_error and
            // py::type_error are std::exception, not error_already_set, so without
            // this handler an exception unwinds through the frame loop into
            // std::terminate and takes the editor down.
            // A bad argument to the Python constructor (py::cast_error,
            // py::type_error, ...) is NOT a py::error_already_set. Letting it
            // escape unwinds through Scene::OnUpdateRuntime and Application::Run
            // into std::terminate, which takes the whole editor down. Mark the
            // script failed instead so one bad class cannot kill the frame loop.
            catch (const std::exception &e)
            {
                WL_CORE_ERROR("Script binding error during init: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
            }
            // Last-resort handler for the same reason: an unrecognised throw type
            // from Python-land must not escape the frame loop either.
            catch (...)
            {
                WL_CORE_ERROR("Unknown error during script init");
                PyErr_Clear();
                s_FailedScripts.insert(id);
            }
        }

        // The lookup is repeated rather than reusing a reference from the block
        // above, because that block may have inserted (or the erase in a catch
        // below may have removed) an entry, and the iterator would be stale.
        auto it = s_EntityInstances.find(id);
        if (it != s_EntityInstances.end())
        {
            try
            {
                // The per-frame call. The attribute is looked up EVERY frame on the
                // instance rather than cached as a bound method -- correct but not free,
                // and it is the hot path for a scene full of scripted entities.
                it->second.attr("OnUpdateEntity")(ts);
            }
            // Same ordering requirement as above: this must come before the
            // std::exception catch. NOTE the asymmetry worth noticing: a Python-level
            // exception during UPDATE logs and clears but does NOT retire the script,
            // so it will be attempted again next frame and can spam the log.
            catch (const py::error_already_set &e)
            {
                WL_CORE_ERROR("Python Error during update: {0}", e.what());
                PyErr_Clear();
            }
            // Same reasoning as init: a cast_error/type_error from the call
            // itself is not error_already_set and would terminate the process.
            // Retire the instance so a permanently-broken update cannot spam
            // the log at 60fps forever.
            // A binding-level failure (wrong signature, bad cast) is treated as fatal
            // to the script, unlike a Python exception. Rationale: the binding will
            // not start working on its own, so retrying forever is pointless.
            catch (const std::exception &e)
            {
                WL_CORE_ERROR("Script binding error during update: {0}", e.what());
                PyErr_Clear();
                s_FailedScripts.insert(id);
                // Erase with the iterator, which is valid because nothing above inserted
                // into the map on this path. The instance's py::object destructor runs
                // here, decrementing Python's reference count.
                s_EntityInstances.erase(it);
            }
            catch (...)
            {
                WL_CORE_ERROR("Unknown error during script update");
                PyErr_Clear();
                s_FailedScripts.insert(id);
                s_EntityInstances.erase(it);
            }
        }
    }

    // Takes the UUID, not the Entity: by the time this is called the entity
    // may already be invalidated, and all the cache needs is the key.
    void ScriptEngine::OnDestroyEntity(UUID id)
    {
        // DEFECT (incomplete teardown, documented not fixed): the erase is
        // guarded, but s_FailedScripts is NOT touched here. A script that failed
        // keeps its UUID in the failed set after the entity is destroyed. If a
        // later entity reuses that UUID -- possible after a scene reload that
        // restarts ID allocation -- it would be permanently skipped. Only
        // Shutdown() clears that set.
        if (s_EntityInstances.find(id) != s_EntityInstances.end())
        {
            // No Py_DECREF dance needed: erasing the map entry destroys the py::object
            // and releases the Python reference with it.
            s_EntityInstances.erase(id);
        }
    }

    // Clears BOTH caches. Clearing only the instances would leave retired IDs
    // behind and every entity would be skipped after a shutdown/reopen cycle.
    void ScriptEngine::Shutdown()
    {
        if (Py_IsInitialized())
        {
            py::gil_scoped_acquire gil;
            s_EntityInstances.clear();
        }
        s_FailedScripts.clear();
        s_ActiveScene = nullptr;

        WL_CORE_INFO("ScriptEngine: Cleaned up instance cache.");
    }

    // Returns a raw pointer INTO the static map. DEFECT (dangling pointer
    // hazard, documented not fixed): the pointer stays valid only until the
    // next map mutation -- an insert or an erase on ANY entity can rehash or
    // move the bucket. A caller holding this across a frame boundary can read
    // freed memory. A reference or a stable handle would be safe.
    py::object* ScriptEngine::GetInstance(UUID id)
    {
        auto it = s_EntityInstances.find(id);
        if (it != s_EntityInstances.end())
            // Address of the mapped py::object, not a copy: callers use it to invoke
            // methods on the live instance.
            return &it->second;
        // Null for "never constructed or already destroyed". Note this does not
        // distinguish the two, and does not consult s_FailedScripts.
        return nullptr;
    }
}