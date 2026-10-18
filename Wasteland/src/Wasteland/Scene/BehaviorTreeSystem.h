#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Wasteland
{
	class Scene;

	// Immutable snapshot record copied out of the per-entity trace ring. The
	// node ID identifies the authored node; NodeType and Event preserve the
	// runtime's actual event (enter, exit:<status>, abort) without UI synthesis.
	struct BehaviorTreeTraceEntry
	{
		uint64_t Tick = 0;
		std::string NodeId;
		std::string NodeType;
		std::string Event;
	};

	// Loads, compiles and ticks behaviour trees.
	//
	// Without this, a Visual Blocks behaviour tree is a DEMO: the engine can
	// load, validate and compile one, and Python can import and tick the
	// generated class -- but nothing in the scene ever asks an entity to own
	// one. `Enemy.py` therefore had to stay 200 lines of imperative code,
	// because that was the only way an NPC actually behaved.
	//
	// `BehaviorTreeComponent` (Components.h) mirrors `ScriptComponent`
	// deliberately: same shape (a project-relative path plus a class name),
	// same Project-relative resolution. A designer should not have to learn
	// two different attachment models. The tree compiles ONCE, on first tick,
	// and the generated module is instantiated per entity -- late attachment
	// is the normal case (a spawned enemy), so that path is the one that
	// matters.
	//
	// Kept out of Scene.cpp because it needs ScriptEngine, Project and pybind
	// together, and because it is the unit a test wants to drive directly.
	class BehaviorTreeSystem
	{
	public:
		// Called every runtime tick for every entity carrying a
		// BehaviorTreeComponent. Creates the instance on first sight.
		static void OnUpdateEntity(Scene &scene, Entity entity, float seconds);

		// Drop the cached instance for an entity being destroyed.
		static void OnDestroyEntity(UUID id);

		// Compile a tree without attaching it. Exposed for tooling and tests:
		// "does this authored tree still compile?" is a question worth asking
		// without a running scene.
		static bool CompileTree(const std::string &path, std::string &outPython,
								std::string &outError);

		// Tracing is opt-in per entity. Enabling before first attach is supported;
		// the next instance gets a hook. Disabling detaches the hook but preserves
		// the bounded log for inspection. GetTraceLog always returns a copy.
		static bool SetTrace(UUID id, bool enabled);
		static bool IsTraceEnabled(UUID id);
		static std::vector<BehaviorTreeTraceEntry> GetTraceLog(UUID id);

		static void Shutdown();

	private:
		static bool SetPythonTrace(UUID id, py::object &instance, bool enabled);
		static void AppendTrace(UUID id, uint64_t tick,
							const std::string &nodeId,
								const std::string &nodeType,
								const std::string &event);
		static constexpr size_t kTraceCapacity = 512;
		static std::unordered_set<UUID> s_TraceEnabled;
		static std::unordered_map<UUID, std::vector<BehaviorTreeTraceEntry>> s_TraceLogs;
		static std::unordered_map<UUID, std::unordered_map<std::string, std::string>> s_NodeTypes;
		// UUID -> the instantiated tree class.
		static std::unordered_map<UUID, py::object> s_Instances;
		// UUIDs whose tree failed to compile, so a broken tree logs once
		// instead of every single frame.
		static std::unordered_map<UUID, bool> s_Failed;
	};

}