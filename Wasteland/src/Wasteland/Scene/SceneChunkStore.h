// SceneChunkStore.h -- a binary sidecar that lets a scene open without parsing YAML.
//
// WHY THIS EXISTS
// ---------------
// SceneSerializer::Deserialize walks a scene document with yaml-cpp. Measured on this
// project that costs roughly 510 ms per megabyte of scene text plus ~11 us per component
// field, so a 1M-entity scene takes about three minutes to open. Nearly all of that work
// is re-deriving numbers that were already known when the scene was last SAVED.
//
// This is a CACHE, not a format. The .wastescene YAML remains the authoring format and
// the only thing a human edits. The sidecar is derived from it, carries no information
// the YAML does not, and can be deleted at any time with no loss.
//
// SAFETY RULES
// ------------
// These rules are the entire reason this is a separate file rather than a fast path
// sprinkled through the serializer:
//
//   1. A sidecar is written ONLY when the scene is FULLY representable in it. If any
//      entity carries a component outside the supported set, holds a texture reference,
//      or has a tag longer than kMaxTagLength, NO sidecar is written and loading stays
//      on YAML. A partial sidecar would silently drop data on the next load, and a
//      silent data loss is far worse than a slow load.
//
//   2. A sidecar is read ONLY when its header matches the YAML file it was built from
//      (byte size and write time) and its version matches. A stale sidecar is ignored,
//      never served.
//
//   3. A read that fails validation at any point leaves the scene untouched and reports
//      failure, so the caller can fall back to YAML.
//
// Supported components are exactly: IDComponent, TagComponent, TransformComponent,
// CubeRendererComponent, CylinderRendererComponent, SphereRendererComponent,
// MaterialComponent, PointLightComponent. That covers the bulk case this exists for --
// large numbers of inert static props, plus the primitives and lights a level uses.
// Scenes with terrain, a neural stage, scripts, physics or colliders keep using the YAML
// path unchanged.

#pragma once

#include "Scene.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Wasteland
{

	// Reads and writes the binary sidecar for one scene file.
	//
	// All methods are static and stateless: the sidecar is a pure function of a scene and
	// its YAML file, so there is nothing for an instance to hold.
	class SceneChunkStore
	{
	public:
		// Longest tag that survives a round trip. Longer tags suppress the sidecar rather
		// than being truncated, because a truncated name is silent data loss.
		static constexpr std::uint32_t kMaxTagLength = 63;

		// Produces the authored YAML for one entity, used for entities the fast records
		// cannot hold.
		//
		// Supplied by SceneSerializer (which owns the only entity writer) so this file
		// never has to know how a component serializes. An entity that needs a fragment --
		// because it carries an unsupported component, a texture reference, or a tag too
		// long for a record -- is stored as its original YAML and replayed through the
		// normal reader on load. That is what makes the sidecar total: no scene is ever
		// refused, it just has fewer fast records.
		using FragmentEmitter = std::function<std::string(Scene &, entt::entity)>;

		// Sidecar path for a scene: the scene path with the extension replaced by .wlbin.
		//
		// "level.wastescene" -> "level.wlbin". Derived rather than stored so the pair can
		// never be separated by a rename of the YAML.
		static std::filesystem::path SidecarPathFor(const std::filesystem::path &scenePath);

		// Writes the sidecar for a scene that has just been saved to scenePath.
		//
		// Call this only after the YAML file is fully written and closed, because the
		// header records that file's size and write time to detect staleness.
		//
		// Every entity is stored: as a fixed record when the record can hold it exactly,
		// otherwise as its authored YAML from `emitFragment`. There is no scene this
		// refuses, so a complex scene still gets a sidecar -- it simply routes more
		// entities through fragments.
		//
		// Returns false only on an I/O failure, in which case no sidecar is left behind.
		//
		// @param scene         Scene to snapshot. Must be the scene just saved. Non-const:
//                      an entity's YAML needs an Entity wrapper, which holds a
//                      Scene*. Write does not modify the scene.
		// @param scenePath     The .wastescene file the scene was written to.
		// @param emitFragment  Produces one entity's YAML. Must not return empty.
		// @param reason        If non-null and this returns false, receives why.
		static bool Write(Scene &scene, const std::filesystem::path &scenePath,
			const FragmentEmitter &emitFragment, std::string *reason);

		// Removes a stale or unusable sidecar, ignoring errors.
		//
		// Called when a scene is saved and no sidecar could be written, so a sidecar from a
		// previous, more-restrictive save cannot be served against the new YAML.
		static void Remove(const std::filesystem::path &scenePath);

		// Loads scene from the sidecar beside scenePath, if one exists and validates.
		//
		// Fast records are emplaced directly. Entities stored as fragments are NOT read
		// here -- their YAML is handed back through `fragments` for the caller to replay
		// through SceneSerializer's own reader, which is the only place that knows how a
		// component is deserialized. Replaying them anywhere else would be a second
		// reader that could drift from the first.
		//
		// On success the scene is populated and true is returned. On any failure the scene
		// is left EMPTY and false is returned; the caller must fall back to YAML. The scene
		// is cleared first, so a partial load can never be mistaken for a success.
		//
		// @param scene      Scene to populate. Emptied before reading.
		// @param scenePath  The .wastescene file whose sidecar should be used.
		// @param fragments  If non-null, receives one YAML document per fragment entity.
		// @param reason     If non-null and this returns false, receives why.
		static bool Read(Scene &scene, const std::filesystem::path &scenePath,
			std::vector<std::string> *fragments, std::string *reason);
	};

}
