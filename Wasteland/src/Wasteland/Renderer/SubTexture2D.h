// Include guard for SubTexture2D.
#pragma once

// glm for the vec2 UV types used throughout.
#include <glm/glm.hpp>

// Texture2D, the thing being sub-divided. Included relatively (same
// directory) rather than by project path -- the only header in this batch
// that does it that way.
#include "Texture.h"

namespace Wasteland {

	// A rectangular region of a Texture2D, expressed as four UV corner pairs.

	// The purpose is a sprite sheet: one atlas texture, many logical sub-images,
	// so a whole UI or tile set costs one bind instead of dozens.
	class SubTexture2D
	{
	public:
		// The general form: any axis-aligned UV rectangle within `texture`.

		// `min`/`max` are in normalised texture coordinates (0-1), not texels, so the
		// same numbers work at any atlas resolution.

		// Declared here, defined in the .cpp. Not const: the four corners are computed
		// once in the constructor body and cached in m_TexCoords.
		SubTexture2D(const Ref<Texture2D>& texture, const glm::vec2& min, const glm::vec2& max);

		// Returns by VALUE, not const reference, so each call bumps the shared_ptr
		// refcount and each temporary drops it. Accessors like this one are called
		// per-vertex or per-batch in a sprite renderer, so the churn is real; the
		// neighbouring GetTexCoords() correctly returns a pointer.
		const Ref<Texture2D> GetTexture() const { return m_Texture; }
		// Pointer to the first of four corner pairs: [bottomLeft, bottomRight,
		// topLeft, topRight] in the order the sprite renderer's vertex layout
		// expects.

		// Returns a raw const pointer rather than a reference or a span: four pairs
		// is a fixed count, so the length is implicit and the caller indexes 0..3.

		// The pointer aliases the member, so it dangles once the SubTexture2D is
		// destroyed, and is invalidated by nothing else -- the array is never resized.
		const glm::vec2* GetTexCoords() const { return m_TexCoords; }

		// The convenience form for sprite sheets: address a cell by row/column index
		// instead of by UV rectangle.

		//   coords      -- the (column, row) cell index within the atlas
		//   cellSize    -- one cell's size in UV units
		//   spriteSize  -- how many cells the sprite spans, for multi-cell sprites;
		//                  defaults to a single 1x1 cell

		// Derives the min/max UV pair from these and defers to the constructor above,
		// so there is one place where corner UVs are computed.

		// The default argument lives in the declaration only, as it must -- repeating
		// it in the definition would be an error.
		static Ref<SubTexture2D> CreateFromCoords(const Ref<Texture2D>& texture, const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize = { 1, 1 });
	private:
		// Shared ownership: the sub-texture must keep the atlas alive, since the UVs
		// are meaningless without it.

		// Held by Ref rather than a raw pointer or a weak reference, so a sub-texture
		// used on its own safely extends the atlas's lifetime.
		Ref<Texture2D> m_Texture;

		// Fixed-size C array of four vec2 corners, not a std::array.

		// Deliberately fixed rather than a vector: the count is a property of the
		// format (a quad has four corners), and GetTexCoords hands out a bare pointer
		// to it for indexing by a sprite quad builder.

		// Trivially copyable, so copying a SubTexture2D copies the corners by value
		// with no allocation.
		glm::vec2 m_TexCoords[4];
	};

}