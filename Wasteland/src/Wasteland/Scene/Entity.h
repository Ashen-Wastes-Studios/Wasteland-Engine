// Entity.h -- a lightweight handle onto one entity in a Scene's component
// registry.
//
// Entity is NOT an object with data. It is two fields -- an entt handle and a
// Scene back-pointer -- and every accessor forwards to the scene's registry.
// That makes it cheap to copy (the copy ctor is defaulted) and safe to store in
// containers, at the cost of the usual handle hazards documented throughout
// below.
//
// The components themselves are plain structs declared in Components.h and held
// in the registry by type, not by member. So there is no inheritance hierarchy
// and no virtual dispatch on entity data; the ECS pattern in its entt form.
#pragma once

// Scene is included, not forward-declared, because every template below
// dereferences m_Scene->m_Registry -- and m_Registry is PRIVATE in Scene,
// reachable only because Scene declares `friend class Entity`. So the complete
// type is genuinely required at the point of instantiation.
//
// This is why Scene.h's inclusion of Entity.h is a forward declaration only:
// the dependency runs one way in each direction for different reasons.
#include "Scene.h"
// The component structs. Needed in full because GetComponent<T> returns T& and
// the templates are instantiated at the call site, where the caller's T must
// be complete anyway.
//
// Included AFTER Scene.h, which itself forward-declares several of these
// component types -- so this is where they become usable.
#include "Components.h"

// The ECS library. `entt::entity` is the handle type stored below, and
// entt::null is its invalid sentinel.
//
// entt::entity is a strongly-typed integer wrapper around an index+version
// pair. The version is what makes a recycled handle distinguishable from the
// destroyed entity that had it -- entt bumps it on reuse, so a stale handle
// fails registry.valid() rather than silently addressing a new entity.
#include <entt/entt.hpp>

// For the UUID return type of GetUUID below. Needed at header scope because
// that method is inline in this file.
#include "Wasteland/Core/UUID.h"

namespace Wasteland
{

	// A value type with no destructor, no allocation and no ownership. Copying an
	// Entity copies a reference; it does not copy the entity or its components.
	//
	// Consequence worth internalising: an Entity held past the destruction of its
	// Scene, or past DestroyEntity on that entity, dangles. IsValid() is the only
	// guard and it cannot detect a DESTROYED Scene -- it checks m_Scene against
	// null, not against a dead object. See the note at operator== for the related
	// asymmetry.
	class Entity
	{
	public:
		// Produces the "nothing" entity: m_EntityHandle is entt::null and m_Scene is
		// nullptr, both from their NSDMIs below.
		//
		// This is a genuinely supported value, not a placeholder. It is what a
		// container hands out before anything is bound, what an unselected editor row
		// holds, and what the two water shims below explicitly handle by returning
		// false. operator bool reports it as false.
		Entity() = default;
		// The only way to make a usable Entity, called by Scene::CreateEntity and
		// friends. Defined in Entity.cpp, which also carries the DEFECT note: this
		// constructor does NOT validate that `handle` is live in `scene`'s registry, so
		// a mismatched pair constructs cleanly and fails later inside an entt call.
		//
		// Raw Scene pointer, deliberately not a Ref: an Entity does not keep its scene
		// alive, and claiming shared ownership would misrepresent the lifetime.
		Entity(entt::entity handle, Scene *scene);
		// Explicitly defaulted, though it would be implicit anyway. Declaring it
		// documents that copying is a supported, cheap operation.
		//
		// Note what is NOT declared: no move constructor and no assignment operators.
		// With a user-declared copy constructor, the move operations are NOT
		// implicitly generated -- so Entity is neither movable nor assignable, and a
		// vector<Entity> must copy on growth. Harmless at this size (two words), but
		// it means `std::vector<Entity>` reallocation copies rather than moves.
		Entity(const Entity &other) = default;

		// A template header shared by AddComponent below. Both are defined in-class,
		// which is what allows them to touch Scene's private m_Registry without Scene
		// granting friendship to each component type individually -- Entity's friendship
		// covers the templates defined inside it.
		//
		// Args&&... is a perfect-forwarding pack, so `AddComponent<TransformComponent>
		// (vec3(1), quat(...))` constructs the component in place inside the registry
		// rather than copying a temporary in.
		template <typename T, typename... Args>
		// emplace, so the component is constructed directly into registry storage.
		// Returns a reference into the registry -- valid until the component is
		// removed or the entity is destroyed.
		T &AddComponent(Args &&...args)
		{
			// Guard against silently doing nothing. entt's emplace is a NO-OP when the
			// component already exists -- it neither replaces nor errors -- so without this
			// assert a duplicate AddComponent would look successful and quietly discard
			// the arguments.
			//
			// That is exactly why AddOrReplaceComponent exists below, and why this one
			// asserts rather than picking a behaviour.
			//
			// Debug-only: in a Release build the duplicate is silently dropped.
			WL_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
			// The single point where a component comes into existence. Reaches into
			// Scene's PRIVATE m_Registry, which compiles only because Scene declares
			// `friend class Entity`.
			//
			// std::forward<Args> preserves value categories, so an rvalue argument is moved
			// into the component rather than copied.
			//
			// No null check on m_Scene or on m_EntityHandle: a default-constructed Entity
			// passed here is a null-pointer dereference. The engine's own call sites always
			// hold a valid pair, and the assert above would itself have crashed first.
			T &component = m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			// The scene's per-type reaction hook, and the non-obvious part of this method.
			//
			// Scene::OnComponentAdded is a TEMPLATE TRAP: its primary template body is
			// `static_assert(false, ...)`, and it must be explicitly specialised for every
			// component type. Most specialisations are empty, and that emptiness is the point
			// -- "nothing to do here" is still a deliberate declaration.
			//
			// So adding a component type to the engine without adding a specialisation is
			// a COMPILE ERROR at this line, not a silent no-op. That is what makes the hook
			// safe to omit from most types.
			//
			// Called AFTER the emplace, so a specialisation sees a fully-constructed
			// component and may modify it in place.
			m_Scene->OnComponentAdded<T>(*this, component);
			// By reference, so the caller mutates the registry's storage directly rather
			// than a copy. Storing the result and holding it across a RemoveComponent is a
			// dangling reference.
			return component;
		}

		// Same template header as AddComponent, for the replace-or-add variant.
		template <typename T, typename... Args>
		// The non-asserting counterpart: emplace_or_replace constructs a new component
		// if absent and overwrites in place if present.
		//
		// This is the one to use for a component whose value is authored wholesale each
		// time (an inspector writing a Transform, say), where "already present" is
		// normal rather than a bug.
		//
		// Note the replacement DESTROYS the old component and constructs a new one, so
		// any reference previously obtained from GetComponent becomes invalid -- unlike
		// entt::replace, which would keep the address stable. In-place mutation of the
		// existing component is therefore not possible through this call.
		T &AddOrReplaceComponent(Args &&...args)
		{
			// Identical to AddComponent's emplace except for the entt call. Reached
			// through the same Scene friendship.
			//
			// Called on a default-constructed Entity this is the same null dereference as
			// above -- there is no guard on either path.
			T &component = m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			// The hook fires on the REPLACEMENT path too, not just on creation.
			//
			// Worth knowing when reading a specialisation: an AddOrReplaceComponent during
			// a load or an inspector edit re-runs whatever side effect the specialisation
			// performs, with the new value. An empty specialisation is unaffected; one with
			// a side effect runs it again.
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		// Header for the non-template-argument accessor group below.
		//
		// T is a component type, deduced explicitly by the caller: GetComponent<
		// TransformComponent>(). There is no deduction from arguments, so the angle
		// brackets are mandatory.
		template <typename T>
		// Mutable access. The assert is the only difference in safety from
		// TryGetComponent below -- there is no null return, so a missing component is a
		// debug abort and undefined behaviour in release.
		//
		// Not const, because it hands out a mutable reference; a const Entity would need
		// a const overload returning const T&, which does not exist.
		T &GetComponent()
		{
			// Matches the message used by RemoveComponent below rather than AddComponent's
			// "already has" -- the two failure modes are distinct and worded accordingly.
			//
			// Debug-only, so a Release caller that asks for an absent component gets
			// entt's own behaviour: a failed assertion inside get<T>() and undefined
			// behaviour on return.
			WL_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			// entt's get, which is the fast path -- a direct index with no search.
			// Throws (or asserts, per entt's config) if absent; the assert above is the
			// engine's own guard, so entt's is a backstop.
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template <typename T>
		// The checked accessor: returns nullptr instead of failing.
		//
		// This is the one to use for an OPTIONAL component -- ScriptComponent on an
		// entity that may not have one, a collider on a prop -- where absence is a
		// normal state rather than a bug.
		//
		// Returns a raw pointer into the registry. The caller does not own it and must
		// not delete it.
		T *TryGetComponent()
		{
			// entt's non-throwing lookup. Returns nullptr for an absent component AND for
			// an invalid entity handle, so a null result is ambiguous between "no such
			// component" and "no such entity" -- the caller cannot tell which.
			//
			// Same null-dereference exposure as the other accessors when m_Scene is null.
			return m_Scene->m_Registry.try_get<T>(m_EntityHandle);
		}

		// Header for the const predicate below.
		template <typename T>
		// A predicate, not an accessor. The natural first check before GetComponent.
		//
		// `const` but NOT static: it needs both members, and a static version could not
		// be called on a const Entity the way this can.
		//
		// Should have been const-qualified more strictly -- it is, and that is correct
		// because it only reads the registry.
		bool HasComponent()
		{
			// entt's all_of, which is a bitmask test over the entity's component set --
			// cheaper than a get. This is why HasComponent is the right thing to call in a
			// loop and GetComponent is not.
			//
			// On a default-constructed Entity this dereferences a null m_Scene, so the
			// predicate is NOT safe on an invalid Entity even though it looks like a safe
			// query. Callers must check operator bool or IsValid() first.
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		// Header for the removal accessor below.
		template <typename T>
		// Removes the whole component type from the entity.
		//
		// Does NOT run any removal hook -- there is no OnComponentRemoved counterpart
		// to OnComponentAdded. So a component whose removal needs cleanup (a physics
		// body, a script instance) must be torn down by the caller. Worth knowing
		// before adding a component that owns something.
		//
		// Also does not destroy other components, and entt does not move the remaining
		// ones -- their addresses are stable.
		void RemoveComponent()
		{
			// Same message as GetComponent's, for the same reason: both fail on an absent
			// component.
			//
			// entt's remove is itself a no-op when absent, so without this assert a
			// redundant removal would look like it worked.
			WL_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			// Destroys and de-registers the component.
			//
			// Any T& previously obtained from GetComponent or AddComponent is now dangling.
			// Same null-dereference exposure as the other accessors on an invalid Entity.
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		// The implicit truth test: `if (entity)`. Checks the HANDLE only.
		//
		// DEFECT (asymmetry, worth knowing): this does not check m_Scene, while
		// IsValid() below checks both. So an Entity holding a live handle and a null
		// scene is truthy here and invalid there -- and every template above
		// dereferences m_Scene unconditionally, so such an entity passes the idiomatic
		// `if (entity)` guard and then crashes.
		//
		// A default-constructed Entity is false on both counts, which is the case that
		// matters most in practice.
		operator bool() const { return m_EntityHandle != entt::null; }
		// Lets an Entity be passed straight to an entt API.
		//
		// A hazard worth naming: this conversion is IMPLICIT and non-explicit, so any
		// function overloaded on both entt::entity and Entity becomes ambiguous, and an
		// Entity can silently convert where a raw handle was expected -- bypassing every
		// check in this class. It also means `entity == entt::null` compiles and means
		// what it looks like.
		//
		// Marking it explicit would remove the ambiguity at the cost of a cast at each
		// call site.
		operator entt::entity() const { return m_EntityHandle; }
		// A THIRD implicit conversion, to a plain integer -- the handle's numeric value,
		// index and version packed together.
		//
		// This one is the most dangerous of the three, because uint32_t is the most
		// over-loaded type in the engine: any function taking an int, an index or a
		// count can now accept an Entity without complaint, and comparisons like
		// `if (someUint == entity)` compile and compare packed handle bits rather than
		// doing anything intended.
		//
		// It also makes Entity usable as a map key or in arithmetic by accident. It
		// reads as a convenience for interop and behaves as a hazard.
		operator uint32_t() const { return (uint32_t)m_EntityHandle; }

		// Every entity carries an IDComponent, and its UUID is the stable identity that
		// survives save/load -- unlike m_EntityHandle, which is regenerated whenever a
		// scene is rebuilt.
		//
		// That is why the save system persists UUIDs: a reference to an entity must
		// resolve across a reload, and only the UUID can.
		//
		// Not const, because GetComponent is not. Returns by value -- a UUID is a
		// small fixed-size type.
		//
		// Requires IDComponent to be present. GetComponent's assert covers it in debug;
		// in release an entity created without one yields undefined behaviour here.
		UUID GetUUID() { return GetComponent<IDComponent>().ID; }
		// Same shape as GetUUID: the display name lives in the entity's TagComponent, so
		// naming an entity is a component lookup rather than a member.
		//
		// Returns a const reference INTO the registry -- no copy, which matters because
		// this is called per row when the editor draws a hierarchy. The reference is
		// invalidated by removing the TagComponent or destroying the entity.
		//
		// Also not const, for the same reason as GetUUID.
		const std::string &GetName() { return GetComponent<TagComponent>().Tag; }

		// The thorough validity check, and the one to prefer over operator bool before
		// touching components.
		//
		// Three conditions, and each covers a different failure:
		//   m_EntityHandle != entt::null -- a default-constructed or moved-from entity
		//   m_Scene != nullptr          -- an entity with no scene to resolve against
		//   registry.valid(handle)      -- a HANDLE THAT WAS VALID ONCE. This is the
		//                                one operator bool cannot do: entt bumps the
		//                                version when an index is recycled, so a handle
		//                                to a destroyed entity fails here instead of
		//                                silently addressing its replacement.
		//
		// DEFECT: it cannot detect a DESTROYED Scene. m_Scene is a raw pointer, so an
		// entity outliving its scene passes this check (the scene pointer is non-null)
		// and then dereferences freed memory in registry.valid(). There is no way to
		// close that hole without shared ownership or a generation counter.
		//
		// It also cannot detect an entity DESTROYED in a different Scene than m_Scene
		// points at, since registry.valid only consults this one registry.
		bool IsValid() const
		{
			// Short-circuit evaluation is what makes this safe to write in one expression:
			// m_Registry is only reached once m_Scene has been shown non-null.
			//
			// The order matters too -- the cheap handle comparison runs before the
			// registry query, which is the more expensive of the three.
			return m_EntityHandle != entt::null && m_Scene != nullptr && m_Scene->m_Registry.valid(m_EntityHandle);
		}

		// These two are the only members of Entity that are not component or registry
		// plumbing. They exist so buoyancy code holding an Entity can ask about water
		// without also carrying a Scene*.
		//
		// Both are thin forwarders defined in Entity.cpp: no water logic lives here.
		// The scene owns the water state; this is convenience.
		// Water queries forwarded to the owning scene (used by C++ and scripts
		// for buoyancy: outHeight = still level + wave height, outFlow = current).
		// @param worldPos  Probe position in world space.
		// @param outHeight Filled with the still level plus the simulated wave height.
		// @param outFlow   Optional. Filled with the river current velocity when
		//                  non-null; left untouched otherwise.
		// @returns Whether a probe happened at all -- false when this entity has no
		//          scene, or when the position is outside every enabled water body.
		//
		// DEFECT (silent failure): on the false path outHeight is NOT written. A caller
		// that reads it without checking the return value gets whatever it happened to
		// be initialised to -- which is a plausible-looking height rather than an
		// obvious sentinel. This is the classic out-parameter contract and the .cpp
		// notes it too.
		//
		// Not const, because the scene's water query is not const (it samples the
		// simulation).
		bool GetWaterHeightAt(const glm::vec3 &worldPos, float &outHeight, glm::vec2 *outFlow = nullptr);
		// @param margin  Extra distance below the surface still counted as underwater.
		//               The default of 0.0f makes the test exact; a small positive
		//               value gives buoyancy code an early-warning band.
		// @returns false when there is no scene to ask, when the position is outside
		//          every water body, or when it is above the surface. The caller
		//          cannot distinguish those three from the return value alone.
		//
		// Duplicated guard from GetWaterHeightAt rather than a shared helper -- the
		// .cpp explains that there is nowhere above them to hang one.
		bool IsUnderwater(const glm::vec3 &worldPos, float margin = 0.0f);

		// Compares the handle, plus a NON-NULL CHECK on the other entity's scene.
		//
		// DEFECT (a genuine logic error, not a style issue): `other.m_Scene` is tested
		// but `this->m_Scene` is not, and neither is the relationship between the two.
		// Consequences:
		//   * two default-constructed entities compare EQUAL, because both handles are
		//     entt::null -- which happens to be convenient for a "no selection"
		//     sentinel, and is presumably why it was written this way.
		//   * entities from DIFFERENT scenes with the same handle number compare equal,
		//     since the handles are per-registry integers. The scene pointer is checked
		//     only for nullness, never for identity against m_Scene.
		//   * a valid entity compares equal to an entity whose scene was destroyed,
		//     since a dangling non-null pointer passes.
		// Comparing both handles AND both scene pointers would fix the second and third
		// cases; it would also stop two default entities being equal.
		//
		// This is what the editor's selection comparisons rely on, so the two-defaults
		// case being true is load-bearing for "deselect".
		bool operator==(const Entity &other) const
		{
			// Read the second operand as a pointer-to-bool conversion -- idiomatic but easy
			// to misread as a comparison. The intent is "and the other entity has a scene".
			//
			// The && short-circuits, so the scene check is skipped when handles differ.
			return m_EntityHandle == other.m_EntityHandle && other.m_Scene;
		}

		// Defined as the negation rather than left to the compiler, which matters: with
		// the three implicit conversion operators above (bool, entt::entity, uint32_t),
		// an implicit != would be ambiguous or would silently pick one of them. Being
		// explicit here keeps `a != b` meaning what it reads as.
		//
		// Consistent with operator== by construction -- there is no way for the two to
		// disagree.
		bool operator!=(const Entity &other) const
		{
			// Delegating rather than re-implementing the comparison. Which means the two
			// defects of operator== above are inherited wholesale: two default entities are
			// not-un-equal, and cross-scene handle collisions are not-un-equal.
			return !(*this == other);
		}

	// Private with no accessors beyond the conversion operators, so the two fields
	// are only reachable through this class's API. The one exception is the implicit
	// conversions above, which hand out the raw handle.
	private:
		// Braced initialiser, not `=`. Both give the same value here; the brace form
		// additionally prevents a narrowing conversion in a copy-initialisation
		// context, which is the safer habit for a strongly-typed handle.
		//
		// entt::null is entt's invalid sentinel -- the same value operator bool and
		// IsValid compare against. A default-constructed Entity therefore reports
		// itself invalid without any explicit flag.
		entt::entity m_EntityHandle{entt::null};
		// The registry this handle belongs to. Needed because a bare entt::entity says
		// nothing about which registry indexes into, and because it is the route to the
		// private m_Registry and to OnComponentAdded.
		//
		// Raw pointer, not Ref: an Entity must not keep its Scene alive. The cost is the
		// documented inability of IsValid() to detect a destroyed scene.
		//
		// Declared second but initialised second in Entity.cpp's ctor init list, so the
		// order matches and no -Wreorder fires.
		Scene *m_Scene = nullptr;
	};

}