#pragma once

#include "Scene.h"
#include "SceneSerializer.h"
#include "Entity.h"

// Forward-declared so SaveGame's private signatures can name them without
// pulling yaml-cpp into every consumer of this header. WastelandPython
// compiles Bindings.cpp (which includes this) and has no yaml-cpp include
// path; only the engine project, which compiles SaveGame.cpp, needs the
// definitions.
//
// These MUST be at global scope. Declared inside `namespace Wasteland` they
// become Wasteland::YAML, a DIFFERENT namespace from ::YAML, and every
// YAML::Key / YAML::BeginMap in SaveGame.cpp then fails to resolve.
namespace YAML
{
	class Emitter;
	class Node;
}

namespace Wasteland
{



	// A save file is NOT a scene file.
	//
	// A .wastescene is authored content: it ships with the game and must never
	// be modified by play. A save is mutable per-playthrough state, keyed by
	// slot, and is allowed to be missing fields that a save did not know about.
	// Reusing the scene format for both is the mistake that eventually makes
	// someone save over the level design.
	//
	// Design decisions worth knowing before changing anything here:
	//
	// - Only entities carrying PersistentComponent are written. A level with
	//   a thousand props should not produce a thousand saved props.
	// - Runtime handles (RigidBody3DComponent::RuntimeBody and friends) are
	//   NEVER written. They are process-local Box3D identifiers; restoring a
	//   stale one points a body at memory that has since been freed.
	// - Transient component timers are never written, for the same reason a
	//   half-elapsed muzzle flash is not written: the effect would replay for
	//   a shot the player never took.
	// - The schema version is checked and a mismatch FAILS LOUDLY. Loading a
	//   half-understood save is worse than refusing it -- a player who
	//   discovers their progress silently vanished has no recourse, whereas a
	//   clear "this save is from a newer version" is actionable.
	class SaveGame
	{
	public:
		// Bump when the on-disk shape changes incompatibly. Old saves then
		// fail loudly rather than loading half-correct.
		//
		// 1: initial layout (schema, slot, world time, entity records with
		//    transform/health/weapon/inventory/quest-flags/world-timers).
		static constexpr int kCurrentSchemaVersion = 1;

		SaveGame(const Ref<Scene> &scene);
		~SaveGame() = default;

		// Write a save. `slot` is a player-facing label ("slot1"), NOT a
		// path: the extension and directory are this class's business, so a
		// caller cannot accidentally write a save over a scene by passing a
		// .wastescene path.
		bool Save(const std::string &slot);

		// Read a save back. Returns false for a missing file, a malformed
		// file, or an incompatible schema version; `outError` explains which.
		//
		// Deliberately does NOT clear the scene first. A failed load must
		// leave the running game exactly as it was -- a load that wiped the
		// world and then hit a bad version would turn "unsupported save" into
		// "corrupt session".
		bool Load(const std::string &slot, std::string *outError = nullptr);

		// Does a save exist for this slot, and what version is it?
		bool SlotExists(const std::string &slot, int *outSchemaVersion = nullptr) const;

		// Absolute path this slot maps to, for display and for tests.
		std::string GetSlotPath(const std::string &slot) const;

		// Elapsed in-game time in seconds, accumulated across the session.
		// Saved separately from any component because it belongs to the world,
		// not to an entity.
		double GetWorldTime() const { return m_WorldTime; }
		void SetWorldTime(double seconds) { m_WorldTime = seconds; }

		// Number of persistent entities the last Save wrote, or the last Load
		// read. Diagnostics and tests.
		int GetLastEntityCount() const { return m_LastEntityCount; }

		// Schema version of the most recent successful Load, for tests that
		// need to confirm a rejection rather than a silent success.
		int GetLoadedSchemaVersion() const { return m_LoadedSchemaVersion; }

		// Directory saves live in. Overridable so tests can redirect without
		// touching the real save folder.
		void SetSaveDirectory(const std::string &directory) { m_SaveDirectory = directory; }
		const std::string &GetSaveDirectory() const { return m_SaveDirectory; }

	private:
		// Write one entity's persistent state.
		//
		// YAML::Emitter is forward-declared at NAMESPACE scope rather than
		// included, so this header does not drag yaml-cpp into every
		// translation unit that constructs a SaveGame -- notably
		// WastelandPython, which compiles Bindings.cpp directly and has no
		// yaml-cpp include path. The engine project, which does, is the only
		// thing that needs the full definition.
		void WriteEntity(YAML::Emitter &out, Entity entity) const;

		// Read one entity's persistent state into an existing entity. The
		// entity is looked up by UUID, so a save restores the state onto the
		// level's own objects rather than cloning the level.
		bool ReadEntity(const YAML::Node &node, std::string *outError);

		// Resolve a saved UUID to a live entity. Returns false when the level
		// no longer contains it, which is a legitimate case (a designer
		// removed a crate between versions), not an error to abort on.
		Entity FindEntityByUUID(const UUID &uuid) const;

		Ref<Scene> m_Scene;
		std::string m_SaveDirectory;
		double m_WorldTime = 0.0;
		int m_LastEntityCount = 0;
		int m_LoadedSchemaVersion = 0;
	};

}
