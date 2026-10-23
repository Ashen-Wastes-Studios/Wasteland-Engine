#include "wlpch.h"
#include "Scene.h"

#include "Components.h"
#include "Wasteland/Renderer/Renderer2D.h"
#include "Wasteland/Renderer/Renderer3D.h"

#include <glm/glm.hpp>

#include "Entity.h"
#include "ScriptableEntity.h"
#include "Wasteland/Scripting/ScriptEngine.h"
#include "Wasteland/Scene/BehaviorTreeSystem.h"

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>

// Box2D
#include "box2d/b2_world.h"
#include "box2d/b2_body.h"
#include "box2d/b2_fixture.h"
#include "box2d/b2_polygon_shape.h"
#include "box2d/b2_circle_shape.h"

// Box3D
#include <box3d/box3d.h>
#include <box3d/collision.h>
#include "Wasteland/Audio/AudioSystem.h"
#include "Platform/NVRHI/NVRHIRendererAPI.h"
#include "Platform/NVRHI/NVRHIContext.h"

namespace Wasteland
{

	// Translates an engine BodyType into the Box2D equivalent.
	//
	// The two enums are structurally identical (Static/Dynamic/Kinematic, same
	// order), so this is a straight 1:1 mapping -- but it is still written out
	// rather than cast, so the engine's physics types stay independent of the
	// Box2D headers.
	//
	// Asserts on an unrecognised value, which matters only if an enumerator is
	// added without updating this switch.
	static b2BodyType Rigidbody2DTypeToBox2DBody(Rigidbody2DComponent::BodyType bodyType)
	{
		switch (bodyType)
		{
		case Rigidbody2DComponent::BodyType::Static:
			return b2_staticBody;
		case Rigidbody2DComponent::BodyType::Dynamic:
			return b2_dynamicBody;
		case Rigidbody2DComponent::BodyType::Kinematic:
			return b2_kinematicBody;
		}

		WL_CORE_ASSERT(false, "Unknown body type");
		return b2_staticBody;
	}

	// Box3D handles are stored inside the component structs as a uint64_t --
	// see RigidBody3DComponent::RuntimeBody in Components.h. This is the guard
	// on that assumption.
	//
	// Good practice and load-bearing: the components are plain structs that
	// must not depend on Box3D's headers, so the handle is flattened to an
	// integer. If a Box3D upgrade widened or narrowed the id, this fires at
	// compile time rather than silently misreading every physics body.
	//
	// The assert checks SIZE only, not that the packing round-trips -- that is
	// guaranteed separately by b3StoreBodyId/b3LoadBodyId doing the packing
	// rather than this code reinterpret-casting the bytes. Those two together
	// are what make the uint64_t safe, including the 0-means-unset property.
	//
	// NOTE: the second static_assert, for b3ShapeId, is identical in purpose
	// -- shape handles are flattened the same way.
	static_assert(sizeof(b3BodyId) == sizeof(uint64_t),
		"b3BodyId must stay 8 bytes: it is flattened into RigidBody3DComponent::RuntimeBody");
	static_assert(sizeof(b3ShapeId) == sizeof(uint64_t),
		"b3ShapeId must stay 8 bytes: it is flattened into BoxCollider3DComponent::RuntimeShape");

	// The 3D counterpart of the Box2D translation above.
	//
	// INCONSISTENT with its 2D twin, and with the pair in SceneSerializer.cpp:
	// the 2D version asserts on an unknown body type, this one does not. Both
	// return b2/b3_staticBody as the fallback, so behaviour matches in
	// Release -- but in Debug a 3D enum mismatch is silent where the 2D one
	// stops the debugger.
	//
	// The three functions that map engine enums to physics enums (this one,
	// the Box2D one above, and the two in SceneSerializer.cpp) should agree on
	// how to handle an unknown value; today three of the four assert and one
	// does not.
	static b3BodyType RigidBody3DTypeToBox3D(RigidBody3DType type)
	{
		switch (type)
		{
		case RigidBody3DType::Static:
			return b3_staticBody;
		case RigidBody3DType::Dynamic:
			return b3_dynamicBody;
		case RigidBody3DType::Kinematic:
			return b3_kinematicBody;
		}
		return b3_staticBody;
	}

	// Converts a glm Euler-angle triple into a Box3D quaternion.
	//
	// Needed because Box3D's body definitions take b3Quat while the engine
	// stores rotation as Euler angles in TransformComponent::Rotation.
	//
	// Field-by-field rather than a memcpy or a cast: glm::quat is {x,y,z,w}
	// with the scalar LAST, while b3Quat is {v{x,y,z}, s} with the scalar in a
	// nested struct named `s`. A naive memory copy would silently produce a
	// garbage rotation -- real, plausible-looking, and wrong. Naming the four
	// components explicitly is what makes that impossible here.
	static b3Quat EulerToB3Quat(const glm::vec3 &eulerRadians)
	{
		glm::quat q(eulerRadians);
		b3Quat result;
		result.v = {q.x, q.y, q.z};
		result.s = q.w;
		return result;
	}

	// Converts a Box3D quaternion back into glm Euler angles.
	//
	// The inverse of EulerToB3Quat, for reading a body's rotation back out of
	// the simulation into the engine's Euler-angle storage.
	//
	// Argument order is the trap here: glm::quat's constructor is
	// (w, x, y, z) -- scalar first -- whereas b3Quat keeps the scalar in `s`.
	// Passing (q.s, q.v.x, q.v.y, q.v.z) is correct precisely because the order
	// matches, and it reads suspiciously like the b3Quat field order.
	//
	// VERIFIED consistent with the engine's rotation convention:
	// glm::eulerAngles(q) returns vec3(pitch, yaw, roll) -- i.e. X, Y, Z --
	// which matches the X-then-Y-then-Z order TransformComponent::Rotation
	// documents, so a value written by EulerToB3Quat and read back here
	// round-trips to the same convention it started in.
	static glm::vec3 B3QuatToEuler(const b3Quat &q)
	{
		glm::quat gq(q.s, q.v.x, q.v.y, q.v.z);
		return glm::eulerAngles(gq);
	}

	// Both special members are empty, and the destructor is deliberately so.
	//
	// All state lives in members whose own destructors run automatically. But
	// note what that does NOT cover: if physics was started, the Box2D and
	// Box3D worlds and every body in them are held by raw handles that no
	// member owns. Destroying a Scene with live physics relies on
	// OnPhysics2DStop / OnPhysics3DStop having been called first. A Scene
	// torn down without that leaves bodies registered against a destroyed
	// world with nothing to catch it.
	Scene::Scene()
	{
	}

	Scene::~Scene()
	{
	}

	template <typename T>
	// Copies one component type from every entity of `src` onto its
	// counterpart in `dst`, matched by UUID.
	//
	// THE GENERIC PATH, AND THE SOURCE OF A REAL BUG. It copies the component
	// wholesale -- `dst.emplace_or_replace<T>(dstEnttID, component)` -- which
	// means it copies EVERY field, including the flattened runtime physics
	// handles (RuntimeBody, RuntimeShape, RuntimeFixture).
	//
	// Those handles belong to the source entity's simulation body. Copying one
	// does not create a second body: it makes two components claim the same
	// one. And because CreatePhysicsBody3D early-returns when RuntimeBody is
	// already non-zero, the copy never gets its own body -- it silently
	// aliases the original. Two entities then share one simulated body, and
	// teardown destroys it twice.
	//
	// The fix already exists in this file for the component types where the
	// author noticed the pattern -- see the three explicit specializations
	// below, which each hand-copy only the safe fields. The physics
	// components have no such specialization, which is the gap.
	//
	// Entity matching is by UUID, not by entt handle, because handles are
	// per-registry and meaningless across two scenes. The three guards in the
	// loop (valid, has ID, present in the map) each skip a different kind of
	// mismatch: a destroyed handle, an entity with no identity, and an entity
	// the caller did not create in the destination at all.
	//
	// @param dst     Destination registry, already populated with the new
	//               entities.
	// @param src     Source registry to copy from.
	// @param enttMap Source entt handle -> destination entt handle. Built by the caller.
	static void CopyComponent(entt::registry &dst, entt::registry &src, const std::unordered_map<entt::entity, entt::entity> &enttMap)
	{
		auto view = src.view<T>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			// Keyed by the SOURCE entt handle, not by UUID. UUIDs are not unique:
			// the deserializer defaults a missing or unconvertible Entity key to 0,
			// so a scene can hold many entities sharing ID 0. A UUID-keyed map
			// collapses those into one entry, and every component after the first is
			// then copied onto the same destination entity while the rest arrive
			// with nothing -- which is how a scene of 1000 buildings came to show a
			// single one after pressing Play.
			auto it = enttMap.find(e);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &component = src.get<T>(e);

			dst.emplace_or_replace<T>(dstEnttID, component);
		}
	}

	template <>
	// Specialization: copies the script BINDINGS but not the script INSTANCE.
	//
	// This is the pattern the generic template above gets wrong, written out
	// correctly. The function pointers (InstantiateScript, DestroyScript) are
	// copied so the duplicate still knows what type to construct, but Instance
	// is forced to null rather than copied.
	//
	// Copying Instance would be the worst of both outcomes: the two components
	// would point at ONE live script object, so a duplicate would share and
	// overwrite the original's state, and both would run the same OnUpdate.
	// Worse, each would register a DestroyScript teardown against the same
	// pointer -- a double delete.
	//
	// Nulling Instance is safe precisely because of the convention documented
	// on the component: every call site checks Instance for null before using
	// it, so a null instance reads as "no script attached yet" rather than as
	// a dangling pointer. Something must therefore instantiate it afterwards
	// -- that is the remaining half of duplicating a scripted entity.
	//
	// emplace_or_replace with no source argument default-constructs a fresh
	// component, which is what guarantees a clean container rather than one
	// carrying state from a previous occupant of that entity.
	static void CopyComponent<NativeScriptComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<entt::entity, entt::entity> &enttMap)
	{
		auto view = src.view<NativeScriptComponent>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			auto it = enttMap.find(e);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<NativeScriptComponent>(e);

			// Emplace a clean script component container
			auto &dstComponent = dst.emplace_or_replace<NativeScriptComponent>(dstEnttID);

			// Explicitly copy only the function pointers, keeping Instance safely null
			dstComponent.InstantiateScript = srcComponent.InstantiateScript;
			dstComponent.DestroyScript = srcComponent.DestroyScript;
			dstComponent.Instance = nullptr;
		}
	}

	template <>
	// Specialization: copies the script's PATH and NAME.
	//
	// Structurally simpler than the NativeScriptComponent case above because a
	// ScriptComponent holds no live object -- only the authoring references --
	// so a plain field copy is already safe. It is still written out by hand
	// rather than left to the generic template, which keeps the two script
	// copies reading the same way and makes the reason (see above) uniform.
	//
	// The log line fires once per scripted entity, so duplicating an entity
	// with several scripts logs several lines at INFO -- useful when tracing
	// a duplication, noisy in bulk.
	static void CopyComponent<ScriptComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<entt::entity, entt::entity> &enttMap)
	{
		auto scriptView = src.view<ScriptComponent>();
		for (auto e : scriptView)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			auto it = enttMap.find(e);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<ScriptComponent>(e);

			WL_CORE_INFO("Copying Script: '{0}' from {1}", srcComponent.ScriptPath, srcComponent.ScriptName);

			// Emplace a clean script component container in the destination scene
			auto &dstComponent = dst.emplace_or_replace<ScriptComponent>(dstEnttID);

			// Explicitly copy the data members
			dstComponent.ScriptPath = srcComponent.ScriptPath;
			dstComponent.ScriptName = srcComponent.ScriptName;
		}
	}

	template <>
	// Specialization: copies camera configuration but constructs a fresh
	// SceneCamera.
	//
	// NOTE THE COMMENT IS WRONG. It claims the hand-copy is done "to trigger
	// any internal setter functions properly", but the body then assigns
	// `dstComponent.Camera = srcComponent.Camera` -- a plain memberwise copy
	// of the whole SceneCamera, which invokes SceneCamera's implicit copy
	// constructor and NOT any of its setters (which are one-liners anyway and
	// all they do is assign a field and call RecalculateProjection).
	//
	// The behaviour is fine, because SceneCamera's copy carries m_Projection
	// and every parameter field together, so a copied camera is internally
	// consistent. But the stated rationale does not describe what the code
	// does, which will mislead the next person who changes it.
	//
	// The reason a fresh container is still wanted is the same as for the
	// script components: emplace_or_replace with no source argument gives a
	// default-constructed SceneCamera, whose projection is degenerate until
	// SetViewportSize runs (see SceneCamera.h) -- so a duplicated camera starts
	// broken and is repaired on the next viewport update.
	//
	// Primary IS copied, so duplicating the scene's primary camera produces
	// TWO entities claiming Primary. Since the Scene picks a primary and the
	// analytic-light path is first-wins, which one survives depends on gather
	// order rather than on anything the user controlled. See the Primary TODO
	// in Components.h.
	static void CopyComponent<CameraComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<entt::entity, entt::entity> &enttMap)
	{
		auto view = src.view<CameraComponent>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			auto it = enttMap.find(e);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<CameraComponent>(e);

			// Force create a completely fresh, brand new camera component container
			auto &dstComponent = dst.emplace_or_replace<CameraComponent>(dstEnttID);

			// Manually copy individual properties to trigger any internal setter functions properly
			dstComponent.Primary = srcComponent.Primary;
			dstComponent.FixedAspectRatio = srcComponent.FixedAspectRatio;

			// Copy standard camera configuration details (adjust these names if your class names vary)
			dstComponent.Camera = srcComponent.Camera;
		}
	}

	template <typename T>
	// Single-entity variant of the copy above, used by DuplicateEntity.
	//
	// Goes through the Entity wrapper rather than raw registries, so it needs
	// no UUID map -- it already has both entity handles.
	//
	// IMPORTANT: this uses the GENERIC copy semantics for every type, with no
	// specializations. The three specializations above are hooked into
	// CopyComponent, which this does not call -- so duplicating an entity in
	// the editor duplicates its script INSTANCE and its camera state too,
	// which is the opposite of what those specializations exist to prevent.
	//
	// DuplicateEntity works around this by choosing not to copy script and
	// camera components at all (they are absent from its call list). For the
	// physics components there is no such avoidance: it copies
	// RigidBody3DComponent, Rigidbody2DComponent and the three 2D/3D collider
	// types through this path, so their flattened runtime handles ARE copied.
	// That is the bug documented on CopyComponent above, and it is reachable
	// from the editor's Duplicate action.
	//
	// @param dst  Entity receiving the copy.
	// @param src  Entity to copy from.
	static void CopyComponentIfExists(Entity dst, Entity src)
	{
		if (src.HasComponent<T>())
			dst.AddOrReplaceComponent<T>(src.GetComponent<T>());
	}

	Ref<Scene> Scene::Copy(Ref<Scene> other)
	{
		Ref<Scene> newScene = CreateRef<Scene>();

		newScene->m_ViewportWidth = other->m_ViewportWidth;
		newScene->m_ViewportHeight = other->m_ViewportHeight;

		auto &srcSceneRegistry = other->m_Registry;
		auto &dstSceneRegistry = newScene->m_Registry;
		// Source handle -> destination handle. Keyed by handle rather than UUID
		// because UUIDs are not guaranteed unique; see CopyComponent below.
		std::unordered_map<entt::entity, entt::entity> enttMap;

		// Create entities in new scene
		auto idView = srcSceneRegistry.view<IDComponent>();
		for (auto e : idView)
		{
			UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
			const auto &name = srcSceneRegistry.get<TagComponent>(e).Tag;
			Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
			enttMap[e] = (entt::entity)newEntity;
		}

		// Copy components (except IDComponent and TagComponent)
		CopyComponent<TransformComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SpriteRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CircleRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CubeRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SphereRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<PlaneRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<Circle3DRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<IcoSphereRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CylinderRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<ConeRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<TorusRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<GridRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<MaterialComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CameraComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<Rigidbody2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<BoxCollider2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CircleCollider2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);

		CopyComponent<RigidBody3DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<BoxCollider3DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SphereCollider3DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CapsuleCollider3DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<VolumetricFogComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<VolumetricCloudsComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<DirectionalLightComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<PointLightComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SpotLightComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<AreaLightComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<WaterComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<HealthComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<WeaponComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<RaycastIgnoreComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<HittableComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<InteractableComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);

		CopyComponent<NativeScriptComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<ScriptComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		if (other->m_TerrainRuntime.IsConfigured())
		{
			std::string error;
			if (!newScene->ConfigureTerrainRuntime(other->m_TerrainRuntime.GetAsset(),
				other->m_TerrainRuntime.GetWorldState(), other->m_TerrainRuntime.GetConfig(), &error,
				other->GetTerrainAssetReference()))
				WL_CORE_WARN("Could not copy scene terrain runtime: {0}", error);
		}

		{
			auto view = dstSceneRegistry.view<CameraComponent>();
			for (auto entity : view)
			{
				auto &cameraComponent = view.get<CameraComponent>(entity);
				if (!cameraComponent.FixedAspectRatio)
				{
					// Set it to an alternate temporary size first to clear dirty flags
					cameraComponent.Camera.SetViewportSize(100, 100);
					// Restore its actual matrix bounds
					cameraComponent.Camera.SetViewportSize(newScene->m_ViewportWidth, newScene->m_ViewportHeight);
				}
			}
		}

		return newScene;
	}

	// Creates a new entity with a FRESH RANDOM UUID.
	//
	// For runtime and editor use. The UUID comes from `UUID()`, whose default
	// constructor draws from the shared distribution (see UUID.cpp) -- so this
	// does mint a new identity rather than leaving it zero.
	//
	// NOTE: that draw is from namespace-scope statics shared across the whole
	// process and unsynchronized (documented in UUID.cpp). Creating entities
	// from more than one thread therefore races on the generator. Single-
	// threaded scene edits are fine.
	//
	// @param name  The entity's tag. An empty name becomes "Entity" inside
	//              CreateEntityWithUUID rather than being stored empty.
	// @returns     The new entity.
	Entity Scene::CreateEntity(const std::string &name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	// Creates an entity with a SPECIFIC identity.
	//
	// The load/deserialization path: the UUID comes from the scene file rather
	// than being generated, so reopening a scene restores the same entity
	// identities -- which is what makes references to entities (handles held
	// by scripts, parent links) survive a reload.
	//
	// Two consequences worth knowing:
	//
	//   * Every entity is born with exactly ID, Transform and Tag. Those three
	//     are unconditional here, and the deserializer adds everything else on
	//     top afterwards.
	//   * A caller passing a DUPLICATE uuid -- a file containing the same id
	//     twice, or a script calling this with an id already in use -- is not
	//     checked. Two entities end up sharing one UUID, and every
	//     UUID-keyed lookup in the engine (the copy map, the script and
	//     behaviour-tree instance maps) silently resolves to one of them.
	//
	// @param uuid  The identity to assign.
	// @param name  The entity's tag; empty becomes "Entity".
	// @returns     The new entity.
	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string &name)
	{
		Entity entity = {m_Registry.create(), this};
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TransformComponent>();
		auto &tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		// Release per-entity Python state BEFORE the registry drops the
		// components -- ScriptEngine::OnDestroyEntity and
		// BehaviorTreeSystem::OnDestroyEntity both key off IDComponent, which
		// is about to disappear. Neither was called here at all, so every
		// destroyed entity leaked its script instance and its behaviour tree
		// instance for the life of the process.
		if (entity.HasComponent<IDComponent>())
		{
			UUID id = entity.GetComponent<IDComponent>().ID;
			ScriptEngine::OnDestroyEntity(id);
			BehaviorTreeSystem::OnDestroyEntity(id);
		}

		m_Registry.destroy(entity);
	}

	// Brings up the runtime-owned subsystems in order: both physics worlds,
	// then audio output and authored PlayOnStart sources.
	//
	// Called when entering play mode. Scripts are still instantiated lazily on the
	// first OnUpdateRuntime; no water or light state is touched here.
	//
	// The 2D world is started before the 3D one, so body handles allocated in
	// 2D get lower indices. Nothing depends on that today, but it means the
	// order of these two calls is observable rather than arbitrary.
	void Scene::OnRuntimeStart()
	{
		OnPhysics2DStart();
		OnPhysics3DStart();

		// The editor's play lifecycle owns the output device. Headless tests set
		// WASTELAND_DISABLE_AUDIO_OUTPUT=1 and still exercise the deterministic
		// mixer clock without opening a physical endpoint.
		auto audio = GetAudioSystem();
		const char *disableOutput = std::getenv("WASTELAND_DISABLE_AUDIO_OUTPUT");
		const bool outputDisabled = disableOutput &&
			disableOutput[0] == '1' && disableOutput[1] == '\0';
		if (!outputDisabled && !audio->IsOutputRunning())
		{
			std::string error;
			if (!audio->StartOutput(&error))
				WL_CORE_WARN("Audio output unavailable; continuing with the offline mixer: {0}", error);
		}
		audio->PlayAutoStartSources();
	}

	// Tears runtime-owned subsystems down in the reverse order of OnRuntimeStart.
	//
	// Stopping 3D before 2D matters more than starting did: the stop passes
	// walk the registry destroying bodies, and doing them in the opposite
	// order to creation keeps any handle comparisons between the two worlds
	// meaningful while they are still valid.
	//
	// Note this does NOT tear down script instances or behaviour trees --
	// DestroyEntity does that per entity. Leaving play mode therefore has to
	// be paired with whatever restores the scene, or the script state built up
	// during play survives.
	void Scene::OnRuntimeStop()
	{
		if (m_AudioSystem)
		{
			m_AudioSystem->StopAll();
			m_AudioSystem->StopOutput();
		}
		OnPhysics3DStop();
		OnPhysics2DStop();
	}

	// The runtime update: everything that advances while the game is playing,
	// in a deliberate order.
	//
	// Sequence, and why each step sits where it does:
	//
	//   1. UpdateWater          -- waves must advance before anything samples
	//                              the surface, including gameplay queries
	//                              later in this same frame.
	//   2. Health cooldowns      -- timers that damage application reads but
	//                              cannot advance itself.
	//   3. Native scripts       -- imperative per-frame script logic.
	//   4. Python scripts       -- see the note on SetActiveScene below.
	//   5. Behaviour trees      -- deliberately AFTER scripts, so a tree
	//                              reacts to what a script did this frame.
	//   6. StepPhysics          -- bodies move after everything has decided
	//                              where it wants to be, so a script's
	//                              movement is what physics resolves.
	//   7. Camera selection and 2D render setup.
	//
	// The ordering is load-bearing throughout; reordering these changes
	// behaviour rather than just performance.
	//
	// @param ts  The frame timestep, passed on to scripts and physics.
	void Scene::OnUpdateRuntime(Timestep ts)
	{
		// Water simulation always advances (drives waves + gameplay queries)
		UpdateWater(ts);

		// Tick damage lockouts. HealthComponent::ApplyDamage reads
		// CooldownTimer but cannot advance it itself (it has no timestep),
		// so the scene owns the countdown.
		{
			auto view = m_Registry.view<HealthComponent>();
			for (auto e : view)
			{
				auto &health = view.get<HealthComponent>(e);
				if (health.CooldownTimer > 0.0f)
				{
					health.CooldownTimer -= ts.GetSeconds();
					if (health.CooldownTimer < 0.0f)
						health.CooldownTimer = 0.0f;
				}
			}
		}

		// Update scripts
		{
			// Instantiates and ticks every native (C++) script.
			//
			// Scripts are created LAZILY, on the first frame they are seen, rather
			// than in OnRuntimeStart. The guard is Instance being null -- which is also
			// why CopyComponent<NativeScriptComponent> can safely null it on a
			// duplicate: the copy instantiates on its first update.
			//
			// HAZARD, AND IT IS LIVE HERE: the only guard is `if (!nsc.Instance)`, and
			// inside that block InstantiateScript is called UNCONDITIONALLY. That thunk
			// is never initialized unless Bind<T>() ran (see NativeScriptComponent in
			// Components.h) -- it is an indeterminate function pointer on a
			// default-constructed component. A NativeScriptComponent emplaced
			// directly, or emplaced and never bound, therefore jumps through garbage
			// on its first update. Checking `nsc.Instance == nullptr` proves nothing
			// about the thunk being valid.
			//
			// SECOND, ADJACENT HAZARD: the factory's return value is used
			// unconditionally on the next two lines -- `nsc.Instance->m_Entity` and
			// `->OnCreate()` -- with no null check. Even a correctly bound script that
			// returns null from its constructor path dereferences null immediately
			// after, and does so again on the OnUpdate below.
			//
			// The capture is [=] rather than [&], so `this` is copied into the lambda.
			// Harmless but unnecessary for a pointer.
			m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto &nsc)
														  {
					// TODO: Move to Scene::OnScenePlay
					if (!nsc.Instance)
					{
						nsc.Instance = nsc.InstantiateScript();
						nsc.Instance->m_Entity = Entity{ entity, this };
						nsc.Instance->OnCreate();
					}

					nsc.Instance->OnUpdate(ts); });

			// Ticks every entity carrying an authored Python script.
			//
			// Note `ScriptEngine::SetActiveScene(this)` is called INSIDE the loop, so
			// it runs once per scripted entity rather than once per frame. It is
			// idempotent, so the result is correct -- just redundant work, and it grows
			// with the number of scripted entities.
			auto view = m_Registry.view<ScriptComponent>();
			for (auto entity : view)
			{
				Entity e = {entity, this};

				ScriptEngine::SetActiveScene(this);
				ScriptEngine::OnUpdateEntity(e, ts);
			}
		}

		// Authored `.wlbtree` trees. Separate from the block above on
		// purpose: a tree is data that suspends and resumes by node ID, so it
		// must NOT be interleaved with imperative per-frame script state that
		// has no notion of resuming. Trees tick after scripts, so a tree may
		// react to what a script did this frame.
		//
		// This is the call that makes an authored tree more than a demo:
		// without it nothing in the scene ever owns one, and NPC behaviour had
		// to stay imperative in Enemy.py.
		{
			ScriptEngine::SetActiveScene(this);
			auto view = m_Registry.view<BehaviorTreeComponent>();
			for (auto entity : view)
			{
				Entity e = {entity, this};
				BehaviorTreeSystem::OnUpdateEntity(*this, e, ts.GetSeconds());
			}
		}

		// Stream terrain before physics so newly selected chunk colliders exist
		// before this frame's Box3D step.
		if (m_TerrainRuntime.IsConfigured())
		{
			auto view = m_Registry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				if (!view.get<CameraComponent>(entity).Primary)
					continue;
				const auto& position = view.get<TransformComponent>(entity).Translation;
				std::string error;
				if (!UpdateTerrainStreaming(position.x, position.z, &error))
					WL_CORE_WARN("Terrain streaming update failed: {0}", error);
				break;
			}
		}

		// Physics. One call, so the runtime path and the headless
		// StepPhysics path cannot drift apart.
		StepPhysics(ts);

		// Audio follows the post-physics listener transform. The output callback
		// advances voices from rendered frames; when no device is active, Update
		// advances the same mixer from the scene timestep for headless use.
		if (m_AudioSystem)
			m_AudioSystem->Update(ts.GetSeconds());

		// Render 2D
		Camera *mainCamera = nullptr;
		glm::mat4 cameraTransform;
		{
			auto view = m_Registry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = transform.GetTransform();
					break;
				}
			}
		}

		if (mainCamera)
		{
			Renderer2D::BeginScene(mainCamera->GetProjection(), cameraTransform);

			// Draw sprites
			{
				auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
				for (auto entity : group)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

					Renderer2D::DrawSprite(transform.GetTransform(), sprite, (int)entity);
				}
			}

			// Draw circles
			{
				auto view = m_Registry.view<TransformComponent, CircleRendererComponent>();
				for (auto entity : view)
				{
					auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

					Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, (int)entity);
				}
			}

			Renderer2D::EndScene();

			// Render 3D
			Renderer3D::BeginScene(mainCamera->GetProjection(), cameraTransform);
			SubmitVolumetricVolumes();
			SubmitSceneLights();
			SubmitWaterSurfaces();
			SubmitMeshPrimitives();
			SubmitTerrainChunks();

			// Draw Cubes
			{
				auto cubeView = m_Registry.view<TransformComponent, CubeRendererComponent, MaterialComponent>();
				for (auto entity : cubeView)
				{
					auto &tc = cubeView.get<TransformComponent>(entity);
					auto &crc = cubeView.get<CubeRendererComponent>(entity);
					auto &mc = cubeView.get<MaterialComponent>(entity);

					// Pass the 3D entity data to the new batch renderer
					Renderer3D::DrawCube(tc.GetTransform(), crc.Color, mc, (int)entity);
				}
			}

			// Draw Spheres
			{
				auto sphereView = m_Registry.view<TransformComponent, SphereRendererComponent, MaterialComponent>();
				for (auto entity : sphereView)
				{
					auto &tc = sphereView.get<TransformComponent>(entity);
					auto &src = sphereView.get<SphereRendererComponent>(entity);
					auto &mc = sphereView.get<MaterialComponent>(entity);

					Renderer3D::DrawSphere(
						tc.GetTransform(),
						src.Color,
						src.Radius,
						src.Sectors,
						src.Stacks,
						mc,
						(int)entity);
				}
			}

			Renderer3D::EndScene();
		}
	}

	// Gives a Box3D body to any 3D rigidbody that does not have one yet.
	//
	// Exists because OnPhysics3DStart used to be the only place bodies were
	// created, so anything added afterwards -- a dropped item, a mid-level
	// enemy, anything a script spawned -- kept RuntimeBody == 0 forever and
	// hung in the air, non-interactive, with nothing logged. The comment below
	// records that history.
	//
	// Polling per step rather than hooking component addition is the right
	// trade here: any path that adds an entity -- editor, script,
	// deserialization -- is covered without each having to remember to
	// register. The cost is a full view scan every frame.
	//
	// SECOND SITE OF THE COPIED-HANDLE BUG: the `RuntimeBody != 0` test below
	// is the same one that appears in CreatePhysicsBody3D, and it cannot tell
	// "this entity legitimately has a body" from "this handle was copied in
	// from a duplicated entity". So a duplicate never reaches
	// CreatePhysicsBody3D at all -- it is skipped here first, having already
	// been skipped there. Two independent guards, both defeated by the same
	// input.
	//
	// 2D NOW HAS ITS OWN PASS: SyncPendingPhysics2D below, with
	// CreatePhysicsBody2D doing the per-entity work. Before it existed,
	// Rigidbody2DComponent::RuntimeBody was assigned in exactly one place in
	// the whole engine -- inside OnPhysics2DStart -- so a 2D rigidbody added
	// after startup got no Box2D body EVER: the read-back in
	// StepPhysicsFixed skipped it because its pointer was null, and nothing
	// ever created it. The symptom was a 2D body that never fell, never
	// collided, and never blocked anything, with no error. The same bug the
	// comment above describes for 3D, diagnosed and fixed there while 2D kept
	// it. tests/headless/test_physics_2d.py is the reproducer.
	//
	// The b3World_IsValid guard makes the whole function a no-op when the 3D
	// world is absent, so this is safe to call unconditionally each step.
	void Scene::SyncPendingPhysicsBodies()
	{
		if (!b3World_IsValid(m_Physics3DWorld))
			return;

		// Entities whose RigidBody3DComponent exists but whose body was never
		// created. Before this, OnPhysics3DStart was the ONLY place bodies were
		// made, so anything added after startup kept RuntimeBody == 0 forever:
		// a dropped item or a mid-level enemy hung in the air, fully
		// non-interactive, with no error to indicate why.
		//
		// Scanned once per step rather than hooked to component addition, so a
		// body added from any path -- editor, script, deserialization -- is
		// picked up without every call site having to remember.
		auto view = m_Registry.view<RigidBody3DComponent>();
		for (auto e : view)
		{
			if (view.get<RigidBody3DComponent>(e).RuntimeBody != 0)
				continue;
			CreatePhysicsBody3D(e);
		}
	}

	// Gives a Box2D body to any 2D rigidbody that does not have one yet.
	//
	// The 2D counterpart of SyncPendingPhysicsBodies, and it exists for the
	// same reason. OnPhysics2DStart used to be the only place a b2Body was
	// ever created, so a Rigidbody2DComponent added afterwards -- a dropped
	// item, a spawned prop, a character that walked in through a portal --
	// kept RuntimeBody == nullptr FOREVER. The read-back in StepPhysicsFixed
	// skips a null body (dereferencing it would take the scene down), so the
	// entity simply sat at its authored position: it never fell, never
	// collided, and never blocked anything, with nothing logged and no way to
	// tell from a script which of those was wrong.
	//
	// Why a scan rather than a hook on component addition: the same argument
	// as the 3D pass above. Editor, script and deserialization all add
	// entities, and each one would otherwise have to remember to register the
	// body. A full view scan per sub-step is cheap next to what the solver
	// just did.
	//
	// GUARD IS THE NULL WORLD, NOT A VALIDITY PREDICATE. Box3D exposes
	// b3World_IsValid; Box2D has no equivalent, and m_PhysicsWorld is a raw
	// pointer that OnPhysics2DStop deletes. So the test is `!m_PhysicsWorld`,
	// which also makes this a no-op before startup and after teardown --
	// safe to call unconditionally, exactly like the 3D pass above.
	//
	// The runtime bodies are owned by the world, not by the components, so
	// this creates handles for Box2D to own and nothing for us to free.
	void Scene::SyncPendingPhysics2D()
	{
		if (!m_PhysicsWorld)
			return;

		// Entities whose Rigidbody2DComponent exists but whose b2Body was
		// never created -- the whole set of late additions, however they got
		// here.
		auto view = m_Registry.view<Rigidbody2DComponent>();
		for (auto e : view)
		{
			if (view.get<Rigidbody2DComponent>(e).RuntimeBody != nullptr)
				continue;
			CreatePhysicsBody2D(e);
		}
	}

	// Advances both physics worlds by one step, then reads the results back
	// into the entities' transforms.
	//
	// Called once per frame from OnUpdateRuntime, and also directly by the
	// headless path -- the comment in OnUpdateRuntime notes this is the point
	// of having one entry point, so the two cannot drift apart.
	//
	// ORDER: pending bodies are created BEFORE the solver runs -- in BOTH
	// worlds, SyncPendingPhysicsBodies and SyncPendingPhysics2D -- so a body
	// added since the last frame participates in this one rather than appearing
	// a frame late.
	//
	// FIXED TIMESTEP: frame-rate independent, deliberately so. The frame dt is
	// banked in m_PhysicsAccumulator and spent in kFixedStep (1/60s) chunks via
	// StepPhysicsFixed, so the solver always integrates the same step SIZE
	// whether the caller runs at 144fps or 20fps. Trajectories for a given
	// input are therefore identical across frame rates.
	//
	// @param seconds  The frame delta. Values <= 0 return immediately rather
	//                 than stepping backwards.
	void Scene::StepPhysics(float seconds)
	{
		if (seconds <= 0.0f)
			return;

		// Bodies added since the last step get theirs now, before the solver
		// runs, so they participate in this frame instead of appearing a
		// frame late. Both worlds, and deliberately in that order: the 3D pass
		// is the older of the two and the 2D pass was added beside it, so
		// 3D-first matches the order the two passes appear in the file.
		SyncPendingPhysicsBodies();
		SyncPendingPhysics2D();

		// Fixed-timestep accumulator.
		//
		// The previous behaviour clamped dt to 1/30 and DISCARDED the excess.
		// That is correct for a single enormous dt -- a 10s step after a
		// breakpoint would tunnel every body through the level -- but it is
		// badly wrong for an ordinary slow frame. Clamping throws away real
		// time, so the simulation runs slower than the clock, and the slower
		// the frame the worse it gets:
		//
		//   measured, 1.0s of free fall (ideal 4.905m):
		//     144fps -> 4.914m (100%)     30fps -> 4.946m (100%)
		//      20fps -> 2.207m  (45%)     10fps -> 0.559m  (11%)
		//
		// So a machine struggling at 10fps gets a game in quarter-speed, with
		// no error, no log, and a green test suite. The existing 30fps test
		// passed only because 30fps is exactly the clamp boundary.
		//
		// The fix is the standard one: bank the leftover time and spend it in
		// fixed-size steps, so behaviour depends on the SIZE of each step and
		// not on how often the caller happens to call us.
		//
		// Two bounds keep this honest:
		//   - kFixedStep is what the solver actually integrates, so the
		//     trajectory no longer varies with frame rate.
		//   - kMaxSubSteps caps how much one frame may catch up. Without it, a
		//     long stall banks 10 seconds and then runs 600 steps in one
		//     frame -- a worse stall than the one it was recovering from. The
		//     cap keeps tunnelling protection intact: at most
		//     kMaxSubSteps*kFixedStep of simulated time per frame, and any
		//     remainder older than that is dropped rather than chased.
		constexpr float kFixedStep = 1.0f / 60.0f;
		constexpr int32_t kMaxSubSteps = 8;

		m_PhysicsAccumulator += seconds;

		int32_t subSteps = 0;
		while (m_PhysicsAccumulator >= kFixedStep && subSteps < kMaxSubSteps)
		{
			StepPhysicsFixed(kFixedStep);
			m_PhysicsAccumulator -= kFixedStep;
			++subSteps;
		}

		// Outran the cap. Drop the backlog rather than carry it into the next
		// frame, where it would compound.
		if (m_PhysicsAccumulator >= kFixedStep)
			m_PhysicsAccumulator = std::fmod(m_PhysicsAccumulator, kFixedStep);	}

	// One fixed-size integration step. StepPhysics owns the accumulator and
	// calls this; nothing else should. Keeping the solver work in its own
	// function is what makes "the step size is constant" checkable by reading
	// one function rather than reasoning about a loop.
	void Scene::StepPhysicsFixed(float seconds)
	{
		if (seconds <= 0.0f)
			return;

		// Bodies added since the last sub-step get theirs now, before the
		// solver runs, so they participate in this step instead of appearing a
		// step late.
		SyncPendingPhysicsBodies();
		SyncPendingPhysics2D();

		// Solver iteration counts.
		//
		// These are Box2D's, and are used for BOTH worlds:
		//   - 2D: b2World::Step(seconds, 6, 2).
		//   - 3D: b3World_Step(..., 4) -- only FOUR velocity iterations.
		// So the 3D solver is deliberately less converged than the 2D one. That is
		// a defensible performance trade for a scene with many bodies, but it is
		// an asymmetry worth knowing about: stacks and joints will visibly sag and
		// jitter more in 3D than the equivalent 2D construction, because the 3D
		// solver stops earlier in its convergence.
		//
		// Neither is adaptive to the scene, so a scene of resting boxes and a
		// scene of fast projectiles get identical solver effort.
		const int32_t velocityIterations = 6;
		const int32_t positionIterations = 2;

		// --- 2D (Box2D) ---
		if (m_PhysicsWorld)
		{
			m_PhysicsWorld->Step(seconds, velocityIterations, positionIterations);

			auto view = m_Registry.view<Rigidbody2DComponent>();
			for (auto e : view)
			{
				Entity entity = {e, this};
				auto &transform = entity.GetComponent<TransformComponent>();
				auto &rb2d = entity.GetComponent<Rigidbody2DComponent>();

				b2Body *body = (b2Body *)rb2d.RuntimeBody;
				// No pointer yet. Normally unreachable -- SyncPendingPhysics2D ran a
				// few lines above and created anything missing -- so this guard is
				// now a backstop rather than the normal late-add path it used to be,
				// when a body added after OnPhysics2DStart stayed null here forever
				// and the entity simply never moved.
				//
				// Skipping is still correct rather than papering over it: there is
				// nothing to read back, and dereferencing null would take the whole
				// scene down. The distinction matters because the two states are
				// indistinguishable here -- "not created yet" and "created but
				// already destroyed with the world" both read as null, and this
				// loop cannot tell them apart. SyncPendingPhysics2D cannot be given
				// the job either, because it runs against a live world.
				if (!body)
					continue;

				const auto &position = body->GetPosition();
				// Same rule as the 3D path below: a Kinematic body is driven by
				// code, so reading it back into the transform would overwrite
				// whatever the script wrote this frame. Push the transform into
				// the body instead and leave the transform alone.
				if (rb2d.Type == Rigidbody2DComponent::BodyType::Kinematic)
				{
					body->SetTransform(b2Vec2(transform.Translation.x, transform.Translation.y),
									   transform.Rotation.z);
					continue;
				}
				transform.Translation.x = position.x;
				transform.Translation.y = position.y;
				transform.Rotation.z = body->GetAngle();
			}
		}

		// --- 3D (Box3D) ---
		if (!b3World_IsValid(m_Physics3DWorld))
			return;

		// Advances the Box3D world by one step.
		//
		// The literal 4 is the velocity iteration count -- see the note on the Box2D
		// iteration constants above for why 3D uses fewer than 2D.
		//
		// Note the early return just above: if the 3D world is invalid this
		// function has ALREADY stepped the 2D world. That ordering is fine -- 2D
		// and 3D are independent -- but it means the 2D step happens even on a
		// build with no 3D physics at all, and the 3D read-back below is skipped
		// while the 2D one already completed.
		b3World_Step(m_Physics3DWorld, seconds, 4);

		auto view3d = m_Registry.view<RigidBody3DComponent>();
		for (auto e : view3d)
		{
			Entity entity = {e, this};
			auto &transform = entity.GetComponent<TransformComponent>();
			auto &rb3d = entity.GetComponent<RigidBody3DComponent>();

			// Same late-add case as 2D: RuntimeBody stays 0 until the body is
			// actually created, and b3LoadBodyId(0) is not a valid handle.
			if (rb3d.RuntimeBody == 0)
				continue;

			b3BodyId bodyId = b3LoadBodyId(rb3d.RuntimeBody);
			if (!b3Body_IsValid(bodyId))
				continue;

			// A Kinematic body is moved BY CODE, not by the solver. Reading the
			// body back into the transform here -- AFTER the script pass --
			// discards whatever a script or behaviour tree wrote this frame:
			// the body has not moved, so the read-back restores the pre-script
			// transform and the NPC stands still with no error anywhere.
			//
			// Verified against the built module: EnemyChase walked 12.6 m with
			// physics stopped and 0.0 m with it running, and the hand-written
			// Enemy_legacy_reference.py behaved identically -- so this is the
			// mechanism, not the tree.
			//
			// Kinematic means "code drives it", so the data flows the OTHER way:
			// push the transform into the body (so collision detection sees the
			// new position this step) and leave the transform alone. Dynamic and
			// Static keep the read-back below, which is where their motion comes
			// from.
			if (rb3d.Type == RigidBody3DType::Kinematic)
			{
				b3Body_SetTransform(bodyId,
									{(float)transform.Translation.x, (float)transform.Translation.y, (float)transform.Translation.z},
									EulerToB3Quat(transform.Rotation));
				continue;
			}

			b3Pos pos = b3Body_GetPosition(bodyId);
			b3Quat rot = b3Body_GetRotation(bodyId);

			transform.Translation.x = (float)pos.x;
			transform.Translation.y = (float)pos.y;
			transform.Translation.z = (float)pos.z;
			transform.Rotation = B3QuatToEuler(rot);
		}
	}

	void Scene::OnSimulationStart()
	{
		OnPhysics2DStart();
		OnPhysics3DStart();
	}

	void Scene::OnSimulationStop()
	{
		OnPhysics2DStop();
		OnPhysics3DStop();
	}

	void Scene::OnUpdateSimulation(Timestep ts, EditorCamera &camera)
	{
		// Water simulation always advances (drives waves + gameplay queries)
		UpdateWater(ts);
		const auto& position = camera.GetPosition();
		if (m_TerrainRuntime.IsConfigured())
		{
			std::string error;
			if (!UpdateTerrainStreaming(position.x, position.z, &error))
				WL_CORE_WARN("Terrain streaming update failed: {0}", error);
		}

		// Physics. One call, so the runtime path and the headless
		// StepPhysics path cannot drift apart.
		StepPhysics(ts);

		// Render
		RenderScene(camera);
	}

	void Scene::OnUpdateEditor(Timestep ts, EditorCamera &camera)
	{
		// Water preview animates in the editor viewport
		UpdateWater(ts);
		const auto& position = camera.GetPosition();
		if (m_TerrainRuntime.IsConfigured())
		{
			std::string error;
			if (!UpdateTerrainStreaming(position.x, position.z, &error))
				WL_CORE_WARN("Terrain streaming update failed: {0}", error);
		}

		// Render
		RenderScene(camera);
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;

		// Resize out non-FixedAspectRatio cameras
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			auto &cameraComponent = view.get<CameraComponent>(entity);
			if (!cameraComponent.FixedAspectRatio)
				cameraComponent.Camera.SetViewportSize(width, height);
		}
	}

	// Duplicates an entity and all of its components -- the editor's
	// Duplicate action.
	//
	// Creates a brand-new entity with a FRESH UUID (via CreateEntity, not
	// CreateEntityWithUUID) and copies each component type across in an
	// explicit, hand-written list. New identity is correct: a duplicate is a
	// distinct entity, and reusing the source's UUID would collide with it.
	//
	// The component list is the mechanism, and its weakness. There is no
	// reflection, so this list is the only place a component type gets copied;
	// a component added to Components.h is NOT duplicated until it is added
	// here, and the omission is silent -- the duplicate simply lacks it.
	//
	// BUG, reachable directly from this function: the physics components in
	// this list are copied through the generic path, so the duplicate inherits
	// the source's RuntimeBody / RuntimeShape / RuntimeFixture handle.
	// CreatePhysicsBody3D then early-returns for the duplicate because that
	// handle is already non-zero, so no second body is ever created and the
	// two entities share one simulated body -- moving either moves both, and
	// teardown destroys it twice.
	//
	// The script and camera components are deliberately NOT in the list,
	// which is how the missing specializations above are worked around here.
	// The physics components were left in.
	//
	// @param entity  The entity to duplicate. Its name is carried over.
	void Scene::DuplicateEntity(Entity entity)
	{
		std::string name = entity.GetName();
		Entity newEntity = CreateEntity(name);

		CopyComponentIfExists<TransformComponent>(newEntity, entity);
		CopyComponentIfExists<SpriteRendererComponent>(newEntity, entity);
		CopyComponentIfExists<CircleRendererComponent>(newEntity, entity);
		CopyComponentIfExists<CubeRendererComponent>(newEntity, entity);
		CopyComponentIfExists<SphereRendererComponent>(newEntity, entity);
		CopyComponentIfExists<PlaneRendererComponent>(newEntity, entity);
		CopyComponentIfExists<Circle3DRendererComponent>(newEntity, entity);
		CopyComponentIfExists<IcoSphereRendererComponent>(newEntity, entity);
		CopyComponentIfExists<CylinderRendererComponent>(newEntity, entity);
		CopyComponentIfExists<ConeRendererComponent>(newEntity, entity);
		CopyComponentIfExists<TorusRendererComponent>(newEntity, entity);
		CopyComponentIfExists<GridRendererComponent>(newEntity, entity);
		CopyComponentIfExists<MaterialComponent>(newEntity, entity);
		CopyComponentIfExists<CameraComponent>(newEntity, entity);
		CopyComponentIfExists<Rigidbody2DComponent>(newEntity, entity);
		CopyComponentIfExists<BoxCollider2DComponent>(newEntity, entity);
		CopyComponentIfExists<CircleCollider2DComponent>(newEntity, entity);

		CopyComponentIfExists<RigidBody3DComponent>(newEntity, entity);
		CopyComponentIfExists<BoxCollider3DComponent>(newEntity, entity);
		CopyComponentIfExists<SphereCollider3DComponent>(newEntity, entity);
		CopyComponentIfExists<CapsuleCollider3DComponent>(newEntity, entity);
		CopyComponentIfExists<VolumetricFogComponent>(newEntity, entity);
		CopyComponentIfExists<VolumetricCloudsComponent>(newEntity, entity);
		CopyComponentIfExists<DirectionalLightComponent>(newEntity, entity);
		CopyComponentIfExists<PointLightComponent>(newEntity, entity);
		CopyComponentIfExists<SpotLightComponent>(newEntity, entity);
		CopyComponentIfExists<AreaLightComponent>(newEntity, entity);
		CopyComponentIfExists<WaterComponent>(newEntity, entity);
		CopyComponentIfExists<HealthComponent>(newEntity, entity);
		CopyComponentIfExists<WeaponComponent>(newEntity, entity);
		CopyComponentIfExists<RaycastIgnoreComponent>(newEntity, entity);
		CopyComponentIfExists<HittableComponent>(newEntity, entity);
		CopyComponentIfExists<InteractableComponent>(newEntity, entity);

		CopyComponentIfExists<NativeScriptComponent>(newEntity, entity);
		CopyComponentIfExists<ScriptComponent>(newEntity, entity);
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			const auto &camera = view.get<CameraComponent>(entity);
			if (camera.Primary)
				return Entity{entity, this};
		}
		return {};
	}

	bool Scene::IsEntityValid(Entity entity) const
	{
		return m_Registry.valid(entity);
	}

	void Scene::ReloadMaterialTextures()
	{
		uint32_t reloaded = 0, missing = 0;
		auto view = m_Registry.view<MaterialComponent>();
		for (auto e : view)
		{
			auto &mc = view.get<MaterialComponent>(e);
			if (mc.TexturePath.empty())
				continue;
			if (!std::filesystem::is_regular_file(mc.TexturePath))
			{
				missing++;
				continue;
			}
			mc.Texture = Texture2D::Create(mc.TexturePath);
			if (mc.Texture)
				reloaded++;
			else
				missing++;
		}
		WL_CORE_INFO("Scene::ReloadMaterialTextures: reloaded {0} material textures ({1} missing)", reloaded, missing);
	}

	// Creates the Box2D world and registers every 2D rigidbody that exists at
	// this moment.
	//
	// Delegates each body to CreatePhysicsBody2D rather than inlining the
	// work -- which is what lets the same function serve both this startup pass
	// and the per-step SyncPendingPhysics2D pass for late additions. Before that
	// split existed, this was the ONLY place a b2Body could ever come into
	// existence, and a Rigidbody2DComponent added later got no body at all.
	//
	// Gravity is -9.8 here against -9.81 in the 3D world below. A negligible
	// difference physically, but it means a 2D and a 3D body in the same scene
	// fall at measurably different rates, which shows up in slow-motion or
	// long-fall comparisons.
	//
	// Handles are raw `new`/raw pointers, freed in OnPhysics2DStop. The world
	// is checked for null at every use site rather than being a smart pointer.
	//
	// Does NOT delete a pre-existing m_PhysicsWorld first, so calling this twice
	// leaks the first world AND leaves every RuntimeBody dangling into it. Both
	// callers (OnRuntimeStart, OnSimulationStart) transition from a stopped
	// scene, so it does not happen in practice -- but nothing here enforces
	// that, and it is the one way this function can now be made worse than the
	// version it replaced.
	void Scene::OnPhysics2DStart()
	{
		m_PhysicsWorld = new b2World({0.0f, -9.8f});

		auto view = m_Registry.view<Rigidbody2DComponent>();
		for (auto e : view)
			CreatePhysicsBody2D(e);
	}

	// Registers one entity's Box2D body and its collider shape(s), if the 2D
	// world exists.
	//
	// Idempotent, for the same reason CreatePhysicsBody3D is: an entity that
	// already has a RuntimeBody is skipped. Without that guard the per-step
	// SyncPendingPhysics2D pass would create a SECOND body for every entity on
	// its second sub-step -- leaking the first, orphaning its fixtures, and
	// leaving the read-back driving a body nobody steps while the one actually
	// in the world moves off somewhere else. The guard is also what stops a
	// startup body being rebuilt on the first step.
	//
	// IT CANNOT DISTINGUISH "ALREADY BUILT FOR THIS ENTITY" FROM "HANDLE COPIED
	// IN FROM A DUPLICATE", for the same reason CreatePhysicsBody3D's cannot:
	// both look identical, a non-null pointer, so a duplicate is treated as
	// already registered, never gets a body, and silently drives the source
	// entity's body instead. The fix belongs in CopyComponent (clearing the
	// handle, as the script components already do), not here.
	//
	// NOTE THE POINTER, NOT AN ID. Box3D packs a b3BodyId into an integer and
	// validates it with b3Body_IsValid; Box2D hands out a raw b2Body*, so
	// "already built" is a plain null check and there is no validity predicate
	// to consult. The consequence is that RuntimeBody is DANGLING once
	// OnPhysics2DStop has deleted the world, and a non-null value there cannot
	// be told apart from a live handle. That is why the world-null guard comes
	// FIRST and returns before anything is read or created.
	//
	// The null-world guard also makes this safe for an entity whose body was
	// never created, and a complete no-op before OnPhysics2DStart.
	//
	// @param e  The entt handle of the entity whose body should exist.
	void Scene::CreatePhysicsBody2D(entt::entity e)
	{
		if (!m_PhysicsWorld)
			return;

		Entity entity = {e, this};
		auto &transform = entity.GetComponent<TransformComponent>();
		auto &rb2d = entity.GetComponent<Rigidbody2DComponent>();

		// Already built. Re-creating would leak the old body and orphan its
		// fixtures, and a body added late would get a duplicate on the next
		// pass.
		if (rb2d.RuntimeBody != nullptr)
			return;

		b2BodyDef bodyDef;
		bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
		bodyDef.position.Set(transform.Translation.x, transform.Translation.y);
		bodyDef.angle = transform.Rotation.z;

		b2Body *body = m_PhysicsWorld->CreateBody(&bodyDef);
		body->SetFixedRotation(rb2d.FixedRotation);
		rb2d.RuntimeBody = body;

		if (entity.HasComponent<BoxCollider2DComponent>())
		{
			auto &bc2d = entity.GetComponent<BoxCollider2DComponent>();

			b2PolygonShape boxShape;
			// TWO REAL ISSUES ON THIS ONE LINE.
			//
			// 1. bc2d.Offset IS SILENTLY IGNORED. Box2D's SetAsBox takes an optional
			//    centre argument for exactly this, and it is not passed -- so the 2D box
			//    collider is always centred on the entity, whatever Offset says. The
			//    field is honoured by the circle collider below and by both 3D shapes,
			//    so it works everywhere except here.
			//
			//    The symptom: an entity with BoxCollider2DComponent::Offset set to
			//    (0, 0.5) to put a foot-level collider under a waist-origin model gets
			//    a collider centred at the waist instead. Nothing errors; the
			//    collision is simply in the wrong place.
			//
			// 2. THE ARGUMENTS ARE HALF-EXTENTS, BUT THE FIELD IS NAMED Size.
			//    SetAsBox takes half-width and half-height. BoxCollider2DComponent::Size
			//    is documented as a size with a default of {0.5, 0.5} -- so the default
			//    produces a 1x1 box, twice the field value.
			//
			//    Compare the 3D equivalent, which is named BoxCollider3DComponent::
			//    HalfExtents precisely because that is what it holds. The 2D field is
			//    misnamed, and a caller who enters 2 expecting a 2x2 box gets a 4x4 one.
			//
			// Scaling by the entity's Scale here is consistent with the 3D path and
			// is baked in at creation time -- later Scale changes do not resize the
			// fixture. Extracting this into a function does not change that, but it
			// does mean the bake now happens per-late-addition as well as at startup
			// -- which is the more useful moment, because a body appearing mid-level
			// gets a fixture matching the Scale it has when it appears.
			boxShape.SetAsBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);

			b2FixtureDef fixtureDef;
			fixtureDef.shape = &boxShape;
			fixtureDef.density = bc2d.Density;
			fixtureDef.friction = bc2d.Friction;
			fixtureDef.restitution = bc2d.Restitution;
			fixtureDef.restitutionThreshold = bc2d.RestitutionThreshold;
			body->CreateFixture(&fixtureDef);
		}

		if (entity.HasComponent<CircleCollider2DComponent>())
		{
			auto &cc2d = entity.GetComponent<CircleCollider2DComponent>();

			b2CircleShape circleShape;
			circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
			// Circle radius, scaled by Scale.X ONLY.
			//
			// INCONSISTENT WITH 3D. SphereCollider3DComponent uses
			// max({Scale.x, Scale.y, Scale.z}) and the capsule uses
			// max({Scale.x, Scale.z}); this takes a single axis. Under non-uniform scale
			// a 2D circle therefore gets an arbitrary radius from one axis rather than
			// the largest, so an entity scaled (2, 0.1, 1) collides with a circle twice
			// its authored radius inside a visually squashed body.
			//
			// The Offset IS honoured here, via the shape's centre -- which is what makes
			// the box's ignored Offset above stand out as an oversight rather than a
			// deliberate difference.
			circleShape.m_radius = transform.Scale.x * cc2d.Radius;

			b2FixtureDef fixtureDef;
			fixtureDef.shape = &circleShape;
			fixtureDef.density = cc2d.Density;
			fixtureDef.friction = cc2d.Friction;
			fixtureDef.restitution = cc2d.Restitution;
			fixtureDef.restitutionThreshold = cc2d.RestitutionThreshold;
			body->CreateFixture(&fixtureDef);
		}
	}

	// Destroys the Box2D world.
	//
	// delete-then-null, so a second call is safe and every use site can rely on
	// the null check. Destroying the world destroys all bodies and fixtures it
	// owns, which is what makes the raw `b2Body*` pointers in
	// Rigidbody2DComponent::RuntimeBody dangling rather than leaked -- they
	// must never be dereferenced after this point, and the component is not
	// cleared.
	//
	// ScriptEngine::Shutdown() HERE IS A LAYERING VIOLATION, and it is
	// load-bearing for a bug elsewhere. Nothing about the Python interpreter
	// has any connection to 2D physics, yet stopping the 2D world tears the
	// whole script engine down. Since OnRuntimeStop calls this BEFORE
	// OnPhysics3DStop, the 3D world is torn down with scripting already dead,
	// and any teardown path that needs a script callback cannot use one.
	//
	// Worse, the consequence depends on the stop ORDER, which the header does
	// not document: stop only 2D and every script in the process dies while 3D
	// physics keeps running; stop 3D only and scripts survive. That asymmetry is
	// the kind of thing that shows up much later as a crash in an unrelated
	// system.
	void Scene::OnPhysics2DStop()
	{
		delete m_PhysicsWorld;
		m_PhysicsWorld = nullptr;

		ScriptEngine::Shutdown();
	}

	// Creates the Box3D world and registers every 3D rigidbody.
	//
	// Delegates each body to CreatePhysicsBody3D rather than inlining the
	// work -- which is what lets the same function serve both the startup pass
	// and the per-frame SyncPendingPhysicsBodies pass for late additions.
	//
	// Gravity is -9.81 against the 2D world's -9.8. See the note in
	// OnPhysics2DStart.
	//
	// No error checking on b3CreateWorld: if it fails, m_Physics3DWorld is
	// invalid, and every 3D call site guards with b3World_IsValid rather than
	// checking here -- so a failure is a permanently inert 3D world rather than
	// a reported error.
	void Scene::OnPhysics3DStart()
	{
		b3WorldDef worldDef = b3DefaultWorldDef();
		worldDef.gravity = {0.0f, -9.81f, 0.0f};
		m_Physics3DWorld = b3CreateWorld(&worldDef);
		m_TerrainRuntime.SetPhysicsWorld(m_Physics3DWorld);

		auto view = m_Registry.view<RigidBody3DComponent>();
		for (auto e : view)
			CreatePhysicsBody3D(e);
	}

	// Registers one entity's Box3D body and its collider shape(s), if the 3D
	// world exists.
	//
	// The guard on a pre-existing handle is what turns DuplicateEntity's
	// copied handle into a shared body rather than a second one:
	//
	//   if (rb3d.RuntimeBody != 0) return;
	//
	// A genuinely already-built body must not be rebuilt -- that would leak the
	// old body and orphan its shapes, and a body added late would otherwise
	// get a duplicate on the next startup pass. Both are real, and the
	// comment above the guard says so.
	//
	// The guard cannot tell "already built for this entity" from "handle",
	// copied in from a duplicate", because both look identical: a non-zero
	// integer. So a duplicate is treated as already registered, never gets a
	// body, and silently drives the source entity's body instead.
	//
	// The fix belongs in CopyComponent (a specialization clearing the handle,
	// as the script components already do) rather than here -- this function
	// cannot tell the two cases apart.
	//
	// @param e  The entt handle of the entity whose body should exist.
	//         Returns without doing anything if the 3D world is invalid, so
	//         it is safe to call for an entity whose body was never created.
	void Scene::CreatePhysicsBody3D(entt::entity e)
	{
		if (!b3World_IsValid(m_Physics3DWorld))
			return;

		{
			Entity entity = {e, this};
			auto &transform = entity.GetComponent<TransformComponent>();
			auto &rb3d = entity.GetComponent<RigidBody3DComponent>();

			// Already built. Re-creating would leak the old body and orphan
			// its shapes, and a body added late would get a second one on the
			// next startup pass.
			if (rb3d.RuntimeBody != 0)
				return;

			b3BodyDef bodyDef = b3DefaultBodyDef();
			bodyDef.type = RigidBody3DTypeToBox3D(rb3d.Type);
			bodyDef.position = {(float)transform.Translation.x, (float)transform.Translation.y, (float)transform.Translation.z};
			bodyDef.rotation = EulerToB3Quat(transform.Rotation);
			bodyDef.gravityScale = rb3d.GravityScale;
			bodyDef.linearDamping = rb3d.LinearDamping;
			bodyDef.angularDamping = rb3d.AngularDamping;
			bodyDef.motionLocks.angularX = rb3d.FixedRotationX;
			bodyDef.motionLocks.angularY = rb3d.FixedRotationY;
			bodyDef.motionLocks.angularZ = rb3d.FixedRotationZ;

			b3BodyId bodyId = b3CreateBody(m_Physics3DWorld, &bodyDef);

			// Store body ID
			rb3d.RuntimeBody = b3StoreBodyId(bodyId);

			// Box collider
			if (entity.HasComponent<BoxCollider3DComponent>())
			{
				auto &bc = entity.GetComponent<BoxCollider3DComponent>();
				b3BoxHull boxHull = b3MakeOffsetBoxHull(
					bc.HalfExtents.x * transform.Scale.x,
					bc.HalfExtents.y * transform.Scale.y,
					bc.HalfExtents.z * transform.Scale.z,
					{bc.Offset.x, bc.Offset.y, bc.Offset.z});

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = bc.Density;
				shapeDef.baseMaterial.friction = bc.Friction;
				shapeDef.baseMaterial.restitution = bc.Restitution;

				b3ShapeId shapeId = b3CreateHullShape(bodyId, &shapeDef, &boxHull.base);
				bc.RuntimeShape = b3StoreShapeId(shapeId);
			}

			// Sphere collider
			if (entity.HasComponent<SphereCollider3DComponent>())
			{
				auto &sc = entity.GetComponent<SphereCollider3DComponent>();
				b3Sphere sphere;
				sphere.center = {sc.Offset.x, sc.Offset.y, sc.Offset.z};
				sphere.radius = sc.Radius * std::max({transform.Scale.x, transform.Scale.y, transform.Scale.z});

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = sc.Density;
				shapeDef.baseMaterial.friction = sc.Friction;
				shapeDef.baseMaterial.restitution = sc.Restitution;

				b3ShapeId shapeId = b3CreateSphereShape(bodyId, &shapeDef, &sphere);
				sc.RuntimeShape = b3StoreShapeId(shapeId);
			}

			// Capsule collider
			if (entity.HasComponent<CapsuleCollider3DComponent>())
			{
				auto &cc = entity.GetComponent<CapsuleCollider3DComponent>();
				float halfH = cc.Height * 0.5f * transform.Scale.y;
				float r = cc.Radius * std::max(transform.Scale.x, transform.Scale.z);

				b3Capsule capsule;
				capsule.center1 = {cc.Offset.x, cc.Offset.y - halfH, cc.Offset.z};
				capsule.center2 = {cc.Offset.x, cc.Offset.y + halfH, cc.Offset.z};
				capsule.radius = r;

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = cc.Density;
				shapeDef.baseMaterial.friction = cc.Friction;
				shapeDef.baseMaterial.restitution = cc.Restitution;

				b3ShapeId shapeId = b3CreateCapsuleShape(bodyId, &shapeDef, &capsule);
				cc.RuntimeShape = b3StoreShapeId(shapeId);
			}
		}
	}

	// Resolves an entity's live Box3D body handle, or B3_NULL_ID when there is
		// nothing to drive.
		//
		// THE reason these three exist as Scene methods instead of handing
		// RigidBody3DComponent::RuntimeBody to Python: that handle is a packed,
		// process-local b3BodyId. It is not meaningful outside this process, and
		// it goes STALE the moment OnPhysics3DStop destroys the world -- a script
		// that captured it and used it after teardown would be dereferencing a
		// destroyed world, which is a use-after-free rather than an error the
		// script could reasonably be expected to handle.
		//
		// So the resolution happens HERE, against the live world, and the script
		// only ever sees a bool. Three guards, all required, none of them
		// redundant:
		//
		//   1. b3World_IsValid -- no world means no body, ever. Every other 3D
		//      call site in this file opens with the same test (see
		//      SyncPendingPhysicsBodies, CreatePhysicsBody3D, Raycast).
		//   2. the component must be present -- an entity with no
		//      RigidBody3DComponent is not a physical thing. TryGetComponent, not
		//      GetComponent: the latter asserts in Debug and is undefined
		//      behaviour in Release, and "this entity has no body" is a normal
		//      state a script must be able to ask about, not a bug.
		//   3. RuntimeBody != 0 -- zero is the documented 'not created yet'
		//      sentinel (see RigidBody3DComponent). A body added since the last
		//      step still has one until SyncPendingPhysicsBodies runs.
		//
		// b3Body_IsValid is the backstop for the case none of the above can see:
		// a handle that was valid when the body was created and whose world has
		// since been rebuilt. b3LoadBodyId on a stale id and asking Box3D whether
		// it is a body is cheaper than reading through it.
		//
		// NOTE: b3_nullBodyId, not B3_NULL_ID. The macro is a BRACED INITIALIZER
		// (`{}` / `{ 0 }`), so `bodyId == B3_NULL_ID` expands to `bodyId == {0}`
		// and fails with error C2059 AT THE COMPARISON, naming neither the macro
		// nor this file's intent. Box3D ships b3_nullBodyId for anything needing
		// a real null value to return or compare against. The three callers below
		// use b3Body_IsValid rather than comparing ids at all, which sidesteps
		// the question entirely.
		b3BodyId Scene::ResolveBody3D(Entity entity) const
		{
		if (!b3World_IsValid(m_Physics3DWorld))
			return b3_nullBodyId;

		RigidBody3DComponent *rb3d = entity.TryGetComponent<RigidBody3DComponent>();
		if (!rb3d)
			return b3_nullBodyId;

		if (rb3d->RuntimeBody == 0)
			return b3_nullBodyId;

		b3BodyId bodyId = b3LoadBodyId(rb3d->RuntimeBody);
		if (!b3Body_IsValid(bodyId))
			return b3_nullBodyId;

		return bodyId;
		}

		// Applies a one-shot world-space impulse at the body's center of mass.
		//
		// Center of mass rather than a world point on purpose: an off-center
		// impulse would inject angular velocity as a side effect, so a "jump"
		// would also make the player spin. b3Body_ApplyLinearImpulseToCenter is
		// the API that changes velocity only.
		//
		// `wake` is a parameter rather than hardcoded true because a body whose
		// solver is asleep IGNORES the impulse -- and Box3D reports nothing when
		// that happens. A sleeping body given an impulse silently does nothing,
		// with no return value to inspect and no log line, which is why the
		// failure is invisible unless the caller thinks to ask for wake=true.
		// The parameter exists so a caller that deliberately wants to push a
		// sleeping body WITHOUT waking it (accumulating it for later) can say so.
		//
		// The impulse is in N*s (kg*m/s), matching the world's gravity scale of
		// 9.81, so a ~25kg character needs roughly (25 * 6.5) to clear a 2m jump.
		//
		// @return true when the impulse reached a real body. False -- never an
		//         exception -- when there is no body, no component, or no world.
		bool Scene::ApplyImpulse(Entity entity, const glm::vec3 &impulse, bool wake)
		{
		b3BodyId bodyId = ResolveBody3D(entity);
		if (!b3Body_IsValid(bodyId))
			return false;

		b3Body_ApplyLinearImpulseToCenter(bodyId, {(float)impulse.x, (float)impulse.y, (float)impulse.z}, wake);
		return true;
		}

		// Overwrites the body's linear velocity.
		//
		// An overwrite, not an addition, and that is the whole point: this is how
		// a script stops a falling body, corrects a bad knockback, or pins a
		// hover body to an exact speed. It also wakes the body, because setting
		// velocity on a sleeping body and leaving it asleep produces a body that
		// reads the right velocity and then refuses to act on it.
		bool Scene::SetLinearVelocity(Entity entity, const glm::vec3 &velocity)
		{
		b3BodyId bodyId = ResolveBody3D(entity);
		if (!b3Body_IsValid(bodyId))
			return false;

		b3Body_SetLinearVelocity(bodyId, {(float)velocity.x, (float)velocity.y, (float)velocity.z});
		b3Body_SetAwake(bodyId, true);
		return true;
		}

		// Reads the body's current linear velocity into `out`.
		//
		// `out` is left untouched on failure, so a caller polling every frame can
		// keep the last known-good value and test the bool for "was this a real
		// reading". Zeroing it instead would be indistinguishable from a body
		// genuinely at rest.
		bool Scene::GetLinearVelocity(Entity entity, glm::vec3 &out) const
		{
		b3BodyId bodyId = ResolveBody3D(entity);
		if (!b3Body_IsValid(bodyId))
			return false;

		b3Vec3 v = b3Body_GetLinearVelocity(bodyId);
		out = glm::vec3(v.x, v.y, v.z);
		return true;
		}

		void Scene::OnPhysics3DStop()
		{
		m_TerrainRuntime.SetPhysicsWorld(B3_NULL_ID);
		if (b3World_IsValid(m_Physics3DWorld))
		{
			b3DestroyWorld(m_Physics3DWorld);
			m_Physics3DWorld = B3_NULL_ID;
		}
	}

	// Submits every fog and cloud volume in the scene to the renderer.
	//
	// Two separate passes rather than one loop over both component types,
	// because they are submitted through different renderer entry points and
	// land in different lists (FogVolumes and CloudVolumes).
	//
	// NOTE the Enabled flag is NOT tested here. Both components have one, and
	// it is honoured -- but inside Renderer3D::SubmitFogVolume /
	// SubmitCloudVolume, which return early when it is false. The cost is a
	// wasted call per disabled volume per frame; the behaviour is correct.
	// Worth knowing before adding a check here, since it would be redundant.
	//
	// The renderer also caps each list (MaxFogVolumes / MaxCloudVolumes) and
	// returns silently once full, with no log. So past that limit extra
	// volumes vanish with no diagnostic -- the same first-N-wins pattern as
	// the analytic lights below, and worth watching if a fog volume
	// "sometimes does nothing".
	//
	// Entity order is entt storage order, not meaningful, so which volumes
	// survive the cap is not something an author controls.
	void Scene::SubmitVolumetricVolumes()
	{
		auto fogView = m_Registry.view<TransformComponent, VolumetricFogComponent>();
		for (auto entity : fogView)
		{
			auto &tc = fogView.get<TransformComponent>(entity);
			auto &fog = fogView.get<VolumetricFogComponent>(entity);
			Renderer3D::SubmitFogVolume(tc.GetTransform(), fog);
		}

		auto cloudView = m_Registry.view<TransformComponent, VolumetricCloudsComponent>();
		for (auto entity : cloudView)
		{
			auto &tc = cloudView.get<TransformComponent>(entity);
			auto &clouds = cloudView.get<VolumetricCloudsComponent>(entity);
			Renderer3D::SubmitCloudVolume(tc.GetTransform(), clouds);
		}
	}

	// Submits every analytic light in the scene to the renderer.
	//
	// Four passes, one per light type, for the same reason as the volumes --
	// each has its own renderer entry point. An entity with two different
	// light components contributes to both passes, and there is no check
	// preventing that.
	//
	// Each pass submits tc.GetTransform(), so the light's POSITION and
	// AIM both come from the transform: local -Z is the direction light
	// travels, matching the convention documented on each light component.
	// GetTransform is cached per entity, so an entity contributing to several
	// passes only recomputes its matrix once.
	//
	// NO ENABLED FLAG on any light component -- unlike fog and clouds, there
	// is no way to switch a light off without removing the component, so
	// disabling one in the editor means deleting it.
	//
	// Renderer3D caps the total at MaxAnalyticLights (8, fixed by the
	// shader's fixed-size light arrays) and is PRIORITY-based, not
	// submission-order: a full list gives its slot to a light type
	// that has none, evicting the least important light from a type
	// that has surplus, with camera distance breaking ties. That quota
	// exists because of the type ordering below -- because each type is
	// submitted in its own pass, a purely first-come list let a scene
	// full of point lights starve every spot and area light in the scene.
	// Anything that still does not fit is counted and reported once per
	// frame by Renderer3D, so the drop is visible in the log rather than
	// silent. Submission order within a type is entt storage order and is
	// therefore not authorable; that is why the distance tiebreak lives in
	// Renderer3D, where the camera position is already known.
	void Scene::SubmitSceneLights()
	{
		auto dirView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
		for (auto entity : dirView)
		{
			auto &tc = dirView.get<TransformComponent>(entity);
			auto &light = dirView.get<DirectionalLightComponent>(entity);
			Renderer3D::SubmitDirectionalLight(tc.GetTransform(), light);
		}

		auto pointView = m_Registry.view<TransformComponent, PointLightComponent>();
		for (auto entity : pointView)
		{
			auto &tc = pointView.get<TransformComponent>(entity);
			auto &light = pointView.get<PointLightComponent>(entity);
			Renderer3D::SubmitPointLight(tc.GetTransform(), light);
		}

		auto spotView = m_Registry.view<TransformComponent, SpotLightComponent>();
		for (auto entity : spotView)
		{
			auto &tc = spotView.get<TransformComponent>(entity);
			auto &light = spotView.get<SpotLightComponent>(entity);
			Renderer3D::SubmitSpotLight(tc.GetTransform(), light);
		}

		auto areaView = m_Registry.view<TransformComponent, AreaLightComponent>();
		for (auto entity : areaView)
		{
			auto &tc = areaView.get<TransformComponent>(entity);
			auto &light = areaView.get<AreaLightComponent>(entity);
			Renderer3D::SubmitAreaLight(tc.GetTransform(), light);
		}
	}


	// Advances every water body's simulation clock.
	//
	// Only the clock. The surface itself is evaluated on demand by whoever
	// samples it -- the mesh, and gameplay queries via GetWaterHeightAt -- so
	// there is no per-vertex or per-fragment state to update here. That makes
	// this proportional to the number of water BODIES, not to their surface
	// area, so it is cheap regardless of how much water is on screen.
	//
	// TimeScale is applied HERE, so it slows the clock and therefore every
	// consumer of it. WaveSpeed is applied separately inside EvaluateSurface,
	// so the two multiply: a preset's WaveSpeed and the user's TimeScale both
	// affect the final rate.
	//
	// @param ts  Frame timestep. A non-positive delta returns immediately.
	void Scene::UpdateWater(Timestep ts)
	{
		float dt = ts.GetSeconds();
		if (dt <= 0.0f)
			return;
		auto view = m_Registry.view<WaterComponent>();
		for (auto entity : view)
		{
			auto &water = view.get<WaterComponent>(entity);
			if (!water.Enabled)
				continue;
			water.Time += dt * water.TimeScale;
			// Guard against float precision decay on very long sessions
			// Wraps the accumulator after a day of simulated time, to stop float
			// precision decaying on very long sessions.
			//
			// A float loses mantissa bits as it grows, so an ever-increasing clock
			// eventually stops advancing waves smoothly -- the phase term
			// (k * dot(dir, p) - omega * t) grows without bound and the fractional
			// part becomes noise. Capping it is the standard fix.
			//
			// THE WRAP IS NOT PHASE-ALIGNED, so hitting it is a visible snap rather
			// than an invisible correction: subtracting 86400 from t only preserves
			// the wave phase if omega * 86400 is a whole multiple of 2*pi, which it
			// is not for any of the six spectrum components (they have different,
			// irrational-ish omegas). Every component therefore jumps to a different
			// phase offset at the wrap, so the whole surface discontinuously changes
			// shape.
			//
			// In practice this needs 24 hours of simulated time -- at TimeScale 1
			// that is a day of wall-clock play -- so it is a genuine long-session guard
			// rather than a visible bug. But it is worth knowing that if it ever DOES
			// fire, the water will visibly jump, and the jump cannot be fixed by
			// choosing a better wrap constant. Wrapping per-body also means bodies wrap
			// at different moments, so their phases permanently desynchronise.
			//
			// Note the comparison is on the already-scaled accumulator, so TimeScale
			// above 1 reaches the cap proportionally faster.
			if (water.Time > 86400.0f)
				water.Time = fmodf(water.Time, 86400.0f);
		}
	}


	// Submits every procedural mesh primitive in the scene.
	//
	// One pass per primitive shape. Each view REQUIRES a MaterialComponent
	// alongside the transform and the shape component, which is the contract:
	// an entity with a primitive but no material has nothing to shade and is
	// skipped entirely rather than drawn untextured. So adding a shape without
	// a material produces an invisible object, not a default-material one --
	// the same rule the cube and sphere paths use.
	//
	// Kept as ONE function called from both the runtime and editor paths,
	// deliberately: these used to be separate Cube and Sphere blocks in each,
	// and they drifted, which is how a shape ends up visible while playing and
	// missing in the editor viewport. Adding a primitive here gets both paths
	// at once.
	//
	// Each call passes (int)entity -- the entt handle cast to int -- as the
	// per-instance identity the renderer keys on. Assuming the handle fits in
	// an int, which entt's default 32-bit entity type does.
	void Scene::SubmitMeshPrimitives()
	{
		// One view per primitive, each requiring MaterialComponent alongside it
		// (same contract as cube/sphere: an entity with a primitive but no
		// material has nothing to shade and is skipped).
		//
		// Kept as one function instead of duplicated into both render paths
		// (runtime and editor viewport). Those used to carry separate Cube and
		// Sphere blocks, and drift between them is how a shape ends up visible
		// in one path and missing from the other.
		// A multi-type view: entt iterates only entities carrying ALL THREE
		// components, so the material requirement is enforced by the view itself
		// rather than by an if inside the loop.
		//
		// view.get<T>(entity) is then a direct reference with no lookup failure --
		// entt guarantees the components exist for every entity the multi-view
		// yields. That is why these loops have no validity checks, unlike the
		// CopyComponent loops above which work across two registries and so must
		// check.
		//
		// The three gets are separate calls returning separate references; this is
		// the idiomatic entt form and costs one lookup each.
		auto planeView = m_Registry.view<TransformComponent, PlaneRendererComponent, MaterialComponent>();
		for (auto entity : planeView)
		{
			auto &tc = planeView.get<TransformComponent>(entity);
			auto &prc = planeView.get<PlaneRendererComponent>(entity);
			auto &mc = planeView.get<MaterialComponent>(entity);
			Renderer3D::DrawPlane(tc.GetTransform(), prc.Color, prc.Subdivisions, mc, (int)entity);
		}

		auto gridView = m_Registry.view<TransformComponent, GridRendererComponent, MaterialComponent>();
		for (auto entity : gridView)
		{
			auto &tc = gridView.get<TransformComponent>(entity);
			auto &grc = gridView.get<GridRendererComponent>(entity);
			auto &mc = gridView.get<MaterialComponent>(entity);
			Renderer3D::DrawGrid(tc.GetTransform(), grc.Color, grc.XSubdivisions, grc.YSubdivisions, mc, (int)entity);
		}

		auto circleView = m_Registry.view<TransformComponent, Circle3DRendererComponent, MaterialComponent>();
		for (auto entity : circleView)
		{
			auto &tc = circleView.get<TransformComponent>(entity);
			auto &crc = circleView.get<Circle3DRendererComponent>(entity);
			auto &mc = circleView.get<MaterialComponent>(entity);
			Renderer3D::DrawCircle(tc.GetTransform(), crc.Color, crc.Radius, crc.Segments, mc, (int)entity);
		}

		auto icoView = m_Registry.view<TransformComponent, IcoSphereRendererComponent, MaterialComponent>();
		for (auto entity : icoView)
		{
			auto &tc = icoView.get<TransformComponent>(entity);
			auto &isc = icoView.get<IcoSphereRendererComponent>(entity);
			auto &mc = icoView.get<MaterialComponent>(entity);
			Renderer3D::DrawIcoSphere(tc.GetTransform(), isc.Color, isc.Radius, isc.Subdivisions, mc, (int)entity);
		}

		auto cylView = m_Registry.view<TransformComponent, CylinderRendererComponent, MaterialComponent>();
		for (auto entity : cylView)
		{
			auto &tc = cylView.get<TransformComponent>(entity);
			auto &cyc = cylView.get<CylinderRendererComponent>(entity);
			auto &mc = cylView.get<MaterialComponent>(entity);
			Renderer3D::DrawCylinder(tc.GetTransform(), cyc.Color, cyc.Radius, cyc.Height, cyc.Segments, mc, (int)entity);
		}

		auto coneView = m_Registry.view<TransformComponent, ConeRendererComponent, MaterialComponent>();
		for (auto entity : coneView)
		{
			auto &tc = coneView.get<TransformComponent>(entity);
			auto &coc = coneView.get<ConeRendererComponent>(entity);
			auto &mc = coneView.get<MaterialComponent>(entity);
			Renderer3D::DrawCone(tc.GetTransform(), coc.Color, coc.Radius, coc.Height, coc.Segments, mc, (int)entity);
		}

		auto torusView = m_Registry.view<TransformComponent, TorusRendererComponent, MaterialComponent>();
		for (auto entity : torusView)
		{
			auto &tc = torusView.get<TransformComponent>(entity);
			auto &trc = torusView.get<TorusRendererComponent>(entity);
			auto &mc = torusView.get<MaterialComponent>(entity);
			Renderer3D::DrawTorus(tc.GetTransform(), trc.Color, trc.MajorRadius, trc.MinorRadius,
							  trc.MajorSegments, trc.MinorSegments, mc, (int)entity);
		}
	}

	void Scene::SubmitTerrainChunks()
	{
		for (const auto& chunk : m_TerrainRuntime.GetStreamer().GetLoadedChunks())
			Renderer3D::DrawTerrainChunk(chunk.second);
	}

	void Scene::SubmitWaterSurfaces()
	{
		auto view = m_Registry.view<TransformComponent, WaterComponent>();
		for (auto entity : view)
		{
			auto &tc = view.get<TransformComponent>(entity);
			auto &water = view.get<WaterComponent>(entity);
			Renderer3D::DrawWaterPlane(tc.GetTransform(), water, (int)entity);
		}
	}


	bool InventoryComponent::IsFull() const
	{
		// Slot limit is checkable from the component alone. Weight is not,
		// because per-item weight lives on another entity's definition, so
		// IsInventoryFull on Scene is the one that answers completely.
		if (MaxSlots > 0 && (int)Items.size() >= MaxSlots)
			return true;
		return false;
	}

	// Looks up an item definition by its Name, across the whole scene.
	//
	// Returns a POINTER INTO THE REGISTRY, not a copy. That is safe while the
	// registry is not mutating -- entt keeps component storage stable across
	// unrelated insertions -- but the pointer is invalidated by any structural
	// change or by the scene being destroyed. A caller must not hold it across
	// an AddComponent/DestroyEntity.
	//
	// Returns the FIRST match. Nothing enforces that item names are unique, so
	// two entities defining the same name resolve to whichever entt happens to
	// yield first -- which is storage order, not creation order, and can change
	// as the scene is edited. Duplicate names are therefore a silent
	// wrong-answer bug rather than a reported error.
	//
	// The empty-string early-out is a guard against finding a definition whose
	// Name is itself empty, which would otherwise let a caller looking up ""
	// match an unnamed item.
	//
	// @param itemId  The definition Name to find.
	// @returns       A borrowed pointer, or nullptr when no entity defines that
	// name.
	ItemDefinitionComponent *Scene::FindItemDefinition(const std::string &itemId)
	{
		if (itemId.empty())
			return nullptr;

		auto view = m_Registry.view<ItemDefinitionComponent>();
		for (auto e : view)
		{
			auto &def = view.get<ItemDefinitionComponent>(e);
			if (def.Name == itemId)
				return &def;
		}
		return nullptr;
	}

	// Total quantity of an item held, summed across every matching stack.
	//
	// SUMS rather than returning the first stack's Quantity, so an entity may
	// legitimately hold several stacks of the same itemId and this reports the
	// sum. That is the useful behaviour for inventory queries, but it means
	// "does the entity have a stack of this" cannot be answered from this
	// function -- a caller needing the stacks themselves must read
	// InventoryComponent::Items directly.
	//
	// TryGetComponent rather than GetComponent, so an entity with no inventory
	// reads as empty instead of tripping an assert. Same safe-access pattern as
	// MoveCapsule's null-checked out-params: the query answers rather than
	// faulting.
	//
	// @param entity  Entity to inspect.
	// @param itemId  Item to total.
	// @returns       Combined quantity, or 0 when the entity has no inventory.
	int Scene::GetItemCount(Entity entity, const std::string &itemId) const
	{
		auto *inv = entity.TryGetComponent<InventoryComponent>();
		if (!inv)
			return 0;

		int total = 0;
		for (const auto &stack : inv->Items)
			if (stack.ItemId == itemId)
				total += stack.Quantity;
		return total;
	}

	// Whether the entity holds at least `quantity` of an item.
	//
	// A thin wrapper over GetItemCount, which it calls by name -- so the two
	// can never disagree about what "has" means.
	//
	// EDGE CASES WORTH KNOWING: the comparison is >=, so a quantity of 0 or
	// less returns TRUE for any entity, including one holding none of the item
	// (GetItemCount returns 0, and 0 >= 0). Callers meaning "has any" must pass
	// 1 explicitly, or use GetItemCount themselves.
	//
	// @param entity    Entity to inspect.
	// @param itemId    Item to check for.
	// @param quantity  Minimum count. Values <= 0 always pass.
	// @returns         Whether the count reaches the threshold.
	bool Scene::HasItem(Entity entity, const std::string &itemId, int quantity) const
	{
		return GetItemCount(entity, itemId) >= quantity;
	}

	float Scene::GetInventoryWeight(Entity entity) const
	{
		auto *inv = entity.TryGetComponent<InventoryComponent>();
		if (!inv)
			return 0.0f;

		float total = 0.0f;
		for (const auto &stack : inv->Items)
		{
			// An undefined item weighs nothing rather than crashing: a
			// scene that is mid-load can legitimately reference an item whose
			// definition has not streamed in yet.
			ItemDefinitionComponent *def = const_cast<Scene *>(this)->FindItemDefinition(stack.ItemId);
			if (def)
				total += def->Weight * (float)stack.Quantity;
		}
		return total;
	}

	bool Scene::IsInventoryFull(Entity entity) const
	{
		auto *inv = entity.TryGetComponent<InventoryComponent>();
		if (!inv)
			return false;

		if (inv->MaxSlots > 0 && (int)inv->Items.size() >= inv->MaxSlots)
			return true;

		if (inv->MaxWeight > 0.0f && GetInventoryWeight(entity) >= inv->MaxWeight)
			return true;

		return false;
	}

	// Weight arithmetic tolerance. Authored weights are floats, so summing
	// them accumulates error: five 0.1 items total 0.5000000014901161, which
	// leaves 0.4999999985 of headroom and silently costs the player an item
	// they can plainly afford. Sized to absorb float error at typical
	// inventory magnitudes without ever letting a real over-capacity load
	// through (a 0.5 item against 0 available still fails by miles).
	static constexpr float kWeightEpsilon = 1e-4f;

	// Notify the script attached to an inventory owner after its count changed.
	// This runs from both Python bindings and native gameplay, so acquire the GIL
	// explicitly and keep a missing hook a quiet no-op like CallScriptMethod.
	static void NotifyInventoryChanged(Entity entity, const std::string &itemId,
									   int oldCount, int newCount)
	{
		if (!entity.IsValid() || !entity.HasComponent<IDComponent>() || !Py_IsInitialized())
			return;

		py::gil_scoped_acquire gil;
		const UUID id = entity.GetComponent<IDComponent>().ID;
		py::object *instance = ScriptEngine::GetInstance(id);
		if (!instance || !instance->ptr())
			return;

		try
		{
			py::object callback = py::getattr(*instance, "OnInventoryChanged", py::none());
			if (callback.is_none() || !PyCallable_Check(callback.ptr()))
				return;
			callback(itemId, oldCount, newCount);
		}
		catch (const py::error_already_set &e)
		{
			WL_CORE_ERROR("Script for entity {0} raised during OnInventoryChanged: {1}", id, e.what());
			PyErr_Clear();
		}
		catch (const std::exception &e)
		{
			WL_CORE_ERROR("Script binding error for entity {0} during OnInventoryChanged: {1}", id, e.what());
			PyErr_Clear();
		}
		catch (...)
		{
			WL_CORE_ERROR("Unknown error for entity {0} during OnInventoryChanged", id);
			PyErr_Clear();
		}
	}

	// Adds items to an inventory, topping up existing stacks before opening new
	// ones, and returns how many were actually accepted.
	//
	// RETURNS THE COUNT ACCEPTED, NOT THE COUNT REQUESTED. Capacity, stack
	// limits and weight can each make it accept less -- or none. A caller that
	// assumes the full quantity arrived will desync its own bookkeeping, so
	// the return value has to be checked whenever the addition might not be
	// fully satisfiable.
	//
	// Everything is pointer-safe: an empty id, a non-positive quantity, or an
	// entity with no InventoryComponent all return 0 rather than asserting.
	// That means "nothing happened" is reported identically for "no inventory"
	// and "inventory full", so a caller wanting to tell those apart must check
	// the entity itself.
	//
	// @param entity    Target entity.
	// @param itemId    Item to add.
	// @param quantity  How many to add. Values <= 0 are a no-op.
	// @returns         How many were actually added.
	int Scene::AddItem(Entity entity, const std::string &itemId, int quantity)
	{
		if (itemId.empty() || quantity <= 0)
			return 0;

		auto *inv = entity.TryGetComponent<InventoryComponent>();
		if (!inv)
			return 0;

		const int oldCount = GetItemCount(entity, itemId);

		ItemDefinitionComponent *def = FindItemDefinition(itemId);
		// An unknown item gets a single-item stack. It is better to carry a
		// misspelled id visibly in the inventory than to refuse the pickup
		// silently; MaxStack of 1 keeps the damage contained.
		// Stack limit and unit weight, both from the item definition when one
		// exists.
		//
		// An UNKNOWN item id gets maxStack 1 and weight 0.0. MaxStack 1 keeps a
		// typo contained to one row per occurrence instead of a single unbounded
		// pile -- visible in the inventory so the misspelling can be found, which
		// the comment says is deliberate.
		//
		// The weight default of 0.0 is the more consequential half, and it is NOT
		// neutral: see the weight guard below, which skips the capacity check
		// entirely for zero-weight items. So every unknown item id is also
		// weightless, and an inventory full of them is unbounded.
		//
		// MaxStack 0 is the codebase's convention for "unlimited" -- see the pass-1
		// loop below, where treating it as unlimited is a bug that was already fixed
		// once.
		const int maxStack = def ? def->MaxStack : 1;
		const float unitWeight = def ? def->Weight : 0.0f;

		int added = 0;
		int remaining = quantity;

		// Pass 1: top up existing partial stacks. Doing this before creating
		// new stacks is what stops "1 ammo" from occupying a second slot when
		// an almost-full stack of the same item is already carried.
		//
		// The `maxStack <= 0` case must NOT be skipped. An unlimited-stack
		// item (MaxStack 0) hits that skip and every subsequent add opened a
		// NEW stack: 500 keys became "1 key" plus "500 keys" in two rows.
		// Unlimited means exactly one stack grows without bound.
		for (auto &stack : inv->Items)
		{
			if (remaining <= 0)
				break;
			if (stack.ItemId != itemId)
				continue;
			if (maxStack > 0 && stack.Quantity >= maxStack)
				continue;

			int take = remaining;
			if (maxStack > 0)
			{
				int room = maxStack - stack.Quantity;
				if (room < take)
					take = room;
			}

			// Weight check per stack, so a partial add respects capacity
			// rather than being refused wholesale.
			//
			// The epsilon matters. Per-item weights are authored floats, so
			// five 0.1 items accumulate to 0.5000000014901161, leaving
			// 0.4999999985 "available" -- just under the 0.5 needed for one
			// more. Without the tolerance the player is one float rounding
			// error short of carrying a whole number of items, which reads
			// as an off-by-one bug in the inventory rather than precision.
			// Weight capacity check, applied per stack so a partial add is possible.
			//
			// PERFORM: GetInventoryWeight(entity) below sums the WHOLE inventory, and
			// it is called once per stack visited. The pass-1 loop therefore makes the
			// add O(n^2) in the number of stacks, recomputing the total from scratch
			// each time. With a slot-limited inventory that is trivial; an
			// unlimited-stack item with many rows makes it the dominant cost of a
			// pickup. Caching the running total and updating it as stacks fill would
			// make the whole operation linear.
			//
			// ZERO-WEIGHT ITEMS BYPASS CAPACITY ENTIRELY. The `unitWeight > 0.0f`
			// conjunct means a zero-weight item is never weight-checked, so it can be
			// added without limit regardless of MaxWeight. That is the right behaviour
			// for intended weightless items (a key, a quest token), but it also applies
			// to every UNKNOWN item id, since those default to weight 0.0 -- so an
			// item id that does not resolve is both unstackable-limited to 1 and
			// unlimited in total count.
			//
			// MaxWeight <= 0 disables the weight system for the whole inventory, which
			// is the same convention the stack system uses for MaxSlots.
			//
			// The + kWeightEpsilon before dividing is a genuine fix, not cargo cult: the
			// comment above it explains that five 0.1-weight items accumulate to
			// 0.5000000014901161, which would leave the player a rounding error short of
			// carrying a whole number of items. Truncating to int then loses the last
			// item entirely, which reads as an off-by-one rather than as precision.
			if (inv->MaxWeight > 0.0f && unitWeight > 0.0f)
			{
				float available = inv->MaxWeight - GetInventoryWeight(entity);
				int fits = (int)((available + kWeightEpsilon) / unitWeight);
				if (fits <= 0)
					return added;
				if (take > fits)
					take = fits;
			}

			stack.Quantity += take;
			added += take;
			remaining -= take;
		}

		// Pass 2: open new stacks.
		while (remaining > 0)
		{
			if (inv->MaxSlots > 0 && (int)inv->Items.size() >= inv->MaxSlots)
				break;

			int take = remaining;
			if (maxStack > 0 && take > maxStack)
				take = maxStack;

			if (inv->MaxWeight > 0.0f && unitWeight > 0.0f)
			{
				float available = inv->MaxWeight - GetInventoryWeight(entity);
				// Same tolerance as pass 1; see kWeightEpsilon.
				int fits = (int)((available + kWeightEpsilon) / unitWeight);
				if (fits <= 0)
					break;
				if (take > fits)
					take = fits;
			}

			inv->Items.emplace_back(itemId, take);
			added += take;
			remaining -= take;
		}

		if (added == quantity)
			NotifyInventoryChanged(entity, itemId, oldCount, GetItemCount(entity, itemId));

		return added;
	}

	int Scene::RemoveItem(Entity entity, const std::string &itemId, int quantity)
	{
		if (itemId.empty() || quantity <= 0)
			return 0;

		auto *inv = entity.TryGetComponent<InventoryComponent>();
		if (!inv)
			return 0;

		const int oldCount = GetItemCount(entity, itemId);

		int removed = 0;
		int remaining = quantity;

		// Consume from the LAST stack first. Newest pickups land in new
		// stacks, so this is LIFO, which matches player expectation: drop the
		// pack you just picked up, not the last one you found.
		for (auto it = inv->Items.end(); it != inv->Items.begin() && remaining > 0;)
		{
			--it;
			if (it->ItemId != itemId)
				continue;

			int take = it->Quantity < remaining ? it->Quantity : remaining;
			it->Quantity -= take;
			remaining -= take;
			removed += take;

			// Drop emptied stacks rather than leaving zero-quantity rows.
			// A row with Quantity 0 still consumes a slot, so keeping it
			// silently eats the player's capacity.
			if (it->Quantity <= 0)
				it = inv->Items.erase(it);
		}

		if (removed > 0)
			NotifyInventoryChanged(entity, itemId, oldCount, GetItemCount(entity, itemId));

		return removed;
	}

	int Scene::RemoveItemFromInventory(Entity entity, const std::string &itemId, int quantity)
	{
		// Renamed from DropItem, which promised a spawn it never performed: the
		// entire body was `return RemoveItem(...)`. The comment that used to
		// sit here argued that spawning "is the caller's job", which is a
		// fair statement about PREFABS and a non-sequitur about the API -- a
		// caller reading the name had no way to know it was responsible for
		// creating a world entity, and this was bound to Python, so a script
		// could call it from anywhere and silently destroy its own items.
		//
		// The behaviour is unchanged on purpose: renaming a lie is a source
		// change, not a semantics change, and this is exactly what every
		// existing call site was already getting. DropItemAt below is the real
		// drop.
		return RemoveItem(entity, itemId, quantity);
	}

	Entity Scene::DropItemAt(Entity entity, const std::string &itemId, int quantity,
						  const glm::vec3 &worldOffset)
	{
		// Every rejection below returns an INVALID Entity, never a default-
		// constructed pile with zeroed fields. Entity() is a genuinely
		// supported "nothing" value (see Entity.h), and IsValid() is the only
		// way a caller can tell it from a real handle -- so the contract is
		// written in terms of that rather than a magic entity.
		if (itemId.empty() || quantity <= 0)
			return Entity();

		// TryGetComponent, not GetComponent: an entity with no inventory is a
		// NORMAL outcome for a drop ("that is not a thing that carries
		// things"), and it must answer rather than assert. Same pattern as
		// GetItemCount and IsInventoryFull below.
		if (!entity.TryGetComponent<InventoryComponent>())
			return Entity();

		// Remove FIRST, then spawn.
		//
		// The order is not interchangeable. Spawning first would put a pile in
		// the world for an item id the carrier never held, and any failure
		// between the two steps would then duplicate or destroy items. Removing
		// first makes the worst case a refusal: nothing moved, nothing spawned.
		// A drop never creates items.
		int dropped = RemoveItem(entity, itemId, quantity);
		if (dropped <= 0)
			return Entity();

		// Spawn position: the carrier's world translation plus the caller's
		// offset. Translation rather than the composed matrix because
		// TransformComponent::Translation is documented as WORLD space -- and
		// the cached matrix would re-apply the entity's rotation and scale to
		// the offset, which is an absolute world delta and must not be
		// transformed by them.
		//
		// TryGetComponent rather than GetComponent: an entity created outside
		// CreateEntity, or one whose transform was removed, has no
		// TransformComponent -- and the drop must still work. Spawning at the
		// bare offset beats an assert.
		glm::vec3 position = worldOffset;
		if (auto *tc = entity.TryGetComponent<TransformComponent>())
			position += tc->Translation;

		// The pile's name carries the item id, so a dropped entity is
		// identifiable in GetAllEntitiesWithTransform() and in a debugger.
		// "Dropped_" + id with no disambiguator: two piles of the same item
		// are legitimately named the same, and TagComponent is not unique.
		Entity pile = CreateEntity("Dropped_" + itemId);

		// Direct field write, in place, because CreateEntity already added a
		// TransformComponent. GetComponent<TransformComponent>() would ASSERT
		// on a duplicate via AddComponent; and SetTransform is the wrong tool --
		// it takes a matrix and runs glm::decompose, which would DISCARD the
		// offset's exact values and leave Rotation/Scale at whatever decompose
		// produced. Assigning Translation is exact, and it leaves the transform
		// cache alone: GetTransform() compares TRS against its cached key and
		// rebuilds on the next call.
		pile.GetComponent<TransformComponent>().Translation = position;

		auto &pile_component = pile.AddComponent<DroppedItemComponent>();
		pile_component.ItemId = itemId;
		// The count ACTUALLY removed, never the requested one. A drop of 100
		// against a stack of 7 spawns 7, and a pile claiming 100 would conjure
		// ninety-three items out of nothing on the next pickup.
		pile_component.Quantity = dropped;

		return pile;
	}

	int Scene::PickUpDroppedItem(Entity dropped, Entity carrier)
	{
		auto *pile = dropped.TryGetComponent<DroppedItemComponent>();
		if (!pile || !pile->IsValid())
			return 0;

		// No capacity check of our own: AddItem already enforces MaxSlots and
		// MaxWeight and reports how many it accepted. Duplicating that logic
		// here would be a second implementation to keep in sync, and the
		// partial-accept path is the interesting one anyway.
		int taken = AddItem(carrier, pile->ItemId, pile->Quantity);
		if (taken <= 0)
			return 0;

		pile->Quantity -= taken;

		// Destroy only on a FULL pickup. A partial pickup leaves a smaller pile
		// where it was, which is what the player expects and what stops items
		// evaporating because the destination was nearly full.
		//
		// The <= 0 rather than == 0 test: taken is bounded by pile->Quantity by
		// construction so the subtraction cannot go negative, but a pile left
		// at Quantity 0 would be an IsValid()==false component sitting in the
		// world forever, and "an empty pile" has no meaning worth preserving.
		if (pile->Quantity <= 0)
			DestroyEntity(dropped);

		return taken;
	}

	// Finds the water surface height at a world position, across every water
	// body that covers it.
	//
	// This is the gameplay entry point for buoyancy, wading and underwater
	// tests. It samples the HEIGHTFIELD (SampleHeightAt), which has no
	// horizontal Gerstner displacement -- so it is stable under a moving
	// query but sits up to the chop offset below the drawn surface. See
	// SampleHeightAt in Components.h for why that split is deliberate.
	//
	// PERFORM QUERY, NOT PER FRAME: glm::inverse(tc.GetTransform()) below is
	// a full 4x4 matrix inverse computed for EVERY water body on EVERY call.
	// The forward matrix is cached by TransformComponent; the inverse is not
	// cached anywhere. IsUnderwater calls this, so a script testing one point
	// per entity per frame pays one matrix inverse per water body per entity.
	// That is likely fine for a handful of bodies and a handful of entities,
	// and becomes the bottleneck if either grows -- the inverse could be
	// cached on the component alongside the forward matrix.
	//
	// OUT PARAMS ARE ONLY WRITTEN ON SUCCESS. On failure outHeight and
	// outFlow are left exactly as the caller passed them, so a caller that
	// ignores the return value reads a stale or uninitialised height.
	//
	// @param worldPos  The query point in world space.
	// @param outHeight Receives the surface Y. Untouched if no body covers
	//                 the point.
	// @param outFlow   Optional; receives the winning body's flow vector.
	//                 Null is allowed and skips the write.
	// @returns         Whether any enabled water body covers the point.
	bool Scene::GetWaterHeightAt(const glm::vec3 &worldPos, float &outHeight, glm::vec2 *outFlow)
	{
		bool found = false;
		float bestHeight = -FLT_MAX;
		glm::vec2 bestFlow(0.0f);

		auto view = m_Registry.view<TransformComponent, WaterComponent>();
		for (auto entity : view)
		{
			auto &tc = view.get<TransformComponent>(entity);
			auto &water = view.get<WaterComponent>(entity);
			if (!water.Enabled)
				continue;

			// Footprint test in the water body's local space (unit XZ plane).
			// Full inverse handles rotation/scale; waves are a world-Y offset.
			glm::mat4 inv = glm::inverse(tc.GetTransform());
			// Transforms the query point into the water body's local space, so the
			// footprint test respects rotation and non-uniform scale.
			//
			// The w=1 makes this a full projective transform, so a body rotated about
			// X or Y still tests correctly against a plane.
			//
			// Only local.x and local.z are tested below: the water surface is a plane
			// at local y=0 extended by the wave offset, which is applied in WORLD Y
			// rather than local. So the footprint is a rectangle in local space while
			// the height is world-vertical -- a deliberate asymmetry, correct for a
			// water body that is level with the world rather than tilted with its
			// own transform.
			glm::vec4 local = inv * glm::vec4(worldPos, 1.0f);
			// Footprint test: the +/-0.5 bounds are the unit XZ plane the water body
			// is built from, so any point inside its scaled extent is covered.
			//
			// Bounds inclusive at both ends, so a point exactly on the edge counts.
			// Negligible in practice, but it means two adjacent bodies sharing an
			// edge both report a hit there and the tie is broken by the bestHeight
			// comparison below.
			if (local.x < -0.5f || local.x > 0.5f || local.z < -0.5f || local.z > 0.5f)
				continue;

			float surfaceY = tc.Translation.y + water.SampleHeightAt(worldPos.x, worldPos.z);
			// Overlapping bodies: the HIGHEST surface wins.
			//
			// So with two water bodies stacked at a point, the query returns the upper
			// one's height AND its flow. The results are not blended and neither
			// body is aware it lost -- which is the right behaviour for a query
			// (a consistent single answer) but means a river flowing into a lake
			// reports only the lake's flow wherever the lake sits on top.
			//
			// Ties -- two bodies at exactly the same height -- keep the FIRST one
			// found, and iteration order is entt storage order, so which wins is not
			// something an author controls.
			if (!found || surfaceY > bestHeight)
			{
				bestHeight = surfaceY;
				bestFlow = water.SampleFlow();
				found = true;
			}
		}

		if (found)
		{
			outHeight = bestHeight;
			if (outFlow)
				*outFlow = bestFlow;
		}
		return found;
	}


	// Whether a world position is below a water surface.
	//
	// Thin wrapper over GetWaterHeightAt: false wherever there is no water
	// body at all, which is what makes it safe to call unconditionally in
	// gameplay.
	//
	// MARGIN SHRINKS THE WATER, which is the opposite of what the name
	// suggests. The test is `y < height - margin`, so a POSITIVE margin
	// raises the bar: a point near the surface reports as NOT underwater.
	// Read it as "how far above the surface still counts as dry". A negative
	// margin instead makes the test fire before the surface is reached.
	//
	// Inherits GetWaterHeightAt's per-call matrix inverse cost, and inherits
	// its heightfield-versus-drawn-surface discrepancy.
	//
	// @param worldPos  The point to test.
	// @param margin    Dry band above the surface, in world units.
	// @returns         Whether the point is underwater.
	bool Scene::IsUnderwater(const glm::vec3 &worldPos, float margin)
	{
		float height = 0.0f;
		if (!GetWaterHeightAt(worldPos, height))
			return false;
		return worldPos.y < height - margin;
	}


	// --- Raycast query ---

	// Maps a Box3D shape id back to the entity handle whose collider created
	// it. Collider components stash the stored shape id (b3StoreShapeId) in
	// RuntimeShape, so the reverse mapping is a scan over the three collider
	// views comparing the packed uint64. Shape ids are unique per world and
	// the scene owns exactly one world, so a match is unambiguous.
	//
	// Returns the raw handle rather than an Entity: the caller still needs
	// to know whether it is a real hit, and Entity::IsValid() requires a
	// live scene pointer, so a partially-built Entity would misreport.
	// Maps a packed Box3D shape id back to the entt handle whose collider
	// created it.
	//
	// A REVERSE LOOKUP by linear scan over the three 3D collider views,
	// comparing each RuntimeShape against the packed id. O(entities) per
	// resolved hit, on the raycast path -- so a scene with many colliders and
	// a raycast that resolves several hits pays several full scans.
	//
	// Acceptable because a raycast resolves few hits, but it is the reason
	// raycast-heavy gameplay (hundreds of traces per frame) scales badly with
	// collider count. A hash map from shape id to handle, rebuilt when
	// colliders change, would make this O(1).
	//
	// shapeId == 0 maps to entt::null, which is correct rather than defensive:
	// 0 is the established "no shape" sentinel (see the collider components'
	// RuntimeShape, and why a real packed Box3D id can never be 0).
	//
	// Returns a raw handle, not an Entity -- see the comment above the
	// function for why: Entity::IsValid needs a live scene pointer, which a
	// caller resolving a partial hit may not be able to supply.
	static entt::entity FindHandleByShape(const entt::registry &registry, uint64_t shapeId)
	{
		if (shapeId == 0)
			return entt::null;

		auto boxView = registry.view<BoxCollider3DComponent>();
		for (auto e : boxView)
		{
			if (boxView.get<BoxCollider3DComponent>(e).RuntimeShape == shapeId)
				return e;
		}

		auto sphereView = registry.view<SphereCollider3DComponent>();
		for (auto e : sphereView)
		{
			if (sphereView.get<SphereCollider3DComponent>(e).RuntimeShape == shapeId)
				return e;
		}

		auto capsuleView = registry.view<CapsuleCollider3DComponent>();
		for (auto e : capsuleView)
		{
			if (capsuleView.get<CapsuleCollider3DComponent>(e).RuntimeShape == shapeId)
				return e;
		}

		return entt::null;
	}


	Entity Scene::FindEntityByShape(uint64_t shapeId) const
	{
		entt::entity handle = FindHandleByShape(m_Registry, shapeId);
		if (handle == entt::null)
			return {};
		return Entity{handle, const_cast<Scene *>(this)};
	}


	bool Scene::IsRaycastIgnored(entt::entity handle) const
	{
		if (handle == entt::null || !m_Registry.valid(handle))
			return false;
		// A marker that is present AND enabled means "traces pass through me".
		// Enabled=false is how a script turns the behaviour off, so a marked
		// entity with Enabled=false is a normal, solid, hittable collider.
		if (const auto *marker = m_Registry.try_get<RaycastIgnoreComponent>(handle))
			return marker->Enabled;
		return false;
	}

	// Sweeps a capsule through the world and reports how far it got and what
	// stopped it. The character controller's core primitive.
	//
	// STRUCTURE: sweep with b3World_CastMover, then recover contact data with a
	// separate Raycast, with a depenetration escape hatch in between. Each of
	// those three pieces is commented at its site below.
	//
	// THE RETURN VALUE MEANS "BLOCKED", NOT "MOVED". false covers three quite
	// different situations: the world was invalid, the requested motion was
	// zero-length, and the move completed fully. A caller cannot distinguish
	// them from the bool alone -- it has to read outFraction. In particular a
	// zero-length translation returns false, which reads as "not blocked" and
	// is the sensible answer, but it means "did nothing" and "moved all the
	// way" are indistinguishable.
	//
	// Every out-param is optional and independently null-checked, and all four
	// are given sensible defaults up front -- so a caller can pass only the
	// ones it cares about and still get defined values on every path, including
	// the early returns.
	//
	// @param origin        Capsule centre at the start of the move.
	// @param radius        Capsule radius.
	// @param halfHeight    Half-length of the cylindrical section, so total height
	// is 2*halfHeight + 2*radius.
	// @param translation   Desired displacement for this move.
	// @param outHitEntity  Optional; receives the blocking entity, or a null
	// Entity when nothing could be attributed. See the note
	// at the contact probe for when that happens.
	// @param outNormal     Optional; receives the contact normal. Note it stays
	// ZERO when the probe ray finds nothing, and a zero
	// normal makes any caller-side slide degenerate.
	// @param outFraction   Optional; receives how much of the motion was free, in
	// [0, 1]. Defaults to 1.
	// @param outTranslation Optional; receives the actual displacement to apply.
	// @returns Whether the move was blocked.
	bool Scene::MoveCapsule(const glm::vec3 &origin, float radius, float halfHeight,
							 const glm::vec3 &translation,
							 Entity *outHitEntity, glm::vec3 *outNormal,
							 float *outFraction, glm::vec3 *outTranslation) const
	{
		// Default outputs for the no-op cases below, so a caller always gets
		// sane values even when the move could not be swept.
		if (outHitEntity)
			*outHitEntity = Entity();
		if (outNormal)
			*outNormal = glm::vec3(0.0f);
		if (outFraction)
			*outFraction = 1.0f;
		if (outTranslation)
			*outTranslation = translation;

		// No physics world yet (script ran before OnRuntimeStart), or a
		// degenerate move. Report "moved the whole way" rather than
		// teleporting the player to the origin. b3World_IsValid is the
		// same guard the rest of the physics paths use.
		if (!b3World_IsValid(m_Physics3DWorld))
			return false;
		// Rejects a zero-length motion before it reaches normalize() below.
		//
		// The threshold is on the SQUARED length, so it corresponds to a motion of
		// about 1e-6 world units -- far below anything meaningful, and generous
		// enough to absorb float noise from a caller integrating velocity.
		//
		// Returns false, which the caller reads as "not blocked". At this point
		// outFraction is already 1.0 and outTranslation already equals the
		// requested translation, so the caller still sees a correct no-op.
		//
		// This also makes the glm::normalize(translation) further down safe -- it
		// would otherwise return NaN for a zero vector.
		if (glm::dot(translation, translation) <= 1e-12f)
			return false;

		// A non-positive radius or half-height is a degenerate capsule that
		// Box3D would treat as a point. Clamp to something sweepable.
		if (radius < 0.001f)
			radius = 0.001f;
		if (halfHeight < 0.0f)
			halfHeight = 0.0f;

		// The capsule is defined relative to the origin, with the origin at
		// its center: the hemisphere centers sit halfHeight apart along Y.
		// b3Vec3 is a C struct, so assign fields rather than brace-init.
		b3Capsule capsule;
		capsule.center1.x = 0.0f;
		capsule.center1.y = -(float)halfHeight;
		capsule.center1.z = 0.0f;
		capsule.center2.x = 0.0f;
		capsule.center2.y = (float)halfHeight;
		capsule.center2.z = 0.0f;
		capsule.radius = (float)radius;

		b3Pos start;
		start.x = (float)origin.x;
		start.y = (float)origin.y;
		start.z = (float)origin.z;

		b3Vec3 motion;
		motion.x = (float)translation.x;
		motion.y = (float)translation.y;
		motion.z = (float)translation.z;

		b3QueryFilter filter = b3DefaultQueryFilter();
		// Everything in the world can block the player, including entities
		// marked RaycastIgnore -- that marker is about traces, not about
		// physical presence, and a player must not walk through a wall just
		// because a door is also hittable.
		// Collision filter: every category, colliding with every mask.
		//
		// Deliberate, and the comment explains why: RaycastIgnore marks an entity
		// as transparent to TRACES, not as absent from the world. A door that
		// bullets pass through must still be solid to a character walking into it.
		//
		// The filter is a CONSTRUCTED value, so the category and mask bits below
		// overwrite whatever b3DefaultQueryFilter set -- which is the point.
		filter.categoryBits = 0xffffffffull;
		filter.maskBits = 0xffffffffull;

		// The sweep itself: how far along `motion` the capsule can travel before
		// touching something.
		//
		// BOTH CONTACT CALLBACKS ARE PASSED AS nullptr. That is why the contact
		// normal and blocking entity have to be recovered by a second Raycast at
		// the end of the function -- Box3D does not consider a mover sweep a good
		// source of contact information, so the cast itself yields only the
		// fraction.
		//
		// One structural consequence worth knowing: this is a MOVER cast, so it
		// accounts for the capsule's full shape rather than testing a point. That
		// is what lets a character slide along a wall instead of catching on it,
		// and it is the whole reason this function exists rather than a raycast.
		float fraction = b3World_CastMover(m_Physics3DWorld, start, &capsule, motion, filter, nullptr, nullptr);

		// Box3D reports the fraction of the motion that was free. Clamp it:
		// a sweep that starts already overlapping can report slightly outside
		// [0, 1], and passing that through would overshoot the obstruction.
		if (fraction < 0.0f)
			fraction = 0.0f;
		if (fraction > 1.0f)
			fraction = 1.0f;

		// Contact skin: back the capsule off a blocked move by a small
		// margin so it stops just short of touching rather than flush
		// against the surface.
		//
		// This is not cosmetic. Box3D treats a shape that is already
		// overlapping as having no free distance to report, so a capsule left
		// exactly tangent starts reporting "free" the moment it is even
		// fractionally inside -- and a player walking with a normal-sized
		// per-frame step crosses that boundary in one frame and is then free
		// to walk straight through. Keeping a skin guarantees the next sweep
		// starts outside, so the obstruction keeps being reported.
		const float skin = 0.01f;

		// A character is standing on something rather than embedded in it if a
		// downward ray from just under its center finds a surface within the
		// given reach. Probed from below-center so a character resting exactly
		// on a floor still intersects it rather than starting inside it.
		// Ground probe: is there a surface under this capsule within `reach`?
		//
		// Cast from just BELOW the capsule's bottom edge (halfHeight + radius down)
		// plus a 0.02 nudge further down, then straight down for `reach`. Starting
		// below the capsule rather than at its centre is what lets a character
		// resting exactly on a floor intersect that floor instead of starting
		// inside it -- a ray from the centre would be inside the capsule's own
		// extent and can miss a tangent floor entirely.
		//
		// IMPORTANT INCONSISTENCY: this goes through Raycast, which HONOURS
		// RaycastIgnoreComponent, while the sweep above explicitly does not (see
		// the filter comment). So an entity marked RaycastIgnore is solid to the
		// capsule but invisible to this probe. The consequences are systematic,
		// not edge cases:
		//   * A RaycastIgnore floor is not seen here, so a character standing on one
		//     looks embedded and gets lifted by the depenetration path below.
		//   * A RaycastIgnore wall blocks the move but cannot be attributed by the
		//     contact probe, so the caller gets blocked=true with a ZERO normal.
		// Any caller that slides along outNormal on a RaycastIgnore wall will
		// behave degenerately, because sliding against a zero vector is undefined.
		//
		// `radius` is captured but unused in the body -- the offset uses only
		// halfHeight. The capture is harmless and keeps the lambda's contract
		// self-describing if the probe is ever extended.
		auto hasFloorBelow = [this, radius, halfHeight](const glm::vec3 &pos, float reach) -> bool {
			Entity floorEntity;
			glm::vec3 floorPoint(0.0f);
			glm::vec3 floorNormal(0.0f);
			float floorDistance = 0.0f;
			bool floorHittable = false;
			return Raycast(pos - glm::vec3(0.0f, halfHeight + radius, 0.0f) +
							   glm::vec3(0.0f, 0.02f, 0.0f),
						   glm::vec3(0.0f, -1.0f, 0.0f), reach,
						   &floorEntity, &floorPoint, &floorNormal, &floorDistance,
						   &floorHittable);
		};

		// Depenetration: a mover that is ALREADY overlapping geometry gets no
		// obstruction from the sweep (the shape intersects at t=0, so there is
		// no free distance to report). Without this, a character that spawns
		// straddling the floor sinks through it and never recovers.
		//
		// Only consulted when the sweep said the move was fully free, and only
		// when there is NO floor within the capsule's own reach below. Having a
		// floor underfoot means the character is standing on the world, not
		// stuck inside it -- and the free sweep is then trustworthy.
		//
		// The downward check is what makes this safe. Testing along the motion
		// direction instead fires on ordinary ground: a character standing on
		// the floor has that floor within its extent in every direction
		// including straight down, so the player would be lifted off it.
		// Depenetration gate: the sweep was free AND there is nothing to stand on.
		//
		// Both conditions are required. The sweep being free alone is not enough,
		// because a character standing normally on a floor has that floor inside
		// its own extent in every direction -- including straight down -- so the
		// sweep reports the downward floor as an obstruction and the character
		// would be shoved upward off the ground it is standing on. Checking
		// downward for a real floor is what distinguishes "embedded in geometry"
		// from "standing on the world".
		//
		// The 0.05 is slack on top of the capsule's own reach, so a character
		// hovering a hair above the ground still counts as grounded rather than
		// being treated as embedded.
		if (fraction >= 1.0f && !hasFloorBelow(origin, halfHeight + radius + 0.05f))
		{
			// Genuinely embedded, with nothing to stand on. Try to rise out of
			// whatever it is inside. The free rise is how far we can go before
			// hitting a ceiling, so lift by that.
			const float escapeDistance = halfHeight * 2.0f + radius + 0.05f;
			float riseFraction = b3World_CastMover(
				m_Physics3DWorld, start, &capsule,
				b3Vec3{0.0f, escapeDistance, 0.0f}, filter, nullptr, nullptr);
			// Only depenetrate when there is barely any room to rise.
			//
			// The 0.1 threshold is a heuristic and worth understanding: if even 10% of
			// the escape distance is free, the character is assumed to be somewhere
			// legitimate and NOT lifted at all. So a character embedded deep in a tall
			// shaft -- where there is plenty of headroom above -- is left embedded,
			// while one wedged under a low ledge is lifted.
			//
			// The lift is the free rise PLUS a flat 0.05, so even a riseFraction of
			// exactly zero still nudges the capsule upward rather than leaving it
			// exactly where it was -- otherwise it would re-detect as embedded every
			// frame and never make progress.
			if (riseFraction <= 0.1f)
			{
				float lift = escapeDistance * riseFraction + 0.05f;
				if (outTranslation)
					*outTranslation = translation + glm::vec3(0.0f, lift, 0.0f);
				if (outNormal)
					*outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
				if (outFraction)
					*outFraction = 1.0f;
				return true;
			}
		}

		// An unobstructed sweep that was not an embedding case: the move is
		// fully allowed. Returning here is what keeps a character standing on
		// open floor from being reported as "blocked" and sent down the
		// contact-normal path below.
		if (fraction >= 1.0f)
		{
			if (outFraction)
				*outFraction = 1.0f;
			if (outTranslation)
				*outTranslation = translation;
			return false;
		}

		// Unit direction of travel. Declared before the branches below so the
		// skin-adjusted output and the contact probe both use it.
		const glm::vec3 dir = glm::normalize(translation);

		// Blocked, so report the clamped move and recover the contact data.
		// Back the travel off by the skin so the capsule ends up just outside
		// the surface rather than tangent to it.
		if (outFraction)
			*outFraction = fraction;
		if (outTranslation)
		{
			float dist = glm::length(translation) * fraction;
			// Backs the blocked move off by the skin so the capsule stops just short of
			// the surface.
			//
			// THE `dist > skin ?` GUARD CREATES A DEADZONE. When the sweep allowed less
			// than the skin distance, the output is exactly ZERO -- the character does
			// not move at all. That is the right call for a move entirely consumed by
			// the skin, but it also means a small legitimate adjustment silently becomes
			// no movement, which can read as input being ignored.
			//
			// The result is a displacement along the ORIGINAL direction, not the
			// post-skin position, so it stays colinear with what was asked for.
			*outTranslation = dir * (dist > skin ? dist - skin : 0.0f);
		}

		// Recover the contact normal and the blocking entity with a short ray
		// along the blocked direction. CastMover deliberately does not report
		// contacts ("not a good source of information about what the mover is
		// touching"), so this second, cheap query supplies them.
		//
		// The ray starts at the capsule's CENTER, not at the clamped position
		// and not offset along the motion. After clamping, the capsule's
		// leading surface already touches the obstruction, so a ray started
		// there begins inside the shape and finds nothing. Starting at the
		// center keeps it inside the capsule -- and therefore outside any
		// geometry the capsule is merely touching -- so the ray travels
		// through open space and lands on the surface ahead.
		glm::vec3 probeOrigin = origin;

		// Reach the surface ahead: the distance the capsule was allowed to
		// travel, plus the capsule's own extent in that direction, plus a
		// skin. The allowed distance is the important part -- it is where
		// the contact actually is, and it can be many units away for a fast
		// move, so probing only the capsule extent would miss it entirely.
		const float clampedDist = glm::length(translation) * fraction;
		const float probeLength = clampedDist + halfHeight + radius + 0.05f;

		Entity hitEntity;
		glm::vec3 hitPoint(0.0f);
		glm::vec3 hitNormal(0.0f);
		float hitDistance = 0.0f;
		bool hitHittable = false;
		// Contact probe: a short ray along the blocked direction, purely to
		// recover WHICH surface blocked the move and WHICH WAY it faces.
		//
		// Inherits the RaycastIgnore inconsistency described at hasFloorBelow: this
		// ray skips entities the sweep treated as solid, so it can come back empty
		// for a RaycastIgnore obstruction. The move is still reported as blocked --
		// correct, the caller must stop -- but outHitEntity and outNormal keep
		// their defaults of a null Entity and a ZERO vector.
		//
		// A zero outNormal is the dangerous part: callers commonly use it to slide
		// the remaining motion along the surface, and sliding against a zero vector
		// produces a degenerate or NaN result.
		//
		// probeLength scales with how far the capsule was allowed to travel, so a
		// fast move still finds its contact -- see the comment above it for why
		// probing only the capsule extent would miss it.
		if (Raycast(probeOrigin, dir, probeLength,
					&hitEntity, &hitPoint, &hitNormal, &hitDistance, &hitHittable))
		{
			if (outNormal)
				*outNormal = hitNormal;
			if (outHitEntity)
				*outHitEntity = hitEntity;
		}

		// Blocked, even if the follow-up ray found no surface to attribute it
		// to (a thin shape the probe slipped past, or geometry with no owning
		// entity). The caller must still stop the move.
		return true;
	}


	// Casts a ray into the 3D world and reports the closest surface it meets.
	//
	// Every out-param is optional and independently null-checked, and all five
	// are cleared to defined values before anything can fail -- so a caller
	// passing only the fields it wants never reads an uninitialised value, on
	// any path including the early returns.
	//
	// Direction need NOT be normalised: it is normalised here. maxDistance is
	// in world units regardless of the direction vector's length.
	//
	// CONTRAST WITH MoveCapsule, the other query here: its outNormal default
	// is the ZERO vector, while this function's is (0, 1, 0). So the same
	// caller pattern -- slide along the returned normal -- degrades differently
	// depending on which query produced the miss. Straight up is the safer of
	// the two defaults, since a zero normal makes a slide degenerate.
	//
	// @param origin         Ray start, in world space.
	// @param direction       Ray direction; normalised internally.
	// @param maxDistance     Maximum reach in world units.
	// @param outEntity       Optional; receives the entity owning the hit, or a
	// null Entity for bare world geometry.
	// @param outPoint        Optional; world-space hit position.
	// @param outNormal       Optional; surface normal. Defaults to (0,1,0) on a
	// miss.
	// @param outDistance     Optional; distance from origin to the hit.
	// @param outHitHittable  Optional; whether the hit counts as a damageable
	// target rather than plain geometry. See the note at
	// that calculation.
	// @returns Whether anything was hit.
	bool Scene::Raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance,
						Entity *outEntity, glm::vec3 *outPoint, glm::vec3 *outNormal,
						float *outDistance, bool *outHitHittable) const
	{
		// Clear the outputs up front so a miss never leaves stale values
		// from a previous successful call visible to the caller.
		if (outEntity)
			*outEntity = Entity();
		if (outPoint)
			*outPoint = glm::vec3(0.0f);
		if (outNormal)
			// Miss defaults: entity null, point at the origin, distance zero, not
			// hittable -- but normal STRAIGHT UP rather than zero.
			//
			// Deliberate and better than the alternative: a caller that slides motion
			// along the returned normal on a miss is projected onto the horizontal
			// plane, which is benign, whereas sliding against a zero vector is
			// undefined and can produce NaN.
			//
			// Note MoveCapsule defaults its normal to zero instead, so the two query
			// functions are inconsistent on this point.
			*outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		if (outDistance)
			*outDistance = 0.0f;
		if (outHitHittable)
			*outHitHittable = false;

		// A non-positive range or a zero-length direction would make
		// Box3D test a degenerate ray; report a miss instead of letting it
		// return a meaningless hit at distance 0.
		if (maxDistance <= 0.0f)
			return false;
		float dirLen = glm::length(direction);
		if (dirLen <= 1e-6f)
			return false;
		if (!b3World_IsValid(m_Physics3DWorld))
			return false;

		glm::vec3 dir = direction / dirLen;

		b3QueryFilter filter = b3DefaultQueryFilter();
		// Accept every collision category/mask pair. Games tune this later;
		// an unfiltered default means a fresh scene just works.
		filter.categoryBits = 0xffffffffull;
		filter.maskBits = 0xffffffffull;

		// Box3D's translation is the vector from ray start to ray end, so a
		// maxDistance trace is dir * maxDistance.
		b3Pos start{(float)origin.x, (float)origin.y, (float)origin.z};
		b3Vec3 translation{(float)(dir.x * maxDistance), (float)(dir.y * maxDistance), (float)(dir.z * maxDistance)};

		// The closest-hit cast. b3World_CastRayClosest returns only the NEAREST
		// surface, not a list.
		//
		// So this function cannot answer "what is behind that wall", and a caller
		// wanting a full picture must call it repeatedly. That is also what makes
		// the RaycastIgnore walk below work: each pass is simply another closest-hit
		// cast from a nudged origin.
		//
		// translation is start-to-end, not a unit direction -- the second half of
		// Box3D's ray convention, and the reason dir * maxDistance is built above.
		b3RayResult result = b3World_CastRayClosest(m_Physics3DWorld, start, translation, filter);
		if (!result.hit)
			return false;

		entt::entity hit = FindHandleByShape(m_Registry, b3StoreShapeId(result.shapeId));

		// The shooter can be its own first hit: a player capsule is a real
		// physics shape, so a trace starting inside it reports distance 0.
		// Walk the ray forward past any hit flagged RaycastIgnore, bounded
		// so a degenerate case cannot spin forever.
		// Bound on how many RaycastIgnore surfaces a single trace can pass through.
		//
		// The loop below re-casts after each ignored surface, so an unbounded walk
		// would spin forever on geometry configured to always be ignored. Eight is
		// generous for glass, foliage and trigger volumes.
		//
		// THE BOUND IS NOT ENFORCED ON THE RESULT. When all eight passes are
		// consumed and the hit is STILL an ignored entity, the loop simply ends --
		// there is no check that the exit was a terminal hit rather than an
		// exhausted budget. Control falls through to the reporting code below,
		// which sets outEntity to that ignored entity and returns true.
		//
		// So a ray crossing nine or more ignored surfaces reports the ninth as a
		// real hit. Rare, but it is a silent wrong answer rather than a clean
		// "out of passes" result, and the caller has no way to tell it apart from a
		// legitimate hit. A flag set when the budget runs out would make it
		// detectable.
		const int MaxIgnorePasses = 8;
		const float Epsilon = 0.01f;
		// Distance already consumed by earlier segments of the walk. Zero
		// when the very first hit is accepted, in which case outDistance
		// below is just the segment length of the original trace.
		float travelled = 0.0f;
		// Walks past ignored surfaces until it lands on something terminal.
		//
		// Termination is on two different conditions -- a non-ignored hit, or the
		// pass budget. See the note on MaxIgnorePasses for what happens when the
		// second one fires.
		for (int pass = 0; pass < MaxIgnorePasses; ++pass)
		{
			// A terminal hit is either a real (non-ignored) entity, or
			// geometry with no owning entity. Both end the walk.
			if (hit == entt::null || !IsRaycastIgnored(hit))
				break;

			float segment = result.fraction * (maxDistance - travelled);
			float remaining = maxDistance - travelled - segment - Epsilon;
			if (remaining <= 0.0f)
				break; // nothing left to trace

			// Nudge the origin past the ignored surface so the next cast
			// starts outside it instead of re-hitting the same shape.
			glm::vec3 advanced = origin + dir * (travelled + segment + Epsilon);
			b3Pos nextStart{(float)advanced.x, (float)advanced.y, (float)advanced.z};
			b3Vec3 nextTranslation{(float)(dir.x * remaining), (float)(dir.y * remaining), (float)(dir.z * remaining)};

			result = b3World_CastRayClosest(m_Physics3DWorld, nextStart, nextTranslation, filter);
			if (!result.hit)
				return false;

			// Carry the ignored surface and the nudge into the running total
			// so the reported distance is measured from the caller's origin.
			travelled += segment + Epsilon;
			hit = FindHandleByShape(m_Registry, b3StoreShapeId(result.shapeId));
		}

		// Only entities that opted into being shootable count as a target.
		// World geometry (floors, walls) must not read as one, otherwise
		// every shot reports a "hit" on the first wall it meets.
		bool isHittable = false;
		if (hit != entt::null && m_Registry.valid(hit))
		{
			if (const auto *marker = m_Registry.try_get<HittableComponent>(hit))
				isHittable = marker->Enabled;
			// An entity counts as a damageable target either by explicitly carrying
			// HittableComponent (honouring its Enabled flag) or by simply having
			// HealthComponent.
			//
			// The implicit second case is a convenience -- anything with health is
			// presumably shootable -- but it means the comment's claim that "only
			// entities that opted into being shootable count" is not quite right:
			// HealthComponent is an implicit opt-in that needs no HittableComponent.
			// An entity that wants to be damageable but NOT raycast-hittable has to
			// carry an explicit HittableComponent with Enabled false, because omitting
			// it while holding HealthComponent still reports true.
			else if (m_Registry.all_of<HealthComponent>(hit))
				isHittable = true;
		}

		if (outEntity && hit != entt::null)
			// Hands the caller an Entity that can MUTATE this scene, out of a const
			// method.
			//
			// The const_cast is necessary because Entity holds a non-const Scene*, but
			// it means the method's const contract does not hold through its return
			// value: a caller with only a `const Scene&` can call Raycast and then
			// mutate the scene through the Entity it gets back. Any genuinely const
			// use of Scene -- a read-only query pass, a const-correct subsystem --
			// leaks write access through this path.
			//
			// Entity also asserts on use via its scene pointer, which is why the null
			// case is handled by leaving outEntity at its cleared default rather than
			// constructing one.
			*outEntity = Entity{hit, const_cast<Scene *>(this)};
		if (outPoint)
			*outPoint = glm::vec3((float)result.point.x, (float)result.point.y, (float)result.point.z);
		if (outNormal)
			*outNormal = glm::vec3((float)result.normal.x, (float)result.normal.y, (float)result.normal.z);
		if (outDistance)
			// Distance from the CALLER's origin, not from the last re-cast origin.
			//
			// The bookkeeping is correct, and worth verifying because it is easy to get
			// wrong: `travelled` accumulates (segment + Epsilon) for each ignored
			// surface passed, so after an update maxDistance - travelled is exactly
			// the `remaining` range the last cast covered. Adding that cast's fraction
			// of it therefore lands on the true distance from the original origin.
			// On the no-ignores path travelled is 0 and this reduces to fraction *
			// maxDistance.
			*outDistance = travelled + result.fraction * (maxDistance - travelled);
		if (outHitHittable)
			*outHitHittable = isHittable;

		return true;
	}

	// Submits the whole scene for drawing: 2D first, then 3D, each bracketed by
	// its own Begin/EndScene pair.
	//
	// ONE entry point for both the editor viewport and the runtime path. The
	// comment on SubmitMeshPrimitives explains why that matters: separate
	// Cube/Sphere blocks in the two paths drifted, and a shape ended up visible
	// in one and missing from the other. Everything below is therefore shared.
	//
	// ORDER IS DELIBERATE: 2D is bracketed and completed before 3D begins, so
	// the 2D batch is flushed rather than interleaved with 3D geometry. The
	// volumetric/light/water/primitive submissions sit INSIDE the 3D
	// BeginScene/EndScene span, which is required -- they feed the same frame
	// the primitives are drawn into.
	//
	// Every draw call takes (int)entity as the per-instance identity, which is
	// how the renderers key per-instance data. Note this is the entt handle, so
	// it is only meaningful within this scene's registry.
	//
	// @param camera  The viewpoint. An EditorCamera in the editor and the active
	// SceneCamera in play mode -- the parameter is typed for the
	// editor case and the runtime path passes the scene's own
	// primary camera through the same slot.
	void Scene::RenderScene(EditorCamera &camera)
		{
			// Skip the entire render pipeline when the device is lost: every NVRHI
			// command below (writeBuffer, drawIndexed, dispatch, etc.) dereferences
			// m_ActiveCommandList, which is null because BeginFrame skipped open()
			// after the TDR. Without this guard the editor crashes inside the first
			// command list call.
			if (auto *ctx = NVRHIRendererAPI::GetCurrentContext())
			{
				if (ctx->IsDeviceLost())
					return;
			}
			// Render 2D
			// Opens the 2D batch, submits sprites and circles, then closes it below
			// with EndScene before any 3D work begins.
			//
			// Sprites and circles are drawn in SEPARATE loops rather than one loop over
			// both. That means all sprites are submitted before all circles, so a
			// circle always draws over a sprite regardless of scene position. With no
			// depth sorting in a 2D batch that ordering is purely submission order, so
			// this is the whole draw-order story for 2D.
			Renderer2D::BeginScene(camera);

		// Draw sprites
		{
			// Sprites use an entt GROUP, while circles and everything in 3D use plain
			// views below.
			//
			// MIXED IDIOM WITHIN ONE FUNCTION, and it has a real consequence. A group
			// iterates in the order of its key component (TransformComponent), so
			// sprite submission order is stable and follows transform identity. A
			// view iterates in component-storage order, which is an implementation
			// detail that changes as entities are added and removed. So sprite draw
			// order is deterministic while circle draw order is not.
			//
			// If sprite ordering mattered -- overlapping sprites where one must sit
			// above another -- it is stable here by accident of using a group, and
			// would break if this were changed to a view to match its neighbours.
			auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
			for (auto entity : group)
			{
				auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

				Renderer2D::DrawSprite(transform.GetTransform(), sprite, (int)entity);
				// Renderer2D::DrawRect(transform.Translation, transform.Scale, glm::vec4(1.0f), (int)entity);
			}
		}

		// Draw circles
		{
			auto view = m_Registry.view<TransformComponent, CircleRendererComponent>();
			for (auto entity : view)
			{
				auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

				Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, (int)entity);
			}
		}

		// Renderer2D::DrawLine(glm::vec3(2.0f), glm::vec3(5.0f), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
		// Renderer2D::DrawRect(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

		Renderer2D::EndScene();

		// Render 3D
		Renderer3D::BeginScene(camera);
		SubmitVolumetricVolumes();
		SubmitSceneLights();
		SubmitWaterSurfaces();
		SubmitMeshPrimitives();
		SubmitTerrainChunks();

		// Draw Cubes
		{
			auto cubeView = m_Registry.view<TransformComponent, CubeRendererComponent, MaterialComponent>();
			for (auto entity : cubeView)
			{
				auto &tc = cubeView.get<TransformComponent>(entity);
				auto &crc = cubeView.get<CubeRendererComponent>(entity);
				auto &mc = cubeView.get<MaterialComponent>(entity);

				// Pass the 3D entity data to the new batch renderer
				Renderer3D::DrawCube(tc.GetTransform(), crc.Color, mc, (int)entity);
			}
		}

		// Draw Spheres
		{
			auto sphereView = m_Registry.view<TransformComponent, SphereRendererComponent, MaterialComponent>();
			for (auto entity : sphereView)
			{
				auto &tc = sphereView.get<TransformComponent>(entity);
				auto &src = sphereView.get<SphereRendererComponent>(entity);
				auto &mc = sphereView.get<MaterialComponent>(entity);

				Renderer3D::DrawSphere(
					tc.GetTransform(),
					src.Color,
					src.Radius,
					src.Sectors,
					src.Stacks,
					mc,
					(int)entity);
			}
		}

		Renderer3D::EndScene();
	}

	// void Scene::OnComponentAdded(Entity entity, T &component)
	// PRIMARY TEMPLATE, WHICH MUST NEVER BE INSTANTIATED.
	//
	// The `static_assert(false)` is a deliberate compile-time trap: a component
	// type with no specialization below is a hard error rather than a silent
	// no-op. That makes "add a new component" an explicit decision -- you
	// either specialize it or deliberately leave it out, and the compiler
	// forces the choice.
	//
	// The assertion is not ill-formed at template definition time only because
	// it depends on nothing here; in practice it fires at the point of
	// instantiation, which is the intended moment. It relies on C++17 or later
	// semantics for a non-dependent false assertion in a template.
	//
	// The pattern is why most specializations below are EMPTY: the hook is
	// dispatched per component type, so a type needing no reaction still needs a
	// specialization to satisfy the trap. An empty body IS the declaration of
	// "nothing to do here".
	template <typename T>
	void Scene::OnComponentAdded(Entity entity, T &component)
	{
		static_assert(false, "OnComponentAdded must be specialized for this component type");
	}

	template <>
	void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent &component)
	{
	}

	template <>
	// Repairs the camera's projection the moment it is attached.
	//
	// This exists because of the SceneCamera default-state hazard: a
	// default-constructed SceneCamera has a zero aspect ratio, and with the
	// default orthographic projection type that makes GLM divide by zero,
	// leaving infinities in the projection matrix. See SceneCamera.h.
	//
	// Calling SetViewportSize here means a camera is valid from the frame it
	// is created rather than from the next viewport update -- so the default
	// state never reaches the renderer.
	//
	// Uses the Scene's cached viewport dimensions rather than a fresh query, so
	// it is correct even before the first OnViewportResize.
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent &component)
	{
		component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}

	template <>
	void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CircleRendererComponent>(Entity entity, CircleRendererComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CubeRendererComponent>(Entity entity, CubeRendererComponent &component)
	{
		// Self-heal: give the entity a transform if it somehow lacks one.
		//
		// Needed because every 3D draw path uses a multi-type view over
		// TransformComponent and the shape component together, so an entity with a
		// shape but no transform would simply be invisible rather than defaulted.
		// Adding the transform here makes the entity renderable immediately.
		//
		// In practice CreateEntityWithUUID already adds a TransformComponent to
		// every entity, so this is defensive -- it matters for an entity built by
		// hand through the registry, or one whose transform was removed.
		//
		// Adding the component re-enters this dispatcher for
		// TransformComponent, whose specialization is empty, so the recursion is
		// exactly one level deep and terminates.
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
	}

	template <>
	void Scene::OnComponentAdded<SphereRendererComponent>(Entity entity, SphereRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Sectors = 20;
		component.Stacks = 20;
	}

	template <>
	void Scene::OnComponentAdded<PlaneRendererComponent>(Entity entity, PlaneRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Subdivisions = 0;
	}

	template <>
	void Scene::OnComponentAdded<Circle3DRendererComponent>(Entity entity, Circle3DRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Segments = 32;
	}

	template <>
	void Scene::OnComponentAdded<IcoSphereRendererComponent>(Entity entity, IcoSphereRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Subdivisions = 2;
	}

	template <>
	void Scene::OnComponentAdded<CylinderRendererComponent>(Entity entity, CylinderRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Height = 1.0f;
		component.Segments = 32;
	}

	template <>
	void Scene::OnComponentAdded<ConeRendererComponent>(Entity entity, ConeRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Height = 1.0f;
		component.Segments = 32;
	}

	template <>
	void Scene::OnComponentAdded<TorusRendererComponent>(Entity entity, TorusRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.MajorRadius = 0.375f;
		component.MinorRadius = 0.125f;
		component.MajorSegments = 48;
		component.MinorSegments = 12;
	}

	template <>
	void Scene::OnComponentAdded<GridRendererComponent>(Entity entity, GridRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.XSubdivisions = 10;
		component.YSubdivisions = 10;
	}

	template <>
	void Scene::OnComponentAdded<MaterialComponent>(Entity entity, MaterialComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<ScriptComponent>(Entity entity, ScriptComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<BehaviorTreeComponent>(Entity entity, BehaviorTreeComponent &component)
	{
		// Nothing to do at attach time. The tree compiles on the FIRST runtime
		// tick, not here: an entity can gain a tree at any point, and doing the
		// work here would make attaching a tree a blocking call on whatever
		// code path happened to add the component.
		//
		// This specialization MUST exist. The primary template is
		// static_assert(false), so without one the linker wants this exact symbol
		// and the whole module fails with LNK1120.
		//
		// Clear stale state so re-attaching a different tree is not confused with
		// the previous one.
		component.Attached = false;
		component.CompileError.clear();
		component.SuspendedTicks = 0;
	}

	template <>
	void Scene::OnComponentAdded<Rigidbody2DComponent>(Entity entity, Rigidbody2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<BoxCollider2DComponent>(Entity entity, BoxCollider2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CircleCollider2DComponent>(Entity entity, CircleCollider2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<RigidBody3DComponent>(Entity entity, RigidBody3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<BoxCollider3DComponent>(Entity entity, BoxCollider3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<SphereCollider3DComponent>(Entity entity, SphereCollider3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<VolumetricFogComponent>(Entity entity, VolumetricFogComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<VolumetricCloudsComponent>(Entity entity, VolumetricCloudsComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<SpotLightComponent>(Entity entity, SpotLightComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<AreaLightComponent>(Entity entity, AreaLightComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<WaterComponent>(Entity entity, WaterComponent &component)
	{
		// NOTE: do NOT apply presets here — AddOrReplaceComponent (duplicate /
		// deserialization) also runs this handler, and a preset would wipe the
		// source values. Member defaults already describe a calm lake; the
		// editor UI applies River/Ocean presets on demand. Just reset runtime.
		component.Time = 0.0f;
	}

	template <>
	void Scene::OnComponentAdded<HealthComponent>(Entity entity, HealthComponent &component)
	{
		// Cooldown is runtime-only; a component arriving via duplicate or
		// deserialization must not inherit a live damage lockout.
		component.CooldownTimer = 0.0f;
	}

	template <>
	void Scene::OnComponentAdded<WeaponComponent>(Entity entity, WeaponComponent &component)
	{
		// Same reasoning as health: a freshly added or copied weapon starts
		// with a full magazine and no in-flight cooldown/reload. The
		// copy-assignment ctors already keep the authored ammo numbers.
		component.FireCooldown = 0.0f;
		component.ReloadTimer = 0.0f;
		component.IsReloading = false;
	}

	template <>
	void Scene::OnComponentAdded<RaycastIgnoreComponent>(Entity entity, RaycastIgnoreComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<HittableComponent>(Entity entity, HittableComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<InteractableComponent>(Entity entity, InteractableComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<WeaponFeedbackComponent>(Entity entity, WeaponFeedbackComponent &component)
	{
		// A fresh feedback component is always "not showing". Authored
		// durations are kept, but the timers start at zero: an entity loaded
		// from a save mid-flash would otherwise pop a muzzle flash on the
		// first frame for no reason the player can act on.
		component.MuzzleFlashTimer = 0.0f;
		component.HitMarkerTimer = 0.0f;
		component.LastShotHit = false;
		component.LastShotKilled = false;
	}

	template <>
	void Scene::OnComponentAdded<AudioSourceComponent>(Entity entity, AudioSourceComponent &component)
	{
		// Nothing to do at attach time, and deliberately so.
		//
		// PlayOnStart is honoured by AudioSystem::PlayAutoStartSources, which runs
		// from the scene update rather than from here. Firing it on ADD would
		// mean a source starts playing when the COMPONENT is added, not when the
		// sound is available -- a clip that has not been loaded yet fails, and
		// deserializing a scene plays every source at once during the load rather
		// than when the world actually begins.
		//
		// This specialization exists because Entity::AddComponent routes through
		// Scene::OnComponentAdded<T>, and a template member with no explicit
		// specialization is an unresolved external at link time. The audio pair
		// were unbound from Python for so long that nothing ever instantiated the
		// template for them, so the missing definition was invisible until the
		// first script could attach a source -- which is the whole point of
		// binding them.
	}

	template <>
	void Scene::OnComponentAdded<AudioListenerComponent>(Entity entity, AudioListenerComponent &component)
	{
		// Also deliberately inert. AudioSystem::FindListenerEntity resolves the
		// listener each mix by scanning for AudioListenerComponent and falls back
		// to the primary camera, so re-deriving that decision here would create a
		// second source of truth for "who is listening". Two listeners would then
		// fight over it: whichever the scan happened to find first, in
		// registration order, rather than whichever the scene intended.
	}

	template <>
	void Scene::OnComponentAdded<ViewStateComponent>(Entity entity, ViewStateComponent &component)
	{
		// Not driven here. The player script is the sole writer of TargetFOV
		// precisely so two systems cannot fight over the camera, and doing
		// anything to it on load would break that.
	}

	template <>
	void Scene::OnComponentAdded<InventoryComponent>(Entity entity, InventoryComponent &component)
	{
		// Drop any stack that deserialized or a caller added as invalid.
		//
		// A zero-quantity or empty-id row still consumes a slot, so leaving
		// one in place silently eats the player's capacity until something
		// else happens to rewrite the list. Scrubbing here means the
		// invariant holds for every path that populates Items.
		for (auto it = component.Items.begin(); it != component.Items.end();)
		{
			if (it->IsValid())
				++it;
			else
				it = component.Items.erase(it);
		}
	}

	template <>
	void Scene::OnComponentAdded<ItemDefinitionComponent>(Entity entity, ItemDefinitionComponent &component)
	{
		// A MaxStack of 0 means "unlimited" by convention, but a NEGATIVE
		// value is meaningless and would make every stack-capacity check
		// behave as if unlimited while looking like a bug. Clamp to the
		// unlimited sentinel.
		if (component.MaxStack < 0)
			component.MaxStack = 0;

		if (component.Weight < 0.0f)
			component.Weight = 0.0f;
	}

	template <>
	void Scene::OnComponentAdded<DroppedItemComponent>(Entity entity, DroppedItemComponent &component)
	{
		// Clamp, rather than reject. A negative quantity is meaningless --
		// there is no such thing as "minus three on the floor" -- and
		// leaving it in place would make PickUpDroppedItem's
		// `pile->Quantity -= taken` walk further negative and destroy
		// nothing. Clamping to 0 matches ItemDefinitionComponent's
		// handling of a negative MaxStack above, and IsValid() then reports
		// false so the serializer and the pickup both refuse it.
		//
		// The ItemId is NOT defaulted or sanitised: an empty id is a
		// meaningful authoring error (the pile does not say what it is),
		// and silently inventing "item" would hide it. IsValid() reports it
		// instead.
		//
		// This specialization exists because Entity::AddComponent routes
		// through Scene::OnComponentAdded<T>, whose primary template is
		// `static_assert(false, ...)` -- so without an explicit
		// specialization this is a COMPILE ERROR at the AddComponent call
		// inside DropItemAt. See the audio pair above for the same trap
		// reached from the scripting side instead.
		if (component.Quantity < 0)
			component.Quantity = 0;
	}


	template <>
	void Scene::OnComponentAdded<PersistentComponent>(Entity entity, PersistentComponent &component)
	{
		// A negative streaming radius is meaningless and would silently
		// disable the entity, so clamp it to the "never streamed" sentinel
		// of 0 rather than letting a typo in the data hide an entity.
		if (component.StreamingRadius < 0.0f)
			component.StreamingRadius = 0.0f;
	}

	template <>
	void Scene::OnComponentAdded<QuestFlagsComponent>(Entity entity, QuestFlagsComponent &component)
	{
		// Drop empty keys. Duplicate keys are the real hazard: Has() and
		// Get() return the FIRST match while Set() updates the first match
		// too, so a duplicate makes a flag read as unset forever with no
		// error anywhere.
		for (auto it = component.Flags.begin(); it != component.Flags.end();)
		{
			if (!it->first.empty())
				++it;
			else
				it = component.Flags.erase(it);
		}
	}

	template <>
	void Scene::OnComponentAdded<WorldTimerComponent>(Entity entity, WorldTimerComponent &component)
	{
		// 0.0 is a MEANINGFUL value here (an expired cooldown), so unlike
		// inventory stacks there is no "invalid" state to scrub. A non-finite
		// value, however, is data corruption: it would poison every later
		// comparison and never compare equal to anything.
		for (auto it = component.Timers.begin(); it != component.Timers.end();)
		{
			if (std::isfinite(it->second))
				++it;
			else
				it = component.Timers.erase(it);
		}
	}

	// =====================================================================
	// Audio (14.4)
	// =====================================================================

	Ref<Audio::AudioSystem> Scene::GetAudioSystem()
	{
		// Lazy, and NOT reset on scene clear. Clearing drops entities but the
		// mixer and its loaded clips are scene-independent content, and
		// re-decoding every WAV on each play start would stall the first frame.
		if (!m_AudioSystem)
			m_AudioSystem = std::make_shared<Audio::AudioSystem>(this);
		return m_AudioSystem;
	}

	int Scene::PlaySound(const std::string &clipName)
	{
		return GetAudioSystem()->Play(clipName);
	}

	int Scene::PlaySoundAt(const std::string &clipName, const glm::vec3 &position,
						   float volume, float pitch)
	{
		return GetAudioSystem()->PlayAt(clipName, position, volume, pitch);
	}

	int Scene::PlayEntitySound(Entity entity)
	{
		return GetAudioSystem()->PlayEntity(entity);
	}

	void Scene::StopSound(const std::string &clipName, float fadeSeconds)
	{
		GetAudioSystem()->StopClip(clipName, fadeSeconds);
	}

	void Scene::StopAllSounds(float fadeSeconds)
	{
		GetAudioSystem()->StopAll(fadeSeconds);
	}

	void Scene::SetMasterVolume(float volume)
	{
		GetAudioSystem()->SetMasterVolume(volume);
	}

	float Scene::GetMasterVolume() const
	{
		// Const-correctness beats convenience here: a caller that only wants
		// to read the volume should not force a mixer into existence just to
		// discover it is the default.
		if (!m_AudioSystem)
			return 1.0f;
		return m_AudioSystem->GetMasterVolume();
	}

	bool Scene::LoadAudioClip(const std::string &path)
	{
		return GetAudioSystem()->LoadClip(path);
	}


bool Scene::ConfigureTerrainRuntime(const Terrain::TerrainAsset& asset,
	const Terrain::TerrainWorldState& worldState,
	const Terrain::TerrainStreamingConfig& config, std::string* error,
	const std::string& assetReference)
{
	if (!m_TerrainRuntime.Configure(asset, worldState, config, error))
		return false;
	m_TerrainRuntime.SetPhysicsWorld(m_Physics3DWorld);
	m_TerrainAssetReference = assetReference;
	return true;
}

bool Scene::SetTerrainWorldState(const Terrain::TerrainWorldState& worldState, std::string* error)
{
	return m_TerrainRuntime.SetWorldState(worldState, error);
}

bool Scene::UpdateTerrainStreaming(double worldX, double worldZ, std::string* error)
{
	return m_TerrainRuntime.UpdateView(worldX, worldZ, error);
}

bool Scene::BakeNavMesh(const Navigation::NavMeshConfig &config)
{
	// Rebuild rather than reuse: level geometry changes while the editor runs,
	// and a stale mesh sends agents through walls that have since appeared.
	// Baking is cheap next to a frame (it is a grid rasterisation over the
	// colliders already in the registry) and happens on demand, not per tick.
	m_NavMesh = std::make_unique<Navigation::NavMesh>();
	return m_NavMesh->Bake(*this, config);
}
}
