// Loads `.wlanim` assets and ticks animation for AnimationComponent entities.
//
// See AnimationSystem.h for why this mirrors FlowGraphSystem and
// BehaviorTreeSystem, and why the runtime keeps BOTH a machine and a bare
// Animator.

#include "wlpch.h"
#include "Wasteland/Scene/AnimationSystem.h"

#include <algorithm>

#include "Wasteland/Animation/AnimationSerializer.h"
#include "Wasteland/Scene/Components.h"
#include "Wasteland/Scene/Scene.h"
// ScriptEventDispatcher, for the OnAnimationEvent hook a flow graph can
// react to. A same-directory dependency, not a layering break: the dispatcher
// already sits above the graph and tree systems.
#include "Wasteland/Scene/ScriptEventDispatcher.h"

namespace Wasteland
{
	// Defined here rather than inline in the header because Runtime is private
	// and these are the only writers. A header-defined map would also drag the
	// Animator definition into every TU that includes this header.
	std::unordered_map<UUID, AnimationSystem::Runtime> AnimationSystem::s_Runtimes;
	std::unordered_map<UUID, bool> AnimationSystem::s_Failed;

	namespace
	{
		// The state names of a machine, sorted.
		//
		// Sorted because GetStates() hands back an unordered_map, so taking
		// begin() would pick a different "first" state run to run. A character
		// whose entry state changes between sessions is a determinism bug, and
		// this is the cheap way to rule it out.
		std::vector<std::string> SortedStateNames(const Animation::AnimationStateMachine &machine)
		{
			std::vector<std::string> names;
			for (const auto &entry : machine.GetStates())
				names.push_back(entry.first);
			std::sort(names.begin(), names.end());
			return names;
		}

		// Area-weighted vertex normals from deformed positions.
		//
		// Skin() moves positions only, so the bind-pose normals would light a bent
		// elbow as if it were still straight. The mesh is de-indexed (one vertex per
		// triangle corner), so accumulating per index is per-corner and the result
		// is flat shading per face -- consistent with how the OBJ importer builds
		// these meshes, and correct rather than approximate for them.
		void RebuildNormals(ProceduralMesh &mesh)
		{
			std::fill(mesh.Normals.begin(), mesh.Normals.end(), glm::vec3(0.0f));
			for (size_t t = 0; t + 2 < mesh.Indices.size(); t += 3)
			{
				const uint32_t a = mesh.Indices[t], b = mesh.Indices[t + 1], c = mesh.Indices[t + 2];
				// Un-normalised cross product IS the area weighting.
				const glm::vec3 n = glm::cross(mesh.Positions[b] - mesh.Positions[a],
											   mesh.Positions[c] - mesh.Positions[a]);
				mesh.Normals[a] += n;
				mesh.Normals[b] += n;
				mesh.Normals[c] += n;
			}
			for (glm::vec3 &n : mesh.Normals)
			{
				const float len = glm::length(n);
				n = len > 1e-8f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
			}
		}

		// The pose the entity currently shows, whichever animator is live.
		const Animation::Pose &LivePose(const Animation::Animator &animator)
		{
			return animator.GetLastPose();
		}
	} // namespace

	// Deforms the runtime's mesh by the current pose. Failure leaves
	// DeformedValid false so the draw is skipped, never drawn half-skinned.
	static void DeformRuntime(Animation::Skeleton &skeleton, Animation::Animator &animator,
							  std::vector<glm::mat4> &skinMatrices, Animation::SkinnedMesh &mesh,
							  ProceduralMesh &out, bool &valid)
	{
		valid = false;
		skeleton.ComputeSkinningMatrices(LivePose(animator), skinMatrices);
		std::vector<glm::vec3> positions;
		if (!mesh.Skin(skinMatrices, positions) || positions.size() != mesh.Vertices.size())
			return;

		out.Positions = std::move(positions);
		out.Indices = mesh.Indices;
		// Normals/UVs sized once; UVs never change under skinning.
		if (out.TexCoords.size() != mesh.Vertices.size())
		{
			out.TexCoords.resize(mesh.Vertices.size());
			for (size_t i = 0; i < mesh.Vertices.size(); ++i)
				out.TexCoords[i] = glm::vec2(mesh.Vertices[i].TexCoord[0], mesh.Vertices[i].TexCoord[1]);
		}
		out.Normals.resize(out.Positions.size());
		RebuildNormals(out);
		valid = true;
	}

	void AnimationSystem::OnUpdateEntity(Scene &scene, Entity entity, float seconds)
	{
		(void)scene; // reserved: the pose may need the entity's world transform

		if (!entity.IsValid())
			return;

		auto &component = entity.GetComponent<AnimationComponent>();
		const UUID id = entity.GetUUID();

		// A component with no asset is inert rather than an error. An entity can
		// legitimately carry the component as a placeholder while its asset is
		// being authored, and a designer can add the component first and the
		// path second.
		if (component.AssetPath.empty())
			return;

		if (s_Failed.find(id) != s_Failed.end())
		{
			// Latched, so a broken asset is reported once instead of every frame.
			// component.LoadError already names the problem.
			return;
		}

		Runtime *runtime = nullptr;
		auto found = s_Runtimes.find(id);
		if (found != s_Runtimes.end())
		{
			runtime = &found->second;
		}
		else
		{
			Animation::AnimationAsset asset;
			Animation::AnimationError error;
			if (!Animation::AnimationSerializer::Load(component.AssetPath, asset, error))
			{
				component.LoadError = error.Format(component.AssetPath);
				s_Failed[id] = true;
				return;
			}

			// A skeleton is what makes a clip meaningful: sampled without one a
			// pose has nowhere to be written. Reported rather than tolerated
			// because an asset with clips and no rig is malformed, not empty.
			if (!asset.Skeleton)
			{
				component.LoadError =
					component.AssetPath + ": the asset carries no skeleton";
				s_Failed[id] = true;
				return;
			}

			Runtime fresh;
			fresh.Skeleton = asset.Skeleton;

			if (asset.HasStateMachine)
			{
				fresh.HasMachine = true;
				fresh.Machine = asset.StateMachine;
				// The machine owns its Animator, and that Animator needs the
				// skeleton before it can evaluate anything.
				fresh.Machine.GetAnimator().SetSkeleton(asset.Skeleton);

				if (!component.DefaultState.empty())
				{
					if (fresh.Machine.HasState(component.DefaultState))
					{
						fresh.Machine.SetState(component.DefaultState);
					}
					else
					{
						// An unknown state name is reported, not ignored: a typo
						// would otherwise leave the character standing still with
						// no error anywhere. This is a WARNING rather than a
						// failure -- the asset loaded, and the character still
						// has a valid rest pose.
						component.LoadError = component.AssetPath + ": state '" +
											  component.DefaultState +
											  "' is not in the asset's state machine";
					}
				}
				else
				{
					// No DefaultState requested: enter the first state by name
					// so a machine with exactly one state (the common case) is
					// playable immediately after attaching.
					auto names = SortedStateNames(fresh.Machine);
					if (!names.empty())
						fresh.Machine.SetState(names.front());
				}
			}
			else
			{
				fresh.Animator.SetSkeleton(asset.Skeleton);
				// No machine means DIRECT CONTROL: no clip is chosen here, and
				// the caller adds one through GetAnimator(). Choosing a clip
				// automatically would make the component's behaviour depend on
				// the order clips happen to be stored in the file.
			}

			auto inserted = s_Runtimes.emplace(id, std::move(fresh));
			runtime = &inserted.first->second;
			component.Attached = true;
			// LoadError is left exactly as the load left it: empty on success, or
			// a state-name warning. It is NOT cleared here -- a warning about a
			// typo'd DefaultState is still true after the load succeeds, and
			// clearing it would hide the one diagnostic the component carries.
		}

		if (runtime->HasMachine)
		{
			// Update advances the machine's own Animator and leaves the blended
			// pose in it, which is what GetAnimator() exposes.
			runtime->Machine.Update(seconds);
		}
		else if (runtime->InstanceId >= 0)
		{
			// Only advance when a clip is actually playing. An Animator with no
			// instances still produces the rest pose, which is a wasted blend
			// every frame for every entity that has not picked a clip yet.
			Animation::Pose pose;
			runtime->Animator.Update(seconds, pose);
		}

		if (runtime->HasMesh && runtime->Skeleton)
		{
			Animation::Animator &live = runtime->HasMachine ? runtime->Machine.GetAnimator()
															: runtime->Animator;
			DeformRuntime(*runtime->Skeleton, live, runtime->SkinMatrices, runtime->Mesh,
						  runtime->Deformed, runtime->DeformedValid);
		}
	}

	bool AnimationSystem::SetSkinnedMesh(UUID id, const Animation::SkinnedMesh &mesh)
	{
		auto found = s_Runtimes.find(id);
		if (found == s_Runtimes.end() || !found->second.Skeleton)
			return false;
		Runtime &runtime = found->second;

		// Bone palette cap from the plan: a rig over 64 bones is rejected here
		// with a refusal, not silently truncated into a mesh that tears.
		if (runtime.Skeleton->GetBoneCount() > 64)
			return false;

		std::string error;
		mesh.Validate(error);
		if (!error.empty())
			return false;

		runtime.Mesh = mesh;
		runtime.HasMesh = true;
		runtime.DeformedValid = false;
		return true;
	}

	void AnimationSystem::NotifyAnimationState(UUID id, const std::string &stateName)
	{
		// Fires only when a flow graph or script has an OnAnimationEvent handler;
		// CallIfPresent makes a missing hook a quiet no-op, so the common case
		// (nobody listening) costs one map lookup per system.
		//
		// The state NAME is the argument, because the useful thing a script does
		// with this is branch on which state was entered -- a bare "something
		// changed" would need a follow-up query.
		ScriptEventDispatcher::DispatchNamedById(
			id, "OnAnimationEvent", {py::str(stateName)});
	}

	bool AnimationSystem::PlayState(UUID id, const std::string &stateName, bool loop)
	{
		auto found = s_Runtimes.find(id);
		// No runtime means the entity has not ticked yet, or its asset failed to
		// load. Both are caller-recoverable: tick once, or fix the asset path.
		if (found == s_Runtimes.end() || !found->second.HasMachine)
			return false;

		Runtime &runtime = found->second;

		// ALREADY IN THIS STATE: no-op rather than a re-cut.
		//
		// SetState unconditionally removes the live instance and adds a new one,
		// so a caller that fires PlayAnimation every tick -- the natural shape for
		// a tree node -- restarted the clip every frame and the pose never left
		// its first keyframe. Found by a test that read the arm angle and saw
		// exactly 1.5 degrees after 60 ticks.
		//
		// The loop flag is still applied: it is a per-instance setting, and a
		// caller that turns looping on mid-play must be able to reach an already
		// playing instance. What is skipped is only the restart.
		//
		// A deliberate restart stays possible through TransitionTo (which is a
		// different authoring act) or by leaving the state and coming back.
		if (runtime.Machine.GetCurrentState() == stateName)
		{
			const int current = runtime.Machine.GetCurrentInstance();
			if (current >= 0)
				runtime.Machine.GetAnimator().SetLooping(current, loop);
			return true;
		}

		// SetState is the hard cut and returns false for an unknown name, leaving
		// the machine untouched -- a typo in a script must not stop the character.
		if (!runtime.Machine.SetState(stateName))
			return false;

		// SetState creates the instance, so the id is only valid AFTER it returns.
		// Looping is per instance, which is why this needs the machine's handle
		// rather than a flag on the runtime.
		const int instance = runtime.Machine.GetCurrentInstance();
		if (instance >= 0)
			runtime.Machine.GetAnimator().SetLooping(instance, loop);

		NotifyAnimationState(id, stateName);
		return true;
	}

	bool AnimationSystem::TransitionState(UUID id, const std::string &stateName,
										float duration)
	{
		auto found = s_Runtimes.find(id);
		if (found == s_Runtimes.end() || !found->second.HasMachine)
			return false;

		Runtime &runtime = found->second;
		// Same idempotency guard as PlayState, for the same reason: TransitionTo
		// also rebuilds the instance, so a tree node that fires every tick would
		// restart the fade and the clip on every frame.
		//
		// The state's own duration is the only per-state setting a caller cannot
		// reach any other way, and re-entering would just replay the fade.
		if (runtime.Machine.GetCurrentState() == stateName)
			return true;

		// A negative duration means "the state's own transition duration"; a caller
		// passing 0 gets the same, because a zero-second fade is indistinguishable
		// from a cut and the state's authored duration is the more useful answer.
		const float effective = duration > 0.0f ? duration : -1.0f;
		if (!runtime.Machine.TransitionTo(stateName, effective))
			return false;

		NotifyAnimationState(id, stateName);
		return true;
	}

	bool AnimationSystem::FireTrigger(UUID id, const std::string &trigger)
	{
		auto found = s_Runtimes.find(id);
		if (found == s_Runtimes.end() || !found->second.HasMachine)
			return false;
		if (trigger.empty())
		{
			// SetTrigger itself ignores an empty name, so returning true here would
			// report success for a call that did nothing. Same reasoning as
			// PlayState's unknown-name refusal: a node that silently no-ops is worse
			// than one that reports failure.
			return false;
		}
		// SetTrigger returns void -- it ARMS the trigger and the transition is
		// evaluated on the machine's next Update. So success here means "armed on
		// a live machine", not "the transition has already happened".
		found->second.Machine.SetTrigger(trigger);
		return true;
	}

	const ProceduralMesh *AnimationSystem::GetDeformedMesh(UUID id)
	{
		auto found = s_Runtimes.find(id);
		if (found == s_Runtimes.end() || !found->second.HasMesh || !found->second.DeformedValid)
			return nullptr;
		return &found->second.Deformed;
	}

	const std::vector<glm::vec3> *AnimationSystem::GetDeformedPositions(UUID id)
	{
		const ProceduralMesh *mesh = GetDeformedMesh(id);
		return mesh ? &mesh->Positions : nullptr;
	}

	Animation::Animator *AnimationSystem::GetAnimator(UUID id)
	{
		auto found = s_Runtimes.find(id);
		if (found == s_Runtimes.end())
			return nullptr;

		Runtime &runtime = found->second;
		// The machine's own Animator when there is one -- it is the one being
		// ticked, so returning the bare one would report a pose that never
		// changes.
		return runtime.HasMachine ? &runtime.Machine.GetAnimator() : &runtime.Animator;
	}

	void AnimationSystem::OnDestroyEntity(UUID id)
	{
		// A respawn reusing a UUID must not inherit the previous entity's
		// Animator. The failure latch is cleared too: the new entity may be
		// given a different asset, and a stale latch would keep it inert.
		s_Runtimes.erase(id);
		s_Failed.erase(id);
	}

	void AnimationSystem::Shutdown()
	{
		s_Runtimes.clear();
		s_Failed.clear();
	}

} // namespace Wasteland
