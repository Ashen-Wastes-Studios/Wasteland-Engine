#pragma once

#include <cmath>

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

	// --- Water Simulation Components ---
// Entity Transform defines the water body: Translation.y = still water level,
// Scale.x/z = surface extents (a 1x1 XZ plane at local y=0, scaled by Scale).
// Rivers are directional: FlowDirection/FlowSpeed advect the waves and drive
// foam; lakes are calm presets; oceans are large high-amplitude presets.
// CPU-simulated Gerstner spectrum (6 directional components with physical
// dispersion + horizontal chop) so gameplay queries (buoyancy, height
// sampling) and the raster mesh agree; the mesh additionally applies the
// Gerstner horizontal displacement for true sharp-crested waves.

enum class WaterBodyType
{
	Lake = 0,
	River = 1,
	Ocean = 2
};

struct WaterComponent
{
	bool Enabled = true;
	WaterBodyType Type = WaterBodyType::Lake;

	glm::vec3 ShallowColor = {0.25f, 0.5f, 0.65f};
	glm::vec3 DeepColor = {0.02f, 0.15f, 0.3f};
	glm::vec3 FoamColor = {0.9f, 0.95f, 1.0f};

	float WaterDepth = 2.0f;      // Absorption depth for shallow/deep gradient
	float WaveAmplitude = 0.05f;  // World units, primary wave height
	float WaveLength = 3.0f;      // World units, primary wavelength (> 0.01)
	float WaveSpeed = 0.6f;       // Phase speed multiplier
	glm::vec2 WaveDirection = {1.0f, 0.3f};
	float Chop = 0.2f;            // Secondary detail waves (0 = glassy .. 1 = choppy)
	float Steepness = 0.1f;       // Gerstner horizontal chop (0 = sine swell .. 1 = sharp crests)

	glm::vec2 FlowDirection = {1.0f, 0.0f}; // River current dir in world XZ
	float FlowSpeed = 0.0f;       // World units/second advection (rivers > 0)
	float FoamAmount = 0.15f;     // 0 = none .. 1 = heavy foam on crests
	float FoamScale = 1.0f;       // Foam pattern frequency multiplier
	float ShoreFoam = 0.5f;       // Bank/shore foamline (0 = none .. 1 = strong)
	float ShoreWidth = 1.0f;      // World-unit width of the shoreline foam band

	float Transparency = 0.75f;   // 0 = opaque .. 1 = clear (tints raster albedo toward shallow)
	float Roughness = 0.1f;       // Specular sharpness (0 = mirror .. 1 = matte)
	float Metallic = 0.0f;

	int Segments = 48;            // Surface grid resolution per side (1 .. 128)
	float TimeScale = 1.0f;       // Simulation speed multiplier

	float Buoyancy = 1.0f;        // Upward force scale for gameplay scripts
	float WaterDensity = 1.0f;    // Drag/density scale for gameplay scripts

	float Time = 0.0f;            // Runtime accumulator (not serialized)

	WaterComponent() = default;
	WaterComponent(const WaterComponent &other) = default;

	static void ApplyPreset(WaterComponent &water, WaterBodyType type)
	{
		water.Type = type;
		switch (type)
		{
		case WaterBodyType::Lake:
			water.ShallowColor = {0.25f, 0.5f, 0.65f};
			water.DeepColor = {0.02f, 0.15f, 0.3f};
			water.WaterDepth = 2.0f;
			water.WaveAmplitude = 0.05f;
			water.WaveLength = 3.0f;
			water.WaveSpeed = 0.6f;
			water.WaveDirection = {1.0f, 0.3f};
			water.Chop = 0.2f;
			water.Steepness = 0.1f;
			water.FlowDirection = {1.0f, 0.0f};
			water.FlowSpeed = 0.0f;
			water.FoamAmount = 0.15f;
			water.ShoreFoam = 0.5f;
			water.ShoreWidth = 1.0f;
			water.Transparency = 0.75f;
			water.Roughness = 0.1f;
			break;
		case WaterBodyType::River:
			water.ShallowColor = {0.3f, 0.55f, 0.6f};
			water.DeepColor = {0.03f, 0.2f, 0.25f};
			water.WaterDepth = 1.2f;
			water.WaveAmplitude = 0.08f;
			water.WaveLength = 2.0f;
			water.WaveSpeed = 1.6f;
			water.WaveDirection = {1.0f, 0.0f};
			water.Chop = 0.5f;
			water.Steepness = 0.35f;
			water.FlowDirection = {1.0f, 0.0f};
			water.FlowSpeed = 1.5f;
			water.FoamAmount = 0.5f;
			water.ShoreFoam = 0.7f;
			water.ShoreWidth = 0.8f;
			water.Transparency = 0.6f;
			water.Roughness = 0.15f;
			break;
		case WaterBodyType::Ocean:
			water.ShallowColor = {0.15f, 0.45f, 0.65f};
			water.DeepColor = {0.005f, 0.08f, 0.22f};
			water.WaterDepth = 6.0f;
			water.WaveAmplitude = 0.35f;
			water.WaveLength = 12.0f;
			water.WaveSpeed = 1.2f;
			water.WaveDirection = {1.0f, 0.25f};
			water.Chop = 0.7f;
			water.Steepness = 0.6f;
			water.FlowDirection = {1.0f, 0.25f};
			water.FlowSpeed = 0.3f;
			water.FoamAmount = 0.4f;
			water.ShoreFoam = 0.3f;
			water.ShoreWidth = 2.0f;
			water.Transparency = 0.85f;
			water.Roughness = 0.08f;
			break;
		}
	}

	// --- Directional wave spectrum (6 components, heightfield) ---
	// Amplitudes follow a Phillips-style falloff from the primary swell;
	// Chop scales the high-frequency detail (0 = glassy single swell).
	// Each component carries a 2nd harmonic for peaked crests / flat
	// troughs, and deep-water dispersion ω=sqrt(g·k) so long swells
	// outrun chop — the same field drives gameplay queries and the mesh.
	static constexpr int WaveComponentCount = 6;
	static float SpectrumLengthRatio(int i)
	{
		constexpr float ratios[WaveComponentCount] = {1.0f, 0.55f, 0.32f, 0.19f, 0.11f, 0.06f};
		return ratios[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumShare(int i)
	{
		constexpr float shares[WaveComponentCount] = {0.55f, 0.22f, 0.11f, 0.06f, 0.035f, 0.025f};
		return shares[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumFanAngle(int i)
	{
		constexpr float angles[WaveComponentCount] = {0.0f, 0.40f, -0.54f, 0.82f, -1.01f, 1.24f};
		return angles[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumPhaseOffset(int i)
	{
		constexpr float phases[WaveComponentCount] = {0.0f, 1.3f, 4.1f, 2.2f, 5.0f, 0.7f};
		return phases[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}

	// Total possible crest height (culling padding + foam normalization)
	float MaxWaveHeight() const
	{
		return WaveAmplitude * (0.55f + 0.45f * glm::clamp(Chop, 0.0f, 1.0f));
	}

	// Advected sample position: the current carries the wave field with it
	glm::vec2 AdvectedPos(glm::vec2 worldXZ) const
	{
		float flowLen = glm::length(FlowDirection);
		if (FlowSpeed > 0.0001f && flowLen > 0.0001f)
			return worldXZ - (FlowDirection / flowLen) * (FlowSpeed * Time);
		return worldXZ;
	}

	// Evaluates the Gerstner surface in one trig pass: vertical height +
	// horizontal chop displacement (outDispX/Z, world units) + analytic
	// Gerstner normal. minWavelength fades out components shorter than the
	// mesh can resolve (spectral LOD: pass ~2x the mesh cell size from the
	// renderer; 0 keeps the full spectrum for gameplay queries).
	void EvaluateSurface(float worldX, float worldZ, float minWavelength, float &outHeight, float &outDispX, float &outDispZ, glm::vec3 &outNormal) const
	{
		outHeight = 0.0f;
		outDispX = 0.0f;
		outDispZ = 0.0f;
		outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		if (!Enabled || WaveAmplitude <= 0.00001f)
			return;
		glm::vec2 p = AdvectedPos({worldX, worldZ});

		glm::vec2 d1 = WaveDirection;
		if (glm::length2(d1) < 1e-8f)
			d1 = {1.0f, 0.0f};
		d1 = glm::normalize(d1);

		float chop = glm::clamp(Chop, 0.0f, 1.0f);
		float steep = glm::clamp(Steepness, 0.0f, 1.0f);
		float baseLen = glm::max(WaveLength, 0.1f);
		float t = Time * WaveSpeed;

		float nx = 0.0f, ny = 1.0f, nz = 0.0f;
		for (int i = 0; i < WaveComponentCount; i++)
		{
			float li = baseLen * SpectrumLengthRatio(i);
			float fade = 1.0f;
			if (minWavelength > 0.00001f && li < minWavelength * 2.0f)
			{
				if (li <= minWavelength)
					continue;
				fade = (li - minWavelength) / minWavelength; // 0..1 across the Nyquist band
			}
			float amp = WaveAmplitude * SpectrumShare(i) * (i == 0 ? 1.0f : chop) * fade;
			if (amp <= 0.0000001f)
				continue;
			float k = 6.2831853f / li;
			if (k > 40.0f)
				k = 40.0f;
			float omega = sqrtf(9.81f * k); // deep-water dispersion: swells outrun chop
			if (omega > 25.0f)
				omega = 25.0f;

			float ca = cosf(SpectrumFanAngle(i));
			float sa = sinf(SpectrumFanAngle(i));
			glm::vec2 dir(d1.x * ca - d1.y * sa, d1.x * sa + d1.y * ca);

			float sharp = (i == 0 ? 0.6f : 1.0f) * chop; // 2nd-harmonic crest sharpening
			float norm = 1.0f + 0.5f * sharp;
			float phi = k * glm::dot(dir, p) - omega * t;
			float s1 = sinf(phi);
			float c1 = cosf(phi);
			float harmonic = 2.0f * phi + SpectrumPhaseOffset(i);
			float s2 = sinf(harmonic);
			float c2 = cosf(harmonic);

			float wa = k * amp;
			// Per-wave Gerstner Q: equal-steepness distribution (GPU Gems),
			// clamped so crests sharpen without looping over.
			float qi = steep / (wa * (float)WaveComponentCount + 1e-6f);
			if (qi > 1.0f)
				qi = 1.0f;
			if (qi < 0.0f)
				qi = 0.0f;

			float shapeS = (s1 + 0.5f * sharp * s2) / norm;
			float shapeC = (c1 + sharp * c2) / norm;
			outHeight += amp * shapeS;
			outDispX += dir.x * (qi * amp) * shapeC;
			outDispZ += dir.y * (qi * amp) * shapeC;
			nx += -dir.x * wa * shapeC;
			nz += -dir.y * wa * shapeC;
			ny += -qi * wa * shapeS;
		}
		outNormal = glm::vec3(nx, ny, nz);
		if (glm::length2(outNormal) < 1e-10f)
			outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		else
			outNormal = glm::normalize(outNormal);
	}

	// Heightfield column height (no horizontal displacement): what gameplay
	// queries and buoyancy use — stable by construction, matches the mesh
	// surface to within the chop offset.
	float SampleHeightAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float h = 0.0f, dx = 0.0f, dz = 0.0f;
		glm::vec3 n(0.0f, 1.0f, 0.0f);
		EvaluateSurface(worldX, worldZ, minWavelength, h, dx, dz, n);
		return h;
	}

	glm::vec3 SampleNormalAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float h = 0.0f, dx = 0.0f, dz = 0.0f;
		glm::vec3 n(0.0f, 1.0f, 0.0f);
		EvaluateSurface(worldX, worldZ, minWavelength, h, dx, dz, n);
		return n;
	}

	// 0 = trough .. 1 = crest, for foam whitening
	float CrestFactorAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float maxH = MaxWaveHeight();
		if (maxH <= 0.00001f)
			return 0.0f;
		return glm::clamp(SampleHeightAt(worldX, worldZ, minWavelength) / maxH * 0.5f + 0.5f, 0.0f, 1.0f);
	}

	glm::vec2 SampleFlow() const
	{
		float flowLen = glm::length(FlowDirection);
		if (flowLen < 0.0001f || FlowSpeed <= 0.00001f)
			return glm::vec2(0.0f);
		return (FlowDirection / flowLen) * FlowSpeed;
	}
};

// --- Analytic Light Components ---
	// Gathered every frame by the Scene and uploaded to Renderer3D (up to
	// Renderer3D::MaxAnalyticLights, first-submitted wins). Unlike emissive
	// materials they need no geometry. Aim with the entity rotation: light
	// travels along local -Z. Shadowed in the Nova path; unshadowed in the
	// raster preview path (which has no shadow maps).

	struct DirectionalLightComponent
	{
		glm::vec3 Color = {1.0f, 0.96f, 0.9f};
		float Intensity = 1.5f;

		DirectionalLightComponent() = default;
		DirectionalLightComponent(const DirectionalLightComponent &other) = default;
	};

	struct PointLightComponent
	{
		glm::vec3 Color = {1.0f, 0.9f, 0.8f};
		float Intensity = 5.0f;
		float Range = 10.0f; // 0 = infinite
		float Falloff = 2.0f; // Windowing exponent (higher = tighter pool)

		PointLightComponent() = default;
		PointLightComponent(const PointLightComponent &other) = default;
	};

	struct SpotLightComponent
	{
		glm::vec3 Color = {1.0f, 0.95f, 0.85f};
		float Intensity = 8.0f;
		float Range = 15.0f; // 0 = infinite
		float Falloff = 2.0f;
		float InnerAngle = 25.0f; // Degrees, hot core (clamped <= outer)
		float OuterAngle = 40.0f; // Degrees, cone edge

		SpotLightComponent() = default;
		SpotLightComponent(const SpotLightComponent &other) = default;
	};

	struct AreaLightComponent
	{
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		float Intensity = 4.0f;
		float Width = 2.0f;  // Rect size in world units (local XY plane)
		float Height = 2.0f;
		float Range = 12.0f; // 0 = infinite
		bool DoubleSided = false; // Emit from both rect faces

		AreaLightComponent() = default;
		AreaLightComponent(const AreaLightComponent &other) = default;
	};
}