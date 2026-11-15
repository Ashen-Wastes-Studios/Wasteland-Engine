// .wlanim -- the animation asset format.
//
// See AnimationSerializer.h for the two rules this file implements: stable
// string keys everywhere, and report-never-throw on load.

#include "wlpch.h"
#include "Wasteland/Animation/AnimationSerializer.h"
// AnimationRetarget::Mapping, the type the `retarget:` section holds.
#include "Wasteland/Animation/AnimationRetarget.h"

#include <yaml-cpp/yaml.h>

// std::sort for the state table, whose backing container is an unordered_map:
// the emitted order must be deterministic for the file to be diffable.
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace Wasteland::Animation
{
	namespace
	{
		// The top-level keys this format recognises. Anything else is an error
		// rather than a drop, so a typo in a hand-authored or imported file is
		// reported instead of silently deleting the section it names.
		bool IsKnownRootKey(const std::string &key)
		{
			return key == "schema" || key == "name" || key == "skeleton" ||
				   key == "clips" || key == "state_machine" || key == "editor" ||
			   key == "retarget";
		}

		const char *InterpolationName(Interpolation mode)
		{
			switch (mode)
			{
				case Interpolation::Step:   return "Step";
				case Interpolation::Linear: return "Linear";
				case Interpolation::Cubic:  return "Cubic";
			}
			// Unreachable for a three-value enum, but a default is required: a
			// switch with no matching case falls through to indeterminate
			// memory otherwise, and the return type is a pointer.
			return "Cubic";
		}

		bool InterpolationFromName(const std::string &name, Interpolation &out)
		{
			if (name == "Step")        { out = Interpolation::Step;   return true; }
			if (name == "Linear")      { out = Interpolation::Linear; return true; }
			if (name == "Cubic")       { out = Interpolation::Cubic;  return true; }
			return false;
		}

		// A short numeric sequence in FLOW style: `[1, 0, 0]`.
		//
		// Flow rather than block because a vec3 or quat as four block lines is
		// four times the file for a value a reader can hold in one glance. The
		// Block manipulator after EndSeq is what restores block style for the
		// nodes that follow -- without it every later key in the document would
		// be emitted inline.
		void EmitFloatSeq(YAML::Emitter &out, const float *values, size_t count)
		{
			out << YAML::Flow << YAML::BeginSeq;
			for (size_t i = 0; i < count; ++i)
				out << values[i];
			out << YAML::EndSeq;
			out << YAML::Block;
		}

		// A quaternion in FLOW style: `[w, x, y, z]`.
		//
		// Written component by component rather than through a pointer, because
		// glm::quat stores its members as {x, y, z, w} -- w is the LAST member.
		// Taking &q.w and reading four floats therefore reads w followed by three
		// floats of adjacent memory, which emits valid-looking but meaningless
		// numbers. Nothing in the key-count assertions catches that, so this is
		// the kind of defect a value assertion exists for.
		void EmitQuat(YAML::Emitter &out, const glm::quat &q)
		{
			out << YAML::Flow << YAML::BeginSeq
				<< q.w << q.x << q.y << q.z
				<< YAML::EndSeq;
			out << YAML::Block;
		}

		// The read-side twin of EmitQuat. Returns false when the node is not a
		// sequence of exactly four floats.
		bool ReadQuat(const YAML::Node &node, glm::quat &out)
		{
			if (!node || !node.IsSequence() || node.size() != 4)
				return false;
			float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
			for (size_t i = 0; i < 4; ++i)
			{
				try
				{
					q[i] = node[i].as<float>();
				}
				catch (const std::exception &)
				{
					return false;
				}
			}
			// glm::quat's constructor takes (w, x, y, z), matching the file.
			out = glm::quat(q[0], q[1], q[2], q[3]);
			return true;
		}
		bool ReadFloatSeq(const YAML::Node &node, size_t count, float *out)
		{
			if (!node || !node.IsSequence() || node.size() != count)
				return false;
			for (size_t i = 0; i < count; ++i)
			{
				try
				{
					out[i] = node[i].as<float>();
				}
				catch (const std::exception &)
				{
					// A non-numeric entry. Caught here rather than at the document
					// level so the diagnostic can name the key.
					return false;
				}
			}
			return true;
		}

		// One scalar curve (a position or scale component) as a key list.
		void EmitScalarKeys(YAML::Emitter &out, const AnimationCurve &curve)
		{
			out << YAML::BeginSeq;
			for (const Keyframe &key : curve.GetKeys())
			{
				out << YAML::BeginMap;
				out << YAML::Key << "t" << YAML::Value << key.Time;
				out << YAML::Key << "v" << YAML::Value << key.Value;
				// Tangents are only meaningful for Cubic, but they are written
				// for every mode so switching a key from Step back to Cubic does
				// not lose authored slopes -- the same rule Keyframe documents.
				out << YAML::Key << "in" << YAML::Value << key.InTangent;
				out << YAML::Key << "out" << YAML::Value << key.OutTangent;
				out << YAML::Key << "mode" << YAML::Value << InterpolationName(key.Mode);
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
	}

		// The rotation channel. Its own emitter because a quaternion is four
		// floats rather than one, and it has no tangents -- a slerp has no
		// slope to author.
		void EmitRotationKeys(YAML::Emitter &out, const RotationTrack &track)
		{
			out << YAML::BeginSeq;
			for (const RotationTrack::Key &key : track.GetKeys())
			{
				out << YAML::BeginMap;
				out << YAML::Key << "t" << YAML::Value << key.Time;
				const glm::quat &q = key.Value;
				out << YAML::Key << "v" << YAML::Value;
				EmitQuat(out, q);
				out << YAML::Key << "mode" << YAML::Value << InterpolationName(key.Mode);
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		// Reads one scalar curve's key list, appending into `curve`.
		bool ReadScalarKeys(const YAML::Node &node, AnimationCurve &curve,
							const std::string &context, AnimationError &error)
		{
			if (!node)
				return true;   // an absent channel is a channel with no keys
			if (!node.IsSequence())
			{
				error.Message = "must be a sequence of keys";
				error.Context = context;
				return false;
			}
			for (const YAML::Node &item : node)
			{
				if (!item.IsMap())
				{
					error.Message = "a key must be a mapping with t and v";
					error.Context = context;
					return false;
				}
				Keyframe key;
				try
				{
					key.Time = item["t"].as<float>();
					key.Value = item["v"].as<float>();
				}
				catch (const std::exception &)
				{
					error.Message = "a key needs numeric t and v";
					error.Context = context;
					return false;
				}
				// Tangents default to 0, which is the struct's own default, so
				// an absent pair is a flat key rather than an error.
				if (item["in"])
					key.InTangent = item["in"].as<float>();
				if (item["out"])
					key.OutTangent = item["out"].as<float>();
				if (item["mode"])
				{
					Interpolation mode = Interpolation::Cubic;
					if (!InterpolationFromName(item["mode"].as<std::string>(), mode))
					{
						error.Message = "unknown interpolation mode '" +
										item["mode"].as<std::string>() + "'";
						error.Context = context;
						return false;
					}
					key.Mode = mode;
				}
				curve.AddKey(key.Time, key.Value, key.Mode);
			}
			return true;
		}

		bool ReadRotationKeys(const YAML::Node &node, RotationTrack &track,
							  const std::string &context, AnimationError &error)
		{
			if (!node)
				return true;
			if (!node.IsSequence())
			{
				error.Message = "must be a sequence of keys";
				error.Context = context;
				return false;
			}
			for (const YAML::Node &item : node)
			{
				if (!item.IsMap())
				{
					error.Message = "a key must be a mapping with t and v";
					error.Context = context;
					return false;
				}
				RotationTrack::Key key;
				try
				{
					key.Time = item["t"].as<float>();
				}
				catch (const std::exception &)
				{
					error.Message = "a key needs a numeric t";
					error.Context = context;
					return false;
				}
				glm::quat quat;
				if (!ReadQuat(item["v"], quat))
				{
					error.Message = "a rotation key needs v as [w, x, y, z]";
					error.Context = context;
					return false;
				}
				key.Value = quat;
				if (item["mode"])
				{
					Interpolation mode = Interpolation::Cubic;
					if (!InterpolationFromName(item["mode"].as<std::string>(), mode))
					{
						error.Message = "unknown interpolation mode '" +
										item["mode"].as<std::string>() + "'";
						error.Context = context;
						return false;
					}
					key.Mode = mode;
				}
				track.AddKey(key.Time, key.Value, key.Mode);
			}
			return true;
		}
	} // namespace

	std::string AnimationError::Format(const std::string &fileName) const
	{
		// The context is inserted only when present, so a document-level error
		// reads as "file: message" rather than "file: : message".
		if (Context.empty())
			return fileName + ": " + Message;
		return fileName + ": " + Context + ": " + Message;
	}

	// ---------------------------------------------------------------- emit

	std::string AnimationSerializer::Emit(const AnimationAsset &asset)
	{
		YAML::Emitter out;

		out << YAML::BeginMap;
		out << YAML::Key << "schema" << YAML::Value << 1;
		out << YAML::Key << "name" << YAML::Value << asset.Name;

		// --- skeleton ---
		if (asset.Skeleton && !asset.Skeleton->IsEmpty())
		{
			out << YAML::Key << "skeleton" << YAML::Value << YAML::BeginMap;

			out << YAML::Key << "bones" << YAML::Value << YAML::BeginSeq;
			for (int i = 0; i < asset.Skeleton->GetBoneCount(); ++i)
			{
				const BoneIndex index((int32_t)i);
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value
					<< asset.Skeleton->GetBoneName(index);
				// The parent as a NAME, empty for a root. A name rather than an
				// index because the whole point of the format is that a file
				// survives its rig being re-exported in a different order.
				const BoneIndex parent = asset.Skeleton->GetParent(index);
				out << YAML::Key << "parent" << YAML::Value
					<< (parent.IsValid() ? asset.Skeleton->GetBoneName(parent)
										: std::string());
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			// The bind pose. Only non-identity bones are written -- a rig whose
			// rest pose is all identity is the common case, and writing 150
			// identity entries would bury the ones that matter.
			const Pose &rest = asset.Skeleton->GetRestPose();
			out << YAML::Key << "rest_pose" << YAML::Value << YAML::BeginSeq;
			for (size_t i = 0; i < rest.GetBoneCount(); ++i)
			{
				const BoneTransform &bone = rest.Bones[i];
				const bool identity =
					bone.Translation == glm::vec3(0.0f, 0.0f, 0.0f) &&
					bone.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f) &&
					bone.Scale == glm::vec3(1.0f, 1.0f, 1.0f);
				if (identity)
					continue;

				out << YAML::BeginMap;
				out << YAML::Key << "bone" << YAML::Value
					<< asset.Skeleton->GetBoneName(BoneIndex((int32_t)i));
				out << YAML::Key << "t" << YAML::Value;
				EmitFloatSeq(out, &bone.Translation.x, 3);
				out << YAML::Key << "q" << YAML::Value;
				EmitQuat(out, bone.Rotation);
				out << YAML::Key << "s" << YAML::Value;
				EmitFloatSeq(out, &bone.Scale.x, 3);
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::EndMap;
		}

		// --- clips ---
		// Emitted even when empty, so a file always says what it has.
		out << YAML::Key << "clips" << YAML::Value << YAML::BeginSeq;
		for (const auto &clipPtr : asset.Clips)
		{
			// A null entry is skipped rather than emitted as a null clip: a
			// vector with a hole is a bug in the caller, and writing it would
			// produce a file that loads with a clip that has no name.
			if (!clipPtr)
				continue;

			const AnimationClip &clip = *clipPtr;
			out << YAML::BeginMap;
			out << YAML::Key << "name" << YAML::Value << clip.GetName();
			out << YAML::Key << "looping" << YAML::Value << clip.IsLooping();
			out << YAML::Key << "play_rate" << YAML::Value << clip.GetPlayRate();

			out << YAML::Key << "tracks" << YAML::Value << YAML::BeginSeq;
			for (BoneIndex bone : clip.GetAnimatedBones())
			{
				const BoneTrack *track = clip.GetTrack(bone);
				if (track == nullptr || track->IsEmpty())
					continue;

				out << YAML::BeginMap;
				out << YAML::Key << "bone" << YAML::Value
					<< asset.Skeleton->GetBoneName(bone);

				// Each of the seven channels is emitted only when it has keys.
				// A walk cycle animates position and rotation but rarely scale,
				// and omitting the empty channels keeps the file to what was
				// actually authored.
				for (int axis = 0; axis < 3; ++axis)
				{
					static const char *kAxes[3] = {"x", "y", "z"};
					if (!track->Position[axis].IsEmpty())
					{
						out << YAML::Key << ("position_" + std::string(kAxes[axis]))
							<< YAML::Value;
						EmitScalarKeys(out, track->Position[axis]);
					}
				}
				if (!track->Rotation.IsEmpty())
				{
					out << YAML::Key << "rotation" << YAML::Value;
					EmitRotationKeys(out, track->Rotation);
				}
				for (int axis = 0; axis < 3; ++axis)
				{
					static const char *kAxes[3] = {"x", "y", "z"};
					if (!track->Scale[axis].IsEmpty())
					{
						out << YAML::Key << ("scale_" + std::string(kAxes[axis]))
							<< YAML::Value;
						EmitScalarKeys(out, track->Scale[axis]);
					}
				}
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		// --- state machine ---
		if (asset.HasStateMachine)
		{
			const AnimationStateMachine &machine = asset.StateMachine;
			out << YAML::Key << "state_machine" << YAML::Value << YAML::BeginMap;

			out << YAML::Key << "states" << YAML::Value << YAML::BeginSeq;
			// The machine stores states in an unordered_map, so the emitted
			// order is not the authoring order. Sorted by name instead: the
			// file is then deterministic, which is what makes it diffable.
			std::vector<std::string> stateNames;
			for (const auto &entry : machine.GetStates())
				stateNames.push_back(entry.first);
			std::sort(stateNames.begin(), stateNames.end());
			for (const std::string &name : stateNames)
			{
				const AnimationState *state = machine.FindState(name);
				if (state == nullptr)
					continue;
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << state->Name;
				// The clip is referenced by NAME, and omitted when the state has
				// no clip -- a state with no clip is a legitimate pause.
				out << YAML::Key << "clip" << YAML::Value
					<< (state->Clip ? state->Clip->GetName() : std::string());
				out << YAML::Key << "speed" << YAML::Value << state->Speed;
				out << YAML::Key << "looping" << YAML::Value << state->Looping;
				out << YAML::Key << "transition_duration" << YAML::Value
					<< state->TransitionDuration;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::Key << "transitions" << YAML::Value << YAML::BeginSeq;
			for (const AnimationTransition &transition : machine.GetTransitions())
			{
				out << YAML::BeginMap;
				out << YAML::Key << "from" << YAML::Value << transition.FromState;
				out << YAML::Key << "to" << YAML::Value << transition.ToState;
				out << YAML::Key << "trigger" << YAML::Value << transition.Trigger;
				out << YAML::Key << "duration" << YAML::Value << transition.Duration;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::EndMap;
		}

		// Authoring-only node positions. Emitted only when there is something to
		// say, so a file that has never been opened in the Animator is byte-for-byte
		// what it was before this section existed -- and a round trip through the
		// serializer does not invent layout for a machine nobody has arranged.
		if (!asset.StatePositions.empty())
		{
			out << YAML::Key << "editor" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "states" << YAML::Value << YAML::BeginSeq;
			// Sorted by name rather than left in hash order: the map's iteration
			// order changes between runs, so an unsorted emission would rewrite
			// the file on every save even when nothing moved.
			std::vector<std::pair<std::string, AnimationAsset::StatePosition>> sorted(
				asset.StatePositions.begin(), asset.StatePositions.end());
			std::sort(sorted.begin(), sorted.end(),
					  [](const auto &a, const auto &b) { return a.first < b.first; });
			for (const auto &entry : sorted)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << entry.first;
				out << YAML::Key << "x" << YAML::Value << entry.second.X;
				out << YAML::Key << "y" << YAML::Value << entry.second.Y;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}

		// Retarget mapping. Emitted only when the asset carries one, for the same
		// reason as `editor`: a file authored for its own rig must not change shape
		// the first time a retarget is applied to something else.
		if (!asset.Retarget.empty())
		{
			out << YAML::Key << "retarget" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "position_scale" << YAML::Value
				<< asset.RetargetPositionScale;
			out << YAML::Key << "bones" << YAML::Value << YAML::BeginSeq;
			// Sorted by source name, not left in the mapping vector's order: the
			// vector is a lookup table whose order carries no meaning, so an
			// emission that followed it would reorder the file on every save.
			std::vector<AnimationRetarget::Mapping> sorted(asset.Retarget.begin(),
														 asset.Retarget.end());
			std::sort(sorted.begin(), sorted.end(),
					  [](const AnimationRetarget::Mapping &a,
						 const AnimationRetarget::Mapping &b) {
					  return a.SourceBone < b.SourceBone;
				  });
			for (const AnimationRetarget::Mapping &mapping : sorted)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "from" << YAML::Value << mapping.SourceBone;
				out << YAML::Key << "to" << YAML::Value << mapping.TargetBone;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;
		}

		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	// ---------------------------------------------------------------- parse

	bool AnimationSerializer::Parse(const std::string &text, AnimationAsset &outAsset,
									AnimationError &error)
	{
		YAML::Node root;
		try
		{
			root = YAML::Load(text);
		}
		catch (const std::exception &e)
		{
			error.Message = std::string("malformed YAML: ") + e.what();
			return false;
		}

		if (!root.IsMap())
		{
			error.Message = "top level must be a mapping";
			return false;
		}

		// A full key check before any field is read, so a typo is reported as an
		// unknown key rather than silently ignored.
		for (const auto &entry : root)
		{
			std::string key = entry.first.as<std::string>();
			if (!IsKnownRootKey(key))
			{
				error.Message = "unknown top-level key '" + key + "'";
				return false;
			}
		}

		// Built into locals and assigned only at the end, so a failure leaves
		// outAsset untouched -- the all-or-nothing contract.
		AnimationAsset scratch;
		scratch.Name = root["name"] ? root["name"].as<std::string>() : std::string();

		// --- skeleton ---
		if (root["skeleton"])
		{
			const YAML::Node skel = root["skeleton"];
			if (!skel.IsMap())
			{
				error.Message = "skeleton must be a mapping";
				return false;
			}

			auto skeleton = std::make_shared<Skeleton>();

			// Bones are built from names, in file order, which is the
			// construction order Skeleton::BuildFromNames requires (a parent
			// must precede its children).
			if (skel["bones"])
			{
				if (!skel["bones"].IsSequence())
				{
					error.Message = "skeleton.bones must be a sequence";
					return false;
				}
				std::vector<std::string> names;
				std::vector<std::string> parents;
				for (const YAML::Node &item : skel["bones"])
				{
					if (!item.IsMap())
					{
						error.Message = "a bone must be a mapping with name and parent";
						return false;
					}
					names.push_back(item["name"].as<std::string>());
					// An absent parent is a root, which is the empty string.
					parents.push_back(item["parent"]
										  ? item["parent"].as<std::string>()
										  : std::string());
				}
				std::string buildError;
				if (!skeleton->BuildFromNames(names, parents, buildError))
				{
					error.Message = buildError;
					return false;
				}
			}

			// The rest pose. Bones not listed keep the identity, which is
			// Pose::Resize's own fill.
			if (skel["rest_pose"])
			{
				if (!skel["rest_pose"].IsSequence())
				{
					error.Message = "skeleton.rest_pose must be a sequence";
					return false;
				}
				Pose rest;
				rest.Resize((size_t)std::max(0, skeleton->GetBoneCount()));
				for (const YAML::Node &item : skel["rest_pose"])
				{
					if (!item.IsMap())
					{
						error.Message = "a rest pose entry must be a mapping";
						return false;
					}
					const std::string boneName = item["bone"].as<std::string>();
					const BoneIndex bone = skeleton->FindBone(boneName);
					if (!bone.IsValid())
					{
						error.Message = "rest pose names unknown bone '" + boneName + "'";
						return false;
					}
					float t[3] = {0.0f, 0.0f, 0.0f};
					float s[3] = {1.0f, 1.0f, 1.0f};
					glm::quat quat;
					if (!ReadFloatSeq(item["t"], 3, t) ||
						!ReadQuat(item["q"], quat) ||
						!ReadFloatSeq(item["s"], 3, s))
					{
						error.Message = "a rest pose entry needs t [x,y,z], "
										"q [w,x,y,z] and s [x,y,z]";
						error.Context = boneName;
						return false;
					}
					rest.Bones[(size_t)bone.Value] =
						BoneTransform(glm::vec3(t[0], t[1], t[2]),
									  quat,
									  glm::vec3(s[0], s[1], s[2]));
				}
				skeleton->SetRestPose(rest);
			}

			// A loaded rig must be ready to skin. Without this the inverse bind
			// matrices stay identity, so ComputeSkinningMatrices returns the plain
			// world pose and a mesh bound at rest is displaced by the whole bone
			// offset instead of by the motion since bind. Captured AFTER the rest
			// pose is set because it reads the rest pose.
			//
			// The bool is checked because an empty rig now reports failure instead
			// of capturing "successfully" into an empty array -- and a rig with no
			// bones cannot skin anything anyway.
			if (!skeleton->CaptureBindMatrices())
			{
				error.Message = "skeleton has no bones to bind";
				return false;
			}

			scratch.Skeleton = skeleton;
		}

		// --- clips ---
		if (root["clips"])
		{
			if (!root["clips"].IsSequence())
			{
				error.Message = "clips must be a sequence";
				return false;
			}
			for (const YAML::Node &item : root["clips"])
			{
				if (!item.IsMap())
				{
					error.Message = "a clip must be a mapping";
					return false;
				}
				const std::string clipName = item["name"].as<std::string>();
				auto clip = std::make_shared<AnimationClip>(clipName);
				clip->SetLooping(item["looping"]
									 ? item["looping"].as<bool>()
									 : true);
				clip->SetPlayRate(item["play_rate"]
									  ? item["play_rate"].as<float>()
									  : 1.0f);

				if (item["tracks"])
				{
					if (!item["tracks"].IsSequence())
					{
						error.Message = "tracks must be a sequence";
						error.Context = clipName;
						return false;
					}
					for (const YAML::Node &trackNode : item["tracks"])
					{
						if (!trackNode.IsMap())
						{
							error.Message = "a track must be a mapping";
							error.Context = clipName;
							return false;
						}
						const std::string boneName =
							trackNode["bone"].as<std::string>();
						// A bone name resolved against the SKELETON. An absent
						// skeleton means the track cannot be placed, which is
						// reported rather than silently dropped.
						if (!scratch.Skeleton)
						{
							error.Message = "a clip names bone '" + boneName +
											"' but the file has no skeleton";
							error.Context = clipName;
							return false;
						}
						const BoneIndex bone = scratch.Skeleton->FindBone(boneName);
						if (!bone.IsValid())
						{
							error.Message = "unknown bone '" + boneName + "'";
							error.Context = clipName;
							return false;
						}

						// The seven channels, each optional. Read into the track
						// through the clip's own authoring calls, so an
						// out-of-range component is dropped by the same rule the
						// editor uses rather than by a second implementation.
						static const char *kAxes[3] = {"x", "y", "z"};
						for (int axis = 0; axis < 3; ++axis)
						{
							const std::string key =
								std::string("position_") + kAxes[axis];
							if (trackNode[key])
							{
								// A scratch curve, so a bad key list leaves the
								// clip untouched rather than half-applied.
								AnimationCurve curve;
								if (!ReadScalarKeys(trackNode[key], curve, clipName,
													error))
									return false;
								for (const Keyframe &k : curve.GetKeys())
									clip->AddKey(bone, TrackChannel::Position, axis,
												 k.Time, k.Value, k.Mode);
							}
						}
						if (trackNode["rotation"])
						{
							RotationTrack track;
							if (!ReadRotationKeys(trackNode["rotation"], track,
												  clipName, error))
								return false;
							for (const RotationTrack::Key &k : track.GetKeys())
								clip->AddRotationKey(bone, k.Time, k.Value, k.Mode);
						}
						for (int axis = 0; axis < 3; ++axis)
						{
							const std::string key =
								std::string("scale_") + kAxes[axis];
							if (trackNode[key])
							{
								AnimationCurve curve;
								if (!ReadScalarKeys(trackNode[key], curve, clipName,
													error))
									return false;
								for (const Keyframe &k : curve.GetKeys())
									clip->AddKey(bone, TrackChannel::Scale, axis,
												 k.Time, k.Value, k.Mode);
							}
						}
					}
				}

				scratch.Clips.push_back(clip);
			}
		}

		// --- state machine ---
		if (root["state_machine"])
		{
			const YAML::Node machine = root["state_machine"];
			if (!machine.IsMap())
			{
				error.Message = "state_machine must be a mapping";
				return false;
			}
			auto stateMachine = std::make_shared<AnimationStateMachine>();

			if (machine["states"])
			{
				if (!machine["states"].IsSequence())
				{
					error.Message = "state_machine.states must be a sequence";
					return false;
				}
				for (const YAML::Node &item : machine["states"])
				{
					if (!item.IsMap())
					{
						error.Message = "a state must be a mapping";
						return false;
					}
					AnimationState state;
					state.Name = item["name"].as<std::string>();
					const std::string clipName = item["clip"]
													 ? item["clip"].as<std::string>()
													 : std::string();
					// The clip is bound by NAME from the clips parsed above.
					// An empty name leaves the state clipless, which is valid.
					if (!clipName.empty())
					{
						bool found = false;
						for (const auto &clip : scratch.Clips)
						{
							if (clip && clip->GetName() == clipName)
							{
								state.Clip = clip;
								found = true;
								break;
							}
						}
						if (!found)
						{
							error.Message = "state '" + state.Name +
											"' names unknown clip '" + clipName + "'";
							return false;
						}
					}
					state.Speed = item["speed"] ? item["speed"].as<float>() : 1.0f;
					state.Looping = item["looping"] ? item["looping"].as<bool>() : true;
					state.TransitionDuration = item["transition_duration"]
												   ? item["transition_duration"].as<float>()
												   : 0.15f;
					stateMachine->AddState(state);
				}
			}

			if (machine["transitions"])
			{
				if (!machine["transitions"].IsSequence())
				{
					error.Message = "state_machine.transitions must be a sequence";
					return false;
				}
				for (const YAML::Node &item : machine["transitions"])
				{
					if (!item.IsMap())
					{
						error.Message = "a transition must be a mapping";
						return false;
					}
					AnimationTransition transition;
					transition.FromState = item["from"].as<std::string>();
					transition.ToState = item["to"].as<std::string>();
					transition.Trigger = item["trigger"]
											 ? item["trigger"].as<std::string>()
											 : std::string();
					transition.Duration = item["duration"]
											  ? item["duration"].as<float>()
											  : -1.0f;
					stateMachine->AddTransition(transition);
				}
			}

			scratch.StateMachine = *stateMachine;
			scratch.HasStateMachine = true;
		}

		// --- editor layout ---
		//
		// Authoring-only. A malformed position is an ERROR rather than a drop,
		// matching every other section: silently ignoring it would mean an author
		// who moved a state and saved saw it snap back with no explanation.
		if (root["editor"])
		{
			const YAML::Node editor = root["editor"];
			if (!editor.IsMap())
			{
				error.Message = "editor must be a mapping";
				return false;
			}
			if (editor["states"])
			{
				if (!editor["states"].IsSequence())
				{
					error.Message = "editor.states must be a sequence";
					return false;
				}
				for (const YAML::Node &item : editor["states"])
				{
					if (!item.IsMap())
					{
						error.Message = "an editor state must be a mapping";
						return false;
					}
					AnimationAsset::StatePosition position;
					position.X = item["x"] ? item["x"].as<float>() : 0.0f;
					position.Y = item["y"] ? item["y"].as<float>() : 0.0f;
					scratch.StatePositions[item["name"].as<std::string>()] = position;
				}
			}
		}

		// --- retarget ---
		// Authoring metadata, not a runtime input: a clip is retargeted when an
		// author asks for it, and the result is stored in the asset's clip list.
		// Parsed here so a mapping survives a save/load round trip and so a file
		// written by an editor that supports retargeting is not degraded by one
		// that does not.
		if (root["retarget"])
		{
			const YAML::Node retarget = root["retarget"];
			if (!retarget.IsMap())
			{
				error.Message = "retarget must be a mapping";
				return false;
			}
			scratch.RetargetPositionScale =
				retarget["position_scale"] ? retarget["position_scale"].as<float>() : 1.0f;
			if (retarget["bones"])
			{
				if (!retarget["bones"].IsSequence())
				{
					error.Message = "retarget.bones must be a sequence";
					return false;
				}
				for (const YAML::Node &item : retarget["bones"])
				{
					if (!item.IsMap())
					{
						error.Message = "a retarget mapping must be a mapping";
						return false;
					}
					// A mapping with an empty source or target is a corrupt entry
					// rather than a no-op: dropping it silently would make a bad file
					// load as a valid one that retargets nothing.
					if (!item["from"] || !item["to"])
					{
						error.Message = "a retarget mapping needs both 'from' and 'to'";
						return false;
					}
					const std::string from = item["from"].as<std::string>();
					const std::string to = item["to"].as<std::string>();
					if (from.empty() || to.empty())
					{
						error.Message = "a retarget mapping has an empty bone name";
						return false;
					}
					scratch.Retarget.push_back({from, to});
				}
			}
		}

		outAsset = std::move(scratch);
		return true;
	}

	// ------------------------------------------------------------ load/save

	bool AnimationSerializer::Load(const std::string &path, AnimationAsset &outAsset,
								   AnimationError &error)
	{
		std::error_code ec;
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
		{
			error.Message = "could not open '" + path + "' for reading";
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(file)),
						 std::istreambuf_iterator<char>());
		if (file.bad())
		{
			error.Message = "failed while reading '" + path + "'";
			return false;
		}

		if (!Parse(text, outAsset, error))
		{
			// The parse error is prefixed with the file name here rather than
			// inside Parse, so Parse stays usable for text that never came from
			// a file (an importer, an agent) and its diagnostics are not
			// polluted with a path that does not apply.
			error.Message = path + ": " + error.Message;
			return false;
		}
		return true;
	}

	bool AnimationSerializer::Save(const std::string &path, const AnimationAsset &asset,
								   AnimationError &error)
	{
		// Binary and truncating, matching GraphSerializer::Save: binary keeps
		// the LF line endings the emitter produces from being rewritten to
		// CRLF on Windows, which is what makes the file byte-identical across
		// platforms.
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			error.Message = "could not open '" + path + "' for writing";
			return false;
		}
		const std::string text = Emit(asset);
		file.write(text.data(), (std::streamsize)text.size());
		// close() before good(), so a flush failure is caught -- the defect
		// GraphSerializer::Save documents and does not fix.
		file.close();
		if (!file.good())
		{
			error.Message = "failed while writing '" + path + "'";
			return false;
		}
		return true;
	}

} // namespace Wasteland::Animation
