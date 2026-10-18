// The animation hierarchy: a BoneIndex handle, a TRS transform, a flat Pose,
// and the Skeleton that resolves a pose into world and skinning matrices.
//
// Header-only declarations -- every operation below is defined in Skeleton.cpp.
// Unlike the NVRHI and renderer headers in this tree, this one carries no
// platform or backend responsibility at all.
#pragma once

// glm's core: vec3, mat4 and the translate/scale helpers ToMatrix composes with.
//
// glm here is the maths library only. No <glm/gtc/matrix_transform.hpp> appears
// below, so glm::translate must be reaching the compiler through this umbrella
// header or a transitive one -- worth knowing if the include set is ever trimmed.
#include <glm/glm.hpp>
// The quaternion extension, for glm::quat and the mat4_cast below.
//
// Separate from line 3 because quaternion is an extension module rather than core,
// and the distinction is exactly the reason the struct stores Rotation as a quat
// (see the comment at that field).
#include <glm/gtc/quaternion.hpp>

// For bone names and the outError message strings. Note every error report here
// is a std::string out-parameter rather than an exception -- build and load paths
// in this engine report failure by return value.
#include <string>
// For m_BoneNameMap, the name -> index table used by authoring code only.
#include <unordered_map>
// For the pose's bone array and every per-bone parallel array in Skeleton.
#include <vector>

// The outer namespace, closed at the end of the file with a trailing comment.
namespace Wasteland {

	// A nested namespace rather than a flat Wasteland::BoneIndex, so animation types
	// are namespaced even in a codebase that has no SkinnedMesh yet -- the header
	// comment below explains the split is deliberate.
	namespace Animation {

		// Index into a Skeleton's bone array. Wrapped so a bad index is a
		// distinct type rather than a bare int that can be confused with any
		// other count -- and so a defaulted bone handle reads as "none"
		// instead of silently meaning bone 0, which is almost always wrong.
		struct BoneIndex
		{
			// -1 is the invalid sentinel, and -1 rather than 0 is the whole design: a
			// default-constructed handle must NOT silently mean bone 0.
			//
			// Bone 0 is almost always wrong as a default -- it is the root, and code that
			// forgot to set an index would then animate the root instead of doing nothing.
			// IsValid() is the single test for it.
			int32_t Value = -1;

			// The default constructor, which leaves Value at -1 via the member initialiser.
			//
			// So BoneIndex() is the "none" handle, and AddBone's `parent` parameter defaults
			// to it below -- which is how a root bone is requested.
			BoneIndex() = default;
			// EXPLICIT, and that is the point of the wrapper.
			//
			// Without `explicit`, `SomeFunction(3)` would silently become a BoneIndex(3) --
			// the bare-int-to-bone-index confusion the struct exists to prevent. With it, a
			// caller must write BoneIndex(3), which is one extra token and documents intent.
			//
			// Also means a BoneIndex will NOT implicitly convert to int, so code cannot treat a
			// bone handle as an array offset without an explicit .Value.
			explicit BoneIndex(int32_t v) : Value(v) {}

			// The validity test, and the only place the -1 convention is interpreted.
			//
			// `>= 0` rather than `!= -1`: a BoneIndex holding some other negative value --
			// constructible, since the constructor is not range-checked -- also reads as
			// invalid, which is the safe answer.
			bool IsValid() const { return Value >= 0; }
			// Explicit again, for the same reason: `if (bone)` should not compile for a bone
			// handle, but `if (bone.IsValid())` or `if (bone)` with the explicit conversion in a
			// boolean context should.
			//
			// Lets `if (index)` work in a condition without permitting an accidental
			// arithmetic conversion.
			explicit operator bool() const { return IsValid(); }

			// The three comparison operators, all defined inline in the header.
			//
			// operator< exists so BoneIndex can key an ordered container or be sorted -- the
			// natural order is index order, which is also parent-before-child order because
			// AddBone requires the parent to exist first. That coincidence is what makes the
			// single-pass hierarchy resolution below valid.
			bool operator==(const BoneIndex &o) const { return Value == o.Value; }
			bool operator!=(const BoneIndex &o) const { return Value != o.Value; }
			bool operator<(const BoneIndex &o) const { return Value < o.Value; }
		};

		// One bone's local transform, relative to its parent.
		//
		// Stored as TRS rather than a matrix because that is what an animator
		// edits and what a curve set produces. Composing to a matrix happens
		// once per bone per frame, at hierarchy-resolution time.
		struct BoneTransform
		{
			// The T of TRS, defaulting to the origin.
			//
			// All three fields have their IDENTITY defaults rather than being left
			// uninitialised. That is what lets BoneTransform() be the identity transform, so
			// a default-constructed BoneTransform composes to an identity matrix and a
			// Pose::Resize can fill with it.
			glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
			// Kept a quaternion, not Euler angles. Euler has gimbal lock at
			// +/-90 degrees and interpolating two of them takes the long way
			// round; a quaternion slerps the short way and is continuous. This
			// is the single most important reason animation is not built on
			// Euler.
			// {w, x, y, z} with w = 1 -- the IDENTITY quaternion, not a zero quat.
			//
			// glm orders the components w first, which is the opposite of most maths
			// libraries' xyzw and a common source of transposed-rotation bugs when data is
			// written by hand. Read this default as documentation of the convention.
			//
			// A zero quaternion here would be the degenerate case: mat4_cast of it produces a
			// matrix of zeros, collapsing every bone to the origin.
			glm::quat Rotation = {1.0f, 0.0f, 0.0f, 0.0f};
			// Unit scale, so a default BoneTransform has no scale.
			//
			// DEFECT (non-uniform scale and normals): the struct stores an independent scale
			// per axis but ToMatrix below composes without any inverse-transpose correction,
			// so non-uniform scale would skew normals on skinned meshes. Harmless for the
			// uniform scaling that bone hierarchies actually use; a real hazard only if an
			// importer starts producing non-uniform bone scale.
			glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

			BoneTransform() = default;
			// The three-argument constructor, taking const references rather than by value.
			//
			// Slightly wasteful for 3-float and 4-float types -- a copy would be cheaper than
			// an indirection -- but consistent with the rest of the engine's style and it
			// makes it obvious these are inputs, not outputs.
			BoneTransform(const glm::vec3 &t, const glm::quat &r, const glm::vec3 &s)
				// The member initialiser list, in DECLARATION order (Translation, Rotation, Scale)
				// rather than the parameter order above.
				//
				// They happen to be the same order here. C++ initialises members in declaration
				// order regardless of how the list is written, and compilers warn (-Wreorder)
				// when the two differ -- which for glm types with real constructors is the
				// difference between correct initialisation and a subtly wrong one.
				: Translation(t), Rotation(r), Scale(s) {}

			// Composes to a matrix in the parent's space.
			//
			// T * R * S in that order: scale is applied first to the geometry,
			// then rotation, then the parent offset. The reverse order gives a
			// visibly different result for anything that is both rotated and
			// scaled, which is most things.
			// The TRS -> matrix composition, and the multiplication order below is the whole
			// subtlety of this function.
			//
			// `const` and returning by value: a 4x4 matrix is 64 bytes, so a copy is fine and
			// returning a reference would invite aliasing at every call site.
			glm::mat4 ToMatrix() const
			{
				// glm::translate IDENTITY * translation, i.e. just a translation matrix. The
				// explicit mat4(1.0f) base makes the composition read uniformly with the other
				// two terms.
				//
				// glm applies post-multiplication here, so these three calls compose
				// LEFT-TO-RIGHT as scale, then rotation, then translation -- which is the
				// correct order for a transform, and the reverse would visibly deform anything
				// both rotated and scaled.
				return glm::translate(glm::mat4(1.0f), Translation)
					 // The quaternion to matrix conversion. glm::mat4_cast is the free function for a
					 // stand-alone quat, as distinct from the glm::to_mat4 form used with quaternions
					 // that carry a translation component.
					 //
					 // This runs ONCE per bone per frame, at hierarchy-resolution time -- which is
					 // precisely why BoneTransform stores a quat rather than a matrix: an animator
					 // interpolates quats, and the cast happens after all interpolation is done.
					 * glm::mat4_cast(Rotation)
					 * glm::scale(glm::mat4(1.0f), Scale);
			}

			// A named factory for the identity transform.
			//
			// Redundant -- BoneTransform() IS the identity, since every field defaults to its
			// identity value. It exists for READABILITY at the call site: Pose::Resize below
			// says Bones.assign(boneCount, BoneTransform::Identity()), which states the
			// intent where a bare BoneTransform() would not.
			static BoneTransform Identity() { return BoneTransform(); }
		};

		// A pose is one local transform per bone.
		//
		// A flat vector rather than a tree of nodes: the hierarchy lives in the
		// Skeleton (which is shared and immutable during playback), and the
		// Pose is just the per-frame data. Copying a flat vector of small
		// structs is fast, which matters because blending produces poses
		// every frame.
		// The per-frame data, and the separation from Skeleton is the key structural
		// decision: the hierarchy lives in the Skeleton (shared, immutable during
		// playback), while the Pose is just one local transform per bone.
		//
		// So two characters sharing a skeleton have separate Poses and share nothing
		// mutable -- which is what makes the type safe to blend every frame.
		struct Pose
		{
			// FLAT and parallel to Skeleton's bone array -- index N here is the same bone as
			// index N there.
			//
			// That invariant is what lets the hierarchy resolution below index both with the
			// same BoneIndex and never search. A tree of poses would need a lookup per node
			// and could not be memcpy'd or blended with a loop.
			//
			// Public with no accessor, which is a mild encapsulation leak: a caller can
			// resize it out of step with the skeleton. Every read path below range-checks
			std::vector<BoneTransform> Bones;

			// Resizes to `boneCount`, filling with identity. Identity is the
			// right default: a bone that no clip animates must still be
			// positioned by its own rest transform, and identity would put it
			// at the skeleton root.
			// assign, not resize -- assign both sizes the vector AND fills every element.
			//
			// resize would leave new elements value-initialised, which for BoneTransform
			// happens to be the same identity... but only by accident of the member
			// initialisers. assign states the intent: this pose has exactly boneCount bones,
			// all identity.
			void Resize(size_t boneCount)
			{
				// The fill value, and the pre-existing comment explains why identity is correct:
				// a bone no clip animates must still be placed by its own rest transform.
				//
				// Without this a fresh pose would put every unanimated bone at the skeleton root,
				// which collapses the character -- the classic "T-pose implodes" bug.
				Bones.assign(boneCount, BoneTransform::Identity());
			}

			// Cleared, not resized to zero -- same effect on the vector's size, but this also
			// releases the capacity back to the allocator.
			//
			// Correct for a reset between clips. Slightly wasteful if the pose is reset every
			// frame for blending, where retaining the capacity would be better.
			void Clear() { Bones.clear(); }
			bool IsEmpty() const { return Bones.empty(); }
			// size_t, unlike Skeleton::GetBoneCount below which returns int.
			//
			// The inconsistency is deliberate-looking in one direction and not the other:
			// returning size_t avoids a signed/unsigned warning in the resize comparisons it
			// feeds. It does mean callers comparing it to a Skeleton::GetBoneCount() result
			// get a sign-compare warning.
			size_t GetBoneCount() const { return Bones.size(); }

			// Out-of-range writes are ignored rather than undefined. A pose
			// sampled from a clip authored for a different skeleton is a
			// plausible authoring mistake, and crashing the whole frame on it
			// is worse than animating nothing.
			// The MUTABLE accessor, returning a pointer into the vector.
			//
			// A raw pointer rather than a reference because the out-of-range case has to be
			// representable, and a null return is how that case is reported.
			//
			// DEFECT (dangling pointer): the pointer aliases Bones, so any Resize, Clear or
			// vector reallocation while it is held invalidates it. Callers must not hold one
			// across a pose mutation. Returning an index or a span would be safer.
			BoneTransform *GetBone(BoneIndex index)
			{
				// Both conditions, and the first is not redundant.
				//
				// IsValid() rejects the -1 sentinel BEFORE the cast. Without it, (size_t)(-1) is
				// a huge number -- which would still compare >= size() and return nullptr by
				// accident, but only because the sentinel happens to be -1. Testing validity
				// first is what makes the check correct for any negative value.
				//
				// The cast to size_t is needed because Value is signed and size() is unsigned;
				// comparing them directly would promote the int to unsigned and turn -1 into the
				// same huge number.
				if (!index.IsValid() || (size_t)index.Value >= Bones.size())
					return nullptr;
				// A pointer to the element, so the caller can read AND write it.
				//
				// The cast is repeated rather than hoisted into a local -- a local would be cleaner
				// but this is a two-line function and the compiler CSEs it anyway.
				return &Bones[(size_t)index.Value];
			}

			// The CONST overload, returning a pointer to const.
			//
			// Not an operator[]: the const-ness has to be enforced by the return type, since a
			// reference return would still allow mutation through a const BoneTransform* that
			// the caller could const_cast. This is the correct conservative shape.
			const BoneTransform *GetBone(BoneIndex index) const
			{
				// The same bounds check, duplicated rather than shared with the non-const
				// overload.
				//
				// Two lines, so duplication is the right trade: routing one through the other
				// would require a const_cast or a private helper taking a const vector, and either
				// costs more than repeating the comparison. The check is deliberately NOT
				// skipped in a release build -- a pose sampled from a mismatched clip is a
				// plausible authoring mistake, and the pre-existing comment says crashing the
				// frame is worse.
				if (!index.IsValid() || (size_t)index.Value >= Bones.size())
					return nullptr;
				return &Bones[(size_t)index.Value];
			}
		};

		// A named hierarchy of bones, and the operations over a pose of it.
		//
		// Separate from SkinnedMesh because the bone hierarchy is useful on its
		// own: a turret with a rotating barrel, a swinging door, a camera on a
		// boom all need exactly this and none of them need skinning.
		// No virtual methods and no virtual destructor: the type is concrete, copied and
		// owned by value by the animation system, and never used through a base pointer.
		//
		// It also holds no allocation beyond its own vectors, so there is nothing to
		class Skeleton
		{
		public:
			// Leaves every vector empty and m_RestPose default.
			//
			// An empty Skeleton is a coherent state -- IsEmpty() and GetRoot() both handle it,
			// returning an invalid root rather than indexing into nothing. So a
			// default-constructed skeleton is safe to pass around before it is built.
			Skeleton() = default;

			// ---- Construction ----

			// Adds a bone and returns its index. `parent` may be invalid for a
			// root. Bones are stored in insertion order; the parent must
			// already exist, which is what lets hierarchy resolution be a
			// single forward pass with no sorting.
			// The construction primitive, and the default argument is the root request: an
			// invalid `parent` means "no parent", which is how a root bone is added.
			//
			// `name` by const reference because a bone name is a short string and callers pass
			// literals or existing names -- no copy is made here, though one will be made into
			// the Bone. The pre-existing comment above states the ordering invariant this
			// signature exists to enforce.
			BoneIndex AddBone(const std::string &name, BoneIndex parent = BoneIndex());

			// Builds a whole hierarchy in one call, resolving names to indices.
			// Returns false and fills `outError` if a parent name is unknown
			// or a child is listed before its parent.
			// The bulk construction path, and the one most importers will use: they have bone
			// names and parent NAMES, not indices.
			//
			// Both vectors by const reference and the same size by contract. `outError` is a
			// non-const reference, matching every other error path in the engine -- the call
			// must supply a string and reads the failure reason from it.
			bool BuildFromNames(const std::vector<std::string> &names,
								// The parallel array of parent names.
								//
								// DEFECT (no size check on the contract): the signature documents that a child
								// listed before its parent is an error, but nothing in the SIGNATURE enforces that
								// parentNames.size() == names.size(). Passing a shorter vector is undefined
								// behaviour at the implementation's index arithmetic, not a clean false. A size_t
								// parameter for the parent count, or a defaulted empty vector, would make the
								// contract visible.
								const std::vector<std::string> &parentNames,
								std::string &outError);

			// ---- Queries ----

			// Returns INT, casting from size_t -- unlike Pose::GetBoneCount which returns
			// size_t.
			//
			// DEFECT (narrowing): an explicit cast, so a skeleton with more than INT_MAX bones
			// would silently report a negative or wrapped count. Not reachable in practice
			// (INT_MAX bones is far beyond any rig), and bone indices are int32_t throughout,
			// so int is at least consistent with BoneIndex::Value.
			int GetBoneCount() const { return (int)m_Bones.size(); }
			bool IsEmpty() const { return m_Bones.empty(); }

			BoneIndex FindBone(const std::string &name) const;
			const std::string &GetBoneName(BoneIndex index) const;

			BoneIndex GetParent(BoneIndex index) const;
			int GetChildCount(BoneIndex index) const;
			BoneIndex GetChild(BoneIndex index, int childSlot) const;

			// The bone at the top of the hierarchy. Valid on any non-empty
			// skeleton, since a skeleton is a tree with exactly one root.
			// Bone 0 IS the root, which the pre-existing comment justifies: a skeleton is a
			// tree with exactly one root, and AddBone's insertion-order rule means the first
			// bone added has no parent.
			//
			// DEFECT (assumption not enforced): BuildFromNames resolves parent NAMES, so it
			// CAN produce a first bone whose named parent appears later -- which the header
			// says is rejected. Whether the implementation actually rejects it or ends up
			// with no root is the question; Validate() below exists precisely to catch
			// ordering problems after the fact, which suggests the construction path may not
			// catch them all.
			//
			// The ternary returns the INVALID handle on an empty skeleton rather than
			// BoneIndex(0), so a caller checking the result first cannot read past the end.
			BoneIndex GetRoot() const { return m_Bones.empty() ? BoneIndex() : BoneIndex(0); }

			// A predicate rather than an accessor, so it can be used in an if without
			// comparing against GetRoot().
			//
			// Defined in the header because it is one expression and needs no project state
			// beyond m_Bones.
			bool IsRoot(BoneIndex index) const
			{
				// DEFINES root as "has no parent", not as "is index 0".
				//
				// That is the better definition and is robust to a hierarchy where index 0 is not
				// the root -- which GetRoot() above assumes but this does not.
				//
				// DEFECT (missing bounds check): unlike Pose::GetBone, this indexes m_Bones
				// directly with no `>= m_Bones.size()` test. A valid-but-out-of-range BoneIndex
				// (constructible via the explicit constructor) reads out of bounds here, where
				// GetBone would have returned nullptr. The short-circuit on index.IsValid()
				// covers the -1 sentinel only.
				return index.IsValid() && !m_Bones[(size_t)index.Value].Parent.IsValid();
			}

			// ---- Rest pose ----

			// The bind pose: where every bone sits when nothing is animating.
			// A clip's values are offsets from this, so it must be captured
			// before any animation runs or the first frame would snap.
			// The bind pose, and the pre-existing comment states the ordering requirement:
			// capture it BEFORE any animation runs, or the first frame snaps to whatever pose
			// happened to be current.
			//
			// By const reference -- a Pose can hold hundreds of BoneTransforms, so copying it
			// to store what is already a reference to caller data would be wasteful.
			void SetRestPose(const Pose &pose);
			// A const reference to the member, not a copy.
			//
			// Correct here because the reference's lifetime is tied to the Skeleton, not to a
			// temporary -- unlike GetBone above, whose result aliases a vector the caller may
			// mutate.
			const Pose &GetRestPose() const { return m_RestPose; }

			BoneTransform GetRestTransform(BoneIndex index) const;

			// ---- Evaluation ----

			// Resolves a pose of local transforms into world-space matrices.
			//
			// Single forward pass over the bones, which is valid because
			// AddBone requires a parent to exist first -- so a parent is
			// always at a lower index than its child and is already resolved.
			//
			// `outWorld` is resized to the bone count. `outWithRoot` controls
			// whether the skeleton's root transform is applied, which is the
			// difference between "the pose in world space" (rendering a
			// character standing in the level) and "the pose in the
			// character's local space" (computing skinning matrices, where the
			// object transform is applied later and must not be baked in).
			// The hierarchy resolution, and the single-forward-pass claim above it is the
			// load-bearing design fact of the whole class.
			//
			// A parent is always at a LOWER index than its child, because AddBone requires the
			// parent to exist first. So iterating bones in index order means every parent is
			// already resolved -- no recursion, no depth-first walk, no topological sort.
			//
			// That makes evaluation O(bones) with a predictable memory access pattern, which
			// is what a per-frame path over hundreds of characters needs.
			void ComputeWorldMatrices(const Pose &pose,
									  // An OUT-parameter rather than a return value, and the pre-existing comment says
									  // why: it is resized to the bone count and filled in place, so the caller can
									  // reuse one buffer across characters and frames without a per-frame allocation.
									  //
									  // Non-const reference, so this writes rather than appends -- the vector is
									  // overwritten in full each call.
									  std::vector<glm::mat4> &outWorld,
									  // The root's world transform, defaulting to identity.
									  //
									  // DEFECT (default-argument gotcha): a temporary glm::mat4 is constructed at EVERY
									  // call site that omits the argument, because a default argument is re-evaluated
									  // per call, not once. For a 64-byte matrix called once per character per frame
									  // that is real but small avoidable work -- inlining the identity as a static const
									  // would remove it. The same pattern is on ComputeSkinningMatrices below.
									  const glm::mat4 &rootTransform = glm::mat4(1.0f),
									  // The flag that separates the two use cases the pre-existing comment describes:
									  // true gives "the pose in world space" (rendering a character standing in the
									  // level), false gives "the pose in the character's local space" (computing
									  // skinning matrices, where the object transform is applied later and must not be
									  // baked in).
									  //
									  // DEFECT (redundant with rootTransform): BOTH parameters control the same thing.
									  // Passing a non-identity rootTransform with outWithRoot false silently ignores
									  // rootTransform, and the two defaults contradict each other -- an identity
									  // rootTransform with outWithRoot true applies an identity, which is a no-op.
									  // Either the flag or the parameter is redundant, and there is no comment saying
									  // which wins.
									  bool outWithRoot = false) const;

			// The matrices a skinned mesh is deformed by:
			// worldPose * inverseBindMatrix.
			//
			// That product is the whole reason bind matrices are cached per
			// bone at bind time -- it is the expensive half, and it never
			// changes unless the skeleton is re-bound.
			// The second evaluation path, producing worldPose * inverseBindMatrix.
			void ComputeSkinningMatrices(const Pose &pose,
										 // Same out-parameter pattern as above, and the same reason: buffer reuse across
										 // frames rather than a fresh allocation per call.
										 std::vector<glm::mat4> &outSkinning,
										 // NOTE the missing outWithRoot flag: this overload always applies the root
										 // transform, defaulting to identity.
										 //
										 // That asymmetry with ComputeWorldMatrices above is not obvious. A skinning
										 // matrix is object-space by definition, so applying the root here is correct, but
										 // the caller cannot opt out -- and the pre-existing comment about "applied later
										 // and must not be baked in" describes the OTHER function's default. Two adjacent
										 // APIs with opposite defaults and no signpost is a real trap.
										 const glm::mat4 &rootTransform = glm::mat4(1.0f)) const;

			// ---- Bind matrices ----

			// Captures the inverse bind matrices from the current rest pose.
			// Must be called once before ComputeSkinningMatrices is used.
			// The one-time bind capture, and the pre-existing comment states the contract:
			// call once before ComputeSkinningMatrices is used.
			//
			// DEFECT (silent failure mode): this returns void, so there is no way to tell
			// whether it succeeded or whether the skeleton was empty. GetInverseBindMatrix
			// below papers over that by returning identity, so an omitted capture produces
			// geometry that does not deform rather than an error. A bool return would have
			// caught the omission at the call site.
			void CaptureBindMatrices();

			// The inverse bind matrix for a bone, or identity if none was
			// captured. Identity is the safe default: it makes an un-bound
			// skeleton behave as "no deformation", rather than collapsing the
			// mesh to a point.
			// Returns a REFERENCE to the matrix, not a copy -- a deliberate and correct
			// optimisation for a 64-byte value read per bone per vertex-batch.
			//
			// DEFECT (the reason identity is the default is subtle): on an out-of-range
			// index this must return something, and it returns m_Identity. But a REFERENCE
			// cannot be null, so "no bind matrix captured" and "this bone does not exist"
			// are indistinguishable to the caller -- both come back as identity. GetBone
			// above returns a pointer and CAN say null; this cannot.
			//
			// Identity is the right value for the un-captured case (the pre-existing comment
			// says so: it makes an un-bound skeleton behave as no deformation rather than
			// collapsing the mesh to a point).
			const glm::mat4 &GetInverseBindMatrix(BoneIndex index) const;

			// ---- Validation ----

			// Checks the hierarchy for cycles, duplicate names, and children
			// whose parent appears later in the array. BuildFromNames enforces
			// ordering at construction, but bones added piecemeal via AddBone
			// can only be checked after the fact. Returns false with a
			// description of the first problem found.
			// The post-hoc integrity check, and the pre-existing comment explains why it is
			// needed at all: BuildFromNames enforces parent ordering at construction, but
			// bones added piecemeal through AddBone can only be checked afterwards.
			//
			// Checks cycles, duplicate names, and out-of-order parents. Cycles matter because
			// the hierarchy resolution above is a single forward pass that would loop forever
			// or silently ignore a back-edge -- there is no cycle detection in the hot path,
			// by design, which makes this the only place it can happen.
			bool Validate(std::string &outError) const;

		private:
			// The per-bone record, and note it stores BOTH directions of the hierarchy:
			struct Bone
			// Parent and Children are redundant with each other, and that redundancy is
			{
				// DEFECT (memory cost, deliberate): a std::string per bone rather than an index
				// into a shared name pool. For a 150-bone rig that is 150 heap-allocated strings
				// duplicated across every Skeleton instance.
				//
				// The trade is that FindBone and the authoring UI need no lifetime rules -- a
				// Skeleton is copyable and self-contained. Reasonable for rigs loaded once, and
				// the wrong call if skeletons are ever streamed per-character.
				std::string Name;
				BoneIndex Parent;
				// The children, which is redundant with every other bone's Parent field and can
				// disagree with it.
				//
				// DEFECT (two sources of truth): nothing in this header keeps them in sync, and
				// every mutation (AddBone, Reparent if one is ever added) has to update both. The
				// header offers no Reparent at all -- unlike the NPC behaviour panel's skeleton,
				// there is no way to move a bone once added, so in practice they cannot drift
				// from the public API.
				//
				// Children is needed for the parent-before-child ORDER to exist as a separate
				// fact; but note the forward-pass resolution above does NOT use it -- it walks
				std::vector<BoneIndex> Children;
			};

			// The three parallel vectors below are indexed by the same BoneIndex as Pose::Bones,
			// which is what makes the whole evaluation path index-and-go.
			std::vector<Bone> m_Bones;
			// Parallel to m_Bones: the local transform of each bone at rest.
			//
			// Kept separately from m_RestPose, which holds the same data. DEFECT (duplicated
			// state): two members that must agree, with SetRestPose as the only thing that
			// keeps them in step. A caller that mutates a Pose obtained from a non-const path
			// -- there is none, GetRestPose returns const& -- would not desynchronise them,
			// so it is currently safe, but it is two copies of the same data.
			std::vector<BoneTransform> m_RestTransforms;
			// Parallel again: the cached inverse bind matrix per bone.
			//
			// Sized by CaptureBindMatrices, and empty until that is called -- which is the
			// state GetInverseBindMatrix's identity fallback exists for.
			std::vector<glm::mat4> m_InverseBindMatrices;

			// Name lookup for authoring, not for the per-frame path (which
			// indexes directly). Keeping it out of Evaluate is what keeps a
			// frame from paying a hash lookup per bone.
			// The name index, and the pre-existing comment gives the rule that keeps it out of
			// the hot path: it is for AUTHORING, not per-frame evaluation, which indexes
			// directly.
			//
			// So a frame pays no hash lookup per bone. The cost is that it is maintained
			// eagerly -- AddBone and BuildFromNames both populate it -- so a skeleton that is
			// never queried by name still pays the hashing at construction.
			std::unordered_map<std::string, BoneIndex> m_BoneNameMap;

			// The rest pose, duplicating m_RestTransforms as noted at that member.
			Pose m_RestPose;
			// The identity used as GetInverseBindMatrix's fallback.
			//
			// A MEMBER rather than a function-local static, so there is one instance per
			// Skeleton -- 64 bytes each. DEFECT (memory): a shared file-scope or function-local
			// static would serve every skeleton. The member form is safe for returning a
			// reference, since it outlives any call.
			//
			// DEFECT (per-instance cache, no sharing): with many characters this is wasted
			// memory, and a static const would be equally correct because a reference to an
			// identity matrix is never mutated through this accessor.
			glm::mat4 m_Identity = glm::mat4(1.0f);
		// The end of the class.
		};

	// The inner namespace, closed with a trailing comment naming it.
	} // namespace Animation

// The outer namespace, likewise.
} // namespace Wasteland
