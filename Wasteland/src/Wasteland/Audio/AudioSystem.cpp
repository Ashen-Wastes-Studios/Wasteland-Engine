// Precompiled header first, by engine convention.
#include "wlpch.h"

// This class's declaration, plus the Mixer it forwards most work to.
#include "AudioSystem.h"

// Entity's component accessors and IsValid, used throughout.
#include "Wasteland/Scene/Entity.h"
// The owning scene: the registry view, the primary camera, and the
// listener query.

// Needed as a complete type because PlayAutoStartSources and
// FindListenerEntity call through it rather than through a reference.
#include "Wasteland/Scene/Scene.h"

// C++17 nested namespace, matching the declaration in AudioSystem.h.
namespace Wasteland::Audio
{

	// Takes a Ref to the scene, so the mixer outlives nothing it depends on:
	// destroying the scene first drops this system's reference, not vice versa.

	// No null check here, though every method below tests m_Scene -- so a null
	// scene constructs cleanly and fails later, at the first Update.
	AudioSystem::AudioSystem(Scene *scene)
		// Member initialiser. m_Mixer is default-constructed, which is fine: Mixer is
		// documented as having no platform device.
		: m_Scene(scene)
	{
	}

	// Loads a WAV from disk and registers it under its file stem.

	// Returns bool rather than throwing or asserting: a missing sound should not
	// stop the game, per the comment in the failure branch below.
	bool AudioSystem::LoadClip(const std::string &path)
	{
		// An OUT-parameter to the loader, initialised optimistically.

		// Pre-seeding it with Success means a loader that fails to write it at all
		// still reports success -- so the initial value is a guess, not a default
		// that means anything.
		AudioClip::LoadResult result = AudioClip::LoadResult::Success;
		// The load. A null return signals failure, with `result` carrying the reason.
		Ref<AudioClip> clip = AudioClip::LoadWav(path, &result);

		// Failure path. Tested on the pointer rather than on `result`, which is the
		// right check -- the pointer is what the caller needs.
		if (!clip)
		{
			// Logged rather than asserted: a missing sound should not stop the
			// game, but it must be visible, or "why is there no footstep" has
			// no answer.
			// Logged rather than asserted, and the reason is given: "why is there no
			// footstep" needs an answer somewhere.

			// WL_CORE_ERROR is not compiled out in release, so unlike WL_CORE_ASSERT this
			// diagnostic survives.
			WL_CORE_ERROR("Audio: could not load '{0}': {1}", path,
						  AudioClip::LoadResultToString(result));
			return false;
		}

		// The file stem is the registered name, so gameplay refers to
		// "shotgun_fire" and not to a path it must keep in sync with the asset
		// layout.
		// Registered under the file's stem, so gameplay says "shotgun_fire" and never
		// has to track the asset layout.

		// DEFECT: the header documents that re-registering the same name REPLACES the
		// clip, which is what a content reload needs. But m_LastListenerName's own
		// comment claims a per-clip volume "survives the clip being re-registered",
		// and it cannot: this registration creates a fresh clip object, and any
		// per-clip volume stored against the old one is gone. So that field's
		// documented purpose is not actually achieved by this code path.
		m_Mixer.RegisterClip(clip->GetName(), clip);
		return true;
	}

	// Play a registered clip as a 2D sound.
	int AudioSystem::Play(const std::string &clipName)
	{
		// Checked BEFORE building the request, so the mixer never sees an unknown
		// name.
		if (!m_Mixer.HasClip(clipName))
		{
			// Warned rather than errored: gameplay asking for an unregistered clip is
			// usually a content mistake, not a runtime failure.

			// Note PlayAt below performs the same check but returns silently -- no
			// warning. So a typo in a PlayAt call is completely silent while the same typo
			// in a Play call is logged. That inconsistency is probably unintentional.
			WL_CORE_WARN("Audio: play requested for unregistered clip '{0}'", clipName);
			return -1;
		}

		// A plain struct, default-initialised, then filled field by field.

		// Any field not set here keeps its default, which is why the order of the
		// assignments below matters: whatever Spatial defaults to applies unless
		// overwritten.
		PlayRequest request;
		// The one field Play always sets. Everything else stays at its default.
		request.ClipName = clipName;
		return m_Mixer.Play(request);
	}

	// Play at an explicit world position -- the convenience most gameplay needs
	// for footsteps, impacts and doors.
	int AudioSystem::PlayAt(const std::string &clipName, const glm::vec3 &position,
							float volume, float pitch)
	{
		if (!m_Mixer.HasClip(clipName))
			return -1;

		PlayRequest request;
		request.ClipName = clipName;
		// Marks the request as 3D, which is what makes the mixer apply distance and
		// panning.
		request.Spatial = true;
		request.Position = position;
		request.Volume = volume;
		request.Pitch = pitch;
		return m_Mixer.Play(request);
	}

	// Play an entity's own sound, honouring its AudioSourceComponent.

	// Every failure returns -1 rather than throwing, per the header: playing an
	// entity's sound when it has no source is a scripting error, but not one worth
	// crashing over.
	int AudioSystem::PlayEntity(Entity entity)
	{
		// Both preconditions in one condition. Safe to combine with || because ||
		// short-circuits, so HasComponent is not called on an invalid entity.
		if (!entity.IsValid() || !entity.HasComponent<AudioSourceComponent>())
			return -1;

		// Non-const reference, though nothing here mutates it. GetComponent returns
		// T& and has no const overload, so const is not available even if wanted.
		auto &source = entity.GetComponent<AudioSourceComponent>();
		// A source component with no clip assigned. Returns -1 silently -- a source
		// left unconfigured by content is a normal authoring state, so warning here
		// would be noise.

		// Note this is checked BEFORE the HasClip lookup below, so the two failures
		// have different diagnostics: empty name is silent, unregistered name warns.
		if (source.ClipName.empty())
			return -1;

		if (!m_Mixer.HasClip(source.ClipName))
		{
			WL_CORE_WARN("Audio: entity '{0}' references unregistered clip '{1}'",
						 entity.GetName(), source.ClipName);
			return -1;
		}

		PlayRequest request;
		request.ClipName = source.ClipName;
		request.Volume = source.Volume;
		request.Pitch = source.Pitch;
		request.Loop = source.Loop;
		// Priority copied verbatim. Higher priority presumably wins voice stealing;
		// the mixer owns that policy.
		request.Priority = source.Priority;
		request.Spatial = source.Spatial;
		request.MinDistance = source.MinDistance;
		request.MaxDistance = source.MaxDistance;

		// Position only matters for a 3D source.
		if (source.Spatial)
		{
			// Default-constructed, so if ResolveSourcePosition fails it stays at the
			// origin -- which is why the else branch exists to avoid using it.
			glm::vec3 position;
			// Resolve the world position, applying the source's local offset.

			// Dereferences m_Scene WITHOUT a null check, unlike every other method in
			// this class. An AudioSystem built with a null scene reaches this line only if
			// the entity is valid and has a spatial source -- at which point this
			// dereferences a null Ref. See the m_Scene test in Update() for the pattern
			// that should be used here.
			// The resolved position, offset and all.
			if (ResolveSourcePosition(*m_Scene, entity, position))
				request.Position = position;
			else
				// No transform to read. Fall back to the listener rather than
				// playing at the world origin, which would make every
				// transform-less source sound like it came from world zero.

				// The right choice: at the origin every transform-less source would sound
				// like it came from world zero, which is audible as all of them panning
				// together.

				// DEFECT: it uses the mixer's CURRENT listener position, which is only
				// updated in Update() below. So during the frame before the first Update, this
				// reads a default-constructed position rather than the real listener.
				request.Position = m_Mixer.GetListener().Position;
		}

		return m_Mixer.Play(request);
	}

	// Stops every voice belonging to a clip.

	// Named StopClip, not StopVoice -- it affects all voices of that clip.
	void AudioSystem::StopClip(const std::string &clipName, float fadeSeconds)
	{
		// Resolve the name to a clip. Returns null for an unknown name.

		// Throws away the fade parameter's opportunity: if the clip is missing there
		// is nothing to stop, and the function returns silently rather than warning.
		Ref<AudioClip> clip = m_Mixer.GetClip(clipName);
		// Null check, so an unknown name is a no-op.
		if (clip)
			m_Mixer.StopClip(clip.get(), fadeSeconds);
	}

	void AudioSystem::StopAll(float fadeSeconds)
	{
		m_Mixer.StopAll(fadeSeconds);
	}

	// Per-clip volume, so gameplay can duck or emphasise without touching
	// individual voices.
	void AudioSystem::SetClipVolume(const std::string &clipName, float volume)
	{
		Ref<AudioClip> clip = m_Mixer.GetClip(clipName);
		if (clip)
			// Passed as a raw pointer because the mixer does not take ownership of clips
			// -- it holds its own Ref in its registry.
			m_Mixer.SetClipVolume(clip.get(), volume);
	}

	// Fires every AudioSourceComponent marked PlayOnStart. Called ONCE at runtime
	// start, not per frame -- so it is not on the hot path.
	void AudioSystem::PlayAutoStartSources()
	{
		// Null-scene guard, present here and in Update and FindListenerEntity.

		// So the absence of this check in PlayEntity is an inconsistency rather than
		// a deliberate omission.
		if (!m_Scene)
			return;

		// Via the public GetAllEntitiesWith accessor rather than the registry:
		// m_Registry is private, and reaching into it from another class would
		// break the encapsulation Scene's accessors exist to provide.
		// Every entity carrying an AudioSourceComponent.

		// A VIEW, not a copy, so this stays cheap even in a large scene. The existing
		// comment explains the choice of the public accessor over the private registry.
		auto view = m_Scene->GetAllEntitiesWith<AudioSourceComponent>();
		// Range-for over the view's handles.
		for (auto e : view)
		{
			// Reads the flag through the view rather than constructing an Entity, which
			// avoids a per-entity construction just to read a bool.
			if (!view.get<AudioSourceComponent>(e).PlayOnStart)
				continue;
			// Reconstruct an Entity from the handle to reuse the full play path.

			// `m_Scene` is the raw Scene*, since Entity's constructor takes a
			// non-owning pointer. That means this Entity is valid only as long as the scene
			// -- true for the duration of the loop.

			// The return value is discarded: a voice id that nothing receives. Acceptable
			// for a one-shot fire-and-forget, though a failure here is then completely
			// silent -- a PlayOnStart source with an unregistered clip logs nothing here.
			PlayEntity(Entity(e, m_Scene));
		}
	}

	// Resolves the listener: an explicit AudioListenerComponent, else the primary
	// camera.

	// Const, and it returns by value. Called every frame from Update below.
	Entity AudioSystem::FindListenerEntity() const
	{
		if (!m_Scene)
			return {};

		// An explicit AudioListenerComponent wins. A scene with two is a
		// content bug, but taking the first is more useful than refusing to
		// pick, and the panning is still coherent.
		// Every candidate listener, scanned linearly for the first ENABLED one.
		auto view = m_Scene->GetAllEntitiesWith<AudioListenerComponent>();

		// DEFECT (per-frame cost): this runs on EVERY Update, and GetAllEntitiesWith
		// iterates the whole registry. With many entities and no AudioListenerComponent,
		// that is a full registry walk per frame to conclude "use the camera". The
		// listener rarely changes, so caching it -- invalidated on scene change -- would
		// remove an O(entities) cost from the frame loop.
		for (auto e : view)
		{
			// Skips disabled listeners rather than treating them as candidates, so a
			// studio can disable one without deleting it.
			if (view.get<AudioListenerComponent>(e).Enabled)
				return Entity(e, m_Scene);
		}

		// Otherwise the ears follow the eyes. Tying the listener to the
		// primary camera is what a player expects without authoring it, and it
		// means a level with no listener component still pans correctly.
		// DEFECT: if the scene has no primary camera either, this returns an invalid
		// Entity. Update() below checks IsValid() before using it, so it degrades to
		// leaving the listener at its last position rather than crashing.
		return m_Scene->GetPrimaryCameraEntity();
	}

	Entity AudioSystem::GetListenerEntity() const
	{
		return FindListenerEntity();
	}

	// Computes a source's world position, applying its local offset.

	// STATIC and taking the scene by const reference rather than reading m_Scene,
	// so a test can call it directly -- the header says exactly that this is why
	// it is public.
	bool AudioSystem::ResolveSourcePosition(const Scene &scene, Entity entity,
											glm::vec3 &outPosition)
	{
		// No transform means no position to report. False, so the caller can fall
		// back -- which PlayEntity does.
		if (!entity.IsValid() || !entity.HasComponent<TransformComponent>())
			return false;

		const auto &transform = entity.GetComponent<TransformComponent>();
		// Start from the entity's world translation. An out-parameter written before
		// any failure is possible below, so the caller's variable is always set on a
		// true return.
		outPosition = transform.Translation;

		// A non-zero local offset is how a muzzle or a foot sits slightly in
		// front of and below the body without needing a child entity.
		if (entity.HasComponent<AudioSourceComponent>())
		{
			const auto &source = entity.GetComponent<AudioSourceComponent>();
			// A zero offset is handled by skipping, which avoids a pointless quaternion
			// multiply for the common case.

			// The test is dot(offset, offset) > 0, i.e. length-squared greater than zero.
			// Comparing the vector to zero directly would not compile, and comparing
			// .length() would cost a square root.
			if (glm::dot(source.PositionOffset, source.PositionOffset) > 0.0f)
			{
				// The offset is in the entity's local space, so apply the
				// entity's rotation. Without that, turning around would leave a
				// footstep playing from where the body used to face.
				// A local copy, since the member is const and the quaternion multiply is
				// non-const.

				// Unnecessary: the quaternion could be applied to the const member directly,
				// since rotation * offset only needs a const vec3. Harmless.
				glm::vec3 offset = source.PositionOffset;
				glm::vec3 radians = glm::radians(transform.Rotation);
				glm::quat rotation = glm::quat(radians);
				// Apply the rotation and add.

				// The vec3 cast around a quaternion-times-vec3 is a no-op: the expression
				// already yields a vec3. Harmless, but it reads as if a conversion were needed.
				outPosition += glm::vec3(rotation * offset);
			}
		}

		return true;
	}

	// Per-frame work: advance the mixer, then move the listener.

	// Called every frame, so everything here is on the hot path.
	void AudioSystem::Update(float seconds)
	{
		// Advance voice envelopes and positions. Done unconditionally, even with a
		// null scene, because the mixer is independent of the scene.
		m_Mixer.Update(seconds);

		if (!m_Scene)
			return;

		// Resolve the listener every frame.

		// Re-resolved rather than cached -- see the per-frame cost note at
		// FindListenerEntity().
		Entity listener = FindListenerEntity();
		// A mutable reference to the mixer's listener state, which the next four
		// lines overwrite.
		auto &listenerState = m_Mixer.GetListener();

		// Only update when there is a real listener with a transform.

		// No else branch, so an invalid or transform-less listener leaves the
		// previous frame's orientation in place. Stable rather than jittering to zero,
		// though after a scene swap that means the stale orientation persists.
		if (listener.IsValid() && listener.HasComponent<TransformComponent>())
		{
			const auto &transform = listener.GetComponent<TransformComponent>();
			listenerState.Position = transform.Translation;

			// The scene stores Euler degrees; the mixer wants a direction.
			// glm's pitch convention is degrees-about-Y-then-X, so the YXZ
			// order matches how a camera yaw/pitch is normally composed and
			// avoids the roll that Euler order otherwise introduces.
			glm::vec3 radians = glm::radians(transform.Rotation);
			glm::quat rotation = glm::quat(radians);
			// Forward is -Z rotated -- the engine's forward convention, matching a right-
			// handed camera looking down its own negative Z.

			// normalize is needed because the quaternion-times-vector product is not
			// unit length after interpolation.
			listenerState.Forward = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, -1.0f));
			// Up is +Y rotated.
			listenerState.Up = glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
			// Rebuilds the listener's orthonormal basis (right, up, forward) from the two
			// vectors above.

			// Not called in the constructor or on any other path, so it runs once per
			// frame. The basis could drift from Forward/Up if a caller set those directly,
			// since RecomputeBasis is the only thing keeping them consistent.
			listenerState.RecomputeBasis();
		}
	}

}
