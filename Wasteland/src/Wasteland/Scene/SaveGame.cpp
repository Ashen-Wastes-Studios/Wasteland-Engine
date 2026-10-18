#include "wlpch.h"

#include "SaveGame.h"

// yaml-cpp is included HERE, in the .cpp, rather than in the header.
// SaveGame.h only forward-declares YAML::Emitter and YAML::Node at global
// scope, which is what keeps this header out of WastelandPython's include
// path -- see the rationale comment in SaveGame.h.
// Consequence: every method touching YAML must live in this file.
#include <yaml-cpp/yaml.h>

// <filesystem> for the path arithmetic in GetSlotPath and the remove_all
// calls in main's cleanup paths; <fstream> for the ofstream/ifstream pairs;
// <sstream> for the stringstream that funnels a save file into YAML::Load.
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Wasteland
{

	// Anonymous namespace: these two helpers have no external contract, so
	// internal linkage keeps them out of the module's symbol table. They must be
	// defined here rather than in the header because they are the only places
	// that need to know UUID's textual form.
	namespace
	{
		// The scene serializer writes a UUID as plain decimal (via the fmt
		// formatter in UUID.h), and this mirrors it exactly. Round-tripping through
		// one representation is what keeps a save's entity ids comparable with the
		// ids in the scene file the save was taken from.
		// A UUID as a stable string. The scene serializer writes the raw
		// integer; saves match entities by this string so a save stays
		// readable and order-independent.
		std::string UUIDToString(const UUID &uuid)
		{
			// std::to_string on a UUID works because UUID has an implicit conversion to
			// uint64_t -- an implicit narrowing footgun in general, but exactly what is
			// wanted here.
			return std::to_string(uuid);
		}

		UUID StringToUUID(const std::string &text)
		{
			// std::stoull THROWS std::invalid_argument on a non-numeric string and
			// std::out_of_range on overflow. DEFECT: none of that is guarded here or at
			// the single call site in ReadEntity, so a hand-edited or truncated save with
			// a non-numeric UUID propagates an exception out of Load -- which its own
			// header promises never happens ("returns false for a malformed file").
			// A try/catch, or std::from_chars, would close that hole.
			return static_cast<UUID>(std::stoull(text));
		}
	}

	// Holds the scene by Ref, so the save outlives no scene. Load() and Save()
	// both bail on a null m_Scene rather than dereferencing it.
	SaveGame::SaveGame(const Ref<Scene> &scene)
		: m_Scene(scene)
	{
		// A RELATIVE path, resolved against the process's working directory.
		// DEFECT / risk: the comment's stated intent ("next to the executable")
		// does not match what a relative string does. It is CWD-relative, so the
		// same save lands in a different place depending on where the game was
		// launched from. It is still the right choice over putting saves in the
		// asset tree -- a Steam-style install would make saving impossible -- but an
		// absolute, platform-known user-data path would deliver both properties.
		// Default next to the executable, matching where the editor already
		// writes its own Wasteland.log. A save is per-user data, so it must
		// NOT live in the game's asset tree -- a Steam-style install or a
		// read-only program directory would make saving impossible.
		m_SaveDirectory = "saves";
	}

	// Maps a caller-supplied slot label to an absolute file path. Single funnel
	// for path construction: Save, Load and SlotExists all go through it, so
	// sanitisation cannot be bypassed by adding a caller.
	std::string SaveGame::GetSlotPath(const std::string &slot) const
	{
		// Built from m_SaveDirectory, which SetSaveDirectory lets a test redirect.
		std::filesystem::path dir(m_SaveDirectory);

		// DEFECT (incomplete sanitisation): this replaces path separators and the
		// Windows-illegal character set, which stops a slot named "../../startup"
		// from escaping -- the traversal characters are gone before the path is ever
		// joined. But '.' is NOT stripped, so a slot named ".." survives as a path
		// component and resolves to the PARENT of the save directory.
		// The slot name comes from a caller and may be a user-visible label,
		// so it is sanitised before touching the filesystem. Without this a
		// slot named "../../startup" writes outside the save directory.
		std::string safe = slot;
		for (char &c : safe)
		{
			// The character set replaced: both separators, the Windows-illegal set
			// (<>:"|?*), and the double quote. Note this leaves '.', whitespace, and
			// trailing dots/spaces untouched -- all of which Windows rejects in a file
			// name, so such a slot fails at fopen time rather than being reported as a
			// bad slot name.
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|')
				c = '_';
		}
		// An empty label (or one made entirely of illegal characters, which sanitise
		// to empty) becomes a usable default rather than producing "saves/.wlsave".
		if (safe.empty())
			safe = "default";

		// The extension is this class's business, not the caller's -- see the
		// header's note that a slot is a label, not a path, so a caller cannot
		// accidentally save over a scene.
		return (dir / (safe + ".wlsave")).string();
	}

	// UUID -> live entity. This is the mechanism that makes a save restore onto
	// the level's own objects instead of cloning the level: identity is matched,
	// geometry is not.
	Entity SaveGame::FindEntityByUUID(const UUID &uuid) const
	{
		// Null-scene guard, returning a default (invalid) Entity. Callers test
		// IsValid() on the result rather than a bool.
		if (!m_Scene)
			return {};

		// Direct access to the registry: SaveGame is a friend of Scene (Scene.h
		// lists it), so private m_Registry is reachable without widening Scene's
		// API for this one consumer.
		auto view = m_Scene->m_Registry.view<IDComponent>();
		// Linear scan per lookup. Fine at the scale this runs (once per saved
		// entity, once per load) -- NOT a frame-time path. NOTE: Save() iterates
		// persistent entities and Load() resolves each by UUID, so a full save/load
		// round trip is O(persistent entities squared). Fine for hundreds of
		// entities; it would need an index if that number grew by orders of
		// magnitude.
		for (auto e : view)
		{
			// Value equality on IDComponent::ID, which is read-only by convention
			// (see Components.h) precisely because handles and serialized references are
			// keyed on it.
			if (view.get<IDComponent>(e).ID == uuid)
				return Entity(e, m_Scene.get());
		}
		return {};
	}

	// Writes one entity's persistent state into an already-open map. Const
	// because saving observes the world and must not modify it.
	void SaveGame::WriteEntity(YAML::Emitter &out, Entity entity) const
	{
		// The map is opened here and closed by the caller (Save), so several
		// entities are emitted as siblings under one 'Entities' sequence instead of
		// each getting its own document.
		out << YAML::BeginMap;

		// UUID first, and Name second. The UUID is the ONLY field a load actually
		// requires -- ReadEntity rejects a record without it -- so it leads the
		// record for readability.
		out << YAML::Key << "UUID" << YAML::Value << UUIDToString(entity.GetUUID());
		out << YAML::Key << "Name" << YAML::Value << entity.GetName();

		// ---- Transform ----
		// Saved because the player moves and enemies die where they died.
		// Without this a reload puts everyone back at their authored spawn.
		// GetComponent, not a checked variant: every persistent entity reaching
		// this function was already filtered to have a TransformComponent by the
		// loop in Save(), so the assumption holds by construction.
		auto &t = entity.GetComponent<TransformComponent>();
		// Position/Rotation/Scale as three-element sequences rather than a nested
		// map, so the reader can index [0..2] positionally (see ReadEntity).
		// Rotation is written as raw floats; the on-disk form carries no unit or
		// order convention beyond this.
		out << YAML::Key << "Position" << YAML::Value << YAML::BeginSeq
			<< t.Translation.x << t.Translation.y << t.Translation.z
			<< YAML::EndSeq;
		out << YAML::Key << "Rotation" << YAML::Value << YAML::BeginSeq
			<< t.Rotation.x << t.Rotation.y << t.Rotation.z
			<< YAML::EndSeq;
		out << YAML::Key << "Scale" << YAML::Value << YAML::BeginSeq
			<< t.Scale.x << t.Scale.y << t.Scale.z
			<< YAML::EndSeq;

		// The Health block is emitted only when the entity HAS a HealthComponent.
		// Save files are therefore sparse by design: a prop with no health produces
		// no Health key at all, and ReadEntity's component checks make that safe.
		// ---- Health ----
		//
		// IsDead and Current are saved; CooldownTimer is NOT. A save taken
		// mid-invulnerability must not restore the invulnerability, or the
		// player loads already immune to a hit that was seconds ago.
		// The guard, not an unconditional write. This is what makes one save file
		// loadable into a level whose components have since changed.
		if (entity.HasComponent<HealthComponent>())
		{
			auto &hc = entity.GetComponent<HealthComponent>();
			// Map opened with Key but no explicit Value marker -- yaml-cpp tracks the
			// Key/Value state machine itself, so BeginMap here correctly starts the
			// nested map's content.
			out << YAML::Key << "Health" << YAML::BeginMap;
			// Current, Max and IsDead only. See the note above about CooldownTimer.
			// Writing Max alongside Current matters: a balance change between versions
			// must not retroactively clamp a loaded save's health to a new maximum.
			out << YAML::Key << "Current" << YAML::Value << hc.Current;
			out << YAML::Key << "Max" << YAML::Value << hc.Max;
			out << YAML::Key << "IsDead" << YAML::Value << hc.IsDead;
			out << YAML::EndMap;
		}

		// Weapon state, same sparse-by-component shape as Health.
		// ---- Weapon ----
		//
		// Ammo counts are saved (progress). FireCooldown, ReloadTimer,
		// IsReloading and ShotsFired are NOT: reloading a gun that was
		// mid-reload at save time is a plausible restore, but restoring a
		// half-elapsed FIRE COOLDOWN would silently eat the player's first
		// shot after load with no way to tell why.
		if (entity.HasComponent<WeaponComponent>())
		{
			auto &wc = entity.GetComponent<WeaponComponent>();
			out << YAML::Key << "Weapon" << YAML::BeginMap;
			// Three ammo counts, all player progress. The transient weapon fields are
			// deliberately omitted -- see the block comment.
			out << YAML::Key << "CurrentAmmo" << YAML::Value << wc.CurrentAmmo;
			out << YAML::Key << "MaxAmmo" << YAML::Value << wc.MaxAmmo;
			out << YAML::Key << "ReserveAmmo" << YAML::Value << wc.ReserveAmmo;
			out << YAML::EndMap;
		}

		// Inventory. MaxWeight and MaxSlots are capacities (design), Items is the
		// actual carried contents; all three are restored on load.
		// ---- Inventory ----
		if (entity.HasComponent<InventoryComponent>())
		{
			auto &inv = entity.GetComponent<InventoryComponent>();
			out << YAML::Key << "Inventory" << YAML::BeginMap;
			out << YAML::Key << "MaxWeight" << YAML::Value << inv.MaxWeight;
			out << YAML::Key << "MaxSlots" << YAML::Value << inv.MaxSlots;
			// Items is a sequence of one-key maps rather than a compact array form,
			// which keeps it human-editable in a save file -- the trade-off is more
			// bytes per stack in exchange for readability.
			out << YAML::Key << "Items" << YAML::Value << YAML::BeginSeq;
			// Iterated in the component's own order, which is preserved on load, so a
			// hotbar-like ordering survives a round trip.
			for (const auto &stack : inv.Items)
			{
				out << YAML::Value << YAML::BeginMap;
				out << YAML::Key << "ItemId" << YAML::Value << stack.ItemId;
				out << YAML::Key << "Quantity" << YAML::Value << stack.Quantity;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}

		// Quest flags as a flat map of string->string. The values are stored as
		// strings (QuestFlagsComponent::Flags is a vector of <string,string> pairs),
		// so a numeric or bool flag is stringified in the file; ReadEntity reads
		// them back with .as<std::string>(), which yaml-cpp will convert from a
		// scalar for us.
		// ---- Quest flags ----
		// Only entities that actually carry quest state get a QuestFlags key.
		if (entity.HasComponent<QuestFlagsComponent>())
		{
			auto &qf = entity.GetComponent<QuestFlagsComponent>();
			out << YAML::Key << "QuestFlags" << YAML::Value << YAML::BeginMap;
			// Written as a YAML map rather than a list of pairs. NOTE: map key order is
			// NOT preserved by YAML, so the vector's order does not survive a round
			// trip -- fine for flags, which are looked up by name (see Components.h),
			// but it means this is not a faithful restore of the vector.
			for (const auto &kv : qf.Flags)
				out << YAML::Key << kv.first << YAML::Value << kv.second;
			out << YAML::EndMap;
		}

		// World timers are per-entity elapsed-time accumulators, distinct from
		// SaveGame::m_WorldTime which is world-scoped and written separately.
		// ---- World timers ----
		if (entity.HasComponent<WorldTimerComponent>())
		{
			auto &wt = entity.GetComponent<WorldTimerComponent>();
			out << YAML::Key << "WorldTimers" << YAML::Value << YAML::BeginMap;
			for (const auto &kv : wt.Timers)
				out << YAML::Key << kv.first << YAML::Value << kv.second;
			// Closes the WorldTimers map (line 166 closes the entity record itself).
			// Both are bare EndMap calls -- yaml-cpp tracks nesting internally, so the
			// writer never keeps a depth counter of its own.
			out << YAML::EndMap;
		}

		out << YAML::EndMap;
	}

	// Save. Returns false rather than throwing for every failure mode, and logs
	// the reason -- the header's contract is that a caller can check one bool.
	bool SaveGame::Save(const std::string &slot)
	{
		// A SaveGame with no scene cannot serialise anything. Returns false with NO
		// log line, unlike the failures below: this is a programming error (a
		// SaveGame constructed without a scene), not a runtime condition.
		if (!m_Scene)
			return false;

		// error_code overload: nothing in Save throws. A failing create_directories
		// means the subsequent ofstream will fail too, and would do so with a much
		// less informative message.
		std::error_code ec;
		std::filesystem::create_directories(m_SaveDirectory, ec);
		// NOT swallowing this error is deliberate and load-bearing. Silently
		// writing into a directory that was never created produces a save that
		// reports success and is not on disk -- the worst possible combination for
		// a player who then quits.
		// A non-empty ec here is worth surfacing, not swallowing: silently
		// writing to a directory that did not get created produces a save
		// that reports success and is not there.
		if (ec)
		{
			// The log names both the directory and the OS error, so a permissions
			// problem is distinguishable from a bad path without guessing.
			WL_CORE_ERROR("SaveGame: could not create save directory '{0}': {1}",
						  m_SaveDirectory, ec.message());
			return false;
		}

		// The emitter is a local, not a member: it holds the document under
		// construction and is meaningless once the bytes are written.
		YAML::Emitter out;
		out << YAML::BeginMap;

		// SchemaVersion FIRST. Load() checks it before touching anything else, so a
		// file whose shape we do not understand is rejected before its bytes are
		// trusted -- see the ordering in Load.
		// The schema version is the FIRST key so a reader can check it
		// before interpreting anything else. A save whose shape changed must
		// be rejected before its bytes are trusted.
		// Slot and WorldTime are scalar top-level keys. WorldTime is written here
		// rather than per entity because it belongs to the world, not to anything
		// in it (see the accessor comment in SaveGame.h).
		out << YAML::Key << "SchemaVersion" << YAML::Value << kCurrentSchemaVersion;
		out << YAML::Key << "Slot" << YAML::Value << slot;
		out << YAML::Key << "WorldTime" << YAML::Value << m_WorldTime;

		// Only entities carrying PersistentComponent are written. A level with a
		// thousand props does not produce a thousand saved props.
		out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
		int written = 0;
		// Same friend-registry access as FindEntityByUUID. A single view built once
		// and reused for the whole traversal.
		auto view = m_Scene->m_Registry.view<PersistentComponent>();
		// Range-for over the view. Entities are handed to WriteEntity as a wrapper
		// type, not as a raw entt handle, because WriteEntity calls the component
		// accessor API on them.
		for (auto e : view)
		{
			Entity entity(e, m_Scene.get());
			// A persistent entity still needs a transform to be placeable.
			// A persistent entity with no transform is SKIPPED rather than written
			// without one. It could not be placed on load, so writing it would produce a
			// record that silently does nothing.
			// Note this skip is silent -- no log line -- unlike the 'entity missing
			// from the level' case on load, which warns.
			if (!entity.HasComponent<TransformComponent>())
				continue;
			// The count is incremented alongside the write, not derived afterwards, so
			// it reflects entities actually serialised rather than entities visited.
			WriteEntity(out, entity);
			written++;
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;

		// Path construction goes through GetSlotPath, so the slot label is sanitised
		// on the way out exactly as it is on the way back in.
		const std::string path = GetSlotPath(slot);
		// binary|trunc: trunc so a shorter document cannot leave a tail of the
		// previous save behind (a YAML file with two documents parses as an error or
		// as the wrong one). binary so no newline translation can alter the bytes.
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
		{
			// is_open is the only reliable test after construction; the stream is not
			// usable until it succeeds.
			WL_CORE_ERROR("SaveGame: could not open '{0}' for writing", path);
			return false;
		}
		// c_str(), not operator<< on the emitter: yaml-cpp's Emitter writes its
		// buffer through a const char* accessor here rather than streaming into an
		// ostream.
		stream << out.c_str();
		// close() BEFORE the state check below. That ordering is the whole point:
		// buffered writes are only flushed at close, so checking the stream while
		// it is still open would report success for a disk that is full.
		stream.close();

		// Post-close failure check. C++ streams keep their failbit set after a
		// failed flush or close, which is how a full disk or a disconnected drive is
		// caught rather than reported as a successful save.
		if (!stream)
		{
			WL_CORE_ERROR("SaveGame: write to '{0}' failed", path);
			return false;
		}

		// Diagnostics only (the header calls this out as such), updated only on the
		// success path so it never advertises entities from a save that failed.
		m_LastEntityCount = written;
		WL_CORE_INFO("SaveGame: wrote slot '{0}' ({1} entities) to {2}",
					 slot, written, path);
		return true;
	}

	// Existence plus optional version probe, for a load menu that wants to warn
	// about an incompatible slot without paying for a full parse.
	bool SaveGame::SlotExists(const std::string &slot, int *outSchemaVersion) const
	{
		// Read-only open. The existence check is the open itself -- there is no
		// separate stat, which saves a TOCTOU window on a network share.
		const std::string path = GetSlotPath(slot);
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		// Early out when the caller only wants existence: parsing is skipped
		// entirely, so a corrupt file still reports 'exists' here and the caller
		// learns the version only if it asked.
		if (!outSchemaVersion)
			return true;

		// Read just the version, tolerating a corrupt file: an unreadable
		// The version is read defensively. A corrupt file should report
		// 'exists, version unknown' rather than propagate a parse error to a menu
		// that is merely listing saves.
		// save should report "exists, version unknown" rather than crash on
		// a parse error.
		// Zeroed first, so a caller that inspects *outSchemaVersion after a parse
		// failure sees a defined value rather than whatever it initialised.
		// NOTE: 0 is indistinguishable from a file that genuinely declares version 0
		// -- both mean 'unknown' to every caller in this file.
		*outSchemaVersion = 0;
		try
		{
			// The whole stream is handed to YAML::Load; there is no streaming parse, so
			// an arbitrarily large save is read into memory at once.
			YAML::Node data = YAML::Load(stream);
			if (data["SchemaVersion"])
				// Boolean test on the node: yaml-cpp nodes are truthy when defined, so this
				// is the 'key is present' idiom rather than a comparison to nil.
				*outSchemaVersion = data["SchemaVersion"].as<int>();
		}
		// catch(...) swallows everything, including the std::bad_alloc a huge
		// corrupt file could raise. Broad by design -- the alternative is letting a
		// save-menu enumeration take the process down.
		catch (...)
		{
			*outSchemaVersion = 0;
		}
		return true;
	}

	// Read side. Note it is NOT const (unlike WriteEntity): loading mutates the
	// live scene, so applying a save is an observable change to the world.
	bool SaveGame::ReadEntity(const YAML::Node &node, std::string *outError)
	{
		// A record with no UUID cannot be matched to anything, so it is a hard
		// failure -- unlike a UUID that simply is not in this level, below.
		if (!node["UUID"])
		{
			if (outError)
				*outError = "entity record has no UUID";
			return false;
		}

		// The unguarded stoull from the anonymous namespace. DEFECT: as noted there,
		// a non-numeric UUID throws std::invalid_argument straight out of here and
		// out of Load, despite the header promising false-on-malformed.
		UUID uuid = StringToUUID(node["UUID"].as<std::string>());
		Entity entity = FindEntityByUUID(uuid);

		// 'No entity with this id' is NOT an error. The level changed between the
		// save being written and being loaded -- a designer deleted a crate -- and
		// refusing the whole save for that would be far worse than losing one crate.
		if (!entity.IsValid())
		{
			// The level changed since this save was written. Skipping is the
			// right call: a designer deleting a crate should not make every
			// save un-loadable. Log it so the loss is visible.
			// Warn, not error, so the loss is visible in the log without implying the
			// load failed. The ternary tolerates a record that also lacks a Name.
			WL_CORE_WARN("SaveGame: entity {0} ('{1}') is not in this level; skipping",
						 uuid, node["Name"] ? node["Name"].as<std::string>() : "?");
			return true;
		}

		// Every restore is guarded twice: the key must exist in the file AND the
		// entity must still have the component. The second half is what makes an old
		// save loadable into a level where a component was since removed.
		if (node["Position"] && entity.HasComponent<TransformComponent>())
		{
			auto &t = entity.GetComponent<TransformComponent>();
			// Positional indexing into the sequence written by WriteEntity. NOTE: a
			// sequence shorter than three elements is read as undefined nodes and
			// .as<float>() THROWS on those -- a truncated save would escape as an
			// exception rather than a reported error.
			t.Translation = glm::vec3(
				node["Position"][0].as<float>(),
				node["Position"][1].as<float>(),
				node["Position"][2].as<float>());
		}
		if (node["Rotation"] && entity.HasComponent<TransformComponent>())
		{
			auto &t = entity.GetComponent<TransformComponent>();
			t.Rotation = glm::vec3(
				node["Rotation"][0].as<float>(),
				node["Rotation"][1].as<float>(),
				node["Rotation"][2].as<float>());
		}
		if (node["Scale"] && entity.HasComponent<TransformComponent>())
		{
			auto &t = entity.GetComponent<TransformComponent>();
			t.Scale = glm::vec3(
				node["Scale"][0].as<float>(),
				node["Scale"][1].as<float>(),
				node["Scale"][2].as<float>());
		}

		// Health restore. Individual keys are guarded individually so a save that
		// predates the 'Max' key still restores Current rather than being rejected
		// wholesale.
		if (node["Health"] && entity.HasComponent<HealthComponent>())
		{
			auto &hc = entity.GetComponent<HealthComponent>();
			if (node["Health"]["Current"])
				hc.Current = node["Health"]["Current"].as<float>();
			if (node["Health"]["Max"])
				hc.Max = node["Health"]["Max"].as<float>();
			if (node["Health"]["IsDead"])
				hc.IsDead = node["Health"]["IsDead"].as<bool>();
			// CooldownTimer is reset, never restored: a load must not inherit
			// an invulnerability window from before the save.
			// CooldownTimer is ZEROED, not restored. A load must not inherit an
			// invulnerability window earned before the save -- the player would load
			// already immune to a hit that happened seconds ago and would have no way
			// to tell why.
			hc.CooldownTimer = 0.0f;
		}

		// Weapon restore, same per-key guarding as Health above.
		if (node["Weapon"] && entity.HasComponent<WeaponComponent>())
		{
			auto &wc = entity.GetComponent<WeaponComponent>();
			if (node["Weapon"]["CurrentAmmo"])
				wc.CurrentAmmo = node["Weapon"]["CurrentAmmo"].as<int>();
			if (node["Weapon"]["MaxAmmo"])
				wc.MaxAmmo = node["Weapon"]["MaxAmmo"].as<int>();
			if (node["Weapon"]["ReserveAmmo"])
				wc.ReserveAmmo = node["Weapon"]["ReserveAmmo"].as<int>();
			// A half-finished reload is deliberately discarded rather than
			// resumed. Resuming would need the remaining time saved, and a
			// restore that silently completes a reload the player never
			// finished is more surprising than one that does not.
			// Three fields deliberately reset rather than restored. IsReloading and
			// ReloadTimer would need the remaining time persisted to resume correctly;
			// FireCooldown would silently eat the first shot after load. Resetting is
			// the surprising-but-honest choice, and the comment above it says why.
			wc.IsReloading = false;
			wc.ReloadTimer = 0.0f;
			wc.FireCooldown = 0.0f;
		}

		// Inventory restore. Capacity is applied first, then the contents are
		// REPLACED rather than merged -- a save is a snapshot, so an item picked up
		// since the save is intentionally gone.
		if (node["Inventory"] && entity.HasComponent<InventoryComponent>())
		{
			auto &inv = entity.GetComponent<InventoryComponent>();
			if (node["Inventory"]["MaxWeight"])
				inv.MaxWeight = node["Inventory"]["MaxWeight"].as<float>();
			if (node["Inventory"]["MaxSlots"])
				inv.MaxSlots = node["Inventory"]["MaxSlots"].as<int>();

			// clear() before repopulating is what makes this a snapshot restore. Without
			// it, stacks added since the save would survive alongside the restored ones.
			inv.Items.clear();
			if (node["Inventory"]["Items"])
			{
				for (const auto &stackNode : node["Inventory"]["Items"])
				{
					// A stack with no ItemId is skipped: it cannot be resolved to an item
					// definition, and keeping it would produce an inventory entry that no item
					// database matches.
					if (!stackNode["ItemId"])
						continue;
					ItemStack stack;
					stack.ItemId = stackNode["ItemId"].as<std::string>();
					// Quantity defaults to 1 when the key is absent, so a hand-edited stack that
					// omits it is still usable rather than rejected.
					stack.Quantity = stackNode["Quantity"] ? stackNode["Quantity"].as<int>() : 1;
					// IsValid() is Components.h's check (non-empty ItemId AND Quantity > 0), so
					// the validity rules live with the type rather than being restated here.
					// DEFECT: an invalid stack is dropped SILENTLY -- no log line, unlike the
					// missing-entity case -- so a corrupt stack disappears with no trace.
					if (stack.IsValid())
						inv.Items.push_back(stack);
				}
			}
		}

		// Flags are cleared then refilled, same snapshot semantics as Inventory.
		if (node["QuestFlags"] && entity.HasComponent<QuestFlagsComponent>())
		{
			auto &qf = entity.GetComponent<QuestFlagsComponent>();
			qf.Flags.clear();
			for (const auto &kv : node["QuestFlags"])
				// .as<std::string>() on both halves. yaml-cpp converts a scalar of any type
				// to string, so this tolerates a hand-edited file where the value was
				// written unquoted -- but a non-scalar (a nested map) would throw here.
				qf.Flags.emplace_back(kv.first.as<std::string>(), kv.second.as<std::string>());
		}

		// World timers: same clear-and-refill shape, with values as double.
		if (node["WorldTimers"] && entity.HasComponent<WorldTimerComponent>())
		{
			auto &wt = entity.GetComponent<WorldTimerComponent>();
			wt.Timers.clear();
			for (const auto &kv : node["WorldTimers"])
				wt.Timers.emplace_back(kv.first.as<std::string>(), kv.second.as<double>());
		}

		return true;
	}

	// Load. Deliberately does NOT clear the scene first (see the header): a
	// failed load must leave the running game exactly as it was.
	bool SaveGame::Load(const std::string &slot, std::string *outError)
	{
		// The `fail` lambda is the single error path: it sets outError when the
		// caller asked for one, logs unconditionally, and returns false. Centralising
		// it means no branch can forget either half.
		auto fail = [&](const std::string &message) {
			if (outError)
				*outError = message;
			WL_CORE_ERROR("SaveGame: load failed -- {0}", message);
			return false;
		};

		// No scene: reported through fail(), so the caller gets a reason, not just
		// a bare false.
		if (!m_Scene)
			return fail("no scene");

		const std::string path = GetSlotPath(slot);
		// Path and open both come from GetSlotPath, so a slot that cannot be opened
		// and a slot that does not exist produce the same message with the path
		// included -- enough to debug without a second lookup.
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return fail("no save in slot '" + slot + "' (" + path + ")");

		YAML::Node data;
		// Only the YAML parse is inside the try. Every .as<T>() below is OUTSIDE
		// it, so a structurally valid but wrongly-typed value (a string where a
		// sequence is expected, say) throws rather than returning a reason.
		try
		{
			std::stringstream buffer;
			buffer << stream.rdbuf();
			// Funnelled through a stringstream rather than parsing the stream directly,
			// which keeps the read and the parse separable -- and lets the catch below
			// own both.
			data = YAML::Load(buffer.str());
		}
		// std::exception, not catch(...): the YAML errors all derive from it, and a
		// narrower catch keeps a genuine bug (bad_alloc, logic_error) from being
		// reported to the player as 'corrupt save'.
		catch (const std::exception &e)
		{
			// A corrupt save is reported, not thrown. A caller mid-game
			// should get false and a reason, not an exception.
			return fail(std::string("corrupt save file: ") + e.what());
		}

		// IsMap before any key access. Without it a save that is a bare scalar or a
		// sequence would be probed for keys that are not there, and the version
		// check below would misreport the problem.
		if (!data.IsMap())
			return fail("save file is not a map");

		// Missing SchemaVersion means this is not a Wasteland save at all -- a
		// different file that happened to be named .wlsave. Rejected before any
		// other key is read.
		if (!data["SchemaVersion"])
			return fail("save has no SchemaVersion; it is not a Wasteland save");

		int version = 0;
		try
		// The version itself is read defensively: a file where SchemaVersion is a
		// string or a map is caught here rather than producing a nonsense integer.
		{
			version = data["SchemaVersion"].as<int>();
		}
		catch (...)
		{
			return fail("SchemaVersion is not an integer");
		}

		// Recorded BEFORE the mismatch check, so GetLoadedSchemaVersion() reports
		// what was on disk even for a save that was then refused. That is what makes
		// the accessor useful to a test asserting a rejection rather than a silent
		// success.
		m_LoadedSchemaVersion = version;

		// Fail LOUDLY on a version mismatch. Loading a save whose shape we
		// Refusing loudly is the whole design decision here. Loading a save whose
		// shape we do not understand silently drops fields the player earned, and
		// the player cannot tell that apart from normal progression.
		// do not understand silently drops fields the player earned, and the
		// player has no way to tell that from normal progression.
		if (version != kCurrentSchemaVersion)
		{
			// The message names BOTH versions -- the one on disk and the one this build
			// wants -- so the player can act on it rather than just being told 'no'.
			return fail("save schema version " + std::to_string(version) +
						" does not match this build's version " +
						std::to_string(kCurrentSchemaVersion));
		}

		// Past this point the file is understood, so state may be applied.
		// Past this point the file is fully understood, so applying state cannot
		// corrupt anything. This is the boundary the whole validation chain protects.
		if (data["WorldTime"])
			m_WorldTime = data["WorldTime"].as<double>();

		// 'applied' counts ReadEntity returning true, which INCLUDES entities that
		// were found missing and skipped -- so this count is records read, not
		// records that changed the world. Worth knowing before the log line below is
		// read as 'N entities restored'.
		int applied = 0;
		if (data["Entities"])
		{
			for (const auto &node : data["Entities"])
			{
				std::string entityError;
				// `entityError` is captured and then DISCARDED -- DEFECT. ReadEntity writes
				// a reason there (the missing-UUID case), but Load never inspects or logs it,
				// so a malformed record vanishes into the applied/not-applied count with no
				// diagnostic anywhere.
				if (ReadEntity(node, &entityError))
					applied++;
			}
		}

		// Set from the applied count, so after a load this reports records read.
		// Overwritten on every successful Save and Load; untouched by a failed one.
		m_LastEntityCount = applied;
		WL_CORE_INFO("SaveGame: loaded slot '{0}' ({1} entities, world time {2}s)",
					 slot, applied, m_WorldTime);
		return true;
	}

}
