#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "Wasteland/Audio/AudioDevice.h"
#include "Wasteland/Audio/Mixer.h"
#include "Wasteland/Avb/Graph/ProjectRuntime.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Wasteland
{

	class Entity;
	class Scene;

	// Forward-declared rather than included: Components.h is a large header and
	// AudioSystem only ever holds a REFERENCE to this component. A forward
	// declaration is enough for that, and it keeps the include graph acyclic.
	struct AudioGraphComponent;

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
		// The platform device stays behind the device-independent mixer. Its
		// callback consumes fixed-rate mixer output; headless callers can still
		// exercise the mixer without opening any audio hardware.
		class AudioSystem
		{
		public:
			explicit AudioSystem(Scene *scene);
			~AudioSystem();

			Mixer &GetMixer() { return m_Mixer; }
			const Mixer &GetMixer() const { return m_Mixer; }

			// Explicit output lifecycle for the Scene runtime integration. Failure
			// is non-fatal and reports the backend/device error to the caller.
			bool StartOutput(std::string *error = nullptr) { return m_AudioDevice.Start(error); }
			void StopOutput() { m_AudioDevice.Stop(); }
			bool IsOutputRunning() const { return m_AudioDevice.IsRunning(); }
			uint32_t GetOutputEndpointSampleRate() const
			{
				return m_AudioDevice.GetEndpointSampleRate();
			}

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

			// ---- Audio graphs ----
			//
			// A .wlavb attached to an entity. The component is DATA; this is the
			// runtime it describes, keyed by entity id because a runtime is neither
			// copyable nor serializable and the chunk store copies components.

			struct GraphInstance
			{
				std::unique_ptr<Avb::AvbProjectRuntime> Runtime;
				std::shared_ptr<ProceduralVoice> Voice;
				int MixerVoiceId = -1;
				UUID EntityId;
				std::string GraphPath;
				std::string Error;

				// The instance's own clock, advanced by the mixer block that drives
				// its voice. A graph is a continuous source, so a one-shot trigger
				// needs a limit or it holds a voice forever.
				float Elapsed = 0.0f;
				float DurationSeconds = 0.0f;

				bool Spatial = false;
				float MinDistance = 1.0f;
				float MaxDistance = 30.0f;
				int Priority = 0;
				float Volume = 1.0f;

				// Triggers queued from gameplay, drained at the next block boundary.
				std::vector<std::pair<std::string, std::string>> Pending;
			};

			// Copy the entity-facing fields onto the instance. Static because it reads
		// no AudioSystem state; split out because AttachGraph runs it on both the
		// reload and the already-loaded path and the two must not drift.
		static void SyncInstanceFromComponent(GraphInstance &instance,
											  const AudioGraphComponent &component);

		// Start the instance's mixer voice if it does not already have a live one.
		void StartGraphVoice(GraphInstance &instance, Entity entity,
							 const AudioGraphComponent &component);

		// Load (or reload) the graph named by the entity's AudioGraphComponent.
			// A failed load leaves any previous runtime live and reports the reason;
			// a graph that cannot compile is an authoring error, not a crash.
			bool AttachGraph(Entity entity, std::string *error = nullptr);

			// Drop the runtime and retire its voice. Called when the component is
			// removed or the entity destroyed.
			void DetachGraph(Entity entity);

			// E.2: map a gameplay event to a graph trigger. Returns true when the
			// event reached a running graph. An event with no matching Trigger entry
			// is a quiet no-op -- the same convention ScriptEventDispatcher uses for
			// a missing Python method, so a game can fire an event at every entity
			// without checking which ones listen.
			bool TriggerGraph(Entity entity, const std::string &event,
							  const std::string &arg = {});

			// Diagnostics. GetGraphVoiceCount is the number the plan's verification
			// reads: it rises when a graph starts and falls when it ends, which is
			// the difference between "the graph is attached" and "the graph is
			// making a sound".
			int GetGraphVoiceCount() const;
			bool IsGraphAttached(Entity entity) const;
			std::string GetGraphError(Entity entity) const;

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

			// A borrowed parent pointer, deliberately NOT Ref<Scene>. Scene
			// constructs this with `this`; wrapping that raw pointer in a new
			// shared_ptr control block would create independent ownership of the
			// Scene. AudioSystem only dereferences the parent and must remain
			// within the Scene's lifetime; Scene stops its output device before
			// tearing down runtime state.
			Scene *m_Scene = nullptr;
			Mixer m_Mixer;
			AudioDevice m_AudioDevice;
			// Remembered so a per-clip volume survives the clip being
			// re-registered; the Mixer stores volumes on voices, not clips.
			std::string m_LastListenerName;

			// Per-frame graph work: prune instances whose component is gone, and
		// follow the emitter for a spatial graph voice.
		void UpdateGraphs();

		// One entry per entity carrying an AudioGraphComponent. Pruned in Update
			// against the scene rather than on component removal: there is no
			// OnComponentRemoved hook in Scene, so a component removed without an
			// explicit DetachGraph would otherwise leak a runtime and hold a voice.
			std::unordered_map<UUID, GraphInstance> m_Graphs;
		};

	}

}
