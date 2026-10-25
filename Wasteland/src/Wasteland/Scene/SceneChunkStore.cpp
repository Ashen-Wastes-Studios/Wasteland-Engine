// SceneChunkStore.cpp -- see SceneChunkStore.h for why this exists and the safety rules.
//
// Layout of a .wlbin file (all little-endian, which is every platform this builds for):
//
//   Header  (48 bytes)
//     char magic[4]        "WLSC"
//     u32  version         kVersion
//     u32  headerSize      sizeof(Header)
//     u32  recordSize      sizeof(Record)
//     u32  entityCount
//     u64  sourceYamlSize
//     u64  sourceYamlWriteTime   last_write_time as a raw tick count
//     u32  reserved
//     u32  reserved2
//
//   Record  (sizeof(Record), packed) x entityCount
//
// Everything is packed and static_asserted, because this is a file format: the compiler's
// default padding must not be able to change the on-disk layout between builds.

#include "wlpch.h"
#include "SceneChunkStore.h"

#include "Components.h"
#include "Entity.h"

#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace Wasteland
{

	namespace
	{
		// Version 2 added the fragment section: entities the fast record cannot hold are
		// stored as their authored YAML instead of suppressing the whole sidecar.
		constexpr std::uint32_t kVersion = 2;
		constexpr char kMagic[4] = { 'W', 'L', 'S', 'C' };

		// Upper bound on one fragment's bytes. A fragment is a whole entity's YAML, and a
		// component can legitimately be large (a script path, an item definition), but a
		// value past this means the file is corrupt -- reading it would allocate on trust.
		// 16 MB is far above any entity this format can hold.
		constexpr std::uint32_t kMaxFragmentBytes = 16u * 1024u * 1024u;

#pragma pack(push, 1)
		struct Header
		{
			char Magic[4];
			std::uint32_t Version;
			std::uint32_t HeaderSize;
			std::uint32_t RecordSize;
			std::uint32_t EntityCount;
			std::uint64_t SourceYamlSize;
			std::uint64_t SourceYamlWriteTime;
			std::uint32_t FragmentCount;
			std::uint64_t FragmentBytes;
			std::uint32_t Reserved;
		};
#pragma pack(pop)

		// Which optional components an entity carried. The three mandatory ones
		// (ID, Tag, Transform) are implied and have no flag: every entity in the registry
		// has them, and CreateEntityWithUUID re-adds them on load.
		enum RecordFlags : std::uint32_t
		{
			kFlagCubeRenderer = 1u << 0,
			kFlagMaterial = 1u << 1,
			kFlagCylinderRenderer = 1u << 2,
			kFlagSphereRenderer = 1u << 3,
			kFlagPointLight = 1u << 4,
		};

#pragma pack(push, 1)
		struct Record
		{
			std::uint64_t Uuid;
			std::uint32_t Flags;

			// TransformComponent
			float Translation[3];
			float Rotation[3];
			float Scale[3];

			// CubeRendererComponent
			float CubeColor[4];
			std::int32_t CubeTextureIndex;
			float CubeTilingFactor;

			// MaterialComponent
			float MaterialAlbedo[4];
			std::int32_t MaterialTextureIndex;
			float MaterialNormalStrength;
			float MaterialMetallic;
			float MaterialRoughness;

			// CylinderRendererComponent
			float CylinderColor[4];
			std::int32_t CylinderTextureIndex;
			float CylinderTilingFactor;
			float CylinderRadius;
			float CylinderHeight;
			std::int32_t CylinderSegments;

			// SphereRendererComponent
			float SphereColor[4];
			std::int32_t SphereTextureIndex;
			float SphereTilingFactor;
			// Sphere carries a Radius of its own. Omitting it would load every sphere at
			// the default 0.5 and look almost right -- the silent kind of wrong this
			// format must not have.
			float SphereRadius;

			// PointLightComponent
			float PointLightColor[3];
			float PointLightIntensity;
			float PointLightRange;
			float PointLightFalloff;

			std::uint32_t TagLength;
			char Tag[SceneChunkStore::kMaxTagLength + 1];
		};
#pragma pack(pop)

		static_assert(sizeof(Header) == 52, "sidecar header must stay 52 bytes");
		static_assert(sizeof(Record) == 260, "sidecar record must stay 260 bytes");
		static_assert(std::is_trivially_copyable_v<Record>, "record is memcpy'd verbatim");

		// True if the entity carries any component the sidecar cannot represent.
		//
		// Written as one any_of over the full unsupported list rather than a chain of
		// HasComponent calls so that adding a component to Components.h without adding it
		// here is a visible omission in one place. Note this list is deliberately the
		// complement of the supported set: if it drifts, the failure mode is "no fast
		// record for that entity" (slow but correct), never "component dropped"
		// (fast but wrong). An entity with an unlisted component becomes a fragment.
		bool HasUnsupportedComponent(const entt::registry &registry, entt::entity entity)
		{
			return registry.any_of<
				AreaLightComponent,
				AudioListenerComponent,
				AudioSourceComponent,
				BehaviorTreeComponent,
				BoxCollider2DComponent,
				BoxCollider3DComponent,
				CameraComponent,
				CapsuleCollider3DComponent,
				Circle3DRendererComponent,
				CircleCollider2DComponent,
				CircleRendererComponent,
				ConeRendererComponent,
				DirectionalLightComponent,
				DroppedItemComponent,
				GridRendererComponent,
				HealthComponent,
				HittableComponent,
				IcoSphereRendererComponent,
				InteractableComponent,
				InventoryComponent,
				ItemDefinitionComponent,
				NativeScriptComponent,
				PersistentComponent,
				PlaneRendererComponent,
				QuestFlagsComponent,
				RaycastIgnoreComponent,
				Rigidbody2DComponent,
				RigidBody3DComponent,
				ScriptComponent,
				SphereCollider3DComponent,
				SpotLightComponent,
				SpriteRendererComponent,
				TorusRendererComponent,
				ViewStateComponent,
				VolumetricCloudsComponent,
				VolumetricFogComponent,
				WaterComponent,
				WeaponComponent,
				WeaponFeedbackComponent,
				WorldTimerComponent>(entity);
		}

		// A renderer or material holding a resolved Texture2D. The record cannot store a
		// texture: it has no path and no handle, only a GPU object. The YAML writer does
		// store the path, so such an entity is a fragment -- which is why a textured scene
		// still gets a sidecar instead of silently losing its textures.
		bool HasTextureReference(const entt::registry &registry, entt::entity entity)
		{
			if (const auto *cube = registry.try_get<CubeRendererComponent>(entity))
				if (cube->Texture)
					return true;
			if (const auto *cylinder = registry.try_get<CylinderRendererComponent>(entity))
				if (cylinder->Texture)
					return true;
			if (const auto *sphere = registry.try_get<SphereRendererComponent>(entity))
				if (sphere->Texture)
					return true;
			if (const auto *material = registry.try_get<MaterialComponent>(entity))
				if (material->Texture)
					return true;
			return false;
		}

		bool ReadExact(std::ifstream &in, void *data, std::size_t size)
		{
			in.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
			return in.good() && static_cast<std::size_t>(in.gcount()) == size;
		}
	}

	std::filesystem::path SceneChunkStore::SidecarPathFor(const std::filesystem::path &scenePath)
	{
		std::filesystem::path sidecar = scenePath;
		sidecar.replace_extension(".wlbin");
		return sidecar;
	}

	void SceneChunkStore::Remove(const std::filesystem::path &scenePath)
	{
		std::error_code ignored;
		std::filesystem::remove(SidecarPathFor(scenePath), ignored);
	}

	bool SceneChunkStore::Write(Scene &scene, const std::filesystem::path &scenePath,
		const FragmentEmitter &emitFragment, std::string *reason)
	{
		const auto fail = [reason](const std::string &message)
		{
			if (reason)
				*reason = message;
			return false;
		};

		// The YAML must already be on disk: its size and write time are the staleness key.
		std::error_code ec;
		const std::uintmax_t yamlSize = std::filesystem::file_size(scenePath, ec);
		if (ec)
			return fail("scene file could not be stat'd: " + ec.message());
		const auto yamlWriteTime = std::filesystem::last_write_time(scenePath, ec);
		if (ec)
			return fail("scene file write time could not be read: " + ec.message());

		// Build every entry first, then write once. Nothing here aborts the write: an entity
		// the fast record cannot hold becomes a fragment, so the sidecar is total and a
		// partial sidecar is never produced.
		std::vector<Record> records;
		std::vector<std::string> fragments;
		const auto &registry = scene.m_Registry;
		const auto view = registry.view<entt::entity>();

		for (const entt::entity entityID : view)
		{
			const auto *id = registry.try_get<IDComponent>(entityID);
			const auto *tag = registry.try_get<TagComponent>(entityID);
			const auto *transform = registry.try_get<TransformComponent>(entityID);
			if (!id || !tag || !transform)
				return fail("scene contains an entity without ID, Tag or Transform");

			// Anything the record cannot represent goes to a fragment: an unsupported
			// component, a resolved texture reference, or a tag too long for Record::Tag.
			// The YAML writer handles all three correctly, including resolving texture
			// paths, which is why fragmenting beats refusing -- refusing would leave a
			// textured scene on the slow path forever.
			const bool representable = !HasUnsupportedComponent(registry, entityID) &&
				tag->Tag.size() <= kMaxTagLength &&
				!HasTextureReference(registry, entityID);

			if (!representable)
			{
				fragments.push_back(emitFragment(scene, entityID));
				continue;
			}

			const auto *cube = registry.try_get<CubeRendererComponent>(entityID);
			const auto *material = registry.try_get<MaterialComponent>(entityID);
			const auto *cylinder = registry.try_get<CylinderRendererComponent>(entityID);
			const auto *sphere = registry.try_get<SphereRendererComponent>(entityID);
			const auto *pointLight = registry.try_get<PointLightComponent>(entityID);

			Record record{};
			record.Uuid = static_cast<std::uint64_t>(id->ID);
			record.Flags = (cube ? kFlagCubeRenderer : 0u) | (material ? kFlagMaterial : 0u) |
				(cylinder ? kFlagCylinderRenderer : 0u) |
				(sphere ? kFlagSphereRenderer : 0u) |
				(pointLight ? kFlagPointLight : 0u);

			record.Translation[0] = transform->Translation.x;
			record.Translation[1] = transform->Translation.y;
			record.Translation[2] = transform->Translation.z;
			record.Rotation[0] = transform->Rotation.x;
			record.Rotation[1] = transform->Rotation.y;
			record.Rotation[2] = transform->Rotation.z;
			record.Scale[0] = transform->Scale.x;
			record.Scale[1] = transform->Scale.y;
			record.Scale[2] = transform->Scale.z;

			if (cube)
			{
				record.CubeColor[0] = cube->Color.r;
				record.CubeColor[1] = cube->Color.g;
				record.CubeColor[2] = cube->Color.b;
				record.CubeColor[3] = cube->Color.a;
				record.CubeTextureIndex = cube->TextureIndex;
				record.CubeTilingFactor = cube->TilingFactor;
			}

			if (material)
			{
				record.MaterialAlbedo[0] = material->Albedo.r;
				record.MaterialAlbedo[1] = material->Albedo.g;
				record.MaterialAlbedo[2] = material->Albedo.b;
				record.MaterialAlbedo[3] = material->Albedo.a;
				record.MaterialTextureIndex = material->TextureIndex;
				record.MaterialNormalStrength = material->NormalStrength;
				record.MaterialMetallic = material->Metallic;
				record.MaterialRoughness = material->Roughness;
			}

			if (cylinder)
			{
				record.CylinderColor[0] = cylinder->Color.r;
				record.CylinderColor[1] = cylinder->Color.g;
				record.CylinderColor[2] = cylinder->Color.b;
				record.CylinderColor[3] = cylinder->Color.a;
				record.CylinderTextureIndex = cylinder->TextureIndex;
				record.CylinderTilingFactor = cylinder->TilingFactor;
				record.CylinderRadius = cylinder->Radius;
				record.CylinderHeight = cylinder->Height;
				record.CylinderSegments = cylinder->Segments;
			}

			if (sphere)
			{
				record.SphereColor[0] = sphere->Color.r;
				record.SphereColor[1] = sphere->Color.g;
				record.SphereColor[2] = sphere->Color.b;
				record.SphereColor[3] = sphere->Color.a;
				record.SphereTextureIndex = sphere->TextureIndex;
				record.SphereTilingFactor = sphere->TilingFactor;
				record.SphereRadius = sphere->Radius;
			}

			if (pointLight)
			{
				record.PointLightColor[0] = pointLight->Color.r;
				record.PointLightColor[1] = pointLight->Color.g;
				record.PointLightColor[2] = pointLight->Color.b;
				record.PointLightIntensity = pointLight->Intensity;
				record.PointLightRange = pointLight->Range;
				record.PointLightFalloff = pointLight->Falloff;
			}

			record.TagLength = static_cast<std::uint32_t>(tag->Tag.size());
			std::memcpy(record.Tag, tag->Tag.data(), tag->Tag.size());

			records.push_back(record);
		}

		Header header{};
		std::memcpy(header.Magic, kMagic, sizeof(kMagic));
		header.Version = kVersion;
		header.HeaderSize = sizeof(Header);
		header.RecordSize = sizeof(Record);
		header.EntityCount = static_cast<std::uint32_t>(records.size());
		header.SourceYamlSize = static_cast<std::uint64_t>(yamlSize);
		header.SourceYamlWriteTime =
			static_cast<std::uint64_t>(yamlWriteTime.time_since_epoch().count());
		header.FragmentCount = static_cast<std::uint32_t>(fragments.size());
		header.FragmentBytes = 0;
		for (const auto &fragment : fragments)
			header.FragmentBytes += static_cast<std::uint64_t>(fragment.size());

		const std::filesystem::path sidecarPath = SidecarPathFor(scenePath);
		std::ofstream out(sidecarPath, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return fail("could not open sidecar for writing: " + sidecarPath.string());

		out.write(reinterpret_cast<const char *>(&header), sizeof(header));
		if (!records.empty())
			out.write(reinterpret_cast<const char *>(records.data()),
				static_cast<std::streamsize>(records.size() * sizeof(Record)));

		// Fragments follow the records, each prefixed by its byte length. The length is
		// needed because a fragment is arbitrary YAML and has no fixed size; without it a
		// truncated file could not be distinguished from a short document.
		for (const auto &fragment : fragments)
		{
			const std::uint32_t length = static_cast<std::uint32_t>(fragment.size());
			out.write(reinterpret_cast<const char *>(&length), sizeof(length));
			out.write(fragment.data(), static_cast<std::streamsize>(fragment.size()));
		}
		out.flush();
		const bool good = out.good();
		out.close();
		if (!good || out.fail())
		{
			// A half-written sidecar must never survive: it would fail validation later
			// anyway, but removing it keeps the directory honest.
			Remove(scenePath);
			return fail("failed while writing sidecar: " + sidecarPath.string());
		}

		return true;
	}

	bool SceneChunkStore::Read(Scene &scene, const std::filesystem::path &scenePath,
		std::vector<std::string> *fragments, std::string *reason)
	{
		const auto refuse = [reason](const std::string &message)
		{
			if (reason)
				*reason = message;
			return false;
		};

		// Cleared only after the whole file validates, so a rejected sidecar cannot leave
		// the caller's scene emptied by accident.
		if (fragments)
			fragments->clear();

		const std::filesystem::path sidecarPath = SidecarPathFor(scenePath);
		std::ifstream in(sidecarPath, std::ios::binary);
		if (!in.is_open())
			return refuse("no sidecar");

		Header header{};
		if (!ReadExact(in, &header, sizeof(header)))
			return refuse("sidecar header is truncated");
		if (std::memcmp(header.Magic, kMagic, sizeof(kMagic)) != 0)
			return refuse("sidecar magic does not match");
		if (header.Version != kVersion)
			return refuse("sidecar version " + std::to_string(header.Version) +
				" does not match " + std::to_string(kVersion));
		if (header.HeaderSize != sizeof(Header) || header.RecordSize != sizeof(Record))
			return refuse("sidecar layout does not match this build");

		// Staleness: the sidecar describes one specific revision of the YAML file.
		std::error_code ec;
		const std::uintmax_t yamlSize = std::filesystem::file_size(scenePath, ec);
		if (ec)
			return refuse("scene file could not be stat'd: " + ec.message());
		const auto yamlWriteTime = std::filesystem::last_write_time(scenePath, ec);
		if (ec)
			return refuse("scene file write time could not be read: " + ec.message());
		if (header.SourceYamlSize != static_cast<std::uint64_t>(yamlSize) ||
			header.SourceYamlWriteTime !=
				static_cast<std::uint64_t>(yamlWriteTime.time_since_epoch().count()))
			return refuse("sidecar is stale (the scene file changed since it was written)");

		// Read every record before touching the scene, so a truncated file cannot leave a
		// half-populated scene behind.
		std::vector<Record> records(header.EntityCount);
		if (!records.empty() &&
			!ReadExact(in, records.data(),
				static_cast<std::size_t>(header.EntityCount) * sizeof(Record)))
			return refuse("sidecar is truncated: expected " +
				std::to_string(header.EntityCount) + " records");

		// Fragments, length-prefixed exactly as Write laid them out. Read fully before
		// touching the scene, for the same reason as the records.
		std::vector<std::string> parsedFragments;
		for (std::uint32_t i = 0; i < header.FragmentCount; ++i)
		{
			std::uint32_t length = 0;
			if (!ReadExact(in, &length, sizeof(length)))
				return refuse("sidecar is truncated: missing fragment " +
					std::to_string(i) + " length");
			if (length == 0 || length > kMaxFragmentBytes)
				return refuse("sidecar fragment " + std::to_string(i) +
					" has an implausible length of " + std::to_string(length));
			std::string fragment(length, '\0');
			if (!ReadExact(in, fragment.data(), length))
				return refuse("sidecar is truncated: expected " + std::to_string(length) +
					" bytes for fragment " + std::to_string(i));
			parsedFragments.push_back(std::move(fragment));
		}

		scene.m_Registry.clear();

		for (const Record &record : records)
		{
			Entity entity = scene.CreateEntityWithUUID(UUID(record.Uuid),
				std::string(record.Tag, record.TagLength));

			auto &transform = entity.GetComponent<TransformComponent>();
			transform.Translation = { record.Translation[0], record.Translation[1],
				record.Translation[2] };
			transform.Rotation = { record.Rotation[0], record.Rotation[1], record.Rotation[2] };
			transform.Scale = { record.Scale[0], record.Scale[1], record.Scale[2] };

			if (record.Flags & kFlagCubeRenderer)
			{
				auto &cube = entity.AddComponent<CubeRendererComponent>();
				cube.Color = { record.CubeColor[0], record.CubeColor[1], record.CubeColor[2],
					record.CubeColor[3] };
				cube.TextureIndex = record.CubeTextureIndex;
				cube.TilingFactor = record.CubeTilingFactor;
			}

			if (record.Flags & kFlagMaterial)
			{
				auto &material = entity.AddComponent<MaterialComponent>();
				material.Albedo = { record.MaterialAlbedo[0], record.MaterialAlbedo[1],
					record.MaterialAlbedo[2], record.MaterialAlbedo[3] };
				material.TextureIndex = record.MaterialTextureIndex;
				material.NormalStrength = record.MaterialNormalStrength;
				material.Metallic = record.MaterialMetallic;
				material.Roughness = record.MaterialRoughness;
			}

			if (record.Flags & kFlagCylinderRenderer)
			{
				auto &cylinder = entity.AddComponent<CylinderRendererComponent>();
				cylinder.Color = { record.CylinderColor[0], record.CylinderColor[1],
					record.CylinderColor[2], record.CylinderColor[3] };
				cylinder.TextureIndex = record.CylinderTextureIndex;
				cylinder.TilingFactor = record.CylinderTilingFactor;
				cylinder.Radius = record.CylinderRadius;
				cylinder.Height = record.CylinderHeight;
				cylinder.Segments = record.CylinderSegments;
			}

			if (record.Flags & kFlagSphereRenderer)
			{
				auto &sphere = entity.AddComponent<SphereRendererComponent>();
				sphere.Color = { record.SphereColor[0], record.SphereColor[1],
					record.SphereColor[2], record.SphereColor[3] };
				sphere.TextureIndex = record.SphereTextureIndex;
				sphere.TilingFactor = record.SphereTilingFactor;
				sphere.Radius = record.SphereRadius;
			}

			if (record.Flags & kFlagPointLight)
			{
				auto &pointLight = entity.AddComponent<PointLightComponent>();
				pointLight.Color = { record.PointLightColor[0], record.PointLightColor[1],
					record.PointLightColor[2] };
				pointLight.Intensity = record.PointLightIntensity;
				pointLight.Range = record.PointLightRange;
				pointLight.Falloff = record.PointLightFalloff;
			}
		}

		// Handed to the caller for replay through SceneSerializer's reader. Done last, so a
		// sidecar that validated but somehow still fails to load records leaves the caller's
		// vector untouched rather than holding stale data.
		if (fragments)
			*fragments = std::move(parsedFragments);

		return true;
	}

}
