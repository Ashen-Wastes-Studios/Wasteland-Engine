#pragma once

#include <memory>
#include <string>
#include <unordered_map>

// The three animation types are included rather than forward-declared because
// Runtime stores an Animator and an AnimationStateMachine BY VALUE: a field of
// incomplete type is ill-formed, and a pointer would add an allocation and a
// lifetime question to the common case for no benefit.
#include "Wasteland/Animation/AnimationClip.h"
#include "Wasteland/Animation/AnimationStateMachine.h"
#include "Wasteland/Animation/Animator.h"
#include "Wasteland/Animation/SkinnedMesh.h"
#include "Wasteland/Renderer/ProceduralMeshes.h"
#include "Wasteland/Core/UUID.h"
#include "Wasteland/Scene/Entity.h"

namespace Wasteland
{
	class Scene;

	// Loads `.wlanim` assets and ticks animation for every entity carrying an
	// AnimationComponent.
	//
	// Mirrors `FlowGraphSystem` and `BehaviorTreeSystem` deliberately: a
	// project-relative path, load once on first tick, one runtime per entity, a
	// latch so a broken asset is not retried every frame. A designer should not
	// have to learn a fourth attachment model, and a bug found in one should be
	// looked for in the others.
	//
	// The difference from the other two is the STATE MACHINE. A graph or tree
	// has a single entry point; an animation asset may carry a machine that owns
	// its own Animator and its own transition logic. So the runtime keeps BOTH
	// -- a machine when the asset has one, and a bare Animator when it does not
	// -- and GetAnimator returns whichever is live. That is the accessor a test,
	// the editor, and gameplay code all want: one way to ask "what is playing
	// on this entity?".
	//
	// Kept out of Scene.cpp for the same reason as its siblings: it needs the
	// serializer, the Animator and the state machine together, and because it is
	// the unit a test wants to drive directly.
	class AnimationSystem
	{
	public:
		// Called every runtime tick for every entity carrying an
		// AnimationComponent. Loads the asset on first sight.
		static void OnUpdateEntity(Scene &scene, Entity entity, float seconds);

		// Drop the cached runtime for an entity being destroyed. Without this a
		// destroyed entity's runtime would survive until Shutdown, and a respawn
		// reusing the UUID would inherit a stale Animator.
		static void OnDestroyEntity(UUID id);

		// Drop every live runtime. Must run while any asset the runtime holds is
		// still valid to release; the assets are shared_ptrs, so this is about
		// deterministic teardown rather than lifetime correctness.
		static void Shutdown();

		// The effective Animator for an entity: the state machine's own when the
		// asset carries one, otherwise the runtime's bare Animator. nullptr when
		// the entity has no runtime -- either no component, an empty path, or an
		// asset that failed to load.
		//
		// This is the accessor the verification and the editor both need. It
		// returns a raw pointer rather than a reference because "no runtime" is
		// an ordinary outcome, not an error.
		static class Animation::Animator *GetAnimator(UUID id);

		// Hard-cuts to a named state and sets whether that instance loops.
		// Returns false when the entity has no runtime, the asset carries no state
		// machine, or the name is unknown -- the three cases a gameplay script can
		// actually recover from. This is what the PlayAnimation graph node drives.
		static bool PlayState(UUID id, const std::string &stateName, bool loop);

		// Crossfades to a named state over `duration` seconds. Same failure set as
		// PlayState. A duration of zero or less falls back to the STATE's own
		// transition duration (TransitionTo's -1 sentinel), which is what an author
		// means by "use whatever the machine says".
		//
		// Separate from PlayState rather than a flag on it: a hard cut and a fade
		// are different authoring intents, and a mode pin would make the careless
		// one and the deliberate one the same act.
		static bool TransitionState(UUID id, const std::string &stateName,
									float duration);

		// Fires a state-machine trigger by name. Triggers are how a machine leaves
		// a state without a transition authored for every cause -- one transition
		// per trigger name, fired from gameplay.
		static bool FireTrigger(UUID id, const std::string &trigger);

		// Hands the runtime a skinned mesh to deform every tick. Returns false
		// when the entity has no live runtime or the mesh fails Validate. The
		// mesh is COPIED: the runtime owns its geometry so a caller mutating
		// theirs afterwards cannot change what is drawn.
		static bool SetSkinnedMesh(UUID id, const Animation::SkinnedMesh &mesh);

		// The deformed geometry for this entity, ready for
		// Renderer3D::DrawSkinnedMesh, or nullptr when there is nothing to draw
		// (no runtime, no mesh, or skinning failed this frame).
		static const ProceduralMesh *GetDeformedMesh(UUID id);

		// The deformed positions only, as the CPU skinner produced them. Exists
		// so a test can assert vertex positions numerically without a GL context.
		static const std::vector<glm::vec3> *GetDeformedPositions(UUID id);

	private:
		// Fires the OnAnimationEvent script hook when a state is entered. Split out
		// from PlayState/TransitionState because both need it and because the
		// argument construction (a py::str) is not a one-liner.
		static void NotifyAnimationState(UUID id, const std::string &stateName);

		struct Runtime
		{
			std::shared_ptr<class Animation::Skeleton> Skeleton;

			// The bare Animator, used when the asset has NO state machine. Kept
			// as a member rather than a pointer so the common case needs no
			// allocation.
			class Animation::Animator Animator;

			// The machine, used when the asset HAS one. It owns its own Animator,
			// which is the one GetAnimator returns in that case.
			class Animation::AnimationStateMachine Machine;
			bool HasMachine = false;

			// The clip the bare Animator should play, once a caller picks one.
			// Null until then: an asset with no machine is directly controlled,
			// and the runtime does not guess a clip.
			std::shared_ptr<class Animation::AnimationClip> Clip;

			// The instance the bare Animator started for Clip, or -1. Sparse ids
			// mean a caller must not treat 0 as "none".
			int InstanceId = -1;

			// The geometry to deform, empty when the entity is animated without
			// a mesh (a door, a headless rig).
			Animation::SkinnedMesh Mesh;
			bool HasMesh = false;
			std::vector<glm::mat4> SkinMatrices; // reused across frames
			ProceduralMesh Deformed;             // what the renderer consumes
			bool DeformedValid = false;
		};

		// UUID -> the per-entity runtime. A map rather than an array because the
		// runtime holds an Animator, which is not cheap to move, and entities
		// come and go.
		static std::unordered_map<UUID, Runtime> s_Runtimes;

		// UUIDs whose asset failed to load, so a broken asset logs once rather
		// than every frame. Deliberately not cleared on success: once an asset
		// fails, only a reload (or a scene reload) should retry it.
		static std::unordered_map<UUID, bool> s_Failed;
	};
}
