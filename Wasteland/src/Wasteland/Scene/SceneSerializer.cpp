#include "wlpch.h"
#include "SceneSerializer.h"

#include "Entity.h"

#include <fstream>
#include <filesystem>

#include <yaml-cpp/yaml.h>
#include "Components.h"

namespace YAML
{

	template <>
	struct convert<glm::vec2>
	{
		static Node encode(const glm::vec2 &rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node &node, glm::vec2 &rhs)
		{
			if (!node.IsSequence() || node.size() != 2)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			return true;
		}
	};

	template <>
	struct convert<glm::vec3>
	{
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
	struct convert<glm::vec4>
	{
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

	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec2 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
		return out;
	}

	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec3 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		return out;
	}

	YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec4 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
		return out;
	}

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

		WL_CORE_ASSERT(false, "Unknown body type");
		return {};
	}

	static Rigidbody2DComponent::BodyType RigidBody2DBodyTypeFromString(const std::string bodyTypeString)
	{
		if (bodyTypeString == "Static")
			return Rigidbody2DComponent::BodyType::Static;
		if (bodyTypeString == "Dynamic")
			return Rigidbody2DComponent::BodyType::Dynamic;
		if (bodyTypeString == "Kinematic")
			return Rigidbody2DComponent::BodyType::Kinematic;

		WL_CORE_ASSERT(false, "Unknown body type");
		return Rigidbody2DComponent::BodyType::Static;
	}

	SceneSerializer::SceneSerializer(const Ref<Scene> &scene)
		: m_Scene(scene)
	{
	}

	static void SerializeEntity(YAML::Emitter &out, Entity entity)
	{
		WL_CORE_ASSERT(entity.HasComponent<IDComponent>(), nullptr);

		out << YAML::BeginMap; // Entity
		out << YAML::Key << "Entity" << YAML::Value << entity.GetUUID();

		if (entity.HasComponent<TagComponent>())
		{
			out << YAML::Key << "TagComponent";
			out << YAML::BeginMap; // TagComponent

			auto &tag = entity.GetComponent<TagComponent>().Tag;
			out << YAML::Key << "Tag" << YAML::Value << tag;

			out << YAML::EndMap; // TagComponent
		}

		if (entity.HasComponent<TransformComponent>())
		{
			out << YAML::Key << "TransformComponent";
			out << YAML::BeginMap; // TransformComponent

			auto &tc = entity.GetComponent<TransformComponent>();
			out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
			out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
			out << YAML::Key << "Scale" << YAML::Value << tc.Scale;

			out << YAML::EndMap; // TransformComponent
		}

		if (entity.HasComponent<CameraComponent>())
		{
			out << YAML::Key << "CameraComponent";
			out << YAML::BeginMap; // CameraComponent

			auto &cameraComponent = entity.GetComponent<CameraComponent>();
			auto &camera = cameraComponent.Camera;

			out << YAML::Key << "Camera" << YAML::Value;
			out << YAML::BeginMap; // Camera
			out << YAML::Key << "ProjectionType" << YAML::Value << (int)camera.GetProjectionType();
			out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
			out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
			out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
			out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
			out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
			out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
			out << YAML::EndMap; // Camera

			out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
			out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;

			out << YAML::EndMap; // CameraComponent
		}

		if (entity.HasComponent<SpriteRendererComponent>())
		{
			out << YAML::Key << "SpriteRendererComponent";
			out << YAML::BeginMap; // SpriteRendererComponent

			auto &spriteRendererComponent = entity.GetComponent<SpriteRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << spriteRendererComponent.Color;

			out << YAML::EndMap; // SpriteRendererComponent
		}

		if (entity.HasComponent<CircleRendererComponent>())
		{
			out << YAML::Key << "CircleRendererComponent";
			out << YAML::BeginMap; // CircleRendererComponent

			auto &circleRendererComponent = entity.GetComponent<CircleRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << circleRendererComponent.Color;
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
			out << YAML::Key << "TextureWidth" << YAML::Value << (cubeRendererComponent.Texture ? cubeRendererComponent.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (cubeRendererComponent.Texture ? cubeRendererComponent.Texture->GetHeight() : 0);
			out << YAML::Key << "TextureIndex" << YAML::Value << cubeRendererComponent.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << cubeRendererComponent.TilingFactor;

			out << YAML::EndMap; // CubeRendererComponent
		}

		if (entity.HasComponent<SphereRendererComponent>())
		{
			out << YAML::Key << "SphereRendererComponent";
			out << YAML::BeginMap; // SphereRendererComponent

			auto &sphereRendererComponent = entity.GetComponent<SphereRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << sphereRendererComponent.Color;
			out << YAML::Key << "TextureWidth" << YAML::Value << (sphereRendererComponent.Texture ? sphereRendererComponent.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (sphereRendererComponent.Texture ? sphereRendererComponent.Texture->GetHeight() : 0);
			out << YAML::Key << "Radius" << YAML::Value << sphereRendererComponent.Radius;
			out << YAML::Key << "Sectors" << YAML::Value << sphereRendererComponent.Sectors;
			out << YAML::Key << "Stacks" << YAML::Value << sphereRendererComponent.Stacks;
			out << YAML::Key << "TextureIndex" << YAML::Value << sphereRendererComponent.TextureIndex;
			out << YAML::Key << "TilingFactor" << YAML::Value << sphereRendererComponent.TilingFactor;

			out << YAML::EndMap; // SphereRendererComponent
		}

		if (entity.HasComponent<MaterialComponent>())
		{
			out << YAML::Key << "MaterialComponent";
			out << YAML::BeginMap; // MaterialComponent

			auto &materialComponent = entity.GetComponent<MaterialComponent>();
			out << YAML::Key << "Albedo" << YAML::Value << materialComponent.Albedo;
			out << YAML::Key << "Metallic" << YAML::Value << materialComponent.Metallic;
			out << YAML::Key << "Roughness" << YAML::Value << materialComponent.Roughness;
			out << YAML::Key << "EmissionColor" << YAML::Value << materialComponent.EmissionColor;
			out << YAML::Key << "EmissionIntensity" << YAML::Value << materialComponent.EmissionIntensity;

			out << YAML::EndMap; // MaterialComponent
		}

		if (entity.HasComponent<Rigidbody2DComponent>())
		{
			out << YAML::Key << "Rigidbody2DComponent";
			out << YAML::BeginMap; // Rigidbody2DComponent

			auto &rigidbody2DComponent = entity.GetComponent<Rigidbody2DComponent>();
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
			out << YAML::Key << "Friction" << YAML::Value << circleCollider2DComponent.Friction;
			out << YAML::Key << "Restitution" << YAML::Value << circleCollider2DComponent.Restitution;
			out << YAML::Key << "RestitutionThreshold" << YAML::Value << circleCollider2DComponent.RestitutionThreshold;

			out << YAML::EndMap; // CircleCollider2DComponent
		}

		if (entity.HasComponent<ScriptComponent>())
		{
			out << YAML::Key << "ScriptComponent";
			out << YAML::BeginMap; // ScriptComponent

			auto &sc = entity.GetComponent<ScriptComponent>();
			std::string pathToSave = sc.ScriptPath;
			if (pathToSave.find("assets/") == std::string::npos)
			{
				pathToSave = "assets/" + pathToSave;
			}
			out << YAML::Key << "ScriptPath" << YAML::Value << pathToSave;
			out << YAML::Key << "ScriptName" << YAML::Value << sc.ScriptName;

			out << YAML::EndMap; // ScriptComponent
		}

		out << YAML::EndMap; // Entity
	}

	void SceneSerializer::Serialize(const std::string &filepath)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Scene" << YAML::Value << "Untitled";
		out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
		m_Scene->m_Registry.view<entt::entity>().each([&](auto entityID)
													  {
				Entity entity = { entityID, m_Scene.get() };
				if (!entity)
					return;

				SerializeEntity(out, entity); });
		out << YAML::EndSeq;
		out << YAML::EndMap;

		std::ofstream fout(filepath);
		fout << out.c_str();
	}

	void SceneSerializer::SerializeRuntime(const std::string &filepath)
	{
		// Not implemented
		WL_CORE_ASSERT(false, nullptr);
	}

	bool SceneSerializer::Deserialize(const std::string &filepath)
	{
		std::ifstream stream(filepath);
		std::stringstream strStream;
		strStream << stream.rdbuf();

		YAML::Node data = YAML::Load(strStream.str());
		if (!data["Scene"])
			return false;

		std::string sceneName = data["Scene"].as<std::string>();
		WL_CORE_TRACE("Deserializing scene '{0}'", sceneName);

		auto entities = data["Entities"];
		if (entities)
		{
			for (auto entity : entities)
			{
				uint64_t uuid = entity["Entity"].as<uint64_t>();

				std::string name;
				auto tagComponent = entity["TagComponent"];
				if (tagComponent)
					name = tagComponent["Tag"].as<std::string>();

				WL_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

				Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

				auto transformComponent = entity["TransformComponent"];
				if (transformComponent)
				{
					// Entities always have transforms
					auto &tc = deserializedEntity.GetComponent<TransformComponent>();
					tc.Translation = transformComponent["Translation"].as<glm::vec3>();
					tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
					tc.Scale = transformComponent["Scale"].as<glm::vec3>();
				}

				auto cameraComponent = entity["CameraComponent"];
				if (cameraComponent)
				{
					auto &cc = deserializedEntity.AddComponent<CameraComponent>();

					auto cameraProps = cameraComponent["Camera"];

					if (cameraProps)
					{
						cc.Camera.SetProjectionType((SceneCamera::ProjectionType)cameraProps["ProjectionType"].as<int>());

						cc.Camera.SetPerspectiveVerticalFOV(cameraProps["PerspectiveFOV"].as<float>());
						cc.Camera.SetPerspectiveNearClip(cameraProps["PerspectiveNear"].as<float>());
						cc.Camera.SetPerspectiveFarClip(cameraProps["PerspectiveFar"].as<float>());

						cc.Camera.SetOrthographicSize(cameraProps["OrthographicSize"].as<float>());
						cc.Camera.SetOrthographicNearClip(cameraProps["OrthographicNear"].as<float>());
						cc.Camera.SetOrthographicFarClip(cameraProps["OrthographicFar"].as<float>());
					}

					cc.Primary = cameraComponent["Primary"].as<bool>();
					cc.FixedAspectRatio = cameraComponent["FixedAspectRatio"].as<bool>();
				}

				auto spriteRendererComponent = entity["SpriteRendererComponent"];
				if (spriteRendererComponent)
				{
					// Entities always have transforms
					auto &src = deserializedEntity.AddComponent<SpriteRendererComponent>();
					src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
				}

				auto circleRendererComponent = entity["CircleRendererComponent"];
				if (circleRendererComponent)
				{
					// Entities always have transforms
					auto &crc = deserializedEntity.AddComponent<CircleRendererComponent>();
					crc.Color = circleRendererComponent["Color"].as<glm::vec4>();
					crc.Thickness = circleRendererComponent["Thickness"].as<float>();
					crc.Fade = circleRendererComponent["Fade"].as<float>();
				}

				auto cubeRendererComponent = entity["CubeRendererComponent"];
				if (cubeRendererComponent)
				{
					// Entities always have transforms
					auto &crc = deserializedEntity.AddComponent<CubeRendererComponent>();
					crc.Color = cubeRendererComponent["Color"].as<glm::vec4>();
					if (cubeRendererComponent["TextureWidth"] && cubeRendererComponent["TextureHeight"])
					{
						int width = cubeRendererComponent["TextureWidth"].as<int>();
						int height = cubeRendererComponent["TextureHeight"].as<int>();
						if (width > 0 && height > 0)
							crc.Texture = Texture2D::Create(width, height);
					}
					crc.TextureIndex = cubeRendererComponent["TextureIndex"].as<int>();
					crc.TilingFactor = cubeRendererComponent["TilingFactor"].as<float>();
				}

				auto sphereRendererComponent = entity["SphereRendererComponent"];
				if (sphereRendererComponent)
				{
					// Entities always have transforms
					auto &src = deserializedEntity.AddComponent<SphereRendererComponent>();
					src.Color = sphereRendererComponent["Color"].as<glm::vec4>();
					if (sphereRendererComponent["TextureWidth"] && sphereRendererComponent["TextureHeight"])
					{
						int width = sphereRendererComponent["TextureWidth"].as<int>();
						int height = sphereRendererComponent["TextureHeight"].as<int>();
						if (width > 0 && height > 0)
							src.Texture = Texture2D::Create(width, height);
					}
					src.Radius = sphereRendererComponent["Radius"].as<float>();
					src.Sectors = sphereRendererComponent["Sectors"].as<int>();
					src.Stacks = sphereRendererComponent["Stacks"].as<int>();
					src.TextureIndex = sphereRendererComponent["TextureIndex"].as<int>();
					src.TilingFactor = sphereRendererComponent["TilingFactor"].as<float>();
				}

				auto materialComponent = entity["MaterialComponent"];
				if (materialComponent)
				{
					// Entities always have transforms
					auto &mc = deserializedEntity.AddComponent<MaterialComponent>();
					mc.Albedo = materialComponent["Albedo"].as<glm::vec4>();
					mc.Metallic = materialComponent["Metallic"].as<float>();
					mc.Roughness = materialComponent["Roughness"].as<float>();
					mc.EmissionColor = materialComponent["EmissionColor"].as<glm::vec4>();
					mc.EmissionIntensity = materialComponent["EmissionIntensity"].as<float>();
				}

				auto rigidbody2DComponent = entity["Rigidbody2DComponent"];
				if (rigidbody2DComponent)
				{
					// Entities always have transforms
					auto &rigidbody2D = deserializedEntity.AddComponent<Rigidbody2DComponent>();
					rigidbody2D.Type = RigidBody2DBodyTypeFromString(rigidbody2DComponent["BodyType"].as<std::string>());
					rigidbody2D.FixedRotation = rigidbody2DComponent["FixedRotation"].as<bool>();
				}

				auto boxCollider2DComponent = entity["BoxCollider2DComponent"];
				if (boxCollider2DComponent)
				{
					// Entities always have transforms
					auto &boxCollider2D = deserializedEntity.AddComponent<BoxCollider2DComponent>();
					boxCollider2D.Offset = boxCollider2DComponent["Offset"].as<glm::vec2>();
					boxCollider2D.Size = boxCollider2DComponent["Size"].as<glm::vec2>();
					boxCollider2D.Density = boxCollider2DComponent["Density"].as<float>();
					boxCollider2D.Restitution = boxCollider2DComponent["Restitution"].as<float>();
					boxCollider2D.RestitutionThreshold = boxCollider2DComponent["RestitutionThreshold"].as<float>();
				}

				auto circleCollider2DComponent = entity["CircleCollider2DComponent"];
				if (circleCollider2DComponent)
				{
					// Entities always have transforms
					auto &circleCollider2D = deserializedEntity.AddComponent<CircleCollider2DComponent>();
					circleCollider2D.Offset = circleCollider2DComponent["Offset"].as<glm::vec2>();
					circleCollider2D.Radius = circleCollider2DComponent["Radius"].as<float>();
					circleCollider2D.Density = circleCollider2DComponent["Density"].as<float>();
					circleCollider2D.Restitution = circleCollider2DComponent["Restitution"].as<float>();
					circleCollider2D.RestitutionThreshold = circleCollider2DComponent["RestitutionThreshold"].as<float>();
				}

				auto scriptComponent = entity["ScriptComponent"];
				if (scriptComponent)
				{
					auto &sc = deserializedEntity.AddComponent<ScriptComponent>();
					std::string rawPath = scriptComponent["ScriptPath"].as<std::string>();
					// Always store as a relative path
					sc.ScriptPath = "scripts/" + std::filesystem::path(rawPath).filename().string();
					sc.ScriptName = scriptComponent["ScriptName"].as<std::string>();
				}
			}
		}

		return true;
	}

	bool SceneSerializer::DeserializeRuntime(const std::string &filepath)
	{
		// Not implemented
		WL_CORE_ASSERT(false, nullptr);
		return false;
	}

}