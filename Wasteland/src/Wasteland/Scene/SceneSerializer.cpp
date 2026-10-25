// SceneSerializer.cpp -- YAML persistence for scenes.
//
// Three layers, in order:
//   1. yaml-cpp converters for glm vector types (lines below), so the
//      library can read and write them as flow sequences.
//   2. Stream operators that write those same vectors directly to an
//      Emitter.
//   3. The serializer itself: SerializeEntity writes one entity, and the
//      four SceneSerializer methods drive whole scenes.
//
// The on-disk format is a YAML mapping with a `Scene` name key and an
// `Entities` sequence, each entity a mapping of component name to
// component fields. Round-tripped by hand -- there is no schema and no
// reflection, so adding a component field means editing the writer AND
// the reader, and forgetting the reader silently drops the field on load.
//
// Every component added to a scene must be handled in all three of
// SerializeEntity, Deserialize, and (once implemented) the Runtime
// variants.
#include "wlpch.h"
#include "SceneSerializer.h"

#include "Entity.h"
#include "SceneChunkStore.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Terrain/TerrainScenePersistence.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <exception>
#include <system_error>

#include <yaml-cpp/yaml.h>
#include "Components.h"

// yaml-cpp customization namespace.
//
// Specializing convert<T> here is how a third-party library is taught to
// handle a type it has never heard of: node.as<glm::vec3>() on load and
// the convert path on save then both work without any call-site changes.
// The alternative -- converting by hand at every component -- is what
// layer 2 below is the residue of.
namespace YAML
{

	template <>
	// Teaches yaml-cpp to treat a glm::vec2 as a 2-element flow sequence,
	// e.g. `[1, 2]`.
	//
	// A vector is a SEQUENCE, not a mapping, so it round-trips as `[x, y]`
	// rather than as named keys. That is what makes these converters
	// non-obvious on read: position carries the meaning, and no key says
	// which component is which.
	//
	// Consequently reordering a vector's components on disk silently
	// reinterprets the value rather than failing -- a vec3 with its y and z
	// swapped loads as valid data that is wrong.
	struct convert<glm::vec2>
	{
		// Builds the Node for a vec2.
		//
		// Uses push_back to build a sequence of the components in order, then
		// sets Flow style so it EMITS as `[x, y]` rather than as a block sequence
		// with dashes. Without the style call a vec3 would be written as three
		// `- value` lines, which is valid YAML but triples the file size for
		// every transform in the scene and makes it unreadable.
		static Node encode(const glm::vec2 &rhs)
		{
			// push_back on a default-constructed Node yields a SEQUENCE, one element
			// per call, in call order. There is no size argument, so the arity is
			// whatever the number of push_backs -- which is why encode below and the
			// size check in decode have to be kept in step by hand.
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			// Flow style makes the emitter write `[x, y]` on one line. Without it the
			// same sequence is written as block style with a dash per element, which
			// is valid YAML but three lines per vector -- a readability and file-size
			// cost multiplied by every transform in the scene.
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		// Reads a Node into a vec2, reporting failure by returning false.
		//
		// Returning bool is yaml-cpp's failure channel for convert -- the caller
		// decides what a bad node means.
		//
		// HAZARD: the false return covers a wrong SHAPE only (not a sequence, or
		// not exactly 2 elements). A correctly-shaped sequence containing a
		// non-float -- `[1, "red"]` -- reaches node[1].as<float>() below, which
		// THROWS YAML::TypedBadConversion. decode does not catch it, so it
		// propagates out of node.as<glm::vec2>() and out of Deserialize. Editing
		// a scene file by hand and mistyping one number therefore throws out of
		// the load path rather than returning false.
		static bool decode(const Node &node, glm::vec2 &rhs)
		{
			// The only guard: wrong KIND, or wrong COUNT. Both return false, which is
			// yaml-cpp's failure channel for convert -- there is no exception and no
			// log here.
			// Note node.size() is compared against an exact 2, so a 3-element sequence
			// fails rather than being truncated. Safer than the alternative.
			if (!node.IsSequence() || node.size() != 2)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			return true;
		}
	};

	template <>
	// glm::vec3 as a 3-element flow sequence, `[x, y, z]`.
	//
	// This is the workhorse: TransformComponent's Translation, Rotation and
	// Scale are all vec3, so almost every entity in a scene round-trips
	// through this converter.
	//
	// NOTE the arity check is strict -- exactly 3 elements, not 'at least
	// 3'. A hand-edited file with an extra element fails rather than
	// ignoring it, which is the safer of the two behaviours.
	struct convert<glm::vec3>
	{
		// Three push_backs for three components, matching the size() != 3 check in
		// decode below. Same manual correspondence as the vec2 converter.
		static Node encode(const glm::vec3 &rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node &node, glm::vec3 &rhs)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template <>
	// glm::vec4 as a 4-element flow sequence, `[r, g, b, a]`.
	//
	// Used by every Color field in the primitive renderer components and by
	// MaterialComponent's Albedo and EmissionColor.
	//
	// For a COLOR this positional format is a real hazard: `[1, 1, 1, 1]`
	// carries no indication of which element is alpha, so a file edited by
	// hand can shift the channels and produce a wrong colour rather than a
	// load error.
	struct convert<glm::vec4>
	{
		// Four components. The [r, g, b, a] ordering is positional and therefore
		// invisible in the file: nothing on disk says which element is alpha.
		static Node encode(const glm::vec4 &rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.push_back(rhs.w);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node &node, glm::vec4 &rhs)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[3].as<float>();
			return true;
		}
	};

}

namespace Wasteland
{

	// Stream operators writing glm vectors straight to an Emitter.
	//
	// These DUPLICATE the work of the convert specializations above -- both
	// produce the same `[x, y]` on disk, by two independent implementations.
	// Both are needed because they serve different call sites: the convert
	// path is what node.as<T>() uses when LOADING, while these are used when
	// writing directly with `out << value`.
	//
	// MAINTENANCE HAZARD: the duplication is unguarded. If the two
	// implementations of the on-disk format ever diverge -- say the stream
	// operator gains different precision -- files written by one path would
	// not read correctly through the other, with nothing to catch it.
	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec2 &v)
	{
		// YAML::Flow sets the style for everything that FOLLOWS it on this emitter,
		// which is why it is emitted once before BeginSeq rather than inside. The
		// style is sticky, not scoped -- see the missing reset noted on the
		// vec3 overload below.
		out << YAML::Flow;
		// BeginSeq/EndSeq bracket the elements; the flow style from the line above
		// is what collapses them onto one line. Emitting v.x and v.y as separate
		// `<<` operations is what makes this an overload-by-arity rather than a
		// loop -- there is no reflection here to iterate a vec2 generically.
		out << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
		return out;
	}

	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec3 &v)
	{
		out << YAML::Flow;
		// DEFECT: YAML::Flow is sticky on the Emitter, and nothing resets it. Any
		// later `out << something` on this same emitter inherits flow style, so a
		// subsequent block-style construct (a nested map or a multi-element
		// sequence written outside a BeginSeq) can come out inline. The serialize
		// path does not appear to hit this because every sequence is explicitly
		// bracketed, but the stream operators have no way to restore block style.
		out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		return out;
	}

	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec4 &v)
	{
		out << YAML::Flow;
		// Same shape as the two overloads above. Note the four overloads of
		// operator<< together cover every glm vector the components use; a vec4
		// field in a NEW component compiles only because this exists, and a
		// different arity would not compile at all rather than fail silently.
		out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
		return out;
	}

	// Enum-to-string maps for the serialized enums.
	//
	// These strings ARE the file format: an enum's saved name is an
	// implementation detail of its spelling here, so renaming an enumerator
	// silently breaks every scene file already on disk. The literals below
	// should be treated as part of the format, not as debug output.
	//
	// ERROR HANDLING IS INCONSISTENT ACROSS THESE FUNCTIONS, which matters
	// when loading a hand-edited or older file:
	//   * These 3D and water pairs fall back to a DEFAULT SILENTLY -- no
	//     assert, no log. An unrecognised string loads as Static or Lake.
	//   * The 2D body type pair below DOES assert on an unknown string.
	// So the same mistake -- a typo in an enum name -- is reported in one
	// case and quietly absorbed in another. In a Release build the asserts
	// compile away entirely, so all of them end up silent.
	static std::string RigidBody3DTypeToString(RigidBody3DType type)
	{
		// One case per enumerator, each returning on the same line -- the style
		// differs from the 2D pair below, which puts each return on its own line.
		// Cosmetic, but the two are meant to be read side by side.
		switch (type)
		{
		case RigidBody3DType::Static:    return "Static";
		case RigidBody3DType::Dynamic:   return "Dynamic";
		case RigidBody3DType::Kinematic: return "Kinematic";
		}
		return "Static";
	}

	// Parses a 3D body type name.
	//
	// A chain of string comparisons rather than a switch, because the input
	// is a string rather than an enumerator.
	//
	// No validation: anything unrecognised -- a typo, an empty string, a
	// missing key -- becomes Static. A body silently reverting to Static is
	// easy to miss, since Static means immovable and infinite-mass, so the
	// entity simply stops responding to gravity rather than looking wrong.
	//
	// Exact and case-sensitive: "dynamic" does not parse.
	static RigidBody3DType RigidBody3DTypeFromString(const std::string &str)
	{
		// A chain of early returns rather than a switch: the input is a string, so
		// the cases are string literals rather than enumerators and cannot be
		// written in a switch.
		// Only two names are tested. Static is the implicit fallback, so it needs
		// no branch -- which also means a typo and an absent key are the same case.
		if (str == "Dynamic")   return RigidBody3DType::Dynamic;
		if (str == "Kinematic") return RigidBody3DType::Kinematic;
		// Static is the last resort rather than the first branch, which is why
		// there are only two comparisons: the third enumerator needs no test.
		return RigidBody3DType::Static;
	}


	// Writes a water body type's name. See the note on the 3D pair above.
	//
	// The trailing return is unreachable while the switch covers every
	// enumerator, but it is needed so the function compiles -- and it will
	// become reachable if an enumerator is added without a case, silently
	// labelling that body "Lake".
	static std::string WaterBodyTypeToString(WaterBodyType type)
	{
		switch (type)
		{
		// Same one-line-per-case shape as the 3D pair. The unreachable trailing
		// return is required for compilation, since the compiler cannot prove the
		// switch is exhaustive.
		case WaterBodyType::Lake:  return "Lake";
		case WaterBodyType::River: return "River";
		case WaterBodyType::Ocean: return "Ocean";
		}
		return "Lake";
	}


	// Parses a water body type name. Silent default to Lake on anything
	// unrecognised; see the note on the 3D pair above.
	//
	// The saved name records the PRESET that was applied, but the preset's
	// values are saved alongside it. If those two disagree -- the file was
	// hand-edited, or a preset changed since it was written -- loading
	// restores the fields and not the preset, so the file wins and Type
	// becomes stale bookkeeping.
	static WaterBodyType WaterBodyTypeFromString(const std::string &str)
	{
		if (str == "River") return WaterBodyType::River;
		if (str == "Ocean") return WaterBodyType::Ocean;
		// Two comparisons for three enumerators, same shape as the 3D pair above.
		// The default is Lake rather than Static because that is the enum's own
		// first enumerator -- an arbitrary choice, but a consistent one.
		return WaterBodyType::Lake;
	}

	// Writes a 2D body type's name.
	//
	// Unlike the two pairs above, this one ASSERTS on an unrecognised value,
	// so a mismatch between the switch and the enum surfaces in Debug
	// instead of being absorbed.
	static std::string RigidBody2DBodyTypeToString(Rigidbody2DComponent::BodyType bodyType)
	{
		switch (bodyType)
		{
		case Rigidbody2DComponent::BodyType::Static:
			return "Static";
		case Rigidbody2DComponent::BodyType::Dynamic:
			return "Dynamic";
		case Rigidbody2DComponent::BodyType::Kinematic:
			return "Kinematic";
		}

		// The one enum pair that ASSERTS rather than falling back silently. The
		// assert is debug-only, so in Release the `return {}` below is what a
		// caller actually gets: an EMPTY STRING, not "Static".
		// DEFECT: that empty string then fails to parse in the FromString
		// counterpart, which asserts again and falls back to Static -- so the
		// inconsistency is invisible until the string is inspected.
		WL_CORE_ASSERT(false, "Unknown body type");
		// DEFECT: `return {}` yields an EMPTY STRING, not "Static" as the two
		// pairs above return. In a Release build -- where the assert compiles out --
		// a caller writing an unrecognised body type writes an empty key, which
		// the parser below then treats as unrecognised and maps to Static.
		// The round trip degrades to a default rather than failing, but via two
		// different defaults than the neighbouring functions use.
		return {};
	}

	// Parses a 2D body type name, asserting on an unknown string.
	//
	// Written out one comparison per line rather than as a chain, unlike the
	// two functions above -- a cosmetic inconsistency between functions that
	// are otherwise the same shape and easy to read side by side.
	//
	// In a Release build the assert vanishes and the fallback Static is all
	// that remains, matching the silent behaviour of the pairs above.
	static Rigidbody2DComponent::BodyType RigidBody2DBodyTypeFromString(const std::string bodyTypeString)
	{
		// One comparison per line with the return on its own line, unlike the two
		// functions above. Same semantics, different formatting.
		if (bodyTypeString == "Static")
			return Rigidbody2DComponent::BodyType::Static;
		if (bodyTypeString == "Dynamic")
			return Rigidbody2DComponent::BodyType::Dynamic;
		if (bodyTypeString == "Kinematic")
			return Rigidbody2DComponent::BodyType::Kinematic;

		WL_CORE_ASSERT(false, "Unknown body type");
		return Rigidbody2DComponent::BodyType::Static;
	}

	// Binds this serializer to the scene it will read and write.
	//
	// Just stores the shared reference. No validation of `scene` itself, so
	// a null scene constructs successfully and fails at first use.
	//
	// @param scene  The scene to serialize. Shared, not unique.
	// Member-init list form rather than an assignment in the body. The scene
	// is a shared Ref, so this copies a refcount rather than the scene.
	SceneSerializer::SceneSerializer(const Ref<Scene> &scene)
		: m_Scene(scene)
	{
	}

	// Writes one entity's ID, tag, transform, and every component present on
	// it, as a single YAML mapping.
	//
	// The whole component-writing half of the file format lives here, and it
	// is hand-written with no reflection or schema. Two consequences worth
	// knowing before adding a component:
	//
	//   * A new component must be added in THREE places -- here,
	//     Deserialize's mirror of this, and (once implemented) the Runtime
	//     pair. Missing the second is the dangerous one: the field is written
	//     to the file and then silently ignored on load, so the component
	//     comes back at its defaults with no error anywhere.
	//   * Nothing is written for a component the entity does not have, so
	//     absence is the on-disk representation of "not present" -- there
	//     are no explicit nulls to confuse the reader.
	//
	// @param out     The emitter to write into, already inside the Entities
	//               sequence begun by Serialize.
	// @param entity  The entity to write. Must carry an IDComponent --
	//               asserted at the top of the body, and in Release that
	//               assertion disappears, leaving an entity serialized with
	//               an empty or garbage id.
	static void SerializeEntity(YAML::Emitter &out, Entity entity)
	{
		// The assert is the only thing standing between a missing IDComponent and
		// GetUUID()'s own assert inside Entity.h. In Release both vanish and the
		// entity serializes with whatever the registry holds for that type.
		WL_CORE_ASSERT(entity.HasComponent<IDComponent>(), "Cannot serialize an entity with no IDComponent!");

		// BeginMap opens the entity's mapping. Every component block below adds a
		// Key then its own nested BeginMap/EndMap, and the matching EndMap at the
		// bottom of the function closes this one -- so the block count here must
		// equal the number of `if (entity.HasComponent...)` sections.
		out << YAML::BeginMap; // Entity
		// The identity key, named "Entity" rather than "UUID" -- the on-disk
		// spelling is the file format, and Deserialize reads the same literal.
		// GetUUID returns a UUID object; the emitter serializes it as the integer
		// the type wraps.
		out << YAML::Key << "Entity" << YAML::Value << entity.GetUUID();

		// Presence-gated, which is the whole error model of this format: no key
		// means the component is not on the entity, not that it has null fields.
		// An entity with no TagComponent simply gets no TagComponent key.
		if (entity.HasComponent<TagComponent>())
		{
			out << YAML::Key << "TagComponent";
			out << YAML::BeginMap; // TagComponent

			// By reference into the component, so no copy of the string. The component
			// outlives the call because the entity is alive for the whole iteration.
			auto &tag = entity.GetComponent<TagComponent>().Tag;
			out << YAML::Key << "Tag" << YAML::Value << tag;

			// Closes the TagComponent map. Every component block pairs its own
			// BeginMap with its own EndMap, and the entity-level EndMap at the bottom
			// of the function closes the outer one -- so a missing EndMap here would
			// nest every following component one level deeper.
			out << YAML::EndMap; // TagComponent
		}

		if (entity.HasComponent<TransformComponent>())
		{
			out << YAML::Key << "TransformComponent";
			out << YAML::BeginMap; // TransformComponent

			// All three vectors go through convert<glm::vec3> on the way out, so each
			// is emitted as a 3-element flow sequence.
			auto &tc = entity.GetComponent<TransformComponent>();
			// Translation, Rotation, Scale, in that order and all as vec3 flow
			// sequences. Order is not semantic in YAML -- a mapping's keys are looked
			// up by name on load -- so these three could be written in any order.
			out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
			// Rotation is stored as a vec3 rather than a quaternion or Euler triple:
			// whatever TransformComponent::Rotation actually is, it round-trips as the
			// same three numbers, which is all this layer promises.
			out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
			out << YAML::Key << "Scale" << YAML::Value << tc.Scale;

			// Closes the TransformComponent map.
			out << YAML::EndMap; // TransformComponent
		}

		if (entity.HasComponent<CameraComponent>())
		{
			out << YAML::Key << "CameraComponent";
			out << YAML::BeginMap; // CameraComponent

			// Two hops, not one: cameraComponent is the component, camera is the
			// SceneCamera it owns. The nested "Camera" key below mirrors that, and
			// Deserialize walks back down through the same two levels.
			auto &cameraComponent = entity.GetComponent<CameraComponent>();
			auto &camera = cameraComponent.Camera;

			out << YAML::Key << "Camera" << YAML::Value;
			out << YAML::BeginMap; // Camera
			// ProjectionType is cast to int on the way out so the file holds a plain
			// number rather than a spelled-out enumerator. That is the opposite choice
			// from the body-type enums further down, which are written as STRINGS --
			// so the format spells some enums and numbers others.
			// DEFECT: a renumbering of SceneCamera::ProjectionType silently changes
			// what every saved scene means, with no version field to catch it.
			out << YAML::Key << "ProjectionType" << YAML::Value << (int)camera.GetProjectionType();
			// Near/far for the perspective projection. Written even when the camera is
			// orthographic, and vice versa below -- the projection type at line 396 is
			// what decides which half of this block matters, and the unused half is
			// still stored so switching projection preserves the settings.
			out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
			out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
			out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
			// OrthographicSize plus near/far, the same arrangement as the perspective
			// pair above. Both projections are always written so that switching one to
			// the other in the editor and back does not lose the other one's values.
			out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
			out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
			out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
			out << YAML::EndMap; // Camera

			// Primary and FixedAspectRatio live on the component, not on the camera,
			// so they sit outside the nested Camera map. Primary decides which camera
			// the renderer picks; nothing in this file enforces a single primary.
			out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
			out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;

			// Closes the CameraComponent map, after the nested Camera map at line 403
			// was already closed. Two levels, because the component owns a camera.
			out << YAML::EndMap; // CameraComponent
		}

		if (entity.HasComponent<SpriteRendererComponent>())
		{
			out << YAML::Key << "SpriteRendererComponent";
			out << YAML::BeginMap; // SpriteRendererComponent

			auto &spriteRendererComponent = entity.GetComponent<SpriteRendererComponent>();
			// One vec4, one line. Every renderer block opens with a colour this way,
			// and the converter that serialises it is the vec4 specialization at the
			// top of the file -- so a colour and a vector of offsets go through
			// different converters on the same save.
			out << YAML::Key << "Color" << YAML::Value << spriteRendererComponent.Color;

			// The sprite block is one field in, one field out. Compare the 24-field
			// water block: the block length is purely a function of how many fields
			// the component has.
			out << YAML::EndMap; // SpriteRendererComponent
		}

		if (entity.HasComponent<CircleRendererComponent>())
		{
			out << YAML::Key << "CircleRendererComponent";
			out << YAML::BeginMap; // CircleRendererComponent

			auto &circleRendererComponent = entity.GetComponent<CircleRendererComponent>();
			// Read back in the same order the writer emits them, which is what makes
			// the block easy to compare side by side. Nothing depends on the order,
			// though: YAML mappings are looked up by key.
			out << YAML::Key << "Color" << YAML::Value << circleRendererComponent.Color;
			// Thickness and Fade are the circle's two non-colour knobs: outline width
			// and the fade toward the edge. Both are floats, both are read back
			// unguarded -- see the note on the circle reader.
			out << YAML::Key << "Thickness" << YAML::Value << circleRendererComponent.Thickness;
			out << YAML::Key << "Fade" << YAML::Value << circleRendererComponent.Fade;

			out << YAML::EndMap; // CircleRendererComponent
		}

		if (entity.HasComponent<CubeRendererComponent>())
		{
			out << YAML::Key << "CubeRendererComponent";
			out << YAML::BeginMap; // CubeRendererComponent

			auto &cubeRendererComponent = entity.GetComponent<CubeRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << cubeRendererComponent.Color;
			// DEFECT (verified by key-name diff of the two halves of this file):
			// TextureWidth and TextureHeight are written here but READ by no
			// CubeRendererComponent block in Deserialize. They are pure file bloat on
			// every save, and a reader cannot tell they were meant to matter.
			// The ternary guards the null texture; the 0 is indistinguishable from a
			// real zero-sized texture once written.
			out << YAML::Key << "TextureWidth" << YAML::Value << (cubeRendererComponent.Texture ? cubeRendererComponent.Texture->GetWidth() : 0);
			// Same write-only pair as the line above -- both are dropped on load.
			out << YAML::Key << "TextureHeight" << YAML::Value << (cubeRendererComponent.Texture ? cubeRendererComponent.Texture->GetHeight() : 0);
			// TextureIndex picks a slot from the texture atlas, TilingFactor repeats
			// it. The index is what makes a scene portable: it names a slot, not a
			// file, so re-ordering the atlas does not break saved scenes.
			out << YAML::Key << "TextureIndex" << YAML::Value << cubeRendererComponent.TextureIndex;
			// TilingFactor is a float rather than an int, so fractional tiling is
			// expressible. It is one of the few fields here that survives a value
			// change in the editor rather than being derived at load.
			out << YAML::Key << "TilingFactor" << YAML::Value << cubeRendererComponent.TilingFactor;

			out << YAML::EndMap; // CubeRendererComponent
		}

		if (entity.HasComponent<SphereRendererComponent>())
		{
			// Sectors and Stacks are the mesh tessellation, written here and read back
			// unchanged, so a sphere round-trips its geometry exactly.
			out << YAML::Key << "SphereRendererComponent";
			out << YAML::BeginMap; // SphereRendererComponent

			auto &sphereRendererComponent = entity.GetComponent<SphereRendererComponent>();
			// Radius, Sectors, Stacks are mesh generation parameters, so a sphere
			// round-trips its exact tessellation. Re-generating with different values
			// is what makes an edited sphere look different rather than the same.
			out << YAML::Key << "Color" << YAML::Value << sphereRendererComponent.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (sphereRendererComponent.Texture ? sphereRendererComponent.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (sphereRendererComponent.Texture ? sphereRendererComponent.Texture->GetHeight() : 0);
			// The Blender primitives below all follow this same six-field template:
			// Colour, the two texture dimensions, the shape parameters that differ per
			// primitive, then TextureIndex and TilingFactor. The uniformity is what
			// makes adding a new primitive mechanical -- and what makes the shared
			// TextureWidth/Height omission easy to miss in any single block.
			out << YAML::Key << "Radius" << YAML::Value << sphereRendererComponent.Radius;
			out << YAML::Key << "Sectors" << YAML::Value << sphereRendererComponent.Sectors;
			out << YAML::Key << "Stacks" << YAML::Value << sphereRendererComponent.Stacks;
			out << YAML::Key << "TextureIndex" << YAML::Value << sphereRendererComponent.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << sphereRendererComponent.TilingFactor;

			out << YAML::EndMap; // SphereRendererComponent
		}

		// --- Blender "Add > Mesh" primitives ---
		// YAML keys are matched by hand against the C++ type names below, so
		// they must stay identical. A missing key on load silently skips, which
		// is what keeps older scene files readable.

		// The eight primitive blocks below share one convention: the YAML key is
		// the C++ type name spelled identically. Nothing enforces that, so a typo
		// in the key would write data that Deserialize's lookup never finds.
		if (entity.HasComponent<PlaneRendererComponent>())
		{
			out << YAML::Key << "PlaneRendererComponent";
			out << YAML::BeginMap;

			auto &prc = entity.GetComponent<PlaneRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << prc.Color;
			// DEFECT: TextureWidth and TextureHeight are write-only across ALL EIGHT
			// primitive blocks and MaterialComponent -- verified by extracting the
			// keys each block emits and each block looks up. Eleven blocks, one bug.
			out << YAML::Key << "TextureWidth" << YAML::Value << (prc.Texture ? prc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (prc.Texture ? prc.Texture->GetHeight() : 0);
			out << YAML::Key << "Subdivisions" << YAML::Value << prc.Subdivisions;
			out << YAML::Key << "TextureIndex" << YAML::Value << prc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << prc.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<GridRendererComponent>())
		{
			out << YAML::Key << "GridRendererComponent";
			out << YAML::BeginMap;

			auto &grc = entity.GetComponent<GridRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << grc.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (grc.Texture ? grc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (grc.Texture ? grc.Texture->GetHeight() : 0);
			out << YAML::Key << "XSubdivisions" << YAML::Value << grc.XSubdivisions;
			// Grid writes X and Y subdivisions separately rather than one count, so the
			// cells need not be square. Both are read back, so the asymmetry holds.
			out << YAML::Key << "YSubdivisions" << YAML::Value << grc.YSubdivisions;
			// Two subdivision counts rather than one, so grid cells need not be
			// square. Both are read back, so the asymmetry between X and Y survives a
			// round trip.
			out << YAML::Key << "TextureIndex" << YAML::Value << grc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << grc.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<Circle3DRendererComponent>())
		{
			out << YAML::Key << "Circle3DRendererComponent";
			out << YAML::BeginMap;

			auto &c3c = entity.GetComponent<Circle3DRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << c3c.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (c3c.Texture ? c3c.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (c3c.Texture ? c3c.Texture->GetHeight() : 0);
			out << YAML::Key << "Radius" << YAML::Value << c3c.Radius;
			out << YAML::Key << "Segments" << YAML::Value << c3c.Segments;
			out << YAML::Key << "TextureIndex" << YAML::Value << c3c.TextureIndex;
			// Segments here, Sectors/Stacks for the sphere and sphere-like shapes:
			// three names for tessellation density across the primitive family, chosen
			// by whichever primitive introduced the field first.
			out << YAML::Key << "TilingFactor" << YAML::Value << c3c.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<IcoSphereRendererComponent>())
		{
			out << YAML::Key << "IcoSphereRendererComponent";
			out << YAML::BeginMap;

			auto &isc = entity.GetComponent<IcoSphereRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << isc.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (isc.Texture ? isc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (isc.Texture ? isc.Texture->GetHeight() : 0);
			// IcoSphere and Plane share the Subdivisions key name for what is a
			// different mesh operation. Harmless because each primitive has its own
			// block, but it is why renaming one would not be a simple find-replace.
			out << YAML::Key << "Radius" << YAML::Value << isc.Radius;
			// IcoSphere uses Subdivisions, the same key name as the plane's, for the
			// same idea at a different base shape. Reusing the key across primitives is
			// fine because each block has its own mapping scope.
			out << YAML::Key << "Subdivisions" << YAML::Value << isc.Subdivisions;
			out << YAML::Key << "TextureIndex" << YAML::Value << isc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << isc.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<CylinderRendererComponent>())
		{
			out << YAML::Key << "CylinderRendererComponent";
			out << YAML::BeginMap;

			auto &cyc = entity.GetComponent<CylinderRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << cyc.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (cyc.Texture ? cyc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (cyc.Texture ? cyc.Texture->GetHeight() : 0);
			out << YAML::Key << "Radius" << YAML::Value << cyc.Radius;
			out << YAML::Key << "Height" << YAML::Value << cyc.Height;
			// Height is the cylinder's defining second dimension, and the last field
			// that is not a tessellation count. Read back, so a cylinder's proportions
			// survive a save.
			out << YAML::Key << "Segments" << YAML::Value << cyc.Segments;
			out << YAML::Key << "TextureIndex" << YAML::Value << cyc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << cyc.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<ConeRendererComponent>())
		{
			out << YAML::Key << "ConeRendererComponent";
			out << YAML::BeginMap;

			auto &coc = entity.GetComponent<ConeRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << coc.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (coc.Texture ? coc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (coc.Texture ? coc.Texture->GetHeight() : 0);
			out << YAML::Key << "Radius" << YAML::Value << coc.Radius;
			out << YAML::Key << "Height" << YAML::Value << coc.Height;
			out << YAML::Key << "Segments" << YAML::Value << coc.Segments;
			out << YAML::Key << "TextureIndex" << YAML::Value << coc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << coc.TilingFactor;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<TorusRendererComponent>())
		{
			out << YAML::Key << "TorusRendererComponent";
			out << YAML::BeginMap;

			auto &trc = entity.GetComponent<TorusRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << trc.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (trc.Texture ? trc.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (trc.Texture ? trc.Texture->GetHeight() : 0);
			out << YAML::Key << "MajorRadius" << YAML::Value << trc.MajorRadius;
			// MajorRadius and MinorRadius: the distance from the centre to the tube
			// centre, and the tube's own thickness. Both needed because the torus is
			// the one primitive whose shape needs two independent lengths.
			out << YAML::Key << "MinorRadius" << YAML::Value << trc.MinorRadius;
			// Torus needs four parameters where the others need one or two: two radii
			// and two independent segment counts. The Major/Minor prefix pair is what
			// distinguishes it, and both halves are read back under the same names.
			out << YAML::Key << "MajorSegments" << YAML::Value << trc.MajorSegments;
			out << YAML::Key << "MinorSegments" << YAML::Value << trc.MinorSegments;
			out << YAML::Key << "TextureIndex" << YAML::Value << trc.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << trc.TilingFactor;

			out << YAML::EndMap;
		}

		// The material block is the only one that writes a PATH, and therefore the
		// only one whose content depends on the filesystem layout rather than on
		// the entity. That makes it the least portable part of the format.
		if (entity.HasComponent<MaterialComponent>())
		{
			out << YAML::Key << "MaterialComponent";
			out << YAML::BeginMap; // MaterialComponent

			auto &materialComponent = entity.GetComponent<MaterialComponent>();
			// Albedo is a vec4 and so is written as a 4-element sequence, unlike the
			// vec3 colours elsewhere -- this one genuinely has an alpha channel.
			// TexturePath, TextureIndex and the generated-map flag follow; the rest
			// are scalar material properties read back by name.
			out << YAML::Key << "Albedo" << YAML::Value << materialComponent.Albedo;
			// TextureWidth/Height again -- the tenth block to emit them. They are the
			// only fields in the material block that no reader consumes; see the
			// defect note on the reader side.
			out << YAML::Key << "TextureWidth" << YAML::Value << (materialComponent.Texture ? materialComponent.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (materialComponent.Texture ? materialComponent.Texture->GetHeight() : 0);
			// Copied from the component, so the component keeps whatever the user
			// authored -- including backslashes. The normalisation below is applied
			// to the copy only, and only the copy reaches the file.
			out << YAML::Key << "TextureIndex" << YAML::Value << materialComponent.TextureIndex;
			// Backslashes are normalised to forward slashes before anything else, so
			// the prefix test below and the is_regular_file call both work on Windows
			// and POSIX alike. Copied rather than mutated: TexturePath is the
			// component's own field and must keep the form the user authored.
			std::string pathToSave = materialComponent.TexturePath;
			std::replace(pathToSave.begin(), pathToSave.end(), '\\', '/');
			// find("assets/") != npos, not a prefix test. A path like
			// "myassets/foo.png" contains the substring at a non-zero offset and is
			// therefore NOT prefixed here, so it is written out as-is and later
			// resolved relative to the working directory. Sloppy, but the substring
			// test is what makes "../assets/x.png" left alone deliberately.
			if (pathToSave.find("assets/") == std::string::npos)
			{
				pathToSave = "assets/" + pathToSave;
			}
			out << YAML::Key << "TexturePath" << YAML::Value << pathToSave;
			// HasGeneratedMaps records that the normal/metallic-roughness maps beside
			// Albedo were baked rather than authored. Without it the loader would not
			// know whether to look for them, so this is load-bearing rather than
			// informational -- and it is read back unconditionally.
			out << YAML::Key << "HasGeneratedMaps" << YAML::Value << materialComponent.HasGeneratedMaps;
			out << YAML::Key << "NormalStrength" << YAML::Value << materialComponent.NormalStrength;
			out << YAML::Key << "Metallic" << YAML::Value << materialComponent.Metallic;
			out << YAML::Key << "Roughness" << YAML::Value << materialComponent.Roughness;
			// EmissionColor and EmissionIntensity are stored separately rather than as
			// a pre-multiplied colour, so an intensity slider in the editor maps
			// directly onto a field here with no conversion on save.
			out << YAML::Key << "EmissionColor" << YAML::Value << materialComponent.EmissionColor;
			out << YAML::Key << "EmissionIntensity" << YAML::Value << materialComponent.EmissionIntensity;
			out << YAML::Key << "DisplacementScale" << YAML::Value << materialComponent.DisplacementScale;

			// Material's EndMap. The largest non-water block at ten fields.
			out << YAML::EndMap; // MaterialComponent
		}

		if (entity.HasComponent<Rigidbody2DComponent>())
		{
			out << YAML::Key << "Rigidbody2DComponent";
			out << YAML::BeginMap; // Rigidbody2DComponent

			auto &rigidbody2DComponent = entity.GetComponent<Rigidbody2DComponent>();
			// The body type goes out as a STRING, via the enum helper pair. Note the
			// asymmetry with ProjectionType above, which goes out as an int: renaming
			// a BodyType enumerator breaks every saved scene, while renumbering
			// ProjectionType does too, and only one of those is greppable in a file.
			out << YAML::Key << "BodyType" << YAML::Value << RigidBody2DBodyTypeToString(rigidbody2DComponent.Type);
			out << YAML::Key << "FixedRotation" << YAML::Value << rigidbody2DComponent.FixedRotation;

			out << YAML::EndMap; // Rigidbody2DComponent
		}

		if (entity.HasComponent<BoxCollider2DComponent>())
		{
			out << YAML::Key << "BoxCollider2DComponent";
			out << YAML::BeginMap; // BoxCollider2DComponent

			auto &boxCollider2DComponent = entity.GetComponent<BoxCollider2DComponent>();
			out << YAML::Key << "Offset" << YAML::Value << boxCollider2DComponent.Offset;
			out << YAML::Key << "Size" << YAML::Value << boxCollider2DComponent.Size;
			// DEFECT (verified by key diff): Friction is written here and READ by no
			// BoxCollider2DComponent block in Deserialize. The 2D box collider's
			// friction silently resets to its default on every load, while the 3D box
			// collider's friction at line 691 does round-trip. The two are otherwise
			// the same shape, which is what makes it easy to miss.
			out << YAML::Key << "Density" << YAML::Value << boxCollider2DComponent.Density;
			out << YAML::Key << "Friction" << YAML::Value << boxCollider2DComponent.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << boxCollider2DComponent.Restitution;
			out << YAML::Key << "RestitutionThreshold" << YAML::Value << boxCollider2DComponent.RestitutionThreshold;

			out << YAML::EndMap; // BoxCollider2DComponent
		}

		if (entity.HasComponent<CircleCollider2DComponent>())
		{
			out << YAML::Key << "CircleCollider2DComponent";
			out << YAML::BeginMap; // CircleCollider2DComponent

			auto &circleCollider2DComponent = entity.GetComponent<CircleCollider2DComponent>();
			out << YAML::Key << "Offset" << YAML::Value << circleCollider2DComponent.Offset;
			out << YAML::Key << "Radius" << YAML::Value << circleCollider2DComponent.Radius;
			out << YAML::Key << "Density" << YAML::Value << circleCollider2DComponent.Density;
			// Same write-only Friction defect as the box collider above. Both 2D
			// colliders lose it; all three 3D colliders keep theirs.
			out << YAML::Key << "Friction" << YAML::Value << circleCollider2DComponent.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << circleCollider2DComponent.Restitution;
			out << YAML::Key << "RestitutionThreshold" << YAML::Value << circleCollider2DComponent.RestitutionThreshold;

			out << YAML::EndMap; // CircleCollider2DComponent
		}

		if (entity.HasComponent<RigidBody3DComponent>())
		{
			out << YAML::Key << "RigidBody3DComponent";
			out << YAML::BeginMap;
			// No blank line after BeginMap here, unlike most of the blocks above -- a
			// formatting inconsistency in a file that is otherwise visually regular,
			// and the kind of thing that makes a diff noisier than it needs to be.
			auto &rb3d = entity.GetComponent<RigidBody3DComponent>();
			out << YAML::Key << "BodyType" << YAML::Value << RigidBody3DTypeToString(rb3d.Type);
			// GravityScale, then the two damping values. Damping is what stops a body
			// sliding forever, so a body with LinearDamping 0 saved and reloaded keeps
			// sliding -- the value is faithfully preserved even though it is often a
			// mistake.
			out << YAML::Key << "GravityScale" << YAML::Value << rb3d.GravityScale;
			out << YAML::Key << "LinearDamping" << YAML::Value << rb3d.LinearDamping;
			out << YAML::Key << "AngularDamping" << YAML::Value << rb3d.AngularDamping;
			// Three independent rotation locks, each presence-checked on load. A
			// body can lock X only, which is a common authored setup for a character
			// that should stay upright.
			out << YAML::Key << "FixedRotationX" << YAML::Value << rb3d.FixedRotationX;
			out << YAML::Key << "FixedRotationY" << YAML::Value << rb3d.FixedRotationY;
			out << YAML::Key << "FixedRotationZ" << YAML::Value << rb3d.FixedRotationZ;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			out << YAML::Key << "BoxCollider3DComponent";
			out << YAML::BeginMap;
			auto &bc = entity.GetComponent<BoxCollider3DComponent>();
			out << YAML::Key << "HalfExtents" << YAML::Value << bc.HalfExtents;
			out << YAML::Key << "Offset" << YAML::Value << bc.Offset;
			out << YAML::Key << "Density" << YAML::Value << bc.Density;
			out << YAML::Key << "Friction" << YAML::Value << bc.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << bc.Restitution;
			out << YAML::EndMap;
		}

		// ---- Gameplay state ----
		//
		// Runtime ACCUMULATORS are deliberately omitted (Health::CooldownTimer,
		// Weapon::FireCooldown/ReloadTimer/IsReloading, Weapon::ShotsFired).
		// They are zero on load so a scene does not resume mid-reload or
		// mid-cooldown. Authored values -- DamageCooldown, ReloadTime, muzzle
		// flash duration -- ARE written, because a save that forgets them
		// reloads with silent guns.
		// The gameplay block is where the deliberate omissions live: live
		// accumulators are not written, so a reloaded scene is always at rest.
		// The cost is that a scene file cannot represent a mid-combat save.
		if (entity.HasComponent<HealthComponent>())
		{
			out << YAML::Key << "HealthComponent";
			out << YAML::BeginMap;
			auto &hc = entity.GetComponent<HealthComponent>();
			// Current and IsDead ARE written, so a scene saved while an entity is dead
			// reloads dead. That is authored state rather than an accumulator, so it
			// is kept -- but it means Save is not a neutral snapshot of the scene.
			out << YAML::Key << "Current" << YAML::Value << hc.Current;
			// Max is authored and Current is a snapshot of play state. Saving both
			// means a scene file records the state the author left it in, not the
			// state a fresh scene would start in -- worth knowing when a hand-edited
			// file disagrees with the entity's designer defaults.
			out << YAML::Key << "Max" << YAML::Value << hc.Max;
			out << YAML::Key << "Invulnerable" << YAML::Value << hc.Invulnerable;
			// DamageCooldown is an AUTHORED property (how long invulnerability lasts)
			// not the live CooldownTimer, which is deliberately not written -- see the
			// note at line 695. The distinction is what lets a scene reload at rest
			// while still remembering how long the effect should block.
			out << YAML::Key << "DamageCooldown" << YAML::Value << hc.DamageCooldown;
			out << YAML::Key << "IsDead" << YAML::Value << hc.IsDead;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<WeaponComponent>())
		{
			out << YAML::Key << "WeaponComponent";
			out << YAML::BeginMap;
			auto &wc = entity.GetComponent<WeaponComponent>();
			// CurrentAmmo is written alongside the authored MaxAmmo and ReserveAmmo.
			// So ammo state IS captured, unlike the cooldown timers -- an inventory
			// snapshot but not a mid-reload one.
			out << YAML::Key << "FireRate" << YAML::Value << wc.FireRate;
			// FireRate and Damage are the authored tuning pair; Range decides whether
			// a shot can reach the target at all. All three are read back, so a scene
			// reloaded mid-fire uses the authored values rather than a snapshot.
			out << YAML::Key << "Damage" << YAML::Value << wc.Damage;
			out << YAML::Key << "Range" << YAML::Value << wc.Range;
			out << YAML::Key << "AmmoPerShot" << YAML::Value << wc.AmmoPerShot;
			// CurrentAmmo, MaxAmmo and ReserveAmmo together: what the gun holds now,
			// what it can hold, and what is in reserve. All three are runtime state
			// being persisted, which is the exception in this block -- the cooldowns
			// are deliberately excluded, these are not.
			out << YAML::Key << "CurrentAmmo" << YAML::Value << wc.CurrentAmmo;
			out << YAML::Key << "MaxAmmo" << YAML::Value << wc.MaxAmmo;
			out << YAML::Key << "ReserveAmmo" << YAML::Value << wc.ReserveAmmo;
			out << YAML::Key << "ReloadTime" << YAML::Value << wc.ReloadTime;
			out << YAML::Key << "AutoReload" << YAML::Value << wc.AutoReload;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<InteractableComponent>())
		{
			out << YAML::Key << "InteractableComponent";
			out << YAML::BeginMap;
			auto &ic = entity.GetComponent<InteractableComponent>();
			out << YAML::Key << "Enabled" << YAML::Value << ic.Enabled;
			// Interactable is two fields only: whether interaction is enabled and what
			// prompt text to show. The prompt is data rather than UI copy, which is why
			// it belongs in the scene file at all.
			out << YAML::Key << "Label" << YAML::Value << ic.Label;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<WeaponFeedbackComponent>())
		{
			out << YAML::Key << "WeaponFeedbackComponent";
			out << YAML::BeginMap;
			auto &fb = entity.GetComponent<WeaponFeedbackComponent>();
			out << YAML::Key << "MuzzleFlashDuration" << YAML::Value << fb.MuzzleFlashDuration;
			out << YAML::Key << "HitMarkerDuration" << YAML::Value << fb.HitMarkerDuration;
			// The live timers are NOT written. A scene loaded mid-flash would
			// otherwise pop a muzzle flash on frame one for a shot the player
			// never took.
			out << YAML::EndMap;
		}

		if (entity.HasComponent<ViewStateComponent>())
		{
			out << YAML::Key << "ViewStateComponent";
			out << YAML::BeginMap;
			auto &vs = entity.GetComponent<ViewStateComponent>();
			// Target and lerp speed for the FOV kick. Read back unconditionally, so a
			// hand-edited view-state node missing one key throws -- unlike the health
			// block, which checks each field.
			out << YAML::Key << "TargetFOV" << YAML::Value << vs.TargetFOV;
			out << YAML::Key << "FOVLerpSpeed" << YAML::Value << vs.FOVLerpSpeed;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<ItemDefinitionComponent>())
		{
			out << YAML::Key << "ItemDefinitionComponent";
			out << YAML::BeginMap;
			auto &def = entity.GetComponent<ItemDefinitionComponent>();
			// Name is the id the rest of the game looks the item up by, so it is the
			// one field here that must not drift; DisplayName and Description are
			// presentation. All three are read back, and Name is the join key.
			out << YAML::Key << "Name" << YAML::Value << def.Name;
			out << YAML::Key << "DisplayName" << YAML::Value << def.DisplayName;
			out << YAML::Key << "Description" << YAML::Value << def.Description;
			out << YAML::Key << "MaxStack" << YAML::Value << def.MaxStack;
			out << YAML::Key << "Weight" << YAML::Value << def.Weight;
			// A sequence written as Key/Value followed by BeginSeq, then one Value per
			// element. Empty Tags still emits the BeginSeq/EndSeq pair, so an empty
			// tag list round-trips as an empty sequence rather than as an absent key --
			// which matters because Deserialize checks `if (node["Tags"])` and an
			// empty sequence is falsy there anyway.
			out << YAML::Key << "Tags" << YAML::Value << YAML::BeginSeq;
			for (const auto &tag : def.Tags)
				out << YAML::Value << tag;
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<InventoryComponent>())
		{
			out << YAML::Key << "InventoryComponent";
			out << YAML::BeginMap;
			auto &inv = entity.GetComponent<InventoryComponent>();
			// MaxWeight and MaxSlots are the inventory's limits; Items below is its
			// contents. An inventory whose limits are saved but whose contents were
			// not would be a very confusing bug, so all three travel together.
			out << YAML::Key << "MaxWeight" << YAML::Value << inv.MaxWeight;
			// Limits before contents. The order is the writer's and the reader's,
			// which makes the two blocks line up when compared side by side.
			out << YAML::Key << "MaxSlots" << YAML::Value << inv.MaxSlots;
			out << YAML::Key << "Items" << YAML::Value << YAML::BeginSeq;
			// Contents are saved. An inventory that resets on reload is the
			// most common complaint about a save system, and a scene file IS
			// a save of authored-plus-current state.
			// Each stack becomes a nested map of ItemId and Quantity. The read side
			// validates each row, so a hand-edited stack with an empty id or a
			// non-positive quantity is dropped rather than loaded.
			for (const auto &stack : inv.Items)
			{
				out << YAML::Value << YAML::BeginMap;
				// ItemId and Quantity per stack -- the whole persistent shape of an
				// inventory row. Nothing else about a stack survives a round trip.
				out << YAML::Key << "ItemId" << YAML::Value << stack.ItemId;
				out << YAML::Key << "Quantity" << YAML::Value << stack.Quantity;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<DroppedItemComponent>())
		{
			out << YAML::Key << "DroppedItemComponent";
			out << YAML::BeginMap;
			auto &pile = entity.GetComponent<DroppedItemComponent>();
			// IsValid() gates the WHOLE block rather than each field,
			// unlike the InventoryComponent row loop above. A pile with an
			// empty id or a zero quantity is not a pile: PickUpDroppedItem
			// refuses it and nothing in the engine can use it, so writing
			// it would only produce a file that loads back into a
			// permanently inert entity. The reader makes the same check.
			//
			// Consequence worth knowing: a hand-placed pile in the editor
			// that has not been given an id yet will not survive a save.
			// That is the honest outcome -- an id-less pile has no
			// meaning -- and it fails the same way every reload.
			if (pile.IsValid())
			{
				out << YAML::Key << "ItemId" << YAML::Value << pile.ItemId;
				out << YAML::Key << "Quantity" << YAML::Value << pile.Quantity;
			}
			out << YAML::EndMap;
		}

		// --- Audio (14.4) ---
		//
		// These two had NO writer and NO reader until now, so a saved
		// scene carried no sound set at all: clips load by path at
		// runtime, and reopening a level lost every audio source on it.
		// The omission was invisible because the components were fully
		// bound and fully working -- only persistence was missing --
		// so nothing errored, nothing warned, and a scene simply came
		// back silent.
		//
		// Every AUTHORED field travels. MinPlayTime does NOT: it is the
		// remaining seconds of a triggered sound's minimum play time, which
		// is mid-playback runtime state. Writing it would make a scene
		// loaded mid-tail refuse to retrigger, so it is left at 0 exactly
		// as it is for a live component.
		if (entity.HasComponent<AudioSourceComponent>())
		{
			out << YAML::Key << "AudioSourceComponent";
			out << YAML::BeginMap;
			auto &src = entity.GetComponent<AudioSourceComponent>();
			out << YAML::Key << "ClipName" << YAML::Value << src.ClipName;
			out << YAML::Key << "Volume" << YAML::Value << src.Volume;
			out << YAML::Key << "Pitch" << YAML::Value << src.Pitch;
			out << YAML::Key << "Loop" << YAML::Value << src.Loop;
			// Priority is a voice-arbitration number the mixer reads at
			// Play time, so it is authored data like the rest -- there is no
			// runtime default that could stand in for it.
			out << YAML::Key << "Priority" << YAML::Value << src.Priority;
			out << YAML::Key << "Spatial" << YAML::Value << src.Spatial;
			// PositionOffset is the local-space nudge that places a muzzle
			// or a foot without a child entity. It is the one field that
			// needs the glm converter installed at the top of this file,
			// and it round-trips as a flow sequence like Translation.
			out << YAML::Key << "PositionOffset" << YAML::Value << src.PositionOffset;
			out << YAML::Key << "MinDistance" << YAML::Value << src.MinDistance;
			out << YAML::Key << "MaxDistance" << YAML::Value << src.MaxDistance;
			// PlayOnStart is DELIBERATELY NOT WRITTEN. See the long note at
			// the reader below, which is where the decision has to be
			// visible to anyone changing it.
			out << YAML::EndMap;
		}

		// The listener travels for the same reason the source does: a
		// scene whose listener is gone has its 3D panning driven by the
		// primary-camera fallback, which is a different sound mix with
		// nothing in the file saying so. One bool, and the authored
		// choice survives the round trip.
		if (entity.HasComponent<AudioListenerComponent>())
		{
			out << YAML::Key << "AudioListenerComponent";
			out << YAML::BeginMap;
			auto &listener = entity.GetComponent<AudioListenerComponent>();
			out << YAML::Key << "Enabled" << YAML::Value << listener.Enabled;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			out << YAML::Key << "SphereCollider3DComponent";
			out << YAML::BeginMap;
			auto &sc = entity.GetComponent<SphereCollider3DComponent>();
			out << YAML::Key << "Radius" << YAML::Value << sc.Radius;
			out << YAML::Key << "Offset" << YAML::Value << sc.Offset;
			out << YAML::Key << "Density" << YAML::Value << sc.Density;
			out << YAML::Key << "Friction" << YAML::Value << sc.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << sc.Restitution;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			out << YAML::Key << "CapsuleCollider3DComponent";
			out << YAML::BeginMap;
			auto &cc = entity.GetComponent<CapsuleCollider3DComponent>();
			out << YAML::Key << "Radius" << YAML::Value << cc.Radius;
			out << YAML::Key << "Height" << YAML::Value << cc.Height;
			out << YAML::Key << "Offset" << YAML::Value << cc.Offset;
			out << YAML::Key << "Density" << YAML::Value << cc.Density;
			out << YAML::Key << "Friction" << YAML::Value << cc.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << cc.Restitution;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<VolumetricFogComponent>())
		{
			out << YAML::Key << "VolumetricFogComponent";
			out << YAML::BeginMap;
			auto &fog = entity.GetComponent<VolumetricFogComponent>();
			// The fog block's first field after Enabled. It is a vec3 widened to a
			// vec4 like every other colour, so the file stores four numbers for a
			// three-number value -- and the reader narrows them straight back.
			out << YAML::Key << "Enabled" << YAML::Value << fog.Enabled;
			// glm::vec4(fog.Color, 1.0f): the component stores a vec3 but the vec4
			// converter is what exists, so alpha is manufactured as 1.0 and discarded
			// on load by the glm::vec3(...) narrowing at line 1492. Every vec3 colour
			// in this file is widened to a 4-element sequence and narrowed back, so
			// the on-disk format for a fog colour carries an element nothing reads.
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(fog.Color, 1.0f);
			out << YAML::Key << "Density" << YAML::Value << fog.Density;
			out << YAML::Key << "Anisotropy" << YAML::Value << fog.Anisotropy;
			out << YAML::Key << "HeightFalloff" << YAML::Value << fog.HeightFalloff;
			out << YAML::Key << "NoiseStrength" << YAML::Value << fog.NoiseStrength;
			out << YAML::Key << "NoiseScale" << YAML::Value << fog.NoiseScale;
			// MaxSteps is written even though it is a rendering budget rather than an
			// authored property -- a scene that renders slowly on load because of a
			// saved MaxSteps is a confusing failure to diagnose.
			out << YAML::Key << "WindSpeed" << YAML::Value << fog.WindSpeed;
			// MaxSteps last: the march-step budget is a rendering cost control rather
			// than an authored property, and it is the field most likely to be raised
			// by hand when the fog looks wrong.
			out << YAML::Key << "MaxSteps" << YAML::Value << fog.MaxSteps;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<VolumetricCloudsComponent>())
		{
			out << YAML::Key << "VolumetricCloudsComponent";
			out << YAML::BeginMap;
			auto &clouds = entity.GetComponent<VolumetricCloudsComponent>();
			// AmbientTint is the second colour: the light the clouds are lit by,
			// separate from the cloud body colour. Two colours because a cloud is lit
			// by something other than itself.
			out << YAML::Key << "Enabled" << YAML::Value << clouds.Enabled;
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(clouds.Color, 1.0f);
			out << YAML::Key << "AmbientTint" << YAML::Value << glm::vec4(clouds.AmbientTint, 1.0f);
			out << YAML::Key << "Coverage" << YAML::Value << clouds.Coverage;
			out << YAML::Key << "Density" << YAML::Value << clouds.Density;
			out << YAML::Key << "NoiseScale" << YAML::Value << clouds.NoiseScale;
			out << YAML::Key << "DetailAmount" << YAML::Value << clouds.DetailAmount;
			out << YAML::Key << "WindDirection" << YAML::Value << clouds.WindDirection;
			out << YAML::Key << "WindSpeed" << YAML::Value << clouds.WindSpeed;
			out << YAML::Key << "SilverLining" << YAML::Value << clouds.SilverLining;
			out << YAML::Key << "ShadowStrength" << YAML::Value << clouds.ShadowStrength;
			out << YAML::Key << "MaxSteps" << YAML::Value << clouds.MaxSteps;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<DirectionalLightComponent>())
		{
			// DirectionalLight is the smallest light: a colour and an intensity. Every
			// light block below starts the same way, then adds what its falloff
			// needs. The vec4 wrapping is shared by all five.
			out << YAML::Key << "DirectionalLightComponent";
			out << YAML::BeginMap;
			auto &light = entity.GetComponent<DirectionalLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(light.Color, 1.0f);
			out << YAML::Key << "Intensity" << YAML::Value << light.Intensity;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<PointLightComponent>())
		{
			// Point adds Range and Falloff on top. Range is where the light reaches
			// zero; Falloff is the curve between there and the source -- two separate
			// knobs for what is really one visual property.
			out << YAML::Key << "PointLightComponent";
			out << YAML::BeginMap;
			auto &light = entity.GetComponent<PointLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(light.Color, 1.0f);
			out << YAML::Key << "Intensity" << YAML::Value << light.Intensity;
			out << YAML::Key << "Range" << YAML::Value << light.Range;
			out << YAML::Key << "Falloff" << YAML::Value << light.Falloff;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<SpotLightComponent>())
		{
			out << YAML::Key << "SpotLightComponent";
			out << YAML::BeginMap;
			auto &light = entity.GetComponent<SpotLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(light.Color, 1.0f);
			out << YAML::Key << "Intensity" << YAML::Value << light.Intensity;
			out << YAML::Key << "Range" << YAML::Value << light.Range;
			out << YAML::Key << "Falloff" << YAML::Value << light.Falloff;
			out << YAML::Key << "InnerAngle" << YAML::Value << light.InnerAngle;
			// Spot adds the cone angles. InnerAngle is where the cone is fully bright
			// and OuterAngle where it reaches zero, so the pair defines both the
			// softness and the spread of the edge.
			out << YAML::Key << "OuterAngle" << YAML::Value << light.OuterAngle;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<AreaLightComponent>())
		{
			out << YAML::Key << "AreaLightComponent";
			out << YAML::BeginMap;
			auto &light = entity.GetComponent<AreaLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(light.Color, 1.0f);
			out << YAML::Key << "Intensity" << YAML::Value << light.Intensity;
			out << YAML::Key << "Width" << YAML::Value << light.Width;
			// Area lights are the odd ones out: they add Width and Height because they
			// are an emitting RECTANGLE, and DoubleSided so a single-sided quad can
			// still light from behind.
			out << YAML::Key << "Height" << YAML::Value << light.Height;
			out << YAML::Key << "Range" << YAML::Value << light.Range;
			out << YAML::Key << "DoubleSided" << YAML::Value << light.DoubleSided;
			out << YAML::EndMap;
		}

		// The largest block in the function at 25 fields. Every one is a float or
		// vec2/vec4 and every one is read back unconditionally on the other side,
		// so this block is a genuine round trip -- unlike the Friction and
		// TextureWidth/TextureHeight cases above.
		if (entity.HasComponent<WaterComponent>())
		{
			out << YAML::Key << "WaterComponent";
			out << YAML::BeginMap;
			auto &water = entity.GetComponent<WaterComponent>();
			out << YAML::Key << "Enabled" << YAML::Value << water.Enabled;
			out << YAML::Key << "BodyType" << YAML::Value << WaterBodyTypeToString(water.Type);
			// Three colours widened to vec4 like every other colour here. Shallow and
			// Deep are the two ends of the depth gradient; FoamColor is separate
			// because foam is not part of the gradient.
			out << YAML::Key << "ShallowColor" << YAML::Value << glm::vec4(water.ShallowColor, 1.0f);
			out << YAML::Key << "DeepColor" << YAML::Value << glm::vec4(water.DeepColor, 1.0f);
			out << YAML::Key << "FoamColor" << YAML::Value << glm::vec4(water.FoamColor, 1.0f);
			out << YAML::Key << "WaterDepth" << YAML::Value << water.WaterDepth;
			// The Gerstner wave triple -- amplitude, wavelength, speed -- plus the
			// direction that orients them. Chop and Steepness after it shape the
			// crest rather than the whole wave.
			out << YAML::Key << "WaveAmplitude" << YAML::Value << water.WaveAmplitude;
			out << YAML::Key << "WaveLength" << YAML::Value << water.WaveLength;
			out << YAML::Key << "WaveSpeed" << YAML::Value << water.WaveSpeed;
			out << YAML::Key << "WaveDirection" << YAML::Value << water.WaveDirection;
			out << YAML::Key << "Chop" << YAML::Value << water.Chop;
			out << YAML::Key << "Steepness" << YAML::Value << water.Steepness;
			out << YAML::Key << "FlowDirection" << YAML::Value << water.FlowDirection;
			out << YAML::Key << "FlowSpeed" << YAML::Value << water.FlowSpeed;
			// Foam is authored in two independent ways: FoamAmount/FoamScale for the
			// wave crests and ShoreFoam/ShoreWidth for the shoreline. They are not
			// derived from one another on load, so both sets are stored.
			out << YAML::Key << "FoamAmount" << YAML::Value << water.FoamAmount;
			out << YAML::Key << "FoamScale" << YAML::Value << water.FoamScale;
			out << YAML::Key << "ShoreFoam" << YAML::Value << water.ShoreFoam;
			out << YAML::Key << "ShoreWidth" << YAML::Value << water.ShoreWidth;
			out << YAML::Key << "Transparency" << YAML::Value << water.Transparency;
			out << YAML::Key << "Roughness" << YAML::Value << water.Roughness;
			// Roughness, Metallic and Segments are the standard PBR pair plus the mesh
			// resolution. Segments is a rendering budget rather than an authored
			// property, like MaxSteps in the fog block above.
			out << YAML::Key << "Metallic" << YAML::Value << water.Metallic;
			out << YAML::Key << "Segments" << YAML::Value << water.Segments;
			out << YAML::Key << "TimeScale" << YAML::Value << water.TimeScale;
			out << YAML::Key << "Buoyancy" << YAML::Value << water.Buoyancy;
			out << YAML::Key << "WaterDensity" << YAML::Value << water.WaterDensity;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<ScriptComponent>())
		{
			out << YAML::Key << "ScriptComponent";
			out << YAML::BeginMap; // ScriptComponent

			auto &sc = entity.GetComponent<ScriptComponent>();
			// ScriptPath is rewritten before it is written, unlike every other field in
			// the file. The rewrite here and the one on load disagree: this side keeps
			// the subdirectory under assets/ (see line 968), while the reader at line
			// 1661 keeps only the FILENAME and re-prefixes "scripts/".
			// DEFECT: a scene saved with assets/scripts/enemies/orc.lua loads as
			// scripts/orc.lua. The directory structure is lost on every round trip,
			// so two scripts with the same basename in different folders collide.
			std::string pathToSave = sc.ScriptPath;
			std::replace(pathToSave.begin(), pathToSave.end(), '\\', '/');

			if (std::filesystem::path(pathToSave).is_absolute())
			{
				const std::string marker = "/assets/";
				auto pos = pathToSave.find(marker);
				if (pos != std::string::npos)
					pathToSave = "assets/" + pathToSave.substr(pos + marker.size());
				else
					pathToSave = "assets/scripts/" + std::filesystem::path(pathToSave).filename().string();
			}
			else if (pathToSave.find("assets/") == std::string::npos)
			{
				pathToSave = "assets/" + pathToSave;
			}

			out << YAML::Key << "ScriptPath" << YAML::Value << pathToSave;
			out << YAML::Key << "ScriptName" << YAML::Value << sc.ScriptName;

			// ScriptComponent's EndMap. The path was rewritten above; only the
			// normalised form reaches the file.
			out << YAML::EndMap; // ScriptComponent
		}

		// Closes the entity mapping opened at line 359. Every component block
		// above balanced its own map, so this single EndMap is what makes the
		// next entity in the sequence start at the right nesting level.
		out << YAML::EndMap; // Entity
	}

	// Writes the whole scene to a YAML file.
	//
	// Emits Scene and Entities, plus Terrain metadata only when the scene has
	// a configured terrain source. Terrain edits stay in project-root sidecars.
	//
	// The iteration goes through m_Registry.view<entt::entity>().each, which
	// visits every live entity in storage order. That order is an entt detail,
	// not a stable or meaningful one -- so a saved file's entity order can
	// differ between runs, producing noisy diffs for the same scene.
	//
	// Writes through a checked same-directory temporary and publishes only
	// after the complete YAML and any dirty terrain sidecars have been saved.
	//
	// @param filepath  Where to write. A relative path resolves against the
	//                  process's current working directory, NOT the project
	//                  root -- so the same call from a different launch
	//                  directory writes the scene somewhere else.
	bool SceneSerializer::Serialize(const std::string &filepath)
	{
		return SerializeChecked(filepath, nullptr);
	}

	bool SceneSerializer::SerializeChecked(const std::string &filepath, std::string *error)
	{
		if (error)
			error->clear();
		const auto fail = [error](const std::string &message)
		{
			if (error)
				*error = message;
			return false;
		};

		const bool terrainConfigured = m_Scene->m_TerrainRuntime.IsConfigured();
		std::string terrainYaml;
		if (terrainConfigured)
		{
			Project &project = Project::Get();
			if (!project.IsOpen())
				return fail("saving configured terrain requires an open project");
			if (m_Scene->m_TerrainAssetReference.empty())
				return fail("configured terrain has no authored asset reference");

			std::filesystem::path assetPath;
			std::string terrainError;
			if (!Terrain::ResolveTerrainAssetReference(project.GetAssetRoot(),
				m_Scene->m_TerrainAssetReference, assetPath, &terrainError))
				return fail("invalid terrain asset reference: " + terrainError);

			Terrain::TerrainAsset referencedAsset;
			if (!Terrain::LoadTerrainAsset(assetPath, referencedAsset, &terrainError))
				return fail("could not validate terrain asset: " + terrainError);
			if (Terrain::TerrainAssetFingerprint(referencedAsset) !=
				Terrain::TerrainAssetFingerprint(m_Scene->m_TerrainRuntime.GetAsset()))
				return fail("terrain reference does not match the configured terrain asset");

			Terrain::TerrainSceneMetadata metadata;
			metadata.AssetReference = m_Scene->m_TerrainAssetReference;
			metadata.Streaming = m_Scene->m_TerrainRuntime.GetConfig();
			if (!Terrain::EncodeTerrainSceneMetadataYaml(metadata, terrainYaml, &terrainError))
				return fail("could not encode terrain scene metadata: " + terrainError);
		}

		// A default-constructed Emitter, not a Node: this writes through the
		// operator<< overloads defined at the top of the file, including the four
		// glm vector ones. The convert specializations are for the LOAD path.
		YAML::Emitter out;
		try
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Scene" << YAML::Value << "Untitled";
			if (terrainConfigured)
			{
				const YAML::Node terrainNode = YAML::Load(terrainYaml);
				out << YAML::Key << "Terrain" << YAML::Value << terrainNode;
			}
			// The trained neural stage's enabled flag. Written as its own top-level
			// map rather than a bare scalar so a later field can be added without
			// re-parsing existing files, and so the key is self-describing.
			//
			// yaml-cpp ignores keys it does not map, so a scene written before this
			// key existed still loads -- and a scene written WITH it loads fine in a
			// build that does not know about it. That is the whole reason this is
			// safe to add to a format every scene file already uses.
			out << YAML::Key << "NeuralStage" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Enabled" << YAML::Value
				<< (m_Scene->IsNeuralStageEnabled() ? "true" : "false");
			out << YAML::EndMap;

			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
			m_Scene->m_Registry.view<entt::entity>().each([&](auto entityID)
			{
				Entity entity = { entityID, m_Scene.get() };
				if (!entity)
					return;
				SerializeEntity(out, entity);
			});
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}
		catch (const YAML::Exception &exception)
		{
			return fail(std::string("could not emit scene YAML: ") + exception.what());
		}
		if (!out.good())
			return fail(std::string("could not emit scene YAML: ") + out.GetLastError());

		const std::string yamlText(out.c_str(), out.size());
		const std::filesystem::path targetPath(filepath);
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		std::filesystem::path temporaryPath = targetPath;
		temporaryPath += ".tmp." + std::to_string(stamp);
		std::ofstream outFile(temporaryPath, std::ios::binary | std::ios::trunc);
		if (!outFile.is_open())
			return fail("could not open temporary scene file for writing: " + temporaryPath.string());
		outFile.write(yamlText.data(), static_cast<std::streamsize>(yamlText.size()));
		if (!outFile.good())
		{
			outFile.close();
			std::error_code ignored;
			std::filesystem::remove(temporaryPath, ignored);
			return fail("failed while writing temporary scene file: " + temporaryPath.string());
		}
		outFile.flush();
		if (!outFile.good())
		{
			outFile.close();
			std::error_code ignored;
			std::filesystem::remove(temporaryPath, ignored);
			return fail("failed while flushing temporary scene file: " + temporaryPath.string());
		}
		outFile.close();
		if (outFile.fail())
		{
			std::error_code ignored;
			std::filesystem::remove(temporaryPath, ignored);
			return fail("failed while closing temporary scene file: " + temporaryPath.string());
		}

		if (terrainConfigured)
		{
			std::string terrainError;
			if (!m_Scene->m_TerrainRuntime.SaveWorldState(Project::Get().GetRoot(), &terrainError))
			{
				std::error_code ignored;
				std::filesystem::remove(temporaryPath, ignored);
				return fail("could not save terrain sidecars: " + terrainError);
			}
		}

		std::error_code publishError;
		std::filesystem::rename(temporaryPath, targetPath, publishError);
		if (publishError)
		{
			std::error_code ignored;
			std::filesystem::remove(temporaryPath, ignored);
			return fail("could not publish scene file: " + publishError.message());
		}

		// The YAML is now the authoritative file on disk, so a binary sidecar can be
		// derived from it. This must happen AFTER the rename: the sidecar records the
		// published file's size and write time as its staleness key.
		//
		// Failure here is never fatal and never reported to the caller. A sidecar is a
		// cache; the only way Write can fail now is an I/O error. Remove() is still called
		// so a sidecar from an earlier save cannot be served against this new YAML --
		// that would be the one way a stale cache could show wrong data.
		{
			// Entities the fast record cannot hold (an unsupported component, a resolved
			// texture, an over-long tag) are stored as their authored YAML. This is the
			// only writer in the file, so it is the only place that knows how a component
			// becomes text.
			const SceneChunkStore::FragmentEmitter emitFragment =
				[](Scene &scene, const entt::entity entityID)
			{
				YAML::Emitter emitter;
				SerializeEntity(emitter, Entity(entityID, &scene));
				return std::string(emitter.c_str(), emitter.size());
			};

			std::string sidecarReason;
			if (!SceneChunkStore::Write(*m_Scene, targetPath, emitFragment, &sidecarReason))
			{
				SceneChunkStore::Remove(targetPath);
				WL_CORE_INFO("No scene sidecar for '{0}': {1}", filepath, sidecarReason);
			}
		}
		return true;
	}

	// NOT IMPLEMENTED.
	//
	// Declared in the header and bound to Python, but does nothing. There is
	// no runtime format writer anywhere in this file.
	//
	// The assert is the only signal, and it is a weak one: WL_CORE_ASSERT
	// expands to NOTHING in a Release build (see Core.h), so a release build
	// compiles this to an empty function that returns successfully. Since
	// Bindings.cpp exposes it to Python, a script calling
	// scene.serialize_runtime(path) gets no file, no exception, and no error
	// -- in Debug it stops in the debugger, and in Release it looks like it
	// worked.
	//
	// @param filepath  Ignored.
	void SceneSerializer::SerializeRuntime(const std::string &filepath)
	{
		// Not implemented
		WL_CORE_ASSERT(false, "SerializeRuntime is not implemented!");
	}

	// Reads a whole scene from a YAML file, replacing the current scene's
	// contents.
	//
	// Terrain metadata, source asset, and sidecars are preflighted before the
	// entity reader starts; the staged terrain runtime is committed afterward.
	//
	// Reconstruction is strictly manual -- no schema, no reflection -- so
	// every component's fields are read back by name in the mirror image of
	// SerializeEntity. An absent key leaves that field at its component's
	// default, and a component absent from the file is not created at all.
	// That is the whole error model: a missing key means default, never an
	// error.
	//
	// Because the converters above THROW rather than return false for a
	// malformed value (see convert<glm::vec3>::decode), a hand-edited file
	// with a bad number inside a correctly-sized vector throws out of this
	// function instead of returning false. The bool therefore does not cover
	// every way a load can fail.
	//
	// @param filepath  The file to read.
	// @returns         Whether the scene was loaded. False for a file that
	//                 could not be opened or whose contents are not a scene.
	// Reads ONE entity from its YAML node into the scene.
	//
	// Extracted from Deserialize's loop so the binary sidecar can feed individual
	// entities back through the same reader. That matters because a scene is only
	// fully representable in the sidecar when every entity's components are in the
	// supported set; rather than refuse such a scene entirely, the sidecar stores the
	// handful of unsupported entities as their original YAML and replays them here.
	// One reader means the two paths cannot drift.
	//
	// @param entity  The entity's node, as read from an "Entities" sequence.
	// @return        Always true today. Present so a future per-entity failure can be
	//                reported without changing every call site.
	bool SceneSerializer::DeserializeEntityNode(const YAML::Node &entity)
	{
		// The identity is read UNCONDITIONALLY. A file hand-edited to drop the
		// "Entity" key throws here rather than skipping the entity, because
		// as<uint64_t>() on a missing node is a typed-conversion failure.
		uint64_t uuid = entity["Entity"].as<uint64_t>(0);

		std::string name;
		// The tag is optional and read BEFORE the entity is created, because its
		// value is a constructor argument. An entity with no TagComponent key
		// therefore still gets created -- with Scene's default name.
		const auto tagComponent = entity["TagComponent"];
		if (tagComponent)
			// The tag is read before the entity exists, then handed to
			// CreateEntityWithUUID below. An entity with no TagComponent key loads with
			// an empty name, which Scene::CreateEntityWithUUID turns into "Entity".
			name = tagComponent["Tag"].as<std::string>();

		// Traced per entity. In a Release build WL_CORE_TRACE compiles out, so
		// this is a Debug-only record of load progress.
		WL_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

		// Creation happens once per file entry, BEFORE any component is read. So a
		// file whose later component throws still leaves a half-built entity in the
		// registry -- the exception path has no rollback.
		// The method also never clears the registry first, despite its own comment
		// saying it replaces the scene's contents: entities from a previous load
		// survive and accumulate.
		Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto transformComponent = entity["TransformComponent"];
		// Guarded by node truthiness, but the three fields inside are read
		// unguarded. This is the OLDEST block in the function and the pattern the
		// newer blocks below it deliberately moved away from.
		if (transformComponent)
		{
			// Entities always have transforms
			// GetComponent, not AddComponent, because CreateEntityWithUUID above
			// already attached a TransformComponent. Every other block here uses
			// AddComponent, which is the asymmetry to watch when adding a new one.
			auto &tc = deserializedEntity.GetComponent<TransformComponent>();
			// All three vectors presence-checked: a partial node leaves defaults.
			if (transformComponent["Translation"])
				tc.Translation = transformComponent["Translation"].as<glm::vec3>();
			if (transformComponent["Rotation"])
				tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
			if (transformComponent["Scale"])
				tc.Scale = transformComponent["Scale"].as<glm::vec3>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto healthNode = entity["HealthComponent"];
		if (healthNode)
		{
			// AddComponent, then every field presence-checked. This is the pattern the
			// newer blocks follow and the older ones do not: a missing key leaves the
			// component's default rather than throwing.
			auto &hc = deserializedEntity.AddComponent<HealthComponent>();
			// Every field presence-checked: a scene authored before a
			// field existed must still load, keeping the default.
			if (healthNode["Current"])
				// The presence-check idiom: read the key only if it exists, otherwise
				// leave the component's default alone. Every field in this block uses it,
				// which is why a scene authored before a field existed still loads.
				hc.Current = healthNode["Current"].as<float>();
			// Health is the first component in the READER, though it is written
			// later in the writer. The order of the reader does not have to match the
			// writer's -- YAML mappings are unordered -- so the two halves drifted.
			if (healthNode["Max"])
				hc.Max = healthNode["Max"].as<float>();
			if (healthNode["Invulnerable"])
				hc.Invulnerable = healthNode["Invulnerable"].as<bool>();
			if (healthNode["DamageCooldown"])
				hc.DamageCooldown = healthNode["DamageCooldown"].as<float>();
			if (healthNode["IsDead"])
				// CooldownTimer is deliberately absent from the read, matching its absence
				// from the write. Every accumulator that is not written is also not read, so
				// the two halves agree on what 'resting state' means.
				hc.IsDead = healthNode["IsDead"].as<bool>();
			// CooldownTimer stays 0 -- never serialized, never resumed.
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto weaponNode = entity["WeaponComponent"];
		if (weaponNode)
		{
			// AddComponent attaches a fresh component; the component's own defaults
			// are what every unchecked field would otherwise keep. So the presence
			// checks below are what make an old scene file load rather than reset.
			auto &wc = deserializedEntity.AddComponent<WeaponComponent>();
			// Weapon's AddComponent. Compare the health block above: both check every
			// field, which is the convention the rest of this function follows and the
			// renderer blocks below do not.
			if (weaponNode["FireRate"])
				wc.FireRate = weaponNode["FireRate"].as<float>();
			if (weaponNode["Damage"])
				wc.Damage = weaponNode["Damage"].as<float>();
			if (weaponNode["Range"])
				wc.Range = weaponNode["Range"].as<float>();
			if (weaponNode["AmmoPerShot"])
				wc.AmmoPerShot = weaponNode["AmmoPerShot"].as<int>();
			if (weaponNode["CurrentAmmo"])
				wc.CurrentAmmo = weaponNode["CurrentAmmo"].as<int>();
			if (weaponNode["MaxAmmo"])
				wc.MaxAmmo = weaponNode["MaxAmmo"].as<int>();
			if (weaponNode["ReserveAmmo"])
				wc.ReserveAmmo = weaponNode["ReserveAmmo"].as<int>();
			if (weaponNode["ReloadTime"])
				wc.ReloadTime = weaponNode["ReloadTime"].as<float>();
			// AutoReload is the last authored weapon field. Everything after it -- the
			// reload progress, the shots fired, the live cooldowns -- is deliberately
			// absent from both halves, which is what makes a reload start clean.
			if (weaponNode["AutoReload"])
				wc.AutoReload = weaponNode["AutoReload"].as<bool>();
			// FireCooldown / ReloadTimer / IsReloading / ShotsFired
			// stay at their defaults; a load does not resume a reload.
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto interactableNode = entity["InteractableComponent"];
		if (interactableNode)
		{
			// The label is presentation text stored in the scene file, which is why
			// it round-trips: an interactable's prompt is part of the level design,
			// not of the UI code.
			auto &ic = deserializedEntity.AddComponent<InteractableComponent>();
			if (interactableNode["Enabled"])
				ic.Enabled = interactableNode["Enabled"].as<bool>();
			if (interactableNode["Label"])
				ic.Label = interactableNode["Label"].as<std::string>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto weaponFeedbackNode = entity["WeaponFeedbackComponent"];
		if (weaponFeedbackNode)
		{
			// WeaponFeedback holds only durations. The live timers are zeroed by
			// OnComponentAdded on load, which is why nothing needs to be read here.
			auto &fb = deserializedEntity.AddComponent<WeaponFeedbackComponent>();
			if (weaponFeedbackNode["MuzzleFlashDuration"])
				fb.MuzzleFlashDuration = weaponFeedbackNode["MuzzleFlashDuration"].as<float>();
			if (weaponFeedbackNode["HitMarkerDuration"])
				fb.HitMarkerDuration = weaponFeedbackNode["HitMarkerDuration"].as<float>();
			// Live timers are not written, and OnComponentAdded zeroes
			// them, so a loaded gun never flashes spuriously.
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto viewStateNode = entity["ViewStateComponent"];
		if (viewStateNode)
		{
			auto &vs = deserializedEntity.AddComponent<ViewStateComponent>();
			if (viewStateNode["TargetFOV"])
				vs.TargetFOV = viewStateNode["TargetFOV"].as<float>();
			if (viewStateNode["FOVLerpSpeed"])
				vs.FOVLerpSpeed = viewStateNode["FOVLerpSpeed"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto itemDefinitionNode = entity["ItemDefinitionComponent"];
		if (itemDefinitionNode)
		{
			// ItemDefinition is added before its fields are read, and Name -- the
			// lookup id -- is read first. A definition whose name is missing throws
			// on the very first assignment in the block.
			auto &def = deserializedEntity.AddComponent<ItemDefinitionComponent>();
			if (itemDefinitionNode["Name"])
				def.Name = itemDefinitionNode["Name"].as<std::string>();
			if (itemDefinitionNode["DisplayName"])
				def.DisplayName = itemDefinitionNode["DisplayName"].as<std::string>();
			if (itemDefinitionNode["Description"])
				def.Description = itemDefinitionNode["Description"].as<std::string>();
			if (itemDefinitionNode["MaxStack"])
				def.MaxStack = itemDefinitionNode["MaxStack"].as<int>();
			if (itemDefinitionNode["Weight"])
				def.Weight = itemDefinitionNode["Weight"].as<float>();
			if (itemDefinitionNode["Tags"])
			{
				// clear() before the loop, so a re-authored tag list replaces rather than
				// appends. Every other vector field in this file is assigned outright;
				// this one needed clearing because the loop appends.
				def.Tags.clear();
				// Iterating the node directly rather than by index: yaml-cpp's Node
				// exposes begin/end over its sequence elements, so this is the
				// range-for over a YAML sequence.
				for (const auto &tag : itemDefinitionNode["Tags"])
					def.Tags.push_back(tag.as<std::string>());
			}
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto inventoryNode = entity["InventoryComponent"];
		if (inventoryNode)
		{
			// The component is created before its contents are known, so a scene with
			// an empty inventory and one with no InventoryComponent key at all are
			// distinguishable: the first has the key and an empty Items list.
			auto &inv = deserializedEntity.AddComponent<InventoryComponent>();
			// Both limits are presence-checked, unlike the item rows below, so an
			// inventory written before a limit existed loads with the default limit
			// rather than throwing.
			if (inventoryNode["MaxWeight"])
				inv.MaxWeight = inventoryNode["MaxWeight"].as<float>();
			if (inventoryNode["MaxSlots"])
				inv.MaxSlots = inventoryNode["MaxSlots"].as<int>();
			if (inventoryNode["Items"])
			{
				// Same clear-then-fill shape as the tags above: the file is authoritative
				// for the whole list, not a delta applied to whatever was there.
				inv.Items.clear();
				for (const auto &stackNode : inventoryNode["Items"])
				{
					// Skip rows with no ItemId rather than adding an
					// empty stack: an empty row still consumes a slot.
					// A row with no ItemId is SKIPPED rather than loaded as an empty stack.
					// That is the one place in the reader where malformed input is discarded
					// instead of throwing -- and it makes a typo'd id key silently drop a row.
					if (!stackNode["ItemId"])
						return true;
					Wasteland::ItemStack stack;
					// ItemId is read after the `if (!stackNode["ItemId"])` guard above, so
					// the conversion is known to have a string behind it. Quantity is
					// conditional, so a missing one takes the explicit 1 on the next line.
					stack.ItemId = stackNode["ItemId"].as<std::string>();
					// Quantity defaults to 1 when the key is absent, rather than the struct's
					// own default of 1. Same value, but stated here, so changing the struct's
					// default would not change what a hand-edited file loads as.
					stack.Quantity = stackNode["Quantity"] ? stackNode["Quantity"].as<int>() : 1;
					// IsValid() is the gate: a stack with an empty ItemId or a quantity <= 0 is
					// dropped. The writer never emits such a stack, so this only rejects rows
					// introduced by hand-editing.
					if (stack.IsValid())
						// push_back, not assignment: the read appends to the cleared vector.
						// A duplicate ItemId in the file therefore produces two stacks rather
						// than one merged stack -- IsValid does not deduplicate.
						inv.Items.push_back(stack);
				}
			}
		}

		// Mirrors the writer's whole-block IsValid() gate exactly: a pile
		// whose ItemId is missing or empty, or whose Quantity is missing,
		// zero or negative, is DISCARDED rather than loaded. Adding the
		// component and scrubbing it afterwards would leave an inert entity
		// in the world that looks like an item and can never be picked up.
		//
		// Quantity defaults to 1 when the key is absent -- the same explicit
		// 1 the InventoryComponent stack above uses, so a pile hand-written
		// with only an id loads as ONE item rather than as zero (which would
		// be dropped) or as the struct's own default quietly changing under us.
		const auto droppedItemNode = entity["DroppedItemComponent"];
		if (droppedItemNode)
		{
			std::string itemId = droppedItemNode["ItemId"] ? droppedItemNode["ItemId"].as<std::string>() : std::string();
			int quantity = droppedItemNode["Quantity"] ? droppedItemNode["Quantity"].as<int>() : 1;
			if (!itemId.empty() && quantity > 0)
			{
				auto &pile = deserializedEntity.AddComponent<DroppedItemComponent>();
				pile.ItemId = itemId;
				pile.Quantity = quantity;
			}
		}

		// --- Audio (14.4) ---
		//
		// Every field is presence-checked, exactly like the InventoryComponent
		// limits above and unlike the ViewStateComponent block that follows. A
		// scene file written before a field existed must load with that field's
		// COMPONENT DEFAULT rather than throwing: an audio clip that has been
		// loading by path since 14.4 must not stop loading because a key is
		// missing from an old file.
		const auto audioSourceNode = entity["AudioSourceComponent"];
		if (audioSourceNode)
		{
			// Added before its fields are read, so an authored source with every
			// field at its default round-trips as a source rather than vanishing.
			auto &src = deserializedEntity.AddComponent<AudioSourceComponent>();
			if (audioSourceNode["ClipName"])
				src.ClipName = audioSourceNode["ClipName"].as<std::string>();
			if (audioSourceNode["Volume"])
				src.Volume = audioSourceNode["Volume"].as<float>();
			if (audioSourceNode["Pitch"])
				src.Pitch = audioSourceNode["Pitch"].as<float>();
			if (audioSourceNode["Loop"])
				src.Loop = audioSourceNode["Loop"].as<bool>();
			if (audioSourceNode["Priority"])
				src.Priority = audioSourceNode["Priority"].as<int>();
			if (audioSourceNode["Spatial"])
				src.Spatial = audioSourceNode["Spatial"].as<bool>();
			if (audioSourceNode["PositionOffset"])
				src.PositionOffset = audioSourceNode["PositionOffset"].as<glm::vec3>();
			if (audioSourceNode["MinDistance"])
				src.MinDistance = audioSourceNode["MinDistance"].as<float>();
			if (audioSourceNode["MaxDistance"])
				src.MaxDistance = audioSourceNode["MaxDistance"].as<float>();
			// MinPlayTime is left at 0, which is both the component default and
			// the correct restored state: it counts the tail of a sound that is
			// CURRENTLY playing, and a scene file describes an authored level
			// rather than a moment in one.
			//
			// PlayOnStart IS DELIBERATELY NOT RESTORED, and this is the
			// decision, recorded where anyone changing it will see it. The writer
			// emits no such key, so every loaded source reads PlayOnStart ==
			// false whatever the authoring tool had set.
			//
			// WHY: PlayOnStart is an ACTION REQUEST, not state. Round-tripping
			// it would make every reload of every level replay its autostart
			// sounds -- a door closing once per load, looping ambience
			// restarting on every scene swap -- which is the classic reason a
			// game "sounds wrong when you reopen the editor". The counterpart
			// is already in place: AudioSystem::PlayAutoStartSources fires
			// sources ONCE from OnRuntimeStart, and Deserialize never calls it,
			// so loading a scene mid-session is silent by construction rather
			// than by this omission.
			//
			// THE COST, stated plainly: an autostart flag authored in the editor
			// is LOST across a reload. If a future tool needs it, the honest fix
			// is a separate authored-only field on the editor path read into a
			// different attribute -- not a blind round trip of this one, because
			// the runtime save path shares this file and would replay on every load.
		}

		// Single bool, same presence-check convention.
		const auto audioListenerNode = entity["AudioListenerComponent"];
		if (audioListenerNode)
		{
			auto &listener = deserializedEntity.AddComponent<AudioListenerComponent>();
			if (audioListenerNode["Enabled"])
				listener.Enabled = audioListenerNode["Enabled"].as<bool>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto cameraComponent = entity["CameraComponent"];
		if (cameraComponent)
		{
			auto &cc = deserializedEntity.AddComponent<CameraComponent>();

				auto cameraProps = cameraComponent["Camera"];

				if (cameraProps)
				{
					if (cameraProps["ProjectionType"])
						cc.Camera.SetProjectionType((SceneCamera::ProjectionType)cameraProps["ProjectionType"].as<int>());
					if (cameraProps["PerspectiveFOV"])
						cc.Camera.SetPerspectiveVerticalFOV(cameraProps["PerspectiveFOV"].as<float>());
					if (cameraProps["PerspectiveNear"])
						cc.Camera.SetPerspectiveNearClip(cameraProps["PerspectiveNear"].as<float>());
					if (cameraProps["PerspectiveFar"])
						cc.Camera.SetPerspectiveFarClip(cameraProps["PerspectiveFar"].as<float>());
					if (cameraProps["OrthographicSize"])
						cc.Camera.SetOrthographicSize(cameraProps["OrthographicSize"].as<float>());
					if (cameraProps["OrthographicNear"])
						cc.Camera.SetOrthographicNearClip(cameraProps["OrthographicNear"].as<float>());
					if (cameraProps["OrthographicFar"])
						cc.Camera.SetOrthographicFarClip(cameraProps["OrthographicFar"].as<float>());
				}

				if (cameraComponent["Primary"])
					cc.Primary = cameraComponent["Primary"].as<bool>();
				if (cameraComponent["FixedAspectRatio"])
					cc.FixedAspectRatio = cameraComponent["FixedAspectRatio"].as<bool>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto spriteRendererComponent = entity["SpriteRendererComponent"];
		if (spriteRendererComponent)
		{
			// Entities always have transforms
			// AddComponent, then ONE unguarded read. SpriteRendererComponent has a
			// single field, so the block is the shortest in the function and the only
			// one with nothing to check.
			auto &src = deserializedEntity.AddComponent<SpriteRendererComponent>();
				if (spriteRendererComponent["Color"])
					src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto circleRendererComponent = entity["CircleRendererComponent"];
		// Guarded node, unguarded fields -- the pattern for every renderer block
		// below. See the note on the sprite block above for the consequence.
		if (circleRendererComponent)
		{
			// Entities always have transforms
			auto &crc = deserializedEntity.AddComponent<CircleRendererComponent>();
				if (circleRendererComponent["Color"])
					crc.Color = circleRendererComponent["Color"].as<glm::vec4>();
				if (circleRendererComponent["Thickness"])
					crc.Thickness = circleRendererComponent["Thickness"].as<float>();
				if (circleRendererComponent["Fade"])
					crc.Fade = circleRendererComponent["Fade"].as<float>();
		}

		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto cubeRendererComponent = entity["CubeRendererComponent"];
		if (cubeRendererComponent)
		{
			// Cube reads back only Colour, TextureIndex and TilingFactor -- the
			// TextureWidth/Height the writer emitted are dropped. See the defect note
			// on the writer side at line 442.
			auto &crc = deserializedEntity.AddComponent<CubeRendererComponent>();
				if (cubeRendererComponent["Color"])
					crc.Color = cubeRendererComponent["Color"].as<glm::vec4>();
				if (cubeRendererComponent["TextureIndex"])
					crc.TextureIndex = cubeRendererComponent["TextureIndex"].as<int>();
				if (cubeRendererComponent["TilingFactor"])
					crc.TilingFactor = cubeRendererComponent["TilingFactor"].as<float>();
		}

		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto sphereRendererComponent = entity["SphereRendererComponent"];
		if (sphereRendererComponent)
		{
			// Sphere's reader. Radius, Sectors and Stacks are the three fields that
			// matter for how the mesh regenerates; TextureIndex and TilingFactor are
			// the two texture fields. TextureWidth/Height are the two it skips.
			auto &src = deserializedEntity.AddComponent<SphereRendererComponent>();
				if (sphereRendererComponent["Color"])
					src.Color = sphereRendererComponent["Color"].as<glm::vec4>();
				if (sphereRendererComponent["Radius"])
					src.Radius = sphereRendererComponent["Radius"].as<float>();
				if (sphereRendererComponent["Sectors"])
					src.Sectors = sphereRendererComponent["Sectors"].as<int>();
				if (sphereRendererComponent["Stacks"])
					src.Stacks = sphereRendererComponent["Stacks"].as<int>();
				if (sphereRendererComponent["TextureIndex"])
					src.TextureIndex = sphereRendererComponent["TextureIndex"].as<int>();
				if (sphereRendererComponent["TilingFactor"])
					src.TilingFactor = sphereRendererComponent["TilingFactor"].as<float>();
		}

		// --- Blender "Add > Mesh" primitives ---
		// Each block is guarded by YAML node truthiness: a scene file
		// written before these shapes existed simply has no key, which
		// is what keeps older files loading.
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto planeRendererComponent = entity["PlaneRendererComponent"];
		if (planeRendererComponent)
		{
		// Every primitive reader from here to the torus has this same shape: add
		// the component, then assign each field straight from the node with no
		// presence check. The result is that a truncated node throws out of
		// Deserialize rather than leaving defaults behind.
		auto &prc = deserializedEntity.AddComponent<PlaneRendererComponent>();
			if (planeRendererComponent["Color"])
				prc.Color = planeRendererComponent["Color"].as<glm::vec4>();
			if (planeRendererComponent["Subdivisions"])
				prc.Subdivisions = planeRendererComponent["Subdivisions"].as<int>();
			if (planeRendererComponent["TextureIndex"])
				prc.TextureIndex = planeRendererComponent["TextureIndex"].as<int>();
			if (planeRendererComponent["TilingFactor"])
				prc.TilingFactor = planeRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto gridRendererComponent = entity["GridRendererComponent"];
		if (gridRendererComponent)
		{
		// Grid's reader: the two subdivision counts then the two texture fields.
		// The colour is read first and unguarded, so a grid node with no Color
		// key throws rather than defaulting to white.
		auto &grc = deserializedEntity.AddComponent<GridRendererComponent>();
			if (gridRendererComponent["Color"])
				grc.Color = gridRendererComponent["Color"].as<glm::vec4>();
			if (gridRendererComponent["XSubdivisions"])
				grc.XSubdivisions = gridRendererComponent["XSubdivisions"].as<int>();
			if (gridRendererComponent["YSubdivisions"])
				grc.YSubdivisions = gridRendererComponent["YSubdivisions"].as<int>();
			if (gridRendererComponent["TextureIndex"])
				grc.TextureIndex = gridRendererComponent["TextureIndex"].as<int>();
			if (gridRendererComponent["TilingFactor"])
				grc.TilingFactor = gridRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto circle3DRendererComponent = entity["Circle3DRendererComponent"];
		if (circle3DRendererComponent)
		{
		// Segments is read as int, matching the writer's own integer. The
		// tessellation parameters are all integral for the same reason: a
		// fractional segment count would have no meaning.
		auto &c3c = deserializedEntity.AddComponent<Circle3DRendererComponent>();
			if (circle3DRendererComponent["Color"])
				c3c.Color = circle3DRendererComponent["Color"].as<glm::vec4>();
			if (circle3DRendererComponent["Radius"])
				c3c.Radius = circle3DRendererComponent["Radius"].as<float>();
			if (circle3DRendererComponent["Segments"])
				c3c.Segments = circle3DRendererComponent["Segments"].as<int>();
			if (circle3DRendererComponent["TextureIndex"])
				c3c.TextureIndex = circle3DRendererComponent["TextureIndex"].as<int>();
			if (circle3DRendererComponent["TilingFactor"])
				c3c.TilingFactor = circle3DRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto icoSphereRendererComponent = entity["IcoSphereRendererComponent"];
		if (icoSphereRendererComponent)
		{
		auto &isc = deserializedEntity.AddComponent<IcoSphereRendererComponent>();
			if (icoSphereRendererComponent["Color"])
				isc.Color = icoSphereRendererComponent["Color"].as<glm::vec4>();
			if (icoSphereRendererComponent["Radius"])
				isc.Radius = icoSphereRendererComponent["Radius"].as<float>();
			if (icoSphereRendererComponent["Subdivisions"])
				isc.Subdivisions = icoSphereRendererComponent["Subdivisions"].as<int>();
			if (icoSphereRendererComponent["TextureIndex"])
				isc.TextureIndex = icoSphereRendererComponent["TextureIndex"].as<int>();
			if (icoSphereRendererComponent["TilingFactor"])
				isc.TilingFactor = icoSphereRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto cylinderRendererComponent = entity["CylinderRendererComponent"];
		if (cylinderRendererComponent)
		{
		// Height appears here but not on the plane or sphere -- the primitives
		// that have an axis need it, the radially symmetric ones do not. Which
		// keys a primitive carries is what distinguishes it in the file.
		auto &cyc = deserializedEntity.AddComponent<CylinderRendererComponent>();
			if (cylinderRendererComponent["Color"])
				cyc.Color = cylinderRendererComponent["Color"].as<glm::vec4>();
			if (cylinderRendererComponent["Radius"])
				cyc.Radius = cylinderRendererComponent["Radius"].as<float>();
			if (cylinderRendererComponent["Height"])
				cyc.Height = cylinderRendererComponent["Height"].as<float>();
			if (cylinderRendererComponent["Segments"])
				cyc.Segments = cylinderRendererComponent["Segments"].as<int>();
			if (cylinderRendererComponent["TextureIndex"])
				cyc.TextureIndex = cylinderRendererComponent["TextureIndex"].as<int>();
			if (cylinderRendererComponent["TilingFactor"])
				cyc.TilingFactor = cylinderRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto coneRendererComponent = entity["ConeRendererComponent"];
		if (coneRendererComponent)
		{
		auto &coc = deserializedEntity.AddComponent<ConeRendererComponent>();
			if (coneRendererComponent["Color"])
				coc.Color = coneRendererComponent["Color"].as<glm::vec4>();
			if (coneRendererComponent["Radius"])
				coc.Radius = coneRendererComponent["Radius"].as<float>();
			if (coneRendererComponent["Height"])
				coc.Height = coneRendererComponent["Height"].as<float>();
			if (coneRendererComponent["Segments"])
				coc.Segments = coneRendererComponent["Segments"].as<int>();
			if (coneRendererComponent["TextureIndex"])
				coc.TextureIndex = coneRendererComponent["TextureIndex"].as<int>();
			if (coneRendererComponent["TilingFactor"])
				coc.TilingFactor = coneRendererComponent["TilingFactor"].as<float>();
		}
		
		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto torusRendererComponent = entity["TorusRendererComponent"];
		if (torusRendererComponent)
		{
		auto &trc = deserializedEntity.AddComponent<TorusRendererComponent>();
			if (torusRendererComponent["Color"])
				trc.Color = torusRendererComponent["Color"].as<glm::vec4>();
			if (torusRendererComponent["MajorRadius"])
				trc.MajorRadius = torusRendererComponent["MajorRadius"].as<float>();
			if (torusRendererComponent["MinorRadius"])
				trc.MinorRadius = torusRendererComponent["MinorRadius"].as<float>();
			if (torusRendererComponent["MajorSegments"])
				trc.MajorSegments = torusRendererComponent["MajorSegments"].as<int>();
			if (torusRendererComponent["MinorSegments"])
				trc.MinorSegments = torusRendererComponent["MinorSegments"].as<int>();
			if (torusRendererComponent["TextureIndex"])
				trc.TextureIndex = torusRendererComponent["TextureIndex"].as<int>();
			if (torusRendererComponent["TilingFactor"])
				trc.TilingFactor = torusRendererComponent["TilingFactor"].as<float>();
		}

		// DEFECT: TextureWidth/TextureHeight are absent here though the writer
		// emits them -- see line 480. Every one of these primitive blocks loses
		// the same two fields, so the omission is systematic, not a one-off.
		const auto materialComponent = entity["MaterialComponent"];
		if (materialComponent)
		{
			// The material is added before its texture is resolved, so the component
			// exists with a null Texture even if the file names one. That is the
			// graceful-degradation path: a missing asset must not fail the load.
			auto &mc = deserializedEntity.AddComponent<MaterialComponent>();
				// All fields presence-checked: a scene authored before a field
				// existed must still load with that field's default.
				if (materialComponent["Albedo"])
					mc.Albedo = materialComponent["Albedo"].as<glm::vec4>();
				if (materialComponent["TextureIndex"])
					mc.TextureIndex = materialComponent["TextureIndex"].as<int>();
				// TexturePath defaults to empty string when missing.
				std::string rawPath = materialComponent["TexturePath"] ? materialComponent["TexturePath"].as<std::string>() : "";
				std::replace(rawPath.begin(), rawPath.end(), '\\', '/');
				// Collapse any duplicate "assets/" prefixes (from old serialization bugs)
				const std::string prefix = "assets/";
				while (rawPath.find(prefix + prefix) == 0)
					rawPath = rawPath.substr(prefix.size());
				mc.TexturePath = rawPath;
				// The texture is only CREATED if the file exists on disk.
				if (!rawPath.empty() && std::filesystem::is_regular_file(rawPath))
					mc.Texture = Texture2D::Create(rawPath);
				if (materialComponent["HasGeneratedMaps"])
					mc.HasGeneratedMaps = materialComponent["HasGeneratedMaps"].as<bool>();
				if (materialComponent["NormalStrength"])
					mc.NormalStrength = materialComponent["NormalStrength"].as<float>();
				if (materialComponent["Metallic"])
					mc.Metallic = materialComponent["Metallic"].as<float>();
				if (materialComponent["Roughness"])
					mc.Roughness = materialComponent["Roughness"].as<float>();
				if (materialComponent["EmissionColor"])
					mc.EmissionColor = materialComponent["EmissionColor"].as<glm::vec4>();
				if (materialComponent["EmissionIntensity"])
					mc.EmissionIntensity = materialComponent["EmissionIntensity"].as<float>();
				if (materialComponent["DisplacementScale"])
					mc.DisplacementScale = materialComponent["DisplacementScale"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		const auto rigidbody2DComponent = entity["Rigidbody2DComponent"];
		if (rigidbody2DComponent)
		{
			// Entities always have transforms
			// The string is parsed by the enum helper, which silently defaults.
			// FixedRotation is read right after and IS restored, so a 2D body's
			// rotation lock survives but its friction (line 1420) does not.
			auto &rigidbody2D = deserializedEntity.AddComponent<Rigidbody2DComponent>();
				if (rigidbody2DComponent["BodyType"])
					rigidbody2D.Type = RigidBody2DBodyTypeFromString(rigidbody2DComponent["BodyType"].as<std::string>());
				if (rigidbody2DComponent["FixedRotation"])
					rigidbody2D.FixedRotation = rigidbody2DComponent["FixedRotation"].as<bool>();
		}

		// DEFECT: Friction is written by this component's writer block and read
		// by no line here, so it resets to the component default on every load.
		// Both 2D colliders do this; all three 3D colliders round-trip theirs.
		auto boxCollider2DComponent = entity["BoxCollider2DComponent"];
		if (boxCollider2DComponent)
		{
			// Entities always have transforms
			auto &boxCollider2D = deserializedEntity.AddComponent<BoxCollider2DComponent>();
				if (boxCollider2DComponent["Offset"])
					boxCollider2D.Offset = boxCollider2DComponent["Offset"].as<glm::vec2>();
				if (boxCollider2DComponent["Size"])
					boxCollider2D.Size = boxCollider2DComponent["Size"].as<glm::vec2>();
				if (boxCollider2DComponent["Density"])
					boxCollider2D.Density = boxCollider2DComponent["Density"].as<float>();
				if (boxCollider2DComponent["Restitution"])
					boxCollider2D.Restitution = boxCollider2DComponent["Restitution"].as<float>();
				if (boxCollider2DComponent["RestitutionThreshold"])
					boxCollider2D.RestitutionThreshold = boxCollider2DComponent["RestitutionThreshold"].as<float>();
		}

		// DEFECT: Friction is written by this component's writer block and read
		// by no line here, so it resets to the component default on every load.
		// Both 2D colliders do this; all three 3D colliders round-trip theirs.
		auto circleCollider2DComponent = entity["CircleCollider2DComponent"];
		if (circleCollider2DComponent)
		{
			// Entities always have transforms
			auto &circleCollider2D = deserializedEntity.AddComponent<CircleCollider2DComponent>();
				if (circleCollider2DComponent["Offset"])
					circleCollider2D.Offset = circleCollider2DComponent["Offset"].as<glm::vec2>();
				if (circleCollider2DComponent["Radius"])
					circleCollider2D.Radius = circleCollider2DComponent["Radius"].as<float>();
				if (circleCollider2DComponent["Density"])
					circleCollider2D.Density = circleCollider2DComponent["Density"].as<float>();
				if (circleCollider2DComponent["Restitution"])
					circleCollider2D.Restitution = circleCollider2DComponent["Restitution"].as<float>();
				if (circleCollider2DComponent["RestitutionThreshold"])
					circleCollider2D.RestitutionThreshold = circleCollider2DComponent["RestitutionThreshold"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto rigidBody3DComponent = entity["RigidBody3DComponent"];
		if (rigidBody3DComponent)
		{
			auto &rb3d = deserializedEntity.AddComponent<RigidBody3DComponent>();
				// All fields presence-checked: a partial node leaves defaults.
				if (rigidBody3DComponent["BodyType"])
					rb3d.Type = RigidBody3DTypeFromString(rigidBody3DComponent["BodyType"].as<std::string>());
				if (rigidBody3DComponent["GravityScale"])
					rb3d.GravityScale = rigidBody3DComponent["GravityScale"].as<float>();
				if (rigidBody3DComponent["LinearDamping"])
					rb3d.LinearDamping = rigidBody3DComponent["LinearDamping"].as<float>();
				if (rigidBody3DComponent["AngularDamping"])
					rb3d.AngularDamping = rigidBody3DComponent["AngularDamping"].as<float>();
				if (rigidBody3DComponent["FixedRotationX"]) rb3d.FixedRotationX = rigidBody3DComponent["FixedRotationX"].as<bool>();
				if (rigidBody3DComponent["FixedRotationY"]) rb3d.FixedRotationY = rigidBody3DComponent["FixedRotationY"].as<bool>();
				if (rigidBody3DComponent["FixedRotationZ"]) rb3d.FixedRotationZ = rigidBody3DComponent["FixedRotationZ"].as<bool>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto boxCollider3DComponent = entity["BoxCollider3DComponent"];
		if (boxCollider3DComponent)
		{
			auto &bc = deserializedEntity.AddComponent<BoxCollider3DComponent>();
				if (boxCollider3DComponent["HalfExtents"])
					bc.HalfExtents = boxCollider3DComponent["HalfExtents"].as<glm::vec3>();
				if (boxCollider3DComponent["Offset"])
					bc.Offset = boxCollider3DComponent["Offset"].as<glm::vec3>();
				if (boxCollider3DComponent["Density"])
					bc.Density = boxCollider3DComponent["Density"].as<float>();
				if (boxCollider3DComponent["Friction"])
					bc.Friction = boxCollider3DComponent["Friction"].as<float>();
				if (boxCollider3DComponent["Restitution"])
					bc.Restitution = boxCollider3DComponent["Restitution"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto sphereCollider3DComponent = entity["SphereCollider3DComponent"];
		if (sphereCollider3DComponent)
		{
			auto &sc = deserializedEntity.AddComponent<SphereCollider3DComponent>();
				if (sphereCollider3DComponent["Radius"])
					sc.Radius = sphereCollider3DComponent["Radius"].as<float>();
				if (sphereCollider3DComponent["Offset"])
					sc.Offset = sphereCollider3DComponent["Offset"].as<glm::vec3>();
				if (sphereCollider3DComponent["Density"])
					sc.Density = sphereCollider3DComponent["Density"].as<float>();
				if (sphereCollider3DComponent["Friction"])
					sc.Friction = sphereCollider3DComponent["Friction"].as<float>();
				if (sphereCollider3DComponent["Restitution"])
					sc.Restitution = sphereCollider3DComponent["Restitution"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto capsuleCollider3DComponent = entity["CapsuleCollider3DComponent"];
		if (capsuleCollider3DComponent)
		{
			auto &cc = deserializedEntity.AddComponent<CapsuleCollider3DComponent>();
				if (capsuleCollider3DComponent["Radius"])
					cc.Radius = capsuleCollider3DComponent["Radius"].as<float>();
				if (capsuleCollider3DComponent["Height"])
					cc.Height = capsuleCollider3DComponent["Height"].as<float>();
				if (capsuleCollider3DComponent["Offset"])
					cc.Offset = capsuleCollider3DComponent["Offset"].as<glm::vec3>();
				if (capsuleCollider3DComponent["Density"])
					cc.Density = capsuleCollider3DComponent["Density"].as<float>();
				if (capsuleCollider3DComponent["Friction"])
					cc.Friction = capsuleCollider3DComponent["Friction"].as<float>();
				if (capsuleCollider3DComponent["Restitution"])
					cc.Restitution = capsuleCollider3DComponent["Restitution"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto volumetricFogComponent = entity["VolumetricFogComponent"];
		if (volumetricFogComponent)
		{
			auto &fog = deserializedEntity.AddComponent<VolumetricFogComponent>();
			// Fog's reader opens with a presence check on Enabled, then reads each
			// field the same way. Twenty-four lines, twenty-four checks -- the most
			// defensive block in the function, and the model the rest should follow.
			if (volumetricFogComponent["Enabled"])
				fog.Enabled = volumetricFogComponent["Enabled"].as<bool>();
			if (volumetricFogComponent["Color"])
				// glm::vec3(vec4): narrows the manufactured alpha away. The component
				// stores a vec3, so this is the exact inverse of the widening at line 838 --
				// including the fact that the round trip spends four file elements on a
				// three-element value.
				fog.Color = glm::vec3(volumetricFogComponent["Color"].as<glm::vec4>());
			// Density, Anisotropy and HeightFalloff: how thick the fog is, how much
			// it stretches along the view ray, and how quickly it thins with altitude.
			// Each is independently authored rather than derived from the others.
			if (volumetricFogComponent["Density"])
				fog.Density = volumetricFogComponent["Density"].as<float>();
			if (volumetricFogComponent["Anisotropy"])
				fog.Anisotropy = volumetricFogComponent["Anisotropy"].as<float>();
			if (volumetricFogComponent["HeightFalloff"])
				fog.HeightFalloff = volumetricFogComponent["HeightFalloff"].as<float>();
			if (volumetricFogComponent["NoiseStrength"])
				fog.NoiseStrength = volumetricFogComponent["NoiseStrength"].as<float>();
			if (volumetricFogComponent["NoiseScale"])
				fog.NoiseScale = volumetricFogComponent["NoiseScale"].as<float>();
			if (volumetricFogComponent["WindSpeed"])
				fog.WindSpeed = volumetricFogComponent["WindSpeed"].as<float>();
			if (volumetricFogComponent["MaxSteps"])
				fog.MaxSteps = volumetricFogComponent["MaxSteps"].as<int>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto volumetricCloudsComponent = entity["VolumetricCloudsComponent"];
		if (volumetricCloudsComponent)
		{
			// Clouds' reader: same shape as fog's, one field longer per colour because
			// there are two of them. Both vec4-to-vec3 narrowings happen here.
			auto &clouds = deserializedEntity.AddComponent<VolumetricCloudsComponent>();
			if (volumetricCloudsComponent["Enabled"])
				clouds.Enabled = volumetricCloudsComponent["Enabled"].as<bool>();
			if (volumetricCloudsComponent["Color"])
				clouds.Color = glm::vec3(volumetricCloudsComponent["Color"].as<glm::vec4>());
			if (volumetricCloudsComponent["AmbientTint"])
				clouds.AmbientTint = glm::vec3(volumetricCloudsComponent["AmbientTint"].as<glm::vec4>());
			if (volumetricCloudsComponent["Coverage"])
				clouds.Coverage = volumetricCloudsComponent["Coverage"].as<float>();
			if (volumetricCloudsComponent["Density"])
				clouds.Density = volumetricCloudsComponent["Density"].as<float>();
			// NoiseScale and DetailAmount govern the cloud texture, then WindDirection
			// and WindSpeed move it. SilverLining and ShadowStrength are appearance
			// controls that a cheaper cloud renderer could drop without changing the
			// scene's meaning -- which is why they are separate fields.
			if (volumetricCloudsComponent["NoiseScale"])
				clouds.NoiseScale = volumetricCloudsComponent["NoiseScale"].as<float>();
			if (volumetricCloudsComponent["DetailAmount"])
				clouds.DetailAmount = volumetricCloudsComponent["DetailAmount"].as<float>();
			if (volumetricCloudsComponent["WindDirection"])
				clouds.WindDirection = volumetricCloudsComponent["WindDirection"].as<glm::vec2>();
			if (volumetricCloudsComponent["WindSpeed"])
				clouds.WindSpeed = volumetricCloudsComponent["WindSpeed"].as<float>();
			if (volumetricCloudsComponent["SilverLining"])
				clouds.SilverLining = volumetricCloudsComponent["SilverLining"].as<float>();
			if (volumetricCloudsComponent["ShadowStrength"])
				clouds.ShadowStrength = volumetricCloudsComponent["ShadowStrength"].as<float>();
			if (volumetricCloudsComponent["MaxSteps"])
				clouds.MaxSteps = volumetricCloudsComponent["MaxSteps"].as<int>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto directionalLightComponent = entity["DirectionalLightComponent"];
		if (directionalLightComponent)
		{
			auto &light = deserializedEntity.AddComponent<DirectionalLightComponent>();
			if (directionalLightComponent["Color"])
				// Colour is narrowed from the manufactured vec4 back to the component's
				// vec3, exactly inverting the widening on the write side. See line 874.
				light.Color = glm::vec3(directionalLightComponent["Color"].as<glm::vec4>());
			if (directionalLightComponent["Intensity"])
				light.Intensity = directionalLightComponent["Intensity"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto pointLightComponent = entity["PointLightComponent"];
		if (pointLightComponent)
		{
			// Point light's reader. Every field checked, so a scene authored before
			// Range or Falloff existed still loads -- the light simply keeps its
			// default falloff.
			auto &light = deserializedEntity.AddComponent<PointLightComponent>();
			if (pointLightComponent["Color"])
				// Point light: the same four fields the writer emits, each presence-
				// checked. Unlike the renderer blocks above, a partial point-light node
				// leaves defaults instead of throwing.
				light.Color = glm::vec3(pointLightComponent["Color"].as<glm::vec4>());
			if (pointLightComponent["Intensity"])
				light.Intensity = pointLightComponent["Intensity"].as<float>();
			if (pointLightComponent["Range"])
				light.Range = pointLightComponent["Range"].as<float>();
			if (pointLightComponent["Falloff"])
				light.Falloff = pointLightComponent["Falloff"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto spotLightComponent = entity["SpotLightComponent"];
		if (spotLightComponent)
		{
			auto &light = deserializedEntity.AddComponent<SpotLightComponent>();
			if (spotLightComponent["Color"])
				// The spot's five scalar fields, all presence-checked. Every field in
				// every light reader is checked, so the light family is the most robust
				// part of this function.
				light.Color = glm::vec3(spotLightComponent["Color"].as<glm::vec4>());
			if (spotLightComponent["Intensity"])
				light.Intensity = spotLightComponent["Intensity"].as<float>();
			if (spotLightComponent["Range"])
				light.Range = spotLightComponent["Range"].as<float>();
			if (spotLightComponent["Falloff"])
				light.Falloff = spotLightComponent["Falloff"].as<float>();
			if (spotLightComponent["InnerAngle"])
				light.InnerAngle = spotLightComponent["InnerAngle"].as<float>();
			if (spotLightComponent["OuterAngle"])
				light.OuterAngle = spotLightComponent["OuterAngle"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto areaLightComponent = entity["AreaLightComponent"];
		if (areaLightComponent)
		{
			// Area light's reader adds the two rectangle dimensions and DoubleSided.
			// All six checked, matching the six the writer emits exactly.
			auto &light = deserializedEntity.AddComponent<AreaLightComponent>();
			if (areaLightComponent["Color"])
				light.Color = glm::vec3(areaLightComponent["Color"].as<glm::vec4>());
			if (areaLightComponent["Intensity"])
				light.Intensity = areaLightComponent["Intensity"].as<float>();
			if (areaLightComponent["Width"])
				light.Width = areaLightComponent["Width"].as<float>();
			if (areaLightComponent["Height"])
				light.Height = areaLightComponent["Height"].as<float>();
			if (areaLightComponent["Range"])
				light.Range = areaLightComponent["Range"].as<float>();
			if (areaLightComponent["DoubleSided"])
				light.DoubleSided = areaLightComponent["DoubleSided"].as<bool>();
		}

		// The water block reads all 25 fields, each presence-checked. It is the
		// most defensive reader in the file and the exact counterpart of the
		// largest writer, so water round-trips completely where the colliders
		// above silently lose Friction.
		auto waterComponent = entity["WaterComponent"];
		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		if (waterComponent)
		{
			// Water's reader opens with the component being added, then walks all 25
			// fields with a presence check each. This is the only block in the file
			// that is exhaustive on BOTH sides, which is why water is the one family
			// that cannot silently lose a field.
			auto &water = deserializedEntity.AddComponent<WaterComponent>();
			// All 25 water fields are checked individually, in the same order the
			// writer emits them. This block is the model the rest of the function
			// should have followed: a scene authored before any of these fields
			// existed still loads, keeping that field's default.
			if (waterComponent["Enabled"])
				water.Enabled = waterComponent["Enabled"].as<bool>();
			if (waterComponent["BodyType"])
				water.Type = WaterBodyTypeFromString(waterComponent["BodyType"].as<std::string>());
			// ShallowColor, DeepColor and FoamColor each go through the same
			// vec4-then-vec3 round trip. Three manufactured alpha values per load for
			// colours that have no alpha channel on either side of it.
			if (waterComponent["ShallowColor"])
				water.ShallowColor = glm::vec3(waterComponent["ShallowColor"].as<glm::vec4>());
			if (waterComponent["DeepColor"])
				water.DeepColor = glm::vec3(waterComponent["DeepColor"].as<glm::vec4>());
			if (waterComponent["FoamColor"])
				water.FoamColor = glm::vec3(waterComponent["FoamColor"].as<glm::vec4>());
			if (waterComponent["WaterDepth"])
				water.WaterDepth = waterComponent["WaterDepth"].as<float>();
			if (waterComponent["WaveAmplitude"])
				water.WaveAmplitude = waterComponent["WaveAmplitude"].as<float>();
			if (waterComponent["WaveLength"])
				water.WaveLength = waterComponent["WaveLength"].as<float>();
			if (waterComponent["WaveSpeed"])
				water.WaveSpeed = waterComponent["WaveSpeed"].as<float>();
			if (waterComponent["WaveDirection"])
				// FlowDirection is a vec2 here, read through convert<glm::vec2> -- the
				// two-element converter. WaveDirection above uses the same one, so the
				// vec2 converter is not dead code despite no transform using it.
				water.WaveDirection = waterComponent["WaveDirection"].as<glm::vec2>();
			if (waterComponent["Chop"])
				water.Chop = waterComponent["Chop"].as<float>();
			if (waterComponent["Steepness"])
				water.Steepness = waterComponent["Steepness"].as<float>();
			if (waterComponent["FlowDirection"])
				water.FlowDirection = waterComponent["FlowDirection"].as<glm::vec2>();
			if (waterComponent["FlowSpeed"])
				water.FlowSpeed = waterComponent["FlowSpeed"].as<float>();
			if (waterComponent["FoamAmount"])
				water.FoamAmount = waterComponent["FoamAmount"].as<float>();
			if (waterComponent["FoamScale"])
				water.FoamScale = waterComponent["FoamScale"].as<float>();
			if (waterComponent["ShoreFoam"])
				water.ShoreFoam = waterComponent["ShoreFoam"].as<float>();
			if (waterComponent["ShoreWidth"])
				water.ShoreWidth = waterComponent["ShoreWidth"].as<float>();
			if (waterComponent["Transparency"])
				water.Transparency = waterComponent["Transparency"].as<float>();
			if (waterComponent["Roughness"])
				water.Roughness = waterComponent["Roughness"].as<float>();
			if (waterComponent["Metallic"])
				water.Metallic = waterComponent["Metallic"].as<float>();
			if (waterComponent["Segments"])
				water.Segments = waterComponent["Segments"].as<int>();
			if (waterComponent["TimeScale"])
				water.TimeScale = waterComponent["TimeScale"].as<float>();
			if (waterComponent["Buoyancy"])
				water.Buoyancy = waterComponent["Buoyancy"].as<float>();
			if (waterComponent["WaterDensity"])
				water.WaterDensity = waterComponent["WaterDensity"].as<float>();
		}

		// Guarded by node truthiness only -- the fields inside are read
		// unconditionally, so a partial node throws out of Deserialize instead of
		// keeping the component's defaults. The health and weapon blocks above
		// are the ones that check every field.
		auto scriptComponent = entity["ScriptComponent"];
		if (scriptComponent)
		{
			auto &sc = deserializedEntity.AddComponent<ScriptComponent>();
				// Presence-checked: a missing ScriptPath or ScriptName leaves
				// the component's defaults.
				if (scriptComponent["ScriptPath"])
				{
					std::string rawPath = scriptComponent["ScriptPath"].as<std::string>();
					// Always store as a relative path
					sc.ScriptPath = "scripts/" + std::filesystem::path(rawPath).filename().string();
				}
				if (scriptComponent["ScriptName"])
					sc.ScriptName = scriptComponent["ScriptName"].as<std::string>();
		}

		return true;
	}

	bool SceneSerializer::Deserialize(const std::string &filepath)
	{
		// Binary sidecar first. If one exists and validates against this exact revision of
		// the YAML file, the scene is populated from it and the YAML is never parsed at
		// all -- which is the entire point, since yaml-cpp parsing is ~510 ms per megabyte
		// of scene text. Read() only accepts a sidecar whose recorded size and write time
		// match the YAML on disk, so a stale sidecar is ignored rather than served.
		//
		// On failure the scene is left empty by Read() and we fall through to YAML. A
		// sidecar is a cache: deleting it, or corrupting it, costs speed and nothing else.
		{
			std::string sidecarReason;
			std::vector<std::string> fragments;
			if (SceneChunkStore::Read(*m_Scene, std::filesystem::path(filepath),
					&fragments, &sidecarReason))
			{
				// Replay the fragment entities through the same reader as YAML. This must
				// happen here: SceneChunkStore hands back text, and the only code that
				// knows how a component deserializes is DeserializeEntityNode. Replaying
				// them anywhere else would be a second reader that could drift.
				//
				// A fragment failure is corruption, not a bad scene -- these came from our
				// own writer. Fall back to YAML rather than returning a scene that is
				// missing its script-bearing entities, which would fail silently.
				try
				{
					for (const std::string &fragment : fragments)
					{
						const YAML::Node node = YAML::Load(fragment);
						DeserializeEntityNode(node);
					}
				}
				catch (const YAML::Exception &exception)
				{
					WL_CORE_WARN(
						"Scene sidecar fragments for '{0}' did not parse ({1}); "
						"falling back to YAML", filepath, exception.what());
					m_Scene->m_Registry.clear();
					goto reloadFromYaml;
				}

				return true;
			}
			if (sidecarReason != "no sidecar")
			{
				WL_CORE_WARN("Ignoring scene sidecar for '{0}': {1}", filepath,
					sidecarReason);
			}
		}

		reloadFromYaml:;

		// Opened without an existence check. A missing file yields a failed
		// stream, and the rdbuf copy below then produces an empty string, so
		// YAML::Load sees "" and data["Scene"] is undefined -- the false below
		// is reached without an explicit is_open() test.
		std::ifstream stream(filepath);
		// A stringstream used only as a copy buffer for the whole file. The
		// three-step shape (ifstream -> stringstream -> string) is how the file
		// gets from disk into YAML::Load; there is no streaming parse here, so the
		// entire scene is held in memory twice.
		std::stringstream strStream;
		strStream << stream.rdbuf();

		// A malformed YAML document is reported as a load failure before any scene
		// mutation begins.
		YAML::Node data;
		try
		{
			data = YAML::Load(strStream.str());
		}
		catch (const YAML::Exception &exception)
		{
			WL_CORE_ERROR("Could not parse scene '{0}': {1}", filepath, exception.what());
			return false;
		}
		if (!data.IsMap() || !data["Scene"])
			return false;

		// The trained neural stage's enabled flag. Absent means default (true),
		// per this file's documented error model: a missing key means default,
		// never an error. A scene saved before this key existed therefore loads
		// unchanged, which is the only behaviour that does not silently disable the
		// stage for scenes that were fine.
		if (data["NeuralStage"] && data["NeuralStage"].IsMap())
		{
			const YAML::Node enabled = data["NeuralStage"]["Enabled"];
			if (enabled && enabled.IsScalar())
			{
				const std::string value = enabled.Scalar();
				// Accept the spelling the emitter writes plus the YAML 1.1 booleans a
				// hand edit is likely to use. A malformed value leaves the flag at its
				// default rather than failing the load: refusing to open a whole scene
				// over one optional toggle would be a far worse regression than
				// honouring the default.
				m_Scene->SetNeuralStageEnabled(!(value == "false" || value == "False" ||
					value == "FALSE" || value == "no" || value == "off" || value == "0"));
			}
		}

		Terrain::TerrainScenePayload terrainPayload;
		const YAML::Node terrainNode = data["Terrain"];
		const bool hasTerrain = terrainNode.IsDefined();
		if (hasTerrain)
		{
			Project &project = Project::Get();
			if (!project.IsOpen())
			{
				WL_CORE_ERROR("Could not load scene terrain: no project is open");
				return false;
			}

			try
			{
				YAML::Emitter terrainEmitter;
				terrainEmitter << terrainNode;
				if (!terrainEmitter.good())
				{
					WL_CORE_ERROR("Could not encode scene terrain metadata: {0}", terrainEmitter.GetLastError());
					return false;
				}
				const std::string terrainMetadata(terrainEmitter.c_str(), terrainEmitter.size());
				std::string terrainError;
				if (!Terrain::LoadTerrainScenePayload(project.GetAssetRoot(), project.GetRoot(),
					terrainMetadata, terrainPayload, &terrainError))
				{
					WL_CORE_ERROR("Could not load scene terrain: {0}", terrainError);
					return false;
				}
			}
			catch (const std::exception &exception)
			{
				WL_CORE_ERROR("Could not load scene terrain: {0}", exception.what());
				return false;
			}
		}

		// The scene name is read and then only logged -- never stored anywhere.
		// Serialize hardcodes "Untitled" and nothing on the Scene type keeps a
		// name, so the key exists purely as a validity marker for the check
		// above and as a breadcrumb in the file.
		std::string sceneName = data["Scene"].as<std::string>();
		// Traced with the name, so this is where the file's Scene key is read at
		// all. The value is not stored on the scene -- see the note at line 1078.
		WL_CORE_TRACE("Deserializing scene '{0}'", sceneName);

		// 'Entities' is optional: a scene file with only the Scene key loads as an
		// empty scene and still returns true. There is no way to distinguish that
		// from a scene that legitimately has no entities.
		//
		// CONST, and the loop variable below is const too. This is not style.
		// yaml-cpp's NON-const Node::operator[] routes to the non-const
		// node_data::get(), which on a MISSING key CREATES a node and INSERTS it
		// into the map (vendor/yaml-cpp/src/node_data.cpp:242-244). The const
		// overload (include/yaml-cpp/node/impl.h:325) returns a ZombieNode and
		// inserts nothing.
		//
		// The reader below probes 25 known component names per entity, of which
		// about 21 are absent on a typical entity. With a mutable node that meant
		// 21 node allocations AND 21 map insertions per entity, and because
		// node_data::get() is a LINEAR scan over m_map, each insertion made every
		// later probe scan a longer map -- the entity's map grew from ~5 entries to
		// ~26 while being read. Measured at ~109 us/entity of pure overhead, which
		// on a 1M-entity scene is ~21M wasted nodes and ~30 minutes of the load.
		// It also corrupted the parsed DOM with keys that were never in the file.
		const auto entities = data["Entities"];
		if (entities)
		{
			// Iteration order is the file's, not the registry's -- the opposite of the
			// write side, which visits in entt storage order. So a load-then-save
			// round trip reorders the entities.
			// `const auto&`, not `auto`: see the note above `entities`. A mutable
			// loop variable makes every `entity["X"]` probe below use the non-const
			// operator[], which INSERTS a phantom key for each absent component.
			// One call per entity. The reader itself lives in DeserializeEntityNode so
			// the binary sidecar can replay individual unsupported entities through it.
			for (const auto& entity : entities)
				DeserializeEntityNode(entity);
		}

		if (hasTerrain)
		{
			std::string terrainError;
			if (!m_Scene->ConfigureTerrainRuntime(terrainPayload.Asset, terrainPayload.WorldState,
				terrainPayload.Metadata.Streaming, &terrainError, terrainPayload.Metadata.AssetReference))
			{
				WL_CORE_ERROR("Could not commit scene terrain: {0}", terrainError);
				return false;
			}
		}
		else
		{
			m_Scene->ClearTerrainRuntime();
		}
		return true;
	}

	// NOT IMPLEMENTED.
	//
	// The load counterpart of the unimplemented SerializeRuntime. Returns
	// false in every build, so at least the failure is consistent here.
	//
	// Exposed to Python through Bindings.cpp, where that false is the only
	// signal a script gets: a script that saves then loads a scene at
	// runtime silently loses the scene.
	//
	// The assert is redundant with the `return false` below it and, like
	// every WL_CORE_ASSERT, vanishes in Release -- where this is simply a
	// function that returns false.
	//
	// @param filepath  Ignored.
	// @returns         Always false.
	bool SceneSerializer::DeserializeRuntime(const std::string &filepath)
	{
		// Not implemented
		WL_CORE_ASSERT(false, "DeserializeRuntime is not implemented!");
		return false;
	}

}