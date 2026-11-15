#pragma once

#include <string>
#include <vector>

#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Wasteland
{
	// The engine events a script, flow graph or behaviour tree can respond to.
	//
	// Each id maps to a method name (`MethodNameFor`) and the dispatcher calls
	// that method on each attached system's live Python instance. The mapping
	// lives here so an event's name is not spelled at every call site -- the
	// same reason Compiler::FlowMethodNameFor exists.
	enum class ScriptEventId
	{
		BeginPlay,
		Update,
		Interact,
		Damage,
		ItemPickedUp,
		AnimationEvent,
	};

	// Fires an engine event at one entity, reaching every attached system.
	//
	// The dispatch order is SCRIPT, then FLOW GRAPH, then BEHAVIOUR TREE -- the
	// same order the scene ticks them in. An entity with several attached gets
	// the event on each, which is what "a component is an attachment point"
	// means in practice: a door can carry a script for its state and a graph
	// for its effects, and both hear the interaction.
	//
	// A MISSING METHOD IS A QUIET NO-OP on every path. The alternative -- an
	// exception per entity per event -- would make "does this entity respond to
	// OnInteract?" a try/catch at every call site, and the answer is almost
	// always "no".
	//
	// NEVER THROWS across the frame loop. The catch ordering is the one
	// ScriptEngine uses: py::error_already_set before std::exception before
	// ... A Python exception is not a C++ one and must be handled first or it
	// escapes as an unhandled C++ exception through the frame.
	class ScriptEventDispatcher
	{
	public:
		static void Dispatch(Entity entity, ScriptEventId id,
							 const std::vector<py::object> &args = {});

		// The same dispatch under an explicit method name, for events that are
		// not in the enum -- a game-specific "OnInventoryChanged", say. Keeping
		// this separate means the enum stays a closed, checkable set while
		// callers are not forced to invent an id for every custom hook.
		static void DispatchNamed(Entity entity, const std::string &methodName,
								  const std::vector<py::object> &args = {});

		// The UUID-only form, for a caller that legitimately has no Entity.
		//
		// AnimationSystem looks entities up by UUID and never holds a handle, so
		// requiring an Entity here would mean either storing one in every runtime
		// or re-looking it up on each animation event. The validity checks
		// DispatchNamed performs are about the ENTITY (IsValid, IDComponent), and
		// a UUID already IS the IDComponent's value -- so the UUID form skips the
		// checks that would be redundant and keeps the GIL acquire and the three
		// per-system calls identical.
		//
		// The single place the dispatch actually happens; DispatchNamed forwards
		// here so the two can never drift.
		static void DispatchNamedById(UUID id, const std::string &methodName,
									  const std::vector<py::object> &args = {});

		// The method name an id dispatches to. Public because a caller that
		// wants to ask "would this entity respond?" needs the same answer the
		// dispatcher would use, and a second spelling of the mapping is how the
		// two drift apart.
		static std::string MethodNameFor(ScriptEventId id);
	};

}
