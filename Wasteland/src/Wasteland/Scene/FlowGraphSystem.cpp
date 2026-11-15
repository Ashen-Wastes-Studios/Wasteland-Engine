// Forced-include PCH. Every translation unit in the project pulls it in first
// so the shared std / GLM / platform headers are parsed once per build instead
// of once per .cpp. Must stay the very first include in the file.
#include "wlpch.h"

// The system's own header: declares the three process-wide maps below plus the
// OnUpdateEntity / OnDestroyEntity hooks that Scene drives.
#include "FlowGraphSystem.h"

// Project::Get() supplies the asset root. A flow graph is authored inside a
// project, so its on-disk location is project-relative by definition.
#include "Wasteland/Project/Project.h"
// VisualBlocksComponent (GraphPath / GraphName / Attached / CompileError) and
// IDComponent, whose UUID is the key everything else hangs off.
#include "Wasteland/Scene/Components.h"
// The Scene& threaded into every graph instance below -- nodes that read or
// write the world have no other way to reach it.
#include "Wasteland/Scene/Scene.h"
// BeginPlay dispatch for a graph that attaches mid-play (Task B.1). Same
// directory, no layering break: the dispatcher already sits above the graph.
#include "Wasteland/Scene/ScriptEventDispatcher.h"
// CompileFlow turns a Graph into Python source; FlowClassNameFor and
// FlowMethodNameFor name the class and the event method that source emits.
#include "Wasteland/VisualBlocks/Compiler.h"
// The deserialized node graph, plus GraphError, which carries a raw Message
// and a Format() that prefixes the originating file.
#include "Wasteland/VisualBlocks/Graph.h"
// Reads the authored graph file from disk into a Graph.
#include "Wasteland/VisualBlocks/GraphSerializer.h"
// ValidationResult / Findings. Runs BEFORE the emitter -- see CompileGraph.
#include "Wasteland/VisualBlocks/Validator.h"

// py::exec lives in pybind11/eval.h, not the main pybind11.h. Without it the
// module can be created and its dict touched, but the generated source can
// never be RUN -- which is the entire job here.
#include <pybind11/eval.h>

// std::remove_if, for the control-character scrub in CleanPath.
#include <algorithm>
#include <filesystem>
// std::string, and std::to_string. The latter matters below: UUID converts to
// uint64_t but has no std::string conversion of its own.
#include <string>

// Matches the header's namespace, so the definitions below attach to the
// already-declared members instead of declaring new ones.
namespace Wasteland
{
	// entity UUID -> the live py::object holding that graph's instance. Holding
	// the object is what keeps the Python instance, and everything it
	// references, alive for as long as the entity exists.
	std::unordered_map<UUID, py::object> FlowGraphSystem::s_Instances;

	py::object *FlowGraphSystem::GetInstance(UUID id)
	{
		// A pointer into the map, not a copy: the dispatcher calls the instance
		// in place. Valid until the next erase on this map, which only happens
		// on destroy or shutdown -- neither of which can run inside a dispatch.
		auto found = s_Instances.find(id);
		return found == s_Instances.end() ? nullptr : &found->second;
	}
	std::unordered_map<UUID, std::string> FlowGraphSystem::s_Methods;
	// entity UUID -> true. A latch, not a status: it is set on any failure and
	// consulted at the top of OnUpdateEntity, so a broken graph is not retried
	// at frame rate. Erased only by OnDestroyEntity or Shutdown.
	std::unordered_map<UUID, bool> FlowGraphSystem::s_Failed;

	// Exposed-variable bookkeeping (Task B.4). s_ExposedNames caches the graph's
	// editable variable names so the per-frame re-application does not re-read
	// the graph file; s_OverrideEpoch is the last map written, so a frame that
	// changed nothing writes nothing.
	std::unordered_map<UUID, std::vector<std::string>>
		FlowGraphSystem::s_ExposedNames;
	std::unordered_map<UUID, std::map<std::string, std::string>>
		FlowGraphSystem::s_OverrideEpoch;

	std::unordered_map<UUID, std::string> FlowGraphSystem::s_PausedNode;

	// UUID -> a node that must be allowed through ONCE. Set by Resume/Step so
	// the breakpoint that just fired does not immediately fire again, which
	// would make Continue and Step both no-ops.
	std::unordered_map<UUID, std::string> FlowGraphSystem::s_SkipOnce;

	// The pause exception, created on first use because PyErr_NewException needs
	// a live interpreter. Held as a PyObject* and leaked deliberately: it is a
	// process-lifetime type object, and releasing it during static destruction
	// would touch Python after finalization -- the same reason s_Instances is
	// drained explicitly in Shutdown().
	PyObject *FlowGraphSystem::PauseException()
	{
		static PyObject *type = nullptr;
		if (type == nullptr)
		{
			// A name, a docstring, and BaseException as the base. It must not
			// derive from RuntimeError or anything a graph could plausibly raise,
			// or a `catch` in generated code could swallow a pause.
			type = PyErr_NewException("Wasteland.VbPause", nullptr, nullptr);
		}
		return type;
	}

	std::string FlowGraphSystem::GetPausedNode(UUID id)
	{
		auto entry = s_PausedNode.find(id);
		return entry == s_PausedNode.end() ? std::string() : entry->second;
	}

	bool FlowGraphSystem::IsPaused(UUID id)
	{
		return s_PausedNode.find(id) != s_PausedNode.end();
	}

	void FlowGraphSystem::Resume(UUID id)
	{
		// Clears the pause and arranges for the node that fired to run once
		// without re-pausing. The breakpoint itself is left SET -- in UE,
		// "Continue" runs until the NEXT breakpoint, it does not silently
		// disable the one you just hit, and a second hit on the same node later
		// in the graph's execution is exactly what you want.
		auto entry = s_PausedNode.find(id);
		if (entry != s_PausedNode.end())
		{
			s_SkipOnce[id] = entry->second;
			s_PausedNode.erase(entry);
		}
	}

	void FlowGraphSystem::Step(UUID id)
	{
		// "Step Over": run the paused node and stop at the next one. Same
		// mechanism as Resume -- skip the node that fired once -- but the
		// caller is expected to tick exactly once before checking IsPaused.
		auto entry = s_PausedNode.find(id);
		if (entry == s_PausedNode.end())
			return;
		s_SkipOnce[id] = entry->second;
		s_PausedNode.erase(entry);
	}

	// Internal-linkage helper -- `static` keeps it out of the header's ABI.
	// Normalizes an authored path: strip control characters, then trim padding.
	static std::string CleanPath(std::string path)
	{
		// Scene files carry authored text, so a stray CR or a padded field is
		// common. Same normalisation ScriptEngine applies to ScriptPath.
		// Every C0 control byte is dropped here, CR included -- an authored file
		// saved on Windows and read back elsewhere otherwise leaves a stray CR
		// glued to the filename, and exists() then fails.
		path.erase(std::remove_if(path.begin(), path.end(),
								  [](char c) { return (unsigned char)c < 32; }),
				   path.end());
		// Left trim: the first index that is not padding.
		size_t first = path.find_first_not_of(" \t\r\n");
		// npos here means the whole string was whitespace. Guarded because the
		// substr below would otherwise compute a length from an uninitialized
		// `last`.
		if (first == std::string::npos)
			// An empty path is the useful outcome: the caller's exists() then
			// fails with a clear "not found at <project root>" message instead of
			// a crash.
			return "";
		// Right trim, symmetric to `first`.
		size_t last = path.find_last_not_of(" \t\r\n");
		return path.substr(first, (last - first + 1));
	}

	// Guarantee the project's scripts/ directory is importable, so an emitted
	// graph can `import` its sibling modules. Idempotent by contract -- this
	// runs once per attaching entity, hence the scan below.
	static void EnsureScriptsOnPath()
	{
		// Keep this handle scoped to the attach operation. A function-local
		// static survives until DLL/process teardown, which can run after CPython
		// has finalized and make its py::object destructor touch a dead
		// interpreter. Python caches imported modules, so a local handle does not
		// repeat module initialization and is released while the interpreter is
		// still active.
		py::module_ sys = py::module_::import("sys");
		// Resolved from the *active* project. Switching project mid-session
		// appends the new directory but never removes the old one.
		std::string scriptsDir = Project::Get().GetAssetPath("scripts").string();
		// sys.path is a list attribute, not a method. sysPath aliases the
		// interpreter's actual list -- that aliasing is what makes the append
		// below take effect.
		py::list sysPath = sys.attr("path");
		// A linear scan of the whole path list on every call. Only runs once per
		// attaching entity, so it is not on the per-frame path.
		for (auto item : sysPath)
		{
			// Exact string comparison, so a trailing slash or a differently
			// spelled but equivalent form of the same directory appends a
			// duplicate entry rather than being recognized as present.
			if (item.cast<std::string>() == scriptsDir)
				return;
		}
		// Mutating the list object in place is what the interpreter observes;
		// rebinding a local py::list here would be invisible to it.
		sysPath.append(scriptsDir);
	}

	// Load, validate and compile one authored graph. Side-effect free with
	// respect to the scene and to Python: it only fills the out-parameters,
	// which is why the caller can run it inside the same try block as the exec()
	// further down. On success outError is deliberately left untouched, so
	// callers must not read it unless the returned bool was false.
	bool FlowGraphSystem::CompileGraph(const std::string &path,
									   std::string &outPython,
									   std::string &outError)
	{
		// Project-relative, never the process working directory: an authored
		// graph has to load from the project its scene belongs to.
		std::filesystem::path full =
			Project::Get().GetAssetPath(CleanPath(path));
		// Separator style is normalized now rather than at use sites, so the
		// path in the error message and the path handed to Load() below are the
		// same string.
		full.make_preferred();

		// Existence is checked before parsing, so a missing file reports the
		// path it looked for instead of a parse error from an empty stream.
		if (!std::filesystem::exists(full))
		{
			// The *absolute resolved* path is echoed, not the authored string:
			// the author typed something project-relative and needs to be told
			// where it was actually looked for.
			outError = "flow graph not found at " + full.string();
			return false;
		}

		// The deserialized graph. Stack-allocated and passed by reference into
		// Load; the Validator and Compiler below both borrow it.
		VisualBlocks::Graph graph;
		// Reused by the compiler call further down. Safe to share only because
		// the load failure above formats it into outError before returning.
		VisualBlocks::GraphError error;
		if (!VisualBlocks::GraphSerializer::Load(full.string(), graph, error))
		{
			// filename(), not full -- the message names the file the author
			// recognises instead of repeating the absolute path they never typed.
			outError = error.Format(full.filename().string());
			return false;
		}

		// Task B.2: where this graph lives, so a function's relative `path:`
		// resolves against the OWNING graph's directory. Set before validation
		// because a CallFunction node's pins are resolved from the loaded
		// function signatures, and the compiler needs the same base.
		//
		// parent_path() rather than the full path: a function path is relative to
		// the graph, not to the filesystem root.
		graph.BasePath = full.parent_path().string();

		// Validate BEFORE emitting. The emitter interpolates author-supplied
		// literals and operator strings into executable Python, so an
		// unvalidated graph must never reach it -- validating after emitting
		// would mean generating first and checking second.
		VisualBlocks::ValidationResult validation;
		if (!VisualBlocks::Validator::ValidateFlow(graph, validation))
		{
			// Report() rolls every finding into one multi-line string, so an
			// author fixing a graph sees all of its problems at once instead of
			// one per recompile.
			outError = validation.Report(full.filename().string());
			return false;
		}

		// Warnings are worth saying out loud -- a graph that validates but warns
		// usually has a branch that will never fire.
		for (const auto &f : validation.Findings)
		{
			if (!f.IsError)
				WL_CORE_WARN("FlowGraph {0}: {1}", path, f.Message);
		}

		// The one call that turns a graph into code. `error` is reused from the
		// Load above -- its contents were consumed into outError on the failure
		// path, so nothing of value is lost by handing it over again.
		outPython = VisualBlocks::Compiler::CompileFlow(graph, error);
		// CompileFlow returns void, so success is signalled by leaving Message
		// empty. There is no boolean to test here.
		if (!error.Message.empty())
		{
			outError = error.Format(full.filename().string());
			// Never hand back partial source. The caller only executes
			// outPython on a true return, but clearing it means a future caller
			// that ignored the bool still cannot exec() a half-built module.
			outPython.clear();
			return false;
		}
		// Success is the bool alone; outError is not cleared here and may still
		// hold text from an earlier call.
		return true;
	}

	// Per-entity, per-frame hook doing two jobs in order: lazily attach a graph
	// to an entity that has never had one, then tick whatever instance exists.
	// Every failure path below sets s_Failed and returns, which is what keeps
	// one broken entity from being retried sixty times a second.
	void FlowGraphSystem::OnUpdateEntity(Scene &scene, Entity entity,
										 float seconds)
	{
		// Both halves matter. entity.IsValid() covers a handle destroyed earlier
		// in this same frame; the component test covers a perfectly live entity
		// that simply does not run a graph.
		if (!entity.IsValid() || !entity.HasComponent<VisualBlocksComponent>())
			return;
		if (!Py_IsInitialized())
			return;
		py::gil_scoped_acquire gil;

		// No IDComponent means no stable identity, so there is nowhere to key an
		// instance -- the same guard ScriptEngine and BehaviorTreeSystem use.
		if (!entity.HasComponent<IDComponent>())
			return;

		// Copied by value. GetComponent hands back a reference into the entity's
		// own storage, and both maps below may rehash as this frame's calls run.
		auto id = entity.GetComponent<IDComponent>().ID;
		// A reference, deliberately -- graph.CompileError and graph.Attached are
		// written through it so the outcome shows up in the editor's inspector.
		auto &graph = entity.GetComponent<VisualBlocksComponent>();

		// A graph that failed once fails forever. Retrying every frame would
		// flood the log with the same compile error at 60Hz.
		if (s_Failed.find(id) != s_Failed.end())
			return;

		// First frame for this entity, or recovery after the tick-failure path
		// below erased the instance. This attach path therefore serves double
		// duty.
		if (s_Instances.find(id) == s_Instances.end())
		{
			// Generated module source, scoped to the attach block so the string
			// is released as soon as exec() has consumed it.
			std::string python;
			// Copied into graph.CompileError immediately below rather than
			// aliased, since this local dies with the enclosing block.
			std::string error;
			// Compile only -- nothing is executed yet, so a malformed emitted
			// module cannot have side effects at this point.
			if (!CompileGraph(graph.GraphPath, python, error))
			{
				WL_CORE_ERROR("FlowGraph: entity {0} could not attach '{1}': {2}",
							  id, graph.GraphPath, error);
				// Published onto the component so the failure is visible from
				// the editor without opening the log.
				graph.CompileError = error;
				s_Failed[id] = true;
				return;
			}

			// Everything from here on crosses into the interpreter, which signals
			// failure by throwing py::error_already_set rather than by returning
			// an error code.
			try
			{
				// Before exec, because a generated module can `import` at
				// class-definition time, not only inside a method body.
				EnsureScriptsOnPath();

				// The class and method names come from the graph's own name and
				// event, sanitised by the compiler. Ask the compiler rather than
				// re-deriving the rules here -- duplicating them is how the two
				// drift and the runtime fails to find a class it just compiled.
				VisualBlocks::Graph parsed;
				// Separate from CompileGraph's error object: that call has
				// already returned and its diagnostics are already in the
				// inspector.
				VisualBlocks::GraphError gerror;
				// Same resolution CompileGraph performed, CleanPath included --
				// so the names are read from exactly the file that produced the
				// Python above. The cost is a second full parse: CompileGraph's
				// graph was a local there and was never exposed. Paid once per
				// attaching entity, never per frame.
				std::string full =
					Project::Get().GetAssetPath(CleanPath(graph.GraphPath)).string();
				// Reaching this failure branch means the graph file changed on
				// disk between the two reads -- plausible with an editor open on
				// the asset -- and the entity is latched rather than retried.
				if (!VisualBlocks::GraphSerializer::Load(full, parsed, gerror))
				{
					graph.CompileError = gerror.Format(full);
					s_Failed[id] = true;
					return;
				}

				// GraphName is an optional author override. When it is left empty
				// the name comes from the graph itself, so an explicitly wrong
				// GraphName is not detected here -- it surfaces as the
				// missing-class check below instead.
				std::string className =
					graph.GraphName.empty()
						? VisualBlocks::Compiler::FlowClassNameFor(parsed)
						: graph.GraphName;

				// The event method, from the compiler, so a renamed event needs
				// no change here.
				std::string methodName =
					VisualBlocks::Compiler::FlowMethodNameFor(parsed);

				// One module per attaching entity, named from the UUID, so two
				// entities running the same authored graph neither share nor
				// clobber one module in sys.modules -- and so a recompiled graph
				// actually replaces the running code.
				std::string moduleName =
					"_wlflow_" + std::to_string((uint64_t)id);
				// The generated name has to be a legal Python identifier before
				// it is used as one in sys.modules and in the hasattr() lookup
				// below.
				for (auto &c : moduleName)
				{
					// The unsigned char cast is not decoration: isalnum is
					// undefined for a negative char, and the UUID rendered as a
					// signed char is readily negative.
					if (!(std::isalnum((unsigned char)c) || c == '_'))
						c = '_';
				}

				py::module_ sys = py::module_::import("sys");

				// types.ModuleType is the constructor -- `types.Module` does not
				// exist and fails at runtime with a bare AttributeError. Built
				// empty and populated below, never fetched from sys.modules, so
				// a stale module left behind by an earlier attach of the same
				// entity cannot be reused by mistake.
				py::module_ typesMod = py::module_::import("types");
				py::object mod = typesMod.attr("ModuleType")(py::str(moduleName));
				// exec() takes the namespace DICT rather than the module, which
				// is why the __dict__ attr() here is not optional.
				py::exec(python, mod.attr("__dict__"));
				// Registered only after exec has fully returned. A module whose
				// body raised half way through is destroyed with the local `mod`
				// and never becomes visible to any import.
				sys.attr("modules")[py::str(moduleName)] = mod;

				// exec() succeeded but bound a different name: the class the
				// emitter produced and className disagree. Checked explicitly so
				// the mismatch is reported once, here, instead of resurfacing as
				// an AttributeError on the event method for the rest of the
				// entity's life.
				if (!py::hasattr(mod, py::str(className)))
				{
					WL_CORE_ERROR(
						"FlowGraph: '{0}' compiled but emitted no class '{1}'",
						graph.GraphPath, className);
					graph.CompileError =
						"compiled but emitted no class " + className;
					s_Failed[id] = true;
					return;
				}

				// `scene` is passed explicitly: an Entity carries no
				// back-reference to its Scene, and a graph that reads or writes
				// the world needs one. That raw pointer is a lifetime hazard --
				// the instance outlives the frame it was taken in, so a graph
				// that stashes the Scene reference has a dangling pointer the
				// moment the Scene is destroyed.
				s_Instances[id] = mod.attr(py::str(className))(entity, &scene);

				// Exposed-variable overrides, applied AFTER construction.
				//
				// __init__ seeds every variable from its graph default; an exposed
				// variable's per-entity value has to overwrite that, and the only
				// place to do it is here -- the instance exists, the graph is parsed,
				// and the entity's overrides are on the component.
				//
				// The value is handed to Python as a LITERAL, evaluated with eval()
				// against an empty namespace. It is authored text, exactly as
				// DefaultLiteral is, so the two take the same path.
				//
				// A variable that is NOT exposed ignores its override: the value is
				// stored on the component, but the graph does not declare the
				// variable editable, so writing it would change behaviour the
				// author never exposed. The inspector must not offer it either.
				for (const auto &variable : parsed.Variables)
				{
					if (!variable.Exposed)
						continue;
					auto entry = graph.VariableOverrides.find(variable.Name);
					if (entry == graph.VariableOverrides.end())
						continue;
					try
					{
						py::object value = py::eval(entry->second);
						s_Instances[id].attr(py::str(variable.Name)) = value;
					}
					catch (const std::exception &exc)
					{
						// A malformed literal must not abort the attach: the graph
					// keeps its default and the failure is reported.
						WL_CORE_ERROR(
							"FlowGraph: exposed variable '{0}' on '{1}' has an "
							"unreadable value '{2}': {3}",
							variable.Name, graph.GraphPath, entry->second, exc.what());
					}
				}

				// THE DEBUG HOOK. Installed only when the graph carries a breakpoint,
				// and only for entities that are not already paused.
				//
				// The hook is a C++ function closing over the entity's UUID. It
				// records the node and raises VbPause, which unwinds the generated
				// method. The tick's catch block treats that exception as a PAUSE,
				// not a failure -- so a breakpoint does not poison the entity the
				// way a thrown error does.
				//
				// The attribute is set on the INSTANCE, not the class, which is why
				// the generated `_vb_trace` uses getattr with a default: a graph with
				// no breakpoint never gets the attribute and never pays for it.
				bool anyBreakpoint = false;
				// VisualBlocks::GraphNode -- the file is in namespace Wasteland, not
				// Wasteland::VisualBlocks, so the type is not visible unqualified.
				for (const VisualBlocks::GraphNode &node : parsed.Nodes)
					if (node.Breakpoint)
					{
						anyBreakpoint = true;
					break;
				}
				if (anyBreakpoint && !IsPaused(id))
				{
					py::cpp_function hook(
						[id](const std::string &nodeId)
					{
						// A skip-once entry means Resume() or Step() just released
						// THIS node: run it to completion instead of pausing again on
						// the same breakpoint, which would make Continue a no-op.
						auto skip = s_SkipOnce.find(id);
						if (skip != s_SkipOnce.end() && skip->second == nodeId)
						{
							s_SkipOnce.erase(skip);
							return;
					}
						s_PausedNode[id] = nodeId;
						PyErr_SetString(PauseException(),
										"breakpoint reached");
						throw py::error_already_set();
					});
					s_Instances[id].attr("_vb_traceHook") = hook;
				}

				// The EXPOSED NAMES, cached for the per-frame re-application below.
				// Cached rather than re-read because the per-frame path must not do
				// a file read; the set is refreshed whenever this attach block runs.
				auto &exposed = s_ExposedNames[id];
				exposed.clear();
				for (const auto &variable : parsed.Variables)
					if (variable.Exposed)
						exposed.push_back(variable.Name);

				s_Methods[id] = methodName;
				// The inspector flag, set only after construction succeeded.
				graph.Attached = true;

				// BeginPlay for a graph that attaches while the runtime is
				// already active -- the mid-play spawn.
				//
				// Scene::OnRuntimeStart broadcasts to the entities that exist at
				// play start, but a graph created AFTER that broadcast has not
				// attached yet at broadcast time, so the broadcast cannot reach
				// it. Attach is the one moment the instance is guaranteed to
				// exist and the entity is guaranteed to be complete, so it is the
				// correct place for this half of the contract.
				//
				// The two halves do not double-fire: a graph attaches exactly once.
				if (scene.IsRuntimeActive())
				{
					ScriptEventDispatcher::Dispatch(entity,
						ScriptEventId::BeginPlay, {});
				}
			}
			// Anything Python raised during attach -- a failing import, a
			// module-body exception, a constructor that threw.
			catch (const py::error_already_set &e)
			{
				WL_CORE_ERROR("FlowGraph: entity {0} failed to start '{1}': {2}",
							  id, graph.GraphPath, e.what());
				graph.CompileError = e.what();
				s_Failed[id] = true;
				return;
			}
		}

		try
		{
			// The per-frame call, and ONLY for a graph whose entry method is
			// OnUpdate.
			//
			// s_Methods[id] is the FIRST event in declaration order, which for a
			// schema-2 graph is not necessarily the per-frame hook. Calling it
			// unconditionally meant an OnAnimationEvent-only graph ran its handler
			// every frame: the runtime was treating the graph's event method as a
			// tick. Found by a test that attached such a graph and saw the entity
			// moved to the handler's target position on the very first tick,
			// before any animation event had fired.
			//
			// Every other event reaches its graph through ScriptEventDispatcher,
			// which calls the resolved method by name. So a graph whose first
			// event is not OnUpdate has no per-frame hook here and is correctly
			// inert between its events.
			//
			// A graph declaring OnUpdate NOT first still loses its per-frame tick
			// -- the same first-event limitation FlowMethodNameFor documents. The
			// fix is to declare OnUpdate first, which is what the template does.
			// Exposed-variable overrides, re-applied EVERY FRAME before the tick.
			//
			// Attach seeds them once, which is enough for values authored in the
			// scene file. But the INSPECTOR edits the same map, and a Python
			// attribute set at construction does not change when the map does --
			// so without this an inspector edit would only take effect on the next
			// Play.
			//
			// The plan's verification is exactly that: set an exposed variable in
			// the inspector, tick, assert the graph observed the value the
			// inspector wrote.
			//
			// A cached set of EXPOSED NAMES (s_ExposedNames, filled at attach)
			// decides which entries to write, so this costs one map comparison per
			// frame and no file read. A non-exposed variable is skipped even if
			// the component carries a stale value for it.
			if (!graph.VariableOverrides.empty())
			{
				auto &exposed = s_ExposedNames[id];
				if (!exposed.empty())
				{
					auto seen = s_OverrideEpoch.find(id);
					if (seen == s_OverrideEpoch.end() ||
						seen->second != graph.VariableOverrides)
					{
						for (const auto &variable : exposed)
						{
							auto entry =
								graph.VariableOverrides.find(variable);
							if (entry == graph.VariableOverrides.end())
								continue;
							try
							{
								py::object value = py::eval(entry->second);
								s_Instances[id].attr(py::str(variable)) = value;
							}
							catch (const std::exception &exc)
							{
								WL_CORE_ERROR(
									"FlowGraph: exposed variable '{0}' on '{1}' has "
									"an unreadable value '{2}': {3}",
									variable, graph.GraphPath,
									entry->second, exc.what());
							}
						}
						s_OverrideEpoch[id] = graph.VariableOverrides;
					}
				}
			}

			// A PAUSED graph does not tick. The breakpoint stays set; Resume()
			// clears the pause and the next tick runs the marked node.
			if (IsPaused(id))
				return;

			if (s_Methods[id] == "OnUpdate")
			{
				// The method name is cached at attach, so this is one attribute
				// lookup per entity before any graph logic runs.
				s_Instances[id].attr(py::str(s_Methods[id]))(seconds);
			}
		}
		catch (const py::error_already_set &e)
		{
			// A BREAKPOINT, not an error. The hook raised VbPause to unwind the
			// generated method, and the node that fired is already recorded.
			//
			// This must be tested BEFORE the generic failure path: a pause carries
			// a Python exception object like any other, and treating it as a throw
			// would poison the entity and erase the very instance the debugger is
			// trying to inspect.
			if (e.matches(PauseException()))
			{
				graph.SuspendedTicks = 1;
				return;
			}

			// A graph that throws must not throw every frame. Detach it and
			// record why, so one bad graph degrades one entity instead of
			// flooding the log and stalling the frame.
			WL_CORE_ERROR("FlowGraph: entity {0} threw while ticking '{1}': {2}",
						  id, graph.GraphPath, e.what());
			graph.CompileError = e.what();
			s_Failed[id] = true;
			// Dropping the last py::object reference releases the instance
			// immediately, so any Python __del__ runs now rather than at
			// shutdown.
			s_Instances.erase(id);
			s_Methods.erase(id);
		}
	}

	// Scene teardown hook, keyed by the same UUID as both maps. The erase below
	// releases the Python object -- which can run arbitrary __del__ code, so this
	// is not a guaranteed-cheap call -- and is what stops a destroyed entity's
	// graph object, and the Scene pointer it was handed, from outliving it.
	void FlowGraphSystem::OnDestroyEntity(UUID id)
	{
		if (Py_IsInitialized())
		{
			py::gil_scoped_acquire gil;
			s_Instances.erase(id);
			s_Methods.erase(id);
			// The latch goes too, so failure state cannot outlive the entity it
			// describes. The variable caches go for the same reason -- a stale
			// name list would write an override into a graph that no longer
			// declares the variable.
			s_ExposedNames.erase(id);
			s_OverrideEpoch.erase(id);
			s_PausedNode.erase(id);
			s_SkipOnce.erase(id);
			s_Failed.erase(id);
			return;
		}
		s_Methods.erase(id);
		s_Failed.erase(id);
	}

	// Drops every live graph at once. This must happen while the interpreter is
	// still alive: s_Instances is a namespace-scope static holding py::object,
	// and releasing those from a static destructor at program teardown would
	// touch Python after interpreter finalization.
	void FlowGraphSystem::Shutdown()
	{
		if (Py_IsInitialized())
		{
			py::gil_scoped_acquire gil;
			s_Instances.clear();
			s_Methods.clear();
			s_Failed.clear();
		}
		else
			s_Failed.clear();
		// Cleared too, so reloading a project starts from a clean slate and a
		// graph that failed to compile once gets another attempt.
		s_Methods.clear();
	}

} // namespace Wasteland
