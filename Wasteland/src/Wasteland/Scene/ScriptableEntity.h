// Include guard by pragma once rather than the usual #ifndef pair: the project's
// house style.
#pragma once

// Entity.h, not a forward declaration: m_Entity below is a BY-VALUE member, so
// the definition is needed here -- which also pulls entt into every translation
// unit that includes this header.
#include "Entity.h"

// Wasteland, not Wasteland::Scene, despite living in src/Scene: every public
// engine type sits in the flat Wasteland namespace. Bindings.cpp binds it as
// Wasteland::ScriptableEntity accordingly.
namespace Wasteland {

	// A plain class, not an Entity subclass: an Entity is a handle (entt handle plus
	// Scene*), so it is cheap to copy and this type holds one BY VALUE as a member.
	class ScriptableEntity
	{
	// public, and the destructor is all it holds. Virtual is required here, not
	// merely nice: NativeScriptComponent stores a `ScriptableEntity *Instance` and
	// destroys it through that pointer, so without `virtual` only the base part would
	// be freed and the derived members would leak.
	public:
		virtual ~ScriptableEntity() {}

		// A member TEMPLATE rather than a virtual, so it is instantiated per T at the
		// call site with no vtable entry. In the header because a template in a .cpp
		// would be unusable from any other translation unit.
		template<typename T>
		// A mutable reference, not a pointer: the point is to edit the component in place.
		//
		// DEFECT (no failure mode): forwards unconditionally to Entity::GetComponent<T>,
		// which asserts HasComponent<T>() in debug then calls entt's .get<T>() -- in a
		// release build, where WL_CORE_ASSERT expands to nothing, asking for a component
		// the entity lacks is undefined behaviour rather than a diagnosable failure.
		// Entity offers TryGetComponent for exactly this and ScriptableEntity exposes no
		// pass-through for it. Not fixed here.
		T& GetComponent()
		{
			return m_Entity.GetComponent<T>();
		}

	// protected, not private: Scene calls these and Scene is not a subclass. Public
	// would let any caller drive a lifecycle event by hand, double-firing against
	// Scene's own calls in Scene.cpp.
	protected:
		// DEFECT (unguarded call site): Scene.cpp dereferences
		// `nsc.Instance->OnCreate()` with no null check, so a script whose factory returns
		// null faults here. The guard belongs in Scene.cpp, not in this header.
		virtual void OnCreate() {}
		virtual void OnDestroy() {}
		virtual void OnUpdate(Timestep ts) {}
	private:
		// BY VALUE -- the most consequential decision in this file. An Entity is a handle
		// pair, so this costs an entt::entity plus a Scene*: no registry allocation, no
		// ownership. It also means the instance cannot outlive its entity without
		// dangling, which is why destruction is Scene's responsibility. The cost is that
		// Entity.h includes entt, so this header is not cheap to include.
		Entity m_Entity;
		// The friend declaration is what lets Scene reach the protected hooks above;
		// placed after the member so it reads as a statement about this class. It also
		// grants access to anything added later -- a wider grant than the three hooks
		// require. Noted, not changed.
		friend class Scene;
	};

}