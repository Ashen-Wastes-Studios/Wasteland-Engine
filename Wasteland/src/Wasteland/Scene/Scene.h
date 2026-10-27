#pragma once

#include <entt/entt.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "Wasteland/Core/Timestep.h"
#include "Wasteland/Renderer/EditorCamera.h"
#include "Wasteland/Core/UUID.h"
#include "Wasteland/Navigation/NavMesh.h"
#include "Wasteland/Terrain/TerrainRuntime.h"

#include <box3d/id.h>

class b2World;

namespace Wasteland
{

	namespace Audio
	{
		class AudioSystem;
	}

	class Entity;

	// Forward-declared at NAMESPACE scope, not as a nested struct inside
	// Scene. A nested declaration creates Scene::ItemDefinitionComponent,
	// which is a DIFFERENT type from Wasteland::ItemDefinitionComponent, and
	// every out-of-line definition then fails to match ("overloaded function
	// differs only by return type").
	//
	// Forward rather than #include because Components.h includes
	// SceneCamera.h; returning a pointer needs only the incomplete type.
	struct ItemDefinitionComponent;

	// Forward for the same reason: Scene declares the drop/pickup API in
	// terms of the pile's identity, and Components.h includes Scene.h.
	struct DroppedItemComponent;

	class Scene
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Copy(Ref<Scene> other);

		Entity CreateEntity(const std::string &name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string &name = std::string());
		void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();
		void OnUpdateSimulation(Timestep ts, EditorCamera &camera);

		// Advance the physics solvers and write the results back into
		// TransformComponent, with no rendering and no camera.
		//
		// OnUpdateSimulation cannot be used for this: it takes an
		// EditorCamera&, which only exists inside the editor viewport. A
		// headless caller has no way to construct one, so the physics step was
		// unreachable from Python or a dedicated server build. Physics has no
		// dependency on a camera, so it does not belong behind one.
		void StepPhysics(float seconds);

		// One fixed-size integration step. StepPhysics() owns the accumulator and
		// calls this; it is not part of the scripting API.
		void StepPhysicsFixed(float seconds);

		// Leftover time not yet consumed by a whole fixed step. Persisted between
		// frames so a caller running at 10fps still advances real time.
		float m_PhysicsAccumulator = 0.0f;

		// See BumpTransformEpoch(). 0 is a valid epoch, so the renderer's "no epoch
		// recorded yet" sentinel is 0xFFFFFFFF rather than 0.
		uint32_t m_TransformEpoch = 0;

		// Build the Box3D body and collider shapes for one entity.
		//
		// Idempotent: an entity that already has a RuntimeBody is skipped.
		// Called from OnPhysics3DStart for the initial registry walk, and from
		// StepPhysics for anything added later -- so a dropped item or a
		// spawned enemy actually simulates instead of hanging in the air.
		void CreatePhysicsBody3D(entt::entity e);

		// Create any physics bodies that do not exist yet.
		void SyncPendingPhysicsBodies();

		// --- The 2D mirror of the two above ---

		// Build the Box2D body and collider shapes for one entity.
		//
		// Idempotent: an entity that already has a RuntimeBody is skipped.
		// Called from OnPhysics2DStart for the initial registry walk, and from
		// SyncPendingPhysics2D for anything added later -- so a dropped item or
		// a spawned prop actually simulates instead of hanging in mid-air.
		void CreatePhysicsBody2D(entt::entity e);

		// Give a Box2D body to any 2D rigidbody that does not have one yet.
		//
		// The 2D counterpart of SyncPendingPhysicsBodies, called from the same
		// two places in StepPhysics. It exists for the same reason: before it,
		// OnPhysics2DStart was the only place a b2Body was ever created, so a
		// Rigidbody2DComponent added after startup kept RuntimeBody null
		// forever -- it never fell, never collided, and never blocked anything,
		// with nothing logged.
		void SyncPendingPhysics2D();

		// --- Driving a 3D body from script (route map 5.2) ---
		//
		// These exist so a script can JUMP, KNOCK BACK or LAUNCH without
		// touching RigidBody3DComponent::RuntimeBody. That handle is a
		// process-local Box3D body id: it is meaningless in another process,
		// and it is STALE after OnPhysics3DStop() -- a script holding one and
		// calling through it after the world was torn down is a
		// use-after-free, not an API. Resolving it here, inside the engine,
		// is what makes these three safe to expose at all.
		//
		// Every one returns false rather than raising when the entity carries
		// no RigidBody3DComponent, when its body has not been created yet,
		// when the handle is stale, or when there is no 3D world. A gameplay
		// script asking "did my jump land?" wants that answer, and a raise
		// would take the whole script frame down instead.
		//
		// The impulse is applied at the center of mass, so it changes
		// velocity without introducing spin.
		bool ApplyImpulse(Entity entity, const glm::vec3 &impulse, bool wake = true);

		// Overwrite the body's linear velocity outright -- the scripted way to
		// STOP something mid-air or to launch it at an exact speed.
		bool SetLinearVelocity(Entity entity, const glm::vec3 &velocity);

		// Read the body's current linear velocity. `out` is untouched when
		// this returns false, so a caller can keep its previous value.
		bool GetLinearVelocity(Entity entity, glm::vec3 &out) const;

		void OnUpdateEditor(Timestep ts, EditorCamera &camera);
		void OnUpdateRuntime(Timestep ts);

		// --- transform change epoch (Phase 2 of the million-entity plan) ---
		//
		// Bumped whenever the runtime path writes a TransformComponent. The renderer
		// publishes the current value each frame via Renderer3D::SetSceneTransformEpoch
		// and compares it against the value recorded when the previous frame's
		// instances were accepted; equal means nothing moved, so the whole-scene
		// instance memcmp can be skipped.
		//
		// The stamp is CONSERVATIVE BY DESIGN, and that is the load-bearing part. It
		// must never miss a write, because the renderer trusts an unchanged epoch to
		// skip the BVH rebuild and the SSBO re-upload -- a missed bump leaves a stale
		// acceleration structure, which is the pop-in this exists to prevent. It is
		// therefore bumped for the WHOLE script pass rather than for an observed
		// transform write: a Python write goes through pybind11's
		// reference_internal view of the member and no C++ setter ever runs, so
		// there is no way to observe it. Over-bumping costs the fast path; the
		// memcmp remains the second gate and recovers from anything the epoch
		// misses, so correctness does not depend on the stamp being precise.
		void BumpTransformEpoch() { ++m_TransformEpoch; }
		uint32_t GetTransformEpoch() const { return m_TransformEpoch; }
		void OnViewportResize(uint32_t width, uint32_t height);

		void DuplicateEntity(Entity entity);

		// Re-create every MaterialComponent texture from its saved TexturePath
		// under the current RendererAPI. GPU texture objects die with their
		// backend (GL names are meaningless after the context is destroyed,
		// NVRHI handles hold a dead device), so after a runtime API switch
		// the stale Refs must be reloaded or every textured draw samples
		// null/dead data. Textures are also re-deduped into the renderers'
		// slots on next submit (slot maps are cleared by RecreateResources).
		void ReloadMaterialTextures();

		Entity GetPrimaryCameraEntity();
		bool IsEntityValid(Entity entity) const;

		// --- Water simulation queries (for gameplay/buoyancy scripts) ---
		// Returns true when worldPos is inside an enabled water body footprint.
		// outHeight = still level (Translation.y) + simulated wave height.
		// outFlow = river current velocity (world XZ), zero for still lakes.
		bool GetWaterHeightAt(const glm::vec3 &worldPos, float &outHeight, glm::vec2 *outFlow = nullptr);
		bool IsUnderwater(const glm::vec3 &worldPos, float margin = 0.0f);

		// Terrain source/settings and sparse edits are owned by the scene runtime;
		// generated meshes and Box3D colliders remain transient derived state.
		bool ConfigureTerrainRuntime(const Terrain::TerrainAsset& asset,
			const Terrain::TerrainWorldState& worldState,
			const Terrain::TerrainStreamingConfig& config, std::string* error = nullptr,
			const std::string& assetReference = {});
		bool SetTerrainWorldState(const Terrain::TerrainWorldState& worldState,
			std::string* error = nullptr);
		bool UpdateTerrainStreaming(double worldX, double worldZ, std::string* error = nullptr);
		void ClearTerrainRuntime() noexcept
		{
			m_TerrainRuntime.Clear();
			m_TerrainAssetReference.clear();
		}
		const Terrain::TerrainRuntime& GetTerrainRuntime() const noexcept { return m_TerrainRuntime; }
		const std::string& GetTerrainAssetReference() const noexcept { return m_TerrainAssetReference; }

		// ---------------------------------------------------------------
		// Trained neural stage (Task 7)
		//
		// A plain bool, deliberately NOT a Renderer3D reference: the Scene layer
		// does not include the renderer and must not start. The editor owns the
		// coupling -- on load it reads this and calls SetNeuralStageEnabled, and on
		// save the serializer reads it and writes the YAML key. Pulling
		// Renderer3D.h into SceneSerializer to save the renderer's live value
		// would invert that dependency for no gain.
		//
		// Default TRUE. The stage is a no-op unless a weights blob is loaded, so
		// enabling it in a scene that never got one costs nothing and a scene that
		// does get one behaves the way it was saved.
		bool IsNeuralStageEnabled() const noexcept { return m_NeuralStageEnabled; }
		void SetNeuralStageEnabled(bool enabled) noexcept { m_NeuralStageEnabled = enabled; }
		bool SaveTerrainWorldState(const std::filesystem::path& projectRoot,
			std::string* error = nullptr)
		{
			return m_TerrainRuntime.SaveWorldState(projectRoot, error);
		}

		// ---------------------------------------------------------------
		// Navigation (14.5.2)
		//
		// The baked navmesh lives on the scene, not on the tree, so ONE bake
		// serves every NPC. A per-tree bake would make pathfinding cost scale
		// with the number of NPCs, which is the opposite of the point.
		// ---------------------------------------------------------------

		// Replace the current mesh and bake from the scene's colliders.
		bool BakeNavMesh(const Navigation::NavMeshConfig &config);
		// The baked mesh, or null when nothing has been baked. Read-only.
		Navigation::NavMesh *GetNavMesh() { return m_NavMesh.get(); }
		// Drop the mesh. Every path query then fails cleanly and agents fall
		// back to direct movement, rather than walking a stale route through
		// geometry that has since moved.
		void ClearNavMesh() { m_NavMesh.reset(); }

		// ---------------------------------------------------------------
		// Inventory
		//
		// These live on Scene rather than on the component because they need
		// to resolve an item's ItemDefinitionComponent, which may be on a
		// different entity. A component cannot reach the registry.
		//
		// After inventory mutation, an instantiated script on the carrier may
		// receive OnInventoryChanged(item_id, old_count, new_count). Counts are
		// totals across all stacks. Fully accepted adds and successful removals
		// notify once after mutation; partial adds and no-op operations do not.
		// RemoveItemFromInventory and DropItemAt route through RemoveItem, and
		// PickUpDroppedItem routes through AddItem, so those paths do not emit a
		// second notification of their own.
		// ---------------------------------------------------------------

		// Add items to an inventory, filling existing partial stacks first,
		// then new stacks, and stopping when capacity runs out.
		//
		// Returns the number actually added. A partial add is a NORMAL
		// outcome, not an error: refusing the whole pickup because only 3 of
		// 10 fit leaves the player holding nothing while 7 items vanish from
		// the world, which reads as a bug. The caller decides what to do with
		// the remainder.
		int AddItem(Entity entity, const std::string &itemId, int quantity = 1);

		// Remove up to `quantity`. Returns how many were actually removed,
		// which is less than requested when the inventory does not hold
		// enough. A caller consuming an item MUST check this -- treating a
		// short remove as success lets a player use a health pack they never
		// had.
		int RemoveItem(Entity entity, const std::string &itemId, int quantity = 1);

		// Count of one item across all stacks.
		int GetItemCount(Entity entity, const std::string &itemId) const;

		// Does the inventory hold at least this many? Use this rather than
		// `GetItemCount() > 0` before consuming a specific amount.
		bool HasItem(Entity entity, const std::string &itemId, int quantity = 1) const;

		// Look up an item definition anywhere in the scene. Returns nullptr
		// when no entity defines this id -- a misspelled item id is a data
		// error, and the default (MaxStack 1, weight 0) silently masks it.
		//
		ItemDefinitionComponent *FindItemDefinition(const std::string &itemId);

		// Total carried weight, summing each stack's quantity times its
		// definition weight. Items with no definition count as 0.
		float GetInventoryWeight(Entity entity) const;

		// True when neither the weight nor the slot limit is exceeded.
		bool IsInventoryFull(Entity entity) const;

		// ---------------------------------------------------------------
		// Save game support (14.3)
		// ---------------------------------------------------------------

		// Elapsed in-game time in seconds. The save system needs it, but it
		// belongs to no single entity -- the world clock is not the player's
		// and not any prop's -- so Scene holds it. The runtime advances it;
		// scripts may read and set it.
		double GetSavedWorldTime() const { return m_SavedWorldTime; }
		void SetSavedWorldTime(double seconds) { m_SavedWorldTime = seconds; }
		void AdvanceWorldTime(double seconds) { m_SavedWorldTime += seconds; }

		// Remove items from an inventory WITHOUT spawning anything.
		// Returns the quantity actually removed.
		//
		// This used to be called `DropItem`, and it was a lie: the body was
		// `return RemoveItem(...)` with no world entity ever created. A
		// caller who read the name spawned nothing and the items simply
		// ceased to exist -- and it was bound to Python, so a script could
		// hit it from anywhere. The name now says what it does; use
		// DropItemAt below when you mean to actually drop something.
		//
		// Identical in behaviour to RemoveItem, which remains the canonical
		// name. This exists because "take it out of the bag and put it
		// nowhere" is a distinct authoring intent from "consume it", and
		// a script may want to say so at the call site.
		int RemoveItemFromInventory(Entity entity, const std::string &itemId, int quantity = 1);

		// Remove up to `quantity` items from `entity` AND put them into
		// the world: a new entity carrying DroppedItemComponent is
		// created at the carrier's world position, offset by
		// `worldOffset`.
		//
		// Returns the spawned entity -- INVALID when nothing was dropped,
		// which happens for an empty id, a non-positive quantity, an
		// entity with no inventory, or an inventory that does not hold
		// that many. No entity is created in that case, so a caller can
		// test `IsValid()` on the result instead of separately counting
		// what was held before.
		//
		// PARTIAL DROPS ARE NORMAL. Dropping 100 of 7 spawns a pile of
		// 7 and returns it; there is no way to "fail" a drop for
		// holding too little, because the alternative -- refusing and
		// leaving the items in the bag -- is what a player reads as the
		// key not working. The pile's DroppedItemComponent::Quantity is
		// the count ACTUALLY dropped, so a caller that needs to know
		// reads that rather than assuming the request.
		//
		// The spawned entity is an ordinary scene entity: name, transform
		// and DroppedItemComponent, nothing else. No mesh, no collider,
		// no rigid body -- DroppedItemComponent is not a known component
		// type to the physics or render layers, and guessing at geometry
		// here would create entities a caller has to hunt down to
		// configure. Pick it up with PickUpDroppedItem below, or attach
		// whatever visuals the game wants.
		Entity DropItemAt(Entity entity, const std::string &itemId, int quantity = 1,
						   const glm::vec3 &worldOffset = glm::vec3(0.0f, 0.0f, 0.0f));

		// Move a dropped pile into an inventory, and destroy the pile
		// entity when it is fully collected. Returns the number of
		// items actually transferred.
		//
		// Partial pickup keeps the pile: adding 3 of 7 leaves 4 on the
		// ground and the entity alive. A full pickup destroys it, so
		// emptied piles do not accumulate as invisible litter. Returns
		// 0 -- destroying nothing -- when the source has no
		// DroppedItemComponent or the destination has no inventory or no
		// room.
		int PickUpDroppedItem(Entity dropped, Entity carrier);


		// --- Raycast query (for gameplay/weapon/interaction scripts) ---
		// Traces a ray against the Box3D world and reports the closest hit.
		// Returns true on a hit; fills outPoint/outNormal/outDistance and
		// outEntity (invalid when the hit shape has no owning entity).
		//
		// outHitHittable distinguishes "did I hit a shootable" from "did I
		// hit any geometry at all": a weapon script uses it to decide
		// between spawning an impact decal (world) and dealing damage
		// (entity carrying HealthComponent or HittableComponent).
		//
		// maxDistance is clamped to [0, FLT_MAX]; a non-positive value or
		// a zero-length direction returns false rather than shooting a
		// degenerate ray.
		bool Raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance,
					 Entity *outEntity = nullptr, glm::vec3 *outPoint = nullptr,
					 glm::vec3 *outNormal = nullptr, float *outDistance = nullptr,
					 bool *outHitHittable = nullptr) const;

		// Drops any query flags a Raycast would honour for this entity.
		// Cheap early-out used by Raycast's shape -> entity walk.
		bool IsRaycastIgnored(entt::entity handle) const;

		// --- Character movement (player collision) ---
		//
		// Sweeps a capsule against the world and returns the translation that
		// was actually travelled, stopping short of the first obstruction.
		// This is what stops a player walking through walls and lets them
		// slide along them instead of sticking.
		//
		// origin is the capsule's center. radius/halfHeight describe the
		// capsule: halfHeight is the distance between the two hemisphere
		// centers, so the full height is 2*halfHeight + 2*radius.
		//
		// outHit reports whether anything was hit, and outNormal is the
		// surface normal of the blocking contact (zero when nothing was
		// hit). A script uses that to decide what the player is standing on:
		// a normal with y > 0.7 means grounded, and a horizontal normal means
		// a wall blocked this frame.
		//
		// The mover is a ghost: the sweep does not resolve contacts against
		// the player entity itself, so an entity that carries a collider
		// (the player's capsule) never blocks its own movement. Everything
		// else in the world does.
		//
		// Returns the allowed fraction of `translation` as a scale factor in
		// [0, 1] via outFraction, and the corrected vector via outTranslation.
		// Both are optional; with a null world or a zero-length translation
		// the input is returned unchanged and the fraction is 1.
		bool MoveCapsule(const glm::vec3 &origin, float radius, float halfHeight,
						 const glm::vec3 &translation,
						 Entity *outHitEntity = nullptr, glm::vec3 *outNormal = nullptr,
						 float *outFraction = nullptr, glm::vec3 *outTranslation = nullptr) const;

		// Walks the collider components of every entity to find which one
		// owns a Box3D shape id. Returns an invalid Entity when unknown.
		// Shape ids are 1:1 with collider components, so this is a linear
		// scan; fine at scene sizes where a per-ray cost like this is noise.
		Entity FindEntityByShape(uint64_t shapeId) const;

		template <typename... T>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<T...>();
		}

		// =====================================================================
		// Audio (14.4)
		// =====================================================================

		// The scene's audio system, created with the scene. Lazily constructed
		// on first use so that a scene nobody plays sound on never allocates a
		// mixer, and so the dependency is visible at the call site rather than
		// requiring every consumer to remember an InitAudio() step.
		Ref<Audio::AudioSystem> GetAudioSystem();

		// Convenience wrappers. Gameplay overwhelmingly wants "play this
		// sound", not a PlayRequest it must fill in, so the common cases are
		// one call and the raw mixer stays available for the rest.
		//
		// All return the voice id, or -1 if nothing started. See
		// Mixer::Play for when -1 is the correct answer rather than a failure.
		int PlaySound(const std::string &clipName);
		int PlaySoundAt(const std::string &clipName, const glm::vec3 &position,
						float volume = 1.0f, float pitch = 1.0f);
		int PlayEntitySound(Entity entity);
		void StopSound(const std::string &clipName, float fadeSeconds = 0.0f);
		void StopAllSounds(float fadeSeconds = 0.0f);

		void SetMasterVolume(float volume);
		float GetMasterVolume() const;

		// Load and register a clip by path. The registered name is the file's
		// stem, so scripts refer to "shotgun_fire" rather than to an asset path.
		bool LoadAudioClip(const std::string &path);

	private:
		template <typename T>
		void OnComponentAdded(Entity entity, T &component);

		void OnPhysics2DStart();
		void OnPhysics2DStop();

		void OnPhysics3DStart();
		void OnPhysics3DStop();

		// The live b3BodyId behind an entity's RigidBody3DComponent, or
		// b3_nullBodyId when there is nothing to drive. Shared by
		// ApplyImpulse / SetLinearVelocity / GetLinearVelocity so all three
		// resolve a body the same way; private because a raw handle is not
		// something a caller outside this class should be holding.
				b3BodyId ResolveBody3D(Entity entity) const;

		void RenderScene(EditorCamera &camera);

	// Gathers VolumetricFog/VolumetricClouds components and submits them to
	// Renderer3D. Called between BeginScene/EndScene at every 3D render site.
	void SubmitVolumetricVolumes();

	// Gathers analytic light components and submits them to Renderer3D.
	// Called alongside SubmitVolumetricVolumes at every 3D render site.
	void SubmitSceneLights();

	// Advances every WaterComponent's Time accumulator. Called at the top of
	// each update path so editor previews animate and runtime simulates.
	void UpdateWater(Timestep ts);
	// Draws all enabled water surfaces via Renderer3D::DrawWaterPlane.
	// Called between BeginScene/EndScene at every 3D render site.
	void SubmitWaterSurfaces();

	// Draws every Blender "Add > Mesh" primitive (plane, grid, circle, ico
	// sphere, cylinder, cone, torus) via the matching Renderer3D::Draw*.
	// Called alongside SubmitWaterSurfaces at both 3D render sites so the
	// runtime and editor paths stay in sync.
	void SubmitMeshPrimitives();
	// Submits every loaded streamed terrain chunk to the renderer once per frame.
	void SubmitTerrainChunks();

	private:
		Ref<Audio::AudioSystem> m_AudioSystem;
		entt::registry m_Registry;

		// Elapsed in-game time in seconds, advanced by the runtime and
		// persisted by the save system. Separate from any physics
		// accumulator on purpose: world time is game state the player would
		// miss losing (day/night phase, door timers), physics time is not.
		//
		// Kept here rather than on an entity because the world clock belongs
		// to no single entity -- it is not the player's and not any prop's.
		double m_SavedWorldTime = 0.0;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		b2World *m_PhysicsWorld = nullptr;
		b3WorldId m_Physics3DWorld = B3_NULL_ID;

		// Baked navmesh, shared by every tree in this scene. Null until
		// BakeNavMesh runs; path queries then fail and agents fall back to
		// direct movement rather than following a stale route.
		std::unique_ptr<Navigation::NavMesh> m_NavMesh;
		Terrain::TerrainRuntime m_TerrainRuntime;
		std::string m_TerrainAssetReference;
		bool m_NeuralStageEnabled = true;

		friend class Entity;
		friend class SceneSerializer;
		// The binary sidecar walks the registry to snapshot entities and repopulates it on
		// load. See SceneChunkStore.h for the safety rules that keep it from ever dropping
		// data; it deliberately reaches the registry directly rather than through a public
		// accessor, because nothing else in the engine should be bulk-writing entities.
		friend class SceneChunkStore;
		// SaveGame resolves saved UUIDs against the live registry to restore
		// state onto the level's own entities rather than cloning it, so it
		// needs the same registry access SceneSerializer has.
		friend class SaveGame;
		friend class SceneHierarchyPanel;
	};

}