#pragma once

#include <string>
#include <unordered_map>

#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Wasteland
{
	class Scene;

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

		static void Shutdown();

	private:
		// UUID -> the instantiated tree class.
		static std::unordered_map<UUID, py::object> s_Instances;
		// UUIDs whose tree failed to compile, so a broken tree logs once
		// instead of every single frame.
		static std::unordered_map<UUID, bool> s_Failed;
	};

}