#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Wasteland
{
	class Scene;

	// Loads, compiles and ticks flow graphs.
	//
	// `BehaviorTreeSystem` is the sibling this mirrors, and the two are
	// deliberately the same shape: a project-relative path, compile once on
	// first tick, one Python instance per entity, a latch so a broken graph is
	// not retried at frame rate. A designer should not have to learn two
	// attachment models, and a bug found in one should be looked for in the
	// other.
	//
	// The one real difference is the ENTRY POINT. A behaviour tree exposes a
	// single `tick(seconds)`. A flow graph emits ONE METHOD PER EVENT, named
	// after the event node -- so the runtime has to ask the compiler which
	// method to call (`Compiler::FlowMethodNameFor`) rather than hardcoding a
	// name. That is why this system is not simply `BehaviorTreeSystem` with a
	// different component type.
	//
	// Nothing is written to the asset tree. The compiler emits Python source
	// and it is executed in an in-memory module, exactly as the tree path does.
	// A runtime that generated `.py` files beside the graph would make every
	// play session mutate the project, and would make "is this graph stale?"
	// a question the runtime has to answer. Compiling in memory removes both.
	//
	// Kept out of Scene.cpp because it needs ScriptEngine, Project and pybind
	// together, and because it is the unit a test wants to drive directly.
	class FlowGraphSystem
	{
	public:
		// Called every runtime tick for every entity carrying a
		// VisualBlocksComponent. Creates the instance on first sight.
		static void OnUpdateEntity(Scene &scene, Entity entity, float seconds);

		// Drop the cached instance for an entity being destroyed.
		static void OnDestroyEntity(UUID id);

		// Compile a graph without attaching it. Exposed for tooling and tests:
		// "does this authored graph still compile?" is a question worth asking
		// without a running scene.
		static bool CompileGraph(const std::string &path, std::string &outPython,
								 std::string &outError);

		// Drop every live instance. Must run while the interpreter is alive:
		// s_Instances is a namespace-scope static holding py::object, and
		// releasing those from a static destructor at teardown would touch
		// Python after interpreter finalization.
		// The live graph instance for an entity, or nullptr when it has none.
		//
		// The event dispatcher reaches a graph exactly the way the tick does, so
		// there is ONE lookup path rather than two. Without this the dispatcher
		// would have to reach into s_Instances, which is private for the same
		// reason CompileGraph is public.
		static py::object *GetInstance(UUID id);

		static void Shutdown();

		// ---- Per-node debugging (Task B.5) --------------------------------
		//
		// A breakpoint is a flag on a GRAPH NODE. The compiler emits a trace call
		// only on a marked node, so a graph nobody is debugging executes exactly
		// as it did before. The runtime installs a Python hook on the instance;
		// when that hook fires it pauses the entity and records the node.
		//
		// A pause is carried by an EXCEPTION that unwinds the tick, not by a
		// return code: the hook fires in the middle of a generated method, and
		// the only way to stop the rest of that method from running is to raise
		// out of it.

		// The entity currently paused, and the node it paused on.
		static std::string GetPausedNode(UUID id);
		static bool IsPaused(UUID id);

		// Continue past the current breakpoint. The node that paused the graph
		// runs to completion on the next tick.
		static void Resume(UUID id);

		// Run exactly ONE more node, then pause again. Implemented by clearing
		// the breakpoint that fired, ticking once, and restoring it -- so the
		// step boundary is the same code path as a real breakpoint rather than a
		// second, divergent one.
		static void Step(UUID id);

		// The Python exception type a pause raises. Created once, on first use,
		// because PyErr_NewException needs a live interpreter.
		static PyObject *PauseException();

	private:
		// UUID -> the instantiated graph class.
		static std::unordered_map<UUID, py::object> s_Instances;
		// UUID -> the method name the emitter wrote for that graph. Cached at
		// attach so the per-frame call is one attribute lookup rather than a
		// second parse of the graph file every frame.
		static std::unordered_map<UUID, std::string> s_Methods;
		// UUIDs whose graph failed to compile, so a broken graph logs once
		// instead of every single frame.
		static std::unordered_map<UUID, bool> s_Failed;

		// Exposed-variable bookkeeping (Task B.4). Names cache so the per-frame
		// path does not re-read the graph; the epoch map suppresses redundant
		// writes when the inspector has not touched anything.
		static std::unordered_map<UUID, std::vector<std::string>>
			s_ExposedNames;
		static std::unordered_map<UUID, std::map<std::string, std::string>>
			s_OverrideEpoch;

		// UUID -> the node that paused it. Empty means "not paused". Kept
		// separate from SuspendedTicks, which the behaviour tree also uses and
		// which counts ticks rather than naming a node.
		static std::unordered_map<UUID, std::string> s_PausedNode;

		// UUID -> a node allowed through ONCE. Set by Resume/Step so the
		// breakpoint that just fired does not fire again immediately.
		static std::unordered_map<UUID, std::string> s_SkipOnce;
	};

}
