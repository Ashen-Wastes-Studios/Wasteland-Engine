#include "wlpch.h"
#include "Wasteland/Scene/ScriptEventDispatcher.h"

#include "Wasteland/Core/Log.h"
#include "Wasteland/Scene/BehaviorTreeSystem.h"
#include "Wasteland/Scene/FlowGraphSystem.h"
#include "Wasteland/Scripting/ScriptEngine.h"

namespace Wasteland
{
	std::string ScriptEventDispatcher::MethodNameFor(ScriptEventId id)
	{
		switch (id)
		{
		case ScriptEventId::BeginPlay:
			return "OnBeginPlay";
		case ScriptEventId::Update:
			return "OnUpdate";
		case ScriptEventId::Interact:
			return "OnInteract";
		case ScriptEventId::Damage:
			return "OnDamage";
		case ScriptEventId::ItemPickedUp:
			return "OnItemPickedUp";
		case ScriptEventId::AnimationEvent:
			return "OnAnimationEvent";
		}

		// Unreachable while the switch is exhaustive. Returning "" rather than
		// asserting keeps a future enum addition a quiet no-op instead of a
		// crash in the frame loop -- and DispatchNamed already treats an empty
		// name as "nothing to do".
		return "";
	}

	namespace
	{
		// Call `methodName` on `instance` if it exists.
		//
		// REQUIRES THE GIL. DispatchNamed acquires it once for the whole
		// dispatch; acquiring per system would be three refcount bumps and
		// three chances to be wrong about who holds it.
		//
		// Returns false when the instance is null, the method is absent, or the
		// call raised -- so a caller can tell "nothing answered" from "something
		// answered", which matters when several systems are attached.
		bool CallIfPresent(py::object *instance, const std::string &methodName,
						   const std::vector<py::object> &args, const UUID &id)
		{
			if (!instance || !instance->ptr())
				return false;

			try
			{
				// getattr with a default: a missing method yields None rather
				// than raising AttributeError, so an entity with no script -- or
				// one that simply does not implement this hook -- is a quiet
				// no-op instead of an exception per entity per event.
				py::object fn = py::getattr(*instance, methodName.c_str(), py::none());
				if (fn.is_none() || !PyCallable_Check(fn.ptr()))
					return false;

				// pybind's operator() takes an `args` (a tuple subclass), and there
				// is no variadic overload that accepts a container -- so the vector
				// is packed into a tuple here. PyTuple_SET_ITEM STEALS the
				// reference, hence the explicit incref on each element.
				py::tuple packed(args.size());
				for (size_t i = 0; i < args.size(); ++i)
				{
					Py_INCREF(args[i].ptr());
					PyTuple_SET_ITEM(packed.ptr(), static_cast<Py_ssize_t>(i),
									 args[i].ptr());
				}
				fn(*packed);
				return true;
			}
			catch (const py::error_already_set &e)
			{
				// A Python exception is NOT a C++ one. This must be caught
				// before std::exception or it escapes the frame loop.
				WL_CORE_ERROR("Script for entity {0} raised during {1}: {2}", id,
							  methodName, e.what());
				PyErr_Clear();
			}
			catch (const std::exception &e)
			{
				WL_CORE_ERROR("Script binding error for entity {0} during {1}: {2}",
							  id, methodName, e.what());
				PyErr_Clear();
			}
			catch (...)
			{
				WL_CORE_ERROR("Unknown script error for entity {0} during {1}", id,
							  methodName);
				PyErr_Clear();
			}
			return false;
		}
	}

	void ScriptEventDispatcher::DispatchNamed(Entity entity,
											  const std::string &methodName,
											  const std::vector<py::object> &args)
	{
		if (!entity.IsValid() || !entity.HasComponent<IDComponent>())
			return;

		DispatchNamedById(entity.GetComponent<IDComponent>().ID, methodName, args);
	}

	void ScriptEventDispatcher::DispatchNamedById(UUID id,
												  const std::string &methodName,
												  const std::vector<py::object> &args)
	{
		if (methodName.empty() || !Py_IsInitialized())
			return;

		// THE GIL, acquired once. Every entry point here is called from C++ --
		// the frame loop, an interactor -- NOT from a pybind-bound function, so
		// unlike the lambdas in Bindings.cpp this acquire is genuinely required.
		py::gil_scoped_acquire gil;

		// Script, then graph, then tree: the scene's tick order. Each is a
		// separate getattr, so an entity with all three attached hears the
		// event on all three.
		CallIfPresent(ScriptEngine::GetInstance(id), methodName, args, id);
		CallIfPresent(FlowGraphSystem::GetInstance(id), methodName, args, id);
		CallIfPresent(BehaviorTreeSystem::GetInstance(id), methodName, args, id);
	}

	void ScriptEventDispatcher::Dispatch(Entity entity, ScriptEventId id,
										 const std::vector<py::object> &args)
	{
		DispatchNamed(entity, MethodNameFor(id), args);
	}

}
