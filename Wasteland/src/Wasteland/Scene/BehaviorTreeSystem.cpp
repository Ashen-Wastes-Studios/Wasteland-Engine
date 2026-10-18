// Forced-include PCH. Every translation unit in the project pulls it in first
// so the shared std / GLM / platform headers are parsed once per build instead
// of once per .cpp. Must stay the very first include in the file.
#include "wlpch.h"

// The system's own header: declares the two process-wide maps below plus the
// OnUpdateEntity / OnDestroyEntity hooks that Scene drives.
#include "BehaviorTreeSystem.h"

// Project::Get() supplies the asset root. A behaviour tree is authored inside a
// project, so its on-disk location is project-relative by definition.
#include "Wasteland/Project/Project.h"
// BehaviorTreeComponent (TreePath / TreeName / Attached / CompileError) and
// IDComponent, whose UUID is the key everything else hangs off.
#include "Wasteland/Scene/Components.h"
// The Scene& threaded into every tree instance below -- perception nodes have
// no other way to reach the world.
#include "Wasteland/Scene/Scene.h"
// CompileBehavior turns a Graph into Python source; ClassNameFor names the
// class that source emits.
#include "Wasteland/VisualBlocks/Compiler.h"
// The deserialized node graph, plus GraphError, which carries a raw Message
// and a Format() that prefixes the originating file.
#include "Wasteland/VisualBlocks/Graph.h"
// Reads the authored tree file from disk into a Graph.
#include "Wasteland/VisualBlocks/GraphSerializer.h"
// ValidationResult / Findings. Runs BEFORE the emitter -- see CompileTree.
#include "Wasteland/VisualBlocks/Validator.h"

// py::exec lives in pybind11/eval.h, not the main pybind11.h. Without it the
// module can be created and its dict touched, but the generated source can
// never be RUN -- which is the entire job here.
// Supplies py::exec. The general pybind11.h above does not.
#include <pybind11/eval.h>

// std::remove_if, for the control-character scrub in CleanPath.
#include <algorithm>
// std::filesystem::path / exists, for resolving and stat-ing the tree file.
#include <filesystem>
// std::string, and std::to_string. The latter matters below: UUID converts to
// uint64_t but has no std::string conversion of its own.
#include <string>

// Matches the header's namespace, so the definitions below attach to the
// already-declared members instead of declaring new ones.
namespace Wasteland
{
	// entity UUID -> the live py::object holding that tree's root instance.
	// Holding the object is what keeps the Python instance, and everything it
	// references, alive for as long as the entity exists.
	std::unordered_map<UUID, py::object> BehaviorTreeSystem::s_Instances;
	// entity UUID -> true. A latch, not a status: it is set on any failure and
	// consulted at the top of OnUpdateEntity, so a broken tree is not retried at
	// frame rate. Erased only by OnDestroyEntity or Shutdown.
	std::unordered_map<UUID, bool> BehaviorTreeSystem::s_Failed;

	// Internal-linkage helper -- `static` keeps it out of the header's ABI.
	// Normalizes an authored path: strip control characters, then trim padding.
	static std::string CleanPath(std::string path)
	{
		// Scene files carry authored text, so a stray CR or a padded field is
		// common. Same normalisation ScriptEngine applies to ScriptPath.
		// remove_if + erase is the trim-in-place idiom: remove_if only partitions, so
		// the surviving characters have to be compacted with erase afterwards.
		// Every C0 control byte is dropped here, CR included -- an authored file
		// saved on Windows and read back elsewhere otherwise leaves a stray CR
		// glued to the filename, and exists() then fails. The remove_if argument
		// list below is left uncommented so nothing is wedged inside the call.
		path.erase(std::remove_if(path.begin(), path.end(),
								  [](char c) { return (unsigned char)c < 32; }),
				   path.end());
		// Left trim: the first index that is not padding.
		size_t first = path.find_first_not_of(" \t\r\n");
		// npos here means the whole string was whitespace. Guarded because the substr
		// below would otherwise compute a length from an uninitialized `last`.
		if (first == std::string::npos)
			// An empty path is the useful outcome: the caller's exists() then fails with a
			// clear "not found at <project root>" message instead of a crash.
			return "";
		// Right trim, symmetric to `first`.
		size_t last = path.find_last_not_of(" \t\r\n");
		// Inclusive length (last - first + 1). An empty range is impossible, because
		// first != npos guarantees a real character at `first`.
		return path.substr(first, (last - first + 1));
	}

	// `wl_behavior` and every generated module live in the project's scripts
	// directory. Idempotent: this runs once per attaching entity, so check
	// before appending.
	// Guarantee the project's scripts/ directory is importable, so an emitted tree
	// can `import` its sibling modules. Idempotent by contract -- this runs once
	// per attaching entity, hence the scan below.
	static void EnsureScriptsOnPath()
	{
		// Function-local static. A namespace-scope py::module_ would be constructed
		// before the interpreter exists, so it must not exist at namespace scope.
		static py::module_ sys = py::module_::import("sys");
		// Resolved from the *active* project. Switching project mid-session appends the
		// new directory but never removes the old one, so sys.path accumulates.
		std::string scriptsDir = Project::Get().GetAssetPath("scripts").string();
		// sys.path is a list attribute, not a method. sysPath aliases the interpreter's
		// actual list -- that aliasing is what makes the append below take effect.
		py::list sysPath = sys.attr("path");
		// A linear scan of the whole path list on every call. Only runs once per
		// attaching entity, so it is not on the per-frame path.
		for (auto item : sysPath)
		{
			// Exact string comparison, so a trailing slash or a differently-spelled but
			// equivalent form of the same directory appends a duplicate entry rather than
			// being recognized as already present.
			if (item.cast<std::string>() == scriptsDir)
				return;
		}
		// Mutating the list object in place is what the interpreter observes; rebinding
		// a local py::list here would be invisible to it.
		sysPath.append(scriptsDir);
	}

	// Load, validate and compile one authored tree. Side-effect free with respect
	// to the scene and to Python: it only fills the out-parameters, which is why
	// the caller can run it inside the same try block as the exec() further down.
	// On success outError is deliberately left untouched, so callers must not read
	// it unless the returned bool was false.
	bool BehaviorTreeSystem::CompileTree(const std::string &path,
										 std::string &outPython,
										 std::string &outError)
	{
		// Project-relative, never the process working directory: an authored
		// tree has to load from the project its scene belongs to.
		// Project-relative resolution. An authored TreePath must never be interpreted
		// against the process working directory, which is wherever the editor happened
		// to be launched from.
		std::filesystem::path full =
			Project::Get().GetAssetPath(CleanPath(path));
		// Separator style is normalized now rather than at use sites, so the path in
		// the error message and the path handed to Load() below are the same string.
		full.make_preferred();

		// Existence is checked before parsing, so a missing file reports the path it
		// looked for instead of a parse error from an empty stream.
		if (!std::filesystem::exists(full))
		{
			// The *absolute resolved* path is echoed, not the authored string. That is
			// deliberate -- the author typed something project-relative and needs to be
			// told where it was actually looked for.
			outError = "behaviour tree not found at " + full.string();
			return false;
		}

		// The deserialized graph. Stack-allocated and passed by reference into Load;
		// the Validator and Compiler below both borrow it rather than copying it.
		VisualBlocks::Graph graph;
		// Reused by the compiler call further down. Safe to share only because the
		// load failure above formats it into outError before returning.
		VisualBlocks::GraphError error;
		// Load reports failure through a struct rather than by throwing, which is why
		// this is a bool test and not a try/catch.
		if (!VisualBlocks::GraphSerializer::Load(full.string(), graph, error))
		{
			// filename(), not full -- the message names the file the author recognises
			// instead of repeating the absolute path they have never typed.
			outError = error.Format(full.filename().string());
			return false;
		}

		// Validate BEFORE emitting. The emitter interpolates author-supplied
		// literals and operator strings into executable Python, so an
		// unvalidated graph must never reach it -- validating after emitting
		// would mean generating first and checking second.
		// Keeps every finding, not just the verdict, so warnings survive on a tree
		// that is about to be accepted.
		VisualBlocks::ValidationResult validation;
		// Returns false only for errors. The ordering is load-bearing rather than
		// stylistic: the emitter interpolates author-supplied literals into executable
		// Python, so an unvalidated graph must never reach it.
		if (!VisualBlocks::Validator::ValidateTree(graph, validation))
		{
			// Report() rolls every finding into one multi-line string, so an author fixing
			// a tree sees all of its problems at once instead of one per recompile.
			outError = validation.Report(full.filename().string());
			return false;
		}

		// Warnings are worth saying out loud -- a tree that validates but warns
		// usually has a branch that will never fire.
		// Logged against the authored TreePath, not the resolved absolute path, so the
		// line matches what is shown in the inspector field.
		for (const auto &f : validation.Findings)
		{
			// Errors already returned above, so anything still in the list is advisory.
			if (!f.IsError)
				// WARN rather than ERROR: the tree is attached and running. The cost is that
				// many entities sharing one authored tree emit the same line once each.
				WL_CORE_WARN("BehaviorTree {0}: {1}", path, f.Message);
		}

		// The one call that turns a graph into code. `error` is reused from the Load
		// above -- its contents were consumed into outError on the failure path, so
		// nothing of value is lost by handing it over again.
		outPython = VisualBlocks::Compiler::CompileBehavior(graph, error);
		// CompileBehavior returns void, so success is signalled by leaving Message
		// empty. There is no boolean to test here.
		if (!error.Message.empty())
		{
			outError = error.Format(full.filename().string());
			// Never hand back partial source. The caller only executes outPython on a true
			// return, but clearing it means a future caller that ignored the bool still
			// cannot exec() a half-built module.
			outPython.clear();
			return false;
		}
		// Success is the bool alone; outError is not cleared here and may still hold
		// text from an earlier call.
		return true;
	}

	// Per-entity, per-frame hook doing two jobs in order: lazily attach a tree to
	// an entity that has never had one, then tick whatever instance exists.
	// Every failure path below sets s_Failed and returns, which is what keeps one
	// broken entity from being retried sixty times a second.
	void BehaviorTreeSystem::OnUpdateEntity(Scene &scene, Entity entity,
											float seconds)
	{
		// Both halves matter. entity.IsValid() covers a handle destroyed earlier in
		// this same frame; the component test covers a perfectly live entity that
		// simply does not run behaviour.
		if (!entity.IsValid() || !entity.HasComponent<BehaviorTreeComponent>())
			return;

		// No IDComponent means no stable identity, so there is nowhere to key
		// an instance. Same guard ScriptEngine uses, for the same reason.
		// Without a stable identity there is no key for s_Instances, so the instance
		// could neither be ticked again next frame nor torn down on destroy. Skipping
		// is the only safe option: there is nothing to register it under.
		if (!entity.HasComponent<IDComponent>())
			return;

		// Copied by value. GetComponent hands back a reference into the entity's own
		// storage, and both maps below may rehash as this frame's calls run.
		auto id = entity.GetComponent<IDComponent>().ID;
		// A reference, deliberately -- tree.CompileError and tree.Attached are written
		// through it so the outcome shows up in the editor's inspector.
		auto &tree = entity.GetComponent<BehaviorTreeComponent>();

		// A tree that failed once fails forever. Retrying every frame would
		// flood the log with the same compile error at 60Hz.
		// The latch, read once per frame for every previously failed entity. Nothing in
		// this function can clear it, so a failed entity stays failed until it is
		// destroyed -- editing the .tree file on disk will not revive it.
		if (s_Failed.find(id) != s_Failed.end())
			return;

		// First frame for this entity, or recovery after the tick-failure path below
		// erased the instance. This attach path therefore serves double duty.
		if (s_Instances.find(id) == s_Instances.end())
		{
			// Generated module source, scoped to the attach block so the string is
			// released as soon as exec() has consumed it.
			std::string python;
			// Copied into tree.CompileError immediately below rather than aliased, since
			// this local dies with the enclosing block.
			std::string error;
			// Compile only -- nothing is executed yet, so a malformed emitted module cannot
			// have side effects at this point.
			if (!CompileTree(tree.TreePath, python, error))
			{
				// The single log line for a tree that will never attach. Carries both the
				// entity UUID and the authored path, so the log entry and the inspector entry
				// can be matched against each other.
				WL_CORE_ERROR("BehaviorTree: entity {0} could not attach '{1}': {2}",
							  id, tree.TreePath, error);
				// Published onto the component so the failure is visible from the editor
				// without opening the log.
				tree.CompileError = error;
				s_Failed[id] = true;
				return;
			}

			// Everything from here on crosses into the interpreter, which signals failure
			// by throwing py::error_already_set rather than by returning an error code.
			try
			{
				// Before exec, because a generated module can `import` at class-definition
				// time, not only inside a method body.
				EnsureScriptsOnPath();

				// The emitted class name comes from the graph's own `name:`,
				// sanitized by the compiler. Ask the compiler rather than
				// re-deriving the rule here -- duplicating it is how the two
				// drift and the runtime fails to find a class it just compiled.
				VisualBlocks::Graph graph;
				// Separate from CompileTree's error object: that call has already returned
				// and its diagnostics are already in the inspector.
				VisualBlocks::GraphError gerror;
				// Same resolution CompileTree performed, CleanPath included -- so the class
				// name is read from exactly the file that produced the Python above. The cost
				// is a second full parse: CompileTree's graph was a local there and was never
				// exposed. Paid once per attaching entity, never per frame.
				std::string full =
					Project::Get().GetAssetPath(CleanPath(tree.TreePath)).string();
				// Load the same file a second time. Reaching this failure branch means the
				// tree file changed on disk between the two reads -- plausible with an editor
				// open on the asset -- and the entity is latched rather than retried.
				if (!VisualBlocks::GraphSerializer::Load(full, graph, gerror))
				{
					// Formatted with the FULL path here, while CompileTree above used filename().
					// The same underlying failure therefore produces two different message shapes
					// depending on which of the two loads rejected the file.
					tree.CompileError = gerror.Format(full);
					s_Failed[id] = true;
					return;
				}
				// TreeName is an optional author override. When it is left empty the name comes
				// from the graph itself, so an explicitly wrong TreeName is not detected here
				// -- it surfaces as the missing-class check below instead.
				std::string className =
					tree.TreeName.empty()
						? VisualBlocks::Compiler::ClassNameFor(graph)
						: tree.TreeName;

				// One module per attaching entity, named from the UUID, so two
				// NPCs running the same authored tree neither share nor clobber
				// one module in sys.modules -- and so a recompiled tree
				// actually replaces the running code.
				// UUID converts to uint64_t; it has no std::string
				// conversion of its own, so to_string() is the correct route
				// (matching the fmt formatter in UUID.h).
				// One module per attaching entity, named from the UUID, so two NPCs running the
				// same authored tree neither share nor clobber a sys.modules entry -- and so a
				// recompiled tree genuinely replaces the running code rather than mutating a
				// module some other entity is still executing.
				std::string moduleName =
					// UUID casts to uint64_t: it has no std::string conversion of its own, so
					// to_string is the only route (and matches the fmt formatter in UUID.h).
					"_wlbtree_" + std::to_string((uint64_t)id);
				// The generated name has to be a legal Python identifier before it is used as
				// one in sys.modules and in the hasattr() lookup below.
				for (auto &c : moduleName)
				{
					// The unsigned char cast is not decoration: isalnum is undefined for a negative
					// char, and the UUID rendered as a signed char is readily negative.
					if (!(std::isalnum((unsigned char)c) || c == '_'))
						c = '_';
				}

				// Re-imported rather than reusing the function-local static in
				// EnsureScriptsOnPath, because this call needs sys.modules specifically and not
				// sys.path.
				py::module_ sys = py::module_::import("sys");

				// types.ModuleType, and the name must go in as a py::str --
				// the Python-level constructor takes an object, not a char*.
				// `types.Module` does not exist, and it fails at RUNTIME with
				// a bare AttributeError that surfaces as an NPC that never
				// attaches and never says why.
				// types.ModuleType is the constructor. Reached through the module attribute
				// rather than through a generated `import types`, so the lookup happens once
				// per attach on the C++ side.
				py::module_ typesMod = py::module_::import("types");
				// Built empty and populated below, never fetched from sys.modules -- so a stale
				// module left behind by an earlier attach of the same entity cannot be reused
				// by mistake.
				py::object mod = typesMod.attr("ModuleType")(py::str(moduleName));
				// exec() takes the namespace DICT rather than the module, which is why the
				// __dict__ attr() here is not optional: without it the generated class would
				// land on this C++ frame instead of on `mod`.
				py::exec(python, mod.attr("__dict__"));
				// Registered only after exec has fully returned. A module whose body raised
				// half way through is destroyed with the local `mod` and never becomes visible
				// to any import.
				sys.attr("modules")[py::str(moduleName)] = mod;

				// exec() succeeded but bound a different name: the class the emitter produced
				// and the className computed above disagree. Checked explicitly so the
				// mismatch is reported once, here, instead of resurfacing as an AttributeError
				// on `tick` for the rest of the entity's life.
				if (!py::hasattr(mod, py::str(className)))
				{
					// Reachable in practice whenever TreeName is set to something the compiler did
					// not use as the class name -- the override is trusted at line 160 and never
					// cross-checked against the graph.
					WL_CORE_ERROR(
						"BehaviorTree: '{0}' compiled but emitted no class '{1}'",
						tree.TreePath, className);
					tree.CompileError =
						"compiled but emitted no class " + className;
					s_Failed[id] = true;
					return;
				}

				// `scene` is passed explicitly: perception nodes need a world to
				// search, and an Entity carries no back-reference to its Scene.
				// The root object is constructed with two arguments: the Entity, and a raw
				// pointer to the Scene. The Scene is passed as &scene because an Entity carries
				// no back-reference to it and perception nodes must have a world to search.
				// That raw pointer is a lifetime hazard -- the instance stored on the left
				// outlives the frame it was taken in, so a tree that stashes the Scene
				// reference has a dangling pointer the moment the Scene is destroyed. (The
				// interpreter-visible contract for the constructor itself is an inference from
				// the emitted code, not something this file can prove.)
				s_Instances[id] = mod.attr(py::str(className))(entity, &scene);
				// The inspector flag, set only after construction succeeded. DEFECT: the
				// tick-failure path lower down erases the instance but never clears this, so
				// an entity reads as attached in the editor after its tree has thrown and been
				// detached.
				tree.Attached = true;
			}
			// Anything Python raised during attach -- a failing import, a module-body
			// exception, a constructor that threw. e.what() carries pybind11's formatted
			// traceback for the pending Python error, which is what makes this the only
			// useful diagnostic for a tree that fails to attach silently.
			catch (const py::error_already_set &e)
			{
				// "failed to start" distinguishes a runtime attach failure from the compile
				// failure above, even though both take the same latch.
				WL_CORE_ERROR("BehaviorTree: entity {0} failed to start '{1}': {2}",
							  id, tree.TreePath, e.what());
				tree.CompileError = e.what();
				s_Failed[id] = true;
				return;
			}
		}

		try
		{
			// The per-frame call. `tick` is resolved by attribute access on every frame
			// rather than being cached as a bound method, so each frame pays a Python dict
			// lookup per entity before any tree logic runs.
			s_Instances[id].attr("tick")(seconds);
		}
		catch (const py::error_already_set &e)
		{
			// A tree that throws must not throw every frame. Detach it and
			// record why, so one bad tree degrades one entity instead of
			// flooding the log and stalling the frame.
			// Logged exactly once, because the latch below stops it recurring. The entity
			// itself survives and keeps being visited every frame -- it simply no longer
			// runs a tree.
			WL_CORE_ERROR("BehaviorTree: entity {0} threw while ticking '{1}': {2}",
						  id, tree.TreePath, e.what());
			tree.CompileError = e.what();
			s_Failed[id] = true;
			// Dropping the last py::object reference releases the instance immediately, so
			// any Python __del__ runs now rather than at shutdown.
			s_Instances.erase(id);
		}
	}

	// Scene teardown hook, keyed by the same UUID as both maps. The erase below
	// releases the Python object -- which can run arbitrary __del__ code, so this
	// is not a guaranteed-cheap call -- and is what stops a destroyed entity's tree
	// object, and the Scene pointer it was handed at line 205, from outliving it.
	void BehaviorTreeSystem::OnDestroyEntity(UUID id)
	{
		s_Instances.erase(id);
		// The latch goes too, so failure state cannot outlive the entity it describes.
		s_Failed.erase(id);
	}

	// Drops every live tree at once. This must happen while the interpreter is
	// still alive: s_Instances is a namespace-scope static holding py::object, and
	// releasing those from a static destructor at program teardown would touch
	// Python after interpreter finalization.
	void BehaviorTreeSystem::Shutdown()
	{
		// Cleared in unspecified order (the container is unordered), so the order in
		// which trees run their destructors is not something a tree may depend on.
		s_Instances.clear();
		// The latches are cleared too, so reloading a project starts from a clean slate
		// and a tree that failed to compile once gets another attempt.
		s_Failed.clear();
	}

} // namespace Wasteland