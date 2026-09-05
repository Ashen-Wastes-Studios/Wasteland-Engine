#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include "SceneCamera.h"

#include "Wasteland/Renderer/Texture.h"

#include "Wasteland/Core/UUID.h"

namespace Wasteland
{

	struct IDComponent
	{
		UUID ID;

		IDComponent() = default;
		IDComponent(const IDComponent &) = default;
	};

	struct TagComponent
	{
		std::string Tag;

		TagComponent() = default;
		TagComponent(const TagComponent &) = default;
		TagComponent(const std::string &tag)
			: Tag(tag) {}
	};

	struct TransformComponent
	{
		glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
		glm::vec3 Rotation = {0.0f, 0.0f, 0.0f};
		glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

		TransformComponent() = default;
		TransformComponent(const TransformComponent &other)
			: Translation(other.Translation), Rotation(other.Rotation), Scale(other.Scale) {}
		TransformComponent(const glm::vec3 &translation)
			: Translation(translation) {}

		glm::mat4 GetTransform() const
		{
			if (Translation != m_CachedTranslation ||
				Rotation != m_CachedRotation ||
				Scale != m_CachedScale)
			{
				glm::mat4 translation = glm::translate(glm::mat4(1.0f), Translation);
				glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));
				glm::mat4 scale = glm::scale(glm::mat4(1.0f), Scale);
				m_CachedTransform = translation * rotation * scale;
				m_CachedTranslation = Translation;
				m_CachedRotation = Rotation;
				m_CachedScale = Scale;
			}
			return m_CachedTransform;
		}

		void SetTransform(const glm::mat4 &transform)
		{
			glm::vec3 skew;
			glm::vec4 perspective;
			glm::quat orientation;
			glm::decompose(transform, Scale, orientation, Translation, skew, perspective);
			Rotation = glm::eulerAngles(orientation);
		}

	private:
		mutable glm::mat4 m_CachedTransform = glm::mat4(1.0f);
		mutable glm::vec3 m_CachedTranslation = {0.0f, 0.0f, 0.0f};
		mutable glm::vec3 m_CachedRotation = {0.0f, 0.0f, 0.0f};
		mutable glm::vec3 m_CachedScale = {1.0f, 1.0f, 1.0f};
	};

	struct SpriteRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		float TilingFactor = 1.0f;

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent &) = default;
		SpriteRendererComponent(const glm::vec4 &color)
			: Color(color) {}
	};

	struct CircleRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		float Thickness = 1.0f;
		float Fade = 0.005f;

		CircleRendererComponent() = default;
		CircleRendererComponent(const CircleRendererComponent &) = default;
	};

	struct CubeRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;

		CubeRendererComponent() = default;
		CubeRendererComponent(const CubeRendererComponent &) = default;
	};

	struct SphereRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		float Radius = 0.5f;
		int Sectors = 20; // Horizontal smoothness
		int Stacks = 20;  // Vertical smoothness

		SphereRendererComponent() = default;
		SphereRendererComponent(const SphereRendererComponent &) = default;
	};

	struct MaterialComponent
	{
		glm::vec4 Albedo = {1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float NormalStrength = 0.0f;
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		glm::vec4 EmissionColor = {1.0f, 1.0f, 1.0f, 1.0f};
		float EmissionIntensity = 0.0f;

		float DisplacementScale = 0.0f;

		bool HasGeneratedMaps = false;
		std::string TexturePath;

		MaterialComponent() = default;
		MaterialComponent(const MaterialComponent &) = default;
	};

	struct CameraComponent
	{
		SceneCamera Camera;
		bool Primary = true; // TODO: think about moving to Scene
		bool FixedAspectRatio = false;

		CameraComponent() = default;
		CameraComponent(const CameraComponent &) = default;
	};

	// Forward declaration
	class ScriptableEntity;

	struct NativeScriptComponent
	{
		ScriptableEntity *Instance = nullptr;

		ScriptableEntity *(*InstantiateScript)();
		void (*DestroyScript)(NativeScriptComponent *);

		template <typename T>
		void Bind()
		{
			InstantiateScript = []()
			{ return static_cast<ScriptableEntity *>(new T()); };
			DestroyScript = [](NativeScriptComponent *nsc)
			{ delete nsc->Instance; nsc->Instance = nullptr; };
		}
	};

	struct ScriptComponent
	{
		std::string ScriptPath;
		std::string ScriptName;

		ScriptComponent() = default;
		ScriptComponent(const ScriptComponent &) = default;
	};

	// Physics

	struct Rigidbody2DComponent
	{
		enum class BodyType
		{
			Static = 0,
			Dynamic,
			Kinematic
		};
		BodyType Type = BodyType::Static;
		bool FixedRotation = 0;

		// Storage for runtime
		void *RuntimeBody = nullptr;

		Rigidbody2DComponent() = default;
		Rigidbody2DComponent(const Rigidbody2DComponent &other) = default;
	};

	struct BoxCollider2DComponent
	{
		glm::vec2 Offset = {0.0f, 0.0f};
		glm::vec2 Size = {0.5f, 0.5f};

		// TODO: move into physics material in the future maybe
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		void *RuntimeFixture = nullptr;

		BoxCollider2DComponent() = default;
		BoxCollider2DComponent(const BoxCollider2DComponent &other) = default;
	};

	struct CircleCollider2DComponent
	{
		glm::vec2 Offset = {0.0f, 0.0f};
		float Radius = 0.5f;

		// TODO: move into physics material in the future maybe
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		void *RuntimeFixture = nullptr;

		CircleCollider2DComponent() = default;
		CircleCollider2DComponent(const CircleCollider2DComponent &other) = default;
	};

	// --- 3D Physics Components (Box3D) ---

	enum class RigidBody3DType
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	struct RigidBody3DComponent
	{
		RigidBody3DType Type = RigidBody3DType::Static;
		float GravityScale = 1.0f;
		float LinearDamping = 0.0f;
		float AngularDamping = 0.0f;
		bool FixedRotationX = false;
		bool FixedRotationY = false;
		bool FixedRotationZ = false;

		// Runtime handle (stores b3BodyId as uint64_t, opaque to this header)
		uint64_t RuntimeBody = 0;

		RigidBody3DComponent() = default;
		RigidBody3DComponent(const RigidBody3DComponent &other) = default;
	};

	struct BoxCollider3DComponent
	{
		glm::vec3 HalfExtents = {0.5f, 0.5f, 0.5f};
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		uint64_t RuntimeShape = 0;

		BoxCollider3DComponent() = default;
		BoxCollider3DComponent(const BoxCollider3DComponent &other) = default;
	};

	struct SphereCollider3DComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		uint64_t RuntimeShape = 0;

		SphereCollider3DComponent() = default;
		SphereCollider3DComponent(const SphereCollider3DComponent &other) = default;
	};

	struct CapsuleCollider3DComponent
	{
		float Radius = 0.5f;
		float Height = 1.0f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		uint64_t RuntimeShape = 0;

		CapsuleCollider3DComponent() = default;
		CapsuleCollider3DComponent(const CapsuleCollider3DComponent &other) = default;
	};

	// --- Volumetric Atmosphere Components ---
	// The entity's Transform defines the volume box: Translation = center,
	// Scale = full box size (a unit cube scaled by Scale, axis-aligned in world).
	// Renderer3D gathers all enabled volumes every frame and raymarches them
	// in both the Nova path-tracer and the raster (Basic) path.

	struct VolumetricFogComponent
	{
		bool Enabled = true;
		glm::vec3 Color = {0.7f, 0.75f, 0.8f};
		float Density = 0.05f;       // Base extinction coefficient (0 = clear)
		float Anisotropy = 0.3f;     // Henyey-Greenstein phase g (-0.9 .. 0.9, forward scatter)
		float HeightFalloff = 0.15f; // 0 = uniform, 1 = concentrated at volume bottom
		float NoiseStrength = 0.5f;  // 0 = uniform fog, 1 = fully broken up
		float NoiseScale = 0.25f;    // World-space noise frequency
		float WindSpeed = 0.05f;     // Noise drift speed (world units / second)
		int MaxSteps = 8;            // Raymarch steps inside this volume (1 .. 32)

		VolumetricFogComponent() = default;
		VolumetricFogComponent(const VolumetricFogComponent &other) = default;
	};

	struct VolumetricCloudsComponent
	{
		bool Enabled = true;
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		glm::vec3 AmbientTint = {0.45f, 0.55f, 0.7f};
		float Coverage = 0.5f;       // 0 = clear sky, 1 = overcast
		float Density = 0.6f;        // Extinction inside cloud mass
		float NoiseScale = 0.08f;    // World-space billow frequency
		float DetailAmount = 0.5f;   // High-frequency erosion (0 = smooth puffs)
		glm::vec2 WindDirection = {1.0f, 0.3f};
		float WindSpeed = 0.02f;     // Drift speed (world units / second)
		float SilverLining = 0.6f;   // Forward-scatter rim around sun (0 .. 1)
		float ShadowStrength = 0.7f; // Self-shadowing depth (0 = flat, 1 = dark cores)
		int MaxSteps = 12;           // Raymarch steps inside this volume (1 .. 32)

		VolumetricCloudsComponent() = default;
		VolumetricCloudsComponent(const VolumetricCloudsComponent &other) = default;
	};
}