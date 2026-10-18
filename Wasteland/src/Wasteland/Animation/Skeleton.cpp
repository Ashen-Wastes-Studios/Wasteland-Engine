#include "wlpch.h"
#include "Wasteland/Animation/Skeleton.h"

// std::stable_sort, used to put bones into topological order.
#include <algorithm>
// glm's inverse() lives behind this include; used to build the bind
// matrices. The file itself calls no std math functions.
#include <cmath>
// Two distinct uses below: an ordered set of SEEN NAMES in Validate, and
// an ordered set of visited indices for cycle detection. std::set rather
// than unordered_* because Validate reports the FIRST problem in bone
// order, which requires deterministic iteration only for the message --
// but both are O(n log n) on small n and the clarity is worth it.
#include <set>

namespace Wasteland {

	namespace Animation {

		// Appends a bone and returns its handle. The default parent is an
		// invalid BoneIndex, i.e. a root -- so the common "no parent" case needs
		// no argument at all.

		// The header's contract: a parent must ALREADY exist. That is what lets
		// ComputeWorldMatrices resolve the hierarchy in a single forward pass
		// with no sorting, which is the central performance property of the
		// whole class.
		BoneIndex Skeleton::AddBone(const std::string &name, BoneIndex parent)
		{
			// A parent that was never added would leave the bone unreachable
			// from the root, and ComputeWorldMatrices' single forward pass
			// assumes every bone's parent is at a lower index. Rejecting here
			// turns a silent "that bone never moves" into an invalid handle
			// the caller can see.
			// The ordering invariant, enforced at the only place a parent can be
			// attached. A parent that was never added would leave the bone
			// unreachable from the root AND break the forward pass, which assumes
			// every bone's parent sits at a lower index.

			// Rejecting here turns a silent "that bone never moves" into an invalid
			// handle the caller can see. The (size_t) cast is safe because the &&
			// short-circuits: IsValid() has already established Value >= 0.

			// NOTE: this rejects an out-of-range parent but does NOT check for a
			// CYCLE. A bone cannot be its own ancestor at insertion time (a bone
			// cannot be its own parent yet), so the forward pass cannot be
			// subverted here; Validate handles cycles for the BuildFromNames path.
			if (parent.IsValid() && (size_t)parent.Value >= m_Bones.size())
			{
				// Warned rather than thrown or silently ignored. This is authoring-time
				// data, not user input, so a log line plus an invalid handle is the right
				// severity: the caller can detect the failure, but nothing crashes if it
				// does not.
				// (the format arguments are on the continuation line below)
				WL_CORE_WARN("Skeleton: bone '{}' has out-of-range parent index {}",
							 name, parent.Value);
				// An INVALID handle -- Value defaults to -1 -- is the failure signal.
				// Chosen over a clamped index so the bad value cannot be mistaken for a
				// real bone. Note this means the caller MUST check the return; nothing
				// downstream can detect the failure for it.
				return BoneIndex();
			}

			// The handle for the bone about to be appended: the current size,
			// because push_back will place it exactly there.

			// int32_t because BoneIndex::Value is int32_t, so a skeleton beyond 2^31
			// bones is not representable. Not a practical limit.
			BoneIndex index((int32_t)m_Bones.size());

			// A fresh local, then moved in. Constructed empty so Name and Parent
			// take their defaults before being assigned below.
			Bone bone;
			// The display and lookup name. Stored by value; the caller keeps
			// ownership of its string.
			bone.Name = name;
			// The parent handle, stored verbatim. A root gets the invalid default
			// that AddBone's parameter already carries.
			bone.Parent = parent;
			// Copied rather than moved, because Children (the next step) needs to
			// read back out of the stored bone to append to the PARENT's list.
			// A move here would work too but would complicate that read.
			m_Bones.push_back(bone);

			// A new bone starts at IDENTITY, not at the origin: identity places it
			// at its parent's origin with no offset, which is the neutral choice.
			// (Identity() is the static factory declared in Skeleton.h)
			m_RestTransforms.push_back(BoneTransform::Identity());
			// Identity bind matrix -- "no deformation" -- so a skinning pass over a
			// skeleton that never called CaptureBindMatrices passes vertices through
			// unchanged instead of collapsing them.
			m_InverseBindMatrices.push_back(glm::mat4(1.0f));
			// DEFECT (documented, not fixed): Resize REBUILDS the whole vector,
			// filling every bone with identity -- not just the new one. So calling
			// AddBone after SetRestPose WIPES THE ENTIRE REST POSE, while
			// m_RestTransforms (the parallel array) correctly keeps its existing
			// entries because it is push_back'd. The two structures that are
			// supposed to mirror each other then disagree, and CaptureBindMatrices
			// reads m_RestPose -- so the bind matrices would be captured from an
			// all-identity pose. The fix would be to append to m_RestPose.Bones
			// rather than Resize it.
			m_RestPose.Resize(m_Bones.size());

			// Only non-roots get an entry in a parent's child list. The same guard
			// as the validation above, so the two cannot disagree about which bones
			// have parents.
			if (parent.IsValid())
				// Back-link registration. Needed for GetChild/GetChildCount and for any
				// traversal that walks downward; ComputeWorldMatrices itself only needs
				// Parent, so this list is not on the hot path.

				// DEFECT (documented, not fixed): Children is not remapped by the
				// topological reorder in BuildFromNames except through the copy there,
				// which does fix it -- but note AddBone called AFTER BuildFromNames
				// appends correctly, whereas a bone added before the reorder is only
				// correct because that reorder rewrites every child handle.
				m_Bones[(size_t)parent.Value].Children.push_back(index);

			// First registration wins, so a duplicate name resolves to the
			// original rather than silently re-pointing every existing
			// reference to the new bone.
			// count() then insert, rather than a single operator[] assignment.
			if (!m_BoneNameMap.count(name))
				// First registration wins for a duplicate name: an existing reference
				// resolves to the ORIGINAL bone rather than every reference silently
				// re-pointing to the new one. Validate rejects duplicates outright
				// anyway, so this only matters for bones added piecemeal and checked
				// later.
				m_BoneNameMap[name] = index;

			// The handle for the new bone. Valid unless the parent check failed.
			return index;
		}

		// Builds a whole hierarchy in one call from parallel name arrays. This
		// is the authoring path (a glTF or authored rig); AddBone is the
		// incremental path.
		bool Skeleton::BuildFromNames(const std::vector<std::string> &names,
									  const std::vector<std::string> &parentNames,
									  std::string &outError)
		{
			// Cleared first, so a caller that reuses one string across attempts
			// never displays a stale message from a previous call on success.
			outError.clear();

			// The two arrays are parallel, so a length mismatch makes every index
			// below meaningless. Checked before anything is created, so the
			// skeleton is untouched when this fails.
			if (names.size() != parentNames.size())
			{
				// Both counts are included, because "they must match" is only useful
				// when the author can see by how much.
				// (continued on the next two source lines)
				outError = "names (" + std::to_string(names.size()) +
						   ") and parentNames (" + std::to_string(parentNames.size()) +
						   ") must be the same length";
				return false;
			}

			// An empty skeleton has no root and no bones. Rejected here rather than
			// deferring to Validate at the end, which would produce the same message
			// after doing nothing -- this just fails sooner and more specifically.
			if (names.empty())
			{
				// The same message Validate would give. Duplicated rather than shared,
				// because Validate's copy is reached through a different call path.
				outError = "skeleton has no bones";
				return false;
			}

			// Pass 1: create every bone parentless, so ordering in the input
			// cannot violate the "parent exists first" rule.
			// The handles in AUTHORED order, parallel to names. Needed because pass
			// 2 assigns parents by name lookup, which can resolve to a bone created
			// by an EARLIER call to this function -- the map is not scoped to this
			// build.
			std::vector<BoneIndex> created;
			// Exact reservation; the final size is already known.
			created.reserve(names.size());
			for (const std::string &name : names)
			{
				// Created PARENTLESS. Adding with a parent would fail for any bone whose
				// parent is listed later, which the input format permits -- so ordering
				// is fixed afterwards instead of being a precondition.
				BoneIndex index = AddBone(name);
				// AddBone only fails on a bad parent, which cannot happen here since
				// none is passed -- so this is defensive rather than reachable.
				if (!index.IsValid())
				{
					// Includes the offending name, which is the only useful identifier the
					// author has for a parallel-array index.
					outError = "failed to add bone '" + name + "'";
					return false;
				}
				// Recorded so pass 2 and the reorder can refer to this bone by its
				// handle rather than re-deriving an index.
				// (inside the pass-1 loop)
				// Pass 2: wire up parents now that every bone exists.
				created.push_back(index);
			}

			// Pass 2: wire up parents now that every bone exists.
			// Every parent name is now guaranteed present in m_BoneNameMap, so the
			// lookup below cannot fail for ordering reasons -- only for a typo.
			for (size_t i = 0; i < names.size(); ++i)
			{
				// Bound by reference to avoid copying the string per bone.
				const std::string &parentName = parentNames[i];
				// An empty parent name means a root. The format's only way to express
				// "no parent", so it is a sentinel rather than a name.
				if (parentName.empty())
					// Skips the lookup entirely for roots, so an empty string is never
					// searched for in the name map.
					continue; // a root

				// DEFECT (documented, not fixed): looked up in the GLOBAL name map, not
				// in the `created` vector. A second call to BuildFromNames on the same
				// skeleton therefore resolves parent names against bones from the FIRST
				// call -- so the second hierarchy can attach itself to the first. There
				// is no clearing of m_Bones or m_BoneNameMap at the top of this
				// function, so BuildFromNames is effectively single-use per Skeleton
				// and nothing says so.
				auto it = m_BoneNameMap.find(parentName);
				// A parent name no bone declares. The only genuine authoring error this
				// function catches -- and it names both bones, because the fix is
				// usually to correct whichever of the two is misspelled.
				if (it == m_BoneNameMap.end())
				{
					// Both names in one message: the child that has the bad reference, and
					// the parent name that could not be found.
					// 						   parentName + "'";
					outError = "bone '" + names[i] + "' names unknown parent '" +
							   parentName + "'";
					return false;
				}

				// The resolved handle. Read from the map rather than from `created`,
				// which is the source of the stale-resolution defect noted above.
				BoneIndex parentIndex = it->second;
				// Sets the FORWARD link, the one ComputeWorldMatrices reads. Assigned
				// directly rather than through AddBone, since the bone already exists.
				m_Bones[(size_t)created[i].Value].Parent = parentIndex;
				// Back-link, matching what AddBone does. Both directions are needed:
				// Parent for the forward pass, Children for traversal and queries.
				m_Bones[(size_t)parentIndex.Value].Children.push_back(created[i]);
			}

			// A single forward pass needs parent index < child index. Wiring
			// parents after creation breaks that ordering when a parent is
			// listed after its child, so reorder into topological order.
			//
			// Done as a stable sort on depth: every bone is emitted after all
			// of its ancestors, and bones at the same depth keep their
			// authored order so an artist's hand-ordered list survives.
			// The permutation: order[newIndex] = oldIndex. Sized to the CURRENT
			// bone count, which includes any bones left by an earlier call -- see
			// the stale-resolution defect above.
			std::vector<int32_t> order(m_Bones.size());
			// Identity permutation as the starting point.
			for (size_t i = 0; i < m_Bones.size(); ++i)
				// A no-op fill, stated explicitly for clarity.
				order[i] = (int32_t)i;

			// Depth of each bone, meaning "number of ancestors". Depth 0 is a root.
			std::vector<int32_t> depth(m_Bones.size(), 0);
			for (size_t i = 0; i < m_Bones.size(); ++i)
			{
				// Accumulator for the walk below, reset per bone.
				int32_t d = 0;
				// Starts at this bone's parent and walks up, so a root's depth ends at 0
				// -- its own existence is not counted.
				BoneIndex walk = m_Bones[i].Parent;
				// Bounded by the bone count so a cycle cannot hang the sort.
				// Bounded by the bone count so a cycle cannot hang the sort. Without the
				// guard this is an infinite loop on a cyclic hierarchy; with it, the
				// worst case is a linear walk that yields a garbage depth, which
				// Validate then rejects.
				for (int32_t guard = 0; walk.IsValid() && guard <= (int32_t)m_Bones.size(); ++guard)
				{
					// One ancestor counted.
					++d;
					// Next step up. Safe because walk.IsValid() was just checked and
					// Validate -- which runs at the end of this function -- bounds every
					// parent index.
					walk = m_Bones[(size_t)walk.Value].Parent;
				}
				// The final ancestor count for this bone. For a bone listed after its
				// own parent, depth is exactly the position that makes the sort correct.
				depth[i] = d;
			}

			// STABLE sort on depth, so bones at equal depth keep authored order --
			// an artist's hand-ordered bone list survives the shuffle.
			// 						 [&](int32_t a, int32_t b) { return depth[(size_t)a] < depth[(size_t)b]; });
			// Sorting indices by their depth's value. Sorting the DEPTH ARRAY itself
			// would lose the mapping back to the bones, which is why `order` exists.
			std::stable_sort(order.begin(), order.end(),
							 [&](int32_t a, int32_t b) { return depth[(size_t)a] < depth[(size_t)b]; });

			// Build the remap, then rebuild the arrays in the new order.
			// Inverse permutation: remap[oldIndex] = newIndex. Both directions are
			// needed -- one to reorder, the other to rewrite the parent/child
			// handles that refer to each other by old index.
			std::vector<int32_t> remap(m_Bones.size());
			// Walk the permutation once, writing the inverse as it goes.
			for (size_t newIndex = 0; newIndex < order.size(); ++newIndex)
				// The inverse assignment. Every slot is written exactly once because
				// `order` is a permutation of 0..n-1.
				remap[(size_t)order[newIndex]] = (int32_t)newIndex;

			std::vector<Bone> reordered;
			std::vector<BoneTransform> reorderedRest;
			// Three parallel buffers, reserved up front. Each is a separate type, so
			// there is no way to reorder them with one operation -- hence three.
			// (each reserved individually to exactly m_Bones.size())
			std::vector<glm::mat4> reorderedBind;
			reordered.reserve(m_Bones.size());
			reorderedRest.reserve(m_Bones.size());
			reorderedBind.reserve(m_Bones.size());

			// Emit bones in topological order, rebuilding each handle on the way.
			for (size_t newIndex = 0; newIndex < order.size(); ++newIndex)
			{
				// Which bone lands here.
				int32_t oldIndex = order[newIndex];
				// COPIED, so the Parent/Children rewrite below cannot corrupt the
				// original -- which matters because the parent may not have been
				// emitted yet and its old handle is still needed for the remap lookup.
				Bone bone = m_Bones[(size_t)oldIndex];

				// Roots keep their invalid parent and need no remap.
				if (bone.Parent.IsValid())
					// The parent handle rewritten from old index to new. This is the step
					// that makes the single-forward-pass invariant true again.
					bone.Parent = BoneIndex(remap[(size_t)bone.Parent.Value]);

				// The child list, rewritten element by element.
				for (BoneIndex &child : bone.Children)
				{
					// Roots have an empty list, so this never fires for them; the guard is
					// for symmetry with the Parent rewrite above.
					if (child.IsValid())
						// Each child handle mapped through the same inverse permutation.
						child = BoneIndex(remap[(size_t)child.Value]);
				}

				// The rewritten bone, now safe to append.
				reordered.push_back(bone);
				// The rest transform follows its bone. DEFECT (documented, not fixed):
				// during BuildFromNames every rest transform is IDENTITY (AddBone
				// pushes identity), so this is currently a no-op -- but if rest
				// transforms were ever populated BEFORE this call, the third array below
				// would still disagree with m_RestPose, which is NOT rebuilt here.
				reorderedRest.push_back(m_RestTransforms[(size_t)oldIndex]);
				// Bind matrices follow too, for the same reason.
				reorderedBind.push_back(m_InverseBindMatrices[(size_t)oldIndex]);
			}

			// Swapped in wholesale. move() rather than copy: the vectors are
			// function-local and about to die, so copying would be pure waste.
			m_Bones = std::move(reordered);
			// The parallel rest array, swapped to match.
			m_RestTransforms = std::move(reorderedRest);
			// And the bind matrices. All three MUST be replaced together -- leaving
			// any one at the old ordering silently mispairs bones with their
			// transforms.
			m_InverseBindMatrices = std::move(reorderedBind);
			// DEFECT (documented, not fixed): this REBUILDS m_RestPose as all
			// identity rather than reordering it the way the three arrays above are
			// reordered. Correct today only because every rest transform is
			// identity at this point. If a rest pose were set before building the
			// names, m_RestPose and m_RestTransforms would disagree -- and
			// CaptureBindMatrices reads m_RestPose, so the bind matrices would be
			// wrong while GetRestTransform returned the right values.
			m_RestPose.Resize(m_Bones.size());

			// Rebuilt below, because the reordering changed which index each name
			// maps to. Clearing first is what makes the rebuild correct rather than
			// merging old and new mappings.
			m_BoneNameMap.clear();
			// Repopulate in the NEW index order.
			for (size_t i = 0; i < m_Bones.size(); ++i)
			{
				// Keep the first bone of each name so a duplicate still
				// resolves to the one that existed first.
				// Same first-wins rule as AddBone, so a duplicate name still resolves to
				// the bone that appeared first in the NEW order.
				if (!m_BoneNameMap.count(m_Bones[i].Name))
					// Name -> new index. Note this is the same length as the loop above;
					// the cast back to int32_t is because BoneIndex holds an int32_t.
					m_BoneNameMap[m_Bones[i].Name] = BoneIndex((int32_t)i);
			}

			// The finished hierarchy is validated before being accepted, rather than
			// trusting the reordering to have produced something well-formed. This
			// is the only place a cycle in a BuildFromNames hierarchy is caught.
			return Validate(outError);
		}

		// Name lookup. Used for authoring, NOT on the per-frame path -- the
		// header notes keeping the map out of evaluation is what keeps a frame
		// from paying a hash lookup per bone.
		BoneIndex Skeleton::FindBone(const std::string &name) const
		{
			// find, not operator[]: a const method must not mutate, and a miss must
			// not create an entry.
			auto it = m_BoneNameMap.find(name);
			// Invalid handle on a miss. An empty or unknown name is not an error
			// here -- callers that need one check for it.
			return (it == m_BoneNameMap.end()) ? BoneIndex() : it->second;
		}

		// Returns a REFERENCE, so no allocation and no copy on the caller's hot
		// path. The empty-string fallback therefore cannot be a temporary.
		const std::string &Skeleton::GetBoneName(BoneIndex index) const
		{
			// A function-local static, used purely as a null-object return value.
			// Deliberately NOT a default-constructed local: returning a reference to
			// a local would dangle the moment the function returned.
			static const std::string empty;
			// The two-part bounds test, repeated verbatim in every accessor in this
			// file. Both halves are needed: IsValid rejects the -1 sentinel, the
			// range check rejects a stale handle from a different skeleton.
			if (!index.IsValid() || (size_t)index.Value >= m_Bones.size())
				// The null object. Callers get "" rather than a crash; nothing in the
				// engine treats an empty bone name as meaningful.
				return empty;
			// The stored name by reference. Stable until the next AddBone or
			// BuildFromNames, both of which can reallocate m_Bones.
			return m_Bones[(size_t)index.Value].Name;
		}

		// The parent handle. Invalid for a root, and for a bad index.
		BoneIndex Skeleton::GetParent(BoneIndex index) const
		{
			if (!index.IsValid() || (size_t)index.Value >= m_Bones.size())
				return BoneIndex();
			// A copy, not a reference: BoneIndex is a small value type, and handing
			// out a reference into the vector would dangle on reallocation.
			return m_Bones[(size_t)index.Value].Parent;
		}

		// Child count, for arity checks in a UI or a validator.
		int Skeleton::GetChildCount(BoneIndex index) const
		{
			if (!index.IsValid() || (size_t)index.Value >= m_Bones.size())
				return 0;
			// Narrowed to int because the return type is int. A bone with more than
			// 2^31 children is not representable, which is not a practical concern.
			return (int)m_Bones[(size_t)index.Value].Children.size();
		}

		// The child at a given SLOT -- positional, not by index into the
		// hierarchy. Callers iterate 0..GetChildCount-1.
		BoneIndex Skeleton::GetChild(BoneIndex index, int childSlot) const
		{
			if (!index.IsValid() || (size_t)index.Value >= m_Bones.size())
				return BoneIndex();
			// Bound by reference: no copy of the child list per call.
			const std::vector<BoneIndex> &children = m_Bones[(size_t)index.Value].Children;
			// Both bounds checked. The negative half is not redundant -- a caller
			// looping with a signed counter that underflows would otherwise index
			// from the end via a huge size_t.
			if (childSlot < 0 || (size_t)childSlot >= children.size())
				// Invalid handle rather than clamping to a valid child: a caller
				// iterating past the end has a bug, and returning a plausible bone would
				// hide it.
				return BoneIndex();
			// The child handle by value.
			return children[(size_t)childSlot];
		}

		// Sets the bind pose -- where every bone sits when nothing is
		// animating. Must be captured BEFORE any animation runs, or the first
		// frame snaps.
		void Skeleton::SetRestPose(const Pose &pose)
		{
			// Copy per bone rather than assigning the vector wholesale: a pose
			// sampled from a clip may be shorter than the skeleton, and the
			// bones it does not cover must keep their existing rest transform
			// instead of collapsing to identity.
			// Bounded by m_RestTransforms.size(), NOT by pose.Bones.size(): a pose
			// sampled from a clip may be shorter, and the bones it does not cover
			// must KEEP their existing rest transform rather than collapsing to
			// identity. Iterating the other way round would silently zero them.
			for (size_t i = 0; i < m_RestTransforms.size(); ++i)
			{
				// The per-bone existence check. When false the loop body does nothing,
				// which is the "keep what is there" behaviour.
				if (i < pose.Bones.size())
					// Copied wholesale -- the whole BoneTransform, position and rotation and
					// scale together. No per-component interpolation: this is a bind pose,
					// not a blend.
					m_RestTransforms[i] = pose.Bones[i];
			}
			// DEFECT (documented, not fixed): Resize REBUILDS m_RestPose as all
			// identity, then the loop below refills it from m_RestTransforms. So
			// m_RestPose ends up correct -- but only for bones 0..m_RestTransforms
			// .size(). If the two arrays ever differ in length, the tail of
			// m_RestPose is identity while m_RestTransforms has no counterpart.
			// The two are kept in sync by AddBone, so this is latent.
			m_RestPose.Resize(m_Bones.size());
			// Refill from the array just written, so the Pose cache matches the
			// authoritative transforms.
			for (size_t i = 0; i < m_RestTransforms.size(); ++i)
				// Copy, mirroring the assignment above. m_RestPose exists only so
				// CaptureBindMatrices and the Animator can read a Pose without knowing
				// the skeleton's internals.
				m_RestPose.Bones[i] = m_RestTransforms[i];
		}

		// One bone's rest transform, by value. Small enough that a copy is
		// cheaper than a reference that could dangle.
		BoneTransform Skeleton::GetRestTransform(BoneIndex index) const
		{
			// Bounds-checked against m_RestTransforms specifically, not m_Bones --
			// the correct array for this accessor.
			if (!index.IsValid() || (size_t)index.Value >= m_RestTransforms.size())
				// Identity for a bad handle: the neutral transform, consistent with the
				// identity bind matrix default. Placing a bone at identity is far less
				// damaging than returning garbage.
				return BoneTransform::Identity();
			// The stored rest transform by value.
			return m_RestTransforms[(size_t)index.Value];
		}

		// Resolves a pose of LOCAL transforms into world-space matrices in a
		// single forward pass. The pass is valid only because every parent sits
		// at a lower index than its child -- an invariant AddBone enforces and
		// BuildFromNames restores by reordering.
		void Skeleton::ComputeWorldMatrices(const Pose &pose,
											std::vector<glm::mat4> &outWorld,
											const glm::mat4 &rootTransform,
											bool outWithRoot) const
		{
			// The loop bound and the output size, captured once.
			const size_t count = m_Bones.size();
			// Sized and pre-filled with identity in one call. The pre-fill is
			// overwritten on every iteration below, so it is not strictly needed --
			// it guarantees no uninitialised entry even if the loop is entered
			// zero times.
			outWorld.assign(count, m_Identity);

			// One pass, ascending. Ascending is the whole point: a parent's world
			// matrix is complete before any child reads it.
			for (size_t i = 0; i < count; ++i)
			{
				// A pose with no entry for this bone falls back to the rest
				// transform. A clip that does not animate every bone must
				// still place the ones it omits, and identity would snap them
				// to the skeleton origin.
				// DEFECT (documented, not fixed): COPIES the transform rather than
				// binding a reference. On a character with hundreds of bones that is
				// hundreds of struct copies per frame, per call, for a value that is
				// only read. A const reference would do -- but it cannot be bound
				// directly to the ternary result, so the copy is a consequence of the
				// two-source fallback written this way.
				// 						  ? pose.Bones[i]
				// The animated transform, when the pose covers this bone.
				// 						  : m_RestTransforms[i];
				// Otherwise the rest transform. A clip that does not animate every bone
				// must still PLACE the ones it omits -- using identity would snap them
				// to the skeleton origin, which is the single most common source of
				// "my character collapses" bugs.
				BoneTransform local = (i < pose.Bones.size())
										  ? pose.Bones[i]
										  : m_RestTransforms[i];

				// Composed once per bone: T * R * S, in that order. Scale first, then
				// rotation, then the parent offset.
				glm::mat4 localMatrix = local.ToMatrix();
				// Read out of the bone rather than looked up by name -- this is the
				// hot path, and the header notes exactly this keeps a frame from paying
				// a hash lookup per bone.
				BoneIndex parent = m_Bones[i].Parent;

				// THE INVARIANT, used as a guard rather than assumed. A parent at a
				// LOWER index is already resolved, so its world matrix can be
				// multiplied on.

				// DEFECT (documented, not fixed): the second half is a silent FALLBACK,
				// not an error. A bone whose parent sits at a HIGHER index -- a
				// malformed skeleton -- is silently treated as a ROOT, so it detaches
				// from the hierarchy and renders at the wrong place with no diagnostic
				// at all. Validate catches this ordering, but ComputeWorldMatrices is
				// const and runs per frame; it cannot report, and it does not.
				if (parent.IsValid() && (size_t)parent.Value < i)
					// Parent world TIMES local. Order is not interchangeable: this is what
					// "relative to its parent" means, and reversing it would translate in
					// local space rather than world space.
					outWorld[i] = outWorld[(size_t)parent.Value] * localMatrix;
				else
					// The fallback: treated as a root, so no parent transform is applied.
					// Correct for a genuine root, wrong-but-silent for a malformed one.
					outWorld[i] = localMatrix;
			}

			// The object transform, applied only on request and only when it is not
			// already identity -- a redundant multiply per bone is worth skipping.

			// This flag is the difference between "the pose in world space"
			// (rendering a character standing in the level) and "the pose in the
			// CHARACTER'S local space" (skinning matrices, where the object
			// transform is applied later and must not be baked in).
			if (outWithRoot && rootTransform != m_Identity)
			{
				// A second pass, unavoidable: the root transform cannot be folded in
				// during the first because a parent's world matrix is not final until
				// the root transform is known.
				for (size_t i = 0; i < count; ++i)
					// Object transform on the LEFT, so it is applied outermost -- the whole
					// hierarchy ends up expressed in the object's space.
					outWorld[i] = rootTransform * outWorld[i];
			}
		}

		// worldPose * inverseBindMatrix per bone -- the matrices a skinned mesh
		// is deformed by. That product is the whole reason bind matrices are
		// cached: it is the expensive half, and it never changes unless the
		// skeleton is re-bound.
		void Skeleton::ComputeSkinningMatrices(const Pose &pose,
											   std::vector<glm::mat4> &outSkinning,
											   const glm::mat4 &rootTransform) const
		{
			// outWithRoot = FALSE, and this is the critical argument. The bind
			// matrices were captured in the character's own space, so baking the
			// object transform in here would apply it twice -- once in the inverse
			// bind and once in the world pose.
			ComputeWorldMatrices(pose, outSkinning, rootTransform, false);

			// BOTH sizes checked. outSkinning comes from ComputeWorldMatrices and
			// equals the bone count, so the second half is defensive -- but
			// m_Bones.size() is what indexes GetInverseBindMatrix, so a mismatch
			// would read past it.
			for (size_t i = 0; i < outSkinning.size() && i < m_Bones.size(); ++i)
				// World times inverse bind. Order matters absolutely: the other way
				// round would apply the deformation in the wrong space, and the mesh
				// would visibly come apart at every joint.
				// (the loop index is cast to int32_t for BoneIndex's constructor)
				outSkinning[i] = outSkinning[i] * GetInverseBindMatrix(BoneIndex((int32_t)i));
		}

		// Snapshots the inverse bind matrices from the CURRENT rest pose. Must
		// be called once before ComputeSkinningMatrices is used.
		void Skeleton::CaptureBindMatrices()
		{
			const size_t count = m_Bones.size();
			// Sized and defaulted in one call. The identity default means a skeleton
			// that is never re-captured keeps "no deformation" rather than garbage.
			// The bind matrices come from the REST pose, not the current one. Using
			// the current pose would make the bind pose itself the identity, and
			// every skinned mesh would collapse to a primitive at the joint instead
			// of keeping its shape.
			m_InverseBindMatrices.assign(count, m_Identity);

			// The bind matrices come from the REST pose, not the current one.
			// Using the current pose would make the bind pose itself the
			// identity, and every skinned mesh would then collapse to a
			// primitive at the joint instead of keeping its shape.
			// Scratch, local rather than a member: this runs once at setup, not per
			// frame, so there is nothing to reuse.
			std::vector<glm::mat4> world;
			// Resolve the rest pose into world matrices, with outWithRoot = false
			// for the same reason ComputeSkinningMatrices does -- these are
			// character-space by definition.
			ComputeWorldMatrices(m_RestPose, world, m_Identity, false);

			// One inverse per bone.
			for (size_t i = 0; i < count; ++i)
				// A general 4x4 inverse, not a transpose. That is correct ONLY because
				// bone transforms are rigid (rotation and translation, unit scale) --
				// for a rigid matrix the inverse equals the transpose, and this is
				// still the general and safe choice.

				// DEFECT (documented, not fixed): a rest pose containing a ZERO-SCALE
				// bone produces a singular matrix, and glm::inverse of a singular
				// matrix is undefined -- it yields inf/NaN rather than an error. Those
				// NaNs then propagate into every vertex skinned to that bone and, via
				// the hierarchy, into its descendants. There is no singularity check.
				m_InverseBindMatrices[i] = glm::inverse(world[i]);
		}

		// One bone's inverse bind matrix. Identity for an out-of-range index --
		// the safe default, since it makes an un-bound skeleton behave as "no
		// deformation" rather than collapsing the mesh to a point.
		const glm::mat4 &Skeleton::GetInverseBindMatrix(BoneIndex index) const
		{
			// Bounds-checked against the bind matrix array itself, which is the
			// array being indexed. Note this array can be SMALLER than m_Bones if
			// bones were added after the last CaptureBindMatrices call -- so this
			// check is load-bearing, not defensive.
			if (!index.IsValid() || (size_t)index.Value >= m_InverseBindMatrices.size())
				// The member identity, by reference. Returning the member rather than a
				// temporary keeps the returned reference valid indefinitely.
				return m_Identity;
			// A reference into the vector. Invalidated by a later CaptureBindMatrices
			// -- or by AddBone, which push_backs and may reallocate.
			return m_InverseBindMatrices[(size_t)index.Value];
		}

		// Checks the hierarchy for duplicate names, out-of-range parents,
		// children appearing before their parent, and cycles. Bones added
		// piecemeal via AddBone can only be checked after the fact, which is
		// what this is for.
		bool Skeleton::Validate(std::string &outError) const
		{
			// Cleared first so a caller reusing one string never displays a stale
			// message from a previous failed call.
			outError.clear();

			// An empty skeleton has no root and nothing to validate. The same
			// message BuildFromNames produces for the same condition, reached
			// through a different path.
			if (m_Bones.empty())
			{
				// Same text as the BuildFromNames guard -- duplicated rather than
				// shared, since the two are separate call paths.
				outError = "skeleton has no bones";
				return false;
			}

			// An ORDERED set of names seen so far. std::set rather than
			// unordered_set so the reported duplicate is deterministic -- the first
			// in bone order, not whichever the hash happened to surface.
			std::set<std::string> seen;
			// One pass over the bones for the three cheap structural checks.
			for (size_t i = 0; i < m_Bones.size(); ++i)
			{
				// insert().second is false when the name was ALREADY present, which is
				// exactly the duplicate condition. One lookup does the work of a count
				// plus an insert.
				if (!seen.insert(m_Bones[i].Name).second)
				{
					// Names the offender. A duplicate is fatal to name-based lookup:
					// FindBone could only ever return one of the two.
					outError = "duplicate bone name '" + m_Bones[i].Name + "'";
					return false;
				}

				// Read once and reused by the next two checks.
				BoneIndex parent = m_Bones[i].Parent;
				// A parent handle pointing outside the bone array. Structurally
				// impossible via AddBone, but reachable if the arrays were ever
				// reordered without remapping the handles.
				if (parent.IsValid() && (size_t)parent.Value >= m_Bones.size())
				{
					// Names the bone whose parent handle is bad -- the parent may not even
					// have a name to report.
					// 						   "' has out-of-range parent index";
					// (continuation of the message above)
					outError = "bone '" + m_Bones[i].Name +
							   "' has out-of-range parent index";
					return false;
				}

				// A parent must come first, or ComputeWorldMatrices' forward
				// pass reads an unresolved matrix.
				// A parent at a HIGHER index. This is the ordering invariant the single
				// forward pass depends on: a parent must come first, or the pass reads
				// an unresolved matrix.

				// The check is `> i`, not `>= i`. So a bone that is its OWN parent
				// (parent.Value == i) passes here and is caught only by the cycle walk
				// below -- which is fine, since that walk does catch it, but it means
				// the two checks are not independent.
				if (parent.IsValid() && (size_t)parent.Value > i)
				{
					// Names the offending bone so the author can find the bad entry.
					// 						   "' appears before its parent";
					// (continuation of the message above)
					outError = "bone '" + m_Bones[i].Name +
							   "' appears before its parent";
					return false;
				}
			}

			// Bones with no parent. Counted rather than merely tested, because
			// several roots is legal (the header notes GetRoot returns index 0 and
			// is valid on any non-empty skeleton) -- the check below is only for
			// having NO root at all.
			int rootCount = 0;
			// A second pass, because cycle detection needs a walk from every bone.
			for (size_t i = 0; i < m_Bones.size(); ++i)
			{
				if (!m_Bones[i].Parent.IsValid())
					// Counted in the same loop that walks for cycles, so the cost of the
					// extra pass is avoided.
					// Walk to the root looking for a repeat. A cycle would make
					// ComputeWorldMatrices multiply a bone by itself forever, which shows
					// up as a NaN several frames later with no obvious cause -- worth
					// catching at the source.
					++rootCount;

				// Walk to the root looking for a repeat. A cycle would make
				// ComputeWorldMatrices multiply a bone by itself forever,
				// which shows up as a NaN several frames later with no obvious
				// cause -- worth catching at the source.
				// Indices seen on THIS walk. A set rather than a counter or a
				// generation-marked array: correctness first, and Validate is not on
				// the per-frame path.

				// Allocated fresh per bone, so this is O(bones^2) in the worst case for
				// a deep hierarchy. Acceptable for an authoring-time check; it would not
				// be acceptable per frame.
				std::set<int32_t> visited;
				// Start the walk at the bone itself, so a bone that is its own parent is
				// detected on the first iteration.
				BoneIndex walk((int32_t)i);
				// Ascend until a root (invalid parent) or a repeat.
				while (walk.IsValid())
				{
					// Already seen this index on this walk, so the ascent has looped.
					// Detected by set insertion failing rather than by a counter, which
					// would need a bound.
					if (!visited.insert(walk.Value).second)
					{
						// Names the bone where the repeat was DETECTED, which is the last bone
						// before the loop closes -- not necessarily the one that started it.
						// 						   m_Bones[(size_t)walk.Value].Name + "'";
						// (continuation of the message above)
						outError = "cycle in bone hierarchy at '" +
								   m_Bones[(size_t)walk.Value].Name + "'";
						return false;
					}
					// Next step up. Safe: the bounds check in the first loop above already
					// established every parent handle is in range.
					walk = m_Bones[(size_t)walk.Value].Parent;
				}
			}

			// No root at all, which means every bone has a parent -- and the only
			// way to arrange that without a cycle is impossible, so this is
			// effectively a backstop for the cycle check above.
			if (rootCount == 0)
			{
				// The parenthetical names the presumed cause. Note this branch is
				// REACHABLE even though the cycle check runs first: a hierarchy where
				// every bone has a valid in-range parent always either contains a cycle
				// (caught above) or terminates at a root. So reaching here implies a
				// cycle that the walk somehow missed -- in practice this is
				// defence-in-depth rather than a live path.
				outError = "skeleton has no root bone (cycle in the hierarchy)";
				return false;
			}

			// All four structural checks passed. This says NOTHING about rest
			// transforms or bind matrices: a skeleton can be perfectly structured
			// -- with every bone at identity, i.e. collapsed to a single point.
			return true;
		}

	} // namespace Animation

} // namespace Wasteland
