// Precompiled header, then the one header this file implements. Nothing
        // else: glm, Ref and CreateRef all arrive via the SubTexture2D header,
        // so this translation unit has no direct dependency of its own.
#include "wlpch.h"
#include "SubTexture2D.h"

// Two methods, both tiny. This is a sprite-atlas helper: a sub-texture is a
// UV rectangle into a larger texture, expressed as four pre-computed
// corner coordinates rather than a min/max pair.
namespace Wasteland {

	// Takes ALREADY-NORMALISED UV coordinates (0..1), not pixels. The pixel to
	// UV division happens in the factory below, so every caller of this
	// ctor does not have to remember it -- and a caller who does pass
	// pixels gets out-of-range UVs rather than a compile error.
	SubTexture2D::SubTexture2D(const Ref<Texture2D>& texture, const glm::vec2& min, const glm::vec2& max)
		// A Ref, so the atlas texture is kept alive by every sub-texture that
		// references it. That is the whole reason to store a Ref rather than
		// a pointer: a sub-texture used by a quad outlives whatever created it.
		: m_Texture(texture)
	{
		// Four corners assigned EXPLICITLY in this order: 0 bottom-left, 1
		// bottom-right, 2 top-right, 3 top-left. That ordering must match the
		// quad vertex buffer used by the 2D renderer, or the sub-texture is
		// rotated or mirrored.
		// DEFECT (coordinate convention undocumented, documented not fixed):
		// this assumes GL-style bottom-left origin for both axes. DirectX and
		// Vulkan use a top-left origin, so the same UVs mean a vertically
		// flipped atlas on those backends. Nothing in this file records which
		// convention it assumes, and nothing flips the V coordinate, so a
		// DX/Vulkan port of the 2D renderer would show every sprite upside
		// down.
		m_TexCoords[0] = { min.x, min.y };
		// Bottom-right: x from max, y from min. The mix-and-match of one coordinate
		// from each corner is what makes this a rectangle rather than two
		// diagonal corners.
		m_TexCoords[1] = { max.x, min.y };
		// Top-right.
		m_TexCoords[2] = { max.x, max.y };
		// Top-left, completing the ring in the same winding as 0-1-2, so a quad
		// indexed as two triangles 0,1,2 and 0,2,3 comes out untwisted.
		m_TexCoords[3] = { min.x, max.y };
	}

	// The pixel-based factory, and the reason the ctor above takes UVs: this is
	// where an atlas cell index is turned into a UV rectangle.
	//
	// Returning a Ref rather than a value: the sub-texture is handed to
	// sprites and quads that may outlive the calling scope, and Ref
	// conveys that. The static factory exists so construction goes through
	// CreateRef rather than a raw new.
	Ref<SubTexture2D> SubTexture2D::CreateFromCoords(const Ref<Texture2D>& texture, const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize)
	{
		// The atlas cell origin in pixels, divided by the texture dimensions to
		// get UVs. Dividing by the REAL texture size rather than the padded
		// grid size is what makes a loose-packed atlas work.
		//
		// DEFECT (zero-division, documented not fixed): texture->GetWidth() is
		// used as an integer divisor with no check. A texture that failed to
		// load commonly reports width 0, and this is an integer division --
		// so it is undefined behaviour, and on x86 an integer divide by zero
		// raises a hardware fault that takes the process down. Nothing
		// between the texture arriving and this line checks that it loaded.
		glm::vec2 min = { (coords.x * cellSize.x) / texture->GetWidth(), (coords.y * cellSize.y) / texture->GetHeight() };
		// The far corner: cell origin plus the sprite size, then the same
		// normalisation.
		// DEFECT (integer truncation, documented not fixed): coords.x is
		// multiplied into cellSize BEFORE the division, and if those are
		// integer-typed this truncates. More importantly, no clamping to 1.0:
		// a cell index past the end of the atlas produces UVs greater than 1,
		// which sample wrapped or clamped texels depending entirely on the
		// texture sampler state set elsewhere.
		glm::vec2 max = { ((coords.x + spriteSize.x) * cellSize.x) / texture->GetWidth(), ((coords.y + spriteSize.y) * cellSize.y) / texture->GetHeight() };
		// createRef -> the ctor above, which computes the four corners. So there
		// is exactly one place the corner layout is written, and this factory
		// is the only pixel-space entry point into it.
		return CreateRef<SubTexture2D>(texture, min, max);
	}

}