#include "wlpch.h"

// PCH first: this project compiles every translation unit against a shared
// precompiled header, and MSVC errors on a source file that omits it.
#include "MeshLoader.h"

// std::min / std::max, used in RecomputeBounds and nowhere else.
#include <algorithm>
// std::array<float,3> is the working type for positions, normals and
// generated normals throughout this file. Chosen over a bare float[3]
// so it can be passed by const reference into the keying lambda and
// returned by value from the faceNormal lambda without a manual copy.
#include <array>
// std::sqrt and std::llround. llround is specifically the RENDERING of
// the quantization step, not of the float math -- see makeKey.
#include <cmath>
// int64_t for the quantized position key, uint32_t for the index
// buffer. Indices are pushed as uint32_t because that is what the GPU
// upload path declares; the vertex count of any real asset fits.
#include <cstdint>
// strtol, in ParseFaceToken. Deliberately not std::stoi -- see the
// rejection rationale at the call site.
#include <cstdlib>
#include <fstream>
#include <sstream>
// The per-position normal bucket. Hashing on a quantized integer
// triple rather than on floats is what lets the key be compared with
// a plain operator== and no epsilon anywhere.
#include <unordered_map>

namespace Wasteland {

	namespace {

		// OBJ face references are 1-based and may carry three slash-separated
		// indices: v, v/vt, v//vn, or v/vt/vn. Only the first two matter here
		// (position and normal); the texture index is parsed and discarded
		// because the loader always emits (0,0) UVs.
		struct FaceRef
		{
			// 0 doubles as "not set" because OBJ indices are 1-based, so a real
			// face reference can never legitimately be 0. That makes the zero
			// sentinel safe without carrying a separate bool alongside it.
			int Position = 0; // 0 = unset
			// Same 0 sentinel. Stays 0 when the token carried no normal field,
			// or when that field failed to parse -- both of which send the
			// corner down the generated-normal path rather than erroring.
			int Normal = 0;
		};

		// Parses one corner of an "f" line: v, v/vt, v//vn, or v/vt/vn.
		// Returns false only for a malformed POSITION index. A malformed or
		// absent normal is tolerated and silently becomes "generate one", so
		// an asset that ships no normals at all still imports.
		bool ParseFaceToken(const std::string &token, FaceRef &out, std::string &outError)
		{
			// Split on '/'. A face like "f 1 2 3" has one field; "f 1/1/1 2/2/2 3/3/3"
			// has three.
			// npos here means the bare "v" form -- position only, no other fields
			// to consider, which is why the normal parse below is guarded.
			size_t firstSlash = token.find('/');
			std::string posPart = (firstSlash == std::string::npos)
									  ? token
									  : token.substr(0, firstSlash);

			// Guards the "" and "/" degenerate forms. Without this the strtol
			// below would report "non-numeric" for what is really a structural
			// mistake, and the error text would send the author looking at the
			// wrong cause.
			if (posPart.empty())
			{
				outError = "face has an empty position index";
				return false;
			}

			// atoi would silently return 0 for garbage, turning a typo into a
			// confusing "index out of range" much later. strtol lets us
			// reject the bad token where it is.
			// strtol writes the first UNCONSUMED character here. Comparing it
			// against the string start detects "no digits at all" -- the check
			// that atoi cannot perform, and the whole reason for using strtol.
			char *end = nullptr;
			// Base 10 spelled out: OBJ has no hex or octal face references, and
			// leaving the base to default to 0 would read a leading "0" as octal
			// and silently change the value of an index like "010".
			long value = std::strtol(posPart.c_str(), &end, 10);
			// Two distinct failures share one branch. end == start means no
			// digits were consumed at all (garbage); value == 0 means digits did
			// parse but produced zero, which for a 1-based index is out of range
			// by definition. Both are fatal, both are reported identically.
			if (end == posPart.c_str() || value == 0)
			{
				outError = "face has a non-numeric position index: '" + posPart + "'";
				return false;
			}

			// Negative indices are relative to the end of the current vertex
			// list. Rejected rather than implemented: getting them wrong is
			// silent, and they are vanishingly rare in exported assets.
			// DEFECT (documented, not fixed): a negative OBJ index means "count
			// back from the end of the vertex list", which is part of the format
			// and is rejected outright here. Rejecting is the SAFE choice -- the
			// alternative is silently using the wrong vertex -- but it means an
			// asset from a tool that emits negative indices fails to import
			// instead of importing. The header documents this as deliberate.
			if (value < 0)
			{
				outError = "negative face indices are not supported";
				return false;
			}

			// Narrowed long -> int with no range check. An index above INT_MAX
			// would wrap; the resulting negative value is then rejected by the
			// < 1 test at the call site, so the wrap is caught rather than used
			// -- though the error text would then name a nonsense index.
			out.Position = (int)value;

			// Only the "v/..." forms have more than one field left to parse.
			if (firstSlash != std::string::npos)
			{
				// A second slash means the 3-field form v/vt/vn. Absent means the
				// 2-field form, whose one remaining field is taken as the normal.
				size_t secondSlash = token.find('/', firstSlash + 1);
				// DEFECT (documented, not fixed): this substring is the field
				// BETWEEN the two slashes. In the OBJ format that position holds the
				// TEXTURE index -- the normal is the THIRD field, after the second
				// slash. Verified by tracing the arithmetic above on a real token:
				// "12/40/6" gives firstSlash=2, secondSlash=5, so the extracted text
				// is "40" (the vt index) and the true normal index 6 is never read.
				// Consequences, in order of severity:
				//  - For the standard v/vt/vn form the normal index is wrong. If the
				//    vt index happens to fall within normals.size() the WRONG normal
				//    is applied silently; otherwise the face falls to the generated
				//    path. Neither is diagnosable from the outside.
				//  - For v//vn the middle field is empty, so this branch is skipped
				//    entirely and the face is generated-normal. That is why the bug
				//    is invisible on v//vn assets and on 2-field v/vn assets, where
				//    the single remaining field really is the normal.
				//  - The comment above this line claims the texture index is
				//    "parsed and discarded"; in the 3-field form it is parsed and
				//    MISUSED. The stale comment and the code disagree.
				std::string normPart =
					(secondSlash == std::string::npos)
						? token.substr(firstSlash + 1)
						: token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
				// Empty means the v//vn form. Skipping is right there: the normal for
				// that corner is legitimately unknown and gets generated later from
				// the face geometry, which is better than failing the import.
				if (!normPart.empty())
				{
					// "v//vn" puts an empty texture index in the middle; the
					// normal is still the third field.
					char *nend = nullptr;
					// Same strtol-not-atoi choice as the position field. There is no
					// negative check here, unlike the position path -- but a negative
					// long narrowed to int stays negative and is rejected by the >= 1
					// test at the call site, so it cannot reach the normals vector.
					long nvalue = std::strtol(normPart.c_str(), &nend, 10);
					// A malformed normal index is dropped rather than reported. That
					// asymmetry with the position field is deliberate: a bad position
					// makes the mesh unbuildable, a bad normal only makes it ugly, and
					// the generated-normal path is a strictly better answer than a
					// failed import.
					if (nend != normPart.c_str() && nvalue != 0)
						out.Normal = (int)nvalue;
				}
			}

			// Success. out.Normal may still be 0 -- callers must read that as
			// "generate one", never as "normal index zero", which would be an
			// invalid OBJ index.
			return true;
		}

	} // namespace

	// AABB over positions only. Normals and the unused TexCoord play no
	// part in framing a model. Public in the header so a test can rebuild
	// the bounds after mutating Vertices directly.
	void MeshData::RecomputeBounds()
	{
		// An empty mesh gets a zeroed, degenerate box rather than an
		// inverted or uninitialized one. IsValid() already reports false in
		// this state, so the bounds are never meaningfully consulted -- zero
		// is chosen because infinities would propagate into any editor
		// framing code that forgot to check IsValid() first.
		if (Vertices.empty())
		{
			for (int i = 0; i < 3; ++i)
			{
				BoundsMin[i] = 0.0f;
				BoundsMax[i] = 0.0f;
			}
			// Nothing to bound. Early return so the seeding loop below can index
			// Vertices[0] unconditionally.
			return;
		}

		// Seed both min and max from the FIRST vertex; every later vertex is
		// then folded in. Seeding from an identity value instead would be
		// wrong on the max side -- a mesh lying entirely in negative space
		// would report BoundsMax = 0.
		//
		// (this is the seeding loop; the identically spelled loop below is
		// the fold, and insert_comments anchors them in source order)
		for (int i = 0; i < 3; ++i)
		{
			// Both bounds start equal, so the very first comparison is a no-op
			// and the first vertex is correctly counted exactly once.
			BoundsMin[i] = Vertices[0].Position[i];
			BoundsMax[i] = Vertices[0].Position[i];
		}

		// Range-for over the DE-INDEXED vertex array. Indices are not
		// consulted: every emitted corner is a real vertex, so scanning
		// Vertices covers exactly the positions that will be drawn.
		for (const MeshVertex &v : Vertices)
		{
			for (int i = 0; i < 3; ++i)
			{
				// Componentwise min/max is the per-axis fold that turns N points into
				// a box. std::min rather than an if/else so that a NaN in a position
				// propagates into the bound instead of being silently discarded --
				// a NaN bound is a visible parse bug, a dropped NaN is an invisible
				// one.
				BoundsMin[i] = std::min(BoundsMin[i], v.Position[i]);
				// The max half of the fold; see the note on BoundsMin above.
				BoundsMax[i] = std::max(BoundsMax[i], v.Position[i]);
			}
		}
	}

	// The filesystem-free entry point. Everything below is written
	// against an istringstream rather than a file, which is what lets a
	// test assert on triangle counts, normals and winding with no disk
	// and no GL context.
	bool ParseOBJFromString(const std::string &text, const std::string &virtualPath,
						   MeshData &outMesh, std::string &outError)
	{
		// Output is cleared FIRST so a failed parse cannot leave the caller
		// holding a half-built mesh from a previous successful one. Without
		// this, an early `return false` on a bad index would leave stale
		// geometry that the caller has no reason to distrust, because the
		// function did report failure.
		outMesh.Vertices.clear();
		// Indices are regenerated in lockstep with Vertices during
		// triangulation, so a stale index buffer would desynchronise from the
		// vertex buffer on any partial failure.
		outMesh.Indices.clear();
		// Recorded even on the failure paths, because an import error is far
		// more useful when it names the asset that produced it.
		outMesh.SourcePath = virtualPath;

		// Raw "v" records, kept positionally aligned with their OBJ indices
		// for the whole parse. This cannot be a streaming structure: the
		// range check in the "f" branch requires every referenced position to
		// exist ALREADY, which is the v-before-f ordering every exporter
		// emits.
		std::vector<std::array<float, 3>> positions;
		// Raw "vn" records, likewise positional. NOT re-normalized on load:
		// exporters emit unit normals, and normalizing here would cost a
		// sqrt per corner for no benefit on well-formed input.
		std::vector<std::array<float, 3>> normals;
		// Unused, but tracked so a face referencing a nonexistent vt is
		// ignored rather than misparsed as a normal index.
		// DEFECT (documented, not fixed): written once, read never. FaceRef
		// has no texture field at all, and every vertex emitted below leaves
		// TexCoord at its (0,0) default. So this vector costs an allocation
		// and a stream extraction per "vt" line for no benefit, and a face
		// referencing a nonexistent vt index is not diagnosed -- it is
		// simply ignored.
		// The comment above this line claims tracking vt is what keeps a bad
		// texture index from being "misparsed as a normal index". In the
		// 3-field form it IS currently misparsed as a normal index; see the
		// ParseFaceToken defect note. The stated mitigation does not exist.
		std::vector<std::array<float, 2>> texcoords;

		// Keyed on quantized position, so triangles sharing a corner average
		// their normals together. Quantized (not exact float) because exporters
		// routinely emit a position as 0.1 from one face and 0.100000001 from
		// the next; exact matching would silently leave every normal flat.
		// Bucket key for smooth-shading accumulation. A fixed-size integer
		// triple rather than a hash of the floats, because operator== must be
		// exact and total, and quantizing up front is what makes "these two
		// corners are the same point" decidable at all.
		struct NormalKey
		{
			// int64_t, not int: a coordinate of 200 units times the 100000
			// quantization already reaches 2e7, and the product is taken in
			// float, whose exact-integer range stops at 2^24.
			int64_t X, Y, Z;
			// Exact triple compare, safe precisely BECAUSE the components are
			// already quantized integers. Comparing the underlying floats would
			// need an epsilon and would still merge or split borderline
			// positions depending on the order they happened to arrive in.
			bool operator==(const NormalKey &o) const { return X == o.X && Y == o.Y && Z == o.Z; }
		};
		struct NormalKeyHash
		{
			// Hash combiner seeded from std::hash<int64_t> so the three axes are
			// mixed. The 0x9e3779b9 constant is the odd 32-bit approximation of
			// the golden ratio used by boost::hash_combine -- a standard choice,
			// not an arbitrary magic number, because it spreads sequential keys.
			size_t operator()(const NormalKey &k) const
			{
				// X seeds; Y and Z fold in below. Only the order matters for
				// determinism, which this file needs: re-parsing the same OBJ must
				// produce the same buckets, or the emitted vertex order changes.
				size_t h = std::hash<int64_t>{}(k.X);
				// boost-style mix: xor in a salted hash, then rotate. The shifts are
				// what stop the low bits -- constant across many keys -- from
				// dominating the bucket index.
				h ^= std::hash<int64_t>{}(k.Y) + 0x9e3779b9 + (h << 6) + (h >> 2);
				// Same mix for Z. Three axes need three rounds because xor alone is
				// commutative, which without the shifting terms would make the hash
				// symmetric under any permutation of the axes.
				h ^= std::hash<int64_t>{}(k.Z) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};
		// Position -> summed generated normals. Deliberately NOT cleared per
		// face: a corner's average is only final once every face touching it
		// has been read, which is what the second pass at the end of this
		// function is for.
		std::unordered_map<NormalKey, std::array<float, 3>, NormalKeyHash> normalAccumulator;

		// Grid spacing 1e-5: tight enough that genuinely distinct corners do
		// not collide, loose enough to absorb the float noise exporters emit
		// when the same corner is written twice from different faces.
		//
		// DEFECT (documented, not fixed): the product is taken in FLOAT, and
		// float32 represents consecutive integers exactly only up to 2^24
		// (~1.6e7). A coordinate beyond ~167 units times this constant
		// exceeds that, so the quantized value starts skipping grid points:
		// two positions 1e-5 apart can then land on the same bucket and
		// silently share an averaged normal, or on different buckets and lose
		// smoothing. Multiplying in double would remove this; it is not
		// changed here.
		constexpr float kPositionQuantization = 100000.0f; // ~1e-5 units
		// Shared by the accumulation site and the final resolve pass, so both
		// sides agree on what "the same position" means by construction
		// rather than by two copies of the same arithmetic that could drift.
		// Captures kPositionQuantization by reference -- constexpr, so this
		// is a compile-time constant rather than a load.
		auto makeKey = [&](const std::array<float, 3> &p) {
			NormalKey k;
			// llround, not a truncating cast: a bare (int64_t) cast maps -0.5e-5
			// and +0.5e-5 -- effectively the same position -- to DIFFERENT
			// buckets, splitting a smooth surface down a seam. Rounding to
			// nearest makes the grid symmetric about zero.
			k.X = (int64_t)std::llround(p[0] * kPositionQuantization);
			// Same for Y. Subject to the same float-precision ceiling noted at
			// kPositionQuantization.
			k.Y = (int64_t)std::llround(p[1] * kPositionQuantization);
			// Same for Z. Subject to the same float-precision ceiling.
			k.Z = (int64_t)std::llround(p[2] * kPositionQuantization);
			return k;
		};

		// The whole input is parsed as a stream rather than from a file
		// handle -- that substitution is the entire reason this function
		// exists alongside LoadOBJ.
		std::istringstream stream(text);
		// Reused across iterations; getline overwrites it rather than
		// allocating per line.
		std::string line;
		// Incremented before use so every outError below reports a 1-based
		// line number matching what an editor displays.
		int lineNumber = 0;
		// Faces seen, so a file with faces but no geometry can be reported
		// precisely instead of just "no mesh".
		// Faces seen, tracked separately from Vertices.size() because the two
		// diverge: a file can declare faces and still produce no triangles,
		// and those two cases warrant different error text at the end.
		int faceCount = 0;

		// A single forward pass. OBJ does not require v records to precede
		// the f records that use them, but this implementation does -- the
		// range check in the "f" branch rejects a forward reference rather
		// than buffering for it.
		while (std::getline(stream, line))
		{
			// Counted before any error can be reported, so all error text is
			// 1-based and consistent.
			++lineNumber;

			// Strip a trailing CR so a file authored on Windows does not leave
			// '\r' glued to the last token of every line.
			// getline splits on \n but leaves any \r behind, so a file authored or
			// converted on Windows would otherwise glue \r onto its last token.
			// "\r" fails stream extraction, which surfaces as a malformed-record
			// error rather than as the line-ending artifact it actually is.
			if (!line.empty() && line.back() == '\r')
				// Drop the CR. Once per line, rather than trimming every token later.
				line.pop_back();

			// Blank lines and comments. Only a # in column 0 counts, so an
			// INDENTED comment falls through to the unhandled-keyword path and
			// is skipped there instead -- harmless, but it is skipped for a
			// different reason than it appears to be.
			if (line.empty() || line[0] == '#')
				continue;

			// A fresh stream per line, so extraction position never carries over
			// between records. Slightly wasteful versus parsing the buffer
			// directly, and worth it because it gets whitespace handling and
			// number parsing for free.
			std::istringstream lineStream(line);
			// The first token selects the record type. Only four keywords are
			// handled; everything else reaches the end of the chain and is
			// ignored.
			std::string keyword;
			lineStream >> keyword;

			// Vertex position. A malformed one is FATAL rather than skipped: a
			// vertex that cannot be read makes every face referencing it
			// unresolvable, so there is nothing to salvage by continuing.
			if (keyword == "v")
			{
				// Zero-initialized so a partially numeric line ("v 1 2") leaves p[2]
				// at a defined 0 rather than reading indeterminate memory. The
				// function returns false either way, but the failure is then
				// deterministic rather than merely likely.
				std::array<float, 3> p{0.0f, 0.0f, 0.0f};
				// Exactly three components required. Trailing fields -- the optional
				// homogeneous weight in "v x y z w" -- are left unread and discarded
				// when lineStream dies, which is the intended handling of that
				// extension rather than an oversight.
				if (!(lineStream >> p[0] >> p[1] >> p[2]))
				{
					// Fatal for the reason above. The message names the line but not the
					// offending text; the token itself is not echoed.
					outError = "line " + std::to_string(lineNumber) + ": malformed vertex";
					return false;
				}
				// "v x y z w" carries a weight; the remaining stream is
				// intentionally ignored.
				// Appended, so OBJ index N is positions[N-1]. That 1-based/0-based
				// offset is the reason for the -1 at every read site below.
				positions.push_back(p);
			}
			// Normal record, stored positionally so it matches its OBJ index.
			else if (keyword == "vn")
			{
				// Zero-initialized for the same reason as the position array above.
				std::array<float, 3> n{0.0f, 0.0f, 0.0f};
				// A malformed vn is fatal, unlike a malformed normal INDEX inside a
				// face token: here the record itself is unreadable, so any index
				// pointing at it resolves to the WRONG vector rather than to none.
				if (!(lineStream >> n[0] >> n[1] >> n[2]))
				{
					// Fatal. As above, the token is not echoed.
					outError = "line " + std::to_string(lineNumber) + ": malformed normal";
					return false;
				}
				// Positional, matching the OBJ index the face tokens will reference.
				normals.push_back(n);
			}
			// Texture coordinate. Parsed and then never used -- see the defect
			// note at the texcoords declaration.
			else if (keyword == "vt")
			{
				// Two components, not three: OBJ's vt record is u,v[,w], and the
				// optional third is not read.
				std::array<float, 2> t{0.0f, 0.0f};
				// DEFECT (documented, not fixed): the extraction result is
				// discarded. A malformed vt -- "vt x" with one number, or "vt" with
				// none -- is stored as (0,0) and never reported, which is
				// inconsistent with the v and vn branches immediately above, where
				// the same malformation aborts the load. Since nothing reads
				// texcoords the entry is inert either way, but the silent acceptance
				// is a real inconsistency, not a design choice.
				lineStream >> t[0] >> t[1];
				// The only write to texcoords anywhere in the codebase.
				texcoords.push_back(t);
			}
			// Face. The only record kind that emits geometry.
			else if (keyword == "f")
			{
				// Incremented on entry, before parsing. A face that fails to parse
				// still counts, but parse failures return early and so never reach
				// the diagnostic this counter feeds -- the count only ever reflects
				// faces that fully succeeded.
				++faceCount;

				// Collect the whole polygon before triangulating. An n-gon is
				// fan-triangulated from its first vertex, which is correct for
				// convex faces (the overwhelming majority) and is the same
				// choice every mainstream importer makes.
				// The whole polygon is collected BEFORE anything is emitted. That
				// ordering is the reason a face is atomic: a bad index on the last
				// corner cannot leave the first n-2 corners already in the mesh.
				std::vector<FaceRef> refs;
				// Reused per corner; extraction into it avoids a per-token allocation.
				std::string token;
				// Remaining tokens on the line are the polygon's corners. Whitespace
				// delimited, which is how the corner count -- and therefore the
				// triangle count -- is implied.
				while (lineStream >> token)
				{
					// Default-constructed: Position and Normal both start at the 0
					// sentinel, which ParseFaceToken overwrites on success.
					FaceRef ref;
					// Scoped to this corner. Constructed per corner rather than hoisted
					// out of the loop because the error path returns immediately, so it
					// costs nothing on the common success path.
					std::string err;
					// One bad corner rejects the entire face -- consistent with the
					// all-or-nothing rule stated above the refs declaration.
					if (!ParseFaceToken(token, ref, err))
					{
						// Prefixed with the line number that ParseFaceToken does not know
						// about; the two halves compose into one message.
						outError = "line " + std::to_string(lineNumber) + ": " + err;
						return false;
					}
					// Appended in authored order. That order becomes the fan order below,
					// so it also determines the winding of every emitted triangle.
					refs.push_back(ref);
				}

				// Fewer than three corners is a degenerate face, not a polygon
				// needing special handling. Rejected rather than skipped: dropping it
				// would leave a hole the author cannot see.
				if (refs.size() < 3)
				{
					outError = "line " + std::to_string(lineNumber) +
								": face has fewer than 3 vertices";
					return false;
				}

				// Resolve and validate every reference BEFORE emitting any
				// geometry. Doing it up front means a bad index on the last
				// triangle of a face cannot leave half a polygon in the mesh.
				// A corner with its OBJ indices already turned into data. Split from
				// FaceRef so the emit loop never re-checks bounds: by the time a
				// Resolved exists, its Position is known to be in range.
				struct Resolved
				{
					// COPIED from the positions vector, not referenced -- that vector can
					// grow on a later line, which would invalidate a reference into it.
					std::array<float, 3> Position;
					// Meaningful only when HasNormal is true. Never read otherwise, which
					// is what lets it be left without an explicit initializer.
					std::array<float, 3> Normal;
					// Distinguishes "no normal supplied" from "supplied, and it is the
					// zero vector". Needed because a legitimate normal CAN be zero, so
					// the float contents cannot answer the question on their own.
					bool HasNormal = false;
				};
				// Sized once per face, below, rather than growing per corner.
				std::vector<Resolved> resolved;
				// Exact reservation -- the size is already known, so this is the one
				// reserve in this file that cannot over- or under-allocate, and the
				// loop below provably cannot reallocate.
				resolved.reserve(refs.size());

				// Resolve and validate EVERY corner before emitting any geometry.
				// That ordering is the entire point of the resolved/Resolved pair.
				for (const FaceRef &ref : refs)
				{
					// OBJ indices are 1-based, so index 0 is invalid and the
					// valid range is 1..positions.size().
					// The 1-based range check. The < 1 half also catches the 0 sentinel
					// from ParseFaceToken and any negative index that survived the
					// narrowing to int. The cast to size_t is safe because || short-
					// circuits: ref.Position < 1 is tested first.
					//
					// FATAL, unlike the normal-index check further down -- these two
					// similar-looking guards deliberately use different policies.
					// outError below reports positions.size() as well as the offending
					// index, because an author needs to know how many vertices WERE
					// defined to judge whether they forgot a "v" line or mistyped one.
					if (ref.Position < 1 || (size_t)ref.Position > positions.size())
					{
						// Assembled across three source lines; the count and the offending
						// index are both included so the message is actionable on its own.
						outError = "line " + std::to_string(lineNumber) +
									": vertex index " + std::to_string(ref.Position) +
									" out of range (" + std::to_string(positions.size()) +
									" vertices defined)";
						return false;
					}

					// Local, then copied into the vector. Not emplaced via a constructor
					// call, so the struct's default member initializers apply.
					Resolved r;
					// The -1 is the 1-based to 0-based conversion. Safe: the range check
					// above already proved this index is within the vector.
					r.Position = positions[ref.Position - 1];

					// A face mixing vertices with and without normals is
					// common (a hard-edged export). Falling back to a
					// generated normal for the bare ones keeps the mesh
					// consistent rather than mixing lighting models.
					// An out-of-range normal index is ABSORBED, not fatal -- the opposite
					// policy from the position check above. The reasoning is defensible
					// (a bad normal only makes the mesh ugly, and the generated path is
					// a better answer than a failed import), but these are two separate
					// decisions and should not be read as one consistent rule.
					//
					// Combined with the ParseFaceToken field bug, this is the mechanism
					// by which a normals-bearing OBJ silently degrades into a
					// generated-normals mesh instead of reporting anything.
					if (ref.Normal >= 1 && (size_t)ref.Normal <= normals.size())
					{
						// -1 for the same 1-based reason as Position above. Not normalized --
						// taken exactly as the exporter wrote it. Falls inside the same
						// bounds check as the line above.
						r.Normal = normals[ref.Normal - 1];
						// Set only in this branch, so the struct default of false is what a
						// corner lacking a normal carries into the emit loop.
						r.HasNormal = true;
					}
					resolved.push_back(r);
				}

				// Face normal for the generated-normal path: Newell's method.
				// Robust for non-planar polygons where a cross product of two
				// edges gives a badly-scaled result.
				// Newell's method over the WHOLE polygon, not per triangle. For a
				// convex n-gon it returns what cross(p1-p0, p2-p0) would, but without
				// the badly-scaled result a cross product gives on a non-planar or
				// sliver polygon.
				//
				// Returns +Y for a degenerate polygon whose accumulated normal
				// cancels -- see the length test below. +Y is chosen because it is
				// also the default normal the MeshVertex struct itself carries.
				auto faceNormal = [](const std::vector<Resolved> &poly) {
					// Accumulated in DOUBLE, narrowed once at the end. Newell sums many
					// cross terms and float32 loses enough of them to tilt the result on
					// a large polygon; the whole cost is one 3-element vector.
					std::array<double, 3> n{0.0, 0.0, 0.0};
					// One term per edge. The modulo wrap below closes the polygon, so
					// the edge from poly[n-1] back to poly[0] is INCLUDED -- that closing
					// edge is what makes the terms cancel correctly.
					for (size_t i = 0; i < poly.size(); ++i)
					{
						// Edge start. A reference into the Resolved, so neither array is
						// copied per iteration.
						const auto &a = poly[i].Position;
						// Edge end. The modulo is why the polygon is walked as a ring rather
						// than as n-1 separate segments.
						const auto &b = poly[(i + 1) % poly.size()].Position;
						// Newell term for X: (y_i - y_j)(z_i + z_j). The sum/difference form
						// is what makes the method area-weighted -- each term is the area
						// swept by one edge about the axis, so they add coherently.
						// Cast to double BEFORE the multiply, so the product never rounds in
						// float; the subtraction itself stays in float, which is harmless
						// because its inputs are already float.
						n[0] += (double)(a[1] - b[1]) * (double)(a[2] + b[2]);
						// Newell term for Y: (z_i - z_j)(x_i + x_j). Same form, next axis.
						n[1] += (double)(a[2] - b[2]) * (double)(a[0] + b[0]);
						// Newell term for Z: (x_i - x_j)(y_i + y_j).
						n[2] += (double)(a[0] - b[0]) * (double)(a[1] + b[1]);
					}
					// Magnitude computed in double, narrowed once to float. Note what
					// this magnitude IS: the accumulator holds position differences, so
					// its length scales with the polygon's AREA, not with 1. The 1e-8
					// test below is therefore an area threshold in disguise, not a
					// "is this already unit length" check.
					float length = (float)std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					// Default +Y, taken only when the computed normal is degenerate. The
					// alternative -- leaving zeros -- would put a zero vector in the
					// vertex buffer and normalize to NaN downstream.
					std::array<float, 3> out{0.0f, 1.0f, 0.0f};
					// Epsilon guard against a zero-area polygon. 1e-8 sits far below any
					// real area and far above float denormal noise.
					if (length > 1e-8f)
					{
						// Normalized in float, matching the MeshVertex::Normal element type.
						// Y and Z follow with identical arithmetic.
						out[0] = (float)n[0] / length;
						out[1] = (float)n[1] / length;
						out[2] = (float)n[2] / length;
					}
					// The face normal, later stored directly on the vertex AND accumulated
					// into that position's bucket for the smoothing pass.
					return out;
				};

				// Emit the polygon as a triangle fan.
				// Fan triangulation from corner 0: (0,1,2), (0,2,3), (0,3,4)...
				// Starting at 1 rather than 0 is what keeps corner 0 shared by every
				// triangle. Correct for convex faces; a concave n-gon fans
				// incorrectly, which is a known and accepted limitation shared with
				// most OBJ importers.
				for (size_t i = 1; i + 1 < resolved.size(); ++i)
				{
					// Pointers, not copies: a fan triangle is three of the n corners, and
					// copying three of these per triangle to emit them once would be
					// waste. resolved[0] is safe unconditionally -- refs.size() >= 3 is
					// guaranteed by the check above.
					const Resolved *tri[3] = {&resolved[0], &resolved[i], &resolved[i + 1]};

					// The fallback normal for corners lacking an authored one.
					//
					// DEFECT (documented, not fixed): computed once per TRIANGLE even
					// though it depends only on the polygon -- faceNormal walks all n
					// corners every time. Redundant work for n-gons, free for triangles
					// (the common case). Hoisting this above the fan loop would compute
					// it once per face. Correctness is unaffected either way.
					std::array<float, 3> fallback = faceNormal(resolved);
					// Whether THIS TRIANGLE is fully authored. Decided for the whole
					// triangle rather than per corner, so the three corners of one
					// triangle cannot end up mixing authored and generated normals --
					// which would shade as a hard edge inside a single triangle.
					bool anyMissingNormal = false;
					for (int k = 0; k < 3; ++k)
					{
						// The missing-normal test. Scans all three corners and stops at the
						// first gap, because a single gap already makes the triangle
						// inconsistent under the rule stated above.
						if (!tri[k]->HasNormal)
						{
							// Set, not accumulated -- the scan breaks immediately after.
							anyMissingNormal = true;
							break;
						}
					}

					for (int k = 0; k < 3; ++k)
					{
						// Default-constructed per corner, so Position/Normal/TexCoord start at
						// their declared defaults. This is why TexCoord is (0,0) for every
						// vertex with no explicit assignment anywhere.
						MeshVertex v;
						// Copied componentwise rather than assigned as a whole array. Y and Z
						// follow with identical statements.
						v.Position[0] = tri[k]->Position[0];
						v.Position[1] = tri[k]->Position[1];
						v.Position[2] = tri[k]->Position[2];

						// The authored-normal path. The second half of the condition is
						// REDUNDANT: anyMissingNormal false already means all three corners
						// have normals, tri[k] included. Harmless, and it keeps the check
						// self-contained against a future change to the scan.
						if (!anyMissingNormal && tri[k]->HasNormal)
						{
							// The exporter's normal, used verbatim and NOT re-normalized. Y and Z
							// follow with identical statements.
							// (inside the authored branch above)
							v.Normal[0] = tri[k]->Normal[0];
							v.Normal[1] = tri[k]->Normal[1];
							v.Normal[2] = tri[k]->Normal[2];
						}
						else
						{
							// Accumulate into the per-position bucket so a
							// vertex shared by several faces ends up with an
							// averaged normal -- smooth shading -- instead of
							// being shaded flat once per face.
							// Position-keyed, NOT corner-keyed. That is what makes smoothing work
							// at all: two corners of different faces at the same position resolve
							// to the same bucket and therefore share one averaged normal.
							NormalKey key = makeKey(tri[k]->Position);
							// operator[] default-constructs to {0,0,0} on first touch and returns
							// a reference thereafter. The reference is taken rather than a copy
							// precisely so the += below accumulates across faces.
							auto &acc = normalAccumulator[key];
							// A SUM of unit face normals, normalized once in the resolve pass
							// rather than averaged here -- so the resolve pass needs only the
							// direction and the magnitude carries no meaning. Y and Z follow.
							acc[0] += fallback[0];
							acc[1] += fallback[1];
							acc[2] += fallback[2];
							// The vertex is ALSO given the face normal immediately, so it holds a
							// sane value before the resolve pass. If the accumulated direction
							// later degenerates to zero length this is the value that survives:
							// the resolve pass skips the write rather than dividing. Y and Z
							// follow with identical statements.
							v.Normal[0] = fallback[0];
							v.Normal[1] = fallback[1];
							v.Normal[2] = fallback[2];
						}

						// OBJ UV origin is bottom-left, matching GL's texture
						// space, so TexCoord is already correct at (0,0). The
						// loader does not sample texture indices yet.
						// One vertex per triangle corner -- the mesh is fully de-indexed,
						// which is what lets Indices be the trivial 0,1,2,3... sequence the
						// header describes.
						// TexCoord is never assigned, so it stays (0,0) -- consistent with
						// the header's claim that OBJ's bottom-left UV origin matches GL's.
						outMesh.Vertices.push_back(v);
						// Appended immediately after its vertex, so the two vectors stay
						// index-aligned by construction and no separate remap is needed. The
						// -1 converts the post-push size to the new vertex's own index.
						outMesh.Indices.push_back((uint32_t)outMesh.Vertices.size() - 1);
					}
				}
			}
			// Everything else (o, g, s, usemtl, mtllib, and the 'l' polyline
			// that is not real OBJ) is intentionally ignored.
		}

		// Resolve the accumulated generated normals. Done in a second pass
		// because a vertex's normal is only final once every face touching it
		// has been read.
		// The second pass. Necessary because a corner's averaged normal is
		// only final once every face touching that position has been read --
		// which was not knowable at emit time. This is why normalAccumulator
		// is never cleared per face.
		for (MeshVertex &v : outMesh.Vertices)
		{
			// Recomputed from the STORED vertex position rather than carried along
			// from the emit loop, so a vertex that received an authored normal is
			// looked up too -- and harmlessly misses the bucket.
			NormalKey key = makeKey({v.Position[0], v.Position[1], v.Position[2]});
			// find, NOT operator[]: a miss here must not insert a zero bucket,
			// which would then normalize to NaN below and poison a vertex that
			// never needed a generated normal in the first place.
			auto it = normalAccumulator.find(key);
			// No bucket means this corner came from a fully authored face, so its
			// normal is already correct and is left exactly as the exporter wrote
			// it.
			if (it == normalAccumulator.end())
				// Skips the rest of this iteration only; the next vertex is unaffected.
				continue;

			// Bound by reference: this is the accumulated sum stored during
			// emission, not a copy of it.
			//
			// DEFECT (documented, not fixed): this is the sum over every face
			// touching the position, including faces that also contributed an
			// AUTHORED normal. A hard-edged mesh -- some faces with normals, some
			// without -- therefore smooths across a crease it should preserve,
			// because the generated sides contribute their face normal to the
			// same bucket as the authored ones. Nothing downstream can tell the
			// two apart.
			const std::array<float, 3> &acc = it->second;
			// A vertex whose accumulated normal cancels to near zero (a
			// degenerate pocket, or a position shared by opposing faces)
			// would normalize to NaN. Keep the current normal instead.
			// Same area-scale caveat as in faceNormal: this is a sum of unit
			// vectors, so the magnitude scales with HOW MANY faces touched the
			// position, not with 1.
			float length = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
			// Same epsilon, same reason. A position shared by opposing faces can
			// sum to near zero; dividing that would produce NaN or inf, which then
			// propagate into every vertex of the mesh.
			//
			// There is deliberately NO else branch: on a degenerate sum the face
			// normal assigned at emit time is KEPT, which is why that assignment
			// exists rather than leaving the default.
			if (length > 1e-8f)
			{
				// The averaged, normalized normal replaces the per-face value set at
				// emit time. Y and Z follow with identical statements.
				v.Normal[0] = acc[0] / length;
				v.Normal[1] = acc[1] / length;
				v.Normal[2] = acc[2] / length;
			}
		}

		// Two distinct empty-mesh causes, distinguished by faceCount because
		// they call for different fixes: a file with no faces at all is
		// simply not a mesh, whereas a file WITH faces produced nothing,
		// which almost always means the v records came after the f records.
		if (outMesh.Vertices.empty())
		{
			// Faces were seen but produced no triangles. Since a valid face always
			// emits at least one triangle, reaching here means no face ever
			// parsed -- and because a parse failure returns early with its own
			// message, this branch is effectively unreachable. It is a defensive
			// net, not a live diagnostic path.
			// (the error text below says as much in its own words)
			if (faceCount > 0)
			{
				outError = "faces were declared but produced no triangles -- "
						   "check that the vertex data precedes them";
			}
			// No faces at all -- an empty or non-mesh file. A different problem
			// with a different fix, which is why it is not folded into the branch
			// above.
			else
			{
				outError = "no geometry found (no 'v'/'f' records)";
			}
			// Fatal, and safe to do so with the mesh left EMPTY: every parse
			// failure above returns immediately, before any vertex is emitted,
			// so there is no partial geometry a caller could mistake for good.
			return false;
		}

		// Every index must be in range, or the GPU draw reads garbage. Cheap
		// to assert here and expensive to debug as a visual glitch.
		// The final belt-and-braces index check. Triangulation writes both
		// vectors in lockstep, so this CANNOT fail today -- it exists so that
		// a future change to the emit loop fails here, loudly and cheaply,
		// rather than as an out-of-range GPU draw that reads garbage memory.
		for (uint32_t idx : outMesh.Indices)
		{
			// Out of range would mean the draw reads past the vertex buffer, which
			// on the GPU is undefined behaviour rather than a crash. Cheap to
			// assert here, very expensive to debug as a visual glitch.
			if (idx >= outMesh.Vertices.size())
			{
				// Prefixed "internal error" because by this point the fault belongs
				// to this file rather than to the asset -- which is the useful signal
				// for whoever has to debug it.
				outError = "internal error: index out of range after triangulation";
				return false;
			}
		}

		// Bounds computed LAST, from the final vertex buffer. Computing them
		// earlier would miss any vertex emitted after that point.
		outMesh.RecomputeBounds();
		// Success. Both containers are non-empty (guaranteed by the check
		// above) and fully index-aligned.
		return true;
	}

	// The thin filesystem wrapper. All parsing lives in
	// ParseOBJFromString so it can be tested without a disk.
	bool LoadOBJ(const std::string &filepath, MeshData &outMesh, std::string &outError)
	{
		// Opened BINARY. On Windows that suppresses text-mode CRLF
		// translation -- which is why the line loop above has to strip the CR
		// itself. Reading in text mode here would make that strip dead code
		// on Windows and necessary on Linux, i.e. inconsistent either way.
		std::ifstream file(filepath, std::ios::binary);
		// Checked explicitly rather than relying on a later failure: an
		// unopened stream would otherwise read as "no geometry found", a
		// misleading error for what is really a missing file.
		if (!file.is_open())
		{
			// Echoes the path, since this is the one error an author can act on
			// without looking at the file contents.
			outError = "could not open '" + filepath + "'";
			return false;
		}

		// Slurps the whole file into memory, then hands it to the parser as a
		// string. Costs a full copy of the asset; buys a single parsing
		// implementation shared with the string entry point.
		std::ostringstream buffer;
		buffer << file.rdbuf();
		// The path is passed as BOTH the buffer source and the virtual path,
		// so SourcePath is meaningful and the parser stays the single owner
		// of every parse error message.
		return ParseOBJFromString(buffer.str(), filepath, outMesh, outError);
	}

} // namespace Wasteland
