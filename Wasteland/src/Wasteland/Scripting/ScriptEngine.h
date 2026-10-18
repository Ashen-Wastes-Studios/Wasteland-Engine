// First include, before the engine headers, because s_EntityInstances below
        // is a std::unordered_map and wants the real type rather than a
        // forward declaration. Note this header has NO #pragma once and no
        // include guard of any kind: it is only safe because every consumer
        // includes it once. DEFECT (missing include guard, documented not
        // fixed): including this header twice in one translation unit is a
        // redefinition error on the class. Compare Project.h and
        // RenderCommand.h, which both use #pragma once.
#include <unordered_map>

// The key type for both static maps below. UUID is the entity's stable
        // identity, chosen over an index or a pointer because an entity can
        // move in the ECS storage without its identity changing.
#include "Wasteland/Core/UUID.h"
// Entity is taken BY VALUE in OnUpdateEntity below, so this is a real
        // dependency. Passing by value rather than by reference is deliberate:
        // a handle, with the scene's storage looked up through it.
#include "Wasteland/Scene/Entity.h"
// Timestep, also by value. Carries the frame delta so a script's
        // OnUpdateEntity(ts) receives the real frame time without the script
        // having to query a global clock.
#include "Wasteland/Core/Timestep.h"

// The pybind11 header, at the BOTTOM of the include block rather than the
// top -- pybind11 is macro-heavy and benefits from being included
// after the engine types it may interact with.
#include <pybind11/pybind11.h>

// The conventional short alias, declared here rather than inside the
// namespace so every Wasteland TU can write py:: without repeating it.
        // Appears to be duplicated in every header that needs it -- harmless
// for an alias, but a `using` declaration repeated identically in
// several headers is duplication with no shared owner.
namespace py = pybind11;

namespace Wasteland
{
    // All-static, like the Renderer3D class: a process-wide singleton whose
    // state is the two members at the bottom. No instance is ever
    // constructed, so the class has no constructor at all.
    class ScriptEngine
    {
    public:
        // The per-frame per-entity entry point, called by the scene for every
        // entity whether or not it is scripted -- so the implementation
        // bails out early for entities with no ScriptComponent. That is why
        // this is a static function called from a hot loop rather than a
        // registration the script triggers.
        static void OnUpdateEntity(Entity entity, Timestep ts);
        // Takes the UUID rather than an Entity: the entity may already be invalid
        // by the time teardown runs, and all the engine needs is the cache key.
        static void OnDestroyEntity(UUID id);

        // The scene currently driving script updates. Lets module-level helpers
        // (e.g. Wasteland.Scene.GetEntities()) resolve a scene without the
        // script having to hold one. Set by Scene before it ticks scripts.
        // An ambient scene pointer, set by Scene before it ticks scripts so a
        // module-level helper such as Wasteland.Scene.GetEntities() can
        // resolve one without the script holding a reference.
        // DEFECT (lifetime hazard, documented not fixed): this is a raw pointer
        // with no ownership and no nulling on scene destruction. If a script
        // keeps a handle obtained from GetActiveScene and the scene is
        // deleted, that handle dangles. Nothing in this header pairs the set
        // with an unset.
        static void SetActiveScene(Scene *scene) { s_ActiveScene = scene; }
        // Non-const, so a caller can mutate the scene through it -- the Python
        // helpers that use this need to read AND write entity state.
        static Scene *GetActiveScene() { return s_ActiveScene; }

        // Declared here, implemented in the .cpp. Called before Py_Finalize and
        // by the Python module's atexit hook to release the instance cache while
        // the interpreter and GIL are still available.
        static void Shutdown();

        // DEFECT (dangling pointer, documented not fixed): returns a pointer INTO
        // the static map below. That pointer stays valid only until the next
        // map mutation -- and the map is mutated on entity destruction and on
        // every script initialisation, both of which can rehash. A caller
        // holding this across a frame boundary can read freed memory.
        // Returning by value would copy the handle (and keep it alive), or an
        // opaque handle type would be the safe fix.
        static py::object* GetInstance(UUID id);

    private:
        // Initialised in the .cpp to nullptr rather than here: an inline
        // initialiser would need a complete Scene type at every use, and the
        // .cpp already has it.
        static Scene *s_ActiveScene;
        // The live Python objects, one per scripted entity. DEFECT (static
        // destruction order, documented not fixed): this map is DEFINED in the
        // .cpp with no in-class initialiser, so it is constructed during
        // static initialisation -- before Py_Initialize. That is safe for an
        // empty unordered_map, but its DESTRUCTOR runs at static teardown and
        // would decrement Python refcounts after Py_Finalize if Shutdown()
        // were not called first. The whole shutdown-ordering argument rests on
        // Shutdown() being called; nothing enforces it.
        static std::unordered_map<UUID, py::object> s_EntityInstances;
    };
}