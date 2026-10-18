// Components.h -- the plain-data structs that make up an entity.
//
// An entity in this engine is not a class: it is a UUID plus a set of these
// components, and behaviour comes from systems that iterate entities looking
// for a particular component. See Scene.h for the registry that holds them.
//
// Design notes that apply to all of them:
//
//   * Every component is a struct with public fields and defaulted copy
//     constructors. No virtual functions, no inheritance in the common case --
//     the scene stores them by type in a tuple, so they must stay cheap to
//     copy and trivially destructible.
//   * Field defaults are the "just added" state, so CreateEntity produces
//     something renderable without the caller filling anything in.
//   * The explicitly-defaulted copy constructors are not redundant noise: they
//     suppress the implicit move constructor, which matters for
//     TransformComponent, whose cache must not be copied (see below).

#pragma once

#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// matrix_decompose and quaternion live in GLM's experimental extension set, so
// the gate has to be set before either header is included. Set here rather than
// in each consumer so every translation unit sees the same GLM configuration.
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include "SceneCamera.h"

#include "Wasteland/Renderer/Texture.h"

#include "Wasteland/Core/UUID.h"

namespace Wasteland
{

	// Every entity's identity. Present on every entity, always.
	//
	// Separate from the scene's own indexing because the UUID is the *stable*
	// identity: it survives a scene reload and is what gets written to disk,
	// whereas the scene's internal slot index does not.
	struct IDComponent
	{
		// The entity's unique id. Generated randomly at construction, so
		// creating two entities in a row yields two distinct ids with no
		// central allocator involved.
		//
		// Read-only by convention: nothing in the engine changes it after
		// creation, because references to the entity (handles, parent/child
		// links, serialized references) are keyed on it.
		UUID ID;

		// Defaulted, so `IDComponent{}` still mints a fresh UUID. Not explicit
		// on purpose: nothing here is defaulted, so there is no ambiguity to
		// guard against.
		IDComponent() = default;

		// Explicitly defaulted rather than left implicit. Suppresses the move
		// constructor, which matters for the components below with cache
		// members -- see the file header.
		IDComponent(const IDComponent &) = default;
	};

	// A short string label for an entity, used by the editor's hierarchy panel
	// and by scene serialization.
	//
	// Purely cosmetic: nothing in the simulation reads it, and an entity with
	// no TagComponent is entirely valid.
	struct TagComponent
	{
		// The entity's display name. Empty by default; the editor falls back to
		// something derived from the UUID when this is empty.
		std::string Tag;

		// Defaulted to an empty tag.
		TagComponent() = default;

		// Suppresses the move constructor; see the file header.
		TagComponent(const TagComponent &) = default;

		// Constructs with a name.
		//
		// @param tag  The label to store, copied into Tag.
		TagComponent(const std::string &tag)
			: Tag(tag) {}
	};

	// An entity's position, rotation and scale.
	//
	// Rotation is stored as Euler angles in radians, NOT a quaternion. That is a
	// deliberate simplification: it is easy to author and to read back in a
	// scene file, at the cost of gimbal lock and of the inability to represent
	// a rotation cleanly over 360 degrees. The composed matrix is built via a
	// quaternion internally so the Euler angles are only a storage format.
	//
	// The fields are public and mutated directly by gameplay code, so the
	// matrix cache below has to validate itself against them on every read.
	struct TransformComponent
	{
		// Position in world space. Origin is (0,0,0).
		glm::vec3 Translation = {0.0f, 0.0f, 0.0f};

		// Rotation as Euler angles in radians, applied X then Y then Z.
		// Identity is (0,0,0), not a quaternion identity.
		glm::vec3 Rotation = {0.0f, 0.0f, 0.0f};

		// Per-axis scale. Defaults to (1,1,1) rather than zero, because a
		// zero-scaled entity is invisible and almost never intended.
		glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

		// Defaulted: an entity created with no transform sits at the origin,
		// unrotated, at unit scale.
		TransformComponent() = default;

		// Copy constructor that deliberately copies ONLY the three transform
		// fields and leaves the cache at its defaults.
		//
		// This is the whole reason the copy constructor is written out by hand
		// rather than defaulted: a defaulted copy would carry the cached matrix
		// and its key across, which would be *correct* (the key matches the
		// copied TRS) but wasteful, and -- more importantly -- would be wrong
		// for any future cache member whose validity depends on something not
		// stored here.
		//
		// Writing it explicitly also suppresses the implicit move constructor,
		// so a TransformComponent is always copied rather than moved. Slightly
		// less efficient, but it keeps the cache invariant obvious.
		TransformComponent(const TransformComponent &other)
			: Translation(other.Translation), Rotation(other.Rotation), Scale(other.Scale) {}

		// Constructs a pure translation, leaving rotation at identity and scale
		// at 1.
		//
		// @param translation  The position to copy in.
		TransformComponent(const glm::vec3 &translation)
			: Translation(translation) {}

		// Returns the composed TRS matrix, recomputing only if a field changed.
		//
		// This is the hot path: every renderable entity calls it at least once
		// per frame, and composing three matrices is not free. The cache turns
		// an unchanged entity's transform into three vector comparisons.
		//
		// Validity is checked by comparing the current TRS against the values
		// the cache was built from, which is what allows the fields above to
		// stay public and be written directly.
		//
		// Ordering is scale, then rotate, then translate -- i.e. the composed
		// matrix is T * R * S, so scale is applied first in object space and
		// translation last. This is the conventional order and means a rotated
		// entity's offset is unaffected by its own scale.
		//
		// @returns The 4x4 transform matrix. Safe to hold: the cache is only
		//          rewritten when a field actually changes.
		glm::mat4 GetTransform() const
		{
			// Any one field differing invalidates the cache. GLM's operator!=
			// on vec3 is an exact component-wise comparison, so this is a true
			// equality test rather than a tolerance-based one -- a transform
			// that drifts by a float epsilon is treated as changed and the
			// matrix is rebuilt, which is the safe direction to be wrong in.
			if (Translation != m_CachedTranslation ||
				Rotation != m_CachedRotation ||
				Scale != m_CachedScale)
			{
				// Identity-seeded so each factor is a pure transform.
				glm::mat4 translation = glm::translate(glm::mat4(1.0f), Translation);

				// Euler angles converted to a quaternion, then to a matrix.
				// Going through the quaternion is what makes the composition
				// order behave correctly and avoids the matrix-level gimbal
				// artefacts; the Euler form is only the storage format.
				glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));
				glm::mat4 scale = glm::scale(glm::mat4(1.0f), Scale);

				// T * R * S -- applied right to left, so scale acts first on
				// the vertex, then rotation, then translation.
				m_CachedTransform = translation * rotation * scale;

				// Record the inputs this matrix was built from, so the next
				// call can tell whether anything changed.
				m_CachedTranslation = Translation;
				m_CachedRotation = Rotation;
				m_CachedScale = Scale;
			}
			// Cache hit or miss, the freshly-built matrix is returned.
			return m_CachedTransform;
		}

		// Replaces TRS from an existing matrix -- the inverse of GetTransform.
		//
		// Used when a transform arrives as a single matrix, typically from a
		// parent entity or from an imported scene file.
		//
		// Two limitations, both inherent to glm::decompose rather than to this
		// wrapper:
		//
		//   * Shear cannot be represented by TRS and is DISCARDED. A sheared
		//     matrix decomposes to a TRS that does not reproduce it.
		//   * A perspective term is likewise discarded, so a projective matrix
		//     silently yields a plausible-looking affine transform rather than
		//     reporting that it could not be represented.
		//
		// The cache is not touched here, which is correct: the next
		// GetTransform() will see TRS differ from the cached key and rebuild.
		//
		// @param transform  The matrix to decompose into this component's fields.
		void SetTransform(const glm::mat4 &transform)
		{
			// Out-parameters glm::decompose requires but this component has no
			// field for. Declared here because decompose takes them by
			// reference; they are intentionally left unused.
			glm::vec3 skew;
			glm::vec4 perspective;

			// The rotation comes back as a quaternion, since decompose cannot
			// produce Euler angles directly.
			glm::quat orientation;

			// decompose's return value is ignored: it reports false only for a
			// singular matrix, in which case the out-params are left
			// untouched and this component keeps its previous transform. That
			// is a reasonable outcome -- a degenerate matrix leaves the entity
			// where it was -- but it is silent, so a caller feeding bad matrices
			// in gets no diagnostic.
			glm::decompose(transform, Scale, orientation, Translation, skew, perspective);

			// Quaternion back to Euler angles for storage, matching the format
			// the Rotation field documents. Round-tripping through a quaternion
			// here is what normalizes the rotation.
			Rotation = glm::eulerAngles(orientation);
		}

	private:
		// The composed matrix from the last GetTransform() call.
		//
		// Seeded to identity, which matches the default TRS of
		// (0,0,0)/(0,0,0)/(1,1,1) -- so a default-constructed component
		// correctly reports its cache as valid on the very first read and
		// returns identity without rebuilding.
		//
		// mutable because GetTransform() is const: it logically does not
		// change the component's transform, only its cache. The mutable cache
		// is safe here only because it is validated against the fields on
		// every read.
		mutable glm::mat4 m_CachedTransform = glm::mat4(1.0f);

		// The TRS values m_CachedTransform was built from. Also mutable, and
		// seeded to the field defaults so the identity case above validates.
		mutable glm::vec3 m_CachedTranslation = {0.0f, 0.0f, 0.0f};
		mutable glm::vec3 m_CachedRotation = {0.0f, 0.0f, 0.0f};
		mutable glm::vec3 m_CachedScale = {1.0f, 1.0f, 1.0f};
	};

	// Draws the entity as a 2D textured quad in a 2D scene.
	//
	// The 2D counterpart to the cube/sphere/mesh components below; a
	// SpriteRendererComponent on an entity in a 3D scene is not rendered by the
	// 3D path.
	struct SpriteRendererComponent
	{
		// Tint, RGBA. Multiplied against the texture, so white leaves the
		// texture unmodified and alpha is the sprite's opacity.
		//
		// Default white rather than a colour, so an untextured sprite is drawn
		// as a solid white quad -- visible, which makes "the component is
		// there but nothing is showing" a distinguishable bug.
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};

		// Texture to sample. Optional -- a null Texture draws the flat Color.
		//
		// Shared (Ref/shared_ptr) because textures are expensive to build and
		// are routinely shared across many entities; ownership is shared with
		// whatever asset system loaded it.
		Ref<Texture2D> Texture;

		// How many times the texture repeats across the quad. 1.0 is one
		// showing; higher values tile.
		float TilingFactor = 1.0f;

		// Defaulted; all fields take their declared defaults.
		SpriteRendererComponent() = default;

		// Suppresses the implicit move constructor; see the file header.
		SpriteRendererComponent(const SpriteRendererComponent &) = default;

		// Constructs with an explicit tint, for the common
		// "coloured untextured quad" case.
		//
		// @param color  The RGBA tint to copy in.
		SpriteRendererComponent(const glm::vec4 &color)
			: Color(color) {}
	};

	// Draws the entity as a stroked circle outline in a 2D scene.
	//
	// Note this is a 2D sprite-space outline, NOT the 3D Circle3D mesh below.
	// The two are separate components because they are filled by different
	// renderers.
	struct CircleRendererComponent
	{
		// Stroke RGBA. See SpriteRendererComponent::Color.
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};

		// Line width in pixels. 1.0 is a hairline.
		float Thickness = 1.0f;

		// Fade applied to the circle's edge, to soften it.
		//
		// Small by default (0.005), giving a barely-perceptible softening. This
		// is a smoothing strength, not an alpha value -- see the 2D renderer
		// for how it is consumed.
		float Fade = 0.005f;

		// Defaulted; all fields take their declared defaults.
		CircleRendererComponent() = default;

		// Suppresses the implicit move constructor; see the file header.
		CircleRendererComponent(const CircleRendererComponent &) = default;
	};

	// Draws the entity as a textured, tinted cube in a 3D scene.
	struct CubeRendererComponent
	{
		// Albedo tint RGBA, multiplied against the texture.
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};

		// Albedo texture. Optional; null draws the flat Color.
		Ref<Texture2D> Texture;

		// Which face of a texture atlas this cube samples.
		//
		// Meaningless when Texture is null or is not an atlas. Present so a
		// whole tileable atlas can be assigned to many cubes with one texture
		// bound.
		int TextureIndex = 0;

		// Texture repeats per face. See SpriteRendererComponent::TilingFactor.
		float TilingFactor = 1.0f;

		// Defaulted.
		CubeRendererComponent() = default;

		// Suppresses the implicit move constructor; see the file header.
		CubeRendererComponent(const CubeRendererComponent &) = default;
	};

	// Draws the entity as a UV sphere in a 3D scene.
	//
	// Unlike the cube above, this carries its own tessellation, because the
	// sphere's smoothness is a per-entity property rather than a mesh asset.
	struct SphereRendererComponent
	{
		// Albedo tint RGBA.
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};

		// Albedo texture. Optional.
		Ref<Texture2D> Texture;

		// Atlas face index. See CubeRendererComponent::TextureIndex.
		int TextureIndex = 0;

		// Texture repeats across the surface.
		float TilingFactor = 1.0f;

		// Sphere radius in local units, before the entity's Scale.
		//
		// Kept separate from TransformComponent::Scale so the radius can be
		// authored without disturbing the transform -- the two are independent.
		float Radius = 0.5f;

		// Horizontal tessellation: longitude divisions around the sphere.
		//
		// Higher is rounder and more expensive. 20 is a low default that reads
		// as smooth at typical viewing distances.
		int Sectors = 20; // Horizontal smoothness

		// Vertical tessellation: latitude divisions pole to pole. See Sectors.
		int Stacks = 20;  // Vertical smoothness

		// Defaulted.
		SphereRendererComponent() = default;

		// Suppresses the implicit move constructor; see the file header.
		SphereRendererComponent(const SphereRendererComponent &) = default;
	};

		// The remaining Blender "Add > Mesh" primitives. Each mirrors the
		// generator in Renderer/ProceduralMeshes.h -- same names, same default
		// tessellation -- so an artist moving from Blender gets the shape they
		// expect, and the two stay in sync.
		//
		// Color/Texture/TilingFactor intentionally duplicate SphereRendererComponent
		// rather than deriving from MaterialComponent: the raster path resolves the
		// per-primitive colour, while MaterialComponent carries the PBR set the
		// path tracer samples. Same split as cube and sphere.
		struct PlaneRendererComponent
	{
		// Albedo tint RGBA.
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};

		// Albedo texture. Optional.
		Ref<Texture2D> Texture;

		// Atlas face index. See CubeRendererComponent::TextureIndex.
		int TextureIndex = 0;

		// Texture repeats across the face.
		float TilingFactor = 1.0f;

		// Subdivision cuts applied to the base quad.
		//
		// Each cut quadruples the triangle count. 0 -- the default -- is a
		// single quad, which is correct for a flat plane; the generator in
		// ProceduralMeshes.h uses this only to add vertices for vertex-level
		// displacement or lighting, so raising it with nothing consuming the
		// extra vertices just costs fill rate.
		int Subdivisions = 0; // Blender: Plane "Add Mesh" -> 0 cuts

		// Defaulted.
		PlaneRendererComponent() = default;

		// Suppresses the implicit move constructor; see the file header.
		PlaneRendererComponent(const PlaneRendererComponent &) = default;
	};

		// Named 3D to stay distinct from the 2D CircleRendererComponent above
		// (which draws a stroked sprite outline, not a filled disc).
		// Draws the entity as a filled, tessellated disc lying in the XZ plane.
		//
		// A real 3D mesh with a rim, not a billboard -- unlike the 2D
		// CircleRendererComponent, which is a flat stroked sprite.
		struct Circle3DRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Disc radius in local units, before the entity's Scale.
		//
		// Independent of TransformComponent::Scale, so radius can be authored
		// without disturbing the transform.
		float Radius = 0.5f;
		// Rim tessellation: how many wedges the disc is cut into.
		//
		// 32 gives a visibly smooth rim at typical viewing distances; each
		// halving is noticeable on a disc seen edge-on, because the silhouette
		// IS the rim.
		int Segments = 32; // Blender: Circle default vertex count

		Circle3DRendererComponent() = default;
		Circle3DRendererComponent(const Circle3DRendererComponent &) = default;
	};

		// Draws the entity as an icosphere: a sphere built from a subdivided
		// icosahedron.
		//
		// Preferred over SphereRendererComponent for most cases because its
		// triangles are near-equilateral, so it tessellates more evenly for the
		// same vertex count -- no dense pole cluster and no visible UV pinch at
		// the top and bottom that a UV sphere's pole-vertex topology forces.
		struct IcoSphereRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Sphere radius in local units, before the entity's Scale.
		//
		// Independent of TransformComponent::Scale.
		float Radius = 0.5f;
		// Recursive subdivision count.
		//
		// Triangle count is 20 * 4^n: 80 at the default of 2, 1280 at 4, 20480
		// at 6. Each level also doubles the sphere's apparent smoothness, so
		// this is the dominant cost control for the primitive -- unlike the
		// linear 2D tessellation of the disc above.
		//
		// Past about 4 the extra triangles are smaller than a pixel at normal
		// viewing distance and cost fill rate for no visible gain.
		int Subdivisions = 2; // Blender: Ico Sphere default (20 * 4^n triangles)

		IcoSphereRendererComponent() = default;
		IcoSphereRendererComponent(const IcoSphereRendererComponent &) = default;
	};

		// Draws the entity as a closed cylinder aligned to the Y axis.
		//
		// The engine's standard vertical collision shape, so it pairs with the
		// capsule character controllers built on it. A cylinder's axis is Y
		// here, NOT Z -- which is the opposite of several other conventions and
		// is the usual reason a capsule model ends up lying on its side.
		struct CylinderRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Cylinder radius in local units, before the entity's Scale.
		//
		// Independent of TransformComponent::Scale.
		float Radius = 0.5f;
		// Total height along Y, from base to cap -- not half-height.
		//
		// Full height rather than half is the convention that keeps the value
		// unambiguous at a glance; a modeler reaching for 2.0 expecting a
		// half-height gets a cylinder twice as tall as they intended.
		//
		// Independent of TransformComponent::Scale.
		float Height = 1.0f;
		int Segments = 32; // Blender: Cylinder default vertex count

		CylinderRendererComponent() = default;
		CylinderRendererComponent(const CylinderRendererComponent &) = default;
	};

		// Draws the entity as a closed cone aligned to the Y axis.
		//
		// Base at y=0, apex at y=Height, matching CylinderRendererComponent's
		// origin convention so the two are interchangeable in a model.
		//
		// NOT tessellated with separate cap segments: the base is a fan from a
		// centre vertex, so raising Segments improves the rim but not the cap's
		// interior shading.
		struct ConeRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Base radius in local units, before the entity's Scale.
		float Radius = 0.5f;
		// Height from base to apex, along Y. See the note on the cylinder's
		// Height: this is full height, not half.
		float Height = 1.0f;
		int Segments = 32; // Blender: Cone default vertex count

		ConeRendererComponent() = default;
		ConeRendererComponent(const ConeRendererComponent &) = default;
	};

		// Draws the entity as a torus (donut).
		//
		// Parameterised by two radii rather than one, because a torus is a
		// circle swept by a circle: the two circles can be sized independently.
		struct TorusRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Radius from the torus centre to the CENTRE of the tube -- not to its
		// outer edge.
		//
		// This is the single most common source of mis-sized tori. The overall
		// outer diameter is (MajorRadius + MinorRadius) * 2, so at the defaults
		// the donut spans 1.0 units across while MajorRadius reads 0.375. A
		// caller who sets MajorRadius to half the diameter they want gets a
		// torus too large by the tube thickness.
		//
		// Independent of TransformComponent::Scale.
		float MajorRadius = 0.375f; // Distance from centre to tube centre
		// Radius of the tube itself -- its thickness.
		//
		// There is no guard against MinorRadius exceeding MajorRadius. When it
		// does, the tube's inner edge would pass through the torus centre and the
		// surface self-intersects, producing inverted normals and a shape that
		// renders as a dark, holed mess rather than a fat donut. A fat torus
		// should be authored by raising MajorRadius too.
		//
		// Independent of TransformComponent::Scale.
		float MinorRadius = 0.125f; // Tube thickness
		// Tessellation around the main ring -- how many steps the circle is
		// swept over.
		//
		// As with the disc above, this sets the quality of the silhouette, which
		// is what the eye reads for a torus.
		int MajorSegments = 48;
		// Tessellation around the tube's own circular cross-section.
		//
		// Independent of MajorSegments, so the donut can be smooth around the
		// ring while keeping the tube cheap, or vice versa. 12 is low: enough
		// that the tube does not read as visibly faceted at typical sizes.
		int MinorSegments = 12;

		TorusRendererComponent() = default;
		TorusRendererComponent(const TorusRendererComponent &) = default;
	};

		// Draws the entity as a subdivided plane in the XZ plane -- an infinite
		// ground made finite and tessellatable.
		//
		// Distinct from PlaneRendererComponent because its subdivision counts
		// are set independently on each axis (a 10 x 10 grid is the default,
		// giving 10x10 cells) and because a grid is intended to be tiled or
		// streamed, whereas a plane is a single authored surface.
		struct GridRendererComponent
	{
		glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		float TilingFactor = 1.0f;
		// Cell count along X. See the note on MajorSegments for how tessellation
		// maps to silhouette quality.
		//
		// Independent of YSubdivisions, so a ground plane can be dense along one
		// axis only.
		int XSubdivisions = 10; // Blender: Grid default (10 x 10)
		// Cell count along Z. Note the name: the field is Y but it subdivides
		// the Z axis, because the grid lies flat and "Y subdivisions" refers to
		// Blender's naming of the flat grid's two in-plane axes, not to world Y.
		//
		// Cells = XSubdivisions * YSubdivisions, so the defaults give 100 cells
		// / 200 triangles.
		int YSubdivisions = 10;

		GridRendererComponent() = default;
		GridRendererComponent(const GridRendererComponent &) = default;
	};

	// The PBR material set used by the path-traced (Nova) renderer.
	//
	// Carries the full physically-based set -- metallic/roughness,
	// emission, displacement -- where the primitive components above carry
	// only an albedo tint and a texture. The two coexist on an entity: a
	// cube's Color drives the rasterised path, and this drives the path
	// tracer, so changing only one leaves the two renderers visibly
	// disagreeing.
	//
	// Every field defaults to a neutral, fully-dielectric, non-emissive
	// material, so adding this component never changes how the entity
	// looks until something is actually set.
	struct MaterialComponent
	{
		// Base colour, RGB. The alpha channel is present for type symmetry with
		// the primitive components' Color but is unused here: opacity in the PBR
		// path comes from the transmission/thickness fields, not from albedo
		// alpha.
		glm::vec4 Albedo = {1.0f, 1.0f, 1.0f, 1.0f};
		Ref<Texture2D> Texture;
		int TextureIndex = 0;
		// Normal-map intensity, in [0, 1].
		//
		// 0 -- the default -- disables the normal map entirely, so the field is
		// both the intensity and the enable flag. At 1.0 the surface normals are
		// taken from the map unmodified; intermediate values lerp between the
		// geometric normal and the mapped one, which is how a bump is turned
		// down without re-authoring the texture.
		float NormalStrength = 0.0f;
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		// Emitted light's colour, RGB.
		//
		// White by default, which is inert on its own because
		// EmissionIntensity defaults to 0 -- the pair is deliberately decoupled
		// so setting intensity does not also change hue.
		//
		// Emission is what the path tracer samples for indirect bounce light,
		// so it is the cheapest way to light a scene without adding an emitter
		// geometry.
		glm::vec4 EmissionColor = {1.0f, 1.0f, 1.0f, 1.0f};
		// Emitted radiance multiplier. 0 -- the default -- means the entity emits
		// no light, regardless of EmissionColor.
		//
		// This is the field that gates emission, not a separate has-emission
		// flag: there is no separate flag, so this doubles as one.
		//
		// Values above 1.0 are legal and are how a surface reads as brighter
		// than its albedo.
		float EmissionIntensity = 0.0f;

		// Parallocclusion-mapping displacement strength.
		//
		// 0 -- the default -- disables displacement entirely. Non-zero values
		// push the shading point along the surface normal by the depth stored in
		// the map, which adds real self-shadowing at grazing angles -- but only
		// for the pixel's shading point. Silhouettes are NOT displaced: the
		// outline against the background stays flat, which is the giveaway when
		// displacement is cranked up.
		//
		// This is a shading effect, not vertex displacement, so it costs nothing
		// in geometry and nothing extra per vertex.
		float DisplacementScale = 0.0f;

		// Set once the engine has procedurally filled in the material's texture
		// maps (albedo/normal/roughness from a material name, say).
		//
		// An optimization guard rather than a validity check: when false, the
		// renderer skips the per-frame map-generation work and uses whatever
		// maps are present, even if that is none. Generators must set this
		// after filling the maps, or the result is silently discarded.
		//
		// Also flipped to false on load so a component whose maps failed to
		// deserialize does not render with the previous material's textures.
		bool HasGeneratedMaps = false;
		// Project-relative path to the source texture this material's maps were
		// generated from or loaded from.
		//
		// Kept for round-tripping and for the editor to show where a material
		// came from. Nothing in the render path reads it -- the loaded
		// Ref<Texture2D> is what gets sampled -- so a path pointing at a moved
		// or deleted file breaks saving and reloading, not rendering.
		std::string TexturePath;

		MaterialComponent() = default;
		MaterialComponent(const MaterialComponent &) = default;
	};

	// Makes the entity a camera: it contributes a SceneCamera to the render
	// pass, and the scene uses the primary one as the active viewpoint.
	//
	// Independent of TransformComponent, even though a camera needs a
	// position: SceneCamera holds its own view and projection, and something
	// else has to keep the two in sync. A camera whose transform is never
	// updated looks correct in the editor's view matrix while rendering from
	// wherever SceneCamera was last set.
	struct CameraComponent
	{
		// The view/projection state this entity renders from.
		//
		// A value, not a pointer: copying the component copies the camera state,
		// which is what makes duplicating a camera entity in the editor produce
		// a working second viewpoint rather than two entities sharing one.
		SceneCamera Camera;
		// Whether this camera is the scene's active viewpoint.
		//
		// True by default, so adding a CameraComponent to an entity makes it
		// the camera -- which means adding a SECOND one silently competes with
		// the first for Primary, and the scene's choice between two primaries is
		// unspecified.
		//
		// The existing TODO is right that this does not belong on a per-entity
		// component. It is scene state: the scene wants exactly one primary and
		// can point at it, rather than every camera asserting it.
		bool Primary = true; // TODO: think about moving to Scene
		// Locks the camera's aspect ratio to the editor viewport's, so resizing
		// the window letterboxes the view instead of widening the field of view.
		//
		// Off by default, meaning the projection's aspect follows the framebuffer
		// and the FOV stays vertical. Turning this on is what keeps a previewed
		// composition from distorting when the window is resized.
		bool FixedAspectRatio = false;

		CameraComponent() = default;
		CameraComponent(const CameraComponent &) = default;
	};

	// Forward declaration
	// Forward declaration so NativeScriptComponent can hold a pointer without
	// this header depending on ScriptableEntity's definition. The full type
	// lives in ScriptableEntity.h; nothing here needs more than the pointer.
	class ScriptableEntity;

	// Attaches a C++ ScriptableEntity subclass to an entity.
	//
	// Type erasure via a pair of function pointers rather than a template
	// member, so the scene can hold a heterogeneous list of scripted
	// entities without knowing their concrete script types -- which is
	// exactly what makes the scripting system work at all.
	//
	// NOTE: the function pointers are NOT initialized. Until Bind<T>() runs,
	// both are indeterminate, and calling either is undefined behaviour. The
	// only reason this has not caused crashes is that every current call
	// site checks Instance for null first, and Instance is the one member
	// that IS initialized. Anything new that calls InstantiateScript must
	// establish that Bind has run rather than assuming it.
	struct NativeScriptComponent
	{
		// The live script object, or null if no script is attached.
		//
		// Null is the sentinel for "nothing attached", and it is checked far more
		// often than the function pointers are called. It is null until either
		// InstantiateScript runs or a script is assigned through the bindings.
		//
		// Owned via the DestroyScript thunk rather than a smart pointer, because
		// the concrete type is erased. Scene must call DestroyScript (and not
		// plain delete) on teardown.
		ScriptableEntity *Instance = nullptr;

		// Factory thunk: creates a new instance of the bound script type and
		// returns it as a ScriptableEntity*.
		//
		// Set by Bind<T>(). Callable with no entity argument, so the caller
		// assigns the result to Instance itself.
		//
		// Indeterminate until Bind<T>() runs -- see the struct-level note.
		ScriptableEntity *(*InstantiateScript)();
		// Teardown thunk: deletes Instance and resets it to null.
		//
		// Set by Bind<T>(). Takes the component rather than the instance so it
		// can clear the field afterwards, leaving the component in a safe
		// re-usable state instead of dangling.
		//
		// The nulling is what makes double-destroy safe: a second call sees
		// Instance == null and deletes nothing. Note that this is why the
		// component has no destructor of its own -- one would have to call this
		// thunk, and a default-constructed component's thunk is indeterminate.
		//
		// Indeterminate until Bind<T>() runs.
		void (*DestroyScript)(NativeScriptComponent *);

		// Binds this component to the concrete script type T.
		//
		// Both thunks are stateless lambdas that capture nothing, so they convert
		// to plain function pointers -- which is what lets them live in this
		// type-erased struct.
		//
		// T must derive from ScriptableEntity and be default-constructible: the
		// factory news it with no arguments. A script needing constructor
		// arguments cannot be attached this way and has to be constructed by its
		// caller instead.
		//
		// Call once, at attach time. Calling it again over the thunks and leaks
		// the previous Instance, since the old object is never destroyed.
		template <typename T>
		void Bind()
		{
			InstantiateScript = []()
			{ return static_cast<ScriptableEntity *>(new T()); };
			DestroyScript = [](NativeScriptComponent *nsc)
			{ delete nsc->Instance; nsc->Instance = nullptr; };
		}
	};

	// References an authored script BY PATH rather than embedding it.
	//
	// The native counterpart (NativeScriptComponent) holds the live object;
	// this holds the authoring info -- where the script lives and what it is
	// called -- so the editor can list and edit scripts on entities without
	// loading any of them.
	//
	// Loading is deferred: something reads these paths and attaches real
	// scripts at load time.
	struct ScriptComponent
	{
		// Project-relative path to the script file, e.g. "scripts/Player.py".
		//
		// Resolved relative to the project root, so a project moved to another
		// machine still resolves. An absolute path here breaks on move.
		std::string ScriptPath;
		// Display name of the script, used by the editor's script list.
		//
		// Separate from ScriptPath because the path's basename is not always the
		// name a designer wants shown -- two scripts in different folders can
		// share a filename.
		std::string ScriptName;

		ScriptComponent() = default;
		ScriptComponent(const ScriptComponent &) = default;
	};

	// Drives an authored `.wlbtree` on an entity. Same shape as
	// ScriptComponent so a designer learns one attachment model, not two --
	// see Scene/BehaviorTreeSystem.h for why this exists at all.
	struct BehaviorTreeComponent
	{
		// Project-relative, e.g. "trees/EnemyChase.wlbtree".
		std::string TreePath;
		// Class name the compiler emits. Empty => use the graph's own `name:`.
		std::string TreeName;
		// Why a compile failed, if it did. Surfaced so a broken tree is
		// visible instead of leaving the NPC standing still forever.
		std::string CompileError;
		bool Attached = false;
		int SuspendedTicks = 0;

		BehaviorTreeComponent() = default;
		BehaviorTreeComponent(const BehaviorTreeComponent &) = default;
	};

	// Physics

	// Gives the entity a 2D rigid body, so the physics simulation moves it
	// and collides it.
	//
	// The simulation only touches entities that have BOTH this and a collider
	// component; a body with no shape is never simulated and silently
	// produces no motion.
	//
	// 2D only. This is a distinct path from any 3D physics in the engine and
	// the two do not interact -- an entity with this and a 3D collider is
	// simulated in neither consistently.
	struct Rigidbody2DComponent
	{
		// How the simulation moves this body. The distinction is what the
		// solver does when two bodies touch.
		enum class BodyType
		{
			Static = 0,
			Dynamic,
			Kinematic
		};
		// The body's movement mode.
		//
		// Static by default, so adding a rigidbody does NOT make something
		// fall. A Static body is immovable and infinite-mass: it is a wall or
		// platform. Dynamic is what falls and is pushed; Kinematic moves only
		// because code moved it.
		//
		// Switching Static to Dynamic at runtime on an entity already in the
		// world needs the simulation to rebuild the body -- see the runtime
		// note below.
		BodyType Type = BodyType::Static;
		// Locks the body's rotation, leaving it free to translate.
		//
		// False by default, so a Dynamic body rotates when it collides.
		//
		// Written as 0 rather than false, which is equivalent but inconsistent
		// with the other bool fields in this file.
		bool FixedRotation = 0;

		// Storage for runtime
		// The live simulation body, owned by the physics world.
		//
		// An untyped pointer to keep this header free of the physics library's
		// types. Created when the entity enters the world and destroyed when it
		// leaves; null means not currently simulated.
		//
		// HAZARD: the defaulted copy constructor below copies this pointer, so a
		// copied Rigidbody2DComponent yields TWO components referencing ONE
		// simulation body. Both would then destroy it on teardown -- a double
		// free -- or worse, one entity would move the other's body. The component
		// must be copied with the pointer cleared, or the copy suppressed.
		//
		// The same applies to RuntimeFixture in the collider components below.
		void *RuntimeBody = nullptr;

		Rigidbody2DComponent() = default;
		// Explicitly defaulted -- and that is the problem noted on
		// RuntimeBody above. Written out to suppress the move constructor (see
		// the file header), but a defaulted copy constructor silently duplicates
		// the runtime pointer. It should copy the authored fields only and leave
		// RuntimeBody null, so the copy registers its own body with the physics
		// world instead of aliasing this one.
		Rigidbody2DComponent(const Rigidbody2DComponent &other) = default;
	};

	// An axis-aligned rectangular collision shape, offset from the entity's
	// position.
	//
	// Pairs with Rigidbody2DComponent, which supplies the motion. The offset
	// is what allows several colliders on one entity -- a character commonly
	// has a body box plus a smaller head box offset upward -- without needing
	// child entities.
	struct BoxCollider2DComponent
	{
		// Shape centre relative to the entity's origin.
		//
		// (0,0) centres the box on the entity. Offsetting is how a foot-level
		// collider is placed under a model whose origin is at its waist.
		glm::vec2 Offset = {0.0f, 0.0f};
		glm::vec2 Size = {0.5f, 0.5f};

		// TODO: move into physics material in the future maybe
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		// Impact speed below which a collision is treated as inelastic.
		//
		// Stops a resting body from jittering: two boxes that have come to rest
		// still overlap slightly each step, and without a threshold restitution
		// would bounce them apart forever and the stack would creep.
		//
		// Interacts with Restitution rather than replacing it -- a collision
		// slower than this simply uses zero bounce.
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		// The live simulation shape, owned by the physics world. Untyped to keep
		// the physics library out of this header.
		//
		// HAZARD: as with Rigidbody2DComponent::RuntimeBody, the defaulted copy
		// constructor duplicates this pointer, so a copied collider aliases one
		// fixture and both teardowns destroy it -- a double free. Copy the
		// authored fields and null this instead.
		void *RuntimeFixture = nullptr;

		BoxCollider2DComponent() = default;
		// Suppresses the move constructor (see the file header), but as noted on
		// RuntimeFixture above, a defaulted copy constructor must not carry the
		// runtime fixture pointer across.
		BoxCollider2DComponent(const BoxCollider2DComponent &other) = default;
	};

	// A circular collision shape, offset from the entity's position.
	//
	// The circle counterpart of BoxCollider2DComponent, and cheaper for the
	// solver than an equivalent box. Pairs with Rigidbody2DComponent for
	// motion.
	//
	// A circle collider rolls: unless the body also sets FixedRotation, a
	// Dynamic circle pushed by a collision spins away, because a circle
	// touching the ground has a single contact point and no restoring torque.
	struct CircleCollider2DComponent
	{
		// Shape centre relative to the entity's origin. See the box collider.
		glm::vec2 Offset = {0.0f, 0.0f};
		// Collision radius, multiplied by TransformComponent::Scale.x when the
		// fixture is created in Scene::OnPhysics2DStart:
		//     circleShape.m_radius = transform.Scale.x * cc2d.Radius;
		//
		// NOTE it uses Scale.X ALONE, not a max across axes. A circle under
		// non-uniform scale therefore takes its radius from one arbitrary axis
		// rather than the largest -- so an entity scaled (2, 0.1, 1) gets a
		// circle twice its authored radius in a visually squashed body. The 3D
		// sphere uses max() across all three axes and the 3D capsule uses
		// max() across the two radial ones, so 2D is the inconsistent case.
		//
		// Baked at world-creation time: changing Scale later does NOT resize the
		// fixture.
		float Radius = 0.5f;

		// TODO: move into physics material in the future maybe
		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		// See BoxCollider2DComponent::RestitutionThreshold.
		float RestitutionThreshold = 0.5f;

		// Storage for runtime
		// The live simulation shape. Untyped to keep the physics library out of
		// this header.
		//
		// HAZARD: as with every other collider here, the defaulted copy
		// constructor duplicates this pointer, so a copied collider aliases one
		// fixture and both teardowns destroy it.
		void *RuntimeFixture = nullptr;

		CircleCollider2DComponent() = default;
		CircleCollider2DComponent(const CircleCollider2DComponent &other) = default;
	};

	// --- 3D Physics Components (Box3D) ---

	// 3D counterpart of Rigidbody2DComponent::BodyType, with the same three
	// modes and the same meanings: Static is immovable and infinite-mass,
	// Dynamic is integrated by the solver, Kinematic moves only when moved.
	//
	// Declared as its own enum rather than reusing the 2D one, so the two
	// simulation paths cannot be wired together by mistake.
	enum class RigidBody3DType
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	// Gives the entity a 3D rigid body in the Box3D simulation.
	//
	// A SEPARATE system from the 2D Box2D bodies above: an entity with both a
	// 2D and a 3D body is registered in both worlds and moved by both, which
	// is almost never intended. Pick one dimension per entity.
	//
	// Needs a 3D collider to be simulated -- a body with no shape is never
	// stepped and silently produces no motion.
	struct RigidBody3DComponent
	{
		// The body's movement mode.
		//
		// Static by default, so adding a rigidbody does not make something fall.
		RigidBody3DType Type = RigidBody3DType::Static;
		// Multiplier on the world's gravity for this body.
		//
		// 1.0 is normal gravity; 0.0 makes the body ignore gravity entirely,
		// which is how a floating platform or a hover-drone is built. Negative
		// values invert the fall, which reads as an upward force and is a
		// legitimate (if unusual) way to hold something against a ceiling.
		//
		// Scales the world gravity rather than overriding it, so it composes with
		// whatever gravity the scene has set.
		float GravityScale = 1.0f;
		// Velocity damping, applied per step to translational motion.
		//
		// 0 -- the default -- means no damping, so a Dynamic body never slows on
		// its own. A small non-zero value is what makes a thrown object settle
		// instead of sliding forever on a low-friction surface.
		//
		// Expressed per step rather than per second, so its effect scales with
		// the timestep: at a lower frame rate the same value damps more per real
		// second.
		float LinearDamping = 0.0f;
		float AngularDamping = 0.0f;
		// Rotation locks, one per axis. Any combination is legal: locking X alone
		// gives a body that can turn in the horizontal plane but not pitch -- the
		// usual setup for a character or a turret.
		//
		// All three false by default, so a Dynamic body rotates freely.
		//
		// NOTE the API asymmetry with 2D: Rigidbody2DComponent exposes a single
		// FixedRotation bool that locks all three axes at once, while this needs
		// three separate flags. There is no single-flag equivalent here.
		bool FixedRotationX = false;
		bool FixedRotationY = false;
		bool FixedRotationZ = false;

		// Runtime handle (stores b3BodyId as uint64_t, opaque to this header)
		// The live Box3D body handle, stored as a packed uint64 so this header
		// needs no Box3D types. Created on world entry, destroyed on exit.
		//
		// Zero is a SAFE 'not yet created' sentinel, verified against the
		// vendored Box3D: b3StoreBodyId packs the id as
		// `(index1 << 32) | (world0 << 16) | generation`, and a real body always
		// has index1 >= 1 (index 0 is Box3D's reserved null body). A valid packed
		// id is therefore never 0, so the zero test cannot confuse a real body
		// for an absent one. Do not 'fix' this by reserving a different sentinel.
		//
		// HAZARD, same as the 2D version: the defaulted copy constructor below
		// copies this handle, so a copied RigidBody3DComponent yields two
		// components driving one simulated body. Both would then manipulate and
		// destroy it. Copy the authored fields and leave this 0.
		uint64_t RuntimeBody = 0;

		RigidBody3DComponent() = default;
		RigidBody3DComponent(const RigidBody3DComponent &other) = default;
	};

	// An axis-aligned box collision shape in the 3D simulation.
	//
	// Sizes ARE multiplied by TransformComponent::Scale, per axis:
	// Scene::CreatePhysicsBody3D passes HalfExtents.x * Scale.x, .y * Scale.y,
	// .z * Scale.z straight to b3CreateBoxShape. So a box at uniform scale 1
	// matches its dimensions exactly, and scaling the entity scales its
	// collider with it.
	//
	// That is a deliberate convenience -- a character authored at one size and
	// then scaled needs no collider edits -- with two consequences worth
	// knowing:
	//
	//   * The collider is baked at world-creation time from whatever Scale was
	//     then. Changing Scale afterwards does NOT resize the shape, so scaling
	//     an entity at runtime leaves a stale collider that no longer matches
	//     its visible mesh. The transform updates; the physics shape does not.
	//   * Non-uniform scale produces a correspondingly non-uniform box, which
	//     is what you want -- but it also means the visual mesh and the collider
	//     agree only while Scale is unchanged.
	//
	// The 2D colliders behave the same way (see OnPhysics2DStart), so this is
	// consistent across dimensions.
	struct BoxCollider3DComponent
	{
		// HALF the box's size on each axis, so the default box is 1x1x1.
		//
		// Half-extents rather than full size, matching Box3D's own convention.
		// This is the most common authoring mistake with box colliders: entering
		// 1 for a 1-unit cube is right here, but a caller thinking in full sizes
		// ends up with a box twice as large as intended on every axis.
		glm::vec3 HalfExtents = {0.5f, 0.5f, 0.5f};
		// Shape centre relative to the entity's origin. Offsetting lets one
		// entity carry several colliders at different places without child
		// entities.
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		// The live Box3D shape handle, packed like RuntimeBody. Zero means not yet
		// created.
		//
		// NOTE: unlike the 2D colliders, there is no RestitutionThreshold here --
		// so a 3D resting stack has no protection against the overlap-and-bounce
		// jitter that the threshold exists to solve in 2D. A stack of 3D boxes
		// that creeps or buzzes at rest is the expected consequence.
		//
		// HAZARD: the defaulted copy constructor duplicates this handle, so a
		// copied collider aliases one shape.
		uint64_t RuntimeShape = 0;

		BoxCollider3DComponent() = default;
		BoxCollider3DComponent(const BoxCollider3DComponent &other) = default;
	};

	// A spherical collision shape in the 3D simulation.
	//
	// The cheapest 3D shape for the solver -- it cannot be caught on an edge,
	// so fewer contact manifolds than an equivalent box. That makes it the
	// usual choice for character and projectile bodies.
	struct SphereCollider3DComponent
	{
		// Collision radius, multiplied by the LARGEST of the entity's three scale
		// components when the shape is created in Scene::CreatePhysicsBody3D:
		//     sphere.radius = sc.Radius * std::max({Scale.x, Scale.y, Scale.z});
		//
		// max() rather than a per-axis product, because a sphere cannot be
		// non-uniformly scaled -- this makes it circumscribe the visual mesh under
		// non-uniform scale, which is the conservative choice for collision.
		//
		// Baked at creation time: changing Scale later does NOT resize the shape.
		float Radius = 0.5f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		uint64_t RuntimeShape = 0;

		SphereCollider3DComponent() = default;
		SphereCollider3DComponent(const SphereCollider3DComponent &other) = default;
	};

	// A capsule collision shape -- a cylinder with hemispherical caps.
	//
	// The usual collision shape for character controllers, because a capsule
	// slides along a wall and over a step without snagging, where a box
	// catches on every edge. Combined with RigidBody3DComponent's per-axis
	// rotation lock it gives stable movement over uneven terrain.
	struct CapsuleCollider3DComponent
	{
		// Radius of the capsule's cylindrical section, which is also the radius of
		// both caps. Multiplied by the larger of the two RADIAL scale components:
		//     r = cc.Radius * std::max(transform.Scale.x, transform.Scale.z);
		//
		// X and Z only -- Y scales the capsule's LENGTH instead (see Height), so
		// the two fields are scaled independently and a non-uniform scale gives a
		// correspondingly non-uniform capsule.
		//
		// Baked at creation time: changing Scale later does NOT resize the shape.
		float Radius = 0.5f;
		// Height of the CYLINDRICAL section only, excluding the two hemispherical
		// caps.
		//
		// Total capsule height is Height + 2 * Radius, so at the defaults it is
		// 2.0, not 1.0. A caller reading Height as the total height gets a
		// capsule twice as tall as intended.
		//
		// Multiplied by the entity's Y scale component when the shape is created:
		//     halfH = cc.Height * 0.5f * transform.Scale.y;
		//
		// So the shape's cylindrical half-length is Height*Scale.y*0.5 and its
		// total height is (Height*Scale.y) + 2*(Radius*max(Scale.x, Scale.z)) --
		// i.e. the total is NOT simply Height + 2*Radius once Scale is non-unit.
		//
		// Baked at creation time: changing Scale later does NOT resize the shape.
		float Height = 1.0f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};

		float Density = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;

		// Runtime handle (stores b3ShapeId as uint64_t)
		uint64_t RuntimeShape = 0;

		CapsuleCollider3DComponent() = default;
		CapsuleCollider3DComponent(const CapsuleCollider3DComponent &other) = default;
	};

	// --- Volumetric Atmosphere Components ---
	// The entity's Transform defines the volume box: Translation = center,
	// Scale = full box size (a unit cube scaled by Scale, axis-aligned in world).
	// Renderer3D gathers all enabled volumes every frame and raymarches them
	// in both the Nova path-tracer and the raster (Basic) path.

	// A raymarched box volume of fog hanging in the world.
	//
	// The entity's Transform defines the box: Translation is the centre and
	// Scale is the FULL size of the volume (a unit cube scaled by Scale).
	// Scale is therefore a size, not a scale factor -- halving Scale halves
	// the box's dimensions rather than its position.
	//
	// Requires a mesh to exist for the box: the engine has no implicit
	// volume primitive, so an entity with this component and no visible mesh
	// contributes nothing to the render.
	struct VolumetricFogComponent
	{
		bool Enabled = true;
		glm::vec3 Color = {0.7f, 0.75f, 0.8f};
		// How quickly the fog removes light along a ray -- the base extinction
		// coefficient, per world unit.
		//
		// Higher is thicker: 0.05 gives a light haze, and the optical depth
		// through a 20-unit volume is about 1, so distant geometry is roughly
		// half faded. 0 disables the volume entirely, so this doubles as the
		// enable switch for the effect.
		//
		// Beer-Lambert, so transmittance falls exponentially with distance
		// rather than linearly -- fog thickens along a ray faster than the
		// number suggests at a glance.
		float Density = 0.05f;       // Base extinction coefficient (0 = clear)
		// Forward-scattering bias of the medium, as a Henyey-Greenstein g value.
		//
		// Positive (the default 0.3) makes the fog brighter when looked toward
		// the light and dimmer when looked away from it -- the halo around a
		// sun seen through mist. Negative does the reverse and gives a dark
		// rim. 0 is isotropic.
		//
		// The documented range is -0.9 .. 0.9 rather than the mathematical -1..1:
		// at |g| -> 1 the phase function becomes a delta in the forward or
		// backward direction, which is physically an infinite-albedo
		// near-field scatter and looks like a hard glare, not fog.
		float Anisotropy = 0.3f;     // Henyey-Greenstein phase g (-0.9 .. 0.9, forward scatter)
		// How strongly density falls off with height within the volume.
		//
		// 0 -- the default -- is uniform fog. Non-zero concentrates it toward
		// the volume's bottom, which is what makes ground mist hug the floor
		// instead of filling the whole box evenly.
		//
		// Expressed relative to the volume's own height, so the same value looks
		// the same on a tall and a short volume.
		float HeightFalloff = 0.15f; // 0 = uniform, 1 = concentrated at volume bottom
		float NoiseStrength = 0.5f;  // 0 = uniform fog, 1 = fully broken up
		float NoiseScale = 0.25f;    // World-space noise frequency
		float WindSpeed = 0.05f;     // Noise drift speed (world units / second)
		// Raymarch step count inside this volume -- the quality/cost dial.
		//
		// Each step is a full sample of the noise field, so this multiplies the
		// fog's cost directly. 8 is the low end: enough for a soft, uniform
		// medium, but too coarse for a volume with high NoiseStrength, where the
		// break-up shows as visible banding rather than detail.
		//
		// This is per volume, so the total march cost is the sum over every
		// enabled volume in the scene, not this value alone.
		int MaxSteps = 8;            // Raymarch steps inside this volume (1 .. 32)

		VolumetricFogComponent() = default;
		VolumetricFogComponent(const VolumetricFogComponent &other) = default;
	};

	// A raymarched box volume of cloud.
	//
	// Shares the Transform-defined box convention with VolumetricFogComponent
	// (Translation = centre, Scale = full size), and is gathered and
	// raymarched by the same Renderer3D pass -- the two are distinguished by
	// component type, not by a separate system.
	//
	// Needs a mesh for the box, as above.
	//
	// Defaults describe broken midday cumulus rather than an empty sky:
	struct VolumetricCloudsComponent
	{
		bool Enabled = true;
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		// Colour of light scattered into the cloud from the sky, applied to the
		// cloud's shadowed and interior regions.
		//
		// Blue by default (more blue than red or green), because Rayleigh
		// scattering in the real atmosphere tints shadowed cloud toward the sky
		// colour rather than toward grey. Without it, cloud shadows go flat and
		// neutral and the whole sky reads as synthetic.
		glm::vec3 AmbientTint = {0.45f, 0.55f, 0.7f};
		// How much of the sky the clouds occupy.
		//
		// 0.5 by default is scattered cumulus. Pushing toward 1 closes gaps
		// until the sky is a continuous overcast; toward 0 the cloud breaks into
		// isolated puffs. This is the cheapest single control over how the whole
		// sky reads.
		float Coverage = 0.5f;       // 0 = clear sky, 1 = overcast
		float Density = 0.6f;        // Extinction inside cloud mass
		float NoiseScale = 0.08f;    // World-space billow frequency
		float DetailAmount = 0.5f;   // High-frequency erosion (0 = smooth puffs)
		glm::vec2 WindDirection = {1.0f, 0.3f};
		float WindSpeed = 0.02f;     // Drift speed (world units / second)
		// Strength of the bright forward-scattered rim where the sun sits behind
		// a cloud edge.
		//
		// The silver lining. Physically it is the same Mie forward-scattering
		// that Anisotropy gives the fog, specialised to the sun's direction and
		// boosted. 0 removes it and cloud edges go flat and cut-out.
		float SilverLining = 0.6f;   // Forward-scatter rim around sun (0 .. 1)
		float ShadowStrength = 0.7f; // Self-shadowing depth (0 = flat, 1 = dark cores)
		// Raymarch step count, as for VolumetricFogComponent::MaxSteps.
		//
		// Higher default than fog's 8 because cloud is a harder volume to march:
		// its interior has real self-shadowing (see ShadowStrength) that a coarse
		// march resolves as noise rather than depth.
		//
		// Clouds are the single most expensive thing in the renderer -- they are
		// why MaxSteps is the dial to reach for when the frame time is dominated
		// by the sky.
		int MaxSteps = 12;           // Raymarch steps inside this volume (1 .. 32)

		VolumetricCloudsComponent() = default;
		VolumetricCloudsComponent(const VolumetricCloudsComponent &other) = default;
	};

	// --- Water Simulation Components ---
// Entity Transform defines the water body: Translation.y = still water level,
// Scale.x/z = surface extents (a 1x1 XZ plane at local y=0, scaled by Scale).
// Rivers are directional: FlowDirection/FlowSpeed advect the waves and drive
// foam; lakes are calm presets; oceans are large high-amplitude presets.
// CPU-simulated Gerstner spectrum (6 directional components with physical
// dispersion + horizontal chop) so gameplay queries (buoyancy, height
// sampling) and the raster mesh agree; the mesh additionally applies the
// Gerstner horizontal displacement for true sharp-crested waves.

// Which body of water this is. A preset selector, not a behaviour switch:
// ApplyPreset overwrites the authored fields from a table, so the type
// and those fields can disagree if either is edited afterwards.
enum class WaterBodyType
{
	Lake = 0,
	River = 1,
	Ocean = 2
};

// A body of water: an animated Gerstner surface plus the shading and
// gameplay parameters that go with it.
//
// The entity's Transform defines the surface's placement and extent:
// Translation.y is the still-water level, and Scale.x/z are the surface's
// extents (a 1x1 XZ plane at local y=0, scaled). Note this differs from the
// volumetric volumes above, where Translation was the box CENTRE.
//
// Three presets -- Lake, River, Ocean -- differing mainly in amplitude,
// wavelength, colour and flow. Rivers are directional (FlowSpeed drives
// wave advection and foam), lakes are calm, oceans are large and
// high-amplitude.
//
// The surface is simulated on the CPU as a directional Gerstner spectrum
// so that gameplay queries (buoyancy, height sampling) and the rendered
// mesh agree on where the water is. The mesh additionally applies the
// horizontal Gerstner displacement, which the gameplay heightfield does
// not -- so a crest is visibly sharp on screen while buoyancy sees the
// smooth heightfield under it.
struct WaterComponent
{
	bool Enabled = true;
	// Which preset this water was last set from.
	//
	// Bookkeeping for the editor's dropdown, not read by the renderer or the
	// simulation. Editing fields by hand does not update Type, so Type and the
	// actual field values can disagree.
	WaterBodyType Type = WaterBodyType::Lake;

	glm::vec3 ShallowColor = {0.25f, 0.5f, 0.65f};
	glm::vec3 DeepColor = {0.02f, 0.15f, 0.3f};
	glm::vec3 FoamColor = {0.9f, 0.95f, 1.0f};

	// Depth over which the colour fades from ShallowColor to DeepColor.
	//
	// A colour-absorption depth rather than the body's geometric depth: it
	// sets how quickly shallow water reads turquoise and deep water reads
	// dark blue. Does not need to match the actual basin depth.
	//
	// Should be roughly the scale at which the ground beneath is visible; too
	// small and the surface looks painted on, too large and everything reads
	// as shallow.
	float WaterDepth = 2.0f;      // Absorption depth for shallow/deep gradient
	float WaveAmplitude = 0.05f;  // World units, primary wave height
	// Wavelength of the PRIMARY (longest, strongest) wave component, in
	// world units. The other five spectrum components are fractions of it.
	//
	// Must exceed 0.01 -- the simulation divides by it, so a zero or
	// near-zero wavelength produces inf/NaN wavelengths and then NaN
	// surface normals. Clamped to a floor of 0.1 inside EvaluateSurface as a
	// backstop, but authoring a tiny wavelength there wastes the clamp.
	//
	// This is the dominant scale control for the whole surface: it sets both
	// how large the swell is and how much chop is packed between crests.
	float WaveLength = 3.0f;      // World units, primary wavelength (> 0.01)
	float WaveSpeed = 0.6f;       // Phase speed multiplier
	glm::vec2 WaveDirection = {1.0f, 0.3f};
	float Chop = 0.2f;            // Secondary detail waves (0 = glassy .. 1 = choppy)
	// How much the Gerstner waves displace HORIZONTALLY, converting a
	// sine-like swell into sharp-crested waves with overhanging faces.
	//
	// This is the visual difference between a gentle roll and a breaking sea,
	// and it is why the surface is not a plain sine: horizontal displacement
	// is what lets a crest lean forward past vertical. 0 is pure sine and
	// reads as an oil-slick calm; high values give pointed crests.
	//
	// Distributed across components as a per-wave Q (see EvaluateSurface).
	float Steepness = 0.1f;       // Gerstner horizontal chop (0 = sine swell .. 1 = sharp crests)

	glm::vec2 FlowDirection = {1.0f, 0.0f}; // River current dir in world XZ
	float FlowSpeed = 0.0f;       // World units/second advection (rivers > 0)
	float FoamAmount = 0.15f;     // 0 = none .. 1 = heavy foam on crests
	float FoamScale = 1.0f;       // Foam pattern frequency multiplier
	float ShoreFoam = 0.5f;       // Bank/shore foamline (0 = none .. 1 = strong)
	float ShoreWidth = 1.0f;      // World-unit width of the shoreline foam band

	float Transparency = 0.75f;   // 0 = opaque .. 1 = clear (tints raster albedo toward shallow)
	float Roughness = 0.1f;       // Specular sharpness (0 = mirror .. 1 = matte)
	float Metallic = 0.0f;

	int Segments = 48;            // Surface grid resolution per side (1 .. 128)
	float TimeScale = 1.0f;       // Simulation speed multiplier

	float Buoyancy = 1.0f;        // Upward force scale for gameplay scripts
	float WaterDensity = 1.0f;    // Drag/density scale for gameplay scripts

	// Elapsed simulation time, advanced each frame and used as the phase
	// clock in EvaluateSurface.
	//
	// Deliberately NOT serialized: a saved scene must not load with the
	// water already partway through a wave cycle, or it would visibly jump
	// on load. It restarts at 0 and rebuilds from there.
	//
	// Scaled by TimeScale and WaveSpeed when it reaches the wave functions, so
	// this is a raw accumulator and not the phase itself.
	float Time = 0.0f;            // Runtime accumulator (not serialized)

	WaterComponent() = default;
	WaterComponent(const WaterComponent &other) = default;

	// Overwrites water's fields from a built-in preset table and sets its Type.
	//
	// DESTRUCTIVE by design: it assigns the preset's values over whatever was
	// there, so calling it on a hand-tuned river replaces every wave, colour
	// and foam setting with the table's.
	//
	// It also sets Type as a side effect, which is what keeps the editor's
	// dropdown consistent with the values -- but it means a preset can never
	// be applied as a starting point without losing the edits.
	//
	// Fields the table does not mention keep their current values, so a preset
	// is a partial override rather than a complete reset. Notably Segments,
	// Transparency's interaction with the other fields, Metallic, Buoyancy,
	// WaterDensity and TimeScale are untouched -- so those stay whatever they
	// were, which is inconsistent if the expectation is a clean slate.
	//
	// NOTE: FoamColor is never assigned by any preset, so the field's
	// initializer is the only thing that ever sets it.
	//
	// The switch has no default case and covers every enumerator, so it is
	// exhaustive today; adding an enumerator without a case leaves that body
	// entirely untouched with no compiler warning to notice it.
	static void ApplyPreset(WaterComponent &water, WaterBodyType type)
	{
		water.Type = type;
		switch (type)
		{
		case WaterBodyType::Lake:
			water.ShallowColor = {0.25f, 0.5f, 0.65f};
			water.DeepColor = {0.02f, 0.15f, 0.3f};
			water.WaterDepth = 2.0f;
			water.WaveAmplitude = 0.05f;
			water.WaveLength = 3.0f;
			water.WaveSpeed = 0.6f;
			water.WaveDirection = {1.0f, 0.3f};
			water.Chop = 0.2f;
			water.Steepness = 0.1f;
			water.FlowDirection = {1.0f, 0.0f};
			water.FlowSpeed = 0.0f;
			water.FoamAmount = 0.15f;
			water.ShoreFoam = 0.5f;
			water.ShoreWidth = 1.0f;
			water.Transparency = 0.75f;
			water.Roughness = 0.1f;
			break;
		case WaterBodyType::River:
			water.ShallowColor = {0.3f, 0.55f, 0.6f};
			water.DeepColor = {0.03f, 0.2f, 0.25f};
			water.WaterDepth = 1.2f;
			water.WaveAmplitude = 0.08f;
			water.WaveLength = 2.0f;
			water.WaveSpeed = 1.6f;
			water.WaveDirection = {1.0f, 0.0f};
			water.Chop = 0.5f;
			water.Steepness = 0.35f;
			water.FlowDirection = {1.0f, 0.0f};
			water.FlowSpeed = 1.5f;
			water.FoamAmount = 0.5f;
			water.ShoreFoam = 0.7f;
			water.ShoreWidth = 0.8f;
			water.Transparency = 0.6f;
			water.Roughness = 0.15f;
			break;
		case WaterBodyType::Ocean:
			water.ShallowColor = {0.15f, 0.45f, 0.65f};
			water.DeepColor = {0.005f, 0.08f, 0.22f};
			water.WaterDepth = 6.0f;
			water.WaveAmplitude = 0.35f;
			water.WaveLength = 12.0f;
			water.WaveSpeed = 1.2f;
			water.WaveDirection = {1.0f, 0.25f};
			water.Chop = 0.7f;
			water.Steepness = 0.6f;
			water.FlowDirection = {1.0f, 0.25f};
			water.FlowSpeed = 0.3f;
			water.FoamAmount = 0.4f;
			water.ShoreFoam = 0.3f;
			water.ShoreWidth = 2.0f;
			water.Transparency = 0.85f;
			water.Roughness = 0.08f;
			break;
		}
	}

	// --- Directional wave spectrum (6 components, heightfield) ---
	// Amplitudes follow a Phillips-style falloff from the primary swell;
	// Chop scales the high-frequency detail (0 = glassy single swell).
	// Each component carries a 2nd harmonic for peaked crests / flat
	// troughs, and deep-water dispersion ω=sqrt(g·k) so long swells
	// outrun chop — the same field drives gameplay queries and the mesh.
	// Number of directional wave components in the spectrum.
	//
	// Six is the balance point between cost and detail: the surface is
	// raymarched/sampled by every vertex and every gameplay query, so the
	// cost is linear in this. Every Spectrum* lookup table below has exactly
	// this many entries.
	//
	// Constexpr, so these tables can be sized by it rather than being a
	// separate magic number that could drift out of step.
	static constexpr int WaveComponentCount = 6;
	// Wavelength of component i as a fraction of WaveLength.
	//
	// A Phillips-style falloff: each successive component is roughly half as
	// long as the last, so the spectrum spans a wide range of scales from one
	// authored WaveLength value.
	//
	// OUT-OF-RANGE INDICES ARE SILENTLY CLAMPED -- negative maps to entry 0
	// and >= WaveComponentCount maps to the LAST entry, rather than being an
	// error. A caller passing an out-of-range index therefore gets a plausible
	// wrong value with no diagnostic. Every Spectrum* function below does
	// this, so it is consistent, but none of them will ever tell you the index
	// was wrong.
	static float SpectrumLengthRatio(int i)
	{
		constexpr float ratios[WaveComponentCount] = {1.0f, 0.55f, 0.32f, 0.19f, 0.11f, 0.06f};
		return ratios[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumShare(int i)
	{
		constexpr float shares[WaveComponentCount] = {0.55f, 0.22f, 0.11f, 0.06f, 0.035f, 0.025f};
		return shares[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumFanAngle(int i)
	{
		constexpr float angles[WaveComponentCount] = {0.0f, 0.40f, -0.54f, 0.82f, -1.01f, 1.24f};
		return angles[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}
	static float SpectrumPhaseOffset(int i)
	{
		constexpr float phases[WaveComponentCount] = {0.0f, 1.3f, 4.1f, 2.2f, 5.0f, 0.7f};
		return phases[i < 0 ? 0 : (i >= WaveComponentCount ? WaveComponentCount - 1 : i)];
	}

	// Total possible crest height (culling padding + foam normalization)
	float MaxWaveHeight() const
	{
		return WaveAmplitude * (0.55f + 0.45f * glm::clamp(Chop, 0.0f, 1.0f));
	}

	// Advected sample position: the current carries the wave field with it
	glm::vec2 AdvectedPos(glm::vec2 worldXZ) const
	{
		float flowLen = glm::length(FlowDirection);
		if (FlowSpeed > 0.0001f && flowLen > 0.0001f)
			return worldXZ - (FlowDirection / flowLen) * (FlowSpeed * Time);
		return worldXZ;
	}

	// Evaluates the Gerstner surface in one trig pass: vertical height +
	// horizontal chop displacement (outDispX/Z, world units) + analytic
	// Gerstner normal. minWavelength fades out components shorter than the
	// mesh can resolve (spectral LOD: pass ~2x the mesh cell size from the
	// renderer; 0 keeps the full spectrum for gameplay queries).
	// Evaluates the whole surface at one world XZ position in a single pass
	// over all WaveComponentCount components.
	//
	// Out-parameter list:
	//   outHeight   vertical displacement from the still-water level
	//   outDispX/Z  HORIZONTAL Gerstner displacement -- the sharp-crest term
	//   outNormal   analytic surface normal, normalized
	//
	// minWavelength is the spectral LOD control: components shorter than it
	// are faded out, because a mesh or query cannot resolve them and they
	// only alias. Pass roughly twice the mesh cell size from the renderer;
	// pass 0 to keep the full spectrum, which is what gameplay queries do
	// because they sample a continuous position rather than a mesh.
	//
	// Cost is linear in WaveComponentCount and this runs per mesh vertex AND
	// per gameplay sample, so it is the water simulation's hot path.
	//
	// TWO INDEPENDENT FREQUENCY CLAMPS, both undocumented in the fields above:
	//   k (wavenumber) is clamped to 40 rad/m, capping the shortest component
	//     at about 0.157 world units; and omega is clamped to 25 rad/s.
	//   These overlap with each other and with minWavelength, so a component
	//   can be faded, k-clamped, and omega-clamped at once.
	//
	// INCONSISTENCY WORTH KNOWING: the per-wave Gerstner Q below divides by
	// WaveComponentCount as the equal-steepness distribution requires, but
	// low-frequency components are SKIPPED when their amplitude underflows
	// (amp = WaveAmplitude * share * chop, and chop is 0 for glassy water).
	// At chop = 0 only component 0 survives, yet Q is still divided by 6 --
	// so horizontal chop displacement is about 1/6 of the intended amount,
	// and it recovers continuously as Chop rises. Visible as: raising Chop
	// from 0 produces far less crest sharpening at first than the numbers
	// suggest.
	//
	// The dispersion relation omega = sqrt(g*k) is computed from the ALREADY
	// k-clamped value, so a k-clamped component gets a slightly wrong phase
	// speed. Harmless in practice since those components are being suppressed
	// anyway.
	//
	// Early-outs, in order: disabled or a negligible amplitude returns the
	// neutral values (height 0, no displacement, normal straight up) rather
	// than a partial result, so callers can treat a disabled water body as a
	// flat surface without testing Enabled first.
	void EvaluateSurface(float worldX, float worldZ, float minWavelength, float &outHeight, float &outDispX, float &outDispZ, glm::vec3 &outNormal) const
	{
		outHeight = 0.0f;
		outDispX = 0.0f;
		outDispZ = 0.0f;
		outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		if (!Enabled || WaveAmplitude <= 0.00001f)
			return;
		glm::vec2 p = AdvectedPos({worldX, worldZ});

		glm::vec2 d1 = WaveDirection;
		if (glm::length2(d1) < 1e-8f)
			d1 = {1.0f, 0.0f};
		d1 = glm::normalize(d1);

		float chop = glm::clamp(Chop, 0.0f, 1.0f);
		float steep = glm::clamp(Steepness, 0.0f, 1.0f);
		float baseLen = glm::max(WaveLength, 0.1f);
		float t = Time * WaveSpeed;

		float nx = 0.0f, ny = 1.0f, nz = 0.0f;
		for (int i = 0; i < WaveComponentCount; i++)
		{
			float li = baseLen * SpectrumLengthRatio(i);
			float fade = 1.0f;
			if (minWavelength > 0.00001f && li < minWavelength * 2.0f)
			{
				if (li <= minWavelength)
					continue;
				fade = (li - minWavelength) / minWavelength; // 0..1 across the Nyquist band
			}
			float amp = WaveAmplitude * SpectrumShare(i) * (i == 0 ? 1.0f : chop) * fade;
			if (amp <= 0.0000001f)
				continue;
			float k = 6.2831853f / li;
			if (k > 40.0f)
				k = 40.0f;
			float omega = sqrtf(9.81f * k); // deep-water dispersion: swells outrun chop
			if (omega > 25.0f)
				omega = 25.0f;

			float ca = cosf(SpectrumFanAngle(i));
			float sa = sinf(SpectrumFanAngle(i));
			glm::vec2 dir(d1.x * ca - d1.y * sa, d1.x * sa + d1.y * ca);

			float sharp = (i == 0 ? 0.6f : 1.0f) * chop; // 2nd-harmonic crest sharpening
			float norm = 1.0f + 0.5f * sharp;
			float phi = k * glm::dot(dir, p) - omega * t;
			float s1 = sinf(phi);
			float c1 = cosf(phi);
			float harmonic = 2.0f * phi + SpectrumPhaseOffset(i);
			float s2 = sinf(harmonic);
			float c2 = cosf(harmonic);

			float wa = k * amp;
			// Per-wave Gerstner Q: equal-steepness distribution (GPU Gems),
			// clamped so crests sharpen without looping over.
			float qi = steep / (wa * (float)WaveComponentCount + 1e-6f);
			if (qi > 1.0f)
				qi = 1.0f;
			if (qi < 0.0f)
				qi = 0.0f;

			float shapeS = (s1 + 0.5f * sharp * s2) / norm;
			float shapeC = (c1 + sharp * c2) / norm;
			outHeight += amp * shapeS;
			outDispX += dir.x * (qi * amp) * shapeC;
			outDispZ += dir.y * (qi * amp) * shapeC;
			nx += -dir.x * wa * shapeC;
			nz += -dir.y * wa * shapeC;
			ny += -qi * wa * shapeS;
		}
		outNormal = glm::vec3(nx, ny, nz);
		if (glm::length2(outNormal) < 1e-10f)
			outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		else
			outNormal = glm::normalize(outNormal);
	}

	// Heightfield column height (no horizontal displacement): what gameplay
	// queries and buoyancy use — stable by construction, matches the mesh
	// surface to within the chop offset.
	// Vertical height only -- the heightfield, with no horizontal
	// displacement.
	//
	// This is what gameplay uses for buoyancy and for "is the player in the
	// water" tests. Stable by construction: the horizontal term of
	// EvaluateSurface is discarded, so a crest's sideways lean cannot shift
	// the reported height under a moving query.
	//
	// It therefore DISAGREES with the rendered surface by up to the chop
	// offset -- buoyancy tracks the smooth heightfield while the mesh shows
	// sharp crests. That split is deliberate (it is what makes gameplay
	// queries stable) but it means a player can appear to float slightly
	// below the drawn waterline on steep waves.
	//
	// Computes all three outputs of EvaluateSurface and discards two of them,
	// so it is not cheaper than calling EvaluateSurface directly despite
	// returning less.
	float SampleHeightAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float h = 0.0f, dx = 0.0f, dz = 0.0f;
		glm::vec3 n(0.0f, 1.0f, 0.0f);
		EvaluateSurface(worldX, worldZ, minWavelength, h, dx, dz, n);
		return h;
	}

	glm::vec3 SampleNormalAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float h = 0.0f, dx = 0.0f, dz = 0.0f;
		glm::vec3 n(0.0f, 1.0f, 0.0f);
		EvaluateSurface(worldX, worldZ, minWavelength, h, dx, dz, n);
		return n;
	}

	// 0 = trough .. 1 = crest, for foam whitening
	float CrestFactorAt(float worldX, float worldZ, float minWavelength = 0.0f) const
	{
		float maxH = MaxWaveHeight();
		if (maxH <= 0.00001f)
			return 0.0f;
		return glm::clamp(SampleHeightAt(worldX, worldZ, minWavelength) / maxH * 0.5f + 0.5f, 0.0f, 1.0f);
	}

	glm::vec2 SampleFlow() const
	{
		float flowLen = glm::length(FlowDirection);
		if (flowLen < 0.0001f || FlowSpeed <= 0.00001f)
			return glm::vec2(0.0f);
		return (FlowDirection / flowLen) * FlowSpeed;
	}
};

// --- Analytic Light Components ---
	// Gathered every frame by the Scene and uploaded to Renderer3D (up to
	// Renderer3D::MaxAnalyticLights, first-submitted wins). Unlike emissive
	// materials they need no geometry. Aim with the entity rotation: light
	// travels along local -Z. Shadowed in the Nova path; unshadowed in the
	// raster preview path (which has no shadow maps).

	// An infinitely distant light with parallel rays -- the sun.
	//
	// Aim it with the entity's rotation: light travels along the entity's
	// local -Z, the same convention as a camera's forward. An unrotated
	// directional light therefore shines straight down.
	//
	// Needs no geometry and no range, and is the one light type that is
	// effectively always worth having -- it is what the sky's
	// SilverLining forward-scatters toward.
	//
	// Up to Renderer3D::MaxAnalyticLights are submitted per frame and the
	// FIRST-submitted wins, so beyond that limit lights are silently dropped
	// with no warning. Ordering therefore decides which lights survive.
	struct DirectionalLightComponent
	{
		// Light colour, RGB.
		//
		// Warm rather than pure white, matching sunlight rather than an ideal
		// illuminant. It multiplies Intensity, so the two together set both hue
		// and brightness.
		glm::vec3 Color = {1.0f, 0.96f, 0.9f};
		// Brightness multiplier.
		//
		// 1.5 rather than 1.0 because the sun is far brighter than the other
		// light types' defaults (point 5.0, spot 8.0, area 4.0) and the renderer
		// exposes no exposure control on these components to compensate --
		// intensity is where that compensation lives, and the presets encode it
		// in the per-type defaults.
		float Intensity = 1.5f;

		DirectionalLightComponent() = default;
		DirectionalLightComponent(const DirectionalLightComponent &other) = default;
	};

	// An omnidirectional point light at the entity's position.
	//
	// Cheapest of the four analytic types and the one to reach for by default.
	// Needs no aiming; position comes from TransformComponent.
	//
	// Subject to the same MaxAnalyticLights first-wins cap as every other
	// analytic light.
	struct PointLightComponent
	{
		glm::vec3 Color = {1.0f, 0.9f, 0.8f};
		float Intensity = 5.0f;
		// Distance at which the light is cut off entirely, or 0 for no cutoff.
		//
		// A hard cutoff, not a fade: the light drops to exactly zero at Range,
		// so a value much smaller than the space it lights produces a visible
		// sphere boundary. 0 removes the cutoff and is the safer default for
		// correctness, at some cost in shading work per pixel.
		float Range = 10.0f; // 0 = infinite
		// How quickly the light's pool narrows with distance.
		//
		// An exponent on the distance falloff, not a radius. 2.0 is physical
		// inverse-square, so doubling the distance quarters the brightness.
		// Raising it tightens the pool around the entity -- useful for making a
		// torch read as a torch -- at the cost of the light dying abruptly
		// instead of fading out.
		float Falloff = 2.0f; // Windowing exponent (higher = tighter pool)

		PointLightComponent() = default;
		PointLightComponent(const PointLightComponent &other) = default;
	};

	// A cone of light from the entity's position, aimed along its local -Z.
	//
	// Same aiming convention as DirectionalLightComponent -- rotate the entity
	// to point it. Needs a mesh or something to attach to, since unlike a
	// point light it has no meaningful position without a scene object.
	struct SpotLightComponent
	{
		glm::vec3 Color = {1.0f, 0.95f, 0.85f};
		float Intensity = 8.0f;
		float Range = 15.0f; // 0 = infinite
		float Falloff = 2.0f;
		// Half-angle, in degrees, of the fully-bright core at the cone's axis.
		//
		// The cone is not a hard edge: brightness falls off smoothly from this
		// angle out to OuterAngle, which is what gives a spotlight its soft
		// boundary. Setting the two equal produces a hard-edged cone.
		//
		// The comment records that this is clamped to OuterAngle; that clamp is
		// applied at submission time in the renderer, not here. Inverting them
		// silently gives an inverted falloff rather than an error.
		float InnerAngle = 25.0f; // Degrees, hot core (clamped <= outer)
		// Half-angle, in degrees, at which the light reaches zero.
		//
		// The full cone therefore spans 2 * OuterAngle -- 80 degrees at the
		// default, not 40.
		//
		// See InnerAngle for the clamping behaviour.
		float OuterAngle = 40.0f; // Degrees, cone edge

		SpotLightComponent() = default;
		SpotLightComponent(const SpotLightComponent &other) = default;
	};

	// A rectangular emitting surface -- a window, a softbox, a screen.
	//
	// Softest of the four analytic types, because it is a real emitting
	// surface rather than a point approximation, so shadows it casts have
	// correspondingly soft edges. The usual choice for interior fill.
	//
	// Lies in the entity's local XY plane, emitting along local -Z -- so
	// rotating the entity aims it, and the Width/Height extents rotate with it.
	struct AreaLightComponent
	{
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		float Intensity = 4.0f;
		// Extent along the entity's local X, in world units.
		//
		// A true size, not a scale factor, and not affected by
		// TransformComponent::Scale -- consistent with the collider components
		// above. A window 2 units wide is 2 units wide whatever the entity's
		// transform says.
		float Width = 2.0f;  // Rect size in world units (local XY plane)
		float Height = 2.0f;
		// Hard cutoff distance, or 0 for none. See PointLightComponent::Range.
		float Range = 12.0f; // 0 = infinite
		// Whether the rectangle emits from both faces.
		//
		// False by default, so the light is one-sided -- which matches a wall or
		// a window and halves the shading cost. True makes it emit both ways,
		// turning the emitter into a flat glowing slab, and is the right choice
		// for a light panel meant to be seen as well as to light.
		bool DoubleSided = false; // Emit from both rect faces

		AreaLightComponent() = default;
		AreaLightComponent(const AreaLightComponent &other) = default;
	};

	// --- Gameplay Components ---
	// Pure data, driven entirely by scripts (ScriptEngine calls
	// OnUpdateEntity each frame). No C++ behaviour: the engine's job is to
	// store the numbers and let the game layer decide what they mean.

	// Marks an entity as damageable and targetable by raycast hits.
	// A Raycast against this entity reports it as a hit so weapons can
	// apply HealthComponent damage.
	struct HealthComponent
	{
		float Current = 100.0f;
		float Max = 100.0f;
		bool Invulnerable = false;
		// Seconds of invulnerability after taking damage (prevents a
		// hitscan from applying its damage every frame it is held down).
		float DamageCooldown = 0.0f;
		// Runtime accumulator, not serialized.
		float CooldownTimer = 0.0f;
		// True once Current reached 0; scripts use this to drive death.
		bool IsDead = false;

		HealthComponent() = default;
		// Copies the authored numbers but not the live damage lockout, so a
		// duplicate/serialized copy does not inherit a transient cooldown.
		HealthComponent(const HealthComponent &other)
			: Current(other.Current), Max(other.Max), Invulnerable(other.Invulnerable),
			  DamageCooldown(other.DamageCooldown), IsDead(other.IsDead) {}

		// Returns the damage actually applied (0 when blocked/dead).
		float ApplyDamage(float amount)
		{
			if (Invulnerable || IsDead || Current <= 0.0f || amount <= 0.0f)
				return 0.0f;
			if (CooldownTimer > 0.0f)
				return 0.0f;

			float applied = amount > Current ? Current : amount;
			Current -= applied;
			if (Current <= 0.0f)
			{
				Current = 0.0f;
				IsDead = true;
			}
			if (DamageCooldown > 0.0f)
				CooldownTimer = DamageCooldown;
			return applied;
		}

		void Heal(float amount)
		{
			if (IsDead || amount <= 0.0f)
				return;
			Current = (Current + amount) > Max ? Max : (Current + amount);
		}

		void Reset()
		{
			Current = Max;
			IsDead = false;
			CooldownTimer = 0.0f;
		}
	};

	// A hitscan weapon. Fire logic lives in the script: it reads FireRate
	// to gate the trigger, calls Fire() to consume a round, and the script
	// then performs a Raycast from the camera and applies damage to whatever
	// HealthComponent it hits.
	struct WeaponComponent
	{
		float FireRate = 8.0f;  // Rounds per second (0 or less = no limit)
		float Damage = 25.0f;
		float Range = 200.0f;   // World units the hitscan trace reaches
		int AmmoPerShot = 1;

		int CurrentAmmo = 30;
		int MaxAmmo = 30;
		// Reserve ammo reloaded from on death of the magazine.
		int ReserveAmmo = 120;

		float ReloadTime = 1.5f;
		bool AutoReload = true;

		// Runtime accumulators, not serialized.
		float FireCooldown = 0.0f;
		float ReloadTimer = 0.0f;
		bool IsReloading = false;
		// Incremented on every accepted shot; scripts can watch it to spawn
		// muzzle flash / recoil without polling the fire input themselves.
		uint32_t ShotsFired = 0;

		WeaponComponent() = default;
		// Same reasoning as health: the live cooldown/reload/shot-counter
		// state is runtime-only and must not ride along on a copy.
		WeaponComponent(const WeaponComponent &other)
			: FireRate(other.FireRate), Damage(other.Damage), Range(other.Range),
			  AmmoPerShot(other.AmmoPerShot), CurrentAmmo(other.CurrentAmmo),
			  MaxAmmo(other.MaxAmmo), ReserveAmmo(other.ReserveAmmo),
			  ReloadTime(other.ReloadTime), AutoReload(other.AutoReload) {}

		bool CanFire() const
		{
			return !IsReloading && FireCooldown <= 0.0f && CurrentAmmo >= AmmoPerShot;
		}

		// Consumes a round. Returns false when the shot was rejected
		// (still cooling down, reloading, or out of ammo).
		bool Fire()
		{
			if (!CanFire())
				return false;
			CurrentAmmo -= AmmoPerShot;
			ShotsFired++;
			if (FireRate > 0.0f)
				FireCooldown = 1.0f / FireRate;
			if (CurrentAmmo <= 0 && AutoReload && ReserveAmmo > 0)
				StartReload();
			return true;
		}

		bool CanReload() const
		{
			return !IsReloading && CurrentAmmo < MaxAmmo && ReserveAmmo > 0;
		}

		void StartReload()
		{
			if (!CanReload())
				return;
			IsReloading = true;
			ReloadTimer = ReloadTime;
		}

		// Ticks the cooldowns. Returns true on the frame the reload finished
		// so the caller can play a "reload complete" cue.
		bool Tick(float dt)
		{
			if (FireCooldown > 0.0f)
			{
				FireCooldown -= dt;
				if (FireCooldown < 0.0f)
					FireCooldown = 0.0f;
			}

			if (!IsReloading)
				return false;

			ReloadTimer -= dt;
			if (ReloadTimer > 0.0f)
				return false;

			IsReloading = false;
			ReloadTimer = 0.0f;
			int needed = MaxAmmo - CurrentAmmo;
			int taken = needed < ReserveAmmo ? needed : ReserveAmmo;
			CurrentAmmo += taken;
			ReserveAmmo -= taken;
			return true;
		}

		void Reset()
		{
			CurrentAmmo = MaxAmmo;
			FireCooldown = 0.0f;
			ReloadTimer = 0.0f;
			IsReloading = false;
		}
	};

	// A raycast-ignorable entity. Entities carrying this are skipped by
	// Scene::Raycast, which is how a player avoids shooting their own
	// collider: the player's capsule is a real physics shape, so a trace
	// starting inside it would otherwise always report a hit at distance 0.
	struct RaycastIgnoreComponent
	{
		// Enabled allows toggling the marker without adding or removing the
		// component. The bool is not decoration: entt stores zero-sized
		// types as presence-only, and for those get()/try_get() return
		// void, which Entity::GetComponent<T>() cannot return to a caller.
		// A real member is what makes this an ordinary storable component.
		bool Enabled = true;

		RaycastIgnoreComponent() = default;
		RaycastIgnoreComponent(const RaycastIgnoreComponent &other) = default;
	};

	// Marks an entity as a hitscan target. Raycast reports outHitHittable
	// only for entities carrying this or a HealthComponent, so world
	// geometry (floors, walls) does not read as a shootable target.
	struct HittableComponent
	{
		// Required for the same entt reason as RaycastIgnoreComponent::Enabled.
		bool Enabled = true;

		HittableComponent() = default;
		HittableComponent(const HittableComponent &other) = default;
	};

	// Marks an entity as interactable ("press E"). Interactor.py traces
	// forward, and only stops at entities carrying this, so level geometry
	// never absorbs an interaction keypress.
	//
	// Label is the on-screen prompt ("[E] Open Door"). It lives on the
	// component rather than in the HUD so the interactor script owns the
	// prompt text and the HUD merely renders whatever is currently set --
	// one writer, one reader, no disagreement.
	struct InteractableComponent
	{
		// Required for the same entt reason as RaycastIgnoreComponent::Enabled.
		bool Enabled = true;

		// Authored in the editor; cleared by the interactor when the player
		// looks away, then rewritten each frame while in range.
		std::string Label = "[E] Interact";

		InteractableComponent() = default;
		InteractableComponent(const InteractableComponent &other) = default;
	};

	// Transient weapon feedback, read by the HUD to render the muzzle flash
	// and the hit marker.
	//
	// These are components rather than fields on WeaponComponent because they
	// are *presentation* state: the weapon script writes them, the HUD reads
	// them, and neither owns the other. Folding them into WeaponComponent
	// would make every weapon durable data carry a field that is only
	// meaningful for one frame.
	//
	// The timers count DOWN, so a value of 0 means "not showing". That makes
	// the HUD's test a single comparison and means an un-ticked entity
	// naturally stops showing the effect.
	struct WeaponFeedbackComponent
	{
		// Seconds of muzzle flash remaining. Set to MuzzleFlashDuration on
		// fire, decremented each frame.
		float MuzzleFlashTimer = 0.0f;
		// Seconds of hit marker remaining. Set on a confirmed hit only, so
		// a miss does not flash the marker.
		float HitMarkerTimer = 0.0f;
		// True on the exact frame the shot connected, so the marker can play
		// a different sound/animation for a kill.
		bool LastShotHit = false;
		// True when the shot that produced the current HitMarkerTimer killed
		// the target. Lets the HUD style a kill differently from a body hit.
		bool LastShotKilled = false;

		// Flash duration for a normal shot.
		float MuzzleFlashDuration = 0.05f;
		// Marker duration. Deliberately longer than the flash: the flash is
		// "a gun went off", the marker is "you hit something", and the player
		// needs the second to land after the first has gone.
		float HitMarkerDuration = 0.25f;

		// Seconds remaining in the current reload, for the HUD's reload
		// progress. 0 when not reloading.
		float ReloadProgress = 0.0f;
		// Total length of the reload in progress, for the progress fraction.
		float ReloadTotal = 0.0f;

		WeaponFeedbackComponent() = default;
		WeaponFeedbackComponent(const WeaponFeedbackComponent &other) = default;
	};

	// Camera state the player script drives and the HUD reads: what the
	// field of view is doing and why.
	//
	// Kept apart from the Camera component because the camera itself has no
	// opinion about *why* its FOV is what it is. The script knows the player
	// is sprinting or aiming; the camera just needs the number.
	struct ViewStateComponent
	{
		// Set by the player script each frame. The player script is the sole
		// writer so FOV cannot be fought over between two systems.
		float TargetFOV = 1.04719755f; // 60 degrees, the default
		// How fast the camera closes on TargetFOV, in FOV units per second.
		// A non-zero value is what makes the transition read as a smooth
		// push-in rather than a snap.
		float FOVLerpSpeed = 8.0f;
		// True while the player is sprinting. Purely informational for the
		// HUD, which may want to tint the screen edge.
		bool IsSprinting = false;
		// True while the player is aiming (right mouse held).
		bool IsAiming = false;

		ViewStateComponent() = default;
		ViewStateComponent(const ViewStateComponent &other) = default;
	};

	// =====================================================================
	// Inventory
	//
	// One stack of one item. Quantity is stored per stack rather than as
	// "N copies of item id X in a flat list" so that a stack limit is
	// expressible, and so the HUD can render "Ammo x30" from one row.
	// =====================================================================
	struct ItemStack
	{
		std::string ItemId;
		int Quantity = 1;

		ItemStack() = default;
		ItemStack(const std::string &id, int quantity)
			: ItemId(id), Quantity(quantity)
		{
		}

		bool IsValid() const { return !ItemId.empty() && Quantity > 0; }

		bool operator==(const ItemStack &other) const
		{
			return ItemId == other.ItemId && Quantity == other.Quantity;
		}
	};

	// Per-item authored properties. Kept SEPARATE from InventoryComponent so
	// that "what is a medkit" is data authored once on a prefab, while
	// "what the player is carrying" is the mutable per-entity state. Folding
	// MaxStack into InventoryComponent would make every carried item carry
	// fields only meaningful for its definition.
	struct ItemDefinitionComponent
	{
		// The lookup key. Unique across the scene.
		//
		// Separate from DisplayName because the two change for different
		// reasons: a designer relabelling "Medkit" to "Field Dressing" must
		// NOT break every inventory reference to it. Scripts address items by
		// Name; the player only ever sees DisplayName.
		std::string Name = "item";

		std::string DisplayName = "Item";
		std::string Description;

		// How many of this item fit in one stack. <= 0 means "no limit",
		// which is what a quest item or a unique key wants.
		int MaxStack = 1;

		// Contribution to the carried weight total. 0 for weightless items.
		float Weight = 0.0f;

		// Free-form tags for gameplay queries: "consumable", "weapon",
		// "quest", "ammo". A vector rather than a bitmask so scripts can
		// invent tags without a C++ change.
		std::vector<std::string> Tags;

		ItemDefinitionComponent() = default;
		ItemDefinitionComponent(const ItemDefinitionComponent &other) = default;

		bool HasTag(const std::string &tag) const
		{
			for (const auto &t : Tags)
				if (t == tag)
					return true;
			return false;
		}
	};

	// The player's carried items.
	//
	// Capacity is enforced on Add, not on assignment: silently truncating on
	// assignment means a script that overwrites the inventory loses items
	// with no report, and one that appends gets different behaviour for the
	// same mistake.
	struct InventoryComponent
	{
		std::vector<ItemStack> Items;

		// 0 means unlimited capacity.
		float MaxWeight = 0.0f;

		// 0 means unlimited slots. Distinct from MaxWeight because a game may
		// want weightless but slot-limited carrying, or the reverse.
		int MaxSlots = 0;

		InventoryComponent() = default;
		InventoryComponent(const InventoryComponent &other) = default;

		// Total weight of everything carried. Needs per-item weight data,
		// which lives on the definition, so this is computed by the Scene
		// helper rather than here -- a component cannot resolve another
		// entity's data without a registry reference.
		float GetStackCount() const { return (float)Items.size(); }

		bool IsFull() const;
	};

	// A pile of items lying in the world, created by Scene::DropItemAt.
	//
	// This is the component that makes a drop a DROP rather than a
	// deletion. Scene::DropItemAt spawns an entity carrying one of these,
	// so the removed quantity exists somewhere observable instead of
	// ceasing to be anywhere at all.
	//
	// ItemId is the join key into ItemDefinitionComponent, exactly as an
	// InventoryComponent stack's ItemId is -- so a pickup reads the same
	// either way. Quantity is the number actually removed from the
	// carrier, which may be fewer than were asked for.
	//
	// Deliberately NOT an InventoryComponent: a pile on the floor is not
	// a container the player can add to, and reusing InventoryComponent
	// would make a dropped stack obey MaxWeight and MaxSlots -- limits
	// that make no sense for something already on the ground.
	struct DroppedItemComponent
	{
		std::string ItemId = "item";
		int Quantity = 1;

		DroppedItemComponent() = default;
		DroppedItemComponent(const DroppedItemComponent &other) = default;

		// A pile with an empty id or a non-positive quantity is not
		// droppable, so the serializer refuses to write or load one.
		bool IsValid() const { return !ItemId.empty() && Quantity > 0; }
	};
	// =====================================================================
	// Save game (14.3)
	//
	// Game state is deliberately NOT the same thing as scene data. A
	// .wastescene file is authored content; a save is mutable per-playthrough
	// state that must never overwrite it. These components mark what is worth
	// writing and hold the state that has no other home.
	// =====================================================================

	// Marks an entity as part of the save. Only flagged entities are written
	// to a save file, so a level with a thousand decorative props does not
	// produce a thousand saved props.
	//
	// An entity with this component saves its transform, health, weapon,
	// inventory, and script variables. The flag itself is authored in the
	// scene, so it is a filter, not state.
	struct PersistentComponent
	{
		// 0 means "this entity never goes away with distance" -- it is part
		// of the save but is not spawned/despawned by streaming. Positive
		// values are a streaming radius in world units, so an entity loaded
		// only when the player is near it still round-trips.
		float StreamingRadius = 0.0f;

		PersistentComponent() = default;
		PersistentComponent(const PersistentComponent &) = default;
	};

	// Quest / progression flags. Stored as a string map rather than a struct
	// because quest state is authored by scripts, and a fixed struct would
	// need a C++ change every time a designer adds a flag.
	//
	// Values are strings so a flag can hold a count, a stage name, or a
	// timestamp without a second component.
	struct QuestFlagsComponent
	{
		std::vector<std::pair<std::string, std::string>> Flags;

		QuestFlagsComponent() = default;
		QuestFlagsComponent(const QuestFlagsComponent &) = default;

		bool Has(const std::string &flag) const
		{
			for (const auto &kv : Flags)
				if (kv.first == flag)
					return true;
			return false;
		}

		const std::string *Get(const std::string &flag) const
		{
			for (const auto &kv : Flags)
				if (kv.first == flag)
					return &kv.second;
			return nullptr;
		}

		void Set(const std::string &flag, const std::string &value)
		{
			for (auto &kv : Flags)
			{
				if (kv.first == flag)
				{
					kv.second = value;
					return;
				}
			}
			Flags.emplace_back(flag, value);
		}

		void Clear(const std::string &flag)
		{
			for (auto it = Flags.begin(); it != Flags.end(); ++it)
			{
				if (it->first == flag)
				{
					Flags.erase(it);
					return;
				}
			}
		}
	};

	// Long-lived world timers -- day/night phase, cooldowns the player
	// cannot see, door open times. Distinct from the transient component
	// timers (muzzle flash, fire cooldown) which are deliberately NOT saved:
	// restoring a half-elapsed flash pops an effect the player never caused.
	//
	// Doubles, because a save may be resumed days of in-game time later.
	struct WorldTimerComponent
	{
		std::vector<std::pair<std::string, double>> Timers;

		WorldTimerComponent() = default;
		WorldTimerComponent(const WorldTimerComponent &) = default;

		double Get(const std::string &name) const
		{
			for (const auto &kv : Timers)
				if (kv.first == name)
					return kv.second;
			return 0.0;
		}

		void Set(const std::string &name, double value)
		{
			for (auto &kv : Timers)
			{
				if (kv.first == name)
				{
					kv.second = value;
					return;
				}
			}
			Timers.emplace_back(name, value);
		}
	};


	// =====================================================================
	// Audio (14.4)
	// =====================================================================

	// An emitter attached to an entity. Purely declarative: the component
	// says WHAT to play and how it should sound, and AudioSystem decides WHEN.
	//
	// Keeping the trigger out of the component is deliberate. If the component
	// decided when to play, every sound would need its own update code and a
	// script could not fire a one-off (a footstep on a jump landing) without
	// going through the component anyway.
	struct AudioSourceComponent
	{
		// Clip to play, by registered name. Empty means the component is inert,
		// which is a valid authoring state (a place to wire up sound later).
		std::string ClipName;

		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;

		// See Mixer::PlayRequest. Higher survives the voice cap.
		int Priority = 0;

		// A 2D source ignores the listener. Footsteps and UI belong here;
		// anything the player should be able to locate belongs in 3D.
		bool Spatial = false;

		// 0 means "follow this entity's transform". A non-zero value is an
		// offset in local space, which is how a gun muzzle or a foot is placed
		// slightly in front of and below the body without a child entity.
		glm::vec3 PositionOffset = {0.0f, 0.0f, 0.0f};

		float MinDistance = 1.0f;
		float MaxDistance = 30.0f;

		// Autoplay fires this sound when the runtime starts. Off by default:
		// most sounds are one-shots triggered by gameplay, and having every
		// source fire on start is an easy way to make a level sound wrong.
		bool PlayOnStart = false;

		// Seconds remaining of a triggered sound's minimum play time, so a
		// rapid retrigger cannot cut the previous one off mid-tail. Runtime
		// state, deliberately not serialised.
		float MinPlayTime = 0.0f;

		AudioSourceComponent() = default;
		AudioSourceComponent(const AudioSourceComponent &) = default;
	};

	// Marks the entity whose transform the audio listener follows. At most one
	// per scene; a second is ignored rather than fighting the first, because two
	// listeners means panning computed from two different heads.
	struct AudioListenerComponent
	{
		bool Enabled = true;

		AudioListenerComponent() = default;
		AudioListenerComponent(const AudioListenerComponent &) = default;
	};

}