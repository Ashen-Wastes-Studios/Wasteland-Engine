#pragma once

#include "Wasteland/Renderer/OrthographicCamera.h"

#include "Wasteland/Renderer/Camera.h"

#include "Wasteland/Renderer/EditorCamera.h"

#include "Texture.h"
#include "SubTexture2D.h"
#include <Wasteland/Scene/Components.h>

namespace Wasteland {

	// All static: the renderer is a global singleton backed by the
	// file-static s_Data in the .cpp. Nothing here can be instantiated,
	// which is why there is no constructor and no instance state.
	class Renderer2D
	{
	// A size-only constructor by VALUE. WindowProps and similar props
	// structs are small and this style is used consistently across the
	// platform window headers.
	public:
		// The whole lifecycle. Init() creates every GPU object; the .cpp has
		// an empty Shutdown() because the Ref members release on static
		// destruction.
		static void Init();
		static void Shutdown();
		// Rebuilds all GPU objects after a renderer-API switch (old-context
		// handles are invalid there). CPU staging buffers are reused.
		// The API-switch hook, and the reason this header documents its own
		// semantics: the .cpp clears the handles and rebuilds them, because
		// a texture/VAO from the old graphics context is invalid in the new
		// one. CPU staging buffers are deliberately reused.
		static void RecreateResources();

		// Takes the world transform separately from the camera, so the
		// caller passes inverse(view) as `transform`. The .cpp inverts it
		// again to build the view-projection -- a confusing contract that
		// costs a 4x4 inversion per frame.
		static void BeginScene(const Camera& camera, const glm::mat4& transform);
		// EditorCamera overload: it already holds a cached view-projection,
		// so no inversion. The distinction is invisible at the call site
		// except in the cost.
		static void BeginScene(const EditorCamera& camera);
		// Legacy, marked for removal. DEFECT: unlike the other two overloads
		// it binds ONLY the quad shader, so a circle or line drawn after it
		// uses whatever u_ViewProjection the previous binding left set.
		static void BeginScene(const OrthographicCamera& camera); // TODO: Remove
		static void EndScene();
		// PUBLIC, not private: callers flush mid-frame when a batch fills, and
		// FlushAndReset below is the private half of that. Exposing Flush
		// means a caller can submit a batch without resetting, which is what
		// makes double-submission possible.
		static void Flush();

		// Primatives
		// The overload set is the API's main cost. vec2/vec3 pairs differ
		// only in the z component and exist because callers pass literal 2D
		// positions everywhere; each pair forwards to the vec3 form.
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
		// tintColor DEFAULTS to glm::vec4(1.0f) -- note the single-argument
		// form, which zero-fills g/b/a. That is harmless only because the
		// implementation IGNORES tintColor and forces opaque white (see
		// the .cpp), so the default is never actually read. If the tint were
		// ever honoured, this default would pass (1,0,0,0) -- opaque red.
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		// SubTexture2D overloads: the sub-texture supplies both the UV rect
		// and the parent texture, so the caller does not have to decompose
		// it. Same ignored tintColor as above.
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));

		// The mat4+color overload is the workhorse the other overloads all
		// forward to, and the first with an entityID -- defaulting to -1,
		// the non-pickable sentinel the .cpp writes into the vertex buffer.
		static void DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
		// The textured mat4 overload. DEFECT: this is where tintColor is
		// accepted and then dropped -- the .cpp sets vertex colour to a
		// hardcoded white constexpr instead. Every tinted quad through this
		// path renders untinted.
		static void DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);

		// Rotation is in radians
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color);
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color);
		// Rotation in RADIANS, and a DIFFERENT overload set from DrawQuad:
		// there is no mat4+color variant and no entityID parameter, so the
		// .cpp's rotated paths leave EntityID uninitialised in the vertex
		// array (inherited from whatever the previous primitive wrote).
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor = glm::vec4(1.0f));

		// radius/fade defaults: thickness 1.0 and a 0.005 feather. A 0 fade
		// would make the rim aliased; the default is small because the
		// shader treats it as a fraction of the unit quad.
		static void DrawCircle(const glm::mat4& transform, const glm::vec4& color, float thickness = 1.0f, float fade = 0.005f, int entityID = -1);

		// Takes the SOURCE COMPONENT by pointer, not a reference. The .cpp
		// relies on that to distinguish 'unset' from 'set to zero', and it
		// also means a caller cannot bind to a temporary.
		static void DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID = -1);

		// Also takes entityID and drops it, like the vec3 overload above: the
		// .cpp builds four corners and forwards without it.
		static void DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID = -1);
		// DrawRect exists in a mat4 form AND a position/size form, and
		// BOTH ignore their entityID -- the .cpp calls DrawLine three
		// arguments deep, so the default -1 applies and an editor overlay
		// rectangle is never pickable regardless of the ID passed.
		static void DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);

		static void	DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID);

		// GetLineWidth reports the value that WILL be sent at flush time, not
		// the width the GPU currently has. After SetLineWidth the reported
		// number runs ahead of what is drawn until the next Flush.
		static float GetLineWidth();
		static void SetLineWidth(float width);

		// Stats
		// Statistics is a POD of counters, defined inline because it is pure
		// data with no platform dependency. The inline getters let the .cpp
		// and the editor compute totals without linking anything.
		struct Statistics
		{
			uint32_t DrawCalls = 0;
			// QuadCount counts PRIMITIVES, and the .cpp increments it for circles
			// too. So a frame with circles reports an over-count and both derived
			// totals are then wrong by 4x/6x per circle.
			uint32_t QuadCount = 0;

			// QuadCount * 4 assumes every counted primitive is a quad. Consistent
			// with the intent, wrong with the circle increment noted above.
			uint32_t GetTotalVertexCount() { return QuadCount * 4; }
			uint32_t GetTotalIndexCount() { return QuadCount * 6; }
		};
		static void ResetStats();
		static Statistics GetStats();
	private:
		// PRIVATE but note it is called from the public Draw* methods when a
		// batch fills -- so the class can end a batch mid-frame. This is why
		// those methods check the index count BEFORE writing any vertex.
		static void FlushAndReset();
	};

}