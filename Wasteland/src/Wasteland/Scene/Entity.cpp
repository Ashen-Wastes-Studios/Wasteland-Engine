#include "wlpch.h"
#include "Entity.h"

namespace Wasteland {

	// Handle-and-scene pair, i.e. a reference into one registry. Copyable by
	// default (see the defaulted copy ctor in Entity.h) and freely copyable, so
	// an Entity outliving its scene becomes a dangling pair -- IsValid() is the
	// only guard, and it checks m_Scene against a null pointer, not against a
	// destroyed Scene.
	//
	// The body below is empty. Nothing to validate or allocate -- the two
	// members are plain scalars handed straight to entt, and all behaviour
	// lives in the templates on Entity (AddComponent and friends).
	//
	// DEFECT: unlike AddComponent and GetComponent, the constructor does not
	// assert that `handle` is live in `scene`'s registry. An Entity built from
	// entt::null, or from a handle belonging to a different Scene, constructs
	// cleanly and only fails later inside an entt call.
	Entity::Entity(entt::entity handle, Scene* scene)
		// Member init order. Entity.h declares m_EntityHandle before m_Scene, so
		// this order matches and the compiler emits no -Wreorder warning.
		//
		// Both members also carry default member initialisers in the header
		// ({entt::null} and nullptr), which are deliberately not repeated here: an
		// explicit ctor init-list overrides the default member initialiser, so
		// repeating them would change nothing.
		: m_EntityHandle(handle), m_Scene(scene)
	{

	}


	// Pure forwarding shim to the owning scene -- no water logic here at all.
	// Entity has no registry access to water state; the surface sampling lives
	// in Scene, and this exists so buoyancy code holding an Entity can ask
	// without also carrying a Scene*.
	//
	// @param worldPos  Probe position in world space.
	// @param outHeight Filled with the water height at worldPos.
	// @param outFlow   Optional; filled with the flow vector when non-null.
	bool Entity::GetWaterHeightAt(const glm::vec3 &worldPos, float &outHeight, glm::vec2 *outFlow)
	{
		// A default-constructed Entity has m_Scene == nullptr, so this check is what
		// stops the forwarding from becoming a null-deref. The out-parameter is left
		// untouched on this path, so a caller that reads outHeight without checking
		// the return value reads whatever it had initialised it to.
		if (!m_Scene)
			// Reporting failure rather than assuming a scene: the default-constructed
			// Entity is a legitimate value type that a container can hand out before
			// it is ever bound to a scene.
			return false;
		// Direct delegation, forwarding outFlow straight through -- including when
		// it is null, which Scene must therefore treat as "flow not wanted".
		//
		// The default argument `= nullptr` is declared in Entity.h, not repeated
		// here; repeating it would be an error.
		return m_Scene->GetWaterHeightAt(worldPos, outHeight, outFlow);
	}


	// Second water shim, same forwarding shape as GetWaterHeightAt.
	//
	// @param worldPos  Probe position in world space.
	// @param margin    Extra distance counted as underwater; the default of
	//                  0.0f is declared in Entity.h, making the test exact.
	bool Entity::IsUnderwater(const glm::vec3 &worldPos, float margin)
	{
		// Identical guard to GetWaterHeightAt. Duplicated rather than factored
		// because there is nothing above these two to hang a shared helper on --
		// and a caller who reads the return value gets the same answer either way.
		if (!m_Scene)
			// Same contract as above: false means "no scene to ask", not "above water".
			// A caller cannot distinguish the two from the return value alone.
			return false;
		// Forwards the margin unchanged, so the scene applies its own hysteresis or
		// surface-offset rule on top of the caller's value.
		return m_Scene->IsUnderwater(worldPos, margin);
	}

}