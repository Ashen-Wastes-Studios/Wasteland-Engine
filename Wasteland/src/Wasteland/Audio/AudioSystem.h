#pragma once

#include <string>

#include <glm/glm.hpp>

#include "Wasteland/Audio/Mixer.h"

namespace Wasteland
{

	class Entity;
	class Scene;

	namespace Audio
	{

		// Ties the device-independent Mixer to a Scene.
		//
		// The split is the whole design and is worth restating: everything that
		// decides WHAT you hear lives in Mixer and has no platform dependency,
		// so it is testable with no sound card. This class adds only the
		// scene-facing concerns -- following the listener entity, resolving
		// source components, and loading clips by path.
		//
		// There is deliberately no platform device here. A device pulls in a
		// backend per OS and cannot be exercised headlessly, so it sits behind
		// the mixer rather than in the middle of it. Until that backend exists,
		// the mixer runs and its output is inspectable via Mix(), which is
		// enough to build and test against.
		class AudioSystem
		{
		public:
			explicit AudioSystem(Scene *scene);
			~AudioSystem() = default;

			Mixer &GetMixer() { return m_Mixer; }
			const Mixer &GetMixer() const { return m_Mixer; }

			// Load and register a clip from a filesystem path. The registered
			// name is the file's stem, so "assets/audio/shotgun_fire.wav"
			// becomes "shotgun_fire".
			//
			// Returns false and logs the reason on failure. Registering the
			// same name twice replaces the clip, which is what a content reload
			// needs.
			bool LoadClip(const std::string &path);

			// Play a registered clip by name. Returns the voice id, or -1 if it
			// could not be started. See Mixer::Play for when -1 is expected.
			int Play(const std::string &clipName);

			// Play a clip as a 3D emitter at a world position. The convenience
			// most gameplay code needs: footsteps, impacts, doors.
			int PlayAt(const std::string &clipName, const glm::vec3 &position,
					   float volume = 1.0f, float pitch = 1.0f);

			// Play a clip attached to an entity, honouring its
			// AudioSourceComponent overrides (volume, pitch, loop, priority,
			// 3D-vs-2D, distances) and the entity's world position.
			//
			// Returns -1 if the entity has no AudioSourceComponent -- playing
			// "this entity's sound" when it has none is a scripting error, but
			// not one worth crashing over.
			int PlayEntity(Entity entity);

			// Stop every voice belonging to a clip.
			void StopClip(const std::string &clipName, float fadeSeconds = 0.0f);
			void StopAll(float fadeSeconds = 0.0f);

			// Per-clip volume, so gameplay can duck or emphasise a sound
			// without touching individual voices. Affects voices started after
			// the call and any currently playing.
			void SetClipVolume(const std::string &clipName, float volume);
			void SetMasterVolume(float volume) { m_Mixer.SetMasterVolume(volume); }
			float GetMasterVolume() const { return m_Mixer.GetMasterVolume(); }

			// Fire every AudioSourceComponent marked PlayOnStart. Called once
			// when the runtime starts, not per frame.
			void PlayAutoStartSources();

			// Per-frame work: advance voice time and follow the listener.
			//
			// The listener is the entity carrying AudioListenerComponent, or
			// failing that the primary camera -- tying the ears to the eyes is
			// the behaviour players expect without having to author it.
			void Update(float seconds);

			// The entity the listener is currently following, or an invalid
			// entity. For diagnostics.
			Entity GetListenerEntity() const;

			// Where a source's sound originates in world space, applying its
			// local offset. Public so a test can assert the offset is honoured
			// without inferring it from a panning result.
			static bool ResolveSourcePosition(const Scene &scene, Entity entity,
											 glm::vec3 &outPosition);

		private:
			// Find the entity the listener follows. Prefers an explicit
			// AudioListenerComponent, falls back to the primary camera.
			Entity FindListenerEntity() const;

			// A BORROWED back-pointer, deliberately NOT Ref<Scene>.
			//
			// Scene constructs this with `Ref<Scene>(this)`, which wraps a
			// non-owning raw pointer in a SECOND, independent shared_ptr control
			// block. Scene is already owned by whoever created it -- pybind11's
			// shared_ptr holder, or CreateRef in the editor -- so when both control
			// blocks die the Scene is deleted twice. The symptom is not a leak but
			// heap corruption (0xC0000374) on interpreter exit, and it appears the
			// moment any AudioSystem exists, which is why it went unnoticed: the
			// audio layer had no test at all.
			//
			// Every use below is a read -- `m_Scene->`, `*m_Scene`,
			// `m_Scene.get()`. None of them needs shared ownership, and a raw
			// borrowed pointer makes the lifetime requirement honest: the Scene
			// must outlive its AudioSystem, which it does by construction, since
			// the AudioSystem is a member of the Scene.
			Scene *m_Scene = nullptr;
			Mixer m_Mixer;
			// Remembered so a per-clip volume survives the clip being
			// re-registered; the Mixer stores volumes on voices, not clips.
			std::string m_LastListenerName;
		};

	}

}
