#pragma once

#include "Wasteland/Animation/Skeleton.h"
#include "Wasteland/Renderer/MeshLoader.h"

#include <memory>
#include <string>
#include <vector>

namespace Wasteland {

	namespace Animation {

		// One vertex's influence from one bone.
		struct BoneWeight
		{
			// BoneIndex, not a raw uint32: it carries its own validity flag, and
			// Validate rejects weights naming a bone that does not exist rather
			// than this struct having to encode 'unset' as a magic number.
			BoneIndex Bone;
			// NOT clamped to [0,1] and not even to [0, something]. Skin() applies
			// whatever it finds, so a weight of 5 stretches the mesh. Nothing in
			// this struct defends against it -- NormalizeWeights() is the pass
			// that makes the sum right, and it still allows negative input to be
			// treated as zero rather than rejected.
			float Weight = 0.0f;
		};

		// A skinned mesh: geometry plus the weights that bind it to a
		// skeleton.
		//
		// Kept separate from the GPU buffers for the same reason MeshData is:
		// skinning runs entirely on the CPU here, so it is testable in a
		// headless process with no GL context. A test can build a two-bone
		// "arm" and assert the elbow vertex lands where the bones put it.
		class SkinnedMesh
		{
		public:
			SkinnedMesh() = default;

			// ---- Geometry ----

			// De-indexed geometry, matching MeshData's layout so an OBJ import
			// can be adopted directly.
			// Public and unencapsulated: the authoring tools write into these
			// directly, and every consistency check between Vertices and Weights
			// has to be re-done by hand. That is why Validate() below exists at all.
			std::vector<MeshVertex> Vertices;
			std::vector<uint32_t> Indices;

			// ---- Skinning ----

			// Bones influencing each vertex, and their weights.
			//
			// A vector per vertex rather than a fixed count: most vertices are
			// rigidly bound to one bone, and only vertices in a joint crease
			// need several. A fixed 4 made every vertex pay for the rare case.
			// A vector PER VERTEX: 24 bytes of overhead per vertex even for the
			// rigid common case, but it means a vertex bound to one bone allocates
			// one small vector rather than padding out to a fixed four weights.
			std::vector<std::vector<BoneWeight>> Weights;

			// The rest-pose vertex positions, kept so skinning can compute
			// each output as bindPose + deformed offset.
			//
			// Storing them is cheaper than the alternative -- storing inverse
			// bind matrices per VERTEX rather than per bone -- because the
			// inverse bind matrix is shared by every vertex a bone touches.
			// Rest-pose POSITIONS, not inverse bind matrices. That is a
			// deliberate trade: an inverse bind matrix is 64 bytes per bone while
			// a position is 12 bytes per vertex, and for a mesh with more vertices
			// than bones the per-vertex storage is larger but far simpler. The
			// cost is that Skin() must apply matrices in the other order.
			std::vector<glm::vec3> BindPositions;

			// ---- Construction ----

			// Binds every vertex to a single bone. Produces a rigid mesh --
			// correct for anything that does not bend, and the right starting
			// point before weights are authored.
			void BindAllToBone(BoneIndex bone);

			// Binds vertices to two bones by a blend factor along an axis.
			//
			// The automatic weighting scheme every DCC tool offers on import:
			// vertices below `splitPoint` go to `boneA`, above go to `boneB`,
			// and those within `blendWidth` of the split get a mix. Good enough
			// to make a two-bone limb bend, and it needs no per-vertex
			// authoring at all.
			// `axis` is an int 0/1/2 rather than an enum, and the .cpp bounds
			// checks it by range. An enum would make an out-of-range value a
			// compile error at the call site instead of a silent early return.
			void BindTwoBonesByAxis(BoneIndex boneA, BoneIndex boneB, int axis,
									float splitPoint, float blendWidth);

			// Renormalizes each vertex's weights to sum to 1.
			//
			// Skinned position is a weighted SUM, so weights that do not total
			// 1 shrink or stretch the mesh: a vertex with total weight 0.9
			// pulls 10% toward the origin every frame. Exporters emit weights
			// that sum to 1 in exact arithmetic and drift in floats, so this
			// is a normalization pass, not a luxury.
			void NormalizeWeights();

			// Drops influences whose weight is below `threshold`, keeping at
			// least one per vertex. Bound to four bones per vertex in practice;
			// every extra influence costs a matrix multiply in the hot loop and
			// contributes less than the weight threshold can express.
			// The threshold has a DEFAULT (unlike maxInfluences), because
			// dropping every influence under a fixed fraction is almost always
			// right and callers only tune it when a specific import needs it.
			void LimitInfluencesPerVertex(size_t maxInfluences,
										  float threshold = 0.001f);

			// ---- Deformation ----

			// Writes deformed positions into `outPositions` using
			// `skinningMatrices` (from Skeleton::ComputeSkinningMatrices).
			//
			// Returns false if the mesh, the weights, or the matrix count do
			// not line up -- the caller can then skip the draw instead of
			// reading a vertex bound to a bone that does not exist.
			// CONST, and the reason is in the header above: skinning is pure CPU
			// arithmetic over plain vectors, so it is testable with no GPU and no
			// graphics context. That property is lost the moment this needs a
			// device handle.
			bool Skin(const std::vector<glm::mat4> &skinningMatrices,
					  std::vector<glm::vec3> &outPositions) const;

			// ---- Queries ----

			size_t GetVertexCount() const { return Vertices.size(); }
			bool IsValid() const;
			// Compares SIZES only, so it reports true for a mesh whose weights
			// are the right length but all zero or all pointing at invalid bones.
			// IsValid() below has the same limitation; Validate() is the real
			// check.
			bool HasWeights() const { return Weights.size() == Vertices.size(); }

			// The most influences any vertex has, for sizing a shader's bone
			// palette and for the importer's budget check.
			size_t GetMaxInfluencesPerVertex() const;

			// Returns std::string AND fills outError -- the same value twice.
			// Redundant, and it invites the two to disagree if one is updated and
			// the other is not. A caller reading the return value and ignoring
			// outError gets the same text, so it is safe, just wasteful.
			std::string Validate(std::string &outError) const;

		private:
			// Captures bind positions from the current vertices.
			void CaptureBindPositions();
		};

	} // namespace Animation

} // namespace Wasteland
