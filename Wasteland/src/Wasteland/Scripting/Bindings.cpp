// PCH first, before anything else in the translation unit. This header also
// carries the project-wide compile definitions, so an include above it would
// change the meaning of the standard headers that follow.
#include "wlpch.h"

// The core pybind11 header. Everything else in this file is built from
// py::class_, py::enum_, py::module_, py::object and py::args.
#include <pybind11/pybind11.h>

#include <pybind11/stl.h>

// Supplies py::self and the operator-binding forms. Used exactly once in this
// file, for Quat.__mul__ below; the other operator-shaped bindings here are
// spelled as named methods instead.
#include <pybind11/operators.h>

// std::function <-> Python callable conversion. No binding in this file takes
// or returns a std::function -- the event-handler hook is a bare py::object --
// so this include is currently unused. Harmless, but it is dead weight and it
// will silently become load-bearing the first time someone binds a callback.
#include <pybind11/functional.h>

// stl_bind.h provides py::bind_vector. stl.h above cannot: its type_caster
// for std::vector is what makes an opaque vector fall back to a by-value
// list, and bind_vector is the deliberate opt-out of that behaviour. Both
// headers are needed -- stl.h for the many by-value returns elsewhere in this
// module, stl_bind.h for the five containers that must stay live.
// stl_bind.h is what py::bind_vector needs; it is a SEPARATE header from
// stl.h because stl.h installs a type_caster for std::vector that would take
// precedence over any container binding. Including both is deliberate here:
// the many by-value std::vector returns in this file (mesh vertices, graph
// nodes, skinning matrices) rely on stl.h's copy-on-return behaviour, while
// the five Visual Blocks containers below rely on bind_vector's live view.
#include <pybind11/stl_bind.h>

// ---------------------------------------------------------------------------
// NOTE: the Visual Blocks container types are declared opaque further down,
// immediately after their headers are included. PYBIND11_MAKE_OPAQUE has to
// see the complete type, and VisualBlocks/Graph.h is not included yet at the
// top of this file.
//
// Why they are opaque at all: pybind11/stl.h -- included above, because a
// dozen other bindings return std::vector by value -- converts every
// std::vector to a FRESH Python list on the way out. So
// `graph.Nodes.append(n)` appends into a temporary copy and the graph is
// unchanged when the statement returns, with no error raised. Assignment
// still works, because the setter copies back, which is what makes this a
// trap rather than an obvious break: the natural way to build a graph in
// Python is append/extend, and it silently does nothing.
//
// Scoped to these five on purpose. The by-value convenience stl.h gives the
// other call sites is worth having there; only graph building needs mutation.

// ---------------------------------------------------------------------------

// Age assurance (Wasteland.Age submodule). Read-only bindings; see
// BindingsAge.cpp. Registered first inside PYBIND11_MODULE so the submodule
// exists before any generic name below could shadow one of its enums.

// The Age submodule is declared in its own translation unit and pulled in
// here. Binding it from BindingsAge.cpp rather than inline keeps the age
// assurance logic -- and its deliberate exceptions -- in one file.
#include "Wasteland/Scripting/BindingsAge.h"

// ---------------------------------------------------------------------------

// Core

// ---------------------------------------------------------------------------

// Core singleton headers. Input/KeyCodes/MouseButtonCodes are needed because
// the Wasteland.*Key* constants exported below are taken from KeyCodes.h by
// macro expansion, so a script comparing against WL_KEY_W must compare
// against the same integer the engine uses.
#include "Wasteland/Core/Input.h"

#include "Wasteland/Core/KeyCodes.h"

#include "Wasteland/Core/MouseButtonCodes.h"

// UUID is the entity identity type. ScriptEngine::GetInstance and
// Entity::GetUUID both speak it, which is why the module exposes a bound UUID
// type with __int__/__eq__/__hash__ rather than leaving scripts to treat
// entities as opaque handles.
#include "Wasteland/Core/UUID.h"

#include "Wasteland/Core/Timestep.h"

#include "Wasteland/Core/Window.h"

#include "Wasteland/Core/Application.h"
// The Visual Blocks headers. These come BEFORE Layer.h and after the core
// includes so that the opaque-container declarations below see complete
// types -- PYBIND11_MAKE_OPAQUE cannot be applied to an incomplete type.
#include "Wasteland/VisualBlocks/BehaviorTree.h"
#include "Wasteland/VisualBlocks/Compiler.h"
// GraphSerializer is the YAML/text format layer. It is the same code path the
// editor uses, so a graph saved from Python is byte-comparable with one saved
// from the editor.
#include "Wasteland/VisualBlocks/GraphSerializer.h"
#include "Wasteland/VisualBlocks/NodeRegistry.h"
#include "Wasteland/VisualBlocks/Validator.h"

// The Visual Blocks containers, made OPAQUE so they bind as live vectors
// rather than as by-value Python lists. Must come after the includes above,
// because PYBIND11_MAKE_OPAQUE needs the complete types. Full rationale in the
// note near the top of this file.
// PYBIND11_MAKE_OPAQUE marks the type so pybind stops using its automatic
// std::vector caster. Without it, a Python list is materialised on every read
// and mutations are silently discarded -- see the note at the top of the file.
PYBIND11_MAKE_OPAQUE(Wasteland::VisualBlocks::GraphNodeList);
// Same treatment for edges. Graph authoring needs append and extend to reach
// the live vector, which a by-value list cannot offer.
PYBIND11_MAKE_OPAQUE(Wasteland::VisualBlocks::GraphLinkList);
// Graph-level variables.
PYBIND11_MAKE_OPAQUE(Wasteland::VisualBlocks::GraphVariableList);
// Blackboard entries. Opaque so a runtime can rebind a key's value from
// Python during a behaviour tree's execution.
PYBIND11_MAKE_OPAQUE(Wasteland::VisualBlocks::BlackboardKeyList);
// Per-node pin literals. This one is the most consequential of the five: a
// literal that appears to set but does not is a graph that runs the previous
// value, with no error.
PYBIND11_MAKE_OPAQUE(Wasteland::VisualBlocks::PinLiteralList);

// Layer and LayerStack come after the opaque declarations only because of the
// include ordering constraint above; nothing about Layer depends on them.
#include "Wasteland/Core/Layer.h"

#include "Wasteland/Core/LayerStack.h"

// ---------------------------------------------------------------------------

// Events

// ---------------------------------------------------------------------------

// Event hierarchy. Each concrete event type in the scene section below is
// registered with Wasteland::Event as its pybind base class, which is what
// lets a script write one isinstance() check instead of enumerating types.
#include "Wasteland/Events/Event.h"

#include "Wasteland/Events/ApplicationEvent.h"

#include "Wasteland/Events/KeyEvent.h"

#include "Wasteland/Events/MouseEvent.h"

// ---------------------------------------------------------------------------

// Math / Utils

// ---------------------------------------------------------------------------

#include <glm/glm.hpp>

// Math::DecomposeTransform is bound as a module-level function rather than a
// static on a class: it is a free function in Math.h with no class scope, so
// there is nowhere natural to hang it.
#include "Wasteland/Math/Math.h"

// PlatformUtils supplies the file dialogs. They are module-level free
// functions for the same reason -- no class to attach them to.
#include "Wasteland/Utils/PlatformUtils.h"

// ---------------------------------------------------------------------------

// Renderer

// ---------------------------------------------------------------------------

// Renderer headers. Every renderer type bound below (Shader, Texture,
// Framebuffer, Camera, Renderer2D/3D) is a Ref<> handle in C++, and the holder
// template argument on each py::class_ must match or construction throws at
// runtime.
#include "Wasteland/Renderer/RendererAPI.h"

#include "Wasteland/Renderer/RenderCommand.h"

#include "Wasteland/Renderer/VertexArray.h"

#include "Wasteland/Renderer/Buffer.h"

#include "Wasteland/Renderer/Shader.h"

#include "Wasteland/Renderer/Texture.h"

#include "Wasteland/Renderer/SubTexture2D.h"

#include "Wasteland/Renderer/Framebuffer.h"

#include "Wasteland/Renderer/GraphicsContext.h"

#include "Wasteland/Renderer/Camera.h"

#include "Wasteland/Renderer/OrthographicCamera.h"

#include "Wasteland/Renderer/EditorCamera.h"

#include "Wasteland/Scene/SceneCamera.h"

#include "Wasteland/Renderer/Renderer2D.h"

#include "Wasteland/Renderer/Renderer3D.h"

// MeshLoader brings in MeshVertex/MeshData and the OBJ parse entry points,
// which are exposed precisely so mesh import is testable headlessly.
#include "Wasteland/Renderer/MeshLoader.h"

#include "Wasteland/Animation/AnimationCurve.h"

// SkinnedMesh and Skeleton are the two ends of the skinning contract: a
// SkinnedMesh holds weights, a Skeleton holds the bind pose they index into.
#include "Wasteland/Animation/AnimationClip.h"

#include "Wasteland/Animation/AnimationStateMachine.h"

#include "Wasteland/Animation/SkinnedMesh.h"

#include "Wasteland/Animation/Skeleton.h"

// Gamepad headers. The button/axis constants are exported with a macro loop
// rather than by binding a struct, because they are compile-time integers and
// a py::enum_ would imply a closed set that a future pad could widen.
#include "Wasteland/Core/Gamepad.h"

#include "Wasteland/Core/GamepadButtonCodes.h"

// ---------------------------------------------------------------------------

// Scene

// ---------------------------------------------------------------------------

// Entity and Scene are the two halves of the scripting model: Scene owns
// entities, and every Python gameplay script receives an Entity.
#include "Wasteland/Scene/Entity.h"

#include "Wasteland/Scene/Scene.h"

// SceneSerializer is what makes a scene reproducible. Bound as both a class
// and as convenience methods on Scene, since the class form needs a Ref<Scene>
// that a script rarely holds.
#include "Wasteland/Scene/SceneSerializer.h"

// SaveGame is the RUNTIME save path. Deliberately separate from
// SceneSerializer: a save must never be able to overwrite authored content.
#include "Wasteland/Scene/SaveGame.h"

#include "Wasteland/Audio/AudioClip.h"

#include "Wasteland/Audio/Mixer.h"

#include "Wasteland/Audio/AudioSystem.h"

// Components.h is the largest single header in the project. Every component
// bound below comes from it, and the BIND_COMP_* macros at the bottom of the
// file instantiate against these exact types.
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Navigation/NavMesh.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Scene/BehaviorTreeSystem.h"

// ScriptableEntity is the base a Python script class inherits from. Its two
// bindings below hand out component references, because a script's OnCreate
// needs its transform without a scene lookup.
#include "Wasteland/Scene/ScriptableEntity.h"

// ---------------------------------------------------------------------------

// Scripting

// ---------------------------------------------------------------------------

// ScriptEngine.h. This is the bridge: it owns the interpreter, the per-entity
// instance table, and the active-scene pointer that Entity.GetScriptAttribute
// and Scene.GetActiveScene both read.
#include "Wasteland/Scripting/ScriptEngine.h"

// The namespace alias used by every binding below. Short because it appears
// thousands of times; there is no functional reason for the choice.
namespace py = pybind11;

// --- Event handler hook (defined in Application.cpp) ---

// Declared here with C linkage to match the definition; the declarations must

// sit at global scope because a linkage specification is illegal inside a block.

// C linkage so the symbol matches the definition in Application.cpp exactly.
// The declaration must sit at global scope -- a linkage specification is
// illegal inside a block -- which is why it is here rather than next to the
// binding that uses it.
extern "C" void Wasteland_SetPythonEventHandler(py::object handler);

// The getter half of the hook. Also extern "C": it is called from
// Application.cpp, not from Python.
extern "C" py::object Wasteland_GetPythonEventHandler();

// The six prefix macros below exist so the component macros can string-concat
// the qualified C++ type onto a verb and get a consistent method name. HAS/
// GET/TRYGET pair with BIND_COMP_*; ADD/ADDOR/REM with the mutators.
#define HAS_PREFIX "Has"

// TRYGET is deliberately distinct from GET: GetComponent asserts and aborts
// in debug when the component is absent, so a script that merely wants to
// probe needs a different name entirely.
#define GET_PREFIX "Get"

// ADD versus ADDOR. AddComponent emplaces and refuses a duplicate;
// AddOrReplaceComponent overwrites. Scripts that retry an add on an entity
// which may already have the component need the second.
#define TRYGET_PREFIX "TryGet"

#define ADD_PREFIX "Add"

// REM has no return value, so a script cannot tell whether anything was
// removed. Calling RemoveComponent on an entity without the component is a
// no-op rather than an error.
#define ADDOR_PREFIX "AddOrReplace"

#define REM_PREFIX "Remove"

// ------------------------------------------------------------------ helpers --

// BIND_COMP_HAS expands to a .def that calls HasComponent<T> and returns a
// bool. HasComponent does not assert, so this is the safe probe on any entity.
#define BIND_COMP_HAS(Comp) \
    .def(HAS_PREFIX #Comp, [](Wasteland::Entity &e) -> bool { \
        return e.HasComponent<Comp>(); })

// BIND_COMP_GET returns T& and therefore needs an explicit
// return_value_policy: without one pybind would copy, and a write through the
// copy would vanish. keep_alive<0,1> ties the returned reference to the Entity
// so the component cannot outlive the handle that produced it.
#define BIND_COMP_GET(Comp) \
    .def(GET_PREFIX #Comp, [](Wasteland::Entity &e) -> Comp & { \
        return e.GetComponent<Comp>(); }, \
        py::return_value_policy::reference, py::keep_alive<0, 1>())

// BIND_COMP_TRYGET is the only one of the three accessors that can fail, and
// it signals that with a null py::object -- i.e. Python None -- rather than
// raising. A missing component is an ordinary outcome for a probe.
#define BIND_COMP_TRYGET(Comp) \
    .def(TRYGET_PREFIX #Comp, [](Wasteland::Entity &e) -> py::object { \
        auto *p = e.TryGetComponent<Comp>(); \
        if (!p) return py::object(); \
        return py::cast(*p, py::return_value_policy::reference); })

// BIND_COMP_ADD passes the component by const reference and copies it in.
// keep_alive<0,1> for the same reason as GET: the new component's storage
// lives in the entity's registry.
#define BIND_COMP_ADD(Comp) \
    .def(ADD_PREFIX #Comp, [](Wasteland::Entity &e, const Comp &c) { \
        return e.AddComponent<Comp>(c); }, py::keep_alive<0, 1>())

// BIND_COMP_ADDOR differs from ADD only in which C++ call it makes.
#define BIND_COMP_ADDOR(Comp) \
    .def(ADDOR_PREFIX #Comp, [](Wasteland::Entity &e, const Comp &c) { \
        return e.AddOrReplaceComponent<Comp>(c); }, py::keep_alive<0, 1>())

// BIND_COMP_REM. No return_value_policy is needed because the lambda returns
// void -- there is no Python object to attach a lifetime to.
#define BIND_COMP_REM(Comp) \
    .def(REM_PREFIX #Comp, [](Wasteland::Entity &e) { \
        e.RemoveComponent<Comp>(); })

// Same surface as BIND_COMP_*, but named from the *unqualified* type so the

// Python-visible method is "GetHealthComponent" rather than the

// "GetWasteland::HealthComponent" that stringifying the qualified name

// produces. The caller passes the qualified type plus its bare name:

//

//     BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, HealthComponent)

//

// BIND_COMP_* keeps its current behaviour so existing code is unaffected.

// BIND_GAMEPLAY_COMP takes the qualified type AND a bare name. The BIND_COMP_*
// family stringifies whatever it is handed, so BIND_COMP_HAS(Wasteland::Health)
// would register HasWasteland::Health -- not addressable from Python.
// Token-pasting #Name onto the prefix produces HasHealth instead.
// The TRYGET body returns py::none() explicitly, where BIND_COMP_TRYGET returns a
// default-constructed py::object. Both reach Python as None; the gameplay form is
// the safer spelling because a default py::object is a null handle that a later
// cast would reject.
//
// ADD here discards the return value that BIND_COMP_ADD forwards. AddComponent
// returns the emplaced T&, so the difference is invisible to a script.
//
// NOTE: this macro used to carry those two comments INSIDE its continuation
// lines, and one of them had no trailing backslash -- so the macro terminated
// there and the following `.def(...)` was parsed as ordinary code. It compiled
// for 20 call sites and broke on the 21st, which is how it stayed invisible.
// Comments inside a multi-line macro are a trap: never let one lack the
// continuation, and prefer keeping them above the #define.
// BIND_GAMEPLAY_COMP is the second of the two accessor macros, and the
// difference from BIND_COMP_* is the SECOND ARGUMENT: a bare name used to
// build the method names, instead of the fully qualified C++ type. That is
// the whole fix for the `::`-in-a-Python-identifier problem, at the cost of
// the caller having to repeat the type name.
// Bind one gameplay component to Entity as the standard six accessors.
//
// The six expansions, in order: Has, Get, TryGet, Add, AddOr, Remove. They
// differ only in the C++ call they make (HasComponent, GetComponent,
// TryGetComponent, AddComponent, AddOrReplaceComponent, RemoveComponent) and
// in the return type, so reading one macro tells you all six.
//
// WHY THE COMMENT BLOCKS ARE OUTSIDE THE MACRO BODY. A // comment inside a
// line-continuation macro is a trap on MSVC, and a silent one. The line
// ends in a backslash so the macro continues, but the comment text is
// discarded BEFORE the continuation is processed, so the preprocessor emits
// nothing for that line. cl /EP reports it and nothing else:
//
//     warning C4010: single-line comment contains line-continuation character
//
// and the macro quietly expands to only its code lines. If the comments were
// the only thing between continuations, the macro expands to NOTHING, every
// invocation binds nothing, and the build still succeeds with zero errors --
// which is exactly what happened: 22 invocations, a clean build, and an
// Entity class with no component accessors at all. Keep all comments ABOVE
// the #define. C4010 in this file means a macro body is losing lines.

// On Get/Add/AddOr: py::keep_alive<0, 1> because the ENTITY is argument 1 and
// the returned component reference is argument 0, so keep_alive keeps the
// entity alive as long as Python holds the component handle. Without it, a
// component obtained from an entity that then dies leaves Python holding a
// reference into freed storage.
//
// TryGet returns py::object -- None or a reference -- because pybind cannot
// deduce a single return type across the None branch. Get returns Comp&
// directly and ASSERTS if the component is absent; TryGet is the safe form.
#define BIND_GAMEPLAY_COMP(Comp, Name) \
    .def(HAS_PREFIX #Name, [](Wasteland::Entity &e) -> bool { \
        return e.HasComponent<Comp>(); }) \
    .def(GET_PREFIX #Name, [](Wasteland::Entity &e) -> Comp & { \
        return e.GetComponent<Comp>(); }, \
        py::return_value_policy::reference, py::keep_alive<0, 1>()) \
    .def(TRYGET_PREFIX #Name, [](Wasteland::Entity &e) -> py::object { \
        auto *p = e.TryGetComponent<Comp>(); \
        if (!p) return py::none(); \
        return py::cast(*p, py::return_value_policy::reference); }) \
    .def(ADD_PREFIX #Name, [](Wasteland::Entity &e, const Comp &c) { \
        e.AddComponent<Comp>(c); }, py::keep_alive<0, 1>()) \
    .def(ADDOR_PREFIX #Name, [](Wasteland::Entity &e, const Comp &c) { \
        e.AddOrReplaceComponent<Comp>(c); }, py::keep_alive<0, 1>()) \
    .def(REM_PREFIX #Name, [](Wasteland::Entity &e) { \
        e.RemoveComponent<Comp>(); })

// ============================================================================

// The single module object. Everything registered inside this brace lives
// under the name `Wasteland` when the extension is imported.
PYBIND11_MODULE(Wasteland, m)

// Opening brace of PYBIND11_MODULE. Indentation in this file is inconsistent
// -- several bindings below sit at column 0 inside this brace -- which is
// cosmetic only but makes region boundaries harder to see than they should be.
{
    // Wasteland.Age -- age assurance. Bound FIRST so its submodule exists
    // before any later binding could shadow one of the generic names.
    //
    // Read-only on everything that enforces a restriction; see
    // BindingsAge.cpp for why, and for the two deliberate exceptions
    // (EuScope and Refresh).
    // Registered before everything else so the submodule object exists ahead of
    // any later binding that could reuse one of its names.
    Wasteland::BindAge(m);

    // ------------------------------------------------------ Navigation (navmesh)
    // Bound early because NavMeshConfig and NavMesh take Vec3/Vec2 arguments,
    // so the math types must already exist by the time these lambdas are
    // INSTANTIATED -- not merely declared above.
    // NavMeshConfig is bound to the ROOT module, not a Navigation submodule, even
    // though NavMeshConfig, NavPathPoint and NavMesh are the only navigation
    // types here. So a script writes Wasteland.NavMesh, not
    // Wasteland.Navigation.NavMesh -- inconsistent with the Animation, Gamepad,
    // Audio and VisualBlocks submodules further down.
    py::class_<Wasteland::Navigation::NavMeshConfig>(m, "NavMeshConfig")
        // Default-constructed and fully mutable: a script builds a config, tunes it,
        // and hands it to Scene.BakeNavMesh. Every field is readwrite, so the whole
        // config is authorable from Python with no missing knob.
        .def(py::init<>())
        .def_readwrite("CellSize", &Wasteland::Navigation::NavMeshConfig::CellSize)
        .def_readwrite("AgentHeight", &Wasteland::Navigation::NavMeshConfig::AgentHeight)
        .def_readwrite("StepHeight", &Wasteland::Navigation::NavMeshConfig::StepHeight)
        .def_readwrite("AgentRadius", &Wasteland::Navigation::NavMeshConfig::AgentRadius)
        .def_readwrite("MaxSampleHeight", &Wasteland::Navigation::NavMeshConfig::MaxSampleHeight);

    // NavPathPoint is a plain value struct: a world position plus a flag saying
    // whether it is a corner the path had to turn at, rather than an
    // interpolated point along a segment. Scripts that follow a path use IsCorner
    // to decide when to re-aim.
    py::class_<Wasteland::Navigation::NavPathPoint>(m, "NavPathPoint")
        .def(py::init<>())
        // Position is a plain vec3 member, so def_readwrite works and both get and
        // set go through the field directly. No reference_internal needed -- the value
        // lives in the NavPathPoint itself, not in a container that can reallocate.
        .def_readwrite("Position", &Wasteland::Navigation::NavPathPoint::Position)
        .def_readwrite("IsCorner", &Wasteland::Navigation::NavPathPoint::IsCorner);

    // Not copyable by default under pybind (NavMesh holds a raw Scene pointer
    // and a large grid), and nobody should be copying one from Python.
    py::class_<Wasteland::Navigation::NavMesh>(m, "NavMesh")
        // py::init<>() with no arguments: a NavMesh is baked afterwards. Constructing
        // one does no work; Bake is what populates it.
        .def(py::init<>())
        // Bake takes the Scene by reference, not by handle, so a script cannot bake
        // against a scene it does not own. The config has a default, which means a
        // script can bake with no arguments at all and get whatever NavMeshConfig's
        // default member values are -- worth knowing, because those defaults decide
        // cell size and agent radius.
        .def("Bake",
             [](Wasteland::Navigation::NavMesh &nm, Wasteland::Scene &sc,
                 const Wasteland::Navigation::NavMeshConfig &cfg) {
                 return nm.Bake(sc, cfg);
             },
             py::arg("scene"), py::arg("config") = Wasteland::Navigation::NavMeshConfig())
        // Clear and IsValid are the two state checks a script needs to avoid querying
        // a mesh that was never baked or has been invalidated by a level change.
        .def("Clear", &Wasteland::Navigation::NavMesh::Clear)
        .def("IsValid", &Wasteland::Navigation::NavMesh::IsValid)
        // FindPath returns an empty py::list rather than None on failure. The
        // distinction matters: an empty list means 'no route exists', which is a
        // gameplay outcome, while None would suggest the call itself failed.
        .def("FindPath",
             [](Wasteland::Navigation::NavMesh &nm, const glm::vec3 &start,
                 const glm::vec3 &goal) {
                 // Return an empty list on failure rather than raising: a
                 // missing path is a normal gameplay outcome (target moved,
                 // region disconnected), not an exception.
                 // The C++ signature is bool-returning with an out-parameter, which pybind
                 // cannot bind directly -- a raw pointer to std::vector has no caster. The
                 // lambda adapts it into (list) or (empty list).
                 std::vector<Wasteland::Navigation::NavPathPoint> path;
                 if (!nm.FindPath(start, goal, path))
                     return py::list();
                 py::list result;
                 // Each point is appended as a bound NavPathPoint, so the script receives real
                 // objects with .Position and .IsCorner rather than tuples. Appending copies
                 // each element; the source vector is untouched.
                 for (const auto &p : path)
                     result.append(p);
                 return result;
             },
             py::arg("start"), py::arg("goal"))
        // HasLineOfWalk is the cheap pre-check: if it is false a FindPath would fail
        // anyway, and this avoids allocating a result vector for the common case of
        // 'can I just walk there'.
        .def("HasLineOfWalk", &Wasteland::Navigation::NavMesh::HasLineOfWalk,
             py::arg("from"), py::arg("to"))
        // FindNearestWalkable returns None on failure. The failure case here is
        // genuinely exceptional (a point far outside any baked region), so None is
        // the right signal rather than an empty list -- there is no list to return.
        .def("FindNearestWalkable",
             [](Wasteland::Navigation::NavMesh &nm, const glm::vec3 &point) -> py::object {
                 glm::vec3 out(0.0f);
                 if (!nm.FindNearestWalkable(point, out))
                     return py::none();
                 return py::cast(out);
             },
             py::arg("point"))
        // SampleHeight also returns None on miss, for the same reason: the C++ form
        // writes through a float& that pybind cannot surface as a return value.
        .def("SampleHeight",
             [](Wasteland::Navigation::NavMesh &nm, const glm::vec3 &point) -> py::object {
                 float h = 0.0f;
                 if (!nm.SampleHeight(point, h))
                     return py::none();
                 return py::cast(h);
             },
             py::arg("point"))
        .def("GetWalkableCellCount", &Wasteland::Navigation::NavMesh::GetWalkableCellCount);


    // ----------------------------------------------------------- Math vectors --
    // Vec2 is deliberately minimal -- no arithmetic, no operators. Scripts that
    // need vector maths use Vec3, which does have Length and Normalize. An
    // incomplete Vec2 is a gap, not an oversight: the 2D colliders below only
    // ever store offsets and sizes.
    py::class_<glm::vec2>(m, "Vec2")
        .def(py::init<float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f)
        .def_readwrite("x", &glm::vec2::x)
        .def_readwrite("y", &glm::vec2::y);

// NOTE: this binding is at column 0 while its neighbours (Vec2 at 361, Vec4
// at 459) are indented. Purely cosmetic, but it means a search for the math
// section cannot rely on indentation.
py::class_<glm::vec3>(m, "Vec3")
        // Every component defaults to 0.0f, so Vec3() is the origin and Vec3(1.0) is
        // (1, 0, 0). The defaults are the reason a two-argument call compiles.
        .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
        .def_readwrite("x", &glm::vec3::x)
        .def_readwrite("y", &glm::vec3::y)
        .def_readwrite("z", &glm::vec3::z)
        // Length and LengthSquared are the two forms every script reaches for: the
        // first for comparisons against a threshold, the second when avoiding a
        // sqrt in a hot loop.
        .def("Length", [](const glm::vec3 &v) { return glm::length(v); })
        .def("LengthSquared", [](const glm::vec3 &v) { return glm::dot(v, v); })
        // Normalize mutates in place and returns None. That is the one shape in this
        // math block that is easy to misuse: `v.Normalize()` normalises v, it does
        // not return a normalised copy, so writing `u = v.Normalize()` sets u to None.
        .def("Normalize",
             [](glm::vec3 &v) {
                 float len = glm::length(v);
                 // The epsilon guard. A zero-length vector has no direction, and dividing by
                 // zero would fill every component with NaN rather than raising -- and a NaN
                 // vector propagates silently through the whole transform stack.
                 if (len > 1e-6f)
                     v /= len;
             });

// glm::quat. Added alongside the Vec types because animation stores bone
    // rotation as a quaternion, and a BoneTransform with no way to construct
    // or compose one is unusable from a script -- every animation call site
    // would have to be C++.
    //
    // Normalize is a METHOD here, not automatic, because a script legitimately
    // builds up an unnormalized quaternion while multiplying and should only
    // pay for the normalize when it asks. gltf and other imports carry
    // unnormalized values that must be fixed deliberately.
    // Quat. Registered here rather than in the Animation submodule because it is
    // a math type that scripts construct independently of any animation asset.
    py::class_<glm::quat>(m, "Quat")
        .def(py::init<>())
        // The four-component constructor defaults w to 1.0 and xyz to 0, so Quat()
        // and Quat(0,0,0,1) are the same identity rotation. This is what makes a
        // default-constructed BoneTransform well-formed.
        .def(py::init<float, float, float, float>(),
             py::arg("x") = 0.0f, py::arg("y") = 0.0f,
             py::arg("z") = 0.0f, py::arg("w") = 1.0f)
        .def_readwrite("x", &glm::quat::x)
        .def_readwrite("y", &glm::quat::y)
        .def_readwrite("z", &glm::quat::z)
        .def_readwrite("w", &glm::quat::w)
        // FromAxisAngle is a STATIC factory, so it takes no self. Binding it
        // as a plain member method made pybind count (self, axis, radians)
        // against two annotations and fail as C2338 -- py::staticmethod is
        // what tells it the first parameter does not exist.
        // FromAxisAngle is def_static, not def. A plain .def would make pybind count
        // self as the first parameter and then try to match it against the two
        // annotated arguments, which is a static-assert failure at compile time.
        .def_static("FromAxisAngle",
                    [](const glm::vec3 &axis, float radians) {
                        // The axis is normalised inside the binding. glm::angleAxis does not do it,
                        // and an unnormalised axis produces a rotation that is not a rotation.
                        return glm::angleAxis(radians, glm::normalize(axis));
                    }, py::arg("axis"), py::arg("radians"))
        // In-place Normalize, matching Vec3.Normalize above: mutates and returns
        // None.
        .def("Normalize", [](glm::quat &q) {
            float lengthSquared = glm::dot(q, q);
            // A zero quaternion has no rotation to normalize; leaving it be
            // beats dividing by zero and producing NaN.
            if (lengthSquared > 1e-8f)
                q = glm::normalize(q);
        })
        .def("Length", [](const glm::quat &q) { return glm::length(q); })
        .def("Conjugate", [](const glm::quat &q) { return glm::conjugate(q); })
        // Inverse for a unit quaternion is just the conjugate, but for a general one
        // it is conjugate/|q|^2 -- hence the dot product rather than a bare
        // conjugate call.
        .def("Inverse", [](const glm::quat &q) {
            float lengthSquared = glm::dot(q, q);
            // A degenerate quaternion has no inverse. Returning identity rather than
            // dividing by ~0 is the difference between a script continuing with a
            // harmless rotation and one that ends up with NaN transforms.
            if (lengthSquared <= 1e-8f)
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            return glm::conjugate(q) / lengthSquared;
        })
        // IsNormalized compares against 1.0f with a 1e-3 tolerance rather than for
        // exact equality. A quaternion built by repeated multiplication drifts, so an
        // exact check would report False for a perfectly usable rotation.
        .def("IsNormalized", [](const glm::quat &q) {
            return std::abs(glm::length(q) - 1.0f) < 1e-3f;
        })
        // Slerp between two rotations. Exposed because a script blending by
        // hand must slerp -- lerping the components gives a non-unit result
        // and a rotation that changes speed through the interval.
        .def("Slerp", [](const glm::quat &a, const glm::quat &b, float t) {
            return glm::slerp(a, b, t);
        }, py::arg("other"), py::arg("t"))
        .def("Dot", [](const glm::quat &a, const glm::quat &b) { return glm::dot(a, b); })
        // Quaternion composition is the ONE operator bound, and it uses
        // py::self * py::self.
        //
        // Registering several .def("__mul__", ...) overloads on one class does
        // not accumulate: each REPLACES the previous, so the last one written
        // is the only one that works. Trying to cover quat*quat, quat*vec3 and
        // quat*float as overloads leaves exactly one alive -- silently. This
        // is a pybind limitation, not a glm one.
        //
        // The mixed-type products stay available as named methods below, which
        // have no such conflict.
        // The ONLY operator bound on Quat. pybind's operator machinery replaces
        // rather than accumulates for the same dunder, so multiple __mul__ overloads
        // would leave only the last alive -- which is why the mixed products below
        // are named methods instead.
        .def(py::self * py::self)
        // Rotate a vector by this quaternion. glm spells this q * v.
        // glm spells vector rotation q * v, but Python scripts read better as a verb.
        // Named RotateVector so it cannot collide with the __mul__ above.
        .def("RotateVector", [](const glm::quat &q, const glm::vec3 &v) {
            return q * v;
        }, py::arg("v"))
        .def("ScaleRotation", [](const glm::quat &q, float s) {
            return q * s;
        }, py::arg("scale"))
        // __repr__ is built with a stringstream, not Python-style formatting.
        // Writing ss << with {} placeholders does not compile here: "{}" is not a
        // format specifier in C++.
        .def("__repr__", [](const glm::quat &q) {
            // Built with a stringstream rather than Python-style format():
            // this is C++, and "{}" is not a placeholder here. Using ".format"
            // fails as C2228 "left of '.format' must have class/struct/union".
            std::stringstream ss;
            ss << "Quat(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ")";
            return ss.str();
        });

// Vec4, again minimal. It exists because Mat4 and the colour uniforms
// (SetFloat4, tintColor) need a four-component carrier, not because scripts
// do 4D maths.
py::class_<glm::vec4>(m, "Vec4")
        .def(py::init<float, float, float, float>(),
             py::arg("x") = 0.0f, py::arg("y") = 0.0f,
             py::arg("z") = 0.0f, py::arg("w") = 0.0f)
        .def_readwrite("x", &glm::vec4::x)
        .def_readwrite("y", &glm::vec4::y)
        .def_readwrite("z", &glm::vec4::z)
        .def_readwrite("w", &glm::vec4::w);

// ======================================== Renderer: Mesh import =========
    // The OBJ parser is exposed to Python so mesh import is testable in a
    // headless process: a test can parse a string, assert vertex counts and
    // normals, and confirm the error messages, with no GL context and no
    // window. ParseOBJFromString is the important one -- ParseOBJ only adds
    // file I/O on top of logic that is already covered.
    // def_property, not def_readwrite: the C++ fields are raw C arrays, and
    // pybind11's def_readwrite needs a real member pointer plus a matching
    // getter/setter pair it can deduce. A property with explicit lambdas
    // converts to/from glm types, which is what callers actually want.
    // MeshVertex is the interchange format for imported geometry. def_property
    // rather than def_readwrite throughout, because Position/Normal/TexCoord are
    // C ARRAYS -- pybind cannot bind an array member directly.
    py::class_<Wasteland::MeshVertex>(m, "MeshVertex")
        .def(py::init<>())
        // The getter reconstructs a glm::vec3 from three floats. Returning by value
        // is correct here: MeshVertex has no lifetime to tie a reference to, and a
        // temporary Vec3 would dangle immediately if it were a reference.
        .def_property("Position",
                      [](const Wasteland::MeshVertex &v) { return glm::vec3(v.Position[0], v.Position[1], v.Position[2]); },
                      // The setter writes component-wise. There is no memcpy shortcut because the
                      // array is three floats and the source is a glm::vec3 whose layout pybind
                      // does not guarantee to match.
                      [](Wasteland::MeshVertex &v, const glm::vec3 &p) {
                          v.Position[0] = p.x; v.Position[1] = p.y; v.Position[2] = p.z;
                      })
        .def_property("Normal",
                      [](const Wasteland::MeshVertex &v) { return glm::vec3(v.Normal[0], v.Normal[1], v.Normal[2]); },
                      [](Wasteland::MeshVertex &v, const glm::vec3 &n) {
                          v.Normal[0] = n.x; v.Normal[1] = n.y; v.Normal[2] = n.z;
                      })
        .def_property("TexCoord",
                      [](const Wasteland::MeshVertex &v) { return glm::vec2(v.TexCoord[0], v.TexCoord[1]); },
                      [](Wasteland::MeshVertex &v, const glm::vec2 &t) {
                          v.TexCoord[0] = t.x; v.TexCoord[1] = t.y;
                      });

// MeshData is the container that comes out of the OBJ parser. Its two vector
// members are exposed through lambdas rather than member pointers because
// def_property_readonly with a plain pointer needs a type pybind can deduce a
// getter for, and these are std::vector of a bound type.
py::class_<Wasteland::MeshData>(m, "MeshData")
        .def(py::init<>())
        // Vertices and Indices are COPY-OUT lists -- the same trap as
        // SkinnedMesh.Vertices and ItemDefinition.Tags. Appending to either from
        // Python mutates a temporary. There is no setter and no mutator here, so a
        // script that wants to build MeshData by hand cannot; it must parse text.
        .def_property_readonly("Vertices", [](const Wasteland::MeshData &d) { return d.Vertices; })
        .def_property_readonly("Indices", [](const Wasteland::MeshData &d) { return d.Indices; })
        // SourcePath needs a lambda: def_property_readonly with a plain
        // member pointer only works for types pybind already has a caster
        // for, and the const-qualification mismatch on a std::string member
        // is not one it deduces.
        .def_property_readonly("SourcePath", [](const Wasteland::MeshData &d) { return d.SourcePath; })
        // VertexCount and TriangleCount are derived, exposed as properties rather
        // than fields so they cannot drift out of sync with the vectors.
        .def_property_readonly("VertexCount", [](const Wasteland::MeshData &d) { return d.Vertices.size(); })
        .def_property_readonly("TriangleCount", &Wasteland::MeshData::GetTriangleCount)
        .def_property_readonly("IsValid", &Wasteland::MeshData::IsValid)
        // BoundsMin/BoundsMax as properties because the underlying members are
        // float arrays again. They are only meaningful after RecomputeBounds; a
        // freshly constructed MeshData has whatever the constructor left there.
        .def_property_readonly("BoundsMin", [](const Wasteland::MeshData &d) { return glm::vec3(d.BoundsMin[0], d.BoundsMin[1], d.BoundsMin[2]); })
        .def_property_readonly("BoundsMax", [](const Wasteland::MeshData &d) { return glm::vec3(d.BoundsMax[0], d.BoundsMax[1], d.BoundsMax[2]); })
        .def("RecomputeBounds", &Wasteland::MeshData::RecomputeBounds);

// Free functions, in a tiny namespace object so the module namespace does
    // not fill with three bare parse entry points.
    // A submodule so two parse entry points do not sit at the top level next to
    // Input and Renderer2D. Note the inconsistency: NavMesh above is at the root
    // while Mesh is a submodule.
    py::module_ meshModule = m.def_submodule("Mesh", "OBJ mesh import");

// ParseOBJ takes the OBJ source as TEXT, not a path. That is the whole reason
// this binding exists: mesh parsing becomes testable in a headless process
// with no temp file and no filesystem.
meshModule.def("ParseOBJ",
                   [](const std::string &text, const std::string &virtualPath) {
                       Wasteland::MeshData mesh;
                       std::string error;
                       bool ok = Wasteland::ParseOBJFromString(text, virtualPath, mesh, error);
                       // The (ok, mesh, error) triple is a deliberate shape. Returning just the mesh
                       // would lose the message on failure; raising would make a malformed asset an
                       // exception, and a bad asset is user data, not a bug.
                       // Returns (ok, mesh, error) so a caller gets the mesh
                       // even on success and always gets a usable message on
                       // failure -- no exception, because a bad asset is a
                       // user-facing condition, not a bug.
                       return py::make_tuple(ok, py::cast(mesh), error);
                   },
                   py::arg("text"), py::arg("virtualPath") = "<memory>");

// LoadOBJ is the thin file-I/O wrapper over the same parser. It is listed
// separately rather than hidden behind ParseOBJ so a script can choose.
meshModule.def("LoadOBJ",
                   [](const std::string &filepath) {
                       Wasteland::MeshData mesh;
                       std::string error;
                       bool ok = Wasteland::LoadOBJ(filepath, mesh, error);
                       return py::make_tuple(ok, py::cast(mesh), error);
                   },
                   py::arg("filepath"));

// Mat4 is the weakest binding in the file and the comment above it says so
// indirectly. It has a default constructor, an Identity factory, and
// __getitem__ -- and nothing else. No __setitem__, no __len__, no indexing by
// row/column pair, and crucially no way to BUILD a matrix other than identity.
py::class_<glm::mat4>(m, "Mat4")
        .def(py::init<>())
        .def_static("Identity", []() { return glm::mat4(1.0f); })
        // __getitem__ reinterprets the matrix as a flat float array of 16. That
        // assumes column-major, contiguous storage with no padding -- true for glm
        // on every platform the engine targets, but an assumption rather than a
        // guarantee.
        .def("__getitem__", [](const glm::mat4 &m, size_t i) {
            // The bounds check raises index_error, which becomes IndexError in Python --
            // so a script indexing past the end gets a Pythonic exception rather than
            // reading whatever memory follows.
            if (i >= 16) throw py::index_error();
            return reinterpret_cast<const float*>(&m[0][0])[i];
        });

// --------------------------------------------------------------- Core: UUID --
    // UUID as a bound class rather than a bare integer. A script that receives a
    // UUID back from GetScriptAttribute or stores one in a dict gets a typed
    // value with __eq__ and __hash__.
    py::class_<Wasteland::UUID>(m, "UUID")
        .def(py::init<>())
        .def(py::init<uint64_t>(), py::arg("value"))
        .def("__int__", [](const Wasteland::UUID &u) { return (uint64_t)u; })
        .def("__eq__", [](const Wasteland::UUID &a, const Wasteland::UUID &b) { return a == b; })
        // __hash__ truncates a uint64 to size_t. On a 32-bit build that would collide
        // more often; on the 64-bit targets the engine ships it is exact. Note also
        // that __eq__ is defined by hand rather than left to pybind, so both halves
        // are consistent.
        .def("__hash__", [](const Wasteland::UUID &u) { return (size_t)(uint64_t)u; });

// ------------------------------------------------------------ Core: Timestep --
    // Timestep is bound as a class, not converted to float everywhere. That lets
    // a script keep a frame time as a first-class value and still do arithmetic
    // on it via __mul__/__rmul__.
    py::class_<Wasteland::Timestep>(m, "Timestep")
        .def(py::init<float>(), py::arg("time") = 0.0f)
        .def("GetSeconds", &Wasteland::Timestep::GetSeconds)
        .def("GetMilliseconds", &Wasteland::Timestep::GetMilliseconds)
        .def("__float__", [](const Wasteland::Timestep &ts) { return (float)ts; })
        // __rmul__ exists alongside __mul__ so `2 * ts` works. Without it Python
        // tries the reflected operation on float first, fails, and raises -- which
        // reads as a pybind limitation rather than a missing overload.
        .def("__mul__", [](const Wasteland::Timestep &ts, float s) { return ts.GetSeconds() * s; })
        .def("__rmul__", [](const Wasteland::Timestep &ts, float s) { return ts.GetSeconds() * s; });

// ---------------------------------------------------------- Animation --
    // A submodule, like Gamepad, so the animation types do not crowd the
    // top-level namespace alongside Input and Mesh.
    // The Animation submodule covers Skeleton, Clip, Animator and the state
    // machine. It is registered late -- after Mesh and UUID -- because nothing in
    // it is needed by an earlier binding.
    //
    // Named Wasteland.Animation, matching the C++ namespace, so a script reads
    // the same whether it is in Python or C++.
    py::module_ anim = m.def_submodule("Animation", "Skeleton, clips, and animation runtime");

// The four animation enums are registered before any class that stores one,
// so AnimationState.Speed and friends can be compared against them.
py::enum_<Wasteland::Animation::Interpolation>(anim, "Interpolation")
        .value("Step", Wasteland::Animation::Interpolation::Step)
        .value("Linear", Wasteland::Animation::Interpolation::Linear)
        .value("Cubic", Wasteland::Animation::Interpolation::Cubic);

py::enum_<Wasteland::Animation::TrackChannel>(anim, "TrackChannel")
        .value("Position", Wasteland::Animation::TrackChannel::Position)
        .value("Rotation", Wasteland::Animation::TrackChannel::Rotation)
        .value("Scale", Wasteland::Animation::TrackChannel::Scale);

py::enum_<Wasteland::Animation::AnimationCurve::Extrapolation>(anim, "Extrapolation")
        .value("Clamp", Wasteland::Animation::AnimationCurve::Extrapolation::Clamp)
        .value("Linear", Wasteland::Animation::AnimationCurve::Extrapolation::Linear)
        .value("Loop", Wasteland::Animation::AnimationCurve::Extrapolation::Loop);

py::enum_<Wasteland::Animation::Animator::BlendMode>(anim, "BlendMode")
        .value("Override", Wasteland::Animation::Animator::BlendMode::Override)
        .value("Interpolate", Wasteland::Animation::Animator::BlendMode::Interpolate)
        .value("Additive", Wasteland::Animation::Animator::BlendMode::Additive);

// BONE_NONE is exported as a plain module attribute rather than as an enum
// value, because it is the sentinel that BoneIndex.IsValid() returns false
// for. It is -1, so it must be an int to compare against an int field.
anim.attr("BONE_NONE") = -1;

// Matrix conversion for the animation bindings.
    //
    // Wasteland.Mat4 is bound as an opaque stub with no element access, so a
    // script holding a glm::mat4 cannot read a single value out of it. Rather
    // than change the existing Mat4 binding (used elsewhere), the animation
    // functions below return matrices as a list of four Vec4 COLUMNS, which is
    // how glm stores them and therefore needs no transpose.
    //
    // Column order, not row order, matters: the translation lives in
    // column 3. Returning rows instead would put it in element [3][0] and any
    // script reading a bone's world position would read zero.
    // mat4ToColumns: the animation bindings return matrices as a list of four
    // Vec4 COLUMNS rather than as a Mat4. Two reasons, both practical: Mat4 is
    // nearly opaque (see above), and glm stores translation in column 3, so
    // column-major output needs no transpose.
    auto mat4ToColumns = [](const glm::mat4 &m) {
        py::list columns;
        for (int c = 0; c < 4; ++c)
            columns.append(py::make_tuple(m[0][c], m[1][c], m[2][c], m[3][c]));
        return columns;
    };

// The world POSITION of a bone, which is what gameplay code actually
    // wants -- a muzzle point, an IK target, a footstep test. Reading it
    // straight out of a matrix column means scripts never index matrices at
    // all.
    // mat4Translation reaches past the columns entirely and returns just the
    // world position. This is the accessor gameplay code actually wants -- a
    // muzzle point, an IK target -- and it means scripts never index a matrix.
    auto mat4Translation = [](const glm::mat4 &m) {
        return glm::vec3(m[3][0], m[3][1], m[3][2]);
    };

// The inverse of mat4ToColumns, so a matrix can make a round trip through
    // Python and come back intact.
    //
    // Without this, ComputeSkinningMatrices -> Skin was a one-way street: the
    // first handed back lists and the second only accepted glm::mat4, so
    // skinning a mesh from a script was impossible despite both halves being
    // bound.
    //
    // Columns, not rows -- the transpose here is deliberate, and getting it
    // backwards yields a matrix that still looks like plausible numbers while
    // rotating geometry about the wrong axes.
    // mat4FromColumns is the inverse of mat4ToColumns, which is what makes a
    // matrix able to make a round trip through Python. Without it, Skin could
    // not accept what ComputeSkinningMatrices produced.
    auto mat4FromColumns = [](const py::handle &item) {
        glm::mat4 result(1.0f);
        // py::object has no operator[], so each element is pulled through a
        // py::sequence, which does. Casting straight to py::sequence is not
        // safe here: a caller may hand back a generator, and indexing a
        // generator raises TypeError.
        // py::reinterpret_borrow rather than a cast: this is a BORROW of an existing
        // object, not a new reference. Casting outright would fail for a generator,
        // since a generator has no __getitem__.
        py::sequence columns = py::reinterpret_borrow<py::sequence>(item);
        for (int c = 0; c < 4 && c < py::len(columns); ++c) {
            py::sequence column = columns[c];
            for (int r = 0; r < 4 && r < py::len(column); ++r) {
                // py::float_(...).cast<float>() per element. Each component is converted
                // individually so a Python int or a numpy scalar in the list works; a bulk
                // cast of the whole sequence would not.
                result[c][r] = py::float_(column[r]).cast<float>();
            }
        }
        return result;
    };

// ---- BoneIndex ----
    // Exposed as a class so a bone handle is a distinct type rather than a
    // bare int, but with __int__ and __index__ so scripts can use it
    // directly as a list index.
    // A distinct Python type so a bone handle cannot be confused with an int in
    // a signature, but __int__/__index__ keep it usable as one.
    py::class_<Wasteland::Animation::BoneIndex>(anim, "BoneIndex")
        .def(py::init<>())
        // Constructing from an int is what lets a script pass a plain number where a
        // BoneIndex is expected, instead of having to build the wrapper first.
        .def(py::init([](int32_t v) { return Wasteland::Animation::BoneIndex(v); }))
        .def_property_readonly("Value", [](const Wasteland::Animation::BoneIndex &b) { return b.Value; })
        .def_property_readonly("IsValid", [](const Wasteland::Animation::BoneIndex &b) { return b.IsValid(); })
        .def("__int__", [](const Wasteland::Animation::BoneIndex &b) { return b.Value; })
        // __index__ is what makes a BoneIndex usable as a list index
        // directly (pose.Bones[bone]). __int__ alone does not -- Python
        // requires __index__ for that.
        // __index__ is the one that matters for subscripting. Python requires
        // __index__, not __int__, for `pose.Bones[bone]`; a class with only __int__
        // raises TypeError there.
        .def("__index__", [](const Wasteland::Animation::BoneIndex &b) { return b.Value; })
        .def("__eq__", [](const Wasteland::Animation::BoneIndex &a, const Wasteland::Animation::BoneIndex &b) { return a == b; })
        .def("__repr__", [](const Wasteland::Animation::BoneIndex &b) {
            return "BoneIndex(" + std::to_string(b.Value) + ")";
        });

// ---- BoneTransform / Pose ----
    // BoneTransform is TRS, not a matrix. Keeping it decomposed is what lets a
    // script author a keyframe without decomposing a composed transform first.
    py::class_<Wasteland::Animation::BoneTransform>(anim, "BoneTransform")
        .def(py::init<>())
        // The convenience constructor takes TRS in that order. Note it is a lambda,
        // not py::init<vec3, quat, vec3>: BoneTransform is likely an aggregate, and
        // pybind's init machinery brace-initialises, which fails for aggregates in
        // the same way it does for BoneWeight below.
        .def(py::init([](const glm::vec3 &t, const glm::quat &r, const glm::vec3 &s) {
            return Wasteland::Animation::BoneTransform(t, r, s);
        }))
        .def_readwrite("Translation", &Wasteland::Animation::BoneTransform::Translation)
        .def_readwrite("Rotation", &Wasteland::Animation::BoneTransform::Rotation)
        .def_readwrite("Scale", &Wasteland::Animation::BoneTransform::Scale)
        .def("ToMatrix", &Wasteland::Animation::BoneTransform::ToMatrix)
        // Identity is a STATIC property, and the lambda takes a discarded py::object
        // parameter to absorb the implicit self that pybind supplies. Without that
        // parameter the annotation count would not match.
        .def_property_readonly_static(
            "Identity",
            [](py::object) { return Wasteland::Animation::BoneTransform::Identity(); });

// Pose holds the per-bone transforms for one frame. Exposed as a value type:
// Animator.Update returns a fresh Pose by copy, so a script holding an old
// one is unaffected by later updates.
py::class_<Wasteland::Animation::Pose>(anim, "Pose")
        .def(py::init<>())
        .def_readwrite("Bones", &Wasteland::Animation::Pose::Bones)
        .def("Resize", &Wasteland::Animation::Pose::Resize, py::arg("boneCount"))
        .def("Clear", &Wasteland::Animation::Pose::Clear)
        .def("GetBoneCount", &Wasteland::Animation::Pose::GetBoneCount)
        .def("IsEmpty", &Wasteland::Animation::Pose::IsEmpty)
        // GetBone returns a COPY, deliberately. Returning a reference into the
        // vector would dangle the instant the pose is resized, and scripts read
        // poses far more often than they mutate them.
        .def("GetBone", [](Wasteland::Animation::Pose &p, int32_t index) -> py::object {
            // Return a COPY, not a reference into the vector. A Python-held
            // reference would dangle the moment the pose is resized, and
            // scripts read poses far more often than they write them.
            const Wasteland::Animation::BoneTransform *bone = p.GetBone(Wasteland::Animation::BoneIndex(index));
            if (!bone)
                return py::none();
            return py::cast(*bone);
        }, py::arg("index"))
        // SetBone silently does nothing if the index is out of range. No exception,
        // no return value -- a script writing to a bone index that does not exist
        // gets no signal at all.
        .def("SetBone", [](Wasteland::Animation::Pose &p, int32_t index, const Wasteland::Animation::BoneTransform &value) {
            Wasteland::Animation::BoneTransform *bone = p.GetBone(Wasteland::Animation::BoneIndex(index));
            if (bone)
                *bone = value;
        }, py::arg("index"), py::arg("transform"));

// ---- AnimationCurve ----
    // AnimationCurve is the value-vs-time curve behind position and scale tracks.
    // Evaluated by the sampler at whatever time the playhead is at.
    py::class_<Wasteland::Animation::AnimationCurve>(anim, "AnimationCurve")
        .def(py::init<>())
        // mode defaults to Cubic. That default matters: adding a key without stating
        // an interpolation gives smooth tangents, not a linear ramp.
        .def("AddKey", &Wasteland::Animation::AnimationCurve::AddKey,
             py::arg("time"), py::arg("value"),
             py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        .def("RemoveKeyAtTime", &Wasteland::Animation::AnimationCurve::RemoveKeyAtTime, py::arg("time"))
        .def("RemoveAllKeys", &Wasteland::Animation::AnimationCurve::RemoveAllKeys)
        .def("RecalculateAutoTangents", &Wasteland::Animation::AnimationCurve::RecalculateAutoTangents)
        .def("Evaluate", &Wasteland::Animation::AnimationCurve::Evaluate, py::arg("time"))
        .def("IsEmpty", &Wasteland::Animation::AnimationCurve::IsEmpty)
        .def("GetKeyCount", &Wasteland::Animation::AnimationCurve::GetKeyCount)
        .def("GetStartTime", &Wasteland::Animation::AnimationCurve::GetStartTime)
        .def("GetEndTime", &Wasteland::Animation::AnimationCurve::GetEndTime)
        .def("GetDuration", &Wasteland::Animation::AnimationCurve::GetDuration)
        // SetExtrapolation is wrapped in a lambda for no functional reason -- the
        // member pointer would bind directly. Left as-is; harmless.
        .def("SetExtrapolation", [](Wasteland::Animation::AnimationCurve &c, Wasteland::Animation::AnimationCurve::Extrapolation mode) {
            c.SetExtrapolation(mode);
        }, py::arg("mode"))
        .def("GetExtrapolation", &Wasteland::Animation::AnimationCurve::GetExtrapolation);

// ---- RotationTrack ----
    // RotationTrack differs from AnimationCurve because rotation interpolates
    // along the arc, not component-wise. Hence a separate type rather than a
    // flag on the curve.
    py::class_<Wasteland::Animation::RotationTrack>(anim, "RotationTrack")
        .def(py::init<>())
        .def("AddKey", &Wasteland::Animation::RotationTrack::AddKey,
             py::arg("time"), py::arg("value"),
             py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        .def("RemoveAllKeys", &Wasteland::Animation::RotationTrack::RemoveAllKeys)
        .def("Evaluate", &Wasteland::Animation::RotationTrack::Evaluate, py::arg("time"))
        .def("IsEmpty", &Wasteland::Animation::RotationTrack::IsEmpty)
        .def("GetKeyCount", &Wasteland::Animation::RotationTrack::GetKeyCount)
        .def("GetStartTime", &Wasteland::Animation::RotationTrack::GetStartTime)
        .def("GetEndTime", &Wasteland::Animation::RotationTrack::GetEndTime);

// ---- BoneTrack ----
    // Position and Scale are exposed by INDEX rather than as a whole array:
    // def_readwrite cannot bind a C array member (pybind's array_accessor
    // fails as C3863 "array type is not assignable"), so a script uses
    // track.GetPositionCurve(i) instead of track.Position[i]. The shape
    // reads the same either way from Python.
    // BoneTrack bundles one bone's position, rotation and scale channels. The
    // position/scale curves are reached by INDEX rather than as arrays, because
    // def_readwrite cannot bind a C array member.
    py::class_<Wasteland::Animation::BoneTrack>(anim, "BoneTrack")
        .def(py::init<>())
        // The index bounds are checked here and 0..2 is accepted INCLUSIVE, which
        // matches a three-element array. Out of range returns None rather than
        // raising, consistent with GetBone above.
        .def("GetPositionCurve", [](Wasteland::Animation::BoneTrack &t, int index) -> py::object {
            if (index < 0 || index > 2)
                return py::none();
            // py::cast on the array element produces a COPY of the AnimationCurve, bound
            // as its own object. Appending to it changes the copy, not the track.
            return py::cast(t.Position[index]);
        }, py::arg("index"))
        .def("GetScaleCurve", [](Wasteland::Animation::BoneTrack &t, int index) -> py::object {
            if (index < 0 || index > 2)
                return py::none();
            return py::cast(t.Scale[index]);
        }, py::arg("index"))
        .def_readwrite("Rotation", &Wasteland::Animation::BoneTrack::Rotation)
        .def("IsEmpty", &Wasteland::Animation::BoneTrack::IsEmpty)
        .def("GetStartTime", &Wasteland::Animation::BoneTrack::GetStartTime)
        .def("GetEndTime", &Wasteland::Animation::BoneTrack::GetEndTime);

// ---- Skeleton ----
    // shared_ptr holder, because a Skeleton is shared between an Animator and
    // every SkinnedMesh bound to it. Registering it with the DEFAULT holder and
    // then passing it as a shared_ptr makes every such call throw "Unable to
    // load a custom holder type from a default-holder instance" -- the two
    // sides disagree about who owns the object. The holder must match how the
    // type is actually passed around.
    // shared_ptr holder, matching how a Skeleton is actually passed around: an
    // Animator holds one, and so does every SkinnedMesh bound to it. Registering
    // with the default holder and then handing out shared_ptrs throws at runtime.
    py::class_<Wasteland::Animation::Skeleton,
               std::shared_ptr<Wasteland::Animation::Skeleton>>(anim, "Skeleton")
        .def(py::init<>())
        // AddBone takes (name, parent) with parent defaulting to an invalid
        // index -- but NOT a BoneIndex default of BoneIndex(), because pybind
        // then counts three annotations against a two-parameter lambda and
        // fails as C2338. The default lives in C++ (AddBone's own signature)
        // instead.
        // AddBone takes parent as a BoneIndex rather than an int32_t, so the wrapper
        // lambda exists to convert. There is deliberately NO py::arg default here:
        // adding one would make pybind count three annotations against a two-parameter
        // lambda.
        .def("AddBone", [](Wasteland::Animation::Skeleton &s, const std::string &name,
                           Wasteland::Animation::BoneIndex parent) {
            return s.AddBone(name, parent);
        }, py::arg("name"), py::arg("parent"))
        // BuildFromNames is the bulk path, taking parallel name lists where parents
        // refers to names rather than indices. That is what lets a script build a
        // hierarchy without knowing the index each parent will get.
        // BuildFromNames takes two parallel vectors -- names and parentNames -- and
        // returns (ok, error). Parallel vectors rather than a list of pairs because
        // the C++ API is built that way; the binding adds no structure.
        .def("BuildFromNames", [](Wasteland::Animation::Skeleton &s,
                                  const std::vector<std::string> &names,
                                  const std::vector<std::string> &parents) {
            std::string error;
            bool ok = s.BuildFromNames(names, parents, error);
            return py::make_tuple(ok, error);
        // The parentNames ARGUMENT is named differently from the C++ parameter
        // (`parents`), so the Python keyword is parentNames. Another place where the
        // binding's spelling does not match the API it wraps.
        }, py::arg("names"), py::arg("parentNames"))
        // Every bone accessor below takes a raw int32_t and rebuilds a BoneIndex in
        // the lambda body. There are ~10 of them and all do it -- BoneIndex is a
        // strong typedef with no pybind caster, so the conversion has to happen by
        // hand at each binding site.
        .def("FindBone", &Wasteland::Animation::Skeleton::FindBone, py::arg("name"))
        .def("GetBoneName", [](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return s.GetBoneName(Wasteland::Animation::BoneIndex(index));
        }, py::arg("index"))
        // GetParent returning a BoneIndex rather than an int means a script gets an
        // opaque integer it cannot compare against GetChild results without calling
        // both. And it has no way to ask for a ROOT's parent sentinel.
        .def("GetParent", [](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return s.GetParent(Wasteland::Animation::BoneIndex(index));
        }, py::arg("index"))
        .def("GetChildCount", [](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return s.GetChildCount(Wasteland::Animation::BoneIndex(index));
        }, py::arg("index"))
        // GetChild takes a SLOT, not an index: children are ordered and a script
        // walks them 0, 1, 2... ChildCount above reports how many slots exist.
        // The hierarchy queries. GetChild takes a (bone, childSlot) pair because
        // the skeleton stores children in an array rather than a name map -- so a
        // script enumerating children must walk slots from 0 to GetChildCount.
        .def("GetChild", [](const Wasteland::Animation::Skeleton &s, int32_t index, int slot) {
            return s.GetChild(Wasteland::Animation::BoneIndex(index), slot);
        }, py::arg("index"), py::arg("childSlot"))
        // GetRoot and IsRoot: the sentinel question. GetParent on the root returns
        // a bone index that is not a real bone, and IsRoot is how a script tells
        // the difference -- walking a hierarchy without checking IsRoot eventually
        // reaches a parent index that looks valid.
        .def("GetRoot", &Wasteland::Animation::Skeleton::GetRoot)
        .def("IsRoot", [](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return s.IsRoot(Wasteland::Animation::BoneIndex(index));
        }, py::arg("index"))
        .def("GetBoneCount", &Wasteland::Animation::Skeleton::GetBoneCount)
        .def("IsEmpty", &Wasteland::Animation::Skeleton::IsEmpty)
        // SetRestPose/GetRestPose and the return_value_policy::copy on the getter.
        // A rest pose is the bind pose every animation is authored relative to, so
        // mutating the returned copy would silently do nothing -- the copy is
        // correct here precisely because callers should not change it.
        .def("SetRestPose", &Wasteland::Animation::Skeleton::SetRestPose, py::arg("pose"))
        .def("GetRestPose", &Wasteland::Animation::Skeleton::GetRestPose,
             py::return_value_policy::copy)
        .def("GetRestTransform", [](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return s.GetRestTransform(Wasteland::Animation::BoneIndex(index));
        }, py::arg("index"))
        // ComputeWorldMatrices runs the parent chain from the root down. The final
        // false argument is passed through to the C++ overload and is not annotated
        // here, so it takes its default.
        // ComputeWorldMatrices FLATTENS THE HIERARCHY into a flat list of world
        // matrices, one per bone in bone-index order. It does not return a tree:
        // the parent relationship must be re-derived from GetParent if a script
        // needs structure rather than final transforms.
        .def("ComputeWorldMatrices", [mat4ToColumns](const Wasteland::Animation::Skeleton &s,
                                        const Wasteland::Animation::Pose &pose) {
            std::vector<glm::mat4> world;
            // The trailing false is the 'not additive' flag, and the identity matrix is
            // the root parent transform. Both are hardcoded here rather than exposed, so
            // a script cannot compute a world matrix in a different space.
            s.ComputeWorldMatrices(pose, world, glm::mat4(1.0f), false);
            py::list out;
            for (const glm::mat4 &m : world)
                out.append(mat4ToColumns(m));
            return out;
        }, py::arg("pose"))
        // GetBoneWorldPosition is the convenience that spares scripts from matrix
        // indexing. It recomputes the world matrices on every call rather than
        // caching -- correct, but O(bones) per query, so a loop over many bones is
        // expensive. ComputeWorldMatrices once and index that if it matters.
        // GetBoneWorldPosition is the accessor gameplay code actually uses --
        // a muzzle point, an IK target, a footstep test. Returning a Vec3
        // rather than a matrix means scripts never have to know that glm
        // stores translation in column 3.
        .def("GetBoneWorldPosition",
             [mat4Translation](const Wasteland::Animation::Skeleton &s,
                const Wasteland::Animation::Pose &pose, int32_t index) {
                 std::vector<glm::mat4> world;
                 s.ComputeWorldMatrices(pose, world, glm::mat4(1.0f), false);
                 if (index < 0 || (size_t)index >= world.size())
                     return glm::vec3(0.0f);
                 return mat4Translation(world[(size_t)index]);
             }, py::arg("pose"), py::arg("index"))
        // ComputeSkinningMatrices produces the per-bone matrices that SkinnedMesh.
        // Skin consumes. Both halves are bound, and the mat4FromColumns helper below
        // is what lets the output round-trip back into Skin.
        .def("ComputeSkinningMatrices", [mat4ToColumns](const Wasteland::Animation::Skeleton &s,
                                           const Wasteland::Animation::Pose &pose) {
            std::vector<glm::mat4> matrices;
            s.ComputeSkinningMatrices(pose, matrices, glm::mat4(1.0f));
            py::list out;
            for (const glm::mat4 &m : matrices)
                out.append(mat4ToColumns(m));
            return out;
        }, py::arg("pose"))
        // CaptureBindMatrices snapshots the current pose as the bind pose. This is
        // the moment rigging data is frozen -- calling it after the mesh has been
        // posed would bake the wrong rest state.
        .def("CaptureBindMatrices", &Wasteland::Animation::Skeleton::CaptureBindMatrices)
        .def("GetInverseBindMatrix", [mat4ToColumns](const Wasteland::Animation::Skeleton &s, int32_t index) {
            return mat4ToColumns(s.GetInverseBindMatrix(Wasteland::Animation::BoneIndex(index)));
        }, py::arg("index"))
        // Validate returns (ok, error) rather than raising, because an invalid
        // skeleton is a content error the caller should report, not an exception.
        .def("Validate", [](const Wasteland::Animation::Skeleton &s) {
            std::string error;
            bool ok = s.Validate(error);
            return py::make_tuple(ok, error);
        });

// ---- AnimationClip ----
    // std::shared_ptr holder, because clips are shared: an Animator holds the
    // same clip the state machine authored, and copying the keyframe data per
    // instance would multiply memory by the instance count.
    //
    // py::init is listed in BOTH holder forms. With a shared_ptr holder and
    // only the factory form, constructing from Python throws "Unable to load a
    // custom holder type from a default-holder instance" -- pybind builds the
    // object with the default holder, which is not the one registered.
    // AnimationClip with a shared_ptr holder, for the same reason as Skeleton:
    // one clip is shared by every instance that plays it, and copying the
    // keyframe data per instance would multiply memory by the instance count.
    py::class_<Wasteland::Animation::AnimationClip,
               std::shared_ptr<Wasteland::Animation::AnimationClip>>(anim, "AnimationClip")
        .def(py::init<>())
        .def(py::init<const std::string &>(), py::arg("name"))
        .def("SetName", &Wasteland::Animation::AnimationClip::SetName, py::arg("name"))
        .def("GetName", &Wasteland::Animation::AnimationClip::GetName)
        // The raw AddKey takes bone/channel/component/value as separate scalars.
        // component is 0/1/2 for X/Y/Z. The three typed helpers below wrap this for
        // the common cases and are what scripts normally use.
        // AddKey is the LOW-LEVEL form: bone index, track channel, component index,
        // time, value. A script has to know the channel/component encoding for the
        // track type it wants. The three convenience wrappers below exist so most
        // scripts never need to.
        //
        // Every wrapper below defaults mode to Cubic. Step gives a robotic look and
        // Linear a cheap one, so defaulting to Cubic means a script that adds keys and
        // does nothing else gets smooth motion.
        .def("AddKey", [](Wasteland::Animation::AnimationClip &c, int32_t bone,
                          Wasteland::Animation::TrackChannel channel, int component,
                          float time, float value,
                          Wasteland::Animation::Interpolation mode) {
            // Every wrapper rebuilds a BoneIndex from the int32_t a script passes.
            // BoneIndex is a strong typedef, so without this the C++ overload would not
            // be callable from Python at all -- pybind has no caster for a typedef'd
            // enum.
            c.AddKey(Wasteland::Animation::BoneIndex(bone), channel, component, time, value, mode);
        }, py::arg("bone"), py::arg("channel"), py::arg("component"),
           py::arg("time"), py::arg("value"),
           py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        // AddPositionKey/AddRotationKey/AddScaleKey are the same three steps with
        // the channel and component implied. Rotation takes a QUATERNION, not an
        // euler angle -- the engine has no euler representation in its pose, so a
        // script must build the quat itself.
        .def("AddPositionKey", [](Wasteland::Animation::AnimationClip &c, int32_t bone,
                                  float time, const glm::vec3 &value,
                                  Wasteland::Animation::Interpolation mode) {
            c.AddPositionKey(Wasteland::Animation::BoneIndex(bone), time, value, mode);
        }, py::arg("bone"), py::arg("time"), py::arg("value"),
           py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        .def("AddRotationKey", [](Wasteland::Animation::AnimationClip &c, int32_t bone,
                                  float time, const glm::quat &value,
                                  Wasteland::Animation::Interpolation mode) {
            c.AddRotationKey(Wasteland::Animation::BoneIndex(bone), time, value, mode);
        }, py::arg("bone"), py::arg("time"), py::arg("value"),
           py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        .def("AddScaleKey", [](Wasteland::Animation::AnimationClip &c, int32_t bone,
                               float time, const glm::vec3 &value,
                               Wasteland::Animation::Interpolation mode) {
            c.AddScaleKey(Wasteland::Animation::BoneIndex(bone), time, value, mode);
        }, py::arg("bone"), py::arg("time"), py::arg("value"),
           py::arg("mode") = Wasteland::Animation::Interpolation::Cubic)
        // RecalculateAutoTangents is required after adding keys to a track that uses
        // automatic tangents. Skipping it leaves the new segment interpolating
        // wrongly, with nothing in the API to tell you that is what happened.
        .def("RecalculateAutoTangents", &Wasteland::Animation::AnimationClip::RecalculateAutoTangents)
        .def("SetLooping", &Wasteland::Animation::AnimationClip::SetLooping, py::arg("looping"))
        .def("IsLooping", &Wasteland::Animation::AnimationClip::IsLooping)
        .def("SetPlayRate", &Wasteland::Animation::AnimationClip::SetPlayRate, py::arg("rate"))
        .def("GetPlayRate", &Wasteland::Animation::AnimationClip::GetPlayRate)
        .def("GetDuration", &Wasteland::Animation::AnimationClip::GetDuration)
        .def("GetStartTime", &Wasteland::Animation::AnimationClip::GetStartTime)
        .def("GetEndTime", &Wasteland::Animation::AnimationClip::GetEndTime)
        .def("IsEmpty", &Wasteland::Animation::AnimationClip::IsEmpty)
        // HasTrack takes a bare int32_t and wraps it, because BoneIndex again has
        // no caster. Consistent with the AddKey wrappers above.
        .def("HasTrack", [](const Wasteland::Animation::AnimationClip &c, int32_t bone) {
            return c.HasTrack(Wasteland::Animation::BoneIndex(bone));
        }, py::arg("bone"))
        // GetTrack returns py::cast(*track) -- BY VALUE. That copies the whole
        // BoneTrack, so a script mutating the result does not affect the clip, and
        // a large track is an expensive copy. Contrast with the reference-returning
        // lookups elsewhere in this file; this one is a copy for a reason the C++
        // signature does not make obvious.
        .def("GetTrack", [](Wasteland::Animation::AnimationClip &c, int32_t bone) -> py::object {
            const Wasteland::Animation::BoneTrack *track = c.GetTrack(Wasteland::Animation::BoneIndex(bone));
            if (!track)
                return py::none();
            return py::cast(*track);
        }, py::arg("bone"))
        // Sample returns a fresh Pose by value. The out-parameter form cannot be bound
        // directly -- a raw Pose* has no caster -- so the lambda owns a local and
        // returns it.
        .def("Sample", [](const Wasteland::Animation::AnimationClip &c,
                          const Wasteland::Animation::Skeleton &skeleton, float time) {
            Wasteland::Animation::Pose pose;
            c.Sample(skeleton, time, pose);
            return pose;
        }, py::arg("skeleton"), py::arg("time"));

// ---- AnimationInstance ----
    // AnimationInstance is one playing clip within an Animator. Registered as a
    // class but its mutators all live on Animator, which owns the storage.
    py::class_<Wasteland::Animation::AnimationInstance>(anim, "AnimationInstance")
        .def(py::init<>())
        // Time/Speed/Weight/Looping are all readwrite here AND reachable through
        // Animator's Set*/Get* helpers below. Two paths to the same state.
        .def_readwrite("Time", &Wasteland::Animation::AnimationInstance::Time)
        .def_readwrite("Speed", &Wasteland::Animation::AnimationInstance::Speed)
        .def_readwrite("Weight", &Wasteland::Animation::AnimationInstance::Weight)
        .def_readwrite("Looping", &Wasteland::Animation::AnimationInstance::Looping)
        .def_readwrite("IsFinished", &Wasteland::Animation::AnimationInstance::IsFinished)
        .def_property_readonly("Clip", [](const Wasteland::Animation::AnimationInstance &i) {
            return i.Clip;
        })
        .def_property_readonly("Duration", [](const Wasteland::Animation::AnimationInstance &i) {
            return i.GetDuration();
        });

// ---- Animator ----
    // Animator. Plain class, no holder: an Animator is always reached through
    // the state machine that owns it, never constructed standalone from Python.
    py::class_<Wasteland::Animation::Animator>(anim, "Animator")
        .def(py::init<>())
        // SetSkeleton takes the shared_ptr by const reference, matching the class
        // holder. This is the other half of the Skeleton holder decision above -- if
        // the two disagreed, this call would throw.
        .def("SetSkeleton", [](Wasteland::Animation::Animator &a,
                               const std::shared_ptr<Wasteland::Animation::Skeleton> &skeleton) {
            a.SetSkeleton(skeleton);
        }, py::arg("skeleton"))
        .def("GetSkeleton", [](const Wasteland::Animation::Animator &a) {
            return a.GetSkeleton();
        })
        // AddInstance returns the instance id the caller needs for every later
        // Set*/Get* call. That id is the handle; there is no Python object for the
        // instance itself until GetInstance is called.
        .def("AddInstance", [](Wasteland::Animation::Animator &a,
                               const std::shared_ptr<Wasteland::Animation::AnimationClip> &clip) {
            return a.AddInstance(clip);
        }, py::arg("clip"))
        .def("RemoveInstance", &Wasteland::Animation::Animator::RemoveInstance, py::arg("instanceId"))
        .def("FadeOutInstance", &Wasteland::Animation::Animator::FadeOutInstance,
             py::arg("instanceId"), py::arg("fadeDuration"))
        .def("RemoveAllInstances", &Wasteland::Animation::Animator::RemoveAllInstances)
        .def("GetInstanceCount", &Wasteland::Animation::Animator::GetInstanceCount)
        .def("HasInstance", &Wasteland::Animation::Animator::HasInstance, py::arg("instanceId"))
        // reference, NOT copy: GetInstance returns a COPY by default, so
        // `anim.GetInstance(i).Looping = False` silently did nothing -- the
        // write landed on a temporary that was then discarded, and the C++
        // side kept looping. Every gameplay script that adjusted playback
        // this way was broken, with no error to show for it.
        //
        // reference, not reference_internal: keep_alive cannot be used because
        // the Animator OWNS the instance, it is not shared with a Python
        // object. Asking pybind to tie a lifetime to a policy that expects
        // shared ownership throws "Could not activate keep_alive!" at
        // runtime. The caller is responsible for not caching the handle
        // across a RemoveInstance.
        // GetInstance returns a REFERENCE into the Animator's own storage. The
        // default policy would copy, which is the bug this fixes: a write through
        // the copy vanished and the clip kept looping. No keep_alive is used, because
        // the Animator owns the instance rather than sharing it -- so a cached handle
        // across RemoveInstance dangles.
        .def("GetInstance", [](Wasteland::Animation::Animator &a, int instanceId) -> py::object {
            Wasteland::Animation::AnimationInstance *instance = a.GetInstance(instanceId);
            if (!instance)
                return py::none();
            return py::cast(instance, py::return_value_policy::reference);
        }, py::arg("instanceId"))
        // Explicit setters for the fields scripts most often change, so the
        // correct path is obvious even if the reference behaviour is missed.
        // The Animator's per-instance controls. Every one is addressed by an
        // instanceId rather than by holding an AnimationInstance, because the
        // animator owns the instances and they are not stable objects.
        .def("SetWeight", [](Wasteland::Animation::Animator &a, int instanceId, float weight) {
            // SetWeight null-checks the instance and does nothing if it is absent,
            // whereas GetWeight below returns 0 for a missing instance. So a mistyped
            // id silently no-ops on a write and reads as zero -- no exception either way.
            if (Wasteland::Animation::AnimationInstance *instance = a.GetInstance(instanceId))
                instance->Weight = weight;
        }, py::arg("instanceId"), py::arg("weight"))
        .def("GetWeight", [](Wasteland::Animation::Animator &a, int instanceId) {
            const Wasteland::Animation::AnimationInstance *instance = a.GetInstance(instanceId);
            return instance ? instance->Weight : 0.0f;
        }, py::arg("instanceId"))
        .def("SetSpeed", &Wasteland::Animation::Animator::SetSpeed,
             py::arg("instanceId"), py::arg("speed"))
        .def("SetLooping", &Wasteland::Animation::Animator::SetLooping,
             py::arg("instanceId"), py::arg("looping"))
        .def("SetTime", &Wasteland::Animation::Animator::SetTime,
             py::arg("instanceId"), py::arg("time"))
        // SetGlobalTimeOffset shifts EVERY instance at once, unlike SetTime above
        // which addresses one. It is the hook for a global slow-motion or a
        // synchronised cutscene beat.
        .def("SetGlobalTimeOffset", &Wasteland::Animation::Animator::SetGlobalTimeOffset, py::arg("offset"))
        .def("GetTotalWeight", &Wasteland::Animation::Animator::GetTotalWeight)
        // Update advances the playhead and returns the evaluated pose. Scripts that
        // only need the pose and not the timing can call Evaluate instead, which does
        // not advance anything.
        .def("Update", [](Wasteland::Animation::Animator &a, float dt) {
            Wasteland::Animation::Pose pose;
            a.Update(dt, pose);
            return pose;
        }, py::arg("deltaSeconds"))
        .def("Evaluate", [](const Wasteland::Animation::Animator &a) {
            Wasteland::Animation::Pose pose;
            a.Evaluate(pose);
            return pose;
        })
        // GetLastPose with an explicit copy policy. A reference here would hand back
        // storage that the next Update overwrites.
        .def("GetLastPose", &Wasteland::Animation::Animator::GetLastPose,
             py::return_value_policy::copy);

// ---- AnimationState / Transition / StateMachine ----
    // AnimationState: a name, a clip, and the playback parameters. The clip is a
    // shared_ptr, so two states can reference the same AnimationClip.
    py::class_<Wasteland::Animation::AnimationState>(anim, "AnimationState")
        .def(py::init<>())
        .def_readwrite("Name", &Wasteland::Animation::AnimationState::Name)
        .def_readwrite("Clip", &Wasteland::Animation::AnimationState::Clip)
        .def_readwrite("Speed", &Wasteland::Animation::AnimationState::Speed)
        .def_readwrite("Looping", &Wasteland::Animation::AnimationState::Looping)
        .def_readwrite("TransitionDuration", &Wasteland::Animation::AnimationState::TransitionDuration);

py::class_<Wasteland::Animation::AnimationTransition>(anim, "AnimationTransition")
        .def(py::init<>())
        .def_readwrite("FromState", &Wasteland::Animation::AnimationTransition::FromState)
        .def_readwrite("ToState", &Wasteland::Animation::AnimationTransition::ToState)
        .def_readwrite("Trigger", &Wasteland::Animation::AnimationTransition::Trigger)
        .def_readwrite("Duration", &Wasteland::Animation::AnimationTransition::Duration);

// The state machine. Note that it OWNS an Animator, which is why GetAnimator
// below uses reference_internal to tie the returned handle's lifetime to the
// state machine.
py::class_<Wasteland::Animation::AnimationStateMachine>(anim, "AnimationStateMachine")
        .def(py::init<>())
        .def("AddState", [](Wasteland::Animation::AnimationStateMachine &sm,
                            const Wasteland::Animation::AnimationState &state) {
            return sm.AddState(state);
        }, py::arg("state"))
        .def("AddStateFromClip", [](Wasteland::Animation::AnimationStateMachine &sm,
                                    const std::string &name,
                                    const std::shared_ptr<Wasteland::Animation::AnimationClip> &clip) {
            return sm.AddState(name, clip);
        }, py::arg("name"), py::arg("clip"))
        // AddTransition takes a fully-built AnimationTransition rather than loose
        // strings, so a script constructs one, sets From/To/Trigger/Duration, and
        // hands it over.
        .def("AddTransition", &Wasteland::Animation::AnimationStateMachine::AddTransition,
             py::arg("transition"))
        .def("SetTrigger", &Wasteland::Animation::AnimationStateMachine::SetTrigger, py::arg("trigger"))
        .def("ClearTrigger", &Wasteland::Animation::AnimationStateMachine::ClearTrigger, py::arg("trigger"))
        .def("IsTriggerSet", &Wasteland::Animation::AnimationStateMachine::IsTriggerSet, py::arg("trigger"))
        .def("SetState", &Wasteland::Animation::AnimationStateMachine::SetState, py::arg("stateName"))
        // TransitionTo's duration defaults to -1.0f, meaning "use the transition's
        // own configured duration". A script passing an explicit value overrides it.
        .def("TransitionTo", &Wasteland::Animation::AnimationStateMachine::TransitionTo,
             py::arg("stateName"), py::arg("duration") = -1.0f)
        .def("GetCurrentState", &Wasteland::Animation::AnimationStateMachine::GetCurrentState)
        .def("GetPreviousState", &Wasteland::Animation::AnimationStateMachine::GetPreviousState)
        .def("IsTransitioning", &Wasteland::Animation::AnimationStateMachine::IsTransitioning)
        .def("HasState", &Wasteland::Animation::AnimationStateMachine::HasState, py::arg("stateName"))
        .def("GetStateCount", &Wasteland::Animation::AnimationStateMachine::GetStateCount)
        // An explicit lambda, not the member pointer: GetAnimator is
        // overloaded (a const and a non-const overload), and pybind cannot
        // pick between them from the address alone, which fails as C2672.
        // Returning a reference_internal keeps the Animator's lifetime tied
        // to the state machine that owns it.
        // GetAnimator via lambda: the C++ method is overloaded (const and non-const)
        // and a member pointer cannot disambiguate them. reference_internal keeps the
        // Animator alive as long as the state machine that owns it.
        .def("GetAnimator", [](Wasteland::Animation::AnimationStateMachine &sm) -> Wasteland::Animation::Animator & {
            return sm.GetAnimator();
        }, py::return_value_policy::reference_internal)
        // Update steps the state machine and then evaluates the result into a Pose,
        // returning it. One call, rather than requiring the caller to remember the
        // second step.
        .def("Update", [](Wasteland::Animation::AnimationStateMachine &sm, float dt) {
            sm.Update(dt);
            Wasteland::Animation::Pose pose;
            sm.GetAnimator().Evaluate(pose);
            return pose;
        }, py::arg("deltaSeconds"))
        // Named "ValidateGraph" rather than "Validate": the state machine's
        // C++ method is also called Validate, and pybind's overload
        // resolution across the binding chain cannot tell the two apart,
        // which fails as C2672. Distinct Python names also read better --
        // it is a graph of states, not a re-validation of the object.
        // Named ValidateGraph, not Validate, to avoid colliding with Skeleton's
        // Validate across the binding chain -- two classes can each bind Validate
        // without a conflict, but the overloaded member pointers here cannot be
        // resolved unambiguously.
        .def("ValidateGraph", [](const Wasteland::Animation::AnimationStateMachine &sm) {
            std::string error;
            bool ok = sm.Validate(error);
            return py::make_tuple(ok, error);
        });

// ---- SkinnedMesh ----
    // BoneWeight pairs a bone index with a weight. One vertex has several of
    // these, and their weights should sum to 1 -- NormalizeWeights below exists
    // because imported meshes frequently do not.
    py::class_<Wasteland::Animation::BoneWeight>(anim, "BoneWeight")
        .def(py::init<>())
        // Two-argument constructor, so the natural call
        // BoneWeight(bone, weight) works. With only the default constructor a
        // script had to write:
        //   w = BoneWeight(); w.Bone = 0; w.Weight = 1.0
        // for every influence, which is unusable for authoring real skin
        // weights.
        //
        // An explicit lambda, not py::init<int32_t, float>: BoneWeight is an
        // aggregate, and pybind's init machinery tries to brace-initialize it,
        // which fails as C2440 "cannot convert from initializer list".
        //
        // The bone parameter is BoneIndex, not int32_t. Accepting a plain int
        // here and assigning it directly fails as C2679, because assigning an
        // integer to BoneIndex needs its explicit constructor. Scripts pass a
        // plain int, so the lambda converts rather than requiring them to
        // build a BoneIndex first.
        // Two-argument construction via a lambda. py::init<int32_t, float> would
        // brace-initialise an aggregate and fail to compile; building the struct in
        // the lambda body sidesteps that.
        .def(py::init([](int32_t bone, float weight) {
            Wasteland::Animation::BoneWeight result;
            result.Bone = Wasteland::Animation::BoneIndex(bone);
            result.Weight = weight;
            return result;
        }), py::arg("bone"), py::arg("weight"))
        .def_readwrite("Bone", &Wasteland::Animation::BoneWeight::Bone)
        .def_readwrite("Weight", &Wasteland::Animation::BoneWeight::Weight);

// SkinnedMesh: vertices plus the weights that deform them. No holder -- it is
// plain data, constructed and copied freely.
py::class_<Wasteland::Animation::SkinnedMesh>(anim, "SkinnedMesh")
        .def(py::init<>())
        // Vertices is a COPY-OUT list on purpose.
        //
        // reference_internal does not work for this container: MeshVertex is
        // itself a pybind-bound type, so `mesh.Vertices.append(v)` mutates a
        // Python list that was copied out and then discarded. The mesh stayed
        // at 0 vertices with no error -- len(Vertices) even reported the
        // appended count while GetVertexCount() said 0, which is the kind of
        // contradiction that costs an afternoon.
        //
        // So mutation goes through the explicit methods: SetVertices for a
        // whole list, AddVertex for one at a time. Reading via .Vertices is
        // safe and cheap.
        // Vertices is a COPY-OUT list. The comment above explains why:
        // reference_internal does not help because MeshVertex is itself a bound type,
        // so appending mutates a Python list that pybind already detached. Reading is
        // safe; writing goes through SetVertices/AddVertex.
        .def_property_readonly("Vertices",
            [](const Wasteland::Animation::SkinnedMesh &sm) {
                return sm.Vertices;
            })
        // AddVertex appends one at a time. This is the mutation path that works when
        // a script cannot assemble a whole list first.
        .def("AddVertex", [](Wasteland::Animation::SkinnedMesh &sm,
                             const Wasteland::MeshVertex &vertex) {
            sm.Vertices.push_back(vertex);
        }, py::arg("vertex"))
        // AddVertices takes a list and appends in bulk. stl.h's list caster converts
        // the Python sequence to a temporary vector, which is then copied in -- the
        // copy happens on the way IN, which is exactly why appending to the argument
        // afterwards has no effect on the mesh.
        .def("AddVertices", [](Wasteland::Animation::SkinnedMesh &sm,
                               const std::vector<Wasteland::MeshVertex> &vertices) {
            sm.Vertices.insert(sm.Vertices.end(), vertices.begin(), vertices.end());
        }, py::arg("vertices"))
        .def("ClearVertices", [](Wasteland::Animation::SkinnedMesh &sm) {
            sm.Vertices.clear();
        })
        // Indices and Weights are plain numeric vectors with no nested bound
        // type, so reference_internal works for them and in-place editing
        // (including append) reaches the real data.
        // Indices and Weights use reference_internal, which DOES work here because
        // they are plain numeric vectors with no nested bound type. So for these two,
        // in-place append reaches the real data -- unlike Vertices immediately above.
        .def_property_readonly("Indices",
            [](Wasteland::Animation::SkinnedMesh &sm) -> std::vector<uint32_t> & { return sm.Indices; },
            py::return_value_policy::reference_internal)
        // DEFECT: Weights is bound a second time here and again at line 1151, with
        // identical getters. pybind silently accepts the second definition and the
        // first becomes dead. Harmless today, but it is a duplicate that will drift:
        // change one and the other keeps the old behaviour.
        .def_property_readonly("Weights",
            [](Wasteland::Animation::SkinnedMesh &sm) -> std::vector<std::vector<Wasteland::Animation::BoneWeight>> & { return sm.Weights; },
            py::return_value_policy::reference_internal)
        // Whole-container setters, so a script can either append in place or
        // assign a complete list in one go. The property above stays
        // reference_internal for append, and these cover assignment --
        // `mesh.Indices = [...]` on a read-only property raises "property has
        // no setter", which is how a freshly imported mesh kept failing to
        // take its triangle list.
        // Whole-container setters. These exist because the property above is
        // read-only: `mesh.Indices = [...]` on a read-only property raises "property
        // has no setter". So append goes through the property and assignment goes
        // through these -- two different paths to the same vector.
        .def("SetVertices",
             [](Wasteland::Animation::SkinnedMesh &sm,
                const std::vector<Wasteland::MeshVertex> &vertices) {
                 sm.Vertices = vertices;
             }, py::arg("vertices"))
        .def("SetIndices",
             [](Wasteland::Animation::SkinnedMesh &sm,
                const std::vector<uint32_t> &indices) {
                 sm.Indices = indices;
             }, py::arg("indices"))
        .def("SetWeights",
             [](Wasteland::Animation::SkinnedMesh &sm,
                const std::vector<std::vector<Wasteland::Animation::BoneWeight>> &weights) {
                 sm.Weights = weights;
             }, py::arg("weights"))
        // The duplicate Weights binding. Whichever pybind keeps, the intent is the
        // reference_internal one described above; the earlier copy at 1127 is
        // redundant.
        .def_property_readonly("Weights",
            [](Wasteland::Animation::SkinnedMesh &sm) -> std::vector<std::vector<Wasteland::Animation::BoneWeight>> & { return sm.Weights; },
            py::return_value_policy::reference_internal)
        // BindPositions is the same reference_internal pattern: append reaches the
        // vector, assignment must go through SetBindPositions.
        .def_property_readonly("BindPositions",
            [](Wasteland::Animation::SkinnedMesh &sm) -> std::vector<glm::vec3> & { return sm.BindPositions; },
            py::return_value_policy::reference_internal)
        // SetBindPositions is the supported way to install bind positions.
        // They are NOT assigned directly: BindPositions is a reference_internal
        // read-only property (see above), so `mesh.BindPositions = [...]` raises
        // "property has no setter". Binding positions in bulk is a legitimate
        // authoring operation -- a script that just loaded an OBJ or glTF has
        // exactly this list -- so it gets a first-class method.
        // SetBindPositions, the supported way to install them. Bulk bind-position
        // assignment is a legitimate authoring operation -- a script that just loaded
        // an OBJ has exactly this list -- so it gets a first-class method rather
        // than being forced through a loop of push_backs.
        .def("SetBindPositions",
             [](Wasteland::Animation::SkinnedMesh &sm, const std::vector<glm::vec3> &positions) {
                 sm.BindPositions = positions;
             }, py::arg("positions"))
        // CaptureBindPositions is deliberately NOT bound: it is a private
        // member on SkinnedMesh. Bind positions are a rigging decision, not a
        // guess, so a script supplies them explicitly through
        // SetBindPositions rather than the mesh silently snapshotting whatever
        // happens to be in Vertices at that moment.
        // BindAllToBone is the degenerate rig: every vertex to one bone. Useful for
        // props and for debugging, since it makes the mesh track exactly one joint.
        .def("BindAllToBone", [](Wasteland::Animation::SkinnedMesh &sm, int32_t bone) {
            sm.BindAllToBone(Wasteland::Animation::BoneIndex(bone));
        }, py::arg("bone"))
        // BindTwoBonesByAxis splits the mesh along one axis between two bones, with
        // a blend width around the split. The cheapest two-bone skin there is, and
        // the one an auto-rigger would produce.
        .def("BindTwoBonesByAxis", [](Wasteland::Animation::SkinnedMesh &sm,
                                     int32_t boneA, int32_t boneB, int axis,
                                     float splitPoint, float blendWidth) {
            sm.BindTwoBonesByAxis(Wasteland::Animation::BoneIndex(boneA),
                                  Wasteland::Animation::BoneIndex(boneB),
                                  axis, splitPoint, blendWidth);
        }, py::arg("boneA"), py::arg("boneB"), py::arg("axis"),
           py::arg("splitPoint"), py::arg("blendWidth"))
        .def("NormalizeWeights", &Wasteland::Animation::SkinnedMesh::NormalizeWeights)
        // LimitInfluencesPerVertex trims each vertex to at most N influences, with a
        // threshold below which an influence is dropped outright. The default
        // threshold of 0.001f means near-zero weights are discarded rather than kept
        // as numerical noise.
        .def("LimitInfluencesPerVertex",
             &Wasteland::Animation::SkinnedMesh::LimitInfluencesPerVertex,
             py::arg("maxInfluences"), py::arg("threshold") = 0.001f)
        // Skin accepts EITHER real glm::mat4 objects or the list-of-4-column-
        // tuples that ComputeSkinningMatrices returns to Python.
        //
        // Without the list overload, the obvious script --
        //   matrices = skeleton.ComputeSkinningMatrices(pose)
        //   mesh.Skin(matrices)
        // -- fails with "incompatible function arguments", because
        // ComputeSkinningMatrices hands back Python lists and Skin's only
        // signature wanted glm::mat4. Making the round trip impossible was a
        // binding bug, not a caller error.
        // First Skin overload: real glm::mat4 objects. Returns (ok, positions) -- the
        // skinned result is a fresh position list, not a mutation of the mesh. A
        // caller that ignores the ok flag will read a zeroed vector on failure.
        .def("Skin", [](const Wasteland::Animation::SkinnedMesh &sm,
                        const std::vector<glm::mat4> &skinningMatrices) {
            std::vector<glm::vec3> positions;
            bool ok = sm.Skin(skinningMatrices, positions);
            return py::make_tuple(ok, positions);
        }, py::arg("skinningMatrices"))
        // Second Skin overload: the list-of-columns form that
        // ComputeSkinningMatrices actually returns. Two overloads on one name is
        // fine here -- unlike the operator binding above, a NAMED method accumulates
        // overloads rather than replacing. The first attempt is tried in order, so a
        // list matches the second and real matrices match the first.
        .def("Skin",
             [mat4FromColumns](const Wasteland::Animation::SkinnedMesh &sm,
                               const py::sequence &skinningMatrices) {
                 std::vector<glm::mat4> matrices;
                 for (const py::handle &item : skinningMatrices)
                     matrices.push_back(mat4FromColumns(item));
                 std::vector<glm::vec3> positions;
                 bool ok = sm.Skin(matrices, positions);
                 return py::make_tuple(ok, positions);
             }, py::arg("skinningMatrices"))
        .def("GetVertexCount", &Wasteland::Animation::SkinnedMesh::GetVertexCount)
        .def("GetMaxInfluencesPerVertex", &Wasteland::Animation::SkinnedMesh::GetMaxInfluencesPerVertex)
        // IsValid and HasWeights are the two preconditions a script should check
        // before Skinning: one says the mesh is self-consistent, the other says any
        // vertex has weights at all.
        .def("IsValid", &Wasteland::Animation::SkinnedMesh::IsValid)
        .def("HasWeights", &Wasteland::Animation::SkinnedMesh::HasWeights)
        // Validate returns (result.empty(), result) -- the C++ method returns the
        // message string itself rather than filling an out-parameter, unlike the
        // Skeleton and StateMachine Validate bindings above. The shape is the same at
        // the Python level, but the implementation is not.
        .def("Validate", [](const Wasteland::Animation::SkinnedMesh &sm) {
            std::string error;
            std::string result = sm.Validate(error);
            return py::make_tuple(result.empty(), result);
        });

// ------------------------------------------------------------ Core: Input --
    // Core Input: six free functions on the root module. No submodule, so a
    // script writes Wasteland.IsKeyPressed(Wasteland.WL_KEY_W).
    m.def("IsKeyPressed", &Wasteland::Input::IsKeyPressed, py::arg("keyCode"));
    m.def("IsMouseButtonPressed", &Wasteland::Input::IsMouseButtonPressed, py::arg("button"));
    // GetMousePosition returns a Vec2 in whatever coordinate space Input uses --
    // client-relative, not screen-relative. There is no screen-space variant bound
    // here.
    m.def("GetMousePosition", &Wasteland::Input::GetMousePosition);
    // GetMouseDelta is the per-frame movement, which is what a camera-drag script
    // wants. It is distinct from GetMousePosition and is not a difference the
    // caller computes themselves, because the window may not have focus.
    m.def("GetMouseDelta", &Wasteland::Input::GetMouseDelta);
    m.def("GetMouseX", &Wasteland::Input::GetMouseX);
    m.def("GetMouseY", &Wasteland::Input::GetMouseY);

// ------------------------------------------------------------ Core: Gamepad --
    // A submodule rather than a scatter of module-level functions, because
    // every call needs a gamepad slot as its first argument and
    // Wasteland.IsGamepadButtonPressed(...) at module scope would collide
    // confusingly with the keyboard functions above.
    // The Gamepad submodule. A submodule rather than root-level functions
    // because every call needs a slot as its first argument, and root-level names
    // would read confusingly next to the keyboard functions.
    py::module_ gamepadModule = m.def_submodule("Gamepad", "Gamepad input");

// MaxGamepads and DefaultDeadzone as module attributes rather than functions.
// Scripts read them to size their own arrays and to know what threshold the
// engine will apply by default.
gamepadModule.attr("MaxGamepads") = Wasteland::Gamepad::MaxGamepads;
    gamepadModule.attr("DefaultDeadzone") = Wasteland::Gamepad::DefaultDeadzone;

#define BIND_GAMEPAD_BUTTON(k) gamepadModule.attr("BUTTON_" #k) = WL_GAMEPAD_BUTTON_##k
    // Button constants via a macro loop. Sixteen entries listed individually is
    // mechanical repetition; the macro keeps the export and the include in sync.
    BIND_GAMEPAD_BUTTON(A);
    BIND_GAMEPAD_BUTTON(B);
    BIND_GAMEPAD_BUTTON(X);
    BIND_GAMEPAD_BUTTON(Y);
    BIND_GAMEPAD_BUTTON(LEFT_BUMPER);
    BIND_GAMEPAD_BUTTON(RIGHT_BUMPER);
    BIND_GAMEPAD_BUTTON(BACK);
    BIND_GAMEPAD_BUTTON(START);
    BIND_GAMEPAD_BUTTON(GUIDE);
    BIND_GAMEPAD_BUTTON(LEFT_THUMB);
    BIND_GAMEPAD_BUTTON(RIGHT_THUMB);
    BIND_GAMEPAD_BUTTON(DPAD_UP);
    BIND_GAMEPAD_BUTTON(DPAD_RIGHT);
    BIND_GAMEPAD_BUTTON(DPAD_DOWN);
    BIND_GAMEPAD_BUTTON(DPAD_LEFT);
    // #undef immediately after, so the macro does not leak into the rest of this
    // file. Same pattern for the axis and key macros below.
    #undef BIND_GAMEPAD_BUTTON

// PlayStation-style aliases, so a script can use whichever spelling its
    // author knows without caring which physical pad is plugged in.
    // PlayStation-style aliases. WL_GAMEPAD_BUTTON_CROSS is the same physical
    // button as BUTTON_A, so a script can use whichever name its author knows --
    // but only one of the two appears in a pad's reported name, so the alias is
    // about spelling, not about which pad is plugged in.
    gamepadModule.attr("BUTTON_CROSS") = WL_GAMEPAD_BUTTON_CROSS;
    gamepadModule.attr("BUTTON_CIRCLE") = WL_GAMEPAD_BUTTON_CIRCLE;
    gamepadModule.attr("BUTTON_SQUARE") = WL_GAMEPAD_BUTTON_SQUARE;
    gamepadModule.attr("BUTTON_TRIANGLE") = WL_GAMEPAD_BUTTON_TRIANGLE;

#define BIND_GAMEPAD_AXIS(k) gamepadModule.attr("AXIS_" #k) = WL_GAMEPAD_AXIS_##k
    BIND_GAMEPAD_AXIS(LEFT_X);
    BIND_GAMEPAD_AXIS(LEFT_Y);
    BIND_GAMEPAD_AXIS(RIGHT_X);
    BIND_GAMEPAD_AXIS(RIGHT_Y);
    BIND_GAMEPAD_AXIS(LEFT_TRIGGER);
    BIND_GAMEPAD_AXIS(RIGHT_TRIGGER);
    #undef BIND_GAMEPAD_AXIS

// slot defaults to 0 everywhere in this block, so single-pad scripts can omit
// it entirely.
gamepadModule.def("IsConnected", &Wasteland::Gamepad::IsConnected, py::arg("slot") = 0);
    gamepadModule.def("GetName", &Wasteland::Gamepad::GetName, py::arg("slot") = 0);
    gamepadModule.def("IsButtonPressed", &Wasteland::Gamepad::IsButtonPressed,
                      py::arg("button"), py::arg("slot") = 0);
    // GetAxisRaw is UNPROCESSED -- no deadzone applied. GetAxisDeadzone below is
    // the filtered form. A script using the raw value has to handle resting drift
    // itself.
    gamepadModule.def("GetAxisRaw", &Wasteland::Gamepad::GetAxisRaw,
                      py::arg("axis"), py::arg("slot") = 0);
    // GetTrigger is a distinct call rather than a named axis, because triggers
    // are conventionally read as 0..1 even though they arrive as a signed axis.
    gamepadModule.def("GetTrigger", &Wasteland::Gamepad::GetTrigger,
                      py::arg("axis"), py::arg("slot") = 0);
    gamepadModule.def("GetAxisDeadzone", &Wasteland::Gamepad::GetAxisDeadzone,
                      py::arg("axis"), py::arg("slot") = 0,
                      py::arg("deadzone") = Wasteland::Gamepad::DefaultDeadzone);
    // ApplyDeadzone is exposed as a module function so a script can apply the
    // same curve to a value it read some other way.
    gamepadModule.def("ApplyDeadzone", &Wasteland::Gamepad::ApplyDeadzone,
                      py::arg("value"), py::arg("deadzone") = Wasteland::Gamepad::DefaultDeadzone);
    // Arg names must match the C++ parameter order exactly:
    // SetRumble(int gamepad, float lowFrequency, float highFrequency).
    // Binding these as (low, high, slot) silently remapped the first Python
    // argument onto the int `gamepad` parameter, so SetRumble(0.5, 0.8)
    // failed to convert 0.5 to int -- an engine bug that looked like a
    // pybind quirk.
    // SetRumble: the argument names here must match the C++ parameter order
    // (gamepad, lowFrequency, highFrequency). Getting this wrong silently
    // remapped a script's first float onto the int slot parameter -- an engine
    // bug that presented as a pybind type-conversion failure.
    gamepadModule.def("SetRumble", &Wasteland::Gamepad::SetRumble,
                      py::arg("gamepad"), py::arg("lowFrequency"),
                      py::arg("highFrequency"));
    gamepadModule.def("StopRumble", &Wasteland::Gamepad::StopRumble,
                      py::arg("gamepad"));

// GetState returns a plain dict rather than a bound GamepadState class. A
// dict is subscriptable by name, cannot outlive its poll in a dangerous way,
// and keeps the binding surface small.
// GetState returns a plain dict rather than a bound GamepadState class.
    // A dict is what a gameplay script actually wants (subscript by name,
    // no reference-semantics trap when a snapshot outlives its poll), and it
    // keeps the binding surface small.
    gamepadModule.def("GetState",
                      [](int slot) {
                          Wasteland::GamepadState state;
                          bool connected = Wasteland::Gamepad::GetState(slot, state);

py::dict result;
                          result["Connected"] = connected;
                          result["Name"] = state.Name;
                          py::list buttons;
                          // The button list runs to WL_GAMEPAD_BUTTON_LAST inclusive -- the engine's
                          // sentinel is the last valid index, not one past it. An off-by-one here would
                          // either drop a button or read one past the array.
                          for (int i = 0; i <= WL_GAMEPAD_BUTTON_LAST; ++i)
                              buttons.append(state.Buttons[i]);
                          result["Buttons"] = buttons;
                          py::list axes;
                          for (int i = 0; i <= WL_GAMEPAD_AXIS_LAST; ++i)
                              axes.append(state.Axes[i]);
                          result["Axes"] = axes;
                          return result;
                      },
                      py::arg("slot") = 0);

// GetConnectedGamepads returns only the CONNECTED pads, not a fixed-length
// list with gaps. count comes from the C++ side and bounds the loop; the
// vector is sized to match.
gamepadModule.def("GetConnectedGamepads",
                      // The GamepadState marshalling lambda. This is the longest hand-written
                      // conversion in the file because GamepadState is a C++ struct with fixed
                      // arrays, and no bind_vector or automatic conversion applies to it.
                      []() {
                          std::vector<Wasteland::GamepadState> states;
                          int count = Wasteland::Gamepad::GetConnectedGamepads(states);

py::list out;
                          for (int i = 0; i < count; ++i)
                          {
                              const Wasteland::GamepadState &s = states[i];
                              py::dict entry;
                              // The state is exposed as a DICTIONARY with capitalised keys -- Connected,
                              // Name, Buttons, Axes -- rather than as an object with properties. So a
                              // script reads state["Axes"][0] and the key names are fixed strings that
                              // nothing checks.
                              entry["Connected"] = s.Connected;
                              entry["Name"] = s.Name;
                              py::list buttons;
                              // Buttons and Axes are converted to py::list by looping to *_LAST, so the
                              // lists are indexed POSITIONALLY. A script must know that index 0 is A on
                              // Xbox layout and 0 is Cross on PlayStation -- the values are the same but
                              // the meanings differ by vendor, and nothing here records which is which.
                              for (int b = 0; b <= WL_GAMEPAD_BUTTON_LAST; ++b)
                                  // The loop bound is <= LAST, so the lists are LAST+1 long and include the
                                  // sentinel. That is one more element than a script iterating 0..count-1
                                  // would expect, and the extra entry is a valid index rather than an error.
                                  buttons.append(s.Buttons[b]);
                              entry["Buttons"] = buttons;
                              py::list axes;
                              for (int a = 0; a <= WL_GAMEPAD_AXIS_LAST; ++a)
                                  axes.append(s.Axes[a]);
                              entry["Axes"] = axes;
                              out.append(entry);
                          }
                          return out;
                      });

// Left stick as a normalized direction. Returned as a tuple rather than
    // mutating out-params, which pybind11 handles but reads badly at the
    // call site.
    // GetLeftStick returns a tuple rather than filling out-parameters. pybind can
    // bind float& and mutate a caller's list, but it reads badly at the call site
    // and silently does nothing without a reference_internal return policy.
    gamepadModule.def("GetLeftStick",
                      [](int slot, float deadzone) {
                          float x = 0.0f, y = 0.0f;
                          Wasteland::Gamepad::GetLeftStick(slot, x, y, deadzone);
                          return py::make_tuple(x, y);
                      },
                      py::arg("slot") = 0,
                      // The deadzone argument defaults to the engine's DefaultDeadzone, so stick
                      // noise is filtered the same way the engine filters it.
                      py::arg("deadzone") = Wasteland::Gamepad::DefaultDeadzone);

// --------------------------------------------------------- Core: KeyCodes --
    // Key constants, same macro-loop shape as the gamepad buttons. The WL_KEY_
    // prefix is kept so script constants cannot collide with anything else on the
    // root module.
    // ONE macro expands to all ~120 key bindings. Each expansion is
    // `m.attr("WL_KEY_A") = WL_KEY_A` -- string-pasted name on the left, real
    // constant on the right. Doing this by hand would be 120 lines of near-identical
    // code with a 120-line chance of a typo; the macro makes the name impossible
    // to mistype because it is pasted from the same token that supplies the value.
    #define BIND_KEY(k) m.attr("WL_KEY_" #k) = WL_KEY_##k
    BIND_KEY(UNKNOWN);
    BIND_KEY(SPACE);
    BIND_KEY(APOSTROPHE);
    BIND_KEY(COMMA);
    BIND_KEY(MINUS);
    BIND_KEY(PERIOD);
    BIND_KEY(SLASH);
    // Digits come from the numeric keycodes, not the letters. Note BIND_KEY(0)
    // pastes to WL_KEY_0 -- a valid Python identifier, unlike a bare number.
    BIND_KEY(0);
    BIND_KEY(1);
    BIND_KEY(2);
    BIND_KEY(3);
    BIND_KEY(4);
    BIND_KEY(5);
    BIND_KEY(6);
    BIND_KEY(7);
    BIND_KEY(8);
    BIND_KEY(9);
    BIND_KEY(SEMICOLON);
    BIND_KEY(EQUAL);
    BIND_KEY(A);
    BIND_KEY(B);
    BIND_KEY(C);
    BIND_KEY(D);
    BIND_KEY(E);
    BIND_KEY(F);
    BIND_KEY(G);
    BIND_KEY(H);
    BIND_KEY(I);
    BIND_KEY(J);
    BIND_KEY(K);
    BIND_KEY(L);
    BIND_KEY(M);
    BIND_KEY(N);
    BIND_KEY(O);
    BIND_KEY(P);
    BIND_KEY(Q);
    BIND_KEY(R);
    BIND_KEY(S);
    BIND_KEY(T);
    BIND_KEY(U);
    BIND_KEY(V);
    BIND_KEY(W);
    BIND_KEY(X);
    BIND_KEY(Y);
    BIND_KEY(Z);
    BIND_KEY(LEFT_BRACKET);
    BIND_KEY(BACKSLASH);
    BIND_KEY(RIGHT_BRACKET);
    BIND_KEY(GRAVE_ACCENT);
    // WORLD_1/WORLD_2 are the physical key positions above the number row, not
    // the character '1'/'2'. They are separate codes precisely so a layout that
    // puts a different character there still reports the same physical key.
    BIND_KEY(WORLD_1);
    BIND_KEY(WORLD_2);
    BIND_KEY(ESCAPE);
    BIND_KEY(ENTER);
    BIND_KEY(TAB);
    BIND_KEY(BACKSPACE);
    // BIND_KEY(DELETE) becomes WL_KEY_DELETE. Note the token DELETE is a C++
    // keyword, but token-pasting it into an identifier is fine -- the preprocessor
    // does not treat the pasted result as a keyword.
    BIND_KEY(INSERT);
    BIND_KEY(DELETE);
    BIND_KEY(RIGHT);
    BIND_KEY(LEFT);
    BIND_KEY(DOWN);
    BIND_KEY(UP);
    BIND_KEY(PAGE_UP);
    BIND_KEY(PAGE_DOWN);
    BIND_KEY(HOME);
    BIND_KEY(END);
    BIND_KEY(CAPS_LOCK);
    BIND_KEY(SCROLL_LOCK);
    BIND_KEY(NUM_LOCK);
    BIND_KEY(PRINT_SCREEN);
    BIND_KEY(PAUSE);
    // F1-F25: a deliberately oversized range. GLFW exposes 25 and the binding
    // mirrors all of them rather than the 12 a keyboard actually has, so a script
    // testing for a high function key gets a real code instead of an exception.
    BIND_KEY(F1);
    BIND_KEY(F2);
    BIND_KEY(F3);
    BIND_KEY(F4);
    BIND_KEY(F5);
    BIND_KEY(F6);
    BIND_KEY(F7);
    BIND_KEY(F8);
    BIND_KEY(F9);
    BIND_KEY(F10);
    BIND_KEY(F11);
    BIND_KEY(F12);
    BIND_KEY(F13);
    BIND_KEY(F14);
    BIND_KEY(F15);
    BIND_KEY(F16);
    BIND_KEY(F17);
    BIND_KEY(F18);
    BIND_KEY(F19);
    BIND_KEY(F20);
    BIND_KEY(F21);
    BIND_KEY(F22);
    BIND_KEY(F23);
    BIND_KEY(F24);
    BIND_KEY(F25);
    // KP_* is the keypad. SEPARATE codes from the number row, so a script can
    // tell which one the player pressed -- which matters for any control scheme
    // that binds numeric input.
    BIND_KEY(KP_0);
    BIND_KEY(KP_1);
    BIND_KEY(KP_2);
    BIND_KEY(KP_3);
    BIND_KEY(KP_4);
    BIND_KEY(KP_5);
    BIND_KEY(KP_6);
    BIND_KEY(KP_7);
    BIND_KEY(KP_8);
    BIND_KEY(KP_9);
    BIND_KEY(KP_DECIMAL);
    BIND_KEY(KP_DIVIDE);
    BIND_KEY(KP_MULTIPLY);
    BIND_KEY(KP_SUBTRACT);
    BIND_KEY(KP_ADD);
    BIND_KEY(KP_ENTER);
    BIND_KEY(KP_EQUAL);
    // LEFT_SHIFT/RIGHT_CONTROL/RIGHT_ALT/RIGHT_SUPER but no LEFT_SUPER. That is
    // not a typo in this file -- GLFW's left super key is bound under a different
    // name, so a script asking for the left super modifier does not find one.
    // Worth knowing before writing a platform-shortcut binding.
    BIND_KEY(LEFT_SHIFT);
    BIND_KEY(RIGHT_SHIFT);
    BIND_KEY(LEFT_CONTROL);
    BIND_KEY(RIGHT_CONTROL);
    BIND_KEY(RIGHT_ALT);
    BIND_KEY(RIGHT_SUPER);
    BIND_KEY(MENU);
    // LAST is exported like any other key so a script can iterate the full range
    // without hardcoding a count.
    // LAST is exported as a key like any other, so a script can iterate 0..LAST
    // to enumerate the range rather than hardcoding a count that would go stale
    // the moment a key is added.
    BIND_KEY(LAST);
    // The #undef matters: without it BIND_KEY stays defined for the rest of the
    // translation unit and any later identifier ending in a matching token
    // sequence would be pasted unexpectedly. Undefining is what confines the
    // macro to this list.
    #undef BIND_KEY

// ----------------------------------------------------- Core: MouseButtons --
    // Mouse buttons. Note the numeric ones (1..5) are exported ALONGSIDE the
    // named ones (LEFT/RIGHT/MIDDLE) -- the numeric set is presumably the raw
    // engine indices and the named set the semantic aliases.
    #define BIND_MB(b) m.attr("WL_MOUSE_BUTTON_" #b) = WL_MOUSE_BUTTON_##b
    BIND_MB(1);
    BIND_MB(2);
    BIND_MB(3);
    BIND_MB(4);
    BIND_MB(5);
    BIND_MB(6);
    BIND_MB(LAST);
    BIND_MB(LEFT);
    BIND_MB(RIGHT);
    BIND_MB(MIDDLE);
    #undef BIND_MB

// ---------------------------------------------------------- Core: Window --
    // WindowProps is a value struct carrying the three things Create needs.
    // Every field has a default, so WindowProps() from Python is a usable
    // 1280x720 window titled "Wasteland Engine".
    py::class_<Wasteland::WindowProps>(m, "WindowProps")
        .def(py::init<const std::string &, uint32_t, uint32_t>(),
             // The defaults are duplicated between the constructor signature here and
             // whatever C++ declares. Two sources of truth for the same defaults.
             py::arg("title") = "Wasteland Engine",
             py::arg("width") = 1280,
             py::arg("height") = 720)
        .def_readwrite("Title", &Wasteland::WindowProps::Title)
        .def_readwrite("Width", &Wasteland::WindowProps::Width)
        .def_readwrite("Height", &Wasteland::WindowProps::Height);

// Window_Create is a module-level function rather than a static, because
// Window::Create returns a Ref<Window> and is a factory. The trailing
// indentation here (column 0) is cosmetic.
m.def("Window_Create", &Wasteland::Window::Create,
          py::arg("props") = Wasteland::WindowProps());

// Window with a shared_ptr holder, matching the Ref<Window> that Create
// returns. GetNativeWindow below hands back a raw HWND as an opaque integer,
// which is how a script reaches platform interop without a windowing
// binding.
py::class_<Wasteland::Window, std::shared_ptr<Wasteland::Window> >(m, "Window")
        .def("GetWidth", &Wasteland::Window::GetWidth)
        .def("GetHeight", &Wasteland::Window::GetHeight)
        .def("SetVSync", &Wasteland::Window::SetVSync)
        .def("IsVSync", &Wasteland::Window::IsVSync)
        .def("GetNativeWindow", &Wasteland::Window::GetNativeWindow)
        // SwitchRendererAPI takes the RendererAPIAPI enum, registered later in the
        // file at 1876. Forward reference is fine because pybind resolves the enum
        // at call time, not at definition time.
        .def("SwitchRendererAPI", &Wasteland::Window::SwitchRendererAPI, py::arg("api"))
        .def("GetCurrentContext", &Wasteland::Window::GetCurrentContext)
        .def("OnUpdate", &Wasteland::Window::OnUpdate)
        // BeginFrame and OnUpdate are the frame hooks. They are bound as methods on
        // the shared Window instance, so a script holding the window can drive its
        // own frame -- which is how a headless test steps the renderer.
        .def("BeginFrame", &Wasteland::Window::BeginFrame)
        // Mouse capture. Exposed so a gameplay script can grab the cursor on
        // entering play and -- crucially -- release it on exit, rather than
        // the editor having to know which entities want it.
        // Mouse capture, exposed so a gameplay script can grab the cursor on
        // entering play and release it on exit. Asymmetric ownership here is a
        // classic leak: capture without a matching release strands the cursor.
        .def("SetMouseCaptured", &Wasteland::Window::SetMouseCaptured)
        .def("IsMouseCaptured", &Wasteland::Window::IsMouseCaptured);

// -------------------------------------------------------- Core: Application --
    // Application as a singleton, reached through the static Get().
    py::class_<Wasteland::Application>(m, "Application")
        .def_static("Get", &Wasteland::Application::Get)
        .def_static("IsInitialized", &Wasteland::Application::IsInitialized)
        // GetWindow returns a REFERENCE, not a copy. An Application owns its window
        // for the process lifetime, so there is nothing to tie a lifetime to -- but
        // the default policy would have copied, and a script that set VSYNC on the
        // copy would see no effect.
        .def("GetWindow", &Wasteland::Application::GetWindow,
             py::return_value_policy::reference)
        .def("Close", &Wasteland::Application::Close)
        .def("GetImGuiLayer", &Wasteland::Application::GetImGuiLayer);

// ----------------------------------------------------------- Core: Layer --
    // Layer is the unit of the layer stack. Bound with virtual methods so a
    // Python class can subclass it: OnAttach/OnUpdate/OnEvent are the hooks a
    // script layer overrides.
    py::class_<Wasteland::Layer>(m, "Layer")
        .def(py::init<const std::string &>(), py::arg("name") = "Layer")
        .def("GetName", &Wasteland::Layer::GetName)
        .def("OnAttach", &Wasteland::Layer::OnAttach)
        .def("OnDetach", &Wasteland::Layer::OnDetach)
        .def("OnUpdate", &Wasteland::Layer::OnUpdate, py::arg("ts"))
        .def("OnImGuiRender", &Wasteland::Layer::OnImGuiRender)
        .def("OnEvent", &Wasteland::Layer::OnEvent, py::arg("event"));

// LayerStack. Four methods, push/pop for both layers and overlays. Overlays
// sit above layers in the stack and are not affected by the layer ordering,
// which is why they need separate calls.
py::class_<Wasteland::LayerStack>(m, "LayerStack")
        .def(py::init<>())
        // PopLayer takes the layer to remove rather than an index. Matching the push
        // shape means a caller that kept the object can pop it.
        .def("PushLayer", &Wasteland::LayerStack::PushLayer, py::arg("layer"))
        .def("PushOverlay", &Wasteland::LayerStack::PushOverlay, py::arg("overlay"))
        .def("PopLayer", &Wasteland::LayerStack::PopLayer, py::arg("layer"))
        .def("PopOverlay", &Wasteland::LayerStack::PopOverlay, py::arg("overlay"));

// --------------------------------------------------------- Utils: Files --
    // File dialogs as root-level functions. Both take a filter string and return
    // a path. Neither is guarded by a check for whether a display exists, so a
    // headless call may block or fail depending on the platform.
    m.def("FileDialogs_OpenFile", &Wasteland::FileDialogs::OpenFile, py::arg("filter"));
    m.def("FileDialogs_SaveFile", &Wasteland::FileDialogs::SaveFile, py::arg("filter"));

// ---------------------------------------------------------- Math helper --
    // Math_DecomposeTransform is an out-parameter form: the bool is only a
    // success flag, and the actual TRS values come back by mutating the three
    // glm::vec3 arguments the caller passed in.
    m.def("Math_DecomposeTransform",
          // None of the three py::arg names carries a default, so a caller must supply
          // all three even though they are purely outputs -- there is no
          // `DecomposeTransform(m)` form. That makes the call site read as three
          // throwaway Vec3s plus three out-parameters, which is the shape a
          // return-a-tuple lambda exists to avoid.
          &Wasteland::Math::DecomposeTransform,
          py::arg("transform"),
          py::arg("translation"),
          py::arg("rotation"),
          py::arg("scale"));

// ============================================ EVENTS ====================
    // EventType, the closed set of event kinds. Bound as an enum rather than an
    // int so a script can compare symbolically.
    // EventType: the full event vocabulary as one flat enum, with hand-written
    // .value() calls rather than an auto-conversion macro. The explicit form
    // means the PYTHON names are decoupled from the C++ ones -- "None" is
    // bound here because a Python attribute cannot be spelled None.
    py::enum_<Wasteland::EventType>(m, "EventType")
        .value("None",            Wasteland::EventType::None)
        .value("WindowClose",     Wasteland::EventType::WindowClose)
        .value("WindowResize",    Wasteland::EventType::WindowResize)
        .value("WindowFocus",     Wasteland::EventType::WindowFocus)
        .value("WindowLostFocus", Wasteland::EventType::WindowLostFocus)
        .value("WindowMoved",     Wasteland::EventType::WindowMoved)
        // AppTick/AppUpdate/AppRender are the engine's three loop phases as events,
        // so a script can subscribe to a phase rather than being called from one.
        // Note they are EVENTS: the same EventHandler path serves them, so a
        // per-phase handler runs from the dispatch loop like any other.
        .value("AppTick",         Wasteland::EventType::AppTick)
        .value("AppUpdate",       Wasteland::EventType::AppUpdate)
        .value("AppRender",       Wasteland::EventType::AppRender)
        .value("KeyPressed",      Wasteland::EventType::KeyPressed)
        .value("KeyReleased",     Wasteland::EventType::KeyReleased)
        // KeyTyped is distinct from KeyPressed: Typed is the TEXT input, i.e. the
        // character produced after layout and modifiers. A script binding a text
        // field wants Typed; one binding a hotkey wants Pressed. Conflating them is
        // the classic input-binding mistake, and both are available so the
        // distinction has to be made deliberately.
        .value("KeyTyped",        Wasteland::EventType::KeyTyped)
        // MouseButton enum values are spelled out by hand below. The indentation
        // of this block differs from its neighbours, purely cosmetic.
        .value("MouseButtonPressed",  Wasteland::EventType::MouseButtonPressed)
        .value("MouseButtonReleased", Wasteland::EventType::MouseButtonReleased)
        .value("MouseMoved",      Wasteland::EventType::MouseMoved)
        .value("MouseScrolled",   Wasteland::EventType::MouseScrolled);

py::enum_<Wasteland::EventCategory>(m, "EventCategory")
        // EventCategory. Note the Python names are short ("Input", "Keyboard")
        // while the C++ enumerators carry a redundant prefix
        // (EventCategoryInput). The renaming happens here, which is deliberate: the
        // script author writes EventCategory.Input, not Wasteland.EventCategoryInput.
        .value("None",              Wasteland::EventCategory::None)
        .value("Application",       Wasteland::EventCategory::EventCategoryApplication)
        .value("Input",             Wasteland::EventCategory::EventCategoryInput)
        .value("Keyboard",          Wasteland::EventCategory::EventCategoryKeyboard)
        .value("Mouse",             Wasteland::EventCategory::EventCategoryMouse)
        .value("MouseButton",       Wasteland::EventCategory::EventCategoryMouseButton);

// Event base class. IsInCategory takes the CATEGORY, not the type, so a
// handler subscribes to "keyboard" and receives all three keyboard events.
py::class_<Wasteland::Event>(m, "Event")
        .def("GetEventType", &Wasteland::Event::GetEventType)
        .def("GetName", &Wasteland::Event::GetName)
        .def("GetCategoryFlags", &Wasteland::Event::GetCategoryFlags)
        .def("IsInCategory", &Wasteland::Event::IsInCategory, py::arg("category"))
        .def("ToString", &Wasteland::Event::ToString)
        // Handled is readwrite because it is the propagation mechanism: the engine
        // walks the layer stack and stops feeding an event once Handled is set. A
        // script that forgets to set it sees the same event again in every layer.
        .def_readwrite("Handled", &Wasteland::Event::Handled);

// --- Application events ---
    // WindowCloseEvent is bound with NO members at all -- just the type, so a
    // script can isinstance-check it. The engine constructs these, never Python.
    py::class_<Wasteland::WindowCloseEvent, Wasteland::Event>(m, "WindowCloseEvent");
    // WindowResizeEvent carries width/height. Exposing the getters rather than
    // the fields keeps the C++ members free to change.
    py::class_<Wasteland::WindowResizeEvent, Wasteland::Event>(m, "WindowResizeEvent")
        .def(py::init<unsigned int, unsigned int>(),
             py::arg("width"), py::arg("height"))
        .def("GetWidth", &Wasteland::WindowResizeEvent::GetWidth)
        .def("GetHeight", &Wasteland::WindowResizeEvent::GetHeight);
    // The three app-level events are pure type declarations. They exist so a
    // script can branch on what kind of frame event it received.
    py::class_<Wasteland::AppTickEvent, Wasteland::Event>(m, "AppTickEvent");
    py::class_<Wasteland::AppUpdateEvent, Wasteland::Event>(m, "AppUpdateEvent");
    py::class_<Wasteland::AppRenderEvent, Wasteland::Event>(m, "AppRenderEvent");

// --- Keyboard events ---
    // KeyEvent is the intermediate base carrying GetKeyCode, so a script
    // handling all keyboard events needs one check rather than three.
    py::class_<Wasteland::KeyEvent, Wasteland::Event>(m, "KeyEvent")
        .def("GetKeyCode", &Wasteland::KeyEvent::GetKeyCode);
    py::class_<Wasteland::KeyPressedEvent, Wasteland::KeyEvent>(m, "KeyPressedEvent")
        // KeyPressedEvent's repeatCount is the OS auto-repeat count: 1 for a fresh
        // press, higher when the key is held. A script that fires once per event
        // rather than once per repeat will auto-fire while the key is held.
        .def(py::init<int, int>(), py::arg("keycode"), py::arg("repeatCount"))
        .def("GetRepeatCount", &Wasteland::KeyPressedEvent::GetRepeatCount);
    py::class_<Wasteland::KeyReleasedEvent, Wasteland::KeyEvent>(m, "KeyReleasedEvent")
        .def(py::init<int>(), py::arg("keycode"));
    // KeyTypedEvent carries the keycode too, which is slightly odd for a text
    // event -- the character itself is not exposed. A script wanting typed text
    // has to map the code itself.
    py::class_<Wasteland::KeyTypedEvent, Wasteland::KeyEvent>(m, "KeyTypedEvent")
        .def(py::init<int>(), py::arg("keycode"));

// --- Mouse events ---
    // MouseMovedEvent exposes X and Y. There is no delta here; the delta is on
    // Input.GetMouseDelta at the root module.
    py::class_<Wasteland::MouseMovedEvent, Wasteland::Event>(m, "MouseMovedEvent")
        .def(py::init<float, float>(), py::arg("x"), py::arg("y"))
        .def("GetX", &Wasteland::MouseMovedEvent::GetX)
        .def("GetY", &Wasteland::MouseMovedEvent::GetY);
    // MouseScrolledEvent offsets can be fractional. A script testing `> 0` on a
    // trackpad gets fractional deltas, so a threshold is safer than a sign test.
    py::class_<Wasteland::MouseScrolledEvent, Wasteland::Event>(m, "MouseScrolledEvent")
        .def(py::init<float, float>(), py::arg("xOffset"), py::arg("yOffset"))
        .def("GetXOffset", &Wasteland::MouseScrolledEvent::GetXOffset)
        .def("GetYOffset", &Wasteland::MouseScrolledEvent::GetYOffset);
    // MouseButtonEvent is the base for pressed/released, carrying the button
    // code. The two derived types are constructed by the engine.
    py::class_<Wasteland::MouseButtonEvent, Wasteland::Event>(m, "MouseButtonEvent")
        .def("GetMouseButton", &Wasteland::MouseButtonEvent::GetMouseButton);
    py::class_<Wasteland::MouseButtonPressedEvent, Wasteland::MouseButtonEvent>(m, "MouseButtonPressedEvent")
        .def(py::init<int>(), py::arg("button"));
    py::class_<Wasteland::MouseButtonReleasedEvent, Wasteland::MouseButtonEvent>(m, "MouseButtonReleasedEvent")
        .def(py::init<int>(), py::arg("button"));

// The event-handler hook. Both functions are extern "C" and defined in
// Application.cpp; the declarations at the top of this file match. This is
// how a Python callable gets invoked for engine events that the C++
// dispatcher handles.
m.def("SetEventHandler", &Wasteland_SetPythonEventHandler, py::arg("handler"));
    // GetEventHandler returns the stored py::object. If nothing was ever set,
    // this returns None -- and the C++ side must check for that before calling
    // it.
    m.def("GetEventHandler", &Wasteland_GetPythonEventHandler);

// ========================================= RENDERER ====================
    // --- ShaderDataType ---
    // ShaderDataType: the vertex-layout type vocabulary. Bound at the root
    // rather than in a Renderer submodule, since BufferElement below exposes it
    // and scripts building a layout need it by name.
    py::enum_<Wasteland::ShaderDataType>(m, "ShaderDataType")
        .value("None",   Wasteland::ShaderDataType::None)
        .value("Float",  Wasteland::ShaderDataType::Float)
        .value("Float2", Wasteland::ShaderDataType::Float2)
        .value("Float3", Wasteland::ShaderDataType::Float3)
        .value("Float4", Wasteland::ShaderDataType::Float4)
        .value("Mat3",   Wasteland::ShaderDataType::Mat3)
        .value("Mat4",   Wasteland::ShaderDataType::Mat4)
        .value("Int",    Wasteland::ShaderDataType::Int)
        .value("Int2",   Wasteland::ShaderDataType::Int2)
        .value("Int3",   Wasteland::ShaderDataType::Int3)
        .value("Int4",   Wasteland::ShaderDataType::Int4)
        .value("Bool",   Wasteland::ShaderDataType::Bool);

// --- BufferElement ---
    // BufferElement describes one attribute in a vertex layout: type, name, and
    // whether integer data should be normalised to 0..1 on upload.
    py::class_<Wasteland::BufferElement>(m, "BufferElement")
        .def(py::init<>())
        // The three-argument constructor mirrors the C++ one. `normalized` defaults
        // to false, which is right for floats and wrong for packed colour -- a
        // script binding a byte-colour attribute has to pass it explicitly.
        .def(py::init<Wasteland::ShaderDataType, const std::string &, bool>(),
             py::arg("type"), py::arg("name"), py::arg("normalized") = false)
        .def_readwrite("Name", &Wasteland::BufferElement::Name)
        .def_readwrite("Type", &Wasteland::BufferElement::Type)
        .def_readwrite("Offset", &Wasteland::BufferElement::Offset)
        .def_readwrite("Size", &Wasteland::BufferElement::Size)
        .def_readwrite("Normalized", &Wasteland::BufferElement::Normalized)
        .def("GetComponentCount", &Wasteland::BufferElement::GetComponentCount);

// --- BufferLayout ---
    // BufferLayout holds the elements plus the computed stride. GetStride is
    // derived from the elements, so a script that mutates an element after
    // construction may need to re-derive it.
    py::class_<Wasteland::BufferLayout>(m, "BufferLayout")
        .def(py::init<>())
        .def(py::init<const std::vector<Wasteland::BufferElement> &>(),
             py::arg("elements"))
        .def("GetStride", &Wasteland::BufferLayout::GetStride)
        // GetElements returns a REFERENCE, not a copy. A layout whose element list a
        // script cannot edit is only half usable, and the default copy policy would
        // have silently discarded any mutation.
        .def("GetElements", &Wasteland::BufferLayout::GetElements,
             py::return_value_policy::reference);

// --- VertexBuffer ---
    // VertexBuffer with a Ref<> holder, matching how the renderer passes buffers
    // around. Create is a static factory returning a Ref, so it is def_static.
    py::class_<Wasteland::VertexBuffer, Wasteland::Ref<Wasteland::VertexBuffer> >(m, "VertexBuffer")
        // py::overload_cast is required because Create is overloaded: this form
        // takes a byte size, the other below takes data. Without the explicit cast
        // the compiler cannot pick between them.
        .def_static("Create", py::overload_cast<uint32_t>(&Wasteland::VertexBuffer::Create), py::arg("size"))
        // CreateFromData takes a Python list of floats and uploads it. The const_cast
        // is safe only because the underlying Create does not retain the pointer --
        // it copies into the GPU buffer immediately. If that ever changed to keep a
        // reference, the const_cast would be a dangling-pointer bug.
        .def_static("CreateFromData",
             [](uint32_t count, const std::vector<float> &data) {
                 return Wasteland::VertexBuffer::Create(const_cast<float*>(data.data()), count * sizeof(float));
             }, py::arg("count"), py::arg("data"))
        .def("Bind", &Wasteland::VertexBuffer::Bind)
        .def("Unbind", &Wasteland::VertexBuffer::Unbind)
        // SetData takes a byte list. There is no size argument: the list length IS
        // the size, which removes the opportunity for the two to disagree.
        .def("SetData",
             [](Wasteland::VertexBuffer &vb, const std::vector<uint8_t> &data) {
                 vb.SetData(data.data(), (uint32_t)data.size());
             }, py::arg("data"))
        .def("SetLayout", &Wasteland::VertexBuffer::SetLayout, py::arg("layout"))
        .def("GetLayout", &Wasteland::VertexBuffer::GetLayout,
             py::return_value_policy::reference);

// --- IndexBuffer ---
    // IndexBuffer. Create takes a list of indices; SetData takes bytes, so the
    // two paths disagree about units -- count for Create, size-in-bytes for
    // SetData's internal computation.
    py::class_<Wasteland::IndexBuffer, Wasteland::Ref<Wasteland::IndexBuffer> >(m, "IndexBuffer")
        .def_static("Create",
             [](const std::vector<uint32_t> &indices) {
                 return Wasteland::IndexBuffer::Create(const_cast<uint32_t*>(indices.data()),
                                                      (uint32_t)indices.size());
             }, py::arg("indices"))
        .def("Bind", &Wasteland::IndexBuffer::Bind)
        .def("Unbind", &Wasteland::IndexBuffer::Unbind)
        .def("SetData",
             [](Wasteland::IndexBuffer &ib, const std::vector<uint32_t> &data) {
                 ib.SetData(data.data(), (uint32_t)(data.size() * sizeof(uint32_t)));
             }, py::arg("data"))
        .def("GetCount", &Wasteland::IndexBuffer::GetCount);

// VertexArray ties the vertex buffers and the optional index buffer together
// into one draw call. Bind/Unbind are state, not data.
// --- VertexArray ---
    py::class_<Wasteland::VertexArray, Wasteland::Ref<Wasteland::VertexArray> >(m, "VertexArray")
        .def_static("Create", &Wasteland::VertexArray::Create)
        .def("Bind", &Wasteland::VertexArray::Bind)
        .def("Unbind", &Wasteland::VertexArray::Unbind)
        .def("AddVertexBuffer", &Wasteland::VertexArray::AddVertexBuffer,
             py::arg("buffer"))
        .def("SetIndexBuffer", &Wasteland::VertexArray::SetIndexBuffer,
             py::arg("buffer"))
        .def("GetVertexBuffers", &Wasteland::VertexArray::GetVertexBuffers)
        .def("GetIndexBuffer", &Wasteland::VertexArray::GetIndexBuffer,
             py::return_value_policy::reference);

// =============================== RENDERER: Shader ======================
    // THIS is the shader-writing API: pass GLSL/HLSL source strings from Python.
    // Shader: the actual shader-writing API. CreateFromSource takes source as
    // strings, which is what makes shader authoring scriptable at all -- a
    // script can compile a shader, tweak it, and recompile without a rebuild.
    py::class_<Wasteland::Shader, Wasteland::Ref<Wasteland::Shader> >(m, "Shader")
        // The overload_cast again: Create is overloaded on (filepath) and
        // (name, vertexSrc, fragmentSrc).
        .def_static("CreateFromFile",
             py::overload_cast<const std::string &>(&Wasteland::Shader::Create),
             py::arg("filepath"))
        .def_static("CreateFromSource",
             py::overload_cast<const std::string &, const std::string &, const std::string &>(&Wasteland::Shader::Create),
             py::arg("name"), py::arg("vertexSrc"), py::arg("fragmentSrc"))
        .def("Bind", &Wasteland::Shader::Bind)
        .def("Unbind", &Wasteland::Shader::Unbind)
        .def("SetInt", &Wasteland::Shader::SetInt, py::arg("name"), py::arg("value"))
        // SetIntArray takes an explicit COUNT alongside the list. That is a
        // footgun: passing a count that disagrees with the list length reads past
        // the end, and a count smaller than the list silently ignores the tail.
        .def("SetIntArray", &Wasteland::Shader::SetIntArray,
             py::arg("name"), py::arg("values"), py::arg("count"))
        .def("SetFloat", &Wasteland::Shader::SetFloat, py::arg("name"), py::arg("value"))
        .def("SetFloat2", &Wasteland::Shader::SetFloat2, py::arg("name"),
             py::arg("value"))
        .def("SetFloat3", &Wasteland::Shader::SetFloat3, py::arg("name"),
             py::arg("value"))
        .def("SetFloat4", &Wasteland::Shader::SetFloat4, py::arg("name"),
             py::arg("value"))
        // SetMat4 takes a Mat4 -- which, per the Mat4 binding above, a script can
        // barely construct. So in practice SetMat4 is reachable only with a matrix
        // obtained from somewhere else in C++.
        .def("SetMat4", &Wasteland::Shader::SetMat4, py::arg("name"),
             py::arg("value"))
        .def("GetRendererID", &Wasteland::Shader::GetRendererID)
        .def("GetName", &Wasteland::Shader::GetName);

// --- ShaderLibrary ---
    // ShaderLibrary: a named cache of shaders. No holder and no shared_ptr --
    // the library is a value type scripts construct and populate themselves.
    py::class_<Wasteland::ShaderLibrary>(m, "ShaderLibrary")
        .def(py::init<>())
        // Add is bound TWICE, once per C++ overload (with and without a name).
        // Unlike the __mul__ case above, named methods accumulate overloads rather
        // than replacing, so both are callable; the unnamed form registers under the
        // shader's own name.
        // ShaderLibrary::Add and Load each have TWO overloads -- with and without an
        // explicit name -- registered under one Python name. The no-name form derives
        // the name from the shader or the file, which is the common case.
        .def("Add",
             py::overload_cast<const std::string &,
                               const Wasteland::Ref<Wasteland::Shader> &>(
                 &Wasteland::ShaderLibrary::Add),
             py::arg("name"), py::arg("shader"))
        .def("Add",
             py::overload_cast<const Wasteland::Ref<Wasteland::Shader> &>(
                 &Wasteland::ShaderLibrary::Add),
             py::arg("shader"))
        // Load's two overloads differ in a way the argument names make subtle:
        // (filepath) versus (name, filepath). A script reading `Load("a.glsl")`
        // could reasonably expect the single-argument form to be the named one.
        .def("Load",
             py::overload_cast<const std::string &>(&Wasteland::ShaderLibrary::Load),
             py::arg("filepath"))
        .def("Load",
             py::overload_cast<const std::string &, const std::string &>(
                 &Wasteland::ShaderLibrary::Load),
             py::arg("name"), py::arg("filepath"))
        // Get returns a Ref<Shader> with no explicit policy. Because Shader is
        // registered with a Ref<> holder, pybind treats the return as a shared
        // handle rather than a raw pointer, so there is no ownership question here.
        // Get returns a shader by name with NO policy, so pybind's default applies
        // -- a copy or a reference depending on what ShaderLibrary::Get returns.
        // Exists above is the non-throwing alternative, and a script should prefer it
        // when probing.
        .def("Get", &Wasteland::ShaderLibrary::Get, py::arg("name"))
        .def("Exists", &Wasteland::ShaderLibrary::Exists, py::arg("name"));

// =============================== RENDERER: Texture =====================
    // Texture base class. The Ref<> holder matches the renderer's handle type.
    // Texture with a Ref<> holder -- a reference-counted smart handle, NOT
    // shared_ptr. pybind needs the exact holder type declared here to match
    // what the engine returns; declaring shared_ptr instead would compile and
    // then hand Python the wrong ownership story.
    py::class_<Wasteland::Texture, Wasteland::Ref<Wasteland::Texture> >(m, "Texture")
        .def("GetWidth", &Wasteland::Texture::GetWidth)
        .def("GetHeight", &Wasteland::Texture::GetHeight)
        .def("GetRendererID", &Wasteland::Texture::GetRendererID)
        // SetData takes a byte list and const_casts away const for the same
        // immediate-upload reason as VertexBuffer.CreateFromData. It also takes no
        // dimensions -- the texture must already have been sized.
        // SetData uploads raw bytes. There is no length argument because the byte
        // count is implied by the format and dimensions -- a script that supplies a
        // short buffer is relying on that implication being honoured.
        .def("SetData",
             [](Wasteland::Texture &t, const std::vector<uint8_t> &data) {
                 t.SetData(const_cast<void *>(
                        reinterpret_cast<const void *>(data.data())),
                           (uint32_t)data.size());
             }, py::arg("data"))
        // Bind takes a SLOT, defaulting to 0. Slot assignment is global renderer
        // state, so two textures bound to the same slot in one frame is a silent
        // wrong-texture bug rather than an error.
        .def("Bind", &Wasteland::Texture::Bind, py::arg("slot") = 0)
        // __eq__ on a Ref-wrapped type compares by handle identity, not by
        // contents. Two Refs to the same texture compare equal; two textures with
        // identical pixels do not.
        // __eq__ on a Ref<> handle. Two wrappers compare equal when they point at
        // the same underlying texture, so a script can test identity without
        // reaching for a renderer id.
        .def("__eq__", &Wasteland::Texture::operator==, py::arg("other"));

// Texture2D derives from Texture in pybind, so a Texture2D instance passes
// an isinstance(texture, Wasteland.Texture) check. The holder is Ref<Texture2D>
// -- a DIFFERENT holder type from the base -- which is what pybind requires
// for a derived class with its own Ref.
// Texture2D declares a DIFFERENT holder from its base Texture, and pybind
// requires it: the holder is part of the registered type identity. Omitting
// the base's holder here is not a shorthand.
py::class_<Wasteland::Texture2D, Wasteland::Texture,
             Wasteland::Ref<Wasteland::Texture2D> >(m, "Texture2D")
        .def_static("Create", py::overload_cast<uint32_t, uint32_t>(&Wasteland::Texture2D::Create),
             py::arg("width"), py::arg("height"))
        .def_static("CreateFromFile", py::overload_cast<const std::string &>(&Wasteland::Texture2D::Create), py::arg("path"))
        .def("Resize", &Wasteland::Texture2D::Resize,
             py::arg("width"), py::arg("height"))
        .def("GetNativeSRV", &Wasteland::Texture2D::GetNativeSRV);

// SubTexture2D: a rectangular region of a Texture2D, the sprite-atlas case.
// Constructed directly from a texture plus min/max bounds.
// SubTexture2D is the sprite-atlas case: a rect into a Texture2D. It holds
// a Ref to its parent, so a subtexture cannot outlive the texture it cuts
// from -- the ownership is what makes that guarantee hold.
py::class_<Wasteland::SubTexture2D, Wasteland::Ref<Wasteland::SubTexture2D> >(m, "SubTexture2D")
        // The constructor takes the parent, a position and a size, so atlas
        // slicing is a plain call rather than a separate factory.
        .def(py::init<const Wasteland::Ref<Wasteland::Texture2D> &, const glm::vec2 &,
                      const glm::vec2 &>(),
             py::arg("texture"), py::arg("min"), py::arg("max"))
        // CreateFromCoords is the sprite-sheet helper: give it a cell in a grid
        // rather than pixel bounds. spriteSize defaults to one cell.
        .def_static("CreateFromCoords",
             &Wasteland::SubTexture2D::CreateFromCoords,
             py::arg("texture"), py::arg("coords"),
             py::arg("cellSize"),
             py::arg("spriteSize") = glm::vec2(1, 1))
        .def("GetTexture", &Wasteland::SubTexture2D::GetTexture,
             py::return_value_policy::reference)
        .def("GetTexCoords", &Wasteland::SubTexture2D::GetTexCoords);

// =============================== RENDERER: Framebuffer =================
    // FramebufferTextureFormat. DEPTH24STENCIL8 and Depth are separate values --
    // so a caller must know whether it wants a depth-only or a packed
    // depth-stencil attachment, and picking wrong is not caught.
    // The framebuffer specification is a three-level graph of plain structs,
    // bound with no methods at all: format per attachment, a list of
    // attachments, and the overall framebuffer. A script assembles the graph by
    // assigning readwrite fields and hands it to Framebuffer.Create.
    py::enum_<Wasteland::FramebufferTextureFormat>(m, "FramebufferTextureFormat")
        .value("None",            Wasteland::FramebufferTextureFormat::None)
        .value("RGBA8",           Wasteland::FramebufferTextureFormat::RGBA8)
        .value("RED_INTEGER",     Wasteland::FramebufferTextureFormat::RED_INTEGER)
        .value("DEPTH24STENCIL8", Wasteland::FramebufferTextureFormat::DEPTH24STENCIL8)
        .value("Depth",           Wasteland::FramebufferTextureFormat::Depth);

// FramebufferTextureSpecification: just a format. The nesting continues
// down through attachment and framebuffer specs, mirroring the C++ structs.
py::class_<Wasteland::FramebufferTextureSpecification>(m, "FramebufferTextureSpecification")
        .def(py::init<>())
        .def(py::init<Wasteland::FramebufferTextureFormat>(), py::arg("format"))
        .def_readwrite("TextureFormat",
             &Wasteland::FramebufferTextureSpecification::TextureFormat);

// Attachments is a std::vector of the spec struct, and def_readwrite on a
// vector of a bound type copies on assignment -- `fb.Attachments = [...]`
// replaces the list rather than appending to it. Reading it copies too, so
// `fb.Attachments.append(x)` silently modifies a temporary.
py::class_<Wasteland::FramebufferAttachmentSpecification>(m, "FramebufferAttachmentSpecification")
        .def(py::init<>())
        .def(py::init<const std::vector<Wasteland::FramebufferTextureSpecification> &>(),
             py::arg("attachments"))
        .def_readwrite("Attachments",
             &Wasteland::FramebufferAttachmentSpecification::Attachments);

// Samples is the MSAA level and SwapChainTarget says whether this
// framebuffer is the window's. Both are readwrite with no validation in the
// binding -- a mismatched combination is caught by the renderer, not here.
py::class_<Wasteland::FramebufferSpecification>(m, "FramebufferSpecification")
        .def(py::init<>())
        .def_readwrite("Width", &Wasteland::FramebufferSpecification::Width)
        .def_readwrite("Height", &Wasteland::FramebufferSpecification::Height)
        .def_readwrite("Attachments", &Wasteland::FramebufferSpecification::Attachments)
        // Samples is the MSAA sample count; SwapChainTarget says whether this
        // framebuffer is the window's back buffer, which is the one case where
        // Resize must not be called directly.
        .def_readwrite("Samples", &Wasteland::FramebufferSpecification::Samples)
        .def_readwrite("SwapChainTarget",
             &Wasteland::FramebufferSpecification::SwapChainTarget);

// Framebuffer with a Ref<> holder. ReadPixel is the testing hook: it lets a
// headless script assert on rendered output, which is how the renderer is
// verified without a human looking at it.
// Framebuffer with a Ref<> holder. ReadPixel below is the reason a script
// can reach this class at all: it is how a headless tool captures output.
py::class_<Wasteland::Framebuffer, Wasteland::Ref<Wasteland::Framebuffer> >(m, "Framebuffer")
        .def_static("Create", &Wasteland::Framebuffer::Create, py::arg("spec"))
        .def("Bind", &Wasteland::Framebuffer::Bind)
        .def("Unbind", &Wasteland::Framebuffer::Unbind)
        .def("Resize", &Wasteland::Framebuffer::Resize,
             py::arg("width"), py::arg("height"))
        // ReadPixel writes into an out-parameter, which pybind surfaces as a
        // Python list the caller passes in and reads back afterwards -- not as a
        // return value. A script expecting a returned pixel gets None.
        .def("ReadPixel", &Wasteland::Framebuffer::ReadPixel,
             py::arg("attachmentIndex"), py::arg("x"), py::arg("y"))
        // ClearAttachment writes a raw value rather than a colour, because it serves
        // integer attachments too. Passing a float where an integer attachment
        // expects bits is a silent conversion.
        .def("ClearAttachment", &Wasteland::Framebuffer::ClearAttachment,
             py::arg("attachmentIndex"), py::arg("value"))
        .def("GetColorAttachmentRendererID",
             &Wasteland::Framebuffer::GetColorAttachmentRendererID,
             py::arg("index") = 0)
        // GetColorAttachmentNativeSRV is bound but has no Python-side type: it
        // returns a backend handle a script can only pass back to the engine.
        // Useful for a screenshot path that goes through C++.
        .def("GetColorAttachmentNativeSRV",
             &Wasteland::Framebuffer::GetColorAttachmentNativeSRV,
             py::arg("index") = 0)
        .def("GetSpecification", &Wasteland::Framebuffer::GetSpecification,
             py::return_value_policy::reference);

// RenderCommand is the thin global layer between a renderer and the backend.
// Every entry here is a free function on the module, and they all act on
// thread-local command state rather than on a RenderCommand object -- which
// is why there is no py::class_ for it at all.
// =============================== RENDERER: RenderCommand ===============
    // RenderCommand as root-level functions with a prefix. They are free
    // functions in C++ (a command buffer pattern), so there is no class to
    // attach them to -- the Renderer2D_/Renderer3D_ prefixes below follow the
    // same convention.
    m.def("RenderCommand_Init", &Wasteland::RenderCommand::Init);
    // SetViewport and SetClearColor both take 4-float vectors. The clear colour
    // is stored and applied by the NEXT Clear, so setting it does not itself
    // clear -- an easy ordering mistake from script.
    m.def("RenderCommand_SetViewport", &Wasteland::RenderCommand::SetViewport,
         py::arg("x"), py::arg("y"), py::arg("width"), py::arg("height"));
    m.def("RenderCommand_SetClearColor", &Wasteland::RenderCommand::SetClearColor,
         py::arg("color"));
    m.def("RenderCommand_Clear", &Wasteland::RenderCommand::Clear);
    // indexCount defaults to 0, meaning "use the whole index buffer". Zero is
    // therefore not an error here -- it is a sentinel, and a script that
    // genuinely wants to draw nothing cannot express it.
    // DrawIndexed/DrawLines/SetLineWidth: the primitive submission set. Line
    // width is a driver-level hint and is commonly ignored by the backend,
    // so a script setting it to 3 may see no change.
    m.def("RenderCommand_DrawIndexed", &Wasteland::RenderCommand::DrawIndexed,
         py::arg("vertexArray"), py::arg("indexCount") = 0);
    m.def("RenderCommand_DrawLines", &Wasteland::RenderCommand::DrawLines,
         py::arg("vertexArray"), py::arg("vertexCount"));
    m.def("RenderCommand_SetLineWidth", &Wasteland::RenderCommand::SetLineWidth,
         py::arg("width"));
    // SetActiveTextures takes a VECTOR of textures, so binding several at once
    // is one call rather than one call per slot. SetAPI below picks the backend
    // and is not a texture concern at all despite sitting here.
    m.def("RenderCommand_SetActiveTextures",
         &Wasteland::RenderCommand::SetActiveTextures, py::arg("textures"));
    // SetAPI/GetRendererAPI switch the backend at runtime. Nothing validates
    // the switch: changing it after resources were created on the old backend
    // leaves those resources invalid.
    m.def("RenderCommand_SetAPI", &Wasteland::RenderCommand::SetAPI, py::arg("api"));
    m.def("RenderCommand_GetRendererAPI", &Wasteland::RenderCommand::GetRendererAPI);

// =============================== RENDERER: RendererAPI ================
    // The Python name is "RendererAPIAPI" -- class name plus enum name. It reads
    // badly, but it is unambiguous, and this is the only place in the file where
    // an enum name repeats its namespace.
    // The enum is registered as "RendererAPIAPI" -- the class name plus the
    // nested enum name. The doubled word is not a typo; it is how the nested
    // scope is spelled, and the same pattern gives SceneCameraProjectionType
    // below.
    py::enum_<Wasteland::RendererAPI::API>(m, "RendererAPIAPI")
        .value("None",       Wasteland::RendererAPI::API::None)
        .value("OpenGL",     Wasteland::RendererAPI::API::OpenGL)
        .value("NVRHI_DX11", Wasteland::RendererAPI::API::NVRHI_DX11)
        .value("NVRHI_DX12", Wasteland::RendererAPI::API::NVRHI_DX12)
        .value("NVRHI_Vulkan", Wasteland::RendererAPI::API::NVRHI_Vulkan);

// The static accessor is the only way to ask which backend is live, since
// the class itself is not bound. One function standing in for a whole class
// is the general pattern for the engine's singletons here.
m.def("RendererAPI_GetAPI", &Wasteland::RendererAPI::GetAPI);

// =============================== RENDERER: Cameras =====================
    // Camera: a projection matrix and almost nothing else. The three derived
    // types below add position, rotation, and editor controls respectively.
    // Camera is the abstract base. Its only data is a projection matrix, and
    // GetProjection returns it BY REFERENCE -- the same reference-return hazard
    // noted for TransformComponent applies here: mutate it and you bypass the
    // derived class's own fields.
    py::class_<Wasteland::Camera>(m, "Camera")
        .def(py::init<>())
        .def(py::init<const glm::mat4 &>(), py::arg("projection"))
        // GetProjection returns a REFERENCE to the internal mat4. The caller can
        // read the elements but cannot usefully write one -- Mat4 has no __setitem__.
        .def("GetProjection", &Wasteland::Camera::GetProjection,
             py::return_value_policy::reference);

// OrthographicCamera with explicit bounds rather than a size+aspect. The
// screen-space camera used by UI and by the 2D renderer.
// OrthographicCamera is the concrete non-scene camera. Its constructor is
// (left, right, bottom, top) -- the projection VOLUME, not (aspect, znear,
// zfar), which is the reverse of most graphics APIs and the reverse of the
// glm convention. Getting the order wrong scales the scene instead of
// framing it.
py::class_<Wasteland::OrthographicCamera>(m, "OrthographicCamera")
        .def(py::init<float, float, float, float>(),
             py::arg("left"), py::arg("right"),
             py::arg("bottom"), py::arg("top"))
        // SetProjection recomputes and recomposes the matrix rather than storing
        // the four edges, so reading them back is not possible -- Set is one-way.
        .def("SetProjection", &Wasteland::OrthographicCamera::SetProjection,
             py::arg("left"), py::arg("right"),
             py::arg("bottom"), py::arg("top"))
        .def("GetPosition", &Wasteland::OrthographicCamera::GetPosition,
             py::return_value_policy::reference)
        .def("SetPosition", &Wasteland::OrthographicCamera::SetPosition,
             py::arg("position"))
        .def("GetRotation", &Wasteland::OrthographicCamera::GetRotation)
        .def("SetRotation", &Wasteland::OrthographicCamera::SetRotation,
             py::arg("rotation"))
        // GetProjectionMatrix / GetViewMatrix / GetViewProjectionMatrix all return
        // references to cached internal matrices. They are recomputed when Set* is
        // called, so a caller holding one across a Set* sees it change underneath.
        // GetProjectionMatrix/GetViewMatrix/GetViewProjectionMatrix all return
        // references to cached matrices. reference_internal ties each to the
        // camera's lifetime, so holding one across the camera's destruction leaves
        // a dangling handle.
        .def("GetProjectionMatrix",
             &Wasteland::OrthographicCamera::GetProjectionMatrix,
             py::return_value_policy::reference)
        .def("GetViewMatrix", &Wasteland::OrthographicCamera::GetViewMatrix,
             py::return_value_policy::reference)
        .def("GetViewProjectionMatrix",
             &Wasteland::OrthographicCamera::GetViewProjectionMatrix,
             py::return_value_policy::reference);

// SceneCamera derives from Camera. The projection parameters are split into
// PERSPECTIVE and ORTHOGRAPHIC accessors rather than shared ones -- a single
// SetOrthographicSize must not overwrite a perspective FOV, and separate
// storage is how that separation is enforced.
// SceneCamera derives from the Camera base and adds the perspective and
// orthographic parameter sets as separate get/set pairs -- see the note
// above about why they are not shared fields.
py::class_<Wasteland::SceneCamera, Wasteland::Camera>(m, "SceneCamera")
        .def(py::init<>())
        .def("SetOrthographic", &Wasteland::SceneCamera::SetOrthographic,
             py::arg("size"), py::arg("nearClip"), py::arg("farClip"))
        .def("SetPerspective", &Wasteland::SceneCamera::SetPerspective,
             py::arg("verticalFOV"), py::arg("nearClip"), py::arg("farClip"))
        // SetViewportSize is separate from SetPerspective/SetOrthographic because
        // the viewport and the projection are genuinely independent: an off-screen
        // camera renders at one aspect and displays at another.
        .def("SetViewportSize", &Wasteland::SceneCamera::SetViewportSize,
             py::arg("width"), py::arg("height"))
        // Twelve strictly-paired get/set functions: FOV, near and far clip for the
        // perspective projection, then size, near and far for the orthographic.
        // Which set is LIVE depends on ProjectionType -- so a script that sets the
        // orthographic size while the camera is in perspective mode gets a value
        // stored and never used, with no error.
        .def("GetPerspectiveVerticalFOV",
             &Wasteland::SceneCamera::GetPerspectiveVerticalFOV)
        // Note that SetPerspectiveVerticalFOV and SetPerspectiveNearClip take ONE
        // argument each, unlike SetPerspective/SetOrthographic at the top which
        // rebuild the whole projection from several at once. The single-value
        // setters must recompute the matrix from the stored parts; the multi-value
        // ones can just build it.
        .def("SetPerspectiveVerticalFOV",
             &Wasteland::SceneCamera::SetPerspectiveVerticalFOV,
             py::arg("verticalFOV"))
        .def("GetPerspectiveNearClip",
             &Wasteland::SceneCamera::GetPerspectiveNearClip)
        .def("SetPerspectiveNearClip",
             &Wasteland::SceneCamera::SetPerspectiveNearClip,
             py::arg("nearClip"))
        .def("GetPerspectiveFarClip",
             &Wasteland::SceneCamera::GetPerspectiveFarClip)
        .def("SetPerspectiveFarClip",
             &Wasteland::SceneCamera::SetPerspectiveFarClip,
             py::arg("farClip"))
        // GetOrthographicSize is a HALF-HEIGHT, matching the usual convention -- so
        // a script setting it to 10 gets a 20-unit tall view. Contrast with
        // OrthographicCamera's constructor above, which takes the projection VOLUME
        // edges rather than a size.
        .def("GetOrthographicSize",
             &Wasteland::SceneCamera::GetOrthographicSize)
        .def("SetOrthographicSize",
             &Wasteland::SceneCamera::SetOrthographicSize,
             py::arg("size"))
        .def("GetOrthographicNearClip",
             &Wasteland::SceneCamera::GetOrthographicNearClip)
        .def("SetOrthographicNearClip",
             &Wasteland::SceneCamera::SetOrthographicNearClip,
             py::arg("nearClip"))
        .def("GetOrthographicFarClip",
             &Wasteland::SceneCamera::GetOrthographicFarClip)
        .def("SetOrthographicFarClip",
             &Wasteland::SceneCamera::SetOrthographicFarClip,
             py::arg("farClip"))
        // GetProjectionType/SetProjectionType switch which of the two parameter
        // sets above is live. Switching does NOT recompute the matrix from the newly
        // selected set, so reading the view right after a switch can still reflect
        // the old projection until the next viewport resize or parameter write.
        .def("GetProjectionType", &Wasteland::SceneCamera::GetProjectionType)
        // SetProjectionType switches between the two parameter sets WITHOUT
        // recomputing the matrix from the newly-selected set. Switching type and
        // then reading the matrix can give a stale projection until the next
        // SetViewportSize or parameter write.
        .def("SetProjectionType", &Wasteland::SceneCamera::SetProjectionType,
             py::arg("type"));

// The projection-type enum, registered AFTER SceneCamera even though
// SetProjectionType below takes it. Legal, because pybind resolves argument
// types at call time -- but it means the binding text reads out of order.
// The projection-type enum. See the note at RendererAPIAPI above for the
// doubled-name spelling, which is the same nested-scope convention.
py::enum_<Wasteland::SceneCamera::ProjectionType>(m, "SceneCameraProjectionType")
        .value("Perspective",   Wasteland::SceneCamera::ProjectionType::Perspective)
        .value("Orthographic",  Wasteland::SceneCamera::ProjectionType::Orthographic);

// EditorCamera derives from Camera and adds pitch/yaw/distance orbit
// controls, plus the event and update hooks. Bound because a headless tool
// or a script-driven editor view needs to drive it.
// EditorCamera: a perspective camera with orbit controls. The four-float
// constructor is FOV/ASPECT/ZNEAR/ZFAR -- the OPPOSITE field order from
// OrthographicCamera above, in the same file. That inconsistency is real
// and is the single most likely thing to get wrong when binding a new
// camera class by copying a neighbour.
py::class_<Wasteland::EditorCamera, Wasteland::Camera>(m, "EditorCamera")
        .def(py::init<>())
        .def(py::init<float, float, float, float>(),
             py::arg("fov"), py::arg("aspectRatio"),
             py::arg("nearClip"), py::arg("farClip"))
        // OnUpdate and OnEvent make the editor camera scriptable end to end: a
        // script can feed it a Timestep and an event and read back the view matrix.
        // OnUpdate/OnEvent make the editor camera a full participant in the
        // engine's loop, which is what lets a script drive it instead of the
        // editor's own input handling.
        .def("OnUpdate", &Wasteland::EditorCamera::OnUpdate, py::arg("ts"))
        .def("OnEvent", &Wasteland::EditorCamera::OnEvent, py::arg("event"))
        .def("GetDistance", &Wasteland::EditorCamera::GetDistance)
        .def("SetDistance", &Wasteland::EditorCamera::SetDistance,
             py::arg("distance"))
        .def("SetViewportSize", &Wasteland::EditorCamera::SetViewportSize,
             py::arg("width"), py::arg("height"))
        // GetViewMatrix by reference, GetViewProjection by value. Inconsistent with
        // its neighbours -- GetViewProjectionMatrix on the orthographic camera below
        // returns a reference. Harmless, but the difference is not obvious.
        .def("GetViewMatrix", &Wasteland::EditorCamera::GetViewMatrix,
             py::return_value_policy::reference)
        .def("GetViewProjection", &Wasteland::EditorCamera::GetViewProjection)
        .def("GetUpDirection", &Wasteland::EditorCamera::GetUpDirection)
        .def("GetRightDirection", &Wasteland::EditorCamera::GetRightDirection)
        .def("GetForwardDirection", &Wasteland::EditorCamera::GetForwardDirection)
        .def("GetPosition", &Wasteland::EditorCamera::GetPosition,
             py::return_value_policy::reference)
        .def("GetOrientation", &Wasteland::EditorCamera::GetOrientation)
        .def("GetPitch", &Wasteland::EditorCamera::GetPitch)
        .def("GetYaw", &Wasteland::EditorCamera::GetYaw);

// =============================== RENDERER: Renderer2D ==================
    m.def("Renderer2D_Init", &Wasteland::Renderer2D::Init);
    m.def("Renderer2D_Shutdown", &Wasteland::Renderer2D::Shutdown);
    m.def("Renderer2D_RecreateResources",
         &Wasteland::Renderer2D::RecreateResources);

// BeginScene has TWO bindings because there are two overloads: an explicit
// (camera, transform) and an EditorCamera-only form. The second is a
// distinct Python NAME rather than another overload of one, because the
// EditorCamera overload takes a single argument and pybind's resolver cannot
// disambiguate the derived-class argument reliably.
m.def("Renderer2D_BeginScene",
         py::overload_cast<const Wasteland::Camera &, const glm::mat4 &>(
             &Wasteland::Renderer2D::BeginScene),
         py::arg("camera"), py::arg("transform"));
    m.def("Renderer2D_BeginScene_EditorCamera",
         py::overload_cast<const Wasteland::EditorCamera &>(&Wasteland::Renderer2D::BeginScene), py::arg("camera"));
    // EndScene and Flush are separate for the usual immediate-mode reason:
    // EndScene closes the frame, Flush actually submits. A script that calls
    // only EndScene leaves the work queued.
    m.def("Renderer2D_EndScene", &Wasteland::Renderer2D::EndScene);
    m.def("Renderer2D_Flush", &Wasteland::Renderer2D::Flush);

// DrawQuad overloads
    // DrawQuad overloads. SIX bindings under ONE Python name, which is why
    // py::overload_cast is mandatory here -- a plain &Renderer2D::DrawQuad
    // would be ambiguous and the module would fail to compile.
    //
    // Overloads 1-2 take no texture (2D and 3D position). Overloads 3-6 add one,
    // and all four DEFAULT tilingFactor and tintColor -- which is why a call
    // with only position/size/texture renders at full brightness. Overloads 5-6
    // take a SubTexture2D, distinguished by the argument NAME "subtexture"
    // rather than "texture", since both calls have the same arity.
    //
    // This is the FIRST of the six identical `m.def("Renderer2D_DrawQuad"`
    // lines, so it is the only one a comment can be pinned to: the inserter is
    // forward-only and a repeated anchor resolves to its first occurrence.
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("color"));
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("color"));
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &,
                           const Wasteland::Ref<Wasteland::Texture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("texture"),
         py::arg("tilingFactor") = 1.0f,
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &,
                           const Wasteland::Ref<Wasteland::Texture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("texture"),
         py::arg("tilingFactor") = 1.0f,
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &,
                           const Wasteland::Ref<Wasteland::SubTexture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("subtexture"),
         py::arg("tilingFactor") = 1.0f,
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &,
                           const Wasteland::Ref<Wasteland::SubTexture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("position"), py::arg("size"), py::arg("subtexture"),
         py::arg("tilingFactor") = 1.0f,
         py::arg("tintColor") = glm::vec4(1.0f));
    // DrawQuad_Transform is a DIFFERENT PYTHON NAME for the two mat4-taking
    // overloads. It is not named DrawQuad because a (mat4, ...) call and a
    // (vec2, ...) call are otherwise ambiguous to pybind's overload resolver
    // once both carry defaults.
    m.def("Renderer2D_DrawQuad_Transform",
         py::overload_cast<const glm::mat4 &, const glm::vec4 &, int>(
             &Wasteland::Renderer2D::DrawQuad),
         py::arg("transform"), py::arg("color"), py::arg("entityID") = -1);
    m.def("Renderer2D_DrawQuad_Transform",
         py::overload_cast<const glm::mat4 &,
                           const Wasteland::Ref<Wasteland::Texture2D> &, float,
                           const glm::vec4 &, int>(&Wasteland::Renderer2D::DrawQuad),
         py::arg("transform"), py::arg("texture"),
         py::arg("tilingFactor") = 1.0f,
         py::arg("tintColor") = glm::vec4(1.0f),
         py::arg("entityID") = -1);

// DrawRotatedQuad overloads
    // DrawRotatedQuad mirrors DrawQuad's six-overload shape but adds a rotation
    // angle before the texture arguments. Note the argument ORDER differs
    // between the untextured and textured forms: rotation comes BEFORE texture
    // in one and AFTER size in the other. A script writing both call sites has
    // to remember which is which.
    //
    // The textured forms also differ on tilingFactor: DrawQuad defaults it to 1.0
    // while DrawRotatedQuad requires it explicitly (tintColor is defaulted in both).
    // So a call that works for an unrotated quad raises a TypeError here.
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"),
         py::arg("rotation"), py::arg("color"));
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"),
         py::arg("rotation"), py::arg("color"));
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &, float,
                           const Wasteland::Ref<Wasteland::Texture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"), py::arg("rotation"),
         py::arg("texture"), py::arg("tilingFactor"),
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &, float,
                           const Wasteland::Ref<Wasteland::Texture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"), py::arg("rotation"),
         py::arg("texture"), py::arg("tilingFactor"),
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec2 &, const glm::vec2 &, float,
                           const Wasteland::Ref<Wasteland::SubTexture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"), py::arg("rotation"),
         py::arg("subtexture"), py::arg("tilingFactor"),
         py::arg("tintColor") = glm::vec4(1.0f));
    m.def("Renderer2D_DrawRotatedQuad",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &, float,
                           const Wasteland::Ref<Wasteland::SubTexture2D> &, float,
                           const glm::vec4 &>(&Wasteland::Renderer2D::DrawRotatedQuad),
         py::arg("position"), py::arg("size"), py::arg("rotation"),
         py::arg("subtexture"), py::arg("tilingFactor"),
         py::arg("tintColor") = glm::vec4(1.0f));

// DrawCircle's `fade` is a smoothing tolerance, not an alpha value. A larger
// fade flattens the curve's shading; a smaller one keeps the gradient but
// can band.
m.def("Renderer2D_DrawCircle", &Wasteland::Renderer2D::DrawCircle,
         py::arg("transform"), py::arg("color"),
         py::arg("thickness") = 1.0f, py::arg("fade") = 0.005f,
         py::arg("entityID") = -1);
    // DrawLine takes two vec3 endpoints and an entityID defaulting to -1.
    // There is no 2D/3D split here: the C++ overload set is small enough that
    // one call covers both, unlike DrawQuad.
    m.def("Renderer2D_DrawLine", &Wasteland::Renderer2D::DrawLine,
         py::arg("p0"), py::arg("p1"), py::arg("color"),
         py::arg("entityID") = -1);
    // DrawRect's two overloads use DIFFERENT argument names -- (position, size,
    // color) for one and (transform, color) for the other. The same Python
    // function name accepts both, so the resolution happens on argument types.
    m.def("Renderer2D_DrawRect",
         py::overload_cast<const glm::vec3 &, const glm::vec2 &, const glm::vec4 &,
                           int>(&Wasteland::Renderer2D::DrawRect),
         py::arg("position"), py::arg("size"), py::arg("color"),
         py::arg("entityID") = -1);
    m.def("Renderer2D_DrawRect",
         py::overload_cast<const glm::mat4 &, const glm::vec4 &, int>(
             &Wasteland::Renderer2D::DrawRect),
         py::arg("transform"), py::arg("color"), py::arg("entityID") = -1);
    m.def("Renderer2D_DrawSprite", &Wasteland::Renderer2D::DrawSprite,
         py::arg("transform"), py::arg("src"), py::arg("entityID"));
    // Line width is a get/set pair here but is also a parameter on some draw
    // calls. Setting it globally and then passing a per-call width are two
    // different mechanisms, and nothing documents which wins.
    m.def("Renderer2D_GetLineWidth", &Wasteland::Renderer2D::GetLineWidth);
    m.def("Renderer2D_SetLineWidth", &Wasteland::Renderer2D::SetLineWidth,
         py::arg("width"));
    m.def("Renderer2D_ResetStats", &Wasteland::Renderer2D::ResetStats);

// Renderer2DStatistics is a class with NO constructor and no holder.
// Scripts never construct one -- they only ever receive it from GetStats,
// which returns by value. So the absence of py::init is correct, not an
// oversight.
// Renderer2DStatistics is a class with NO constructor and no holder.
// Scripts never construct one -- they only ever receive it from GetStats,
// which returns by value. So the absence of py::init is correct, not an
// oversight.
py::class_<Wasteland::Renderer2D::Statistics>(m, "Renderer2DStatistics")
        // DEFECT-ish: DrawCalls and QuadCount are named identically on both
        // Renderer2DStatistics and Renderer3DStatistics, which is fine because they
        // are different classes -- but the field names are generic enough that a
        // script switching between the two could silently read the wrong one.
        // DrawCalls and QuadCount carry the same names here and on
        // Renderer3DStatistics. Fine -- different classes -- but generic enough that
        // a script moving between the two could read the wrong one by habit.
        .def_readonly("DrawCalls",
             &Wasteland::Renderer2D::Statistics::DrawCalls)
        .def_readonly("QuadCount",
             &Wasteland::Renderer2D::Statistics::QuadCount)
        .def("GetTotalVertexCount",
             &Wasteland::Renderer2D::Statistics::GetTotalVertexCount)
        .def("GetTotalIndexCount",
             &Wasteland::Renderer2D::Statistics::GetTotalIndexCount);

// GetStats returns the struct BY VALUE (verified: `static Statistics
// GetStats()` in Renderer2D.h). So the Python object is a snapshot; holding
// it across frames gives stale numbers rather than a live view.
// GetStats returns the struct BY VALUE (verified: `static Statistics
// GetStats()` in Renderer2D.h). So the Python object is a snapshot; holding
// it across frames gives stale numbers rather than a live view.
m.def("Renderer2D_GetStats", &Wasteland::Renderer2D::GetStats);

// =============================== RENDERER: Renderer3D ==================
    // QualityPreset lives at the root module and is used by Renderer3D below.
    // Bound once for both renderers even though only 3D consumes it.
    // QualityPreset lives at the root module and is used by Renderer3D below.
    // Bound once for both renderers even though only 3D consumes it.
    py::enum_<Wasteland::QualityPreset>(m, "QualityPreset")
        .value("Low",    Wasteland::QualityPreset::Low)
        .value("Medium", Wasteland::QualityPreset::Medium)
        .value("High",   Wasteland::QualityPreset::High)
        .value("Ultra",  Wasteland::QualityPreset::Ultra);

// Renderer3D lifecycle, mirroring Renderer2D above.
// Renderer3D lifecycle, mirroring Renderer2D above.
m.def("Renderer3D_Init", &Wasteland::Renderer3D::Init);
    m.def("Renderer3D_Shutdown", &Wasteland::Renderer3D::Shutdown);
    m.def("Renderer3D_RecreateResources",
         &Wasteland::Renderer3D::RecreateResources);

// BeginScene again takes (camera, transform), with the EditorCamera variant
// split into its own name for the same ambiguity reason as Renderer2D.
// BeginScene again takes (camera, transform), with the EditorCamera variant
// split into its own name for the same ambiguity reason as Renderer2D.
m.def("Renderer3D_BeginScene_Camera",
         py::overload_cast<const Wasteland::Camera &, const glm::mat4 &>(&Wasteland::Renderer3D::BeginScene),
         py::arg("camera"), py::arg("transform"));
    m.def("Renderer3D_BeginScene_EditorCamera",
         py::overload_cast<const Wasteland::EditorCamera &>(&Wasteland::Renderer3D::BeginScene), py::arg("camera"));
    m.def("Renderer3D_EndScene", &Wasteland::Renderer3D::EndScene);
    m.def("Renderer3D_Flush", &Wasteland::Renderer3D::Flush);

// DrawCube takes a material, unlike the 2D draws which take only a colour.
// Passing a None material is not checked here.
// DrawCube takes a material, unlike the 2D draws which take only a colour.
// Passing a None material is not checked here.
m.def("Renderer3D_DrawCube", &Wasteland::Renderer3D::DrawCube,
         py::arg("transform"), py::arg("color"), py::arg("material"),
         py::arg("entityID") = -1);
    // DrawSphere exposes sectors and stacks explicitly -- the tessellation is a
    // caller decision, so a script can trade smoothness for vertex count.
    // DrawSphere exposes sectors and stacks explicitly -- the tessellation is a
    // caller decision, so a script can trade smoothness for vertex count.
    m.def("Renderer3D_DrawSphere", &Wasteland::Renderer3D::DrawSphere,
         py::arg("transform"), py::arg("color"), py::arg("radius"),
         py::arg("sectors"), py::arg("stacks"), py::arg("material"),
         py::arg("entityID") = -1);
    // Submit is the low-level path: shader + vertex array + transform, bypassing
    // the convenience draws above. This is how a script renders a mesh it built
    // itself.
    // Submit is the low-level path: shader + vertex array + transform, bypassing
    // the convenience draws above. This is how a script renders a mesh it built
    // itself.
    m.def("Renderer3D_Submit", &Wasteland::Renderer3D::Submit,
         py::arg("shader"), py::arg("vertexArray"),
         py::arg("transform") = glm::mat4(1.0f));

py::class_<Wasteland::Renderer3D::Statistics>(m, "Renderer3DStatistics")
        .def_readonly("DrawCalls",
             &Wasteland::Renderer3D::Statistics::DrawCalls)
        .def_readonly("QuadCount",
             &Wasteland::Renderer3D::Statistics::QuadCount)
        .def("GetTotalVertexCount",
             &Wasteland::Renderer3D::Statistics::GetTotalVertexCount)
        .def("GetTotalIndexCount",
             &Wasteland::Renderer3D::Statistics::GetTotalIndexCount);

// Renderer3D stats, same shape as the 2D ones. ResetStats and GetStats are
// separate functions rather than methods, because Statistics is a plain
// struct in C++ with no such methods.
m.def("Renderer3D_ResetStats", &Wasteland::Renderer3D::ResetStats);
    m.def("Renderer3D_GetStats", &Wasteland::Renderer3D::GetStats);

// The ray-tracing block. Every setting is a Get/Set pair on the module,
// which means a script can flip ray tracing at runtime -- and that doing so
// mid-frame is the caller's problem, since there is no batching here.
// The ray-tracing block. Every setting is a Get/Set pair on the module,
// which means a script can flip ray tracing at runtime -- and that doing so
// mid-frame is the caller's problem, since there is no batching here.
// Ray tracing
    m.def("Renderer3D_IsRayTracingEnabled",
         &Wasteland::Renderer3D::IsRayTracingEnabled);
    m.def("Renderer3D_SetRayTracingEnabled",
         &Wasteland::Renderer3D::SetRayTracingEnabled, py::arg("enabled"));
    // IsRayTracingAccumulate is the temporal-accumulation toggle. Accumulation
    // trades motion blur/ghosting for sample cost; the pair of flags interact,
    // so setting one without the other can be a no-op.
    // IsRayTracingAccumulate is the temporal-accumulation toggle. Accumulation
    // trades motion blur/ghosting for sample cost; the pair of flags interact,
    // so setting one without the other can be a no-op.
    m.def("Renderer3D_IsRayTracingAccumulate",
         &Wasteland::Renderer3D::IsRayTracingAccumulate);
    m.def("Renderer3D_SetRayTracingAccumulate",
         &Wasteland::Renderer3D::SetRayTracingAccumulate, py::arg("enabled"));
    m.def("Renderer3D_GetQualityPreset", &Wasteland::Renderer3D::GetQualityPreset);
    m.def("Renderer3D_SetQualityPreset",
         &Wasteland::Renderer3D::SetQualityPreset, py::arg("preset"));
    m.def("Renderer3D_GetRenderScale", &Wasteland::Renderer3D::GetRenderScale);
    m.def("Renderer3D_SetRenderScale",
         &Wasteland::Renderer3D::SetRenderScale, py::arg("scale"));

// Dynamic resolution. The engine can drop the internal render scale to hold
// a frame-time target, which is why GetRenderScale is a separate query from
// whatever the user asked for.
// Dynamic resolution. The engine can drop the internal render scale to hold
// a frame-time target, which is why GetRenderScale is a separate query from
// whatever the user asked for.
// Dynamic resolution
    m.def("Renderer3D_IsDynamicResolutionEnabled",
         &Wasteland::Renderer3D::IsDynamicResolutionEnabled);
    m.def("Renderer3D_SetDynamicResolutionEnabled",
         &Wasteland::Renderer3D::SetDynamicResolutionEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetTargetFPS", &Wasteland::Renderer3D::GetTargetFPS);
    m.def("Renderer3D_SetTargetFPS", &Wasteland::Renderer3D::SetTargetFPS,
         py::arg("fps"));
    m.def("Renderer3D_GetDynamicMinScale",
         &Wasteland::Renderer3D::GetDynamicMinScale);
    m.def("Renderer3D_SetDynamicMinScale",
         &Wasteland::Renderer3D::SetDynamicMinScale, py::arg("scale"));
    m.def("Renderer3D_GetCurrentFPS", &Wasteland::Renderer3D::GetCurrentFPS);
    m.def("Renderer3D_GetGPUFrameTimeMs", &Wasteland::Renderer3D::GetGPUFrameTimeMs);
    m.def("Renderer3D_IsDynamicResolutionActive",
         &Wasteland::Renderer3D::IsDynamicResolutionActive);
    m.def("Renderer3D_GetGPUTierName", &Wasteland::Renderer3D::GetGPUTierName);
    // NotifyFrameTime lets an EXTERNAL loop feed the dynamic-resolution
    // controller. That is the hook that makes this testable: a headless script
    // can drive the controller with synthetic frame times instead of waiting for
    // real ones.
    m.def("Renderer3D_NotifyFrameTime",
         &Wasteland::Renderer3D::NotifyFrameTime, py::arg("ms"));

// Ray tracing settings
    // Every post-processing effect below follows one shape: an IsXEnabled pair,
    // then a Get/Set pair per parameter, all as free functions on the module
    // rather than methods on Renderer3D. The renderer is a singleton the engine
    // owns, so free functions are what a script can actually reach.
    m.def("Renderer3D_GetSamplesPerPixel",
         &Wasteland::Renderer3D::GetSamplesPerPixel);
    m.def("Renderer3D_SetSamplesPerPixel",
         &Wasteland::Renderer3D::SetSamplesPerPixel, py::arg("samples"));
    // MovementThreshold is the ray-tracing re-use distance: how far the camera
    // may move before the traced result is considered stale. Raising it buys
    // frame time at the cost of visible lag between view and reflections.
    m.def("Renderer3D_GetMovementThreshold",
         &Wasteland::Renderer3D::GetMovementThreshold);
    m.def("Renderer3D_SetMovementThreshold",
         &Wasteland::Renderer3D::SetMovementThreshold, py::arg("threshold"));

// Ray trace target
    m.def("Renderer3D_GetRayTraceTargetID",
         &Wasteland::Renderer3D::GetRayTraceTargetID);
    // GetRayTraceTargetNativeSRV hands Python a native handle with no caster of
    // its own. There is no way to use it without knowing the engine's graphics
    // API -- which makes this bound for the same C++ code, reached from a
    // script, rather than for a script.
    m.def("Renderer3D_GetRayTraceTargetNativeSRV",
         &Wasteland::Renderer3D::GetRayTraceTargetNativeSRV);
    m.def("Renderer3D_ResizeRayTraceTarget",
         &Wasteland::Renderer3D::ResizeRayTraceTarget,
         py::arg("width"), py::arg("height"));

// Sky
    // The sky is a two-colour gradient: bottom and top. Both are full get/set
    // pairs because a sunset needs to move both ends, not just one.
    m.def("Renderer3D_GetSkyBottomColor",
         &Wasteland::Renderer3D::GetSkyBottomColor);
    m.def("Renderer3D_SetSkyBottomColor",
         &Wasteland::Renderer3D::SetSkyBottomColor, py::arg("color"));
    m.def("Renderer3D_GetSkyTopColor",
         &Wasteland::Renderer3D::GetSkyTopColor);
    m.def("Renderer3D_SetSkyTopColor",
         &Wasteland::Renderer3D::SetSkyTopColor, py::arg("color"));

// Volumetric
    // Sun and ambient are SEPARATE from the point/spot/area lights above. A
    // directional sun has no position -- only a direction and a colour -- so it
    // cannot share the light submission path.
    // SubmitFogVolume and SubmitCloudVolume take a transform AND the component.
    // Passing the component separately from the transform is what lets one fog
    // body be submitted at several positions -- the scene does not have to own
    // a distinct entity per instance.
    m.def("Renderer3D_SubmitFogVolume",
         &Wasteland::Renderer3D::SubmitFogVolume,
         py::arg("transform"), py::arg("fog"));
    m.def("Renderer3D_SubmitCloudVolume",
         &Wasteland::Renderer3D::SubmitCloudVolume,
         py::arg("transform"), py::arg("clouds"));
    // Two separate Is/SetEnabled pairs, one per volumetric. They are independent
    // flags, so a script can have fog raymarched with clouds off, or both off
    // while still submitting volumes -- in which case the submit calls do real
    // work that is then not rendered.
    m.def("Renderer3D_IsVolumetricFogEnabled",
         &Wasteland::Renderer3D::IsVolumetricFogEnabled);
    m.def("Renderer3D_SetVolumetricFogEnabled",
         &Wasteland::Renderer3D::SetVolumetricFogEnabled, py::arg("enabled"));
    m.def("Renderer3D_IsVolumetricCloudsEnabled",
         &Wasteland::Renderer3D::IsVolumetricCloudsEnabled);
    m.def("Renderer3D_SetVolumetricCloudsEnabled",
         &Wasteland::Renderer3D::SetVolumetricCloudsEnabled, py::arg("enabled"));
    // SunDirection is here with the volumetrics rather than with the sky colour
    // above, because the sun direction is what the volumetric shaders need in
    // order to scatter. The binding order follows shader dependencies, not the
    // authoring panel.
    m.def("Renderer3D_GetSunDirection",
         &Wasteland::Renderer3D::GetSunDirection);
    m.def("Renderer3D_SetSunDirection",
         &Wasteland::Renderer3D::SetSunDirection, py::arg("direction"));
    // VolumetricStepScale is the single global quality knob for BOTH raymarched
    // effects. One number trades fog and cloud quality together, so a script
    // cannot improve one at the other's expense.
    m.def("Renderer3D_GetVolumetricStepScale",
         &Wasteland::Renderer3D::GetVolumetricStepScale);
    m.def("Renderer3D_SetVolumetricStepScale",
         &Wasteland::Renderer3D::SetVolumetricStepScale, py::arg("scale"));

// Global fog
    m.def("Renderer3D_IsGlobalFogEnabled",
         &Wasteland::Renderer3D::IsGlobalFogEnabled);
    m.def("Renderer3D_SetGlobalFogEnabled",
         &Wasteland::Renderer3D::SetGlobalFogEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetGlobalFogDensity",
         &Wasteland::Renderer3D::GetGlobalFogDensity);
    m.def("Renderer3D_SetGlobalFogDensity",
         &Wasteland::Renderer3D::SetGlobalFogDensity, py::arg("density"));
    m.def("Renderer3D_GetGlobalFogHeightFalloff",
         &Wasteland::Renderer3D::GetGlobalFogHeightFalloff);
    // GlobalFogHeightFalloff and the colour/base-height/distance-attenuation
    // group below it are the four fog controls as one set: how fast density
    // falls off with altitude, what colour the fog is, at what altitude it
    // starts, and how quickly it thins with distance.
    m.def("Renderer3D_SetGlobalFogHeightFalloff",
         &Wasteland::Renderer3D::SetGlobalFogHeightFalloff, py::arg("falloff"));
    m.def("Renderer3D_GetGlobalFogColor",
         &Wasteland::Renderer3D::GetGlobalFogColor);
    m.def("Renderer3D_SetGlobalFogColor",
         &Wasteland::Renderer3D::SetGlobalFogColor, py::arg("color"));
    m.def("Renderer3D_GetGlobalFogBaseHeight",
         &Wasteland::Renderer3D::GetGlobalFogBaseHeight);
    m.def("Renderer3D_SetGlobalFogBaseHeight",
         &Wasteland::Renderer3D::SetGlobalFogBaseHeight, py::arg("height"));
    m.def("Renderer3D_GetGlobalFogDistanceAttenuation",
         &Wasteland::Renderer3D::GetGlobalFogDistanceAttenuation);
    m.def("Renderer3D_SetGlobalFogDistanceAttenuation",
         &Wasteland::Renderer3D::SetGlobalFogDistanceAttenuation,
         py::arg("atten"));

// God rays
    // GOD RAYS, group 1 of 5. Intensity/Decay/Samples/Density/Color is the full
    // parameter set; Samples is the count of occlusion samples along the ray,
    // so it is the direct cost knob for the effect.
    m.def("Renderer3D_IsGodRaysEnabled",
         &Wasteland::Renderer3D::IsGodRaysEnabled);
    m.def("Renderer3D_SetGodRaysEnabled",
         &Wasteland::Renderer3D::SetGodRaysEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetGodRayIntensity",
         &Wasteland::Renderer3D::GetGodRayIntensity);
    m.def("Renderer3D_SetGodRayIntensity",
         &Wasteland::Renderer3D::SetGodRayIntensity, py::arg("intensity"));
    // Decay is exponent-shaped falloff, and Samples is quality. Both scale the
    // effect's cost, and both can be raised to the point where the pass is the
    // frame's bottleneck.
    m.def("Renderer3D_GetGodRayDecay",
         &Wasteland::Renderer3D::GetGodRayDecay);
    m.def("Renderer3D_SetGodRayDecay",
         &Wasteland::Renderer3D::SetGodRayDecay, py::arg("decay"));
    m.def("Renderer3D_GetGodRaySamples",
         &Wasteland::Renderer3D::GetGodRaySamples);
    m.def("Renderer3D_SetGodRaySamples",
         &Wasteland::Renderer3D::SetGodRaySamples, py::arg("samples"));
    m.def("Renderer3D_GetGodRayDensity",
         &Wasteland::Renderer3D::GetGodRayDensity);
    m.def("Renderer3D_SetGodRayDensity",
         &Wasteland::Renderer3D::SetGodRayDensity, py::arg("density"));
    m.def("Renderer3D_GetGodRayColor",
         &Wasteland::Renderer3D::GetGodRayColor);
    m.def("Renderer3D_SetGodRayColor",
         &Wasteland::Renderer3D::SetGodRayColor, py::arg("color"));

// Bloom
    // BLOOM, group 1 of 3. Intensity scales the whole effect; Threshold is the
    // luminance above which a pixel is allowed to bloom at all. Raising the
    // threshold is the cheap way to reduce bloom without changing its look.
    m.def("Renderer3D_IsBloomEnabled", &Wasteland::Renderer3D::IsBloomEnabled);
    m.def("Renderer3D_SetBloomEnabled",
         &Wasteland::Renderer3D::SetBloomEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetBloomIntensity",
         &Wasteland::Renderer3D::GetBloomIntensity);
    m.def("Renderer3D_SetBloomIntensity",
         &Wasteland::Renderer3D::SetBloomIntensity, py::arg("intensity"));
    m.def("Renderer3D_GetBloomThreshold",
         &Wasteland::Renderer3D::GetBloomThreshold);
    m.def("Renderer3D_SetBloomThreshold",
         &Wasteland::Renderer3D::SetBloomThreshold, py::arg("threshold"));

// Bilateral blur
    // Bilateral blur is the edge-aware filter bloom uses so a bright object's
    // glow does not bleed across a hard silhouette. Passes is the iteration
    // count -- more passes widen the glow without adding energy.
    m.def("Renderer3D_IsBilateralBlurEnabled",
         &Wasteland::Renderer3D::IsBilateralBlurEnabled);
    m.def("Renderer3D_SetBilateralBlurEnabled",
         &Wasteland::Renderer3D::SetBilateralBlurEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetBilateralBlurPasses",
         &Wasteland::Renderer3D::GetBilateralBlurPasses);
    m.def("Renderer3D_SetBilateralBlurPasses",
         &Wasteland::Renderer3D::SetBilateralBlurPasses, py::arg("passes"));

// Vignette
    // VIGNETTE, group 1 of 4. Intensity/Radius/Softness: radius is where the
    // darkening starts, softness is how gradually it reaches full strength. A
    // vignette with softness 0 is a hard mask.
    m.def("Renderer3D_IsVignetteEnabled",
         &Wasteland::Renderer3D::IsVignetteEnabled);
    m.def("Renderer3D_SetVignetteEnabled",
         &Wasteland::Renderer3D::SetVignetteEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetVignetteIntensity",
         &Wasteland::Renderer3D::GetVignetteIntensity);
    m.def("Renderer3D_SetVignetteIntensity",
         &Wasteland::Renderer3D::SetVignetteIntensity, py::arg("intensity"));
    m.def("Renderer3D_GetVignetteRadius",
         &Wasteland::Renderer3D::GetVignetteRadius);
    m.def("Renderer3D_SetVignetteRadius",
         &Wasteland::Renderer3D::SetVignetteRadius, py::arg("radius"));
    m.def("Renderer3D_GetVignetteSoftness",
         &Wasteland::Renderer3D::GetVignetteSoftness);
    m.def("Renderer3D_SetVignetteSoftness",
         &Wasteland::Renderer3D::SetVignetteSoftness, py::arg("softness"));

// Chromatic Aberration
    // CHROMATIC ABERRATION: one parameter. It splits the RGB channels by an
    // amount that grows with distance from the centre, which is the lens effect
    // being simulated.
    m.def("Renderer3D_IsChromaticAberrationEnabled",
         &Wasteland::Renderer3D::IsChromaticAberrationEnabled);
    m.def("Renderer3D_SetChromaticAberrationEnabled",
         &Wasteland::Renderer3D::SetChromaticAberrationEnabled,
         py::arg("enabled"));
    m.def("Renderer3D_GetChromaticAberrationIntensity",
         &Wasteland::Renderer3D::GetChromaticAberrationIntensity);
    m.def("Renderer3D_SetChromaticAberrationIntensity",
         &Wasteland::Renderer3D::SetChromaticAberrationIntensity,
         py::arg("intensity"));

// Color Grading
    // COLOR GRADING, group 1 of 3. Temperature and Tint push the image along a
    // warm/cool and green/magenta axis respectively; Gamma then sets the
    // midpoint. The three are not independent in the shader, so setting only
    // gamma produces a result no colourist would choose.
    m.def("Renderer3D_IsColorGradingEnabled",
         &Wasteland::Renderer3D::IsColorGradingEnabled);
    m.def("Renderer3D_SetColorGradingEnabled",
         &Wasteland::Renderer3D::SetColorGradingEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetColorTemperature",
         &Wasteland::Renderer3D::GetColorTemperature);
    m.def("Renderer3D_SetColorTemperature",
         &Wasteland::Renderer3D::SetColorTemperature,
         py::arg("temperature"));
    m.def("Renderer3D_GetColorTint",
         &Wasteland::Renderer3D::GetColorTint);
    m.def("Renderer3D_SetColorTint",
         &Wasteland::Renderer3D::SetColorTint, py::arg("tint"));
    m.def("Renderer3D_GetColorGamma",
         &Wasteland::Renderer3D::GetColorGamma);
    m.def("Renderer3D_SetColorGamma",
         &Wasteland::Renderer3D::SetColorGamma, py::arg("gamma"));

// Film Grain
    // Film grain is a per-pixel noise overlay whose intensity is the only knob.
    // Its scale and shape are shader constants, not parameters -- a limit worth
    // knowing before writing a grain settings UI in script.
    // Film grain: intensity is the ONLY parameter. There is no size, seed or
    // grain-shape control exposed, so a script can dial the amount of grain to
    // zero or up but cannot change what it looks like. That is a binding-surface
    // limitation, not an engine one.
    m.def("Renderer3D_IsFilmGrainEnabled",
         &Wasteland::Renderer3D::IsFilmGrainEnabled);
    m.def("Renderer3D_SetFilmGrainEnabled",
         &Wasteland::Renderer3D::SetFilmGrainEnabled, py::arg("enabled"));
    m.def("Renderer3D_GetFilmGrainIntensity",
         &Wasteland::Renderer3D::GetFilmGrainIntensity);
    m.def("Renderer3D_SetFilmGrainIntensity",
         &Wasteland::Renderer3D::SetFilmGrainIntensity,
         py::arg("intensity"));

// Neural Rendering
    // NEURAL RENDERING, group 1 of 3, and the most experimental block here.
    // Texture/Light/Material strength are three separate weights on the three
    // passes; each is independently scaled, so a material-only effect is a
    // matter of setting two of them to zero.
    // Neural rendering, the third pass-weight group. Like film grain, the
    // enabling flag and the three strengths are the whole exposed surface --
    // there is no way to select which model or feed it data from script.
    // The three neural strength accessors. SetNeuralRenderingEnabled takes the
    // flag and each SetNeural*Strength defaults none -- all three are required
    // arguments, unlike the other effect groups where parameters have defaults.
    m.def("Renderer3D_IsNeuralRenderingEnabled",
         &Wasteland::Renderer3D::IsNeuralRenderingEnabled);
    m.def("Renderer3D_SetNeuralRenderingEnabled",
         &Wasteland::Renderer3D::SetNeuralRenderingEnabled,
         py::arg("enabled"));
    // Texture/Light/Material strength are three INDEPENDENT weights on three
    // passes. Setting one to 1 does not imply the others, so enabling neural
    // rendering without touching any strength leaves it nominally on and
    // visually inert.
    m.def("Renderer3D_GetNeuralTextureStrength",
         &Wasteland::Renderer3D::GetNeuralTextureStrength);
    m.def("Renderer3D_SetNeuralTextureStrength",
         &Wasteland::Renderer3D::SetNeuralTextureStrength,
         py::arg("strength"));
    m.def("Renderer3D_GetNeuralLightStrength",
         &Wasteland::Renderer3D::GetNeuralLightStrength);
    m.def("Renderer3D_SetNeuralLightStrength",
         &Wasteland::Renderer3D::SetNeuralLightStrength,
         py::arg("strength"));
    m.def("Renderer3D_GetNeuralMaterialStrength",
         &Wasteland::Renderer3D::GetNeuralMaterialStrength);
    m.def("Renderer3D_SetNeuralMaterialStrength",
         &Wasteland::Renderer3D::SetNeuralMaterialStrength,
         py::arg("strength"));

// Halton / Jitter
    // Halton is the low-discrepancy sequence used for TAA jitter. Exposed so a
    // script driving synthetic frames can reproduce the engine's sampling
    // rather than inventing its own pattern -- which is exactly the aliasing
    // TAA exists to avoid.
    m.def("Renderer3D_Halton", &Wasteland::Renderer3D::Halton,
         py::arg("index"), py::arg("base"));
    m.def("Renderer3D_GetCurrentJitter",
         &Wasteland::Renderer3D::GetCurrentJitter,
         py::arg("frameIndex"), py::arg("viewportSize"));

// Material map generation
    // GenerateMaterialMaps is the offline bake, callable from script. It writes
    // to disk, so it is one of the few bindings here with a side effect a script
    // cannot undo. Nothing in this layer checks the return value.
    m.def("Renderer3D_GenerateMaterialMaps",
         &Wasteland::Renderer3D::GenerateMaterialMaps,
         py::arg("texturePath"), py::arg("normalStrength"),
         py::arg("roughBias"));

// Analytic lights
    m.def("Renderer3D_SubmitDirectionalLight",
         &Wasteland::Renderer3D::SubmitDirectionalLight,
         py::arg("transform"), py::arg("light"));
    m.def("Renderer3D_SubmitPointLight",
         &Wasteland::Renderer3D::SubmitPointLight,
         py::arg("transform"), py::arg("light"));
    m.def("Renderer3D_SubmitSpotLight",
         &Wasteland::Renderer3D::SubmitSpotLight,
         py::arg("transform"), py::arg("light"));
    m.def("Renderer3D_SubmitAreaLight",
         &Wasteland::Renderer3D::SubmitAreaLight,
         py::arg("transform"), py::arg("light"));

// Water
    // DrawWaterPlane takes entityID defaulting to -1, which is the sentinel for
    // "not a scene entity". The default keeps the editor-preview call short, but
    // it also means a script that forgets the argument silently renders an
    // unbound plane.
    m.def("Renderer3D_DrawWaterPlane",
         &Wasteland::Renderer3D::DrawWaterPlane,
         py::arg("transform"), py::arg("water"), py::arg("entityID") = -1);

// ======================================== SCENE =========================
    // --- IDComponent ---
    // The component classes themselves. Each is a plain struct registered with
    // def_readwrite for every field, so a script can read and write component
    // state -- but only AFTER it has obtained the component, which needs one of
    // the Entity accessors bound at the bottom of this file.
    py::class_<Wasteland::IDComponent>(m, "IDComponent")
        .def(py::init<>())
        .def_readwrite("ID", &Wasteland::IDComponent::ID);

// TagComponent with two constructors: empty, and one taking the tag string.
// The string overload is the one that saves three lines of script.
// --- TagComponent ---
    py::class_<Wasteland::TagComponent>(m, "TagComponent")
        .def(py::init<>())
        .def(py::init<const std::string &>(), py::arg("tag"))
        .def_readwrite("Tag", &Wasteland::TagComponent::Tag);

// --- TransformComponent ---
    // CRITICAL: the vec3 property getters must return a REFERENCE to the member.
    // Returning by value makes `t.Translation.x += 1` mutate a temporary copy that
    // is immediately discarded — the write silently vanishes and every
    // read-modify-write script (Mover, Floaty, Rotator, Player) freezes in place.
    // reference_internal keeps the vec3 alive as long as the component it came from.
    // TransformComponent is the most consequential binding in the scene section.
    // Its three vector properties return REFERENCES (reference_internal), which
    // is what makes `t.Translation.x += 1` actually write. Returning by value
    // here was a real bug: the write landed on a discarded temporary and every
    // mover-style script silently stopped moving.
    // TransformComponent: three def_property blocks, each a getter lambda
    // returning glm::vec3& and a setter lambda. Written as properties rather
    // than def_readwrite precisely because the getters must return references.
    py::class_<Wasteland::TransformComponent>(m, "TransformComponent")
        .def(py::init<>())
        // reference_internal on the GETTER is the load-bearing detail. It tells
        // pybind the returned vec3 borrows from the component, so `t.Translation`
        // hands back a view of the live member -- which is what makes
        // `t.Translation.x += 1` work.
        .def_property("Translation",
             [](Wasteland::TransformComponent &t) -> glm::vec3 & { return t.Translation; },
             [](Wasteland::TransformComponent &t, const glm::vec3 &v) {
                 t.Translation = v;
             },
             // WITHOUT reference_internal, this property returns a copy: the attribute
             // read still succeeds, but assigning into it mutates a discarded temporary
             // and the write vanishes silently. That failure is invisible -- no error, no
             // warning -- so the three properties below all specify the policy explicitly
             // rather than relying on a default.
             py::return_value_policy::reference_internal)
        .def_property("Rotation",
             [](Wasteland::TransformComponent &t) -> glm::vec3 & { return t.Rotation; },
             [](Wasteland::TransformComponent &t, const glm::vec3 &v) {
                 t.Rotation = v;
             },
             py::return_value_policy::reference_internal)
        .def_property("Scale",
             [](Wasteland::TransformComponent &t) -> glm::vec3 & { return t.Scale; },
             [](Wasteland::TransformComponent &t, const glm::vec3 &v) {
                 t.Scale = v;
             },
             py::return_value_policy::reference_internal)
        // SetTransformTrs writes the three fields directly rather than going through
        // GetTransform/SetTransform, which would decompose and recompose a matrix --
        // a lossy round trip for a script that means "set these three values".
        .def("GetTransform", &Wasteland::TransformComponent::GetTransform)
        .def("SetTransform", &Wasteland::TransformComponent::SetTransform)
        .def("SetTransformTrs",
             [](Wasteland::TransformComponent &t, const glm::vec3 &translation,
                const glm::vec3 &rotation, const glm::vec3 &scale) {
                 t.Translation = translation;
                 // The seven Blender-style mesh primitives each get their own class with the
                 // same five-field shape (Color, Texture, TextureIndex, TilingFactor) plus
                 // shape-specific parameters. Uniform by design: a script that knows one
                 // knows all seven.
                 t.Rotation = rotation;
                 t.Scale = scale;
             },
             py::arg("translation"), py::arg("rotation"),
             py::arg("scale") = glm::vec3(1.0f));

// --- SpriteRendererComponent ---
    py::class_<Wasteland::SpriteRendererComponent>(m, "SpriteRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::SpriteRendererComponent::Color)
        .def_readwrite("Texture",
             &Wasteland::SpriteRendererComponent::Texture)
        // PlaneRendererComponent takes Subdivisions, which controls tessellation.
        // Raising it is the cheap way to get a smoother ground plane without a mesh.
        .def_readwrite("TilingFactor",
             &Wasteland::SpriteRendererComponent::TilingFactor);

// --- CircleRendererComponent ---
    py::class_<Wasteland::CircleRendererComponent>(m, "CircleRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::CircleRendererComponent::Color)
        .def_readwrite("Thickness",
             &Wasteland::CircleRendererComponent::Thickness)
        .def_readwrite("Fade", &Wasteland::CircleRendererComponent::Fade);

// --- CubeRendererComponent ---
    // Cube/Sphere/Plane/Grid/Cone/Cylinder/Torus: seven classes with the same
    // five-field shape (Color, Texture, TextureIndex, TilingFactor) plus
    // shape-specific parameters. They are NOT derived from a shared base in the
    // bindings -- each is an independent py::class_ -- so a script that treats
    // them generically has to duck-type rather than isinstance against a base.
    py::class_<Wasteland::CubeRendererComponent>(m, "CubeRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::CubeRendererComponent::Color)
        .def_readwrite("Texture",
             // Texture is a Ref<> member, so def_readwrite copies the smart handle in and
             // out. Assigning one shares the underlying texture rather than copying the
             // pixels, which is why a script can hand the same texture to fifty cubes
             // cheaply.
             &Wasteland::CubeRendererComponent::Texture)
        .def_readwrite("TextureIndex",
             &Wasteland::CubeRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor",
             &Wasteland::CubeRendererComponent::TilingFactor);

// --- SphereRendererComponent ---
    // The sphere adds Slices and Stacks -- the tessellation counts. These are
    // the one pair here that changes GEOMETRY at runtime, so the engine has to
    // rebuild the mesh rather than just re-upload a texture.
    py::class_<Wasteland::SphereRendererComponent>(m, "SphereRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::SphereRendererComponent::Color)
        .def_readwrite("Texture",
             &Wasteland::SphereRendererComponent::Texture)
        .def_readwrite("TextureIndex",
             &Wasteland::SphereRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor",
             &Wasteland::SphereRendererComponent::TilingFactor)
        .def_readwrite("Radius", &Wasteland::SphereRendererComponent::Radius)
        .def_readwrite("Sectors", &Wasteland::SphereRendererComponent::Sectors)
        .def_readwrite("Stacks", &Wasteland::SphereRendererComponent::Stacks);

// MaterialComponent. NormalStrength, Metallic and Roughness are the standard
// PBR triple; EmissionColor/EmissionIntensity are separated so an emissive
// material can change colour without changing brightness.
// --- Blender "Add > Mesh" primitives ---
// One class per shape, mirroring the Sphere block above. Each has at least one
// real member -- a zero-sized type makes entt's get() return void and breaks
// Entity::GetComponent<T>().
    // Plane adds Subdivisions for the same reason: a flat quad needs no
    // tessellation, so any visible faceting is fixed by raising it.
    py::class_<Wasteland::PlaneRendererComponent>(m, "PlaneRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::PlaneRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::PlaneRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::PlaneRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::PlaneRendererComponent::TilingFactor)
        .def_readwrite("Subdivisions", &Wasteland::PlaneRendererComponent::Subdivisions);

    // Grid adds CellCount/AxisCount, which are COUNTS not sizes. The cell SIZE
    // comes from the entity's scale, so a script that sets CellCount without
    // touching scale gets a different world size than it may expect.
    py::class_<Wasteland::GridRendererComponent>(m, "GridRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::GridRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::GridRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::GridRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::GridRendererComponent::TilingFactor)
        .def_readwrite("XSubdivisions", &Wasteland::GridRendererComponent::XSubdivisions)
        .def_readwrite("YSubdivisions", &Wasteland::GridRendererComponent::YSubdivisions);

    py::class_<Wasteland::Circle3DRendererComponent>(m, "Circle3DRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::Circle3DRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::Circle3DRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::Circle3DRendererComponent::TextureIndex)
        // HasGeneratedMaps and TexturePath are readwrite even though they describe
        // the OUTPUT of an offline bake rather than an authorable value. A script can
        // clear them by assignment, which would leave the renderer believing maps
        // exist that do not -- nothing here validates the transition.
        .def_readwrite("TilingFactor", &Wasteland::Circle3DRendererComponent::TilingFactor)
        .def_readwrite("Radius", &Wasteland::Circle3DRendererComponent::Radius)
        .def_readwrite("Segments", &Wasteland::Circle3DRendererComponent::Segments);

    py::class_<Wasteland::IcoSphereRendererComponent>(m, "IcoSphereRendererComponent")
        .def(py::init<>())
        // CameraComponent carries the SceneCamera itself plus a Primary flag.
        // Primary is how the engine picks THE camera when a scene has several.
        .def_readwrite("Color", &Wasteland::IcoSphereRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::IcoSphereRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::IcoSphereRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::IcoSphereRendererComponent::TilingFactor)
        .def_readwrite("Radius", &Wasteland::IcoSphereRendererComponent::Radius)
        // FixedAspectRatio, for rendering at an aspect other than the viewport's.
        // This is the letterbox/pillarbox path -- it does not letterbox, it tells
        // the projection to use a different aspect.
        .def_readwrite("Subdivisions", &Wasteland::IcoSphereRendererComponent::Subdivisions);

    py::class_<Wasteland::CylinderRendererComponent>(m, "CylinderRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::CylinderRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::CylinderRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::CylinderRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::CylinderRendererComponent::TilingFactor)
        .def_readwrite("Radius", &Wasteland::CylinderRendererComponent::Radius)
        // Rigidbody2DComponent plus its BodyType enum registered immediately before
        // it, so Type is comparable against a Python enum member.
        .def_readwrite("Height", &Wasteland::CylinderRendererComponent::Height)
        .def_readwrite("Segments", &Wasteland::CylinderRendererComponent::Segments);

    py::class_<Wasteland::ConeRendererComponent>(m, "ConeRendererComponent")
        .def(py::init<>())
        // RuntimeBody and RuntimeFixture are the PHYSICS-WORLD handles, not the
        // authored data. They are exposed readwrite, which means a script can null
        // out a live fixture handle and leave the solver holding a dangling pointer.
        // There is no setter guard here.
        // Cone and Torus are the two primitives with genuinely non-uniform
        // parameter sets. Cone adds Radius/Height/Segments; Torus splits its radii
        // into major and minor and tessellates on two independent axes.
        .def_readwrite("Color", &Wasteland::ConeRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::ConeRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::ConeRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::ConeRendererComponent::TilingFactor)
        .def_readwrite("Radius", &Wasteland::ConeRendererComponent::Radius)
        .def_readwrite("Height", &Wasteland::ConeRendererComponent::Height)
        // Segments is the radial tessellation count. Raising it changes the GEOMETRY,
        // so the engine must rebuild the mesh -- unlike TilingFactor, which is a
        // shader uniform and costs nothing to change per frame.
        .def_readwrite("Segments", &Wasteland::ConeRendererComponent::Segments);

    // The 2D and 3D collider sets differ in one important way: the 2D ones
    // carry RestitutionThreshold, the 3D ones do not. That is a real engine
    // asymmetry, not an omission here -- Box2D applies a velocity threshold
    // before bouncing, Box3D does not.
    // Torus's MajorSegments/MinorSegments are independent: the first is around
    // the ring, the second around the tube. A torus with many major and few minor
    // segments is the usual way to get a visible faceting artefact on a smooth
    // object -- the two need not match.
    py::class_<Wasteland::TorusRendererComponent>(m, "TorusRendererComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::TorusRendererComponent::Color)
        .def_readwrite("Texture", &Wasteland::TorusRendererComponent::Texture)
        .def_readwrite("TextureIndex", &Wasteland::TorusRendererComponent::TextureIndex)
        .def_readwrite("TilingFactor", &Wasteland::TorusRendererComponent::TilingFactor)
        .def_readwrite("MajorRadius", &Wasteland::TorusRendererComponent::MajorRadius)
        .def_readwrite("MinorRadius", &Wasteland::TorusRendererComponent::MinorRadius)
        .def_readwrite("MajorSegments", &Wasteland::TorusRendererComponent::MajorSegments)
        .def_readwrite("MinorSegments", &Wasteland::TorusRendererComponent::MinorSegments);

// --- MaterialComponent ---
    // MaterialComponent: the PBR parameter set. Albedo/Texture/TextureIndex
    // first, then the scalars. Note NormalStrength sits BETWEEN the texture
    // fields and Metallic/Roughness, which is the shader's layout order rather
    // than a logical grouping.
    // MaterialComponent: the full PBR field list. Albedo/Texture/TextureIndex
    // are the base colour inputs; NormalStrength, Metallic and Roughness are the
    // material response; EmissionColor/EmissionIntensity and DisplacementScale
    // are the extras; HasGeneratedMaps/TexturePath describe the bake output.
    py::class_<Wasteland::MaterialComponent>(m, "MaterialComponent")
        .def(py::init<>())
        .def_readwrite("Albedo", &Wasteland::MaterialComponent::Albedo)
        .def_readwrite("Texture", &Wasteland::MaterialComponent::Texture)
        .def_readwrite("TextureIndex",
             &Wasteland::MaterialComponent::TextureIndex)
        .def_readwrite("NormalStrength",
             &Wasteland::MaterialComponent::NormalStrength)
        .def_readwrite("Metallic", &Wasteland::MaterialComponent::Metallic)
        .def_readwrite("Roughness", &Wasteland::MaterialComponent::Roughness)
        .def_readwrite("EmissionColor",
             &Wasteland::MaterialComponent::EmissionColor)
        .def_readwrite("EmissionIntensity",
             &Wasteland::MaterialComponent::EmissionIntensity)
        // DisplacementScale is vertex displacement, not normal-map parallax. So it
        // needs geometry with enough vertices to displace, and raising it on a
        // low-poly mesh tears the surface rather than adding detail.
        .def_readwrite("DisplacementScale",
             &Wasteland::MaterialComponent::DisplacementScale)
        // HasGeneratedMaps and TexturePath are the bake OUTPUT, exposed readwrite
        // as noted earlier. A script can set HasGeneratedMaps True for maps that
        // were never generated, and the renderer will believe them.
        .def_readwrite("HasGeneratedMaps",
             &Wasteland::MaterialComponent::HasGeneratedMaps)
        .def_readwrite("TexturePath",
             &Wasteland::MaterialComponent::TexturePath);

// --- CameraComponent ---
    // CameraComponent. Note this class had no Entity accessor for a long time
    // because the BIND_COMP_* macro stringified the fully qualified C++ type
    // into an unaddressable name containing '::'; BIND_GAMEPLAY_COMP below
    // fixes that, and the broken name is left bound for getattr() callers.
    py::class_<Wasteland::CameraComponent>(m, "CameraComponent")
        .def(py::init<>())
        .def_readwrite("Camera", &Wasteland::CameraComponent::Camera)
        .def_readwrite("Primary", &Wasteland::CameraComponent::Primary)
        .def_readwrite("FixedAspectRatio",
             &Wasteland::CameraComponent::FixedAspectRatio);

// --- Rigidbody2D ---
    py::enum_<Wasteland::Rigidbody2DComponent::BodyType>(m, "Rigidbody2DBodyType")
        .value("Static",   Wasteland::Rigidbody2DComponent::BodyType::Static)
        .value("Dynamic",  Wasteland::Rigidbody2DComponent::BodyType::Dynamic)
        .value("Kinematic",Wasteland::Rigidbody2DComponent::BodyType::Kinematic);

// The rigidbody Type enum is registered BEFORE the class so the Type field
// has something to convert to. Ordering matters for enums used as field
// types -- pybind resolves the caster when the class is instantiated.
py::class_<Wasteland::Rigidbody2DComponent>(m, "Rigidbody2DComponent")
        .def(py::init<>())
        .def_readwrite("Type", &Wasteland::Rigidbody2DComponent::Type)
        .def_readwrite("FixedRotation",
             &Wasteland::Rigidbody2DComponent::FixedRotation)
        .def_readwrite("RuntimeBody",
             &Wasteland::Rigidbody2DComponent::RuntimeBody);

// --- BoxCollider2D ---
    py::class_<Wasteland::BoxCollider2DComponent>(m, "BoxCollider2DComponent")
        .def(py::init<>())
        .def_readwrite("Offset", &Wasteland::BoxCollider2DComponent::Offset)
        .def_readwrite("Size", &Wasteland::BoxCollider2DComponent::Size)
        .def_readwrite("Density", &Wasteland::BoxCollider2DComponent::Density)
        .def_readwrite("Friction", &Wasteland::BoxCollider2DComponent::Friction)
        .def_readwrite("Restitution",
             &Wasteland::BoxCollider2DComponent::Restitution)
        .def_readwrite("RestitutionThreshold",
             &Wasteland::BoxCollider2DComponent::RestitutionThreshold)
        .def_readwrite("RuntimeFixture",
             // GravityScale multiplies world gravity for this body alone. A value of 0
             // gives a body that ignores gravity while still colliding, which is the
             // standard trick for a player character.
             &Wasteland::BoxCollider2DComponent::RuntimeFixture);

// --- CircleCollider2D ---
    py::class_<Wasteland::CircleCollider2DComponent>(m, "CircleCollider2DComponent")
        .def(py::init<>())
        // FixedRotation is split per-axis in 3D (X/Y/Z) but is a single bool in 2D.
        // 2D has one rotation axis, so one flag is the whole story.
        .def_readwrite("Offset",
             &Wasteland::CircleCollider2DComponent::Offset)
        .def_readwrite("Radius",
             &Wasteland::CircleCollider2DComponent::Radius)
        .def_readwrite("Density",
             &Wasteland::CircleCollider2DComponent::Density)
        .def_readwrite("Friction",
             &Wasteland::CircleCollider2DComponent::Friction)
        .def_readwrite("Restitution",
             &Wasteland::CircleCollider2DComponent::Restitution)
        .def_readwrite("RestitutionThreshold",
             &Wasteland::CircleCollider2DComponent::RestitutionThreshold)
        .def_readwrite("RuntimeFixture",
             &Wasteland::CircleCollider2DComponent::RuntimeFixture);

// --- RigidBody3D ---
    py::enum_<Wasteland::RigidBody3DType>(m, "RigidBody3DType")
        .value("Static",   Wasteland::RigidBody3DType::Static)
        .value("Dynamic",  Wasteland::RigidBody3DType::Dynamic)
        .value("Kinematic",Wasteland::RigidBody3DType::Kinematic);

// RigidBody3DComponent. GravityScale plus the two damping values and three
// per-axis rotation locks. All plain fields -- the physics world reads them
// when it builds the body, so changing one at runtime may need a body
// rebuild the binding does not request.
py::class_<Wasteland::RigidBody3DComponent>(m, "RigidBody3DComponent")
        .def(py::init<>())
        .def_readwrite("Type", &Wasteland::RigidBody3DComponent::Type)
        // GravityScale of 0 gives a body that ignores gravity but still collides,
        // which is how a player character is normally built.
        .def_readwrite("GravityScale",
             &Wasteland::RigidBody3DComponent::GravityScale)
        .def_readwrite("LinearDamping",
             &Wasteland::RigidBody3DComponent::LinearDamping)
        .def_readwrite("AngularDamping",
             &Wasteland::RigidBody3DComponent::AngularDamping)
        // FixedRotationX/Y/Z are three separate bools rather than one, because a
        // common setup is a body that may yaw but not pitch or roll -- a character
        // that stays upright. The 2D rigidbody above has a single flag for the
        // same reason its world has one rotation axis.
        .def_readwrite("FixedRotationX",
             &Wasteland::RigidBody3DComponent::FixedRotationX)
        .def_readwrite("FixedRotationY",
             &Wasteland::RigidBody3DComponent::FixedRotationY)
        .def_readwrite("FixedRotationZ",
             &Wasteland::RigidBody3DComponent::FixedRotationZ)
        // RuntimeBody is the PHYSICS-WORLD handle, exposed readwrite with no guard.
        // Writing to it replaces the pointer the solver is using; see the note on
        // the 2D rigidbody's RuntimeFixture above.
        .def_readwrite("RuntimeBody",
             &Wasteland::RigidBody3DComponent::RuntimeBody);

// --- BoxCollider3D ---
    py::class_<Wasteland::BoxCollider3DComponent>(m, "BoxCollider3DComponent")
        .def(py::init<>())
        .def_readwrite("HalfExtents",
             &Wasteland::BoxCollider3DComponent::HalfExtents)
        // The four 3D colliders. HalfExtents on the box is the half-size, not the
        // full size -- a name that catches scripts out when they halve their own
        // numbers a second time.
        // The four 3D colliders share Offset/Density/Friction/Restitution plus a
        // shape parameter and a RuntimeShape handle. The material triple is
        // IDENTICAL across all four -- Box2D's version above additionally carries
        // RestitutionThreshold, Box3D's does not. That asymmetry is a physics-engine
        // difference, not an oversight in the binding.
        .def_readwrite("Offset", &Wasteland::BoxCollider3DComponent::Offset)
        .def_readwrite("Density", &Wasteland::BoxCollider3DComponent::Density)
        .def_readwrite("Friction", &Wasteland::BoxCollider3DComponent::Friction)
        .def_readwrite("Restitution",
             &Wasteland::BoxCollider3DComponent::Restitution)
        // RuntimeShape is the live physics handle, exposed readwrite with no guard
        // on any of the four colliders. Writing it replaces the shape the solver is
        // using; see the note on the 2D rigidbody's RuntimeFixture.
        .def_readwrite("RuntimeShape",
             &Wasteland::BoxCollider3DComponent::RuntimeShape);

// --- SphereCollider3D ---
    py::class_<Wasteland::SphereCollider3DComponent>(m, "SphereCollider3DComponent")
        .def(py::init<>())
        .def_readwrite("Radius", &Wasteland::SphereCollider3DComponent::Radius)
        .def_readwrite("Offset",
             &Wasteland::SphereCollider3DComponent::Offset)
        .def_readwrite("Density",
             &Wasteland::SphereCollider3DComponent::Density)
        .def_readwrite("Friction",
             &Wasteland::SphereCollider3DComponent::Friction)
        .def_readwrite("Restitution",
             &Wasteland::SphereCollider3DComponent::Restitution)
        .def_readwrite("RuntimeShape",
             &Wasteland::SphereCollider3DComponent::RuntimeShape);

// --- CapsuleCollider3D ---
    // Capsule adds Height on top of Radius. In Box3D the two are half-extents,
    // so Height here is the FULL height of the cylindrical section -- the two
    // conventions differ within the same file, which is a genuine trap when
    // porting a value from a 2D capsule.
    py::class_<Wasteland::CapsuleCollider3DComponent>(m, "CapsuleCollider3DComponent")
        .def(py::init<>())
        .def_readwrite("Radius",
             &Wasteland::CapsuleCollider3DComponent::Radius)
        .def_readwrite("Height",
             &Wasteland::CapsuleCollider3DComponent::Height)
        .def_readwrite("Offset",
             &Wasteland::CapsuleCollider3DComponent::Offset)
        .def_readwrite("Density",
             &Wasteland::CapsuleCollider3DComponent::Density)
        .def_readwrite("Friction",
             &Wasteland::CapsuleCollider3DComponent::Friction)
        .def_readwrite("Restitution",
             &Wasteland::CapsuleCollider3DComponent::Restitution)
        .def_readwrite("RuntimeShape",
             // The two volumetric components. MaxSteps on both is the raymarch step
             // count, i.e. the direct quality/cost knob for the effect.
             &Wasteland::CapsuleCollider3DComponent::RuntimeShape);

// --- VolumetricFogComponent ---
    // Fog has NoiseStrength/NoiseScale/WindSpeed: the fog volume is animated,
    // not a static density field. Clouds below has the same three plus a
    // wind DIRECTION that fog lacks.
    // VolumetricFogComponent: Color/Density/Anisotropy/HeightFalloff are the
    // base look, NoiseStrength/NoiseScale/WindSpeed animate it, and MaxSteps is
    // the raymarch budget. Ten fields, of which exactly one affects cost.
    py::class_<Wasteland::VolumetricFogComponent>(m, "VolumetricFogComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::VolumetricFogComponent::Enabled)
        .def_readwrite("Color", &Wasteland::VolumetricFogComponent::Color)
        .def_readwrite("Density", &Wasteland::VolumetricFogComponent::Density)
        // Anisotropy is how directional the scattering is -- 0 is isotropic haze,
        // high values give shafts. It is the parameter that separates a fog volume
        // from a smoke volume, and it is the one most worth animating.
        .def_readwrite("Anisotropy",
             &Wasteland::VolumetricFogComponent::Anisotropy)
        .def_readwrite("HeightFalloff",
             &Wasteland::VolumetricFogComponent::HeightFalloff)
        .def_readwrite("NoiseStrength",
             &Wasteland::VolumetricFogComponent::NoiseStrength)
        .def_readwrite("NoiseScale",
             &Wasteland::VolumetricFogComponent::NoiseScale)
        .def_readwrite("WindSpeed",
             &Wasteland::VolumetricFogComponent::WindSpeed)
        // MaxSteps is the quality/cost knob, and it interacts with
        // Renderer3D_VolumetricStepScale above, which scales both volumetric
        // effects globally. So the effective step count is this times that global
        // scale.
        .def_readwrite("MaxSteps", &Wasteland::VolumetricFogComponent::MaxSteps);

// --- VolumetricCloudsComponent ---
    // VolumetricCloudsComponent mirrors VolumetricFogComponent's Enabled/Color/
    // NoiseScale/WindSpeed/MaxSteps but replaces Anisotropy and HeightFalloff
    // with Coverage, Density, DetailAmount, SilverLining and ShadowStrength.
    // So the shared names do NOT mean the same thing across the two: Density in
    // fog is opacity, in clouds it is optical thickness.
    py::class_<Wasteland::VolumetricCloudsComponent>(m, "VolumetricCloudsComponent")
        .def(py::init<>())
        .def_readwrite("Enabled",
             &Wasteland::VolumetricCloudsComponent::Enabled)
        .def_readwrite("Color", &Wasteland::VolumetricCloudsComponent::Color)
        .def_readwrite("AmbientTint",
             &Wasteland::VolumetricCloudsComponent::AmbientTint)
        // Coverage is how much of the sky is covered; DetailAmount is the
        // high-frequency erosion on top. Raising DetailAmount without raising
        // MaxSteps buys noise rather than resolution -- the step count is what
        // resolves it.
        .def_readwrite("Coverage",
             &Wasteland::VolumetricCloudsComponent::Coverage)
        .def_readwrite("Density",
             &Wasteland::VolumetricCloudsComponent::Density)
        .def_readwrite("NoiseScale",
             &Wasteland::VolumetricCloudsComponent::NoiseScale)
        .def_readwrite("DetailAmount",
             &Wasteland::VolumetricCloudsComponent::DetailAmount)
        // WindDirection exists for clouds but NOT for fog, which only has
        // WindSpeed. Fog therefore animates in a fixed direction baked into the
        // shader, while clouds get a direction vector from the component.
        .def_readwrite("WindDirection",
             // Cloud coverage, density and detail amount are separate. DetailAmount
             // controls the high-frequency erosion; raising it without raising the
             // step count buys noise, not detail.
             &Wasteland::VolumetricCloudsComponent::WindDirection)
        .def_readwrite("WindSpeed",
             &Wasteland::VolumetricCloudsComponent::WindSpeed)
        // SilverLining and ShadowStrength are the two lighting terms: how much
        // forward-scatter the cloud edges get, and how dark the cloud bodies are
        // against that scatter. They are independent, so a script can have bright
        // edges with flat bodies or the reverse.
        .def_readwrite("SilverLining",
             &Wasteland::VolumetricCloudsComponent::SilverLining)
        .def_readwrite("ShadowStrength",
             &Wasteland::VolumetricCloudsComponent::ShadowStrength)
        .def_readwrite("MaxSteps",
             &Wasteland::VolumetricCloudsComponent::MaxSteps);

// --- Lights ---
    // The four light components. Only Range and Falloff differ between point,
    // spot and area -- which is why the engine can share a submission path.
    // The four light components. DirectionalLight has ONLY Color and
    // Intensity -- no position, no range, because it is infinitely distant and
    // parallel. Every other light type adds Range and Falloff.
    py::class_<Wasteland::DirectionalLightComponent>(m, "DirectionalLightComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::DirectionalLightComponent::Color)
        .def_readwrite("Intensity",
             &Wasteland::DirectionalLightComponent::Intensity);

py::class_<Wasteland::PointLightComponent>(m, "PointLightComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::PointLightComponent::Color)
        .def_readwrite("Intensity",
             &Wasteland::PointLightComponent::Intensity)
        .def_readwrite("Range", &Wasteland::PointLightComponent::Range)
        .def_readwrite("Falloff", &Wasteland::PointLightComponent::Falloff);

// SpotLightComponent adds InnerAngle/OuterAngle, the cone's two edges.
// The falloff BETWEEN them is what distinguishes a spotlight from a point
// light visually, and it is not derivable from Range.
// SpotLight adds InnerAngle/OuterAngle on top of the point light's set.
// The falloff BETWEEN the two angles is what makes a spotlight read as a
// spotlight; it cannot be derived from Range. Setting InnerAngle greater
// than OuterAngle inverts the cone, and nothing here prevents it.
py::class_<Wasteland::SpotLightComponent>(m, "SpotLightComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::SpotLightComponent::Color)
        .def_readwrite("Intensity",
             &Wasteland::SpotLightComponent::Intensity)
        .def_readwrite("Range", &Wasteland::SpotLightComponent::Range)
        .def_readwrite("Falloff", &Wasteland::SpotLightComponent::Falloff)
        .def_readwrite("InnerAngle",
             &Wasteland::SpotLightComponent::InnerAngle)
        .def_readwrite("OuterAngle",
             &Wasteland::SpotLightComponent::OuterAngle);

// AreaLight replaces angles with Width/Height and is DoubleSided -- an area
// light emits from a face, so whether it emits backwards is a real choice.
// It is the only light here with a DoubleSided field.
py::class_<Wasteland::AreaLightComponent>(m, "AreaLightComponent")
        .def(py::init<>())
        .def_readwrite("Color", &Wasteland::AreaLightComponent::Color)
        .def_readwrite("Intensity",
             &Wasteland::AreaLightComponent::Intensity)
        .def_readwrite("Width", &Wasteland::AreaLightComponent::Width)
        .def_readwrite("Height", &Wasteland::AreaLightComponent::Height)
        .def_readwrite("Range", &Wasteland::AreaLightComponent::Range)
        // ScriptComponent and its two siblings are the script attachment points.
        // They hold engine-owned state (the loaded instance, tick bookkeeping) as
        // plain readwrite fields, which means a script CAN overwrite the instance
        // pointer the engine is about to call. Nothing here validates it.
        .def_readwrite("DoubleSided",
             &Wasteland::AreaLightComponent::DoubleSided);

// --- ScriptComponent ---
    // ScriptComponent is the attachment point for a Python script: a path and a
    // display name. The engine reads them at load time; changing them at
    // runtime does not hot-reload the script.
    py::class_<Wasteland::ScriptComponent>(m, "ScriptComponent")
        .def(py::init<>())
        .def_readwrite("ScriptPath",
             &Wasteland::ScriptComponent::ScriptPath)
        .def_readwrite("ScriptName",
             &Wasteland::ScriptComponent::ScriptName);

// --- BehaviorTreeComponent ---
    // Mirrors ScriptComponent so one attachment model covers both. Until this
    // existed an authored .wlbtree could not be attached to an entity from
    // Python at all, which also made it untestable headlessly.
    // BehaviorTreeComponent mirrors ScriptComponent exactly, which is what makes
    // one attachment model cover both. Its three extra read-only fields are
    // engine-owned diagnostics.
    py::class_<Wasteland::BehaviorTreeComponent>(m, "BehaviorTreeComponent")
        .def(py::init<>())
        .def_readwrite("TreePath", &Wasteland::BehaviorTreeComponent::TreePath)
        .def_readwrite("TreeName", &Wasteland::BehaviorTreeComponent::TreeName)
        // Read-only: the ENGINE owns these. Exposed so a test (and the editor)
        // can see WHY an NPC is standing still instead of guessing.
        .def_readonly("CompileError", &Wasteland::BehaviorTreeComponent::CompileError)
        .def_readonly("Attached", &Wasteland::BehaviorTreeComponent::Attached)
        .def_readonly("SuspendedTicks",
                      // SuspendedTicks is the behaviour tree's pause counter. It is readwrite, so
                      // a script can freeze a tree by setting it, but nothing stops a script
                      // setting it to a value so large the tree never resumes.
                      &Wasteland::BehaviorTreeComponent::SuspendedTicks);

// --- NativeScriptComponent ---
    // Instance is a raw pointer (ScriptableEntity*), InstantiateScript/DestroyScript
    // are function pointers — skip the function-pointer members; they're C++-only.
    // NativeScriptComponent for C++-side ScriptableEntity subclasses. Instance
    // is a raw ScriptableEntity pointer exposed readwrite -- writing to it from
    // Python replaces the pointer the engine thinks is live.
    py::class_<Wasteland::NativeScriptComponent>(m, "NativeScriptComponent")
        .def(py::init<>())
        .def_readwrite("Instance",
             // Instance on NativeScriptComponent is a raw ScriptableEntity pointer --
             // the C++ subclass instance the engine owns. Exposing it readwrite lets a
             // script replace the pointer the engine will call next tick, with no check
             // that the replacement is a live object.
             &Wasteland::NativeScriptComponent::Instance);

// --- Water ---
    // WaterBodyType distinguishes still bodies from flowing ones, because the
    // flow parameters below only mean something for the latter.
    py::enum_<Wasteland::WaterBodyType>(m, "WaterBodyType")
        .value("Lake",   Wasteland::WaterBodyType::Lake)
        .value("River",  Wasteland::WaterBodyType::River)
        .value("Ocean",  Wasteland::WaterBodyType::Ocean);

// WaterComponent is the largest single component in the file -- about twenty
// fields covering look, wave shape, flow and foam. They fall into four
// groups: colour/depth, wave shape (amplitude/length/direction/steepness),
// flow, and shoreline foam.
py::class_<Wasteland::WaterComponent>(m, "WaterComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::WaterComponent::Enabled)
        .def_readwrite("Type", &Wasteland::WaterComponent::Type)
        // WaterComponent's field list, in the shader's own order rather than any
        // logical grouping: three colours (shallow/deep/foam), then the wave
        // parameters, then flow, then foam, then surface response, then simulation
        // controls. Every field is a plain readwrite -- no accessor methods, so a
        // script writes uniforms directly.
        .def_readwrite("ShallowColor",
             &Wasteland::WaterComponent::ShallowColor)
        .def_readwrite("DeepColor", &Wasteland::WaterComponent::DeepColor)
        .def_readwrite("FoamColor", &Wasteland::WaterComponent::FoamColor)
        // WaterDepth is the depth at which the water reaches DeepColor. With
        // ShallowColor and DeepColor both set, depth is what blends between them,
        // so a shallow body and a deep one look different from the same settings.
        .def_readwrite("WaterDepth",
             &Wasteland::WaterComponent::WaterDepth)
        .def_readwrite("WaveAmplitude",
             // Amplitude and WaveLength together set the wave shape; Direction is a
             // NORMALISED direction, so changing its length without normalising has no
             // effect. Steepness is how sharply the wave peaks.
             &Wasteland::WaterComponent::WaveAmplitude)
        .def_readwrite("WaveLength",
             &Wasteland::WaterComponent::WaveLength)
        // WaveSpeed is separate from WaveAmplitude and WaveLength: speed is how
        // fast the pattern travels, the other two are its size. Chop adds the
        // high-frequency detail on top.
        .def_readwrite("WaveSpeed", &Wasteland::WaterComponent::WaveSpeed)
        .def_readwrite("WaveDirection",
             &Wasteland::WaterComponent::WaveDirection)
        .def_readwrite("Chop", &Wasteland::WaterComponent::Chop)
        .def_readwrite("Steepness",
             // FlowDirection/FlowSpeed give the current. On a still WaterBodyType these
             // are carried but ignored by the shader, so setting them on a pond costs
             // nothing and does nothing.
             &Wasteland::WaterComponent::Steepness)
        .def_readwrite("FlowDirection",
             &Wasteland::WaterComponent::FlowDirection)
        .def_readwrite("FlowSpeed",
             &Wasteland::WaterComponent::FlowSpeed)
        // FoamAmount and FoamScale: amount is how much foam, scale is how fine it
        // looks. They are independent, so raising scale on a fixed amount gives
        // finer foam rather than more.
        .def_readwrite("FoamAmount",
             &Wasteland::WaterComponent::FoamAmount)
        .def_readwrite("FoamScale", &Wasteland::WaterComponent::FoamScale)
        .def_readwrite("ShoreFoam",
             // ShoreFoam and ShoreWidth are the shoreline effect: how much foam, over how
             // wide a band. ShoreWidth is a world distance, so it interacts with entity
             // scale rather than being a percentage.
             &Wasteland::WaterComponent::ShoreFoam)
        .def_readwrite("ShoreWidth",
             &Wasteland::WaterComponent::ShoreWidth)
        .def_readwrite("Transparency",
             // Transparency and Roughness are the two water look controls a script is
             // most likely to animate, and both are cheap -- they are uniform updates,
             // not geometry changes.
             // Transparency/Roughness/Metallic/Segments are the surface response group.
             // Segments is the tessellation count -- a GEOMETRY change, unlike its three
             // neighbours, so this one is not a free uniform update.
             &Wasteland::WaterComponent::Transparency)
        .def_readwrite("Roughness",
             &Wasteland::WaterComponent::Roughness)
        .def_readwrite("Metallic", &Wasteland::WaterComponent::Metallic)
        .def_readwrite("Segments", &Wasteland::WaterComponent::Segments)
        .def_readwrite("TimeScale",
             // TimeScale animates the whole water simulation independently of game time.
             // Setting it above 1 is the cheapest way to make a still scene look alive
             // without advancing any other timer in the game.
             &Wasteland::WaterComponent::TimeScale)
        // Buoyancy and WaterDensity together decide how strongly an object floats.
        // They are distinct knobs: buoyancy is a force coefficient, density is a
        // fluid property, and changing only one does not give the same result as
        // scaling the other.
        .def_readwrite("Buoyancy", &Wasteland::WaterComponent::Buoyancy)
        .def_readwrite("WaterDensity",
             &Wasteland::WaterComponent::WaterDensity)
        // Time is the water simulation's own clock, exposed readwrite. It advances
        // independently of game time (see TimeScale below), so a script can freeze
        // the waves without freezing the game by writing Time directly.
        .def_readwrite("Time", &Wasteland::WaterComponent::Time)
        // SampleHeightAt / SampleNormalAt / CrestFactorAt all take (worldX, worldZ)
        // plus an optional minWavelength. That last argument filters out waves
        // shorter than a threshold -- which is how a script asks for the same
        // filtered surface the renderer uses instead of the full detail sum.
        .def("SampleHeightAt", &Wasteland::WaterComponent::SampleHeightAt,
             py::arg("worldX"), py::arg("worldZ"),
             py::arg("minWavelength") = 0.0f)
        .def("SampleNormalAt", &Wasteland::WaterComponent::SampleNormalAt,
             py::arg("worldX"), py::arg("worldZ"),
             py::arg("minWavelength") = 0.0f)
        // CrestFactorAt is the wave-height proportion at a point: 1 at a crest, 0 at
        // a trough. It is what a script uses to detect a crest for a splash or a
        // buoy trigger without reimplementing the wave sum.
        .def("CrestFactorAt", &Wasteland::WaterComponent::CrestFactorAt,
             py::arg("worldX"), py::arg("worldZ"),
             py::arg("minWavelength") = 0.0f)
        .def("SampleFlow", &Wasteland::WaterComponent::SampleFlow);

// --- Gameplay components ---
    // Behaviour lives in the script; these bindings expose the data plus the
    // small amount of state machine the C++ side owns (fire gating, reload
    // countdown, damage lockout) so scripts don't reimplement it.
    // HealthComponent: the fields are Current/Max/Invulnerable plus the
    // cooldown pair, and then three methods. Note IsDead is a FIELD rather than
    // a derived value from Current -- so it can disagree with the health total,
    // and a script can set either independently.
    py::class_<Wasteland::HealthComponent>(m, "HealthComponent")
        .def(py::init<>())
        .def_readwrite("Current", &Wasteland::HealthComponent::Current)
        .def_readwrite("Max", &Wasteland::HealthComponent::Max)
        // Invulnerable is the master switch and is independent of the cooldown: it
        // makes ApplyDamage a no-op outright, where the cooldown only throttles.
        .def_readwrite("Invulnerable", &Wasteland::HealthComponent::Invulnerable)
        // DamageCooldown and CooldownTimer implement invulnerability frames. Both
        // writable, so setting the timer past its cooldown ends i-frames early,
        // and setting DamageCooldown to a large value makes the entity nearly
        // unkillable.
        .def_readwrite("DamageCooldown", &Wasteland::HealthComponent::DamageCooldown)
        .def_readwrite("CooldownTimer", &Wasteland::HealthComponent::CooldownTimer)
        .def_readwrite("IsDead", &Wasteland::HealthComponent::IsDead)
        // ApplyDamage/Heal/Reset are the only METHODS on HealthComponent -- every
        // other component in this file is pure fields. So health is the one place a
        // script changes state through an engine-side method that can have side
        // effects (death events) rather than by assigning a field.
        .def("ApplyDamage", &Wasteland::HealthComponent::ApplyDamage, py::arg("amount"))
        .def("Heal", &Wasteland::HealthComponent::Heal, py::arg("amount"))
        // Reset restores full health and clears IsDead. The IsDead FIELD is
        // readwrite too, so a script can force-resurrect by setting it False
        // directly -- bypassing whatever the engine did on death.
        .def("Reset", &Wasteland::HealthComponent::Reset);

py::class_<Wasteland::WeaponComponent>(m, "WeaponComponent")
        .def(py::init<>())
        .def_readwrite("FireRate", &Wasteland::WeaponComponent::FireRate)
        .def_readwrite("Damage", &Wasteland::WeaponComponent::Damage)
        .def_readwrite("Range", &Wasteland::WeaponComponent::Range)
        .def_readwrite("AmmoPerShot", &Wasteland::WeaponComponent::AmmoPerShot)
        .def_readwrite("CurrentAmmo", &Wasteland::WeaponComponent::CurrentAmmo)
        .def_readwrite("MaxAmmo", &Wasteland::WeaponComponent::MaxAmmo)
        .def_readwrite("ReserveAmmo", &Wasteland::WeaponComponent::ReserveAmmo)
        .def_readwrite("ReloadTime", &Wasteland::WeaponComponent::ReloadTime)
        .def_readwrite("AutoReload", &Wasteland::WeaponComponent::AutoReload)
        // FireCooldown, ReloadTimer and IsReloading are engine-owned counters, all
        // exposed readwrite. Setting FireCooldown to 0 is a way to bypass the fire
        // rate; setting ReloadTimer past its target completes a reload instantly.
        // Convenient for a debug console, and a cheat vector in a shipped build.
        .def_readwrite("FireCooldown", &Wasteland::WeaponComponent::FireCooldown)
        .def_readwrite("ReloadTimer", &Wasteland::WeaponComponent::ReloadTimer)
        .def_readwrite("IsReloading", &Wasteland::WeaponComponent::IsReloading)
        .def_readwrite("ShotsFired", &Wasteland::WeaponComponent::ShotsFired)
        // CanFire/CanReload are the QUERY side of the same state the setters above
        // can corrupt. Since the timers are writable, a script can make CanFire
        // disagree with what Fire actually does -- the binding layer offers no
        // consistency check between them.
        .def("CanFire", &Wasteland::WeaponComponent::CanFire)
        .def("Fire", &Wasteland::WeaponComponent::Fire)
        .def("CanReload", &Wasteland::WeaponComponent::CanReload)
        .def("StartReload", &Wasteland::WeaponComponent::StartReload)
        // Tick is exposed so a script can drive weapon cooldown itself. The engine
        // also calls it, so a script that calls it too advances the cooldown twice
        // as fast.
        .def("Tick", &Wasteland::WeaponComponent::Tick, py::arg("dt"))
        .def("Reset", &Wasteland::WeaponComponent::Reset);

// Marker components: presence is the payload, but they still need a bound
    // type so GetComponent/HasComponent can name them. Enabled is exposed so a
    // script can toggle a marker without adding/removing the component.
    py::class_<Wasteland::RaycastIgnoreComponent>(m, "RaycastIgnoreComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::RaycastIgnoreComponent::Enabled);

py::class_<Wasteland::HittableComponent>(m, "HittableComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::HittableComponent::Enabled);

py::class_<Wasteland::InteractableComponent>(m, "InteractableComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::InteractableComponent::Enabled)
        .def_readwrite("Label", &Wasteland::InteractableComponent::Label);

// ---- Inventory ----
    py::class_<Wasteland::ItemStack>(m, "ItemStack")
        .def(py::init<>())
        .def(py::init([](const std::string &itemId, int quantity) {
                 return Wasteland::ItemStack(itemId, quantity);
             }), py::arg("itemId"), py::arg("quantity") = 1)
        .def_readwrite("ItemId", &Wasteland::ItemStack::ItemId)
        .def_readwrite("Quantity", &Wasteland::ItemStack::Quantity)
        .def("IsValid", &Wasteland::ItemStack::IsValid)
        .def("__repr__", [](const Wasteland::ItemStack &s) {
                 std::stringstream ss;
                 ss << "ItemStack(" << s.ItemId << " x" << s.Quantity << ")";
                 return ss.str();
             });

py::class_<Wasteland::ItemDefinitionComponent>(m, "ItemDefinitionComponent")
        .def(py::init<>())
        .def_readwrite("Name", &Wasteland::ItemDefinitionComponent::Name)
        // ItemDefinitionComponent: the authored item data. MaxStack and Weight are
        // the two numbers gameplay code reads constantly -- stack limits and
        // inventory mass -- and both are plain writable fields.
        .def_readwrite("DisplayName", &Wasteland::ItemDefinitionComponent::DisplayName)
        .def_readwrite("Description", &Wasteland::ItemDefinitionComponent::Description)
        .def_readwrite("MaxStack", &Wasteland::ItemDefinitionComponent::MaxStack)
        .def_readwrite("Weight", &Wasteland::ItemDefinitionComponent::Weight)
        // Tags is a COPY-OUT list, like SkinnedMesh.Vertices.
        //
        // reference_internal does not rescue it: pybind's list_caster
        // materialises a fresh Python list for std::vector<std::string>,
        // so `def.Tags.append("ammo")` mutated a temporary and the C++
        // vector stayed empty. HasTag then returned False for a tag the
        // script had just added -- silent, and with no way to tell from the
        // failure that the write was the problem.
        // Tags is READONLY and returns a copy, so `def.Tags.append(x)` mutates a
        // temporary and does nothing. AddTag below is the supported mutation path,
        // which is why the tag methods exist at all.
        .def_property_readonly("Tags",
            [](const Wasteland::ItemDefinitionComponent &def) {
                return def.Tags;
            })
        // Explicit mutators, which do reach the real vector.
        // The ItemDefinition tag methods. AddTag/RemoveTag/HasTag/ClearTags form the
        // full set, and they are METHODS rather than a readwrite Tags field --
        // because the container is manipulated in place by the engine.
        // AddTag pushes without checking for a duplicate, so the container is a
        // LIST and HasTag can report True for a tag added twice. RemoveTag then
        // erases only the FIRST match -- a duplicate needs two calls to clear.
        .def("AddTag", [](Wasteland::ItemDefinitionComponent &def,
                          const std::string &tag) {
                 def.Tags.push_back(tag);
             }, py::arg("tag"))
        .def("RemoveTag", [](Wasteland::ItemDefinitionComponent &def,
                             const std::string &tag) {
                 for (auto it = def.Tags.begin(); it != def.Tags.end(); ++it)
                 {
                     // RemoveTag scans for the tag and erases the first match, returning early
                     // when found. A Tags SET means one erase is enough; against a multiset it
                     // would leave duplicates behind.
                     if (*it == tag)
                     {
                         def.Tags.erase(it);
                         return;
                     }
                 }
             }, py::arg("tag"))
        // ClearTags empties the set in place. A script that held a reference to the
        // previous Tags container would see it emptied rather than replaced -- the
        // difference matters only because the field is not exposed here.
        .def("ClearTags", [](Wasteland::ItemDefinitionComponent &def) {
                 def.Tags.clear();
             })
        .def("HasTag", &Wasteland::ItemDefinitionComponent::HasTag, py::arg("tag"));

// ---- Audio ----
    //
    // A submodule rather than root-level classes: audio has a lot of surface
    // (clip, mixer, voice, request) and none of it belongs next to Scene in a
    // flat namespace.
    auto audio = m.def_submodule("Audio", "Audio playback: clips, voices, mixing.");

// LoadResult as a six-valued enum rather than a bool. The distinction the
// script actually needs is between "the file is not there" and "the file
// is there and I cannot decode it" -- those need different fixes.
py::enum_<Wasteland::Audio::AudioClip::LoadResult>(audio, "LoadResult")
        .value("Success", Wasteland::Audio::AudioClip::LoadResult::Success)
        .value("FileNotFound", Wasteland::Audio::AudioClip::LoadResult::FileNotFound)
        .value("InvalidFormat", Wasteland::Audio::AudioClip::LoadResult::InvalidFormat)
        .value("UnsupportedEncoding", Wasteland::Audio::AudioClip::LoadResult::UnsupportedEncoding)
        .value("TruncatedFile", Wasteland::Audio::AudioClip::LoadResult::TruncatedFile)
        .value("EmptyClip", Wasteland::Audio::AudioClip::LoadResult::EmptyClip);

// EmptyClip is separated from TruncatedFile because a zero-length WAV is a
// common authoring accident (a stub exported by a tool) rather than a
// truncated upload.
// AudioSourceComponent: the authored counterpart to PlayRequest. Its fields
// mirror that struct exactly, which is what lets one component description
// be copied into a runtime request without translation.
py::class_<Wasteland::AudioSourceComponent>(m, "AudioSourceComponent")
        .def(py::init<>())
        .def_readwrite("ClipName", &Wasteland::AudioSourceComponent::ClipName)
        .def_readwrite("Volume", &Wasteland::AudioSourceComponent::Volume)
        .def_readwrite("Pitch", &Wasteland::AudioSourceComponent::Pitch)
        .def_readwrite("Loop", &Wasteland::AudioSourceComponent::Loop)
        .def_readwrite("Priority", &Wasteland::AudioSourceComponent::Priority)
        .def_readwrite("Spatial", &Wasteland::AudioSourceComponent::Spatial)
        .def_readwrite("PositionOffset", &Wasteland::AudioSourceComponent::PositionOffset)
        .def_readwrite("MinDistance", &Wasteland::AudioSourceComponent::MinDistance)
        .def_readwrite("MaxDistance", &Wasteland::AudioSourceComponent::MaxDistance)
        .def_readwrite("PlayOnStart", &Wasteland::AudioSourceComponent::PlayOnStart);

py::class_<Wasteland::AudioListenerComponent>(m, "AudioListenerComponent")
        .def(py::init<>())
        .def_readwrite("Enabled", &Wasteland::AudioListenerComponent::Enabled);

// AudioClip with a shared_ptr holder. Clips are expensive to decode and are
// shared between every voice that plays them, so a copy per playback would
// be the single largest audio cost in the engine.
py::class_<Wasteland::Audio::AudioClip, std::shared_ptr<Wasteland::Audio::AudioClip>>(audio, "AudioClip")
        // LoadWav is a static factory returning a shared_ptr. The out-parameter for
        // the result is filled in by the C++ side and returned as a tuple element.
        .def_static("LoadWav", [](const std::string &path) {
                 // Returns (clip, result) rather than clip-or-None, so a script
                 // can tell "file missing" from "unsupported encoding". A
                 // silent None makes a missing sound indistinguishable from a
                 // typo in the path.
                 Wasteland::Audio::AudioClip::LoadResult result =
                     Wasteland::Audio::AudioClip::LoadResult::Success;
                 auto clip = Wasteland::Audio::AudioClip::LoadWav(path, &result);
                 return py::make_tuple(clip, result);
             }, py::arg("path"))
        // FromSamples builds a clip from raw float data -- the synthesis path. A
        // script generating a tone or a procedural sound goes through here rather
        // than writing a WAV to disk.
        .def_static("FromSamples", &Wasteland::Audio::AudioClip::FromSamples,
                    py::arg("samples"), py::arg("sampleRate") = 44100)
        .def("IsValid", &Wasteland::Audio::AudioClip::IsValid)
        .def("GetName", &Wasteland::Audio::AudioClip::GetName)
        .def("SetName", &Wasteland::Audio::AudioClip::SetName, py::arg("name"))
        .def("GetSampleRate", &Wasteland::Audio::AudioClip::GetSampleRate)
        .def("GetChannels", &Wasteland::Audio::AudioClip::GetChannels)
        .def("GetFrameCount", &Wasteland::Audio::AudioClip::GetFrameCount)
        .def("GetDuration", &Wasteland::Audio::AudioClip::GetDuration)
        .def("GetPeak", &Wasteland::Audio::AudioClip::GetPeak)
        .def("SaveWav", &Wasteland::Audio::AudioClip::SaveWav,
             py::arg("filepath"));

// PlayRequest is the mutable description of "play this". It is a value
// type, and every field mirrors a field on AudioSourceComponent: the
// component is the authored form, this is the runtime form.
py::class_<Wasteland::Audio::PlayRequest>(audio, "PlayRequest")
        .def(py::init<>())
        .def_readwrite("ClipName", &Wasteland::Audio::PlayRequest::ClipName)
        // The field list is identical to AudioSourceComponent's apart from Position
        // being a world Vec3 here rather than an offset. That is the whole difference
        // between configuring a component and issuing a one-shot request.
        .def_readwrite("Volume", &Wasteland::Audio::PlayRequest::Volume)
        .def_readwrite("Pitch", &Wasteland::Audio::PlayRequest::Pitch)
        .def_readwrite("Loop", &Wasteland::Audio::PlayRequest::Loop)
        .def_readwrite("Priority", &Wasteland::Audio::PlayRequest::Priority)
        .def_readwrite("Spatial", &Wasteland::Audio::PlayRequest::Spatial)
        .def_readwrite("Position", &Wasteland::Audio::PlayRequest::Position)
        .def_readwrite("MinDistance", &Wasteland::Audio::PlayRequest::MinDistance)
        .def_readwrite("MaxDistance", &Wasteland::Audio::PlayRequest::MaxDistance);

// Voice is a READ-ONLY view of one playing sound, plus IsPlaying. Every
// field is def_readonly, so a script can watch a voice's volume and pitch
// evolve but cannot steer it -- steering goes through Mixer.SetVoiceVolume.
py::class_<Wasteland::Audio::Voice>(audio, "Voice")
        .def(py::init<>())
        .def_readonly("VoiceId", &Wasteland::Audio::Voice::VoiceId)
        .def_readonly("Volume", &Wasteland::Audio::Voice::Volume)
        .def_readonly("Pitch", &Wasteland::Audio::Voice::Pitch)
        .def_readonly("Loop", &Wasteland::Audio::Voice::Loop)
        .def_readonly("Priority", &Wasteland::Audio::Voice::Priority)
        .def_readonly("Spatial", &Wasteland::Audio::Voice::Spatial)
        .def_readonly("Position", &Wasteland::Audio::Voice::Position)
        // FramePosition is the playhead in FRAMES, not seconds. A script wanting
        // progress has to divide by the clip's sample rate, which GetSampleRate
        // provides but which is easy to forget.
        .def_readonly("FramePosition", &Wasteland::Audio::Voice::FramePosition)
        .def("IsPlaying", &Wasteland::Audio::Voice::IsPlaying);

// SpatialGains is the stereo result of a panning computation: Left and Right
// gains plus an overall attenuation. Exposing it separately from Voice lets a
// script test a panning hypothesis without playing anything.
py::class_<Wasteland::Audio::SpatialGains>(audio, "SpatialGains")
        .def(py::init<>())
        .def_readonly("Left", &Wasteland::Audio::SpatialGains::Left)
        .def_readonly("Right", &Wasteland::Audio::SpatialGains::Right)
        .def_readonly("Attenuation", &Wasteland::Audio::SpatialGains::Attenuation);

// Mixer with a shared_ptr holder -- it is reachable both directly here and
// through AudioSystem.GetMixer below, and the holder must match both paths.
// Mixer registration group. RegisterClip/HasClip/GetClip/GetClipNames form
// the clip-registry API: clips are NAMED, so a script plays a sound by
// string and the mixer owns the lifetime of the decoded data.
py::class_<Wasteland::Audio::Mixer, std::shared_ptr<Wasteland::Audio::Mixer>>(audio, "Mixer")
        .def(py::init<>())
        // GetClipNames walks the registry map and collects just the keys. The
        // indirect step exists because the map is a C++ type with no caster.
        // Two constructors: the default one, and one taking an explicit voice
        // budget. The budget is the hard cap on simultaneous voices, so it is a
        // latency/quality trade a script may legitimately want to set.
        // The second constructor is a LAMBDA because it returns a shared_ptr built
        // with make_shared -- pybind cannot construct that directly from a raw
        // constructor, so the factory has to be written out.
        .def(py::init([](int maxVoices) { return std::make_shared<Wasteland::Audio::Mixer>(maxVoices); }),
             py::arg("maxVoices"))
        .def("RegisterClip", &Wasteland::Audio::Mixer::RegisterClip,
             py::arg("name"), py::arg("clip"))
        .def("HasClip", &Wasteland::Audio::Mixer::HasClip, py::arg("name"))
        // GetClip returns reference_internal rather than a copy or a transfer.
        // reference_internal ties the clip's lifetime to the MIXER's, which is the
        // correct choice here: the mixer owns the decoded buffer and outlives any
        // single voice.
        .def("GetClip", &Wasteland::Audio::Mixer::GetClip, py::arg("name"),
             py::return_value_policy::reference_internal)
        // SetVoiceVolume and SetVoicePitch address a voice BY ID, not by object.
        // There is no way to hold a Voice and pass it back -- hence the id-based
        // setter/getter pair everywhere below.
        // GetClipNames walks the map and collects keys into a fresh vector. This is
        // the indirect step mentioned above -- the map has no caster, so the lambda
        // exists purely to project it into something pybind can convert.
        .def("GetClipNames", [](const Wasteland::Audio::Mixer &mixer) {
            std::vector<std::string> names;
            for (const auto &kv : mixer.GetClips())
                names.push_back(kv.first);
            return names;
        })
        // Play takes a PlayRequest BY REFERENCE, so the request object is not
        // copied. That is what allows the mixer to store pointers into it -- and
        // means a script that mutates the request while it is playing changes
        // playback, which may or may not be intended.
        .def("Play", &Wasteland::Audio::Mixer::Play, py::arg("request"))
        .def("Stop", &Wasteland::Audio::Mixer::Stop, py::arg("voiceId"),
             py::arg("fadeSeconds") = 0.0f)
        .def("StopAll", &Wasteland::Audio::Mixer::StopAll, py::arg("fadeSeconds") = 0.0f)
        .def("SetVoiceVolume", &Wasteland::Audio::Mixer::SetVoiceVolume,
             py::arg("voiceId"), py::arg("volume"))
        .def("SetVoicePitch", &Wasteland::Audio::Mixer::SetVoicePitch,
             py::arg("voiceId"), py::arg("pitch"))
        .def("IsPlaying", &Wasteland::Audio::Mixer::IsPlaying, py::arg("voiceId"))
        .def("GetActiveVoiceCount", &Wasteland::Audio::Mixer::GetActiveVoiceCount)
        .def("GetMaxVoices", &Wasteland::Audio::Mixer::GetMaxVoices)
        .def("SetMaxVoices", &Wasteland::Audio::Mixer::SetMaxVoices, py::arg("max"))
        .def("SetMasterVolume", &Wasteland::Audio::Mixer::SetMasterVolume, py::arg("volume"))
        .def("GetMasterVolume", &Wasteland::Audio::Mixer::GetMasterVolume)
        // Mix and RETURN the buffer.
        //
        // Taking `std::vector<float>&` and mutating it looked right and
        // silently did nothing: pybind's list_caster for std::vector hands
        // C++ a converted COPY, and without a reference_internal return policy
        // the caller's list is never written back. Every mix produced zeros
        // with no error. Returning the vector removes the copy entirely, and
        // returning a fresh buffer is also what a device wants -- it fills a
        // block it then hands to the driver, it does not own a persistent
        // Python list.
        // Mix allocates frameCount*2 floats -- a hardcoded stereo assumption. The
        // Mixer's channel count is not queried, so a mono configuration would still
        // allocate and fill two channels. Returning the buffer rather than filling a
        // caller's list is deliberate; see the note above.
        .def("Mix", [](Wasteland::Audio::Mixer &mixer, int frameCount) {
                 std::vector<float> buffer((size_t)frameCount * 2, 0.0f);
                 mixer.Mix(buffer.data(), frameCount);
                 return buffer;
             }, py::arg("frameCount"))
        // Update advances the audio clock. It is separate from the scene tick
        // because audio time and simulation time genuinely differ -- sound keeps
        // playing while the game is paused.
        .def("Update", &Wasteland::Audio::Mixer::Update, py::arg("seconds"))
        // SetListenerPosition writes Position and then calls RecomputeBasis. The
        // second call is not optional: the mixer's spatial maths uses a cached basis
        // derived from position and orientation, and skipping it leaves panning
        // computed for the old listener.
        .def("SetListenerPosition", [](Wasteland::Audio::Mixer &mixer, const glm::vec3 &p) {
                 mixer.GetListener().Position = p;
                 mixer.GetListener().RecomputeBasis();
             }, py::arg("position"))
        // SetListenerOrientation takes forward and up, with up defaulting to world
        // +Y. It writes both fields then recomputes the basis for the same reason.
        // A degenerate forward (parallel to up) is not validated here.
        .def("SetListenerOrientation", [](Wasteland::Audio::Mixer &mixer,
                                          const glm::vec3 &forward, const glm::vec3 &up) {
                 mixer.GetListener().Forward = forward;
                 mixer.GetListener().Up = up;
                 mixer.GetListener().RecomputeBasis();
             }, py::arg("forward"), py::arg("up") = glm::vec3(0.0f, 1.0f, 0.0f))
        .def("GetListenerPosition", [](const Wasteland::Audio::Mixer &mixer) {
                 return mixer.GetListener().Position;
             })
        // ComputeSpatial is the panning primitive as a method, and
        // ComputeAttenuation below as a static. The static form matters: it lets a
        // script ask "how loud would this be at distance d" without a Mixer, which
        // is what a debug overlay wants.
        .def("ComputeSpatial", &Wasteland::Audio::Mixer::ComputeSpatial,
             py::arg("position"), py::arg("spatial") = true,
             py::arg("minDistance") = 1.0f, py::arg("maxDistance") = 30.0f)
        .def_static("ComputeAttenuation", &Wasteland::Audio::Mixer::ComputeAttenuation,
                    py::arg("distance"), py::arg("minDistance"), py::arg("maxDistance"))
        // GetVoices returns a reference_internal vector -- a live view of the
        // voice table. Because the mixer owns the storage, any operation that starts
        // or stops a voice can invalidate iterators over this list.
        .def("GetVoices", &Wasteland::Audio::Mixer::GetVoices,
             py::return_value_policy::reference_internal)
        .def("GetDroppedVoiceCount", &Wasteland::Audio::Mixer::GetDroppedVoiceCount)
        // DroppedVoiceCount and StealedVoiceCount are the two distinct overload
        // failures: DROPPED means the request never started, STOLEN means it took
        // over from another voice. Which one rises tells you whether to raise the
        // voice budget or lower the simultaneous-sound count.
        .def("GetStealedVoiceCount", &Wasteland::Audio::Mixer::GetStealedVoiceCount)
        .def("ResetCounters", &Wasteland::Audio::Mixer::ResetCounters);

// The scene-facing layer. Exposed so a script can reach the mixer
// (SetListenerPosition, custom PlayRequests, voice inspection) without
// going through the Scene wrappers, which only cover the common cases.
// AudioSystem has NO default constructor, deliberately: it holds a Scene
// reference and a standalone instance would have no entity positions to
// resolve against. Instances come from Scene.GetAudio().
py::class_<Wasteland::Audio::AudioSystem, std::shared_ptr<Wasteland::Audio::AudioSystem>>(
    audio, "AudioSystem")
        // No default constructor: AudioSystem holds a Scene reference, so a
        // standalone instance would have nothing to resolve entity positions
        // against. Instances come from Scene.GetAudio().
        .def("LoadClip", &Wasteland::Audio::AudioSystem::LoadClip, py::arg("path"))
    .def("Play", &Wasteland::Audio::AudioSystem::Play, py::arg("clipName"))
    .def("PlayAt", &Wasteland::Audio::AudioSystem::PlayAt,
         py::arg("clipName"), py::arg("position"),
         // PlayAutoStartSources is the per-frame entry point the engine itself calls
         // for components with PlayOnStart set. Exposed so a script driving its own
         // loop does not have to discover and start each source by hand.
         py::arg("volume") = 1.0f, py::arg("pitch") = 1.0f)
    .def("PlayEntity", &Wasteland::Audio::AudioSystem::PlayEntity, py::arg("entity"))
    // The AudioSystem control block: clip stopping, volume, and the per-frame
    // update. StopClip and StopAll both take fadeSeconds defaulting to 0, so the
    // default is a HARD CUT -- a script that wants a fade must pass one
    // explicitly.
    .def("StopClip", &Wasteland::Audio::AudioSystem::StopClip,
         py::arg("clipName"), py::arg("fadeSeconds") = 0.0f)
    .def("StopAll", &Wasteland::Audio::AudioSystem::StopAll,
         py::arg("fadeSeconds") = 0.0f)
    // SetClipVolume is per-clip and SetMasterVolume is global, and they are
    // independent: raising the master does not raise a clip that has been
    // turned down, so a script can permanently duck one sound without touching
    // the global level.
    .def("SetClipVolume", &Wasteland::Audio::AudioSystem::SetClipVolume,
         py::arg("clipName"), py::arg("volume"))
    .def("SetMasterVolume", &Wasteland::Audio::AudioSystem::SetMasterVolume,
         py::arg("volume"))
    .def("GetMasterVolume", &Wasteland::Audio::AudioSystem::GetMasterVolume)
    // PlayAutoStartSources is what the engine calls each frame for components
    // flagged PlayOnStart. Exposing it means a script driving its own loop can
    // start sources without finding each component itself.
    .def("PlayAutoStartSources", &Wasteland::Audio::AudioSystem::PlayAutoStartSources)
    .def("Update", &Wasteland::Audio::AudioSystem::Update, py::arg("seconds"))
    // GetListenerEntity returns the entity the audio listener is attached to.
    // The mixer's spatial maths is relative to that entity, so moving it moves
    // the whole soundscape -- which is why panning follows the camera without
    // any per-frame update.
    .def("GetListenerEntity", &Wasteland::Audio::AudioSystem::GetListenerEntity)
    // GetMixer is overloaded (Mixer& and const Mixer&), which a member-pointer
    // cannot disambiguate -- and a pointer-to-member-of-reference is illegal.
    // A lambda sidesteps both and states the intent: Python always gets the
    // mutable mixer.
    // GetMixer via lambda because the C++ method is overloaded and a pointer to
    // a reference-returning overload is not a thing. reference_internal ties the
    // mixer's lifetime to the AudioSystem that owns it.
    // GetMixer returns a Mixer& with reference_internal, tying the mixer's
    // lifetime to the AudioSystem. Without the policy pybind's default for a
    // reference would be to copy -- and a copied mixer would have an empty voice
    // table, so every Play against it would silently do nothing.
    .def("GetMixer", [](Wasteland::Audio::AudioSystem &sys) -> Wasteland::Audio::Mixer & {
        return sys.GetMixer();
    }, py::return_value_policy::reference_internal)
    // ResolveSourcePosition is static and takes BOTH scene and entity plus an
    // out-parameter -- it is a free function on AudioSystem, not a method, and
    // the scene argument exists precisely because it is static.
    // ResolveSourcePosition is def_static and takes a scene argument BECAUSE it
    // is static -- it has no AudioSystem instance to read the scene from. The
    // out-parameter is written in place, so the caller passes a value and reads
    // it back rather than receiving a return.
    .def_static("ResolveSourcePosition",
               &Wasteland::Audio::AudioSystem::ResolveSourcePosition,
               py::arg("scene"), py::arg("entity"), py::arg("outPosition"));


// PersistentComponent has exactly one field: StreamingRadius, the distance
// at which the entity's data stops being kept resident. It is the whole
// component, which is why it needs no methods.
py::class_<Wasteland::PersistentComponent>(m, "PersistentComponent")
        .def(py::init<>())
        .def_readwrite("StreamingRadius", &Wasteland::PersistentComponent::StreamingRadius);

// QuestFlagsComponent: a string-keyed flag map with Has/Get/Set/Clear and
// two conveniences -- __len__ and Keys. Get returns by reference, so the
// value handed back is the live one.
py::class_<Wasteland::QuestFlagsComponent>(m, "QuestFlagsComponent")
        .def(py::init<>())
        .def("Has", &Wasteland::QuestFlagsComponent::Has, py::arg("flag"))
        // return_value_policy::reference on Get means a script can write through
        // the returned reference rather than only reading it, which is an implicit
        // Set. There is also a real Set below, so the map has two mutation paths.
        .def("Get", &Wasteland::QuestFlagsComponent::Get, py::arg("flag"),
             py::return_value_policy::reference)
        .def("Set", &Wasteland::QuestFlagsComponent::Set, py::arg("flag"), py::arg("value"))
        .def("Clear", &Wasteland::QuestFlagsComponent::Clear, py::arg("flag"))
        // __len__ and Keys make the component usable with Python's protocol:
        // `if len(flags):` asks whether any flag is set, and Keys enumerates them.
        // That is why this component is not exposed as a bare def_readwrite map --
        // the methods make it a small mapping-like object.
        .def("__len__", [](const Wasteland::QuestFlagsComponent &qf) { return qf.Flags.size(); })
        .def("Keys", [](const Wasteland::QuestFlagsComponent &qf) {
            std::vector<std::string> keys;
            for (const auto &kv : qf.Flags)
                keys.push_back(kv.first);
            return keys;
        });

// WorldTimerComponent is a named-timer map: Get and Set by name, no
// enumeration. A script that needs to list timers has no way to -- the
// asymmetry with QuestFlags above is a binding-surface gap.
py::class_<Wasteland::WorldTimerComponent>(m, "WorldTimerComponent")
        .def(py::init<>())
        .def("Get", &Wasteland::WorldTimerComponent::Get, py::arg("name"))
        .def("Set", &Wasteland::WorldTimerComponent::Set, py::arg("name"), py::arg("value"));

// DroppedItemComponent is what Scene::DropItemAt puts on the pile it
// creates. Bound so a script can inspect a dropped stack -- and,
// critically, so it can be ATTACHED: see the BIND_GAMEPLAY_COMP line
// below, which is what makes a pile placeable in a scene file by hand
// rather than only producible at runtime.
//
// A two-argument init mirrors ItemStack's, so "one shell" reads as
// DroppedItemComponent("shell", 1) rather than needing two attribute
// writes on a fresh object.
py::class_<Wasteland::DroppedItemComponent>(m, "DroppedItemComponent")
        .def(py::init<>())
        .def(py::init<const std::string &, int>(),
             py::arg("itemId"), py::arg("quantity") = 1)
        .def_readwrite("ItemId", &Wasteland::DroppedItemComponent::ItemId)
        .def_readwrite("Quantity", &Wasteland::DroppedItemComponent::Quantity)
        // IsValid is the same gate the serializer uses in both directions,
        // exposed so a script can ask "would this pile survive a save?"
        // without reimplementing the rule.
        .def("IsValid", &Wasteland::DroppedItemComponent::IsValid)
        .def("__repr__", [](const Wasteland::DroppedItemComponent &pile) {
            std::stringstream ss;
            ss << "DroppedItemComponent(" << pile.ItemId << " x" << pile.Quantity << ")";
            return ss.str();
        });

// InventoryComponent wraps a vector of ItemStack. Items is
// def_property_readonly, so reading it COPIES the vector -- appending to the
// returned list does not change the inventory. SetItems is the only way to
// write the whole thing.
py::class_<Wasteland::InventoryComponent>(m, "InventoryComponent")
        .def(py::init<>())
        // Items is a COPY-OUT list for the same reason as Tags: pybind's
        // list_caster builds a new Python list, so `inv.Items.append(...)`
        // mutated a temporary. Reading is what this is for -- use the Scene
        // helpers (AddItem/RemoveItem) to change contents, which is the path
        // that enforces stacking and capacity.
        .def_property_readonly("Items",
            [](const Wasteland::InventoryComponent &inv) {
                return inv.Items;
            })
        // Direct replacement, for save loading and scripted setup where the
        // caller already knows the exact contents.
        .def("SetItems", [](Wasteland::InventoryComponent &inv,
                            const std::vector<Wasteland::ItemStack> &items) {
                 inv.Items = items;
             }, py::arg("items"))
        .def_readwrite("MaxWeight", &Wasteland::InventoryComponent::MaxWeight)
        .def_readwrite("MaxSlots", &Wasteland::InventoryComponent::MaxSlots)
        .def("IsFull", &Wasteland::InventoryComponent::IsFull)
        .def("GetStackCount", &Wasteland::InventoryComponent::GetStackCount);

// Presentation state. The weapon script writes the timers; the HUD reads
    // them. Exposed with a Tick() so the countdown lives in one place rather
    // than being reimplemented in every script that happens to hold the
    // component.
    py::class_<Wasteland::WeaponFeedbackComponent>(m, "WeaponFeedbackComponent")
        .def(py::init<>())
        .def_readwrite("MuzzleFlashTimer", &Wasteland::WeaponFeedbackComponent::MuzzleFlashTimer)
        .def_readwrite("HitMarkerTimer", &Wasteland::WeaponFeedbackComponent::HitMarkerTimer)
        .def_readwrite("LastShotHit", &Wasteland::WeaponFeedbackComponent::LastShotHit)
        .def_readwrite("LastShotKilled", &Wasteland::WeaponFeedbackComponent::LastShotKilled)
        .def_readwrite("MuzzleFlashDuration", &Wasteland::WeaponFeedbackComponent::MuzzleFlashDuration)
        .def_readwrite("HitMarkerDuration", &Wasteland::WeaponFeedbackComponent::HitMarkerDuration)
        .def_readwrite("ReloadProgress", &Wasteland::WeaponFeedbackComponent::ReloadProgress)
        .def_readwrite("ReloadTotal", &Wasteland::WeaponFeedbackComponent::ReloadTotal)
        // Arms the flash. Separate from the field write so "start flashing"
        // is one call that always uses the component's own duration.
        .def("TriggerMuzzleFlash",
             [](Wasteland::WeaponFeedbackComponent &f) {
                 f.MuzzleFlashTimer = f.MuzzleFlashDuration;
             })
        .def("TriggerHitMarker",
             [](Wasteland::WeaponFeedbackComponent &f, bool killed) {
                 f.HitMarkerTimer = f.HitMarkerDuration;
                 f.LastShotHit = true;
                 f.LastShotKilled = killed;
             })
        .def("ClearHitMarker",
             [](Wasteland::WeaponFeedbackComponent &f) {
                 f.HitMarkerTimer = 0.0f;
                 f.LastShotHit = false;
                 f.LastShotKilled = false;
             })
        .def("Tick", [](Wasteland::WeaponFeedbackComponent &f, float seconds) {
             // Clamp at zero: a long frame must not drive the timers negative,
             // which would read as "not showing" but is a lie about state and
             // can make a later comparison flip the wrong way.
             f.MuzzleFlashTimer = std::max(0.0f, f.MuzzleFlashTimer - seconds);
             f.HitMarkerTimer = std::max(0.0f, f.HitMarkerTimer - seconds);
         });

py::class_<Wasteland::ViewStateComponent>(m, "ViewStateComponent")
        .def(py::init<>())
        .def_readwrite("TargetFOV", &Wasteland::ViewStateComponent::TargetFOV)
        .def_readwrite("FOVLerpSpeed", &Wasteland::ViewStateComponent::FOVLerpSpeed)
        .def_readwrite("IsSprinting", &Wasteland::ViewStateComponent::IsSprinting)
        .def_readwrite("IsAiming", &Wasteland::ViewStateComponent::IsAiming);

// ======================================== SCENE: Entity ================
    // Trace records are immutable snapshots; only BehaviorTreeSystem can write
    // them, while Python receives copied values from Entity.GetBehaviorTreeTrace.
    py::class_<Wasteland::BehaviorTreeTraceEntry>(m, "BehaviorTreeTraceEntry")
        .def_readonly("Tick", &Wasteland::BehaviorTreeTraceEntry::Tick)
        .def_readonly("NodeId", &Wasteland::BehaviorTreeTraceEntry::NodeId)
        .def_readonly("NodeType", &Wasteland::BehaviorTreeTraceEntry::NodeType)
        .def_readonly("Event", &Wasteland::BehaviorTreeTraceEntry::Event);

    // Entity is the handle a gameplay script spends its whole life holding. It
    // is a thin pair of (entt handle, Scene pointer) -- not an owning reference
    // -- so copying one is free and a destroyed entity leaves a dangling Scene
    // pointer rather than a safe empty.
    py::class_<Wasteland::Entity>(m, "Entity")
        .def(py::init<>())
        // py::init<>() default-constructs an INVALID entity. That is the one thing
        // scripts must never do and the binding permits: a default Entity has no
        // handle, and every method on it asserts.
        .def("IsValid", [](const Wasteland::Entity &e) { return e.IsValid(); })
        .def("GetUUID", [](Wasteland::Entity &e) { return e.GetUUID(); })
        .def("GetName", [](Wasteland::Entity &e) { return e.GetName(); })
        // Trace access is identity-checked and component-checked before GetUUID:
        // Entity::GetUUID asserts if IDComponent is absent. The log is a bounded
        // copy, never a reference into BehaviorTreeSystem's per-entity storage.
        .def("SetBehaviorTreeTrace",
             [](Wasteland::Entity &e, bool enabled) {
                 if (!e.IsValid() ||
                     !e.HasComponent<Wasteland::BehaviorTreeComponent>() ||
                     !e.HasComponent<Wasteland::IDComponent>())
                     return false;
                 return Wasteland::BehaviorTreeSystem::SetTrace(
                     e.GetComponent<Wasteland::IDComponent>().ID, enabled);
             }, py::arg("enabled"))
        .def("IsBehaviorTreeTraceEnabled",
             [](Wasteland::Entity &e) {
                 if (!e.IsValid() ||
                     !e.HasComponent<Wasteland::BehaviorTreeComponent>() ||
                     !e.HasComponent<Wasteland::IDComponent>())
                     return false;
                 return Wasteland::BehaviorTreeSystem::IsTraceEnabled(
                     e.GetComponent<Wasteland::IDComponent>().ID);
             })
        .def("GetBehaviorTreeTrace",
             [](Wasteland::Entity &e) {
                 if (!e.IsValid() ||
                     !e.HasComponent<Wasteland::BehaviorTreeComponent>() ||
                     !e.HasComponent<Wasteland::IDComponent>())
                     return std::vector<Wasteland::BehaviorTreeTraceEntry>();
                 return Wasteland::BehaviorTreeSystem::GetTraceLog(
                     e.GetComponent<Wasteland::IDComponent>().ID);
             })

// GetUUID requires an IDComponent. Entity::GetUUID does GetComponent<ID>
// internally, so an entity without one asserts in debug rather than
// returning a zero UUID.
// Calls a method on this entity's live Python script instance, if it
        // has one and the method exists. This is the dispatch path gameplay
        // scripts use to act on another entity -- an Interactor finding a
        // door cannot call door.OnInteract() directly, because the entity it
        // received from a raycast is a C++ Entity wrapper, not the Python
        // object the ScriptEngine constructed.
        //
        // Returns True when the method was found and called, False otherwise,
        // so a caller can distinguish "no such method" from "method ran and
        // returned something falsy". A missing instance or method is not an
        // error: most entities in a scene have no script at all.
        // CallScriptMethod is the cross-entity dispatch path. An Interactor that
        // raycasts a door receives a C++ Entity wrapper, not the Python object the
        // ScriptEngine built for that door -- so door.OnInteract() is not reachable
        // directly and this is the substitute.
        .def("CallScriptMethod",
             // py::args means "swallow the rest". Without it pybind would count the
             // declared annotations against the lambda's parameters and reject the
             // binding.
             [](Wasteland::Entity &e, const std::string &method, py::args args) -> bool {
                 py::object *instance = Wasteland::ScriptEngine::GetInstance(e.GetUUID());
                 if (!instance)
                     return false;

// NOTE: the gil_scoped_acquire here is REDUNDANT. pybind already holds the
// GIL for the whole duration of a bound function -- it acquires before
// calling this lambda and releases after. Acquiring again is a no-op
// refcount bump, not a bug, but it implies the GIL was not held, which
// would matter if this path were ever reached from C++ directly.
py::gil_scoped_acquire gil;
                 // getattr with a default: a missing method yields None
                 // instead of raising AttributeError, so an entity with no
                 // script (or a script that simply does not implement this
                 // hook) is a quiet no-op rather than an exception.
                 // NOTE: the gil_scoped_acquire below is redundant. pybind already holds
                 // the GIL for the duration of a bound function -- it acquires it before
                 // calling the lambda and releases it after. Acquiring again here is a no-op
                 // refcount bump, not a bug, but it implies the GIL was not held, which
                 // would matter if this code were ever called from C++ directly.
                 py::object fn = py::getattr(*instance, method.c_str(), py::none());
                 if (fn.is_none())
                     return false;
                 fn(*args);
                 return true;
             },
             py::arg("method"))

// Returns True/False rather than the method's return value. A script that
// wants the result must use GetScriptAttribute or read state afterwards.
// Returns one attribute off this entity's live script instance, or
        // None if the entity has no script or no such attribute. Lets a test
        // or a debug overlay inspect script-owned state without reaching
        // into the instance table directly.
        // GetScriptAttribute reads any attribute off the live instance -- not just
        // the declared API. That is the debugging hook: a script can inspect
        // another script's private state without that state being bound.
        .def("GetScriptAttribute",
             [](Wasteland::Entity &e, const std::string &attr) -> py::object {
                 py::object *instance = Wasteland::ScriptEngine::GetInstance(e.GetUUID());
                 if (!instance)
                     return py::none();
                 py::gil_scoped_acquire gil;
                 // The same redundant GIL acquire as CallScriptMethod, and here it is also
                 // placed AFTER the early return, so the two paths are inconsistent about it.
                 return py::getattr(*instance, attr.c_str(), py::none());
             },
             py::arg("attr"))

// ---- Transform convenience API ----
        // Scripts treat the transform as a mutable value object:
        //     t = entity.GetTransform(); t.Translation.x += 1
        //     entity.SetTransform(t.Translation, t.Rotation, t.Scale)
        // GetTransform returns a *copy* by value so mutating it is side-effect free
        // until SetTransform commits it (matching how the scripts are written).
        .def("GetTransform",
             [](Wasteland::Entity &e) -> Wasteland::TransformComponent {
                 return e.GetComponent<Wasteland::TransformComponent>();
             })
        .def("SetTransform",
             // The transform convenience API. GetTransform returns a COPY so mutating it
             // is side-effect free until SetTransform commits -- see the note above.
             [](Wasteland::Entity &e, const glm::vec3 &translation,
                const glm::vec3 &rotation, const glm::vec3 &scale) {
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Translation = translation;
                 tc.Rotation = rotation;
                 tc.Scale = scale;
             // SetTransform writes the three fields directly. It duplicates
             // SetTransformTrs almost exactly; the only difference is that SetTransform
             // requires a rotation while SetTransformTrs defaults scale to 1.
             },
             py::arg("translation"), py::arg("rotation"),
             py::arg("scale") = glm::vec3(1.0f))
        // SetPosition / SetRotation write ONE field and leave the rest alone.
        //
        // GetTransform returns a copy on purpose, so `t = e.GetTransform();
        // t.Translation = v` silently discards the write -- the entity does
        // not move and nothing raises. Every shipped script avoids this with
        // the read-modify-SetTransform dance, which is correct but verbose
        // and easy to get wrong when only one axis changes.
        .def("SetPosition", [](Wasteland::Entity &e, const glm::vec3 &position) {
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Translation = position;
             }, py::arg("position"))
        // SetPositionY exists because "change only the height" is the single most
        // common transform write in gameplay (jumping, falling, hover). Doing it
        // with a full Get/Set round trip is three lines where one will do.
        // The transform convenience API: six one-line lambdas. Each gets the
        // TransformComponent and writes or reads ONE field. They exist so ordinary
        // movement code is `e.SetPosition(p)` rather than three lines of component
        // access -- at the cost of six more bound methods that all do the same
        // get-component dance.
        .def("SetPositionY", [](Wasteland::Entity &e, float y) {
                 // GetComponent<> ASSERTS if the component is absent. So every one of these
                 // six lambdas requires the entity to have a TransformComponent; calling
                 // SetPosition on an entity without one aborts in debug and is undefined in
                 // release. The typed Get/Add accessors earlier give a script a way to check
                 // first; these convenience wrappers do not.
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Translation.y = y;
             }, py::arg("y"))
        .def("SetRotation", [](Wasteland::Entity &e, const glm::vec3 &rotation) {
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Rotation = rotation;
             }, py::arg("rotation"))
        .def("SetScale", [](Wasteland::Entity &e, const glm::vec3 &scale) {
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Scale = scale;
             }, py::arg("scale"))
        // The three getters return by VALUE -- a vec3 copy -- unlike the component
        // properties above, which return references. That is the right choice for
        // these (the caller cannot meaningfully mutate the entity's position through
        // a returned copy) and is why `e.GetPosition().x = 5` silently does nothing
        // while the component-property equivalent works.
        .def("GetPosition", [](Wasteland::Entity &e) {
                 return e.GetComponent<Wasteland::TransformComponent>().Translation;
             })
        .def("GetRotation", [](Wasteland::Entity &e) {
                 return e.GetComponent<Wasteland::TransformComponent>().Rotation;
             })
        .def("GetScale", [](Wasteland::Entity &e) {
                 return e.GetComponent<Wasteland::TransformComponent>().Scale;
             })
        .def("SetTransformTrs",
             [](Wasteland::Entity &e, const glm::vec3 &translation,
                const glm::vec3 &rotation, const glm::vec3 &scale) {
                 // GetTransformMatrix returns a glm::mat4 -- which, per the Mat4 binding,
                 // a script can barely do anything with beyond indexing. The value is
                 // useful mostly for passing straight back into a renderer call.
                 // Assign fields directly: TransformComponent::SetTransform takes a
                 // mat4 and decomposes it (lossy round-trip), which is not what a
                 // TRS assignment from a script means.
                 auto &tc = e.GetComponent<Wasteland::TransformComponent>();
                 tc.Translation = translation;
                 tc.Rotation = rotation;
                 tc.Scale = scale;
             },
             py::arg("translation"), py::arg("rotation"),
             py::arg("scale") = glm::vec3(1.0f))
        .def("GetTransformMatrix",
             [](Wasteland::Entity &e) {
                 return e.GetComponent<Wasteland::TransformComponent>().GetTransform();
             })

// By-name component lookup, so scripts can write
        // entity.GetComponent("MaterialComponent") without knowing C++ types.
        // NOTE: every branch must return the SAME type. Return a *valid* handle:
        // a default-constructed py::object is a NULL handle, and returning one
        // fails the conversion with "Unable to convert function return value to
        // a Python type!" — exactly the no-such-component case.
        // GetComponent(string) is the name-based lookup, so a script can say
        // entity.GetComponent("MaterialComponent") without importing a C++ type.
        // It is a LINEAR SCAN of hardcoded if-statements, not a registry -- so it
        // covers only the ~19 components listed below and silently returns None
        // for the other 40-odd that ARE bound above.
        .def("GetComponent",
             [](Wasteland::Entity &e, const std::string &name) -> py::object {
                 // That gap is a real limitation: the colliders, lights, volumetrics, water
                 // and mesh primitives all have py::class_ bindings and BIND_COMP_*
                 // accessors, but none of them appear in this name table. A script that
                 // looks one up by string gets None and has no way to tell "no such
                 // component" from "not implemented in this lookup".
                 if (name == "TransformComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::TransformComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 // Every branch returns the SAME type -- a py::object -- because pybind
                 // deduces the return type from the lambda. Mixing a raw component pointer
                 // and a py::object across branches would not compile.
                 // The lookup table, restated at the point where the repeated branches
                 // begin. Every branch is the same three lines; only the type in the
                 // TryGetComponent<> template argument changes. That template is what makes
                 // each branch a distinct component -- there is no runtime type registry.
                 if (name == "TagComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::TagComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "IDComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::IDComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "MaterialComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::MaterialComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "SpriteRendererComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::SpriteRendererComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "HealthComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::HealthComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "WeaponComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::WeaponComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 // RaycastIgnoreComponent and HittableComponent are adjacent because they are
                 // the two answers to "should this raycast result be returned?". A script
                 // filtering its own raycasts wants the first; a script implementing damage
                 // wants the second.
                 if (name == "RaycastIgnoreComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::RaycastIgnoreComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "HittableComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::HittableComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "InteractableComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::InteractableComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 // WeaponFeedback and ViewState: presentation state, not simulation state.
                 // They are here because a script needs to read them, but nothing in the
                 // engine's own logic reads them back.
                 if (name == "WeaponFeedbackComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::WeaponFeedbackComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "ViewStateComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::ViewStateComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "InventoryComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::InventoryComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "PersistentComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::PersistentComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 // QuestFlags and WorldTimer: the save-relevant components. Both are in this
                 // table, which is why GetComponent by name works for them -- and both are
                 // persisted, so mutating the returned reference changes what a save will
                 // record.
                 if (name == "QuestFlagsComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::QuestFlagsComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "WorldTimerComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::WorldTimerComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 if (name == "ItemDefinitionComponent")
                     if (auto *p = e.TryGetComponent<Wasteland::ItemDefinitionComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 // ScriptComponent closes the table. The reference returned here is LIVE:
                 // return_value_policy::reference means writes through it reach the actual
                 // component, which is what makes this lookup useful rather than a snapshot.
                 if (name == "ScriptComponent")
                     // The name-based component lookup, continued. Each branch is the same
                     // three-line shape: compare the string, TryGet the component, cast it by
                     // reference if present. There is no fallthrough -- a name that matches no
                     // branch reaches the single `return py::none()` at the end.
                     if (auto *p = e.TryGetComponent<Wasteland::ScriptComponent>())
                         return py::cast(*p, py::return_value_policy::reference);
                 return py::none();  // valid py::object wrapping None
             },
             py::arg("name"))
        // IsUnderwater on the ENTITY rather than the Scene: it resolves the water
        // MaterialComponent through InteractableComponent: the components a
        // gameplay script is most likely to probe for by name. The branches are in
        // no particular order and the order does not matter -- a script asking for
        // two different names cannot match two branches.
        // body relative to this entity, so two entities in the same scene can
        // disagree. margin is a tolerance band on the surface test.
        .def("IsUnderwater",
             [](Wasteland::Entity &e, const glm::vec3 &pos, float margin) {
                 return e.IsUnderwater(pos, margin);
             },
             py::arg("pos"), py::arg("margin") = 0.0f)
        // GetWaterHeightAt returns (hit, height, flow) -- three values because the
        // flow vector is needed for swimming current and cannot be recovered from
        // Weapon through WorldTimer: the mid-block. WeaponComponent and
        // HittableComponent are adjacent because a raycast hit is usually resolved
        // against both together -- hit an entity, check if it is hittable, then ask
        // its weapon what it did.
        // the height. A miss still returns a tuple, with height and flow zeroed.
        .def("GetWaterHeightAt",
             [](Wasteland::Entity &e, const glm::vec3 &pos) {
                 float height = 0.0f;
                 glm::vec2 flow(0.0f);
                 bool hit = e.GetWaterHeightAt(pos, height, &flow);
                 return py::make_tuple(hit, height, flow);
             },
             py::arg("pos"))
        // __bool__ makes an Entity usable in an if-statement: `if entity:` reads as
        // "is this still alive", which is the question a script asks constantly.
        .def("__bool__", [](const Wasteland::Entity &e) { return e.IsValid(); })
        // __eq__/__ne__ compare Entity by its entt handle, not by scene pointer.
        // Two wrappers onto the same handle in the same scene compare equal, which
        // is what a script wants when it checks "is this the entity I passed in".
        .def("__eq__",
             [](const Wasteland::Entity &a, const Wasteland::Entity &b) {
                 return a == b;
             // Inventory through ScriptComponent closes the table. The list is NOT the
             // set of bound components: roughly 20 more are bound above, including every
             // collider, light and mesh primitive, and none of those can be found here.
             // GetComponent("CubeRendererComponent") returns None on an entity that
             // definitely has one.
             })
        .def("__ne__",
             [](const Wasteland::Entity &a, const Wasteland::Entity &b) {
                 return a != b;
             })

// Per-component accessors
        // The per-component accessor block. Roughly 40 groups of six macro calls,
        // each expanding to Has/Get/TryGet/Add/AddOr/Remove. This is where an Entity
        // gets its typed surface.
        BIND_COMP_HAS(Wasteland::IDComponent)
        // IDComponent gets FIVE accessors, not six -- TryGet is omitted. The only
        // component in the block missing it, and the only one where it would make
        // sense: every valid entity has an ID, so probing for it is pointless.
        // IDComponent accessors. Add/AddOr let a script mint a NEW entity by
        // attaching an ID -- which is how an entity becomes addressable at all,
        // since GetUUID above depends on one.
        BIND_COMP_GET(Wasteland::IDComponent)
        // The single fallthrough return. `return py::none();` rather than
        // `return nullptr` -- pybind's nullptr would become None too, but the
        // explicit py::none() makes the intent visible and matches the py::object
        // return type deduced from the branches above.
        BIND_COMP_ADD(Wasteland::IDComponent)
        BIND_COMP_ADDOR(Wasteland::IDComponent)
        BIND_COMP_REM(Wasteland::IDComponent)

// TagComponent onward: all six. The group boundaries are purely visual --
// each macro expands into the same py::class_ chain, so these lines are
// continuation tokens rather than statements. A semicolon here would end
// the chain and silently drop every binding after it.
// TagComponent. Tag is the only field, so the six accessors are really six
// spellings of "does this entity have a name".
BIND_COMP_HAS(Wasteland::TagComponent)
        BIND_COMP_GET(Wasteland::TagComponent)
        BIND_COMP_TRYGET(Wasteland::TagComponent)
        BIND_COMP_ADD(Wasteland::TagComponent)
        BIND_COMP_ADDOR(Wasteland::TagComponent)
        BIND_COMP_REM(Wasteland::TagComponent)

// TransformComponent. Note the macro uses the FULL type name, which is why
// the generated Python method is GetWasteland::TransformComponent -- see
// the BIND_GAMEPLAY_COMP note at the end of this block.
BIND_COMP_HAS(Wasteland::TransformComponent)
        BIND_COMP_GET(Wasteland::TransformComponent)
        BIND_COMP_TRYGET(Wasteland::TransformComponent)
        BIND_COMP_ADD(Wasteland::TransformComponent)
        BIND_COMP_ADDOR(Wasteland::TransformComponent)
        BIND_COMP_REM(Wasteland::TransformComponent)

// The seven Blender mesh primitives. Identical accessor shape, so a script
// that handles one handles all seven -- which is the entire point of the
// macro existing.
BIND_COMP_HAS(Wasteland::SpriteRendererComponent)
        BIND_COMP_GET(Wasteland::SpriteRendererComponent)
        BIND_COMP_TRYGET(Wasteland::SpriteRendererComponent)
        BIND_COMP_ADD(Wasteland::SpriteRendererComponent)
        BIND_COMP_ADDOR(Wasteland::SpriteRendererComponent)
        BIND_COMP_REM(Wasteland::SpriteRendererComponent)

BIND_COMP_HAS(Wasteland::CircleRendererComponent)
        // The Blender primitives. Each has the same six accessors, which is what
        // makes a script generic over mesh types -- but the script still has to
        // know which six method names to call, since they are named per component.
        BIND_COMP_GET(Wasteland::CircleRendererComponent)
        BIND_COMP_TRYGET(Wasteland::CircleRendererComponent)
        BIND_COMP_ADD(Wasteland::CircleRendererComponent)
        BIND_COMP_ADDOR(Wasteland::CircleRendererComponent)
        BIND_COMP_REM(Wasteland::CircleRendererComponent)

BIND_COMP_HAS(Wasteland::CubeRendererComponent)
        BIND_COMP_GET(Wasteland::CubeRendererComponent)
        BIND_COMP_TRYGET(Wasteland::CubeRendererComponent)
        BIND_COMP_ADD(Wasteland::CubeRendererComponent)
        BIND_COMP_ADDOR(Wasteland::CubeRendererComponent)
        BIND_COMP_REM(Wasteland::CubeRendererComponent)

BIND_COMP_HAS(Wasteland::SphereRendererComponent)
        BIND_COMP_GET(Wasteland::SphereRendererComponent)
        BIND_COMP_TRYGET(Wasteland::SphereRendererComponent)
        BIND_COMP_ADD(Wasteland::SphereRendererComponent)
        BIND_COMP_ADDOR(Wasteland::SphereRendererComponent)
        // MaterialComponent. Has/Get/TryGet are the read path; Add/AddOr the write
        // path. TryGet differs from Get in returning None rather than asserting when
        // the component is absent.
        BIND_COMP_REM(Wasteland::SphereRendererComponent)

// Blender "Add > Mesh" primitives. No semicolons -- a trailing ';' would end
// the py::class_<Entity> chain and break every binding after it.
BIND_COMP_HAS(Wasteland::PlaneRendererComponent)
        BIND_COMP_GET(Wasteland::PlaneRendererComponent)
        BIND_COMP_TRYGET(Wasteland::PlaneRendererComponent)
        // CameraComponent, then the six colliders, then the lights. The visual
        // ordering of this block follows the authoring panel, not any dependency
        // between the components -- they do not reference each other.
        BIND_COMP_ADD(Wasteland::PlaneRendererComponent)
        BIND_COMP_ADDOR(Wasteland::PlaneRendererComponent)
        BIND_COMP_REM(Wasteland::PlaneRendererComponent)

BIND_COMP_HAS(Wasteland::GridRendererComponent)
        BIND_COMP_GET(Wasteland::GridRendererComponent)
        BIND_COMP_TRYGET(Wasteland::GridRendererComponent)
        BIND_COMP_ADD(Wasteland::GridRendererComponent)
        BIND_COMP_ADDOR(Wasteland::GridRendererComponent)
        BIND_COMP_REM(Wasteland::GridRendererComponent)

BIND_COMP_HAS(Wasteland::Circle3DRendererComponent)
        BIND_COMP_GET(Wasteland::Circle3DRendererComponent)
        BIND_COMP_TRYGET(Wasteland::Circle3DRendererComponent)
        BIND_COMP_ADD(Wasteland::Circle3DRendererComponent)
        BIND_COMP_ADDOR(Wasteland::Circle3DRendererComponent)
        BIND_COMP_REM(Wasteland::Circle3DRendererComponent)

BIND_COMP_HAS(Wasteland::IcoSphereRendererComponent)
        BIND_COMP_GET(Wasteland::IcoSphereRendererComponent)
        BIND_COMP_TRYGET(Wasteland::IcoSphereRendererComponent)
        BIND_COMP_ADD(Wasteland::IcoSphereRendererComponent)
        BIND_COMP_ADDOR(Wasteland::IcoSphereRendererComponent)
        // Rigidbody2D/Rigidbody3D. These are the only accessors that touch the
        // PHYSICS WORLD rather than just entity storage: AddOr on a body that is
        // already in a world mutates the live solver fixture, not a copy.
        BIND_COMP_REM(Wasteland::IcoSphereRendererComponent)

BIND_COMP_HAS(Wasteland::CylinderRendererComponent)
        BIND_COMP_GET(Wasteland::CylinderRendererComponent)
        BIND_COMP_TRYGET(Wasteland::CylinderRendererComponent)
        BIND_COMP_ADD(Wasteland::CylinderRendererComponent)
        BIND_COMP_ADDOR(Wasteland::CylinderRendererComponent)
        // The 3D colliders. Offset/HalfExtents are readwrite, so a script can resize
        // a collider at runtime -- the engine must rebuild the fixture, and the
        // binding does not say whether it does.
        BIND_COMP_REM(Wasteland::CylinderRendererComponent)

BIND_COMP_HAS(Wasteland::ConeRendererComponent)
        BIND_COMP_GET(Wasteland::ConeRendererComponent)
        BIND_COMP_TRYGET(Wasteland::ConeRendererComponent)
        BIND_COMP_ADD(Wasteland::ConeRendererComponent)
        BIND_COMP_ADDOR(Wasteland::ConeRendererComponent)
        BIND_COMP_REM(Wasteland::ConeRendererComponent)

BIND_COMP_HAS(Wasteland::TorusRendererComponent)
        BIND_COMP_GET(Wasteland::TorusRendererComponent)
        BIND_COMP_TRYGET(Wasteland::TorusRendererComponent)
        BIND_COMP_ADD(Wasteland::TorusRendererComponent)
        BIND_COMP_ADDOR(Wasteland::TorusRendererComponent)
        BIND_COMP_REM(Wasteland::TorusRendererComponent)

BIND_COMP_HAS(Wasteland::MaterialComponent)
        BIND_COMP_GET(Wasteland::MaterialComponent)
        BIND_COMP_TRYGET(Wasteland::MaterialComponent)
        BIND_COMP_ADD(Wasteland::MaterialComponent)
        // SoundComponent and the remaining gameplay components. Sound is last of
        // the components here because its accessors are the ones that depend on the
        // audio mixer existing.
        BIND_COMP_ADDOR(Wasteland::MaterialComponent)
        BIND_COMP_REM(Wasteland::MaterialComponent)

BIND_COMP_HAS(Wasteland::CameraComponent)
        BIND_COMP_GET(Wasteland::CameraComponent)
        BIND_COMP_TRYGET(Wasteland::CameraComponent)
        BIND_COMP_ADD(Wasteland::CameraComponent)
        BIND_COMP_ADDOR(Wasteland::CameraComponent)
        BIND_COMP_REM(Wasteland::CameraComponent)

BIND_COMP_HAS(Wasteland::Rigidbody2DComponent)
        BIND_COMP_GET(Wasteland::Rigidbody2DComponent)
        BIND_COMP_TRYGET(Wasteland::Rigidbody2DComponent)
        BIND_COMP_ADD(Wasteland::Rigidbody2DComponent)
        BIND_COMP_ADDOR(Wasteland::Rigidbody2DComponent)
        BIND_COMP_REM(Wasteland::Rigidbody2DComponent)

BIND_COMP_HAS(Wasteland::BoxCollider2DComponent)
        BIND_COMP_GET(Wasteland::BoxCollider2DComponent)
        BIND_COMP_TRYGET(Wasteland::BoxCollider2DComponent)
        // DEFECT, already noted above and repeated here because the macro is where
        // it is actually generated: BIND_COMP_* stringifies the fully qualified C++
        // type, so the Python names are GetWasteland::SoundComponent. BIND_GAMEPLAY_COMP
        // below takes a bare name and produces the usable GetSoundComponent. Both
        // sets exist on Entity; only the second is callable as an identifier.
        BIND_COMP_ADD(Wasteland::BoxCollider2DComponent)
        BIND_COMP_ADDOR(Wasteland::BoxCollider2DComponent)
        BIND_COMP_REM(Wasteland::BoxCollider2DComponent)

BIND_COMP_HAS(Wasteland::CircleCollider2DComponent)
        BIND_COMP_GET(Wasteland::CircleCollider2DComponent)
        BIND_COMP_TRYGET(Wasteland::CircleCollider2DComponent)
        BIND_COMP_ADD(Wasteland::CircleCollider2DComponent)
        BIND_COMP_ADDOR(Wasteland::CircleCollider2DComponent)
        BIND_COMP_REM(Wasteland::CircleCollider2DComponent)

BIND_COMP_HAS(Wasteland::RigidBody3DComponent)
        BIND_COMP_GET(Wasteland::RigidBody3DComponent)
        BIND_COMP_TRYGET(Wasteland::RigidBody3DComponent)
        BIND_COMP_ADD(Wasteland::RigidBody3DComponent)
        BIND_COMP_ADDOR(Wasteland::RigidBody3DComponent)
        BIND_COMP_REM(Wasteland::RigidBody3DComponent)

BIND_COMP_HAS(Wasteland::BoxCollider3DComponent)
        BIND_COMP_GET(Wasteland::BoxCollider3DComponent)
        BIND_COMP_TRYGET(Wasteland::BoxCollider3DComponent)
        BIND_COMP_ADD(Wasteland::BoxCollider3DComponent)
        BIND_COMP_ADDOR(Wasteland::BoxCollider3DComponent)
        BIND_COMP_REM(Wasteland::BoxCollider3DComponent)

BIND_COMP_HAS(Wasteland::SphereCollider3DComponent)
        BIND_COMP_GET(Wasteland::SphereCollider3DComponent)
        BIND_COMP_TRYGET(Wasteland::SphereCollider3DComponent)
        BIND_COMP_ADD(Wasteland::SphereCollider3DComponent)
        BIND_COMP_ADDOR(Wasteland::SphereCollider3DComponent)
        BIND_COMP_REM(Wasteland::SphereCollider3DComponent)

BIND_COMP_HAS(Wasteland::CapsuleCollider3DComponent)
        BIND_COMP_GET(Wasteland::CapsuleCollider3DComponent)
        BIND_COMP_TRYGET(Wasteland::CapsuleCollider3DComponent)
        BIND_COMP_ADD(Wasteland::CapsuleCollider3DComponent)
        BIND_COMP_ADDOR(Wasteland::CapsuleCollider3DComponent)
        BIND_COMP_REM(Wasteland::CapsuleCollider3DComponent)

// From here the macro block is mechanical: six calls per component, in the
// order Has, Get, TryGet, Add, AddOr, Rem. No component in this half has a
// special case, which is the point of the macro -- the light, water and
// script components above all get identical treatment.
BIND_COMP_HAS(Wasteland::VolumetricFogComponent)
        BIND_COMP_GET(Wasteland::VolumetricFogComponent)
        BIND_COMP_TRYGET(Wasteland::VolumetricFogComponent)
        BIND_COMP_ADD(Wasteland::VolumetricFogComponent)
        BIND_COMP_ADDOR(Wasteland::VolumetricFogComponent)
        BIND_COMP_REM(Wasteland::VolumetricFogComponent)

BIND_COMP_HAS(Wasteland::VolumetricCloudsComponent)
        BIND_COMP_GET(Wasteland::VolumetricCloudsComponent)
        BIND_COMP_TRYGET(Wasteland::VolumetricCloudsComponent)
        BIND_COMP_ADD(Wasteland::VolumetricCloudsComponent)
        BIND_COMP_ADDOR(Wasteland::VolumetricCloudsComponent)
        BIND_COMP_REM(Wasteland::VolumetricCloudsComponent)

// The four light components get the same six accessors despite having very
// different field sets (see the light component bindings above). Uniform
// accessors, non-uniform data.
BIND_COMP_HAS(Wasteland::DirectionalLightComponent)
        BIND_COMP_GET(Wasteland::DirectionalLightComponent)
        BIND_COMP_TRYGET(Wasteland::DirectionalLightComponent)
        BIND_COMP_ADD(Wasteland::DirectionalLightComponent)
        BIND_COMP_ADDOR(Wasteland::DirectionalLightComponent)
        BIND_COMP_REM(Wasteland::DirectionalLightComponent)

// BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, HealthComponent) expands
// to the same six operations under the SHORT name. Passing the short name
// separately is the whole reason this second macro family exists.
BIND_COMP_HAS(Wasteland::PointLightComponent)
        BIND_COMP_GET(Wasteland::PointLightComponent)
        BIND_COMP_TRYGET(Wasteland::PointLightComponent)
        BIND_COMP_ADD(Wasteland::PointLightComponent)
        // The cost of the workaround is a duplicated surface: every one of these
        // components now has two accessor sets on Entity. Nothing removes the
        // unusable one, and hasattr() will report it as present.
        BIND_COMP_ADDOR(Wasteland::PointLightComponent)
        BIND_COMP_REM(Wasteland::PointLightComponent)

BIND_COMP_HAS(Wasteland::SpotLightComponent)
        BIND_COMP_GET(Wasteland::SpotLightComponent)
        BIND_COMP_TRYGET(Wasteland::SpotLightComponent)
        BIND_COMP_ADD(Wasteland::SpotLightComponent)
        BIND_COMP_ADDOR(Wasteland::SpotLightComponent)
        BIND_COMP_REM(Wasteland::SpotLightComponent)

BIND_COMP_HAS(Wasteland::AreaLightComponent)
        BIND_COMP_GET(Wasteland::AreaLightComponent)
        BIND_COMP_TRYGET(Wasteland::AreaLightComponent)
        BIND_COMP_ADD(Wasteland::AreaLightComponent)
        BIND_COMP_ADDOR(Wasteland::AreaLightComponent)
        BIND_COMP_REM(Wasteland::AreaLightComponent)

// ScriptComponent, BehaviorTreeComponent and WaterComponent close the
// BIND_COMP_* block. The gameplay components below use the other macro, so
// the two families are adjacent here and separated by only a comment.
BIND_COMP_HAS(Wasteland::ScriptComponent)
        BIND_COMP_GET(Wasteland::ScriptComponent)
        BIND_COMP_TRYGET(Wasteland::ScriptComponent)
        BIND_COMP_ADD(Wasteland::ScriptComponent)
        BIND_COMP_ADDOR(Wasteland::ScriptComponent)
        BIND_COMP_REM(Wasteland::ScriptComponent)
        // Behaviour trees: same six accessors as scripts, so attaching a
        // tree looks exactly like attaching a script from Python.
        BIND_COMP_HAS(Wasteland::BehaviorTreeComponent)
        BIND_COMP_GET(Wasteland::BehaviorTreeComponent)
        BIND_COMP_TRYGET(Wasteland::BehaviorTreeComponent)
        BIND_COMP_ADD(Wasteland::BehaviorTreeComponent)
        BIND_COMP_ADDOR(Wasteland::BehaviorTreeComponent)
        BIND_COMP_REM(Wasteland::BehaviorTreeComponent)
        BIND_COMP_HAS(Wasteland::WaterComponent)
        BIND_COMP_GET(Wasteland::WaterComponent)
        BIND_COMP_TRYGET(Wasteland::WaterComponent)
        BIND_COMP_ADD(Wasteland::WaterComponent)
        BIND_COMP_ADDOR(Wasteland::WaterComponent)
        BIND_COMP_REM(Wasteland::WaterComponent)

// --- Gameplay components ---
        // NOTE: BIND_COMP_* stringifies the fully-qualified type, so it emits
        // unreachable names like "GetWasteland::HealthComponent". That is a
        // pre-existing engine-wide bug; BIND_GAMEPLAY_COMP takes the bare
        // name as a second argument so gameplay scripts get methods they can
        // actually call.
        // The gameplay block switches to BIND_GAMEPLAY_COMP, which takes the bare
        // type name as a second argument. Both macros bind the same six operations;
        // only the resulting Python method names differ.
        // The BIND_GAMEPLAY_COMP block: one call per component instead of six.
        // This is the form that produces USABLE method names -- GetHealthComponent
        // rather than GetWasteland::HealthComponent -- at the cost of a second
        // parallel surface that the BIND_COMP_* block above also generates.
        BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, HealthComponent)
        // So Entity ends up with BOTH GetWasteland::HealthComponent (from
        // BIND_COMP_*) and GetHealthComponent (from BIND_GAMEPLAY_COMP). The first
        // is unaddressable as a Python identifier because of the '::', so it is
        // effectively dead weight kept alive only for getattr() callers.
        BIND_GAMEPLAY_COMP(Wasteland::WeaponComponent, WeaponComponent)
        BIND_GAMEPLAY_COMP(Wasteland::RaycastIgnoreComponent, RaycastIgnoreComponent)
        BIND_GAMEPLAY_COMP(Wasteland::HittableComponent, HittableComponent)
        BIND_GAMEPLAY_COMP(Wasteland::InteractableComponent, InteractableComponent)
        BIND_GAMEPLAY_COMP(Wasteland::WeaponFeedbackComponent, WeaponFeedbackComponent)
        BIND_GAMEPLAY_COMP(Wasteland::ViewStateComponent, ViewStateComponent)
        BIND_GAMEPLAY_COMP(Wasteland::InventoryComponent, InventoryComponent)
        BIND_GAMEPLAY_COMP(Wasteland::PersistentComponent, PersistentComponent)
        BIND_GAMEPLAY_COMP(Wasteland::QuestFlagsComponent, QuestFlagsComponent)
        BIND_GAMEPLAY_COMP(Wasteland::WorldTimerComponent, WorldTimerComponent)
        BIND_GAMEPLAY_COMP(Wasteland::ItemDefinitionComponent, ItemDefinitionComponent)
        // The pile a drop creates. Without this line a script can construct a
        // DroppedItemComponent and read one off an entity, but cannot ATTACH one,
        // which is the same gap the audio pair above had.
        BIND_GAMEPLAY_COMP(Wasteland::DroppedItemComponent, DroppedItemComponent)
        // CameraComponent only ever got the BIND_COMP_* macro, which
        // stringifies the fully qualified C++ type -- producing the attribute
        // name "GetWasteland::CameraComponent", which is not addressable as
        // a Python identifier because of the "::". So no script could ever
        // read the camera to change its FOV. Bind a usable name alongside the
        // broken one; the old name stays so anything already relying on it via
        // getattr() keeps working.
        BIND_GAMEPLAY_COMP(Wasteland::CameraComponent, CameraComponent)
        // ScriptComponent is pre-existing but its typed accessor was never
        // reachable from Python (the BIND_COMP_* macro stringifies the fully
        // qualified C++ type), so scripts could never attach a script at
        // runtime. Bind it properly here.
        BIND_GAMEPLAY_COMP(Wasteland::ScriptComponent, ScriptComponent)
        // Same reason as ScriptComponent above: BIND_COMP_* stringifies the
        // qualified C++ type, so the unqualified-name accessor must be
        // declared explicitly or a tree is not attachable at all.
        BIND_GAMEPLAY_COMP(Wasteland::BehaviorTreeComponent, BehaviorTreeComponent)

// The 3D physics set a gameplay script needs to build a level:
        // static floors/walls, dynamic props, and the player's own collider
        // (which it will also mark RaycastIgnoreComponent so a trace from the
        // muzzle does not terminate on the shooter's capsule).
        BIND_GAMEPLAY_COMP(Wasteland::RigidBody3DComponent, RigidBody3DComponent)
        BIND_GAMEPLAY_COMP(Wasteland::BoxCollider3DComponent, BoxCollider3DComponent)
        BIND_GAMEPLAY_COMP(Wasteland::SphereCollider3DComponent, SphereCollider3DComponent)
        BIND_GAMEPLAY_COMP(Wasteland::CapsuleCollider3DComponent, CapsuleCollider3DComponent)

        // The audio pair. Both components were already registered as py::class_
        // with every field readable, but NO Entity accessor was bound for either,
        // so a script could inspect a source it had somehow obtained and could
        // never attach one. That made Scene.PlayEntitySound and
        // PlayAutoStartSources unreachable from Python -- the whole scene-facing
        // audio layer, which is otherwise complete and tested at the DSP level.
        //
        // The tell was that the classes existed but nothing referenced them in
        // the Entity binding: a missing BIND_GAMEPLAY_COMP line reads exactly
        // like a deliberate omission, and the symptom ("no sound plays") points
        // at the audio system rather than at a binding.
        BIND_GAMEPLAY_COMP(Wasteland::AudioSourceComponent, AudioSourceComponent)
        BIND_GAMEPLAY_COMP(Wasteland::AudioListenerComponent, AudioListenerComponent);

// ======================================== SCENE: Scene ================
    // Scene with a shared_ptr holder. It is a value-ish engine object but is
    // passed around by handle everywhere, including the save-game lambdas below
    // which take `const std::shared_ptr<Scene>&` so the scene cannot die
    // mid-serialisation.
    py::class_<Wasteland::Scene, std::shared_ptr<Wasteland::Scene> >(m, "Scene")
        .def(py::init<>())
        // --- Navigation (14.5.2) ---
        // One bake per scene, shared by every NPC. Baking per tree would make
        // pathfinding cost scale with the NPC count, which is the opposite of
        // the point of having a navmesh.
        .def("BakeNavMesh",
             [](Wasteland::Scene &s, const Wasteland::Navigation::NavMeshConfig &cfg) {
                 return s.BakeNavMesh(cfg);
             },
             py::arg("config") = Wasteland::Navigation::NavMeshConfig())
        .def("ClearNavMesh", &Wasteland::Scene::ClearNavMesh)
        .def("GetNavMesh",
             [](Wasteland::Scene &s) -> py::object {
                 // None rather than a crash when nothing is baked: a tree with
                 // UsePathfinding on must degrade to direct movement instead of
                 // taking the frame down.
                 // GetNavMesh returns None when nothing is baked. The cast has NO explicit
                 // return_value_policy, so pybind's default for a raw pointer applies --
                 // take_ownership. That means Python believes it owns this NavMesh, while the
                 // Scene holds it in a unique_ptr. When the scene is destroyed, Python's
                 // handle dangles, and when the Python object is collected it may delete a
                 // NavMesh the scene still points at. A double-free waiting for a teardown
                 // order. The null check above prevents the crash-on-null case but not this.
                 Wasteland::Navigation::NavMesh *mesh = s.GetNavMesh();
                 if (mesh == nullptr)
                     return py::none();
                 return py::cast(mesh);
             })
        // Copy is def_static and takes a Ref<Scene>, returning a new scene. Note the
        // arg is named "other" -- the Python-visible name is the scene being copied
        // FROM, which reads backwards at the call site.
        .def_static("Copy", &Wasteland::Scene::Copy, py::arg("other"))
        .def("CreateEntity", &Wasteland::Scene::CreateEntity,
             py::arg("name") = std::string())
        .def("CreateEntityWithUUID", &Wasteland::Scene::CreateEntityWithUUID,
             py::arg("uuid"), py::arg("name") = std::string())
        .def("DestroyEntity", &Wasteland::Scene::DestroyEntity,
             py::arg("entity"))
        .def("DuplicateEntity", &Wasteland::Scene::DuplicateEntity,
             py::arg("entity"))
        .def("IsEntityValid", &Wasteland::Scene::IsEntityValid,
             py::arg("entity"))
        .def("GetPrimaryCameraEntity",
             &Wasteland::Scene::GetPrimaryCameraEntity)
        // OnSimulationStart is the supported entry point that brings a physics
        // world up. The two OnPhysicsXDStart methods below are private by design.
        .def("OnRuntimeStart", &Wasteland::Scene::OnRuntimeStart)
        .def("OnRuntimeStop", &Wasteland::Scene::OnRuntimeStop)
        .def("OnSimulationStart", &Wasteland::Scene::OnSimulationStart)
        // StepPhysics is the headless entry point. OnUpdateSimulation needs an
        // EditorCamera& that only exists in the editor viewport, so before
        // this the 3D solver could not be advanced from Python at all --
        // meaning "does gravity work?" was untestable and a dedicated server
        // build had no way to simulate.
		// ---- Audio (14.4) ----
		//
		// These audio wrappers (through StopAllSounds) exist so a script can make
		// noise without knowing a mixer exists. GetAudio below is the escape hatch
		// for the cases that need a full PlayRequest.
		// Scene-level wrappers so a script can play a sound without knowing
		// that a mixer, a clip registry, and an AudioSystem exist underneath.
		// The raw mixer stays reachable through GetAudio() for the cases that
		// need a full PlayRequest.
		.def("PlaySound", &Wasteland::Scene::PlaySound, py::arg("clipName"))
		.def("PlaySoundAt", &Wasteland::Scene::PlaySoundAt, py::arg("clipName"),
			 py::arg("position"), py::arg("volume") = 1.0f, py::arg("pitch") = 1.0f)
		.def("PlayEntitySound", &Wasteland::Scene::PlayEntitySound, py::arg("entity"))
		.def("StopSound", &Wasteland::Scene::StopSound, py::arg("clipName"),
			 py::arg("fadeSeconds") = 0.0f)
		.def("StopAllSounds", &Wasteland::Scene::StopAllSounds, py::arg("fadeSeconds") = 0.0f)
		.def("LoadAudioClip", &Wasteland::Scene::LoadAudioClip, py::arg("path"))
		.def("SetMasterVolume", &Wasteland::Scene::SetMasterVolume, py::arg("volume"))
		.def("GetMasterVolume", &Wasteland::Scene::GetMasterVolume)
		.def("GetAudio", &Wasteland::Scene::GetAudioSystem, py::return_value_policy::reference_internal)
		// Advance the audio clock. Separate from the physics step because audio
		// time and simulation time genuinely differ: audio advances by frames
		// actually mixed, and the game may pause simulation while sound
		// UpdateAudio advances the audio clock independently of simulation. Note
		// the tab indentation here and on the lines around it -- the only place in
		// this file using tabs. Harmless, but it means a whitespace-insensitive
		// search for these bindings behaves differently from its neighbours.
		// continues.
		.def("UpdateAudio", [](Wasteland::Scene &scene, float seconds) {
			scene.GetAudioSystem()->Update(seconds);
		}, py::arg("seconds"))
        .def("StepPhysics", &Wasteland::Scene::StepPhysics, py::arg("seconds"))

        // ---- Driving a 3D body from script (route map 5.2) ----
        //
        // Scripts could READ physics for a long time and could not DRIVE it:
        // nothing jumped, nothing was knocked back, nothing launched. These
        // three close that, and they deliberately do NOT expose
        // RigidBody3DComponent::RuntimeBody. That handle is a raw, packed,
        // process-local Box3D body id -- harmless to bind as a number, and a
        // use-after-free the moment the world is torn down and the id is
        // dereferenced through it. So the lookup happens inside Scene
        // (Scene::ResolveBody3D) and Python sees a bool.
        //
        // Every one RETURNS FALSE rather than raising when the target has no
        // rigid body, when its body has not been created yet, or when there is
        // no physics world. A raise would take down a whole script frame for
        // what is an ordinary "that thing is not physical" answer.
        .def("ApplyImpulse", &Wasteland::Scene::ApplyImpulse,
             py::arg("entity"), py::arg("impulse"), py::arg("wake") = true)
        .def("SetLinearVelocity", &Wasteland::Scene::SetLinearVelocity,
             py::arg("entity"), py::arg("velocity"))
        // GetLinearVelocity has a C++ out-parameter, which pybind11 cannot bind:
        // a non-const reference return target is not a thing .def() supports,
        // and the failure it produces names no line in this file. So the out
        // param is converted to the return value, with None for the failure
        // case -- which is the same information bool-plus-out carried, and is
        // the only shape a Python caller can branch on without a dummy Vec3.
        //
        // The explicit -> py::object return type is required: mixing py::none()
        // and py::cast results in one lambda without it fails with
        // "all return expressions must deduce to the same type".
        .def("GetLinearVelocity",
             [](const Wasteland::Scene &s, Wasteland::Entity entity) -> py::object {
                 glm::vec3 velocity(0.0f, 0.0f, 0.0f);
                 if (!s.GetLinearVelocity(entity, velocity))
                     return py::none();
                 return py::cast(velocity);
             },
             py::arg("entity"))
        // Drive the FULL runtime tick from Python: water, health cooldowns,
        // StepPhysics advances ONLY the solver. It does not tick scripts, behaviour
        // trees, cooldowns or water. See OnUpdateRuntime below for why that
        // distinction matters.
        // scripts, behaviour trees, then physics -- the same order
        // OnUpdateRuntime uses.
        //
        // Exposed because StepPhysics alone skips behaviour trees entirely,
        // which is why an attached tree would appear to do nothing in a test
        // while working in-game. Two entry points that differ in behaviour is
        // exactly how "it works in the editor" bugs happen.
        .def("OnUpdateRuntime", [](Wasteland::Scene &scene, float seconds) {
            scene.OnUpdateRuntime(Wasteland::Timestep(seconds));
        }, py::arg("seconds"))
        // Compile an authored tree without attaching it, so "does this tree
        // still compile?" is answerable from a test or a tool.
        // The docstring MUST precede py::arg. pybind counts trailing string
        // literals as annotations, so a docstring after py::arg("path") reads
        // as a second argument and trips the static assert on argument count.
        // def_static, NOT def: the lambda takes no Scene&, and a .def on
        // class_<Scene> tells pybind to expect a method signature. With one
        // lambda parameter and one py::arg that should balance, but the
        // method-binding path also wants an implicit first `this` argument,
        // so the annotation count no longer matches -- and pybind reports
        // that from inside pybind11.h with NO line number in this file.
        // This is a static operation (it needs no scene), so say so.
        .def_static("CompileBehaviorTree",
                    [](const std::string &path) {
                        std::string python, error;
                        if (!Wasteland::BehaviorTreeSystem::CompileTree(path, python, error))
                            throw std::runtime_error(error);
                        return python;
                    },
                    py::arg("path"))
        // OnPhysics2DStart / OnPhysics3DStart are private, and deliberately so:
        // bringing a physics world up is a scene-lifecycle action, not
        // something a script should do on its own. They ARE reachable through
        // OnSimulationStart, which is the supported entry point.
        //
        // This matters for tests: without a world there is no Box3D solver, so
        // a headless script can call StepPhysics forever and see nothing move,
        // with no error. Physics suites must call OnSimulationStart() first.
        // See tests/headless/test_physics_3d.py, where omitting it produced a
        // "0/0 checks" green rather than a failure.
        // ---- Save / load (SceneSerializer) ----
        //
        // Previously unreachable from Python: the editor loads scenes through
        // C++, so no binding existed and a scripted save game was impossible.
        // Without these, an inventory resets on every reload.
        //
        // Serialize/Deserialize are the AUTHORED scene format (editor-facing,
        // keeps full entity data). The Runtime variants persist live play
        // state. Both are exposed because a save game wants Runtime and a
        // tool-authored scene wants the authored form.
        // SaveScene writes the AUTHORED format via a locally constructed
        // SceneSerializer and returns its actual save status to Python.
        .def("SaveScene", [](const std::shared_ptr<Wasteland::Scene> &sc, const std::string &path) {
                 Wasteland::SceneSerializer serializer(sc);
                 return serializer.Serialize(path);
             }, py::arg("path"))
        // LoadScene returns the serializer's bool just like SaveScene.
        .def("LoadScene", [](const std::shared_ptr<Wasteland::Scene> &sc, const std::string &path) {
                 Wasteland::SceneSerializer serializer(sc);
                 return serializer.Deserialize(path);
             }, py::arg("path"))
        // ---- Save games (14.3) ----
        //
        // Replaces the stub Runtime serializer calls, which asserted rather
        // than saving anything. These take a SLOT name, not a path: a save
        // must never be able to overwrite a .wastescene, and making the
        // caller supply a path is exactly how that would happen.
        // SaveGame takes a SLOT name, not a path -- deliberately, so a save can
        // never overwrite authored scene content. The directory is optional and only
        // overrides the default when non-empty.
        .def("SaveGame", [](const std::shared_ptr<Wasteland::Scene> &sc,
                            const std::string &slot,
                            const std::string &directory) {
                 Wasteland::SaveGame save(sc);
                 // SetSavedWorldTime is called before saving so the world clock round-trips.
                 // Without it a loaded save would resume at time zero, and any timer driven
                 // by world time would restart.
                 if (!directory.empty())
                     save.SetSaveDirectory(directory);
                 save.SetWorldTime(sc->GetSavedWorldTime());
                 return save.Save(slot);
             }, py::arg("slot"), py::arg("directory") = std::string())
        // LoadGame: the error string is taken BY VALUE and passed into make_tuple
        // as std::string(error). That cast is load-bearing -- py::make_tuple deduces
        // its type from the first branch, and a std::string& there would make the
        // success branch's std::string a mismatched return type (C3487).
        .def("LoadGame", [](const std::shared_ptr<Wasteland::Scene> &sc,
                            const std::string &slot,
                            const std::string &directory) {
                 Wasteland::SaveGame save(sc);
                 if (!directory.empty())
                     save.SetSaveDirectory(directory);
                 // By value, not by reference: py::make_tuple deduces its
                 // return type from the first branch's expression, and a
                 // std::string& there makes the std::string on the success
                 // path a mismatched return type (C3487).
                 std::string error;
                 if (!save.Load(slot, &error))
                     return py::make_tuple(false, std::string(error));
                 sc->SetSavedWorldTime(save.GetWorldTime());
                 return py::make_tuple(true, std::string());
             }, py::arg("slot"), py::arg("directory") = std::string())
        // (exists, schemaVersion) so a caller can report "this save is from
        // another version" instead of failing opaquely at load time.
        // GameSlotExists returns (exists, schemaVersion) together, so a caller can
        // refuse to load a save written by a different engine version instead of
        // discovering the mismatch as a corrupt file.
        .def("GameSlotExists", [](const std::shared_ptr<Wasteland::Scene> &sc,
                                  const std::string &slot,
                                  const std::string &directory) {
                 Wasteland::SaveGame save(sc);
                 if (!directory.empty())
                     save.SetSaveDirectory(directory);
                 int version = 0;
                 bool exists = save.SlotExists(slot, &version);
                 return py::make_tuple(exists, version);
             }, py::arg("slot"), py::arg("directory") = std::string())
        // CurrentSchemaVersion takes an unnamed Scene parameter -- it ignores it and
        // reads a static constant. The parameter exists so the method reads as part
        // of the Scene API; the lambda discards it.
        .def("CurrentSchemaVersion", [](const std::shared_ptr<Wasteland::Scene> &) {
                 return Wasteland::SaveGame::kCurrentSchemaVersion;
             })
        .def("GetWorldTime", [](const std::shared_ptr<Wasteland::Scene> &sc) {
                 return sc->GetSavedWorldTime();
             })
        .def("SetWorldTime", [](const std::shared_ptr<Wasteland::Scene> &sc, double t) {
                 sc->SetSavedWorldTime(t);
             }, py::arg("seconds"))
        // SaveRuntimeState / LoadRuntimeState are the RUNTIME variants, distinct
        // from SaveScene/LoadScene. Same serializer object, different method: the
        // authored form keeps full entity data, the runtime form persists play state.
        .def("SaveRuntimeState", [](const std::shared_ptr<Wasteland::Scene> &sc, const std::string &path) {
                 Wasteland::SceneSerializer serializer(sc);
                 serializer.SerializeRuntime(path);
                 return true;
             }, py::arg("path"))
        .def("LoadRuntimeState", [](const std::shared_ptr<Wasteland::Scene> &sc, const std::string &path) {
                 Wasteland::SceneSerializer serializer(sc);
                 return serializer.DeserializeRuntime(path);
             }, py::arg("path"))
        // ---- Inventory ----
        // Take Entity by value like every other gameplay Scene method, so the
        // Python call reads the same as the C++ one.
        // The inventory wrappers take Entity BY VALUE, matching the C++ signatures,
        // so the Python call reads the same as the C++ one. Entity is a cheap
        // (handle, scene pointer) pair, so the copy is free.
        // The inventory wrappers are all trivial forwarding lambdas -- Scene is a
        // bound class, so &Scene::AddItem would work directly. The lambdas exist to
        // pin the argument NAMES and the quantity default, which a method pointer
        // would not give.
        .def("AddItem", [](Wasteland::Scene &sc, Wasteland::Entity e,
                           const std::string &itemId, int quantity) {
                 // quantity defaults to 1 on every inventory method. That default is what
                 // makes AddItem(e, "pistol") mean "one pistol" rather than an error.
                 // quantity defaults to 1 throughout, so AddItem(e, "pistol") means one.
                 // The return value is the scene's own bool, which presumably reports
                 // whether the item was accepted -- so a full-inventory rejection is visible
                 // rather than silent.
                 return sc.AddItem(e, itemId, quantity);
             }, py::arg("entity"), py::arg("itemId"), py::arg("quantity") = 1)
        .def("RemoveItem", [](Wasteland::Scene &sc, Wasteland::Entity e,
                              const std::string &itemId, int quantity) {
                 return sc.RemoveItem(e, itemId, quantity);
             }, py::arg("entity"), py::arg("itemId"), py::arg("quantity") = 1)
        // Renamed from DropItem, which promised a drop and only removed from the
        // inventory: the whole C++ body was `return RemoveItem(...)`, so a
        // script reading the name spawned nothing and the items simply
        // ceased to exist. The old binding comment here went further and
        // claimed DropItem "spawns world geometry" -- the opposite of what
        // it did, which is why the footgun survived review. See
        // Scene::RemoveItemFromInventory in Scene.cpp.
        .def("RemoveItemFromInventory",
             [](Wasteland::Scene &sc, Wasteland::Entity e,
                const std::string &itemId, int quantity) {
                 return sc.RemoveItemFromInventory(e, itemId, quantity);
             }, py::arg("entity"), py::arg("itemId"), py::arg("quantity") = 1)
        // DropItemAt is the REAL drop: it removes AND spawns a world entity
        // carrying DroppedItemComponent, so the items exist somewhere
        // afterwards. It returns that entity, or an INVALID one when nothing
        // was dropped -- IsValid() on the result is the documented test,
        // which is why this returns Entity rather than a bool or a count.
        //
        // worldOffset defaults to the origin because a script with no camera
        // and no physics world cannot usefully compute a world position, and
        // "at my feet" is the position the caller can always express.
        .def("DropItemAt",
             [](Wasteland::Scene &sc, Wasteland::Entity e,
                const std::string &itemId, int quantity, const glm::vec3 &offset) {
                 return sc.DropItemAt(e, itemId, quantity, offset);
             }, py::arg("entity"), py::arg("itemId"), py::arg("quantity") = 1,
             py::arg("worldOffset") = glm::vec3(0.0f, 0.0f, 0.0f))
        // PickUpDroppedItem closes the loop, so a test can assert that a
        // round trip restores the count rather than only that one direction
        // works. Returns how many were actually taken, which is less than the
        // pile held when the carrier was nearly full.
        .def("PickUpDroppedItem", &Wasteland::Scene::PickUpDroppedItem,
             py::arg("dropped"), py::arg("carrier"))
        // GetItemCount and HasItem differ in one way that matters: HasItem takes a
        // QUANTITY defaulting to 1, so `HasItem(e, "pistol")` asks "any at all"
        // while `HasItem(e, "pistol", 2)` asks for a stack.
        .def("GetItemCount", [](Wasteland::Scene &sc, Wasteland::Entity e,
                                 const std::string &itemId) {
                 return sc.GetItemCount(e, itemId);
             }, py::arg("entity"), py::arg("itemId"))
        .def("HasItem", [](Wasteland::Scene &sc, Wasteland::Entity e,
                           const std::string &itemId, int quantity) {
                 return sc.HasItem(e, itemId, quantity);
             }, py::arg("entity"), py::arg("itemId"), py::arg("quantity") = 1)
        // GetInventoryWeight and IsInventoryFull are the derived queries -- both read
        // the inventory rather than the item definitions directly, so they reflect
        // the entity's CURRENT contents including unequipped items.
        .def("GetInventoryWeight", [](Wasteland::Scene &sc, Wasteland::Entity e) {
                 return sc.GetInventoryWeight(e);
             }, py::arg("entity"))
        .def("IsInventoryFull", [](Wasteland::Scene &sc, Wasteland::Entity e) {
                 return sc.IsInventoryFull(e);
             }, py::arg("entity"))
        // FindItemDefinition returns a REFERENCE to the definition, so a script can
        // read MaxStack and Weight off a definition it located by id. None means the
        // id is unknown -- a data error the caller can report.
        // Returns a live reference so a script can read MaxStack/Weight of a
        // definition it found. Returns None when the id is unknown, which is
        // a data error the caller can report.
        .def("FindItemDefinition", [](Wasteland::Scene &sc, const std::string &itemId) -> py::object {
                 Wasteland::ItemDefinitionComponent *def = sc.FindItemDefinition(itemId);
                 if (!def)
                     return py::none();
                 return py::cast(*def, py::return_value_policy::reference);
             }, py::arg("itemId"))
        // The frame-loop hooks. OnSimulationStop / OnUpdateSimulation /
        // OnUpdateEditor / OnUpdateRuntime are the engine's own entry points, bound
        // so a headless tool can drive a scene without the main loop.
        .def("OnSimulationStop", &Wasteland::Scene::OnSimulationStop)
        .def("OnUpdateSimulation", &Wasteland::Scene::OnUpdateSimulation,
             py::arg("ts"), py::arg("camera"))
        .def("OnUpdateEditor", &Wasteland::Scene::OnUpdateEditor,
             py::arg("ts"), py::arg("camera"))
        // OnUpdateRuntime takes ONLY a timestep, while OnUpdateSimulation and
        // OnUpdateEditor also take a camera. So the runtime path has no camera --
        // it gets whatever the renderer was last given. A script driving runtime
        // updates cannot choose the view.
        .def("OnUpdateRuntime", &Wasteland::Scene::OnUpdateRuntime,
             py::arg("ts"))
        // OnViewportResize is a scene-level hook, so a script that resizes the
        // window from Python ends up here as well as in the engine's own handler.
        // Both run; neither is suppressed.
        .def("OnViewportResize", &Wasteland::Scene::OnViewportResize,
             py::arg("width"), py::arg("height"))
        .def("ReloadMaterialTextures",
             &Wasteland::Scene::ReloadMaterialTextures)
        // GetWaterHeightAt at scene scope, returning the same (hit, height, flow)
        // triple as the entity method above. The scene version queries any water
        // body; the entity version resolves relative to that entity. Two spellings
        // of the same query with different cost.
        .def("GetWaterHeightAt",
             [](Wasteland::Scene &s, const glm::vec3 &pos) {
                 float height = 0.0f;
                 glm::vec2 flow(0.0f);
                 bool hit = s.GetWaterHeightAt(pos, height, &flow);
                 return py::make_tuple(hit, height, flow);
             },
             py::arg("pos"))
        // IsUnderwater's margin defaults to 0 at scene scope, while the entity
        // binding above makes it required. Inconsistent defaults for the same
        // concept -- the entity form was added later and copied a stricter shape.
        .def("IsUnderwater", &Wasteland::Scene::IsUnderwater,
             py::arg("pos"), py::arg("margin") = 0.0f)
        // Raycast returns a flat tuple rather than a struct so scripts can
        // unpack it directly:
        //     hit, entity, point, normal, dist, hittable = scene.Raycast(o, d, r)
        // `entity` is a default-constructed (invalid) Entity on a world-geometry
        // hit, so check `hit` and `hittable` before touching it.
        // Raycast returns a flat six-tuple rather than a struct, so a script can
        // unpack it in one line. The shape is: (hit, entity, point, normal,
        // distance, hittable).
        .def("Raycast",
             [](Wasteland::Scene &s, const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance) {
                 // DEFECT-adjacent: the `entity` slot is a default-constructed (invalid)
                 // Entity on a world-geometry hit, and nothing here signals that. A script
                 // that reads `entity` without checking `hit` and `hittable` gets a handle
                 // whose Scene pointer is null -- and every method on it asserts. The tuple
                 // gives no way to distinguish it from a real hit without checking hit first.
                 Wasteland::Entity hitEntity;
                 glm::vec3 point(0.0f);
                 glm::vec3 normal(0.0f, 1.0f, 0.0f);
                 float distance = 0.0f;
                 bool hittable = false;
                 bool hit = s.Raycast(origin, direction, maxDistance,
                                      &hitEntity, &point, &normal, &distance, &hittable);
                 return py::make_tuple(hit, hitEntity, point, normal, distance, hittable);
             },
             py::arg("origin"), py::arg("direction"), py::arg("maxDistance"))

// Capsule sweep for character movement. Flat tuple, matching Raycast:
        //     hit, entity, normal, fraction, translation = scene.MoveCapsule(
        //         origin, radius, halfHeight, delta)
        //
        // `translation` is the move that was actually allowed after sliding
        // along whatever blocked it, so a caller just does
        // position += translation rather than re-deriving the clamp itself.
        // `fraction` is the same value as a [0, 1] multiplier on the input,
        // for callers that prefer to scale.
        // MoveCapsule is the character-movement primitive. It returns
        // (hit, entity, normal, fraction, translation) -- note the ORDER differs
        // from Raycast's, which is an easy source of silent misreads.
        .def("MoveCapsule",
             [](Wasteland::Scene &s, const glm::vec3 &origin, float radius, float halfHeight,
                const glm::vec3 &translation) {
                 Wasteland::Entity hitEntity;
                 glm::vec3 normal(0.0f);
                 glm::vec3 allowed(0.0f);
                 float fraction = 1.0f;
                 bool hit = s.MoveCapsule(origin, radius, halfHeight, translation,
                                           &hitEntity, &normal, &fraction, &allowed);
                 return py::make_tuple(hit, hitEntity, normal, fraction, allowed);
             },
             py::arg("origin"), py::arg("radius"), py::arg("halfHeight"),
             // `translation` is the move that was actually ALLOWED after sliding along
             // whatever blocked it. So the correct call is position += translation, not
             // position = origin + translation. Getting that wrong makes a character
             // stick to walls.
             py::arg("translation"))

// Specific entity queries
        // Lets a script (or a headless tool driving one) resolve which scene
        // is live without holding a reference. The engine sets this itself in
        // Scene::OnUpdateRuntime; exposing the setter makes that reachable.
        // No keep_alive here: this is a static method, so the return slot
        // (position 0) is what pybind would tie a lifetime to.
        // SetActiveScene is def_static but takes a Scene as an argument -- it sets
        // the global the engine also sets itself in OnUpdateRuntime. Exposing the
        // setter is what lets a headless tool nominate which scene is live.
        .def_static("SetActiveScene",
                    [](Wasteland::Scene &s) {
                        Wasteland::ScriptEngine::SetActiveScene(&s);
                    },
                    py::arg("scene"))
        .def_static("GetActiveScene",
                    []() -> py::object {
                        Wasteland::Scene *s = Wasteland::ScriptEngine::GetActiveScene();
                        if (!s)
                            return py::none();
                        return py::cast(s, py::return_value_policy::reference);
                    })
        // GetEntities reads the ACTIVE scene rather than taking one, and filters to
        // entities that have a TransformComponent. So it silently omits any entity
        // without a transform -- which includes an entity mid-construction.
        // GetEntities (the static form) reads the ACTIVE scene through
        // ScriptEngine, then filters to entities carrying a TransformComponent. The
        // null check returns an empty list rather than raising, so a call before any
        // scene is open yields [].
        .def_static("GetEntities",
                    []() {
                        py::list result;
                        Wasteland::Scene *s = Wasteland::ScriptEngine::GetActiveScene();
                        if (!s)
                            return result;
                        auto view = s->GetAllEntitiesWith<Wasteland::TransformComponent>();
                        // GetAllEntitiesWithTransform is the instance method equivalent, so a
                        // script holding a Scene does not have to set it active first. The two
                        // spellings exist because the static form is a module-level convenience.
                        // Each Entity is constructed as Entity(handle, scene) -- a thin wrapper
                        // around the entt handle. The wrapper owns nothing, so a list of these from
                        // GetEntities stays valid only as long as the scene does.
                        for (auto e : view)
                            result.append(Wasteland::Entity(e, s));
                        return result;
                    })
        // GetAllEntitiesWithTransform is the instance form of the same query, so a
        // script holding a Scene does not have to make it active first. GetAllEntitiesWithTag
        // below is the same shape filtered on TagComponent instead.
        .def("GetAllEntitiesWithTransform",
             [](Wasteland::Scene &s) {
                 py::list result;
                 for (auto e : s.GetAllEntitiesWith<Wasteland::TransformComponent>())
                     result.append(Wasteland::Entity(e, &s));
                 return result;
             })
        // Tag filtering uses GetAllEntitiesWith<TagComponent>, so only entities that
        // HAVE a tag appear. An entity tagged with an empty string still matches --
        // the filter is on component presence, not on the tag's value.
        .def("GetAllEntitiesWithTag",
             [](Wasteland::Scene &s) {
                 py::list result;
                 for (auto e : s.GetAllEntitiesWith<Wasteland::TagComponent>())
                     result.append(Wasteland::Entity(e, &s));
                 return result;
             });

// ======================================== SCENE: SceneSerializer =======
    py::class_<Wasteland::SceneSerializer>(m, "SceneSerializer")
        .def(py::init<const Wasteland::Ref<Wasteland::Scene> &>(),
             py::arg("scene"))
        .def("Serialize", &Wasteland::SceneSerializer::Serialize,
             py::arg("filepath"))
        .def("SerializeRuntime", &Wasteland::SceneSerializer::SerializeRuntime,
             py::arg("filepath"))
        .def("Deserialize", &Wasteland::SceneSerializer::Deserialize,
             py::arg("filepath"))
        .def("DeserializeRuntime", &Wasteland::SceneSerializer::DeserializeRuntime,
             py::arg("filepath"))
        // SceneSerializer::Trim is exposed because Python source uses the same
        // indenting rules as C++ and needs the identical helper to generate
        // consistent whitespace in generated code.
        .def_static("Trim", &Wasteland::SceneSerializer::Trim, py::arg("str"));

// ======================================== SCENE: ScriptableEntity ======
    // ScriptableEntity exposes exactly TWO accessors -- the transform and the
    // tag -- because that is the base class's entire surface. Every other
    // component comes through the Entity binding instead, so a script working
    // with a ScriptableEntity cannot reach it except via these two.
    py::class_<Wasteland::ScriptableEntity>(m, "ScriptableEntity")
        .def("GetComponentTransform",
             [](Wasteland::ScriptableEntity &se)
                 -> Wasteland::TransformComponent & {
                 return se.GetComponent<Wasteland::TransformComponent>();
             },
             // reference without reference_internal here, unlike the component
             // properties elsewhere. The difference matters: reference_internal ties the
             // result to the SCRIPTABLE ENTITY's lifetime, while plain reference ties it
             // to the ENTITY the component actually belongs to -- and it is the entity
             // that must outlive the handle.
             py::return_value_policy::reference)
        .def("GetComponentTag",
             [](Wasteland::ScriptableEntity &se)
                 -> Wasteland::TagComponent & {
                 return se.GetComponent<Wasteland::TagComponent>();
             },
             py::return_value_policy::reference);

// ======================================== SCRIPTING =====================
    // The ScriptEngine free functions. OnUpdateEntity and OnDestroyEntity are
    // the engine CALLING INTO the script layer, so they are bound so a script
    // can drive the loop itself; they are not methods a script would normally
    // invoke.
    m.def("ScriptEngine_OnUpdateEntity",
         &Wasteland::ScriptEngine::OnUpdateEntity,
         py::arg("entity"), py::arg("ts"));
    m.def("ScriptEngine_OnDestroyEntity",
         &Wasteland::ScriptEngine::OnDestroyEntity, py::arg("id"));
    m.def("ScriptEngine_Shutdown", &Wasteland::ScriptEngine::Shutdown);
    // GetInstance returns a raw instance POINTER with no return_value_policy,
    // so pybind's default for pointers applies. That is a borrowed handle with
    // no lifetime tie to anything -- the hazard noted at the Entity-level
    // GetInstance above, and the same hazard reached by a shorter path.
    m.def("ScriptEngine_GetInstance",
         &Wasteland::ScriptEngine::GetInstance, py::arg("id"));

// ======================================== ImGui =========================
    // ImGuiLayer with Begin/End framing the UI. The engine calls those itself,
    // so they are bound for a script that embeds its own ImGui content inside
    // the existing frame rather than opening a second one.
    py::class_<Wasteland::ImGuiLayer, Wasteland::Layer>(m, "ImGuiLayer")
        .def(py::init<>())
        .def("Begin", &Wasteland::ImGuiLayer::Begin)
        .def("End", &Wasteland::ImGuiLayer::End)
        .def("BlockEvents", &Wasteland::ImGuiLayer::BlockEvents,
             py::arg("block"))
        // SwitchBackend/HasBackend change which graphics API ImGui renders
        // through. Like RendererAPI_SetAPI above, nothing validates the switch --
        // changing it with live ImGui resources from the old backend leaves them
        // invalid.
        .def("SwitchBackend", &Wasteland::ImGuiLayer::SwitchBackend,
             py::arg("api"))
        .def("HasBackend", &Wasteland::ImGuiLayer::HasBackend);

// ============================== Visual Blocks ==========================
    {
        // A bare block rather than a namespace or a function. Everything Visual
        // Blocks needs is registered into vbMod and no local C++ state escapes, so
        // the extra scope exists only to keep `vb` and `vbMod` from colliding with
        // names in the enclosing module scope.
        namespace vb = Wasteland::VisualBlocks;
        // RegisterAllNodes is called at IMPORT time, before any binding is used.
        // Skipping it would leave the registry empty, every lookup would return
        // nothing, and the symptom would be "unknown node type" for every node in
        // every graph -- with no hint that registration never ran.
        auto vbMod = m.def_submodule("VisualBlocks", "Visual graph data model, "
                                                     "validator and compiler.");

        // Populate the node table on import. Without this every lookup returns
        // nothing and the compiler has no node definitions to emit from --
        // which surfaces as "unknown node type" for every node in every graph.
        vb::RegisterAllNodes();

        // PinType is the data-type vocabulary for pin connections. It uses
        // .export_values(), so the enumerators ALSO appear bare in the VisualBlocks
        // namespace. PinDir below does the same; no other enum in this file does.
        py::enum_<vb::PinType>(vbMod, "PinType")
            .value("Exec", vb::PinType::Exec)
            .value("Bool", vb::PinType::Bool)
            .value("Int", vb::PinType::Int)
            .value("Float", vb::PinType::Float)
            .value("String", vb::PinType::String)
            .value("Vec2", vb::PinType::Vec2)
            .value("Vec3", vb::PinType::Vec3)
            .value("Vec4", vb::PinType::Vec4)
            .value("Entity", vb::PinType::Entity)
            // The wildcard Any type is exported deliberately. Without it a wildcard pin
            // renders as "???" because pybind has no name for an enumerator it was
            // never told about, which reads as a corrupted pin rather than a wildcard.
            // The wildcard data type. Without this entry a wildcard pin binds
            // as "???" -- pybind11 has no name for an enumerator it was never
            // told about -- which reads as a corrupted pin rather than a
            // missing binding.
            .value("Any", vb::PinType::Any)
            .export_values();

        py::enum_<vb::PinDir>(vbMod, "PinDir")
            .value("In", vb::PinDir::In)
            .value("Out", vb::PinDir::Out)
            .export_values();

        // Not constructible from Python: a NodeDef's pins and emitter are set
        // through RegisterNodeDef, which is the only supported extension path.
        // PinDef is deliberately NOT constructible from Python. A node's pins are
        // established through RegisterNodeDef, the single supported extension path;
        // letting a script build one would let it invent a pin the compiler has no
        // emitter for.
        py::class_<vb::PinDef>(vbMod, "PinDef")
            .def_readonly("Id", &vb::PinDef::Id)
            .def_readonly("Label", &vb::PinDef::Label)
            .def_readonly("Type", &vb::PinDef::Type)
            .def_readonly("Dir", &vb::PinDef::Dir)
            .def_readonly("Required", &vb::PinDef::Required)
            // GraphNode is the constructible one. Id, TypeId and Title identify it; X/Y
            // are editor canvas coordinates and have no meaning to the compiler.
            .def_readonly("DefaultLiteral", &vb::PinDef::DefaultLiteral);

        // PinLiteral: a key/value/IsSet triple for a node's literal input. IsSet is
        // separate from Value because an EMPTY STRING is a legitimate value --
        // without the flag, "set to empty" and "not set" would be
        // indistinguishable.
        py::class_<vb::PinLiteral>(vbMod, "PinLiteral")
            .def(py::init<>())
            .def_readwrite("Key", &vb::PinLiteral::Key)
            .def_readwrite("Value", &vb::PinLiteral::Value)
            .def_readwrite("IsSet", &vb::PinLiteral::IsSet);

        // GraphInput carries both a literal AND a link target (LinkedNode,
        // LinkedPin, IsLinked). IsLinked is the discriminator: when true the link
        // wins and Literal is ignored, so writing both is legal but only one is read.
        py::class_<vb::GraphInput>(vbMod, "GraphInput")
            .def(py::init<>())
            .def_readwrite("Literal", &vb::GraphInput::Literal)
            .def_readwrite("HasLiteral", &vb::GraphInput::HasLiteral)
            .def_readwrite("LinkedNode", &vb::GraphInput::LinkedNode)
            .def_readwrite("LinkedPin", &vb::GraphInput::LinkedPin)
            .def_readwrite("IsLinked", &vb::GraphInput::IsLinked);

        // GraphNode: Id/TypeId/Title are identity, X/Y are editor layout, and the
        // rest (EventName, Latent, Abort, Literals, ChildIds) are graph semantics.
        // Latent in particular is a control-flow flag the compiler reads, not a
        // rendering one.
        py::class_<vb::GraphNode>(vbMod, "GraphNode")
            .def(py::init<>())
            .def_readwrite("Id", &vb::GraphNode::Id)
            .def_readwrite("TypeId", &vb::GraphNode::TypeId)
            // The author-facing instance label. GraphNode::Title and the YAML
            // `title:` key both existed in C++ and in the serializer, but were
            // never bound here -- so a saved graph could carry a title that
            // Python could neither read back nor set, and the authoring tests
            // for it failed against a feature that was half-shipped.
            .def_readwrite("Title", &vb::GraphNode::Title)
            .def_readwrite("X", &vb::GraphNode::X)
            .def_readwrite("Y", &vb::GraphNode::Y)
            .def_readwrite("EventName", &vb::GraphNode::EventName)
            .def_readwrite("Latent", &vb::GraphNode::Latent)
            .def_readwrite("Abort", &vb::GraphNode::Abort)
            // FindLiteral takes a const ref and returns a raw pointer with
            // return_value_policy::reference. Two consequences: the returned pointer is
            // borrowed, so it dangles if Literals is mutated afterwards; and returning a
            // pointer to a const type means pybind const_casts away the const to hand
            // Python a writable handle. A script can therefore write through what the
            // C++ API declares read-only.
            // Plain def_readwrite is correct now that PinLiteralList is
            // opaque: append on Literals reaches the node.
            .def_readwrite("Literals", &vb::GraphNode::Literals)
            .def_readwrite("ChildIds", &vb::GraphNode::ChildIds)
            // SetLiteral takes an explicit isSet defaulting to TRUE, so clearing a
            // literal requires passing False -- there is no ClearLiteral. That is the
            // deliberate inverse of PinLiteral's IsSet field being separate.
            .def("SetLiteral", &vb::GraphNode::SetLiteral, py::arg("pinId"),
                 py::arg("value"), py::arg("isSet") = true)
            // A lambda, not a member pointer: FindLiteral is overloaded
            // (const and non-const) and the const cast does not disambiguate
            // it for the compiler.
            .def("FindLiteral",
                 [](const vb::GraphNode &node, const std::string &pinId) -> const vb::PinLiteral * {
                     return node.FindLiteral(pinId);
                 },
                 py::arg("pinId"), py::return_value_policy::reference);

        // GraphLink is four strings: two node ids and two pin ids. No Type field --
        // compatibility is the validator's job, so a script can construct a link
        // that will not typecheck and only find out at Validate time.
        py::class_<vb::GraphLink>(vbMod, "GraphLink")
            .def(py::init<>())
            .def_readwrite("FromNode", &vb::GraphLink::FromNode)
            .def_readwrite("FromPin", &vb::GraphLink::FromPin)
            .def_readwrite("ToNode", &vb::GraphLink::ToNode)
            .def_readwrite("ToPin", &vb::GraphLink::ToPin);

        py::class_<vb::GraphVariable>(vbMod, "GraphVariable")
            .def(py::init<>())
            .def_readwrite("Name", &vb::GraphVariable::Name)
            .def_readwrite("Type", &vb::GraphVariable::Type)
            .def_readwrite("DefaultLiteral", &vb::GraphVariable::DefaultLiteral);

        py::class_<vb::BlackboardKey>(vbMod, "BlackboardKey")
            .def(py::init<>())
            .def_readwrite("Name", &vb::BlackboardKey::Name)
            .def_readwrite("Type", &vb::BlackboardKey::Type)
            .def_readwrite("DefaultLiteral", &vb::BlackboardKey::DefaultLiteral);

        // Graph is the root document. Nodes/Links/Variables/Blackboard are
        // def_readwrite over the OPAQUE container types, which is what makes append
        // and extend reach the real vector rather than a discarded list copy.
        py::class_<vb::Graph>(vbMod, "Graph")
            .def(py::init<>())
            .def_readwrite("Kind", &vb::Graph::Kind)
            .def_readwrite("Schema", &vb::Graph::Schema)
            .def_readwrite("Name", &vb::Graph::Name)
            // Plain def_readwrite is correct now that these container types
            // are opaque (see PYBIND11_MAKE_OPAQUE above): the getter hands
            // back the live std::vector rather than a list copy, so append
            // and extend reach the graph.
            .def_readwrite("Variables", &vb::Graph::Variables)
            .def_readwrite("Blackboard", &vb::Graph::Blackboard)
            .def_readwrite("Nodes", &vb::Graph::Nodes)
            .def_readwrite("Links", &vb::Graph::Links)
            // FindNode returns a borrowed pointer, same caveat as FindLiteral: it
            // dangles on any mutation that reallocates the Nodes vector, and the const is
            // cast away.
            // Lambda for the same reason as FindLiteral: FindNode is
            // overloaded and a member-pointer cast will not pick one.
            .def("FindNode",
                 [](const vb::Graph &graph, const std::string &id) -> const vb::GraphNode * {
                     return graph.FindNode(id);
                 },
                 py::arg("id"), py::return_value_policy::reference)
            .def("FindEvent", &vb::Graph::FindEvent,
                 py::return_value_policy::reference);

        // Live bindings for the opaque containers. This is what makes
        // `graph.Nodes.append(n)` actually add a node -- without these,
        // pybind11 refuses the opaque type outright rather than silently
        // copying, which would be a loud failure but still no way to build
        // a graph.
        // The five bind_vector calls are what make the opaque declarations at the
        // top of this file meaningful. Without them pybind would refuse the type
        // outright rather than silently copying -- a loud failure, but still no way
        // to build a graph from Python.
        py::bind_vector<vb::GraphNodeList>(vbMod, "GraphNodeList");
        py::bind_vector<vb::GraphLinkList>(vbMod, "GraphLinkList");
        py::bind_vector<vb::GraphVariableList>(vbMod, "GraphVariableList");
        py::bind_vector<vb::BlackboardKeyList>(vbMod, "BlackboardKeyList");
        py::bind_vector<vb::PinLiteralList>(vbMod, "PinLiteralList");

        py::class_<vb::GraphError>(vbMod, "GraphError")
            .def(py::init<>())
            .def_readwrite("Message", &vb::GraphError::Message)
            .def_readwrite("NodeId", &vb::GraphError::NodeId)
            .def_readwrite("PinId", &vb::GraphError::PinId)
            .def_readwrite("HasNode", &vb::GraphError::HasNode)
            .def_readwrite("IsError", &vb::GraphError::IsError)
            .def("Format", &vb::GraphError::Format, py::arg("fileName"));

        // ValidationResult.Findings is a read-only vector of GraphError. The three
        // count helpers exist so a caller can branch without iterating, which is
        // what an editor does on every keystroke.
        py::class_<vb::ValidationResult>(vbMod, "ValidationResult")
            .def(py::init<>())
            .def_readonly("Findings", &vb::ValidationResult::Findings)
            .def("HasErrors", &vb::ValidationResult::HasErrors)
            .def("ErrorCount", &vb::ValidationResult::ErrorCount)
            .def("WarningCount", &vb::ValidationResult::WarningCount)
            .def("Report", &vb::ValidationResult::Report, py::arg("fileName"));

        // NodeDef is read-only and not constructible -- it is the registry's view of
        // a node type. Pins on it are the same non-constructible PinDef as above.
        py::class_<vb::NodeDef>(vbMod, "NodeDef")
            .def_readonly("TypeId", &vb::NodeDef::TypeId)
            .def_readonly("Category", &vb::NodeDef::Category)
            .def_readonly("BtRole", &vb::NodeDef::BtRole)
            .def_readonly("IsStatement", &vb::NodeDef::IsStatement)
            .def_readonly("Pins", &vb::NodeDef::Pins);

        // ---- functions ----
        // Parse returns None on success or a GraphError on failure. The (ok, error)
        // tuple shape would also work, but None-or-error reads better for a function
        // whose failure carries a structured reason rather than just a message.
        vbMod.def("Parse", [](const std::string &text, vb::Graph &graph) -> py::object {
            vb::GraphError error;
            if (vb::GraphSerializer::Parse(text, graph, error))
                // The GraphError is CAST, not returned as a tuple. It is a bound type with
                // Message/NodeId/PinId/HasNode/IsError, so a script can inspect which pin on
                // which node failed rather than parsing a string.
                return py::none();
            return py::cast(error);
        // The docstring MUST come before py::arg. pybind counts trailing string
        // literals as annotations, so a docstring placed after py::arg("graph")
        // reads as a second argument and trips the static assert on argument count.
        }, py::arg("text"), py::arg("graph"), "Returns None on success, or a "
           "GraphError naming the problem. Never partially loads.");

        vbMod.def("Load", [](const std::string &path, vb::Graph &graph) -> py::object {
            vb::GraphError error;
            if (vb::GraphSerializer::Load(path, graph, error))
                return py::none();
            // Load is the file-reading twin of Parse. Same return contract, and the
            // same guarantee: a failed parse never partially populates the graph, so a
            // caller can retry after fixing the input.
            return py::cast(error);
        }, py::arg("path"), py::arg("graph"), "Returns None on success, or a "
           "GraphError naming the problem. Never partially loads.");

        // Emit is the inverse of Parse: graph to text. The docstring's promise of
        // DETERMINISM -- same graph, byte-identical output -- is what makes the
        // format diffable and safe to commit.
        vbMod.def("Emit", [](const vb::Graph &graph) {
            return vb::GraphSerializer::Emit(graph);
        }, py::arg("graph"), "Deterministic: the same graph always produces "
           "byte-identical text.");

        // Save shares Parse/Load's None-or-GraphError contract and therefore does
        // report failure. Contrast Scene::SaveScene higher up, which always
        // returns true and swallows the serializer's result.
        vbMod.def("Save", [](const std::string &path, const vb::Graph &graph) -> py::object {
            vb::GraphError error;
            if (vb::GraphSerializer::Save(path, graph, error))
                return py::none();
            return py::cast(error);
        // Save and Parse/Load share one GraphError contract. Unlike LoadScene on
        // the Scene class above, this one reports failure rather than always
        // returning true.
        }, py::arg("path"), py::arg("graph"));

        vbMod.def("Validate", [](const vb::Graph &graph) {
            vb::ValidationResult result;
            vb::Validator::Validate(graph, result);
            return result;
        // Validate, ValidateTree and ValidateFlow are three DIFFERENT checks bound
        // as three names. Validate is the general pass; the other two are specialised
        // for the two graph kinds. Nothing forces a caller to pick the right one,
        // so ValidateTree on a flow graph validates something meaningless.
        }, py::arg("graph"));

        vbMod.def("ValidateTree", [](const vb::Graph &graph) {
            vb::ValidationResult result;
            vb::Validator::ValidateTree(graph, result);
            return result;
        }, py::arg("graph"));

        vbMod.def("ValidateFlow", [](const vb::Graph &graph) {
            vb::ValidationResult result;
            vb::Validator::ValidateFlow(graph, result);
            return result;
        }, py::arg("graph"));

        // CompileBehavior and CompileFlow both validate FIRST, so a graph that
        // would not compile cannot produce output. CompileBehavior's docstring says
        // it returns '' on failure, while CompileFlow has no docstring at all --
        // the two callers must learn their contracts from different places.
        vbMod.def("CompileBehavior", [](const vb::Graph &graph) {
            vb::GraphError error;
            return vb::Compiler::CompileBehavior(graph, error);
        }, py::arg("graph"), "Returns the generated Python, or '' on failure "
           "(the graph is validated first).");

        vbMod.def("CompileFlow", [](const vb::Graph &graph) {
            vb::GraphError error;
            return vb::Compiler::CompileFlow(graph, error);
        }, py::arg("graph"));

        // The registry, read-only from Python. A new node is a C++ one-line
        // registration; authoring one from Python would put arbitrary code in
        // the generated file.
        // The registry is read-only from Python and the copies are deliberate:
        // returning the registry's own container would hand Python a view into
        // engine-owned memory. A new node type is a C++ registration -- authoring
        // one from Python would put arbitrary code in the generated file.
        // RegisteredNodes copies the whole registry. The loop takes each NodeDef
        // BY CONST REFERENCE here while BehaviorNodes below takes POINTERS and
        // dereferences -- two spellings of the same copy, which is why neither one
        // returns a view into engine memory.
        vbMod.def("RegisteredNodes", []() {
            std::vector<vb::NodeDef> copy;
            for (const vb::NodeDef &def : vb::NodeRegistry::Get().All())
                copy.push_back(def);
            return copy;
        });
        // BehaviorNodes and FlowNodes are the same registry filtered by kind. A
        // NodeDef carries a BtRole field, so the filter is a property of the
        // definition rather than a separate table.
        // The three registry readers each build a COPY vector. Returning the
        // registry's own container would expose engine-owned memory to Python; the
        // copy is the cost of not doing that.
        vbMod.def("BehaviorNodes", []() {
            std::vector<vb::NodeDef> copy;
            for (const vb::NodeDef *def : vb::NodeRegistry::Get().BehaviorNodes())
                copy.push_back(*def);
            return copy;
        });
        vbMod.def("FlowNodes", []() {
            std::vector<vb::NodeDef> copy;
            for (const vb::NodeDef *def : vb::NodeRegistry::Get().FlowNodes())
                copy.push_back(*def);
            return copy;
        });
        // HasNodeType is the cheap existence check -- one registry lookup, no
        // copying. It is what a graph editor should call while typing a connection,
        // rather than NodeInfo below.
        vbMod.def("HasNodeType", [](const std::string &typeId) {
            return vb::NodeRegistry::Get().IsRegistered(typeId);
        }, py::arg("typeId"));
        // NodeInfo returns None for an unknown id rather than raising, so a script
        // probing the registry does not need a try/except. Same None-or-value
        // contract as Parse and Validate above.
        vbMod.def("NodeInfo", [](const std::string &typeId) -> py::object {
            const vb::NodeDef *def = vb::NodeRegistry::Get().Find(typeId);
            if (def == nullptr)
                return py::none();
            return py::cast(*def);
        }, py::arg("typeId"));

        // ---- the .wlbtree compiler (NB.3) ----
        //
        // A facade over the shared serializer/validator/emitter, not a second
        // implementation. What is exposed here is the runtime-type contract and
        // the end-to-end path.
        // The behaviour-tree runtime type readers. RuntimeTypes/CompositeTypes/
        // DecoratorTypes return the engine's own category lists, and
        // IsRuntimeType is the membership test. These let a script build a tree
        // using only node types the runtime actually implements -- the failure they
        // prevent is a tree that validates but cannot execute.
        vbMod.def("RuntimeTypes", []() { return vb::BehaviorTree::RuntimeTypes(); });
        vbMod.def("IsRuntimeType", &vb::BehaviorTree::IsRuntimeType, py::arg("type"));
        // These four are one-line lambdas with no arguments beyond self, so the
        // py::call_guard machinery and the argument naming are all trivial here. The
        // grouping is by BEHAVIOURAL ROLE: composites sequence, decorators
        // single-child, leaves do the work.
        vbMod.def("CompositeTypes", []() { return vb::BehaviorTree::CompositeTypes(); });
        vbMod.def("DecoratorTypes", []() { return vb::BehaviorTree::DecoratorTypes(); });

        // Returns (ok, python, report). `report` is the full finding list, so a
        // designer fixing a tree sees every problem at once rather than one per
        // compile attempt.
        // CompileTree returns a FOUR-tuple -- (ok, python, report, message) --
        // against the docstring's comment above, which describes three. The report
        // is the full finding list so a designer sees every problem at once; the
        // separate message is the single blocking error. Four values where the note
        // says three is worth knowing before unpacking.
        vbMod.def("CompileTree", [](const vb::Graph &graph) {
            // The FOUR out-parameters of BehaviorTree::Compile -- python, validation,
            // error, plus the bool -- are the widest call in this file. All four are
            // filled by the callee; the lambda then projects them into a tuple, so the
            // C++ out-parameter style is invisible from Python.
            std::string python;
            vb::ValidationResult validation;
            vb::GraphError error;
            const bool ok = vb::BehaviorTree::Compile(graph, python, validation, error);
            return py::make_tuple(ok, python, validation.Report("graph"),
                                  error.Message);
        }, py::arg("graph"));

        // CompileTreeFile is the file-loading twin, taking a path instead of a graph
        // and naming the path in the report. Same four-tuple, so a caller can handle
        // both with one branch.
        vbMod.def("CompileTreeFile", [](const std::string &path) {
            std::string python;
            vb::ValidationResult validation;
            vb::GraphError error;
            const bool ok = vb::BehaviorTree::CompileFile(path, python, validation, error);
            return py::make_tuple(ok, python, validation.Report(path), error.Message);
        }, py::arg("path"));

        // SaveTreeFile refuses to write a graph that would not load back. That
        // round-trip check is the point: without it a tree could silently lose a
        // node on save and only fail much later, at load time.
        vbMod.def("SaveTreeFile",
                  [](const std::string &path, const vb::Graph &graph) -> py::object {
            vb::GraphError error;
            if (vb::BehaviorTree::SaveFile(path, graph, error))
                return py::none();
            return py::cast(error);
        }, py::arg("path"), py::arg("graph"), "Refuses to write a graph that "
           "would not load back, so a tree cannot silently lose a node on save.");

        // The three type-system helpers. IsAllowedOperator and IsAssignable are
        // bound as direct method pointers because they are plain functions over
        // enums; PinTypeName below needs a lambda because it must convert the
        // returned const char* to std::string for pybind.
        vbMod.def("IsAllowedOperator", &vb::IsAllowedOperator, py::arg("op"));
        // PinTypeName is the reverse of the enum: given a PinType, the name. A
        // script needs it to print a type in an error message, since the enum's own
        // str() is not reliably the name the graph format uses.
        vbMod.def("PinTypeName", [](vb::PinType type) {
            return std::string(vb::PinTypeName(type));
        }, py::arg("type"));
        // IsAssignable(from, to) is the compatibility test the VALIDATOR uses.
        // Exposing it lets a script check a connection before attempting it, rather
        // than building the link and discovering the problem at Validate time.
        vbMod.def("IsAssignable", &vb::IsAssignable, py::arg("from"), py::arg("to"));
        // ParseLiteral's tuple TYPE is declared explicitly rather than deduced from
        // the branches. Two branches whose elements deduce differently make pybind's
        // tuple types disagree, and the failure surfaces far from its cause.
        vbMod.def("ParseLiteral",
                  // ParseLiteral is the other half of the type system: text plus a target type
                  // to a value. It returns a std::tuple<bool, string, string> DECLARED
                  // EXPLICITLY -- see the note on the explicit tuple type above, which is
                  // exactly why this declaration exists.
                  [](vb::PinType type, const std::string &text) {
            // The tuple type is declared explicitly: two branches whose
            // elements deduce differently make pybind's tuple types disagree.
            std::tuple<bool, std::string, std::string> result;
            std::string value;
            std::string error;
            if (vb::ParseLiteral(type, text, value, error))
                result = std::make_tuple(true, value, std::string());
            else
                result = std::make_tuple(false, std::string(), error);
            // The closing brace of the Visual Blocks block. Everything after this -- the
            // Project bindings -- sits back at module scope.
            return result;
        }, py::arg("type"), py::arg("literal"));
    }

    // ---- Project ----------------------------------------------------
    //
    // Project had NO binding at all, which made every asset-path feature
    // untestable from Python: ScriptEngine resolves ScriptPath through
    // Project::GetAssetPath, and so does behaviour-tree attachment, so a
    // headless test had no way to set an asset root and could not exercise
    // either. Binding it is what makes those features testable at all.
    {
        // Project: the open project's paths and identity. Every property is
        // READ-ONLY -- IsOpen, Root, AssetRoot, Name, ProjectFile. There is no way
        // to move or rename a project from script, which is the safe default.
        py::class_<Wasteland::Project>(m, "Project")
            .def_property_readonly("IsOpen", &Wasteland::Project::IsOpen)
            .def_property_readonly("Root", &Wasteland::Project::GetRoot)
            .def_property_readonly("AssetRoot", &Wasteland::Project::GetAssetRoot)
            .def_property_readonly("Name", &Wasteland::Project::GetName)
            .def_property_readonly("ProjectFile", &Wasteland::Project::GetProjectFile)
            // The three path helpers are the interesting part: GetAssetPath resolves
            // RELATIVE to the asset root, GetProjectPath relative to the project root,
            // and MakeProjectRelative goes the other way. Using the wrong one yields a
            // path that looks valid and resolves to nothing.
            .def("GetAssetPath", &Wasteland::Project::GetAssetPath,
                 py::arg("relative"))
            .def("GetProjectPath", &Wasteland::Project::GetProjectPath,
                 py::arg("relative"))
            .def("MakeProjectRelative", &Wasteland::Project::MakeProjectRelative,
                 py::arg("absolute"))
            // Open is the only Project method that can fail, and it translates the
            // error into a Python exception -- throw std::runtime_error, which pybind
            // converts to RuntimeError. So unlike SaveScene above, this one does report
            // failure, and a script gets a message rather than a silent False.
            .def("Open", [](Wasteland::Project &self, const std::string &path) {
                std::string error;
                if (!self.Open(path, &error))
                    throw std::runtime_error(error);
            }, py::arg("path"))
            // IsOpen() stays FALSE for an attached asset root -- there is no
            // descriptor to save settings into. Asset resolution is the only
            // thing this changes.
            // AttachAssetRoot follows Open with the same error-to-exception pattern.
            // Both are the exceptions in this class -- everything else either has no
            // failure mode or returns a value.
            .def("AttachAssetRoot",
                 [](Wasteland::Project &self, const std::string &root) {
                     std::string error;
                     if (!self.AttachAssetRoot(root, &error))
                         throw std::runtime_error(error);
                 }, py::arg("root"))
            .def("Close", &Wasteland::Project::Close)
            // Project::Get is def_static with return_value_policy::reference, so it
            // returns the single open project as a live reference rather than a copy.
            // There is no constructor exposed, so a script cannot create a second
            // project -- the singleton is the only option.
            .def_static("Get", &Wasteland::Project::Get,
                        py::return_value_policy::reference)
            // Exists and the two directory getters are the non-throwing, side-effect
            // free queries. A script listing available projects should use Exists plus
            // GetUserProjectsDirectory rather than attempting to open each one.
            .def_static("Exists", &Wasteland::Project::Exists)
            .def_static("GetUserProjectsDirectory",
                        &Wasteland::Project::GetUserProjectsDirectory)
            .def_static("GetEngineAssetsRoot",
                        &Wasteland::Project::GetEngineAssetsRoot);
    }

    // Python-hosted tools do not own an Application whose destructor can clear
    // these process-wide pybind11 caches. Run cleanup from Python's atexit phase,
    // before CPython finalizes, so static maps never decref into a dead runtime.
    py::module_::import("atexit").attr("register")(
        py::cpp_function([]() {
            if (!Py_IsInitialized())
                return;
            Wasteland::BehaviorTreeSystem::Shutdown();
            Wasteland::ScriptEngine::Shutdown();
            Wasteland_SetPythonEventHandler(py::object());
        }));

}
