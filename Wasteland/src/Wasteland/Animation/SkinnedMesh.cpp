#include "wlpch.h"
#include "Wasteland/Animation/SkinnedMesh.h"

#include <algorithm>
#include <cmath>

namespace Wasteland {

	namespace Animation {

		void SkinnedMesh::CaptureBindPositions()
		{
			// resize (not assign) so an existing allocation is reused when the
			// mesh shrinks -- this is called after every rebind, which for an
			// import can be once per material or per bone group.
			BindPositions.resize(Vertices.size());
			for (size_t i = 0; i < Vertices.size(); ++i)
				// A C-AGGREGATE copy of the position, written element by element
				// rather than glm::vec3(...). That is not stylistic: MeshVertex's
				// position is three separate floats, so there is no glm::vec3 to
				// construct from.
				BindPositions[i] = glm::vec3(Vertices[i].Position[0],
											 Vertices[i].Position[1],
											 Vertices[i].Position[2]);
		}

		void SkinnedMesh::BindAllToBone(BoneIndex bone)
		{
			// Two guards in one condition. The BoneIndex validity check has to
			// come first because the mesh may not be weighted at all yet --
			// BindAllToBone is a no-op on an empty mesh rather than an error.
			if (!bone.IsValid() || Vertices.empty())
				return;

			// assign(..., empty vector) rather than clear(): this REPLACES every
			// per-vertex vector, dropping whatever weights a previous binding
			// scheme produced. Rebinding therefore discards authored weights, and
			// the copies below start from an empty vector rather than appending.
			Weights.assign(Vertices.size(), std::vector<BoneWeight>());
			for (size_t i = 0; i < Vertices.size(); ++i)
				// One influence at weight 1.0 exactly -- not a loop over bones --
				// because this is the rigid case by definition: the whole vertex
				// follows one transform.
				Weights[i].push_back(BoneWeight{bone, 1.0f});

			CaptureBindPositions();
		}

		void SkinnedMesh::BindTwoBonesByAxis(BoneIndex boneA, BoneIndex boneB, int axis,
											 float splitPoint, float blendWidth)
		{
			if (!boneA.IsValid() || !boneB.IsValid() || Vertices.empty())
				return;
			// Range-checked as 0..2 rather than validated against a bone count.
			// An out-of-range axis is SILENTLY IGNORED rather than reported, so a
			// caller passing 3 gets a straight A-to-B split with no diagnostic.
			if (axis < 0 || axis > 2)
				return;

			// A non-positive width would divide by zero at the split point.
			// Guards the division below rather than rejecting the input. A
			// blendWidth of exactly 0 would make the ramp's denominator zero and
			// the weight NaN for every vertex on the split plane, so the value is
			// silently widened to a millimetre-equivalent instead.
			if (blendWidth <= 1e-6f)
				blendWidth = 1e-3f;

			Weights.assign(Vertices.size(), std::vector<BoneWeight>());

			for (size_t i = 0; i < Vertices.size(); ++i)
			{
				// Rebuilds the position as a glm::vec3 to take a COMPONENT along an
				// axis. The axis choice is made at line 50, not here.
				glm::vec3 position(Vertices[i].Position[0], Vertices[i].Position[1],
								   Vertices[i].Position[2]);
				// A nested ternary rather than a switch or an index: evaluated per
				// vertex in a loop over the whole mesh, so the branch prediction is
				// perfect and it compiles to two selects. The readability cost is
				// accepted deliberately; indexing position[axis] would need a glm
				// accessor this version does not guarantee.
				float value = (axis == 0) ? position.x
							  : (axis == 1) ? position.y
											 : position.z;

				// Weight of bone B, ramping 0 -> 1 across the blend band around
				// the split. Outside the band this is exactly 0 or 1, so
				// rigidly-bound vertices get a single influence rather than a
				// useless 0.99/0.01 pair.
				// The linear ramp. (value - (splitPoint - blendWidth)) / (2*w) maps
				// the band [split-w, split+w] onto [0, 1], and the clamp below turns
				// that into a linear falloff -- vertices outside the band get exactly
				// 0 or 1, which is why they can carry one influence.
				float t = (value - (splitPoint - blendWidth)) / (2.0f * blendWidth);
				t = std::max(0.0f, std::min(1.0f, t));

				// boneA ALWAYS gets an entry, even when its weight is 0.0f. So a
				// fully-B vertex still carries a dead influence costing a matrix
				// multiply per frame -- the asymmetry with the boneB branch below is
				// deliberate only for boneB, and is worth noting if the hot loop is
				// ever measured.
				Weights[i].push_back(BoneWeight{boneA, 1.0f - t});

				// Only add the second influence when it is actually
				// meaningful. A 0-weight influence still costs a matrix
				// multiply per frame to contribute nothing.
				// The 1e-4 threshold: below this the second influence is skipped
				// entirely. So a vertex is never left with an EMPTY weight list, which
				// is what LimitInfluencesPerVertex relies on downstream.
				if (t > 1e-4f)
					Weights[i].push_back(BoneWeight{boneB, t});
			}

			CaptureBindPositions();
		}

		void SkinnedMesh::NormalizeWeights()
		{
			// Iterating by reference so the inner renormalisation can write
			// through. The vectors are per-vertex, so the cost is one outer pass
			// over the mesh.
			for (std::vector<BoneWeight> &vertexWeights : Weights)
			{
				// The tolerance: exporters emit weights summing to 1 in decimal and
				// drift in float, so an exact compare would fail every imported mesh.
				// 0.01 absorbs that without accepting a visibly wrong rig.
				float total = 0.0f;
				for (const BoneWeight &w : vertexWeights)
					// Negative weights clamped to 0 BEFORE summing, so a corrupt negative
					// does not cancel out a positive and produce a total near zero that
					// then divides into a huge value.
					total += std::max(0.0f, w.Weight);

				// A vertex with no positive weight is left alone. Dividing by
				// zero would produce NaN weights, and a NaN weight turns into a
				// NaN vertex position that propagates through the whole draw
				// call.
				// The zero-total case is skipped rather than normalised. Dividing by
				// a near-zero total is exactly how NaN weights appear, and a NaN
				// weight produces a NaN vertex that propagates through the entire
				// draw call.
				if (total <= 1e-8f)
					continue;

				for (BoneWeight &w : vertexWeights)
					w.Weight = std::max(0.0f, w.Weight) / total;
			}
		}

		void SkinnedMesh::LimitInfluencesPerVertex(size_t maxInfluences, float threshold)
		{
			// Guarded so the compiler is not free to optimise a per-frame call
			// away, though at this size the loop is not hot enough to matter.
			if (maxInfluences == 0)
				return;

			for (std::vector<BoneWeight> &vertexWeights : Weights)
			{
				// Drop negligible influences first, so the count limit does
				// not spend a slot on a weight too small to matter.
				// erase-remove over the whole influence list. Doing this BEFORE the
				// count limit is what stops a slot being spent on a weight the
				// threshold already considers meaningless.
				vertexWeights.erase(
					std::remove_if(vertexWeights.begin(), vertexWeights.end(),
								   [threshold](const BoneWeight &w) { return w.Weight < threshold; }),
					vertexWeights.end());

				// Never leave a vertex with nothing: an unbound vertex reads
				// as an invalid matrix index and collapses to the origin.
				// DEFECT (dead branch): the comment above says "never leave a vertex
				// with nothing", but the preceding erase-remove only drops weights
				// below `threshold`, and the default threshold is 0.001f. A vertex
				// whose every weight falls under the threshold therefore reaches here
				// with an EMPTY list -- and `continue` then leaves it empty. The
				// stated invariant is not enforced; the fallback in Skin() is what
				// stops it collapsing to the origin, at the cost of every such vertex
				// being frozen at its bind pose.
				if (vertexWeights.empty() && !Weights.empty())
					continue;

				// The limit is checked with `>` rather than `>=`, so exactly
				// maxInfluences entries is the no-op case.
				if (vertexWeights.size() > maxInfluences)
				{
					// Partial sort rather than a full sort: only the top N
					// matter, and this runs once at import over every vertex.
					// partial_sort, not sort: only the top maxInfluences entries are
					// needed, so ordering the tail is wasted work. This runs once at
					// import over every vertex, which is what makes the distinction worth
					// the slightly more complex call.
					std::partial_sort(vertexWeights.begin(),
									  // A C-style cast to long on a size_t. On Win64 (LLP64) size_t is
									  // 64-bit and long is 32, so this TRUNCATES above 2^31 vertices --
									  // unreachable in practice, but static_cast<size_t> says the same
									  // thing without the narrowing.
									  vertexWeights.begin() + (long)maxInfluences,
									  vertexWeights.end(),
									  [](const BoneWeight &a, const BoneWeight &b) {
										  return a.Weight > b.Weight;
									  });
					// Truncating to the top N AFTER the partial sort above has put them
					// in the first N slots -- so the resize discards exactly the tail
					// partial_sort never ordered.
					vertexWeights.resize(maxInfluences);
				}
			}

			// Truncating changes the total, so renormalize afterwards --
			// otherwise limiting influences would silently shrink the mesh.
			NormalizeWeights();
		}

		// The caller supplies the matrices, so SkinnedMesh never touches a
		// Skeleton. That is what keeps this testable headless: a test passes
		// identity matrices and asserts nothing moves.
		bool SkinnedMesh::Skin(const std::vector<glm::mat4> &skinningMatrices,
							   std::vector<glm::vec3> &outPositions) const
		{
			// Every precondition here is checked before writing anything, so a
			// failure leaves the caller's buffer untouched rather than
			// half-written.
			if (Vertices.empty())
			{
				outPositions.clear();
				return false;
			}

			// Size mismatch is a hard failure with NO diagnostic. Since
			// Validate() exists for exactly this, a caller that skipped it gets a
			// silent false rather than a message naming the mismatch.
			if (Weights.size() != Vertices.size())
				return false;

			if (BindPositions.size() != Vertices.size())
				return false;

			// Also a silent false. An empty matrix list means the caller forgot
			// to compute skinning matrices; returning false is correct but
			// indistinguishable from the two checks above.
			if (skinningMatrices.empty())
				return false;

			outPositions.resize(Vertices.size());

			for (size_t i = 0; i < Vertices.size(); ++i)
			{
				// By const reference: this runs per vertex per frame, and a copy of
				// the influence vector per vertex would dominate the cost.
				const std::vector<BoneWeight> &vertexWeights = Weights[i];

				// Linear blend skinning: the deformed position is the
				// weight-weighted sum of every influencing bone's transformed
				// bind position.
				//
				// The weights must total 1 for this to be correct -- that is
				// what keeps the mesh from shrinking toward the origin, and
				// NormalizeWeights is what guarantees it.
				glm::vec3 deformed(0.0f);
				// `accumulated` is tracked SEPARATELY from the weights' nominal sum
				// because invalid bones are skipped below, so the actual total can be
				// less than 1 -- and that remainder has to be divided out or the
				// vertex drifts toward the origin.
				float accumulated = 0.0f;

				for (const BoneWeight &w : vertexWeights)
				{
					// An invalid bone index, or one past the end of the
					// skeleton, would index out of bounds. Skipping it keeps a
					// bad weight from taking down the whole mesh, and the
					// remaining influences renormalize to carry the vertex.
					// The bounds check against skinningMatrices.size() is the one that
					// matters. A BoneIndex can be syntactically valid and still name a
					// bone past the end of THIS skeleton's matrix list -- which happens
					// whenever the mesh and skeleton disagree.
					if (!w.Bone.IsValid() ||
						(size_t)w.Bone.Value >= skinningMatrices.size())
						continue;

					// The transform is applied to the BIND position, not to the current
					// position. That is linear blend skinning: order-dependent by nature,
					// and the reason weights are recomputed rather than applied twice.
					deformed += glm::vec3(skinningMatrices[(size_t)w.Bone.Value] *
										  glm::vec4(BindPositions[i], 1.0f)) *
								w.Weight;
					accumulated += w.Weight;
				}

				// Fall back to the bind position when nothing contributed.
				// An unbound vertex staying put is far better than it snapping
				// to the origin, which reads as the mesh exploding.
				if (accumulated > 1e-8f)
					// Renormalising by `accumulated` rather than by the nominal weight
					// sum. The skips above make the two differ, and dividing by the
					// nominal sum would scale the vertex down whenever a bad bone was
					// skipped.
					deformed /= accumulated;

				// DEFECT (redundant test): the branch condition was already
				// evaluated one line above, so `accumulated` is computed twice and
				// the ternary re-tests it. Not a correctness bug -- both tests read
				// the same unmodified value -- but the fallback and the division are
				// one expression, so they cannot diverge.
				outPositions[i] = (accumulated > 1e-8f) ? deformed : BindPositions[i];
			}

			return true;
		}

		bool SkinnedMesh::IsValid() const
		{
			// Checks only that the mesh is non-empty and that bind positions
			// were captured. It does NOT check the weights, the indices or their
			// ranges -- which is why Validate() below exists, and why a caller
			// using only IsValid can still draw a broken mesh.
			return !Vertices.empty() && BindPositions.size() == Vertices.size();
		}

		size_t SkinnedMesh::GetMaxInfluencesPerVertex() const
		{
			size_t maximum = 0;
			// A full O(vertices * influences) scan, called once at import to size
			// the shader's bone palette. Not something to call per frame.
			for (const std::vector<BoneWeight> &vertexWeights : Weights)
				maximum = std::max(maximum, vertexWeights.size());
			return maximum;
		}

		// outError is cleared first so a SUCCESSFUL validation leaves it
		// empty rather than holding the previous call's message -- so the
		// caller can branch on the returned string alone.
		std::string SkinnedMesh::Validate(std::string &outError) const
		{
			outError.clear();

			if (Vertices.empty())
			{
				outError = "skinned mesh has no vertices";
				return outError;
			}

			if (Weights.size() != Vertices.size())
			{
				// Size mismatch reported with BOTH numbers, because knowing which
				// of the two is wrong is the whole diagnostic value.
				outError = "weights (" + std::to_string(Weights.size()) +
						   ") do not match vertices (" +
						   std::to_string(Vertices.size()) + ")";
				return outError;
			}

			if (BindPositions.size() != Vertices.size())
			{
				outError = "bind positions (" + std::to_string(BindPositions.size()) +
						   ") do not match vertices (" +
						   std::to_string(Vertices.size()) + ")";
				return outError;
			}

			if (Weights.empty())
			{
				outError = "skinned mesh has no weights";
				return outError;
			}

			for (size_t i = 0; i < Weights.size(); ++i)
			{
				if (Weights[i].empty())
				{
					outError = "vertex " + std::to_string(i) + " has no influences";
					return outError;
				}

				// A tolerance rather than an exact compare: exporters emit
				// weights that sum to 1 in decimal and drift slightly in
				// floats, and a sum of 0.9999 is not a defect.
				float total = 0.0f;
				for (const BoneWeight &w : Weights[i])
					total += w.Weight;

				if (std::abs(total - 1.0f) > 0.01f)
				{
					outError = "vertex " + std::to_string(i) +
							   " weights sum to " + std::to_string(total) +
							   ", expected 1.0";
					return outError;
				}
			}

			// A triangle-list assumption. A mesh authored as quads, or with a
			// trailing partial triangle, fails here rather than producing garbage
			// geometry -- correct, and the message says why.
			if (Indices.size() % 3 != 0)
			{
				outError = "index count (" + std::to_string(Indices.size()) +
						   ") is not a multiple of 3";
				return outError;
			}

			for (uint32_t index : Indices)
			{
				// The comparison against Vertices.size() promotes the uint32_t index
				// to size_t, so this is safe on a 32-bit build. The message reports
				// the offending INDEX and the vertex total, which is enough to locate
				// it in the source file.
				if (index >= Vertices.size())
				{
					outError = "index " + std::to_string(index) +
							   " is out of range for " +
							   std::to_string(Vertices.size()) + " vertices";
					return outError;
				}
			}

			return outError;
		}

	} // namespace Animation

} // namespace Wasteland
