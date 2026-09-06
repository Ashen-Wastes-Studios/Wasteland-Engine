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

	static std::string RigidBody3DTypeToString(RigidBody3DType type)
	{
		switch (type)
		{
		case RigidBody3DType::Static:    return "Static";
		case RigidBody3DType::Dynamic:   return "Dynamic";
		case RigidBody3DType::Kinematic: return "Kinematic";
		}
		return "Static";
	}

	static RigidBody3DType RigidBody3DTypeFromString(const std::string &str)
	{
		if (str == "Dynamic")   return RigidBody3DType::Dynamic;
		if (str == "Kinematic") return RigidBody3DType::Kinematic;
		return RigidBody3DType::Static;
	}


	static std::string WaterBodyTypeToString(WaterBodyType type)
	{
		switch (type)
		{
		case WaterBodyType::Lake:  return "Lake";
		case WaterBodyType::River: return "River";
		case WaterBodyType::Ocean: return "Ocean";
		}
		return "Lake";
	}


	static WaterBodyType WaterBodyTypeFromString(const std::string &str)
	{
		if (str == "River") return WaterBodyType::River;
		if (str == "Ocean") return WaterBodyType::Ocean;
		return WaterBodyType::Lake;
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
			out << YAML::Key << "TextureWidth" << YAML::Value << (materialComponent.Texture ? materialComponent.Texture->GetWidth() : 0);
			out << YAML::Key << "TextureHeight" << YAML::Value << (materialComponent.Texture ? materialComponent.Texture->GetHeight() : 0);
			out << YAML::Key << "TextureIndex" << YAML::Value << materialComponent.TextureIndex;
			std::string pathToSave = materialComponent.TexturePath;
			std::replace(pathToSave.begin(), pathToSave.end(), '\\', '/');
			if (pathToSave.find("assets/") == std::string::npos)
			{
				pathToSave = "assets/" + pathToSave;
			}
			out << YAML::Key << "TexturePath" << YAML::Value << pathToSave;
			out << YAML::Key << "HasGeneratedMaps" << YAML::Value << materialComponent.HasGeneratedMaps;
			out << YAML::Key << "NormalStrength" << YAML::Value << materialComponent.NormalStrength;
			out << YAML::Key << "Metallic" << YAML::Value << materialComponent.Metallic;
			out << YAML::Key << "Roughness" << YAML::Value << materialComponent.Roughness;
			out << YAML::Key << "EmissionColor" << YAML::Value << materialComponent.EmissionColor;
			out << YAML::Key << "EmissionIntensity" << YAML::Value << materialComponent.EmissionIntensity;
			out << YAML::Key << "DisplacementScale" << YAML::Value << materialComponent.DisplacementScale;

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

		if (entity.HasComponent<RigidBody3DComponent>())
		{
			out << YAML::Key << "RigidBody3DComponent";
			out << YAML::BeginMap;
			auto &rb3d = entity.GetComponent<RigidBody3DComponent>();
			out << YAML::Key << "BodyType" << YAML::Value << RigidBody3DTypeToString(rb3d.Type);
			out << YAML::Key << "GravityScale" << YAML::Value << rb3d.GravityScale;
			out << YAML::Key << "LinearDamping" << YAML::Value << rb3d.LinearDamping;
			out << YAML::Key << "AngularDamping" << YAML::Value << rb3d.AngularDamping;
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
			out << YAML::Key << "Enabled" << YAML::Value << fog.Enabled;
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(fog.Color, 1.0f);
			out << YAML::Key << "Density" << YAML::Value << fog.Density;
			out << YAML::Key << "Anisotropy" << YAML::Value << fog.Anisotropy;
			out << YAML::Key << "HeightFalloff" << YAML::Value << fog.HeightFalloff;
			out << YAML::Key << "NoiseStrength" << YAML::Value << fog.NoiseStrength;
			out << YAML::Key << "NoiseScale" << YAML::Value << fog.NoiseScale;
			out << YAML::Key << "WindSpeed" << YAML::Value << fog.WindSpeed;
			out << YAML::Key << "MaxSteps" << YAML::Value << fog.MaxSteps;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<VolumetricCloudsComponent>())
		{
			out << YAML::Key << "VolumetricCloudsComponent";
			out << YAML::BeginMap;
			auto &clouds = entity.GetComponent<VolumetricCloudsComponent>();
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
			out << YAML::Key << "DirectionalLightComponent";
			out << YAML::BeginMap;
			auto &light = entity.GetComponent<DirectionalLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << glm::vec4(light.Color, 1.0f);
			out << YAML::Key << "Intensity" << YAML::Value << light.Intensity;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<PointLightComponent>())
		{
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
			out << YAML::Key << "Height" << YAML::Value << light.Height;
			out << YAML::Key << "Range" << YAML::Value << light.Range;
			out << YAML::Key << "DoubleSided" << YAML::Value << light.DoubleSided;
			out << YAML::EndMap;
		}

		if (entity.HasComponent<WaterComponent>())
		{
			out << YAML::Key << "WaterComponent";
			out << YAML::BeginMap;
			auto &water = entity.GetComponent<WaterComponent>();
			out << YAML::Key << "Enabled" << YAML::Value << water.Enabled;
			out << YAML::Key << "BodyType" << YAML::Value << WaterBodyTypeToString(water.Type);
			out << YAML::Key << "ShallowColor" << YAML::Value << glm::vec4(water.ShallowColor, 1.0f);
			out << YAML::Key << "DeepColor" << YAML::Value << glm::vec4(water.DeepColor, 1.0f);
			out << YAML::Key << "FoamColor" << YAML::Value << glm::vec4(water.FoamColor, 1.0f);
			out << YAML::Key << "WaterDepth" << YAML::Value << water.WaterDepth;
			out << YAML::Key << "WaveAmplitude" << YAML::Value << water.WaveAmplitude;
			out << YAML::Key << "WaveLength" << YAML::Value << water.WaveLength;
			out << YAML::Key << "WaveSpeed" << YAML::Value << water.WaveSpeed;
			out << YAML::Key << "WaveDirection" << YAML::Value << water.WaveDirection;
			out << YAML::Key << "Chop" << YAML::Value << water.Chop;
			out << YAML::Key << "Steepness" << YAML::Value << water.Steepness;
			out << YAML::Key << "FlowDirection" << YAML::Value << water.FlowDirection;
			out << YAML::Key << "FlowSpeed" << YAML::Value << water.FlowSpeed;
			out << YAML::Key << "FoamAmount" << YAML::Value << water.FoamAmount;
			out << YAML::Key << "FoamScale" << YAML::Value << water.FoamScale;
			out << YAML::Key << "ShoreFoam" << YAML::Value << water.ShoreFoam;
			out << YAML::Key << "ShoreWidth" << YAML::Value << water.ShoreWidth;
			out << YAML::Key << "Transparency" << YAML::Value << water.Transparency;
			out << YAML::Key << "Roughness" << YAML::Value << water.Roughness;
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
					auto &crc = deserializedEntity.AddComponent<CubeRendererComponent>();
					crc.Color = cubeRendererComponent["Color"].as<glm::vec4>();
					crc.TextureIndex = cubeRendererComponent["TextureIndex"].as<int>();
					crc.TilingFactor = cubeRendererComponent["TilingFactor"].as<float>();
				}

				auto sphereRendererComponent = entity["SphereRendererComponent"];
				if (sphereRendererComponent)
				{
					auto &src = deserializedEntity.AddComponent<SphereRendererComponent>();
					src.Color = sphereRendererComponent["Color"].as<glm::vec4>();
					src.Radius = sphereRendererComponent["Radius"].as<float>();
					src.Sectors = sphereRendererComponent["Sectors"].as<int>();
					src.Stacks = sphereRendererComponent["Stacks"].as<int>();
					src.TextureIndex = sphereRendererComponent["TextureIndex"].as<int>();
					src.TilingFactor = sphereRendererComponent["TilingFactor"].as<float>();
				}

				auto materialComponent = entity["MaterialComponent"];
				if (materialComponent)
				{
					auto &mc = deserializedEntity.AddComponent<MaterialComponent>();
					mc.Albedo = materialComponent["Albedo"].as<glm::vec4>();
					mc.TextureIndex = materialComponent["TextureIndex"].as<int>();
					std::string rawPath = materialComponent["TexturePath"].as<std::string>();
					std::replace(rawPath.begin(), rawPath.end(), '\\', '/');
					// Collapse any duplicate "assets/" prefixes (from old serialization bugs)
					const std::string prefix = "assets/";
					while (rawPath.find(prefix + prefix) == 0)
						rawPath = rawPath.substr(prefix.size());
					mc.TexturePath = rawPath;
					if (!rawPath.empty() && std::filesystem::is_regular_file(rawPath))
						mc.Texture = Texture2D::Create(rawPath);
					mc.HasGeneratedMaps = materialComponent["HasGeneratedMaps"].as<bool>();
					mc.NormalStrength = materialComponent["NormalStrength"].as<float>();
					mc.Metallic = materialComponent["Metallic"].as<float>();
					mc.Roughness = materialComponent["Roughness"].as<float>();
					mc.EmissionColor = materialComponent["EmissionColor"].as<glm::vec4>();
					mc.EmissionIntensity = materialComponent["EmissionIntensity"].as<float>();
					if (materialComponent["DisplacementScale"])
						mc.DisplacementScale = materialComponent["DisplacementScale"].as<float>();
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

				auto rigidBody3DComponent = entity["RigidBody3DComponent"];
				if (rigidBody3DComponent)
				{
					auto &rb3d = deserializedEntity.AddComponent<RigidBody3DComponent>();
					rb3d.Type = RigidBody3DTypeFromString(rigidBody3DComponent["BodyType"].as<std::string>());
					rb3d.GravityScale = rigidBody3DComponent["GravityScale"].as<float>();
					rb3d.LinearDamping = rigidBody3DComponent["LinearDamping"].as<float>();
					rb3d.AngularDamping = rigidBody3DComponent["AngularDamping"].as<float>();
					if (rigidBody3DComponent["FixedRotationX"]) rb3d.FixedRotationX = rigidBody3DComponent["FixedRotationX"].as<bool>();
					if (rigidBody3DComponent["FixedRotationY"]) rb3d.FixedRotationY = rigidBody3DComponent["FixedRotationY"].as<bool>();
					if (rigidBody3DComponent["FixedRotationZ"]) rb3d.FixedRotationZ = rigidBody3DComponent["FixedRotationZ"].as<bool>();
				}

				auto boxCollider3DComponent = entity["BoxCollider3DComponent"];
				if (boxCollider3DComponent)
				{
					auto &bc = deserializedEntity.AddComponent<BoxCollider3DComponent>();
					bc.HalfExtents = boxCollider3DComponent["HalfExtents"].as<glm::vec3>();
					bc.Offset = boxCollider3DComponent["Offset"].as<glm::vec3>();
					bc.Density = boxCollider3DComponent["Density"].as<float>();
					bc.Friction = boxCollider3DComponent["Friction"].as<float>();
					bc.Restitution = boxCollider3DComponent["Restitution"].as<float>();
				}

				auto sphereCollider3DComponent = entity["SphereCollider3DComponent"];
				if (sphereCollider3DComponent)
				{
					auto &sc = deserializedEntity.AddComponent<SphereCollider3DComponent>();
					sc.Radius = sphereCollider3DComponent["Radius"].as<float>();
					sc.Offset = sphereCollider3DComponent["Offset"].as<glm::vec3>();
					sc.Density = sphereCollider3DComponent["Density"].as<float>();
					sc.Friction = sphereCollider3DComponent["Friction"].as<float>();
					sc.Restitution = sphereCollider3DComponent["Restitution"].as<float>();
				}

				auto capsuleCollider3DComponent = entity["CapsuleCollider3DComponent"];
				if (capsuleCollider3DComponent)
				{
					auto &cc = deserializedEntity.AddComponent<CapsuleCollider3DComponent>();
					cc.Radius = capsuleCollider3DComponent["Radius"].as<float>();
					cc.Height = capsuleCollider3DComponent["Height"].as<float>();
					cc.Offset = capsuleCollider3DComponent["Offset"].as<glm::vec3>();
					cc.Density = capsuleCollider3DComponent["Density"].as<float>();
					cc.Friction = capsuleCollider3DComponent["Friction"].as<float>();
					cc.Restitution = capsuleCollider3DComponent["Restitution"].as<float>();
				}

				auto volumetricFogComponent = entity["VolumetricFogComponent"];
				if (volumetricFogComponent)
				{
					auto &fog = deserializedEntity.AddComponent<VolumetricFogComponent>();
					if (volumetricFogComponent["Enabled"])
						fog.Enabled = volumetricFogComponent["Enabled"].as<bool>();
					if (volumetricFogComponent["Color"])
						fog.Color = glm::vec3(volumetricFogComponent["Color"].as<glm::vec4>());
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

				auto volumetricCloudsComponent = entity["VolumetricCloudsComponent"];
				if (volumetricCloudsComponent)
				{
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

				auto directionalLightComponent = entity["DirectionalLightComponent"];
				if (directionalLightComponent)
				{
					auto &light = deserializedEntity.AddComponent<DirectionalLightComponent>();
					if (directionalLightComponent["Color"])
						light.Color = glm::vec3(directionalLightComponent["Color"].as<glm::vec4>());
					if (directionalLightComponent["Intensity"])
						light.Intensity = directionalLightComponent["Intensity"].as<float>();
				}

				auto pointLightComponent = entity["PointLightComponent"];
				if (pointLightComponent)
				{
					auto &light = deserializedEntity.AddComponent<PointLightComponent>();
					if (pointLightComponent["Color"])
						light.Color = glm::vec3(pointLightComponent["Color"].as<glm::vec4>());
					if (pointLightComponent["Intensity"])
						light.Intensity = pointLightComponent["Intensity"].as<float>();
					if (pointLightComponent["Range"])
						light.Range = pointLightComponent["Range"].as<float>();
					if (pointLightComponent["Falloff"])
						light.Falloff = pointLightComponent["Falloff"].as<float>();
				}

				auto spotLightComponent = entity["SpotLightComponent"];
				if (spotLightComponent)
				{
					auto &light = deserializedEntity.AddComponent<SpotLightComponent>();
					if (spotLightComponent["Color"])
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

				auto areaLightComponent = entity["AreaLightComponent"];
				if (areaLightComponent)
				{
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

				auto waterComponent = entity["WaterComponent"];
				if (waterComponent)
				{
					auto &water = deserializedEntity.AddComponent<WaterComponent>();
					if (waterComponent["Enabled"])
						water.Enabled = waterComponent["Enabled"].as<bool>();
					if (waterComponent["BodyType"])
						water.Type = WaterBodyTypeFromString(waterComponent["BodyType"].as<std::string>());
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