#include "wlpch.h"
#include "ProceduralMeshes.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace Wasteland
{
    namespace
    {
        // Memo table keyed on the generator's parameter tuple. The scene
        // submits these meshes every frame per entity; regenerating on each
        // call would re-walk the whole index buffer for geometry that never
        // changes. Entries are never evicted -- the parameter space is bounded
        // by the editor's sliders, so the table stays small.
        template <typename Key>
        // MEMO TABLE, AND IT NEVER EVICTS -- SO IT GROWS WITH EVERY DISTINCT PARAMETER
        // SET THE USER EVER PASSES THROUGH.
        //
        // The rationale above the declaration is that "the parameter space is bounded
        // by the editor's sliders, so the table stays small". The first half is true;
        // the conclusion does not follow. Bounded means FINITE, not SMALL, and the
        // reachable state counts are:
        //    Plane        65
        //    Circle       254      Cylinder  254      Cone  254      IcoSphere  5
        //    Grid      16,384  (128 x 128)
        //    Torus     64,516  (254 x 254)
        //
        // The torus figure is not theoretical: the editor's DragInt controls span
        // 3.0 to 256.0 on both axes, which is exactly the ClampCount domain, so every
        // pair is reachable by dragging the sliders.
        //
        // EACH ENTRY IS A WHOLE MESH. A 256x256 torus is 257^2 = 66,049 vertices and
        // 393,216 indices -- roughly 3.7 MB. Dragging Major Segments from 3 to 256
        // creates 254 permanent entries; sweeping both axes does the same in both
        // dimensions. Nothing is ever freed, so this accumulates across a whole
        // editing session.
        //
        // In practice a user does not sweep 254x254, so this is a slow leak rather
        // than an immediate failure -- but it is unbounded, and the fix is cheap: an
        // LRU bound on Entries, or clearing the cache when a scene is saved or a
        // project is closed. Right now nothing tells the user or the developer it is
        // happening.
        struct MeshCache
        {
            // std::map, NOT unordered_map or vector, and the choice is load-bearing.
            //
            // Get returns a REFERENCE into this container, and that reference has to
            // outlive the call -- callers hold it while the frame is being submitted.
            // std::map guarantees exactly that: node-based storage never invalidates a
            // reference on insert. A vector would reallocate and dangle every reference,
            // and even unordered_map only guarantees stability for references to ELEMENTS
            // (not iterators), so map is the safe choice for a reference-returning cache.
            //
            // Cost is a tree lookup per submission rather than a hash. Given one lookup
            // per entity per frame against a table that is almost always tiny, that is the
            // right trade.
            std::map<Key, ProceduralMesh> Entries;

            template <typename Fn>
            // Memoized lookup: return the cached mesh, or build and store it.
            //
            // THE POINT IS THAT THE SCENE SUBMITS THESE EVERY FRAME PER ENTITY. A cube-
            // shaded plane at 64 subdivisions is thousands of vertices, and regenerating it
            // per entity per frame would walk the entire index buffer for geometry that is
            // identical every time. So generation happens once per distinct parameter set,
            // and everything after is a lookup.
            //
            // `build` is a forwarded callable so the generator stays out of the cache type
            // entirely -- the cache does not need to know which shape it is caching.
            //
            // emplace rather than operator[] so the mesh is built once and moved in, rather
            // than default-constructed and then assigned. The returned reference is into
            // the map node, which is stable.
            //
            // NOT THREAD SAFE. Entries is a plain std::map mutated without any lock, and
            // the file-static caches below are shared process-wide. Two threads submitting
            // the first instance of the same shape would race on both the find and the
            // emplace. Fine while submission is single-threaded off the main thread; not
            // safe as it stands for concurrent submission.
            const ProceduralMesh &Get(const Key &key, Fn &&build)
            {
                auto it = Entries.find(key);
                if (it != Entries.end())
                    return it->second;

                ProceduralMesh mesh = build();
                return Entries.emplace(key, std::move(mesh)).first->second;
            }
        };

        MeshCache<int> g_PlaneCache;
        MeshCache<std::pair<int, int>> g_GridCache;
        MeshCache<int> g_CircleCache;
        MeshCache<int> g_IcoSphereCache;
        MeshCache<int> g_CylinderCache;
        MeshCache<int> g_ConeCache;
        // One cache per generator, keyed on whatever that generator varies.
        //
        // The torus key is a PAIR because the generator takes two independent segment
        // counts, and keying on only one would collide shapes that differ in the other.
        //
        // See the note on eviction at the cache declaration -- this is where that
        // matters most, since a pair key has far more reachable states than a scalar.
        MeshCache<std::pair<int, int>> g_TorusCache;

        // Clamps user/slider-driven counts so a stray 0 or negative can't
        // produce a degenerate mesh (and so nothing can overflow the batcher's
        // MaxVertices/MaxIndices budget through subdivision explosion).
        // Clamps slider-driven counts, for two separate reasons.
        //
        // CORRECTNESS: a zero or negative subdivision count would produce a degenerate
        // mesh -- an empty vertex array, or a division by zero in the step calculation
        // -- and the lower bound prevents that before it can reach the batcher.
        //
        // BUDGET: the upper bound is what keeps a mesh from exceeding the renderer's
        // MaxVertices/MaxIndices. Without it a large count would generate a mesh too
        // big to append, and DrawProceduralPrimitive DROPS such a mesh silently rather
        // than drawing it -- so an unclamped value here is an invisible object, not an
        // error.
        //
        // Written as two ifs rather than glm::clamp so this header-free helper stays
        // usable without GLM in scope.
        int ClampCount(int value, int minValue, int maxValue)
        {
            if (value < minValue)
                return minValue;
            if (value > maxValue)
                return maxValue;
            return value;
        }

        // Appends one quad as two triangles sharing the a-c diagonal.
        //
        // Ordering (a, b, c) and (a, c, d) is the standard split; the shared diagonal
        // is what keeps the two triangles interpolating consistently across the quad,
        // since both reference the same a and c vertices with the same parameterisation.
        //
        // Winding is not the caller's concern -- OrientTrianglesOutward fixes every
        // triangle in the finished mesh.
        void AddQuad(ProceduralMesh &mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            mesh.Indices.push_back(a);
            mesh.Indices.push_back(b);
            mesh.Indices.push_back(c);
            mesh.Indices.push_back(a);
            mesh.Indices.push_back(c);
            mesh.Indices.push_back(d);
        }

        // Flips any triangle wound against its own vertex normals.
        //
        // Every generator here computes exact outward-facing normals in closed
        // form (radial for the sphere/cylinder, the analytic torus tube
        // direction, the cone's slope normal). That makes the normals a
        // reliable orientation oracle, so winding is derived from them instead
        // of being hand-ordered per primitive -- a class of bug that is easy to
        // introduce and invisible until the shape renders inside-out.
        //
        // Flat primitives are covered by the same test: their normals are all
        // +Y, so a reversed quad is still caught.
        // DERIVES WINDING FROM THE ANALYTIC NORMALS rather than trusting the emitter.
        //
        // This is the best idea in the file. Every generator here computes exact
        // outward normals in closed form -- radial for sphere and cylinder, the
        // analytic tube direction for the torus, the slope normal for the cone -- and
        // those normals are therefore a RELIABLE ORIENTATION ORACLE. So winding is
        // derived from them instead of being hand-ordered per primitive.
        //
        // The class of bug this removes is easy to introduce and invisible until it
        // ships: a quad emitted with its corners in the wrong order, or a generator
        // whose parameter sweep reverses direction, produces a shape that renders
        // inside-out under back-face culling. Hand-maintained winding for seven
        // generators with caps, poles and seam columns is exactly the kind of thing
        // that rots.
        //
        // Running this unconditionally after generation also means a generator cannot
        // get winding wrong by accident -- it only has to get POSITIONS and NORMALS
        // right, which the closed-form normal already constrains.
        //
        // Cost is one cross product and two dots per triangle, once per distinct mesh
        // (memoized), so it is not on the per-frame path.
        void OrientTrianglesOutward(ProceduralMesh &mesh)
        {
            for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
            {
                const uint32_t ia = mesh.Indices[i];
                const uint32_t ib = mesh.Indices[i + 1];
                const uint32_t ic = mesh.Indices[i + 2];

                const glm::vec3 &a = mesh.Positions[ia];
                const glm::vec3 &b = mesh.Positions[ib];
                const glm::vec3 &c = mesh.Positions[ic];

                const glm::vec3 edge1 = b - a;
                const glm::vec3 edge2 = c - a;

                // Degenerate (zero-area) facets carry no orientation; leave them.
                const glm::vec3 faceCross = glm::cross(edge1, edge2);
                // Degenerate triangles are skipped, because a zero-area facet has no
                // orientation to get wrong.
                //
                // These are real here: the pole rows of the sphere and cylinder collapse to a
                // point, and the generators avoid emitting triangles for them -- but a near
                // degenerate sliver can still appear at a cap or seam. Without this test,
                // normalize() on a zero vector would produce NaN and every comparison against
                // it would be false, silently leaving those triangles' winding unfixed.
                //
                // The squared-length form avoids a sqrt and the threshold is correspondingly
                // a squared length -- 1e-20 is an area-squared, i.e. an edge length around
                // 1e-10.
                if (glm::dot(faceCross, faceCross) < 1e-20f)
                    continue;

                const glm::vec3 faceNormal = glm::normalize(faceCross);
                // The orientation reference is the SUM of the three vertex normals.
                //
                // Averaging (or summing -- only the sign matters) is right for a closed convex
                // shape, where the corner normals bracket the true face normal. It is also what
                // makes the flat primitives work without a special case: all their normals are
                // +Y, so a reversed quad is caught by the same test.
                //
                // LIMITATION WORTH KNOWING: the sum can approach zero when a triangle spans a
                // large solid angle whose normals cancel, and then the dot product is ~0 and
                // the test is inconclusive -- the winding is simply left as emitted. This is a
                // benign fallback (no corruption, just no correction), but it means the oracle
                // is not universally reliable on shapes with high curvature or concavity, and
                // a torus triangle spanning the hole is the plausible case. A fallback to the
                // mesh centroid for closed shapes would close the gap.
                const glm::vec3 reference =
                    mesh.Normals[ia] + mesh.Normals[ib] + mesh.Normals[ic];

                // Swap the second and third indices when the face points against its own
                // normals.
                //
                // Swapping b and c reverses the triangle's winding in place, which is the
                // minimal correction -- cheaper than rebuilding the index buffer, and it keeps
                // the vertex array untouched so every other generator assumption still holds.
                //
                // The comparison is a strict < 0, so an exactly-perpendicular face (dot == 0)
                // is left alone. That is the same inconclusive case as the degenerate test above.
                if (glm::dot(faceNormal, reference) < 0.0f)
                    std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
            }
        }
    }

    // Unit plane on the XZ plane, facing +Y, on a [-0.5, 0.5] footprint.
    //
    // TWO EMITTERS, NOT ONE. Zero subdivisions take a hand-written single quad;
    // anything higher goes through a general grid loop. The split exists because a
    // grid needs at least one subdivision before it has any cells to index.
    //
    // The two paths do NOT emit vertices in the same corner order -- see the
    // AddQuad notes below -- which is fine only because OrientTrianglesOutward runs
    // at the end of both and derives winding from the normals.
    const ProceduralMesh &ProceduralMeshes::Plane(int subdivisions)
    {
        // Plane subdivisions, 0 to 64.
        //
        // The lower bound of 0 is legal here -- a plane with no subdivisions is a
        // single quad, which is a valid mesh -- unlike the shapes below that need at
        // least three segments to close.
        subdivisions = ClampCount(subdivisions, 0, 64);
        return g_PlaneCache.Get(subdivisions, [subdivisions]()
                                {
            ProceduralMesh mesh;

            // Blender's default Plane is a single quad (0 cuts). The general
            // grid path below needs at least one subdivision to emit indices,
            // so the 0-cut case is handled explicitly rather than degenerating
            // into a vertex with no triangles.
            // SINGLE-QUAD CASE, matching Blender's default Plane (zero cuts).
            //
            // Hand-written rather than folded into the loop because the general path divides
            // by the subdivision count. The vertex order here is chosen to match Blender's
            // own plane corner order, so a mesh round-tripped through a .blend keeps its
            // layout.
            //
            // TexCoords are (0,0), (0,1), (1,1), (1,0) -- note this starts at (0,0) whereas
            // the general path derives UV from position and so also starts there; the
            // correspondence holds because the hand-written corners follow the same
            // (i, j) traversal.
            if (subdivisions == 0)
            {
                mesh.Positions = {{-0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, 0.5f},
                                  {0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f}};
                // All four normals are +Y, so the plane is single-sided and one-sided lit.
                //
                // assign rather than four emplace_backs purely for brevity. The uniformity is
                // also what lets OrientTrianglesOutward's "sum of vertex normals" oracle work
                // on flat primitives with no special case: four +Y vectors sum to +Y, so a
                // reversed quad is detected like any other.
                mesh.Normals.assign(4, glm::vec3(0.0f, 1.0f, 0.0f));
                mesh.TexCoords = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
                // NOTE THE CORNER ORDER DIFFERS FROM THE GRID PATH BELOW, which passes
                // (a, c, d, b).
                //
                // These are the same quadrilateral expressed differently, so on its own this
                // looks like a bug -- and it would be one if winding were taken on trust. Both
                // paths end in OrientTrianglesOutward, which rewrites any triangle facing against
                // its own normals, so the emitted order only has to be geometrically sensible.
                // That is the whole payoff of deriving orientation: generators stop having to
                // agree on a winding convention.
                AddQuad(mesh, 0, 1, 2, 3);
                return mesh;
            }

            const int n = subdivisions + 1;
            // The reciprocal is computed AFTER the zero case returns, and that ordering is
            // load-bearing: a division by subdivisions == 0 would produce inf, and inf * 0
            // is NaN, which would then poison every vertex position in the mesh.
            //
            // Hoisting it out of the inner loop is also why this differs from Grid, which
            // divides per iteration. One reciprocal multiply instead of a divide per vertex
            // is a small win, but it is why the two functions look almost identical without
            // being textually the same.
            const float inv = 1.0f / static_cast<float>(subdivisions);
            const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);

            mesh.Positions.reserve(count);
            mesh.Normals.reserve(count);
            mesh.TexCoords.reserve(count);

            for (int j = 0; j < n; ++j)
            {
                for (int i = 0; i < n; ++i)
                {
                    // -0.5..0.5 so a unit Scale matches the cube's footprint.
                    const float u = static_cast<float>(i) * inv;
                    const float v = static_cast<float>(j) * inv;
                    mesh.Positions.emplace_back(u - 0.5f, 0.0f, v - 0.5f);
                    mesh.Normals.emplace_back(0.0f, 1.0f, 0.0f);
                    mesh.TexCoords.emplace_back(u, v);
                }
            }

            for (int j = 0; j < subdivisions; ++j)
            {
                for (int i = 0; i < subdivisions; ++i)
                {
                    const uint32_t a = static_cast<uint32_t>(j * n + i);
                    const uint32_t b = a + 1;
                    const uint32_t c = a + static_cast<uint32_t>(n);
                    const uint32_t d = c + 1;
                    AddQuad(mesh, a, c, d, b);
                }
            }
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Editor grid, and a near-duplicate of Plane's general path.
    //
    // The two differ only in that this one takes INDEPENDENT X and Y counts. With
    // xSubdivisions == ySubdivisions the bodies produce an identical mesh, and the
    // only code difference is the hoisted reciprocal versus a per-iteration divide.
    //
    // Worth noting because it means a grid and a plane can never disagree -- but it
    // is also duplicated logic that has to be kept in sync. A shared grid emitter
    // taking both counts would remove the duplication without changing behaviour.
    const ProceduralMesh &ProceduralMeshes::Grid(int xSubdivisions, int ySubdivisions)
    {
        xSubdivisions = ClampCount(xSubdivisions, 1, 128);
        ySubdivisions = ClampCount(ySubdivisions, 1, 128);
        return g_GridCache.Get({xSubdivisions, ySubdivisions}, [xSubdivisions, ySubdivisions]()
                               {
            ProceduralMesh mesh;
            const int nx = xSubdivisions + 1;
            const int ny = ySubdivisions + 1;
            const size_t count = static_cast<size_t>(nx) * static_cast<size_t>(ny);

            mesh.Positions.reserve(count);
            mesh.Normals.reserve(count);
            mesh.TexCoords.reserve(count);

            for (int j = 0; j < ny; ++j)
            {
                for (int i = 0; i < nx; ++i)
                {
                    const float u = static_cast<float>(i) / static_cast<float>(xSubdivisions);
                    const float v = static_cast<float>(j) / static_cast<float>(ySubdivisions);
                    mesh.Positions.emplace_back(u - 0.5f, 0.0f, v - 0.5f);
                    mesh.Normals.emplace_back(0.0f, 1.0f, 0.0f);
                    mesh.TexCoords.emplace_back(u, v);
                }
            }

            for (int j = 0; j < ySubdivisions; ++j)
            {
                for (int i = 0; i < xSubdivisions; ++i)
                {
                    const uint32_t a = static_cast<uint32_t>(j * nx + i);
                    const uint32_t b = a + 1;
                    const uint32_t c = a + static_cast<uint32_t>(nx);
                    const uint32_t d = c + 1;
                    // Quad corner order used by BOTH the plane grid path and the Grid generator.
                    //
                    // a is the top-left of a cell, b its right neighbour, c the cell below a, d the
                    // diagonal. Passing (a, c, d, b) walks the quad as two triangles sharing the
                    // a-d diagonal, which is the opposite diagonal from the single-quad path above.
                    //
                    // As with that path, the order is corrected by OrientTrianglesOutward before the
                    // mesh is used, so this is a geometric convenience rather than a winding
                    AddQuad(mesh, a, c, d, b);
                }
            }
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Unit disc on the XZ plane, radius 0.5, facing +Y.
    //
    // A Blender NGON fill is a single n-gon, which most renderers triangulate; this
    // emits an explicit triangle fan from a centre vertex instead. The result is
    // visually identical and avoids handing an n-gon to the tessellator.
    const ProceduralMesh &ProceduralMeshes::Circle(int vertexCount)
    {
        vertexCount = ClampCount(vertexCount, 3, 256);
        return g_CircleCache.Get(vertexCount, [vertexCount]()
                                 {
            ProceduralMesh mesh;
            mesh.Positions.reserve(static_cast<size_t>(vertexCount) + 1);
            mesh.Normals.reserve(static_cast<size_t>(vertexCount) + 1);
            mesh.TexCoords.reserve(static_cast<size_t>(vertexCount) + 1);

            // Blender's Circle with NGON fill is a single n-gon; we emit a
            // triangle fan from a centre vertex so it renders identically.
            mesh.Positions.emplace_back(0.0f, 0.0f, 0.0f);
            mesh.Normals.emplace_back(0.0f, 1.0f, 0.0f);
            mesh.TexCoords.emplace_back(0.5f, 0.5f);

            // Angular step for a full 2*pi sweep.
            //
            // RADIUS IS 0.5, matching the plane and the cube footprint, which is what lets
            // DrawCircle scale the cached unit disc by (r, 1, r) and land on the requested
            // radius. The convention is consistent across every generator here.
            const float step = glm::two_pi<float>() / static_cast<float>(vertexCount);
            for (int i = 0; i < vertexCount; ++i)
            {
                const float angle = step * static_cast<float>(i);
                const float x = glm::cos(angle) * 0.5f;
                const float z = glm::sin(angle) * 0.5f;
                mesh.Positions.emplace_back(x, 0.0f, z);
                mesh.Normals.emplace_back(0.0f, 1.0f, 0.0f);
                // UVs ARE THE POSITION, remapped from [-0.5, 0.5] to [0, 1].
                //
                // For a disc this produces a RADIAL mapping rather than a planar one -- the
                // centre vertex lands at (0.5, 0.5) and the rim traces a circle in UV space. That
                // is the useful behaviour for a disc (a radial gradient maps correctly) and it
                // also means no seam handling is needed: the UV wraps naturally because it is
                // derived from position rather than from an angle.
                //
                // The centre vertex uses the same formula, giving (0.5, 0.5) from x = z = 0, so
                // the fan is continuous.
                mesh.TexCoords.emplace_back(x + 0.5f, z + 0.5f);
            }

            for (int i = 0; i < vertexCount; ++i)
            {
                const uint32_t a = static_cast<uint32_t>(i) + 1;
                // Fan indices, offset by 1 because vertex 0 is the centre.
                //
                // THE MODULO IS WHAT CLOSES THE DISC. The last triangle wraps from the final
                // rim vertex back to vertex 1, so no duplicate seam vertex is needed -- the
                // saving a UV-sphere-style seam column would otherwise cost.
                //
                // Order is (centre, b, a) rather than (centre, a, b); like every other generator
                // here the direction is arbitrary because OrientTrianglesOutward rewrites it.
                const uint32_t b = static_cast<uint32_t>((i + 1) % vertexCount) + 1;
                mesh.Indices.push_back(0);
                mesh.Indices.push_back(b);
                mesh.Indices.push_back(a);
            }
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Geodesic sphere by recursive edge-midpoint subdivision of an icosahedron.
    //
    // Preferred over a UV sphere here because subdivision splits TRIANGLES evenly:
    // every face gains the same number of vertices, so there are no polar pinching
    // and no seam column, and triangle density stays roughly uniform over the whole
    // surface. A UV sphere concentrates vertices at the poles and wastes them at the
    // equator, which is why this shape exists alongside DrawSphere.
    //
    // Cost of that uniformity: 20 * 4^n faces, which is what drives the much tighter
    // clamp at the top of this function.
    const ProceduralMesh &ProceduralMeshes::IcoSphere(int subdivisions)
    {
        // Each level quadruples the triangle count (20 * 4^n). Cap at 4 so a
        // slider drag can't blow past the batcher's 100k-vertex budget.
        // Icosphere subdivisions, 0 to 4 -- a much tighter range than every other
        // generator.
        //
        // Correct for the shape: an icosphere refines by splitting each triangular
        // face into four, so vertex count multiplies by roughly 4 per level. Level 4 is
        // about 5,120 triangles, which is comparable to a sphere at a few dozen
        // segments. The exponential growth is why this bound is 16x tighter than the
        // sphere-equivalent clamps, and why a higher level would need the clamp raised
        // deliberately rather than by copying a number from a neighbouring generator.
        subdivisions = ClampCount(subdivisions, 0, 4);
        return g_IcoSphereCache.Get(subdivisions, [subdivisions]()
                                    {
            // THE GOLDEN RATIO, phi.
            //
            // The 12 vertices below are the Cartesian form of a regular icosahedron: the
            // cyclic permutations of (0, +/-1, +/-phi). Using phi rather than 1 gives the
            // exact Platonic solid -- all edges equal, all faces regular pentagons in the dual
            // -- whereas an arbitrary value such as 2 produces a valid but irregular
            // polyhedron whose faces are not equilateral. Since every triangle below is then
            // subdivided uniformly, a wrong phi would show up as visibly non-uniform triangle
            // sizes rather than as a subtle shading error.
            const float t = (1.0f + glm::sqrt(5.0f)) * 0.5f;

            ProceduralMesh mesh;
            mesh.Positions = {
                {-t, 0.0f, 1.0f}, {t, 0.0f, 1.0f}, {-t, 0.0f, -1.0f}, {t, 0.0f, -1.0f},
                {0.0f, -1.0f, t},  {0.0f, 1.0f, t},  {0.0f, -1.0f, -t}, {0.0f, 1.0f, -t},
                {1.0f, t, 0.0f},  {-1.0f, t, 0.0f},  {1.0f, -t, 0.0f}, {-1.0f, -t, 0.0f}};

            // THE 20 BASE FACES of the icosahedron, hand-listed.
            //
            // These indices are the ONLY place winding is decided by hand: the comment at the
            // refinement step below notes that children inherit their parent's order. Even so,
            // OrientTrianglesOutward runs at the end and would correct them, so this list only
            // has to be geometrically valid, not correctly wound.
            std::vector<uint32_t> indices = {
                0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11,
                1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9,
                4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};

            // Edge-midpoint cache: without it every subdivision duplicates the
            // shared midpoints and the mesh cracks into disconnected shells.
            // Adds the edge midpoint to the position array, normalized back onto the sphere.
            //
            // THE NORMALIZE IS THE WHOLE POINT. A chord midpoint is INSIDE the sphere, not on
            // it, so without this every subdivided vertex would sink toward the interior and
            // the mesh would visibly shrink and crease with each level.
            //
            // Captures mesh by reference and pushes into it directly, returning the new index,
            // so callers do not need to track the growing array.
            auto midpoint = [&mesh](uint32_t a, uint32_t b) {
                const glm::vec3 mid = (mesh.Positions[a] + mesh.Positions[b]) * 0.5f;
                mesh.Positions.push_back(glm::normalize(mid));
                return static_cast<uint32_t>(mesh.Positions.size() - 1);
            };

            for (int level = 0; level < subdivisions; ++level)
            {
                std::vector<uint32_t> refined;
                // Each triangle becomes four, so the new index count is 4/3 of the old.
                //
                // Reserving avoids repeated reallocation while building. Integer division is exact
                // here because the index count is always a multiple of 3.
                refined.reserve(indices.size() * 4 / 3);
                // The edge cache is PER LEVEL, not per mesh, and that is correct.
                //
                // Vertex indices are reassigned every level, so an index pair from level N does
                // not mean the same edge at level N+1. Reusing the cache across levels would
                // return midpoints of unrelated edges.
                std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeCache;

                auto split = [&](uint32_t a, uint32_t b) {
                    // THE EDGE KEY IS CANONICALIZED BY SORTING, and this is the detail that keeps the
                    // mesh watertight.
                    //
                    // Adjacent triangles reference a shared edge in opposite directions -- one as
                    // (a, b), its neighbour as (b, a). Without min/max the two would be different map
                    // keys, each would create its own midpoint, and the surface would split along that
                    // edge. The result is a mesh that looks fine until it is shaded: each face has
                    // slightly different normals on either side of a crack, showing as a dark seam
                    // network across the sphere.
                    //
                    // std::map rather than unordered_map because the key is a pair, which needs an
                    // ordering to compare; the cost is fine here since this runs once per mesh, not per
                    // frame.
                    const std::pair<uint32_t, uint32_t> key(std::min(a, b), std::max(a, b));
                    auto it = edgeCache.find(key);
                    if (it != edgeCache.end())
                        return it->second;
                    const uint32_t m = midpoint(a, b);
                    edgeCache.emplace(key, m);
                    return m;
                };

                for (size_t i = 0; i < indices.size(); i += 3)
                {
                    const uint32_t a = indices[i];
                    const uint32_t b = indices[i + 1];
                    const uint32_t c = indices[i + 2];

                    const uint32_t ab = split(a, b);
                    const uint32_t bc = split(b, c);
                    const uint32_t ca = split(c, a);

                    // Children inherit the parent's vertex order, so winding is
                    // decided entirely by the 20 base faces below.
                    // FOUR CHILD TRIANGLES PER PARENT, and they INHERIT THE PARENT'S VERTEX ORDER.
                    //
                    // Corners only, midpoints only -- no interior vertex, because an icosahedron's
                    // triangles are already equilateral so a face centre adds nothing. That is what
                    // keeps the count at 4x per level rather than growing quadratically.
                    //
                    // The comment notes winding is inherited; combined with OrientTrianglesOutward at
                    // the end, the requirement on this list is only that each triple forms a real
                    // triangle.
                    refined.insert(refined.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
                }
                // swap rather than assign, so the previous level's index array is released with
                // `refined` going out of scope instead of being copied into.
                //
                // The vertex array is deliberately NOT reset between levels -- new midpoints
                // accumulate across all levels, and the unused base vertices simply remain as
                // unreferenced entries. That costs a handful of dead vertices (12 at level 0) and
                // saves reallocating and re-filling the position array each level.
                indices.swap(refined);
            }

            // Blender's Ico Sphere is radius 1 centred on the origin; scale to
            // the engine's half-extent convention.
            // Blender authors the Ico Sphere at radius 1; scale to the engine's half-extent
            // convention, matching the plane, disc and cube footprint.
            //
            // Applied AFTER subdivision, so the midpoints are normalized on a unit sphere and
            // the whole mesh is scaled in one pass at the end -- cheaper and better conditioned
            // than scaling each vertex as it is created.
            constexpr float kRadius = 0.5f;
            for (glm::vec3 &p : mesh.Positions)
                p *= kRadius;

            mesh.Normals.resize(mesh.Positions.size());
            for (size_t i = 0; i < mesh.Positions.size(); ++i)
            {
                // Unit-radius icosphere: the position IS the normal.
                mesh.Normals[i] = glm::normalize(mesh.Positions[i]);
                // EQUIRECTANGULAR (LAT/LONG) UV, and it has a seam the other generators do not.
                //
                // u wraps at the +/-pi discontinuity of atan2, so adjacent vertices across that
                // meridian get u near 1 and near 0. Interpolating between them sweeps the whole
                // texture across one triangle -- a visible smear line down the icosphere. Cylinder
                // and circle avoid this by DUPLICATING a seam column or closing with a modulo; an
                // icosphere indexed as a triangle soup cannot, because the seam vertices would
                // have to be split per face.
                //
                // So a texture on an icosphere shows one meridian artefact. That is inherent to
                // this mapping rather than a bug in it, and worth knowing before choosing a UV
                // projection for a textured icosphere.
                //
                // v runs from 0 at the north pole to 1 at the south, because acos is DECREASING:
                // y = 1 gives acos(1) = 0. Some conventions flip v, so this is worth confirming
                // against a reference texture rather than assumed.
                const float u = 0.5f + std::atan2(mesh.Normals[i].z, mesh.Normals[i].x) / glm::two_pi<float>();
                const float v = std::acos(glm::clamp(mesh.Normals[i].y, -1.0f, 1.0f)) / glm::pi<float>();
                mesh.TexCoords.emplace_back(u, v);
            }

            mesh.Indices = std::move(indices);
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Cylinder, centred on the origin with radius and half-height 0.5, so a uniform
    // unit scale gives a 1x1x1 cylinder matching the cube footprint.
    const ProceduralMesh &ProceduralMeshes::Cylinder(int vertexCount)
    {
        vertexCount = ClampCount(vertexCount, 3, 256);
        return g_CylinderCache.Get(vertexCount, [vertexCount]()
                                   {
            ProceduralMesh mesh;
            // ONE EXTRA COLUMN, which is the seam.
            //
            // The ring runs 0..vertexCount inclusive, so the last column duplicates the first
            // positionally. That duplicate exists purely so UVs can run 0..1 across the
            // circumference and wrap WITHOUT a smear -- the same problem the icosphere has
            // and cannot solve, solved here at the cost of one redundant vertex per ring.
            //
            // Note the comment says "for UVs" rather than for geometry, which is the point:
            // the shape is closed either way, but the TEXTURE would tear without it.
            const int sideCount = vertexCount + 1; // duplicated seam column for UVs
            const float step = glm::two_pi<float>() / static_cast<float>(vertexCount);
            constexpr float kRadius = 0.5f;
            constexpr float kHalfHeight = 0.5f;

            mesh.Positions.reserve(static_cast<size_t>(sideCount) * 2);
            mesh.Normals.reserve(static_cast<size_t>(sideCount) * 2);
            mesh.TexCoords.reserve(static_cast<size_t>(sideCount) * 2);

            for (int i = 0; i < sideCount; ++i)
            {
                // THE MODULO PUTS THE SEAM COLUMN BACK ON TOP OF COLUMN 0.
                //
                // Note the deliberate asymmetry with the UV one line below: POSITION wraps
                // with `i % vertexCount`, while UV runs `i / vertexCount` all the way to 1.0.
                // That is the whole seam mechanism -- the last column shares its position with
                // the first but has the far UV, so the texture wraps cleanly.
                //
                // The modulo rather than just letting the angle reach 2*pi matters. cos and sin
                // are mathematically periodic, but the last column computed at 2*pi lands on
                // very slightly different floats than column 0 at 0.0 -- enough to leave a
                // hairline crack between the final and first columns of the wall. Wrapping the
                // index guarantees the two are bit-identical.
                const float angle = step * static_cast<float>(i % vertexCount);
                const float x = glm::cos(angle) * kRadius;
                const float z = glm::sin(angle) * kRadius;
                const float nx = glm::cos(angle);
                const float nz = glm::sin(angle);

                mesh.Positions.emplace_back(x, -kHalfHeight, z);
                mesh.Normals.emplace_back(nx, 0.0f, nz);
                // v = 0 at the bottom ring, 1 at the top, so the texture runs up the wall.
                //
                // u = i / vertexCount is the wrap described above -- it reaches exactly 1.0 at
                // the seam column, which is what closes the texture around the circumference.
                mesh.TexCoords.emplace_back(static_cast<float>(i) / static_cast<float>(vertexCount), 0.0f);

                mesh.Positions.emplace_back(x, kHalfHeight, z);
                mesh.Normals.emplace_back(nx, 0.0f, nz);
                mesh.TexCoords.emplace_back(static_cast<float>(i) / static_cast<float>(vertexCount), 1.0f);
            }

            // Side wall. Even i is the bottom ring, odd i the top. The ring
            // runs counter-clockwise seen from +Y, so bottom-left ->
            // bottom-right -> top-right winds outward.
            for (int i = 0; i < vertexCount; ++i)
            {
                const uint32_t bl = static_cast<uint32_t>(i) * 2;
                const uint32_t tl = bl + 1;
                const uint32_t br = bl + 2;
                const uint32_t tr = bl + 3;
                // Wall quad: bl and br are the two bottom vertices, tl and tr the top.
                //
                // The layout makes the indices regular -- bottom and top alternate, so column i
                // occupies 2i and 2i+1 and the next column starts at 2i+2. That is why the four
                // corners are bl, bl+1, bl+3, bl+2 rather than a simple +1 stride.
                //
                // NOTE THE COMMENT ABOVE DESCRIBES A DIFFERENT TRAVERSAL -- it says
                // "bottom-left -> bottom-right -> top-right", but the arguments here are
                // bl, tl, tr, br, i.e. bottom-left -> top-left -> top-right. The two are opposite
                // rounds of the same quad. Harmless, because OrientTrianglesOutward derives winding
                // from the normals and would correct either, but the comment does not match the
                // code and should not be trusted when changing this line.
                AddQuad(mesh, bl, tl, tr, br);
            }

            // CAP EMITTER, called twice with opposite sign.
            //
            // Parameterized on the normal sign rather than written out twice, which is what
            // keeps the +Y and -Y caps from drifting apart as they are edited -- a real risk
            // when the two are copy-pasted blocks that only differ in sign.
            //
            // The cap gets its OWN ring of vertices plus a centre rather than reusing the
            // wall's ring. Duplicating them is what lets the cap carry flat (0, normalY, 0)
            // normals while the wall's shared vertices carry radial ones: one vertex cannot
            // have both. It costs vertexCount extra vertices per cap.
            auto addCap = [&](float y, float normalY) {
                const uint32_t centreIndex = static_cast<uint32_t>(mesh.Positions.size());
                mesh.Positions.emplace_back(0.0f, y, 0.0f);
                mesh.Normals.emplace_back(0.0f, normalY, 0.0f);
                mesh.TexCoords.emplace_back(0.5f, 0.5f);

                const uint32_t ringStart = static_cast<uint32_t>(mesh.Positions.size());
                for (int i = 0; i < sideCount; ++i)
                {
                    const float angle = step * static_cast<float>(i % vertexCount);
                    const float x = glm::cos(angle) * kRadius;
                    const float z = glm::sin(angle) * kRadius;
                    mesh.Positions.emplace_back(x, y, z);
                    mesh.Normals.emplace_back(0.0f, normalY, 0.0f);
                    mesh.TexCoords.emplace_back(x + 0.5f, z + 0.5f);
                }

                for (int i = 0; i < vertexCount; ++i)
                {
                    const uint32_t a = ringStart + static_cast<uint32_t>(i);
                    const uint32_t b = ringStart + static_cast<uint32_t>(i + 1);
                    // Ring runs counter-clockwise seen from +Y (x = cos, z = sin), so a
                                    // front-facing (+Y) fan is centre-b-a; the -Y cap is reversed.
                                    // THE CAP WINDING IS SET EXPLICITLY PER SIDE, unlike the wall above.
                                    //
                                    // A triangle fan's facing reverses with the cap's normal, so a single winding
                                    // cannot serve both -- hence the branch, with the order reversed for the -Y cap.
                                    //
                                    // This is belt-and-braces: OrientTrianglesOutward would fix either order from the
                                    // (0, +/-1, 0) normals. Deriving it explicitly here is still the clearer
                                    // expression of intent, and it means the mesh is already correct before that pass.
                                    //
                                    // The -Y branch takes (centre, a, b) where the +Y branch takes (centre, b, a) --
                                    // the same two vertices in opposite rotational order.
                                    if (normalY > 0.0f)
                                    {
                                        mesh.Indices.push_back(centreIndex);
                                        mesh.Indices.push_back(b);
                                        mesh.Indices.push_back(a);
                                    }
                                    else
                                    {
                                        mesh.Indices.push_back(centreIndex);
                                        mesh.Indices.push_back(a);
                                        mesh.Indices.push_back(b);
                                    }
                }
            };

            // Top cap first, then bottom.
            //
            // Order matters only for index assignment, and nothing here depends on which cap
            // comes first -- the orientation pass and the material are both per-vertex. It is
            // noted because vertex indices are positional, so changing this order renumbers
            // everything after it.
            addCap(kHalfHeight, 1.0f);
            addCap(-kHalfHeight, -1.0f);
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Cone, centred on the origin, radius and half-height 0.5, so a uniform unit
    // scale matches the cube footprint -- same convention as the cylinder.
    const ProceduralMesh &ProceduralMeshes::Cone(int vertexCount)
    {
        vertexCount = ClampCount(vertexCount, 3, 256);
        return g_ConeCache.Get(vertexCount, [vertexCount]()
                               {
            ProceduralMesh mesh;
            const int sideCount = vertexCount + 1;
            const float step = glm::two_pi<float>() / static_cast<float>(vertexCount);
            constexpr float kRadius = 0.5f;
            constexpr float kHalfHeight = 0.5f;

            // Cone wall slope: the apex is (0, +halfHeight, 0) and the rim is at
            // radius kRadius on y = -kHalfHeight. The outward normal is the rim
            // direction tilted up by the slope angle, so the side shades as a
            // true cone rather than as vertical strips.
            // CONE WALL SLOPE: radius over FULL height, which is what sets the side normal.
            //
            // The factor 2 is because the apex sits at +halfHeight and the rim at
            // -halfHeight, so the vertical run is the full height. Using the half-height here
            // would halve the slope and tilt every side normal too far from horizontal -- a
            // cone that shades like a much steeper one, brighter at the top than it should
            // be.
            //
            // THE NORMAL IS THE RIM DIRECTION TILTED UP BY THIS SLOPE, which is the whole
            // reason the cone is not a bundle of vertical strips: it is why the shading sweeps
            // continuously from bright at the apex to grazing at the rim. Computing it in
            // closed form rather than by finite-differencing neighbouring triangles also
            // keeps the lighting stable as vertexCount changes.
            const float slope = kRadius / (2.0f * kHalfHeight);
            const float ny = slope;
            const float nScale = 1.0f / glm::sqrt(1.0f + slope * slope);

            mesh.Positions.reserve(static_cast<size_t>(sideCount) + 1);
            mesh.Normals.reserve(static_cast<size_t>(sideCount) + 1);
            mesh.TexCoords.reserve(static_cast<size_t>(sideCount) + 1);

            const uint32_t apex = 0;
            mesh.Positions.emplace_back(0.0f, kHalfHeight, 0.0f);
            mesh.Normals.emplace_back(0.0f, 1.0f, 0.0f);
            mesh.TexCoords.emplace_back(0.5f, 1.0f);

            for (int i = 0; i < sideCount; ++i)
            {
                const float angle = step * static_cast<float>(i % vertexCount);
                const float x = glm::cos(angle) * kRadius;
                const float z = glm::sin(angle) * kRadius;
                mesh.Positions.emplace_back(x, -kHalfHeight, z);
                mesh.Normals.emplace_back(glm::cos(angle) * nScale, ny * nScale, glm::sin(angle) * nScale);
                mesh.TexCoords.emplace_back(static_cast<float>(i) / static_cast<float>(vertexCount), 0.0f);
            }

            for (int i = 0; i < vertexCount; ++i)
            {
                // Apex, then ring i+1 -> ring i: seen from outside the wall,
                // this traverses the rim clockwise and faces away from the axis.
                mesh.Indices.push_back(apex);
                mesh.Indices.push_back(static_cast<uint32_t>(i + 2));
                mesh.Indices.push_back(static_cast<uint32_t>(i + 1));
            }

            // Base cap, facing -Y.
            const uint32_t centreIndex = static_cast<uint32_t>(mesh.Positions.size());
            mesh.Positions.emplace_back(0.0f, -kHalfHeight, 0.0f);
            mesh.Normals.emplace_back(0.0f, -1.0f, 0.0f);
            mesh.TexCoords.emplace_back(0.5f, 0.5f);

            const uint32_t ringStart = static_cast<uint32_t>(mesh.Positions.size());
            for (int i = 0; i < sideCount; ++i)
            {
                const float angle = step * static_cast<float>(i % vertexCount);
                const float x = glm::cos(angle) * kRadius;
                const float z = glm::sin(angle) * kRadius;
                mesh.Positions.emplace_back(x, -kHalfHeight, z);
                mesh.Normals.emplace_back(0.0f, -1.0f, 0.0f);
                mesh.TexCoords.emplace_back(x + 0.5f, z + 0.5f);
            }

            for (int i = 0; i < vertexCount; ++i)
            {
                // Base faces -Y, so the rim runs the opposite way to the top cap.
                mesh.Indices.push_back(centreIndex);
                mesh.Indices.push_back(ringStart + static_cast<uint32_t>(i));
                mesh.Indices.push_back(ringStart + static_cast<uint32_t>(i + 1));
            }
            OrientTrianglesOutward(mesh);
            return mesh; });
    }

    // Torus parameterised by two independent segment counts.
    //
    // DOUBLY PERIODIC SURFACE, so BOTH axes need a seam column -- majorCount and
    // minorCount are each one greater than their segment count. A surface that wraps
    // in two directions needs the wrap handled twice, and missing either one shows as
    // a texture smear along that axis only.
    //
    // The cached radii are 0.375 and 0.125, a tube/ring ratio of exactly 1/3. That
    // ratio is not incidental -- see DrawTorus in Renderer3D.cpp, which rescales this
    // mesh per instance and can only honour the requested MINOR radius in the tube's
    // vertical extent, never radially.
    const ProceduralMesh &ProceduralMeshes::Torus(int majorSegments, int minorSegments)
    {
        // Torus segments, 3 to 256 on both axes.
        //
        // THIS IS THE RANGE THE EDITOR ACTUALLY OFFERS -- the DragInt controls for
        // Major Segments and Minor Segments are both 3.0 to 256.0. That matters for
        // the cache: it means every one of the 254 x 254 = 64,516 possible key pairs
        // is reachable by dragging the sliders, and the cache never evicts. See the
        // note at the MeshCache declaration.
        majorSegments = ClampCount(majorSegments, 3, 256);
        minorSegments = ClampCount(minorSegments, 3, 256);
        return g_TorusCache.Get({majorSegments, minorSegments}, [majorSegments, minorSegments]()
                                {
            ProceduralMesh mesh;
            // Blender's default torus is major 1, minor 0.25, scaled by 0.5 to
            // the engine convention -> outer radius 0.5.
            // AUTHORED RADII: major 0.375, minor 0.125.
            //
            // Blender's default torus is major 1, minor 0.25, scaled by 0.5 to the engine's
            // half-extent convention, which puts the outer radius at 0.5. Written as two
            // constants rather than derived, so the pair can be changed together and the 1/3
            // ratio they imply stays explicit.
            constexpr float kMajorRadius = 0.375f;
            constexpr float kMinorRadius = 0.125f;

            const int majorCount = majorSegments + 1;
            const int minorCount = minorSegments + 1;

            mesh.Positions.reserve(static_cast<size_t>(majorCount) * static_cast<size_t>(minorCount));
            mesh.Normals.reserve(static_cast<size_t>(majorCount) * static_cast<size_t>(minorCount));
            mesh.TexCoords.reserve(static_cast<size_t>(majorCount) * static_cast<size_t>(minorCount));

            const float majorStep = glm::two_pi<float>() / static_cast<float>(majorSegments);
            const float minorStep = glm::two_pi<float>() / static_cast<float>(minorSegments);

            for (int i = 0; i < majorCount; ++i)
            {
                // SEAM WRAP ON BOTH AXES, same mechanism as the cylinder.
                //
                // The modulo returns the duplicated final column to angle 0 so it lands on
                // bit-identical coordinates with column 0. Computing it at a full 2*pi instead
                // would differ in the last bits and open a hairline crack in the surface.
                const float u = majorStep * static_cast<float>(i % majorSegments);
                const float cu = glm::cos(u);
                const float su = glm::sin(u);

                for (int j = 0; j < minorCount; ++j)
                {
                    const float v = minorStep * static_cast<float>(j % minorSegments);
                    const float cv = glm::cos(v);
                    const float sv = glm::sin(v);

                    // Ring centre, then offset by the tube radius. The normal
                    // points from the ring centre toward the surface point, so
                    // it stays correct on the inner wall where the position
                    // gradient would be degenerate.
                    // SURFACE POINT = RING CENTRE + NORMAL * TUBE RADIUS.
                    //
                    // THE NORMAL IS TAKEN FROM THE RING CENTRE RATHER THAN DERIVED FROM THE POSITION
                    // FIELD, and that is not a stylistic choice. On the INNER wall of the tube the
                    // parametric derivatives become degenerate -- the surface folds back on itself
                    // there -- so a cross product of neighbouring differences collapses toward zero
                    // and a finite-differenced normal is undefined exactly where the torus is most
                    // concave. The closed-form direction has no such singularity, so shading stays
                    // correct all the way around.
                    //
                    // It is also why this shape is a good test of OrientTrianglesOutward: the normals
                    // here are reliable even where position-derived ones would not, so the winding
                    // oracle is sound for the whole surface.
                    const glm::vec3 ringCentre(cu * kMajorRadius, 0.0f, su * kMajorRadius);
                    // THE ANALYTIC TUBE NORMAL, AND IT IS ALREADY UNIT LENGTH.
                    //
                    // |normal|^2 = cv^2*cu^2 + sv^2 + cv^2*su^2 = cv^2*(cu^2 + su^2) + sv^2 = 1, since
                    // cos^2 + sin^2 = 1 for both pairs. So pushing it directly below, with no
                    // normalize, is safe -- which is worth stating because every other generator here
                    // normalizes, and the omission looks like a bug.
                    //
                    // The components are the spherical direction (cv, sv) rotated into the ring's
                    // frame at angle u: cv scales the outward radial component (cu, 0, su) and sv is
                    // the tube's vertical component.
                    //
                    // WHAT MAKES THIS NECESSARY is the note below -- deriving the normal from the
                    // position field instead would break on the inner wall of the tube.
                    const glm::vec3 normal(cv * cu, sv, cv * su);
                    mesh.Positions.push_back(ringCentre + normal * kMinorRadius);
                    mesh.Normals.push_back(normal);
                    mesh.TexCoords.emplace_back(static_cast<float>(i) / static_cast<float>(majorSegments),
                                                static_cast<float>(j) / static_cast<float>(minorSegments));
                }
            }

            for (int i = 0; i < majorSegments; ++i)
            {
                for (int j = 0; j < minorSegments; ++j)
                {
                    const uint32_t a = static_cast<uint32_t>(i * minorCount + j);
                    const uint32_t b = static_cast<uint32_t>((i + 1) * minorCount + j);
                    const uint32_t c = static_cast<uint32_t>((i + 1) * minorCount + j + 1);
                    const uint32_t d = static_cast<uint32_t>(i * minorCount + j + 1);
                    // QUADS OVER THE (major, minor) PARAMETRIC GRID, closed in both directions.
                    //
                    // a, b, c, d are the four corners of one cell: (i,j), (i+1,j), (i+1,j+1),
                    // (i,j+1). The argument order differs from the grid generators' (a, c, d, b) only
                    // in naming, and as everywhere here the winding is settled by
                    // OrientTrianglesOutward rather than by this call.
                    //
                    // The loops run to majorSegments/minorSegments rather than to the +1 counts, so
                    // the seam columns are the duplicated closure of the surface, not extra cells.
                    AddQuad(mesh, a, d, c, b);
                }
            }
            OrientTrianglesOutward(mesh);
            return mesh; });
    }
}