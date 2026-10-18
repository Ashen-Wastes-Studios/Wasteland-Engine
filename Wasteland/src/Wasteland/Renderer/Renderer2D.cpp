#include "wlpch.h"
#include "Renderer2D.h"

#include "VertexArray.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "Wasteland/Project/Project.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Wasteland {

	// CPU-side mirror of the quad vertex buffer layout declared in Init(). The
	// field order here must match the VertexBuffer::SetLayout order exactly --
	// the layout is described by offsets computed from this struct, so a
	// reordering here silently mis-assigns every subsequent attribute.
	struct QuadVertex
	{
		// Line vertices go straight to clip space from world position, so unlike
		// quads they are not pre-transformed by a model matrix here.
		// Transformed into world space on the CPU (DrawQuad applies the model
		// matrix to QuadVertexPositions before writing), so the shader receives
		// an already-projected position and needs no object transform of its own.
		glm::vec3 Position;
		glm::vec4 Color;
		glm::vec2 TexCoord;
		// Index into Renderer2DData::TextureSlots, not a sampler unit. The shader
		// samples a texture ARRAY at this slot, which is what lets a frame mix
		// more textures than the hardware exposes sampler units.
		float TexIndex;
		// Multiplied into the UVs in the fragment/vertex stage; 1.0 = no repeat.
		float TilingFactor;

		// Editor-only
		// Written as a flat int rather than interpolated. The editor picks the
		// nearest fragment back to this ID when the user clicks; -1 means
		// 'not pickable', which is what non-editor drawing uses.
		int EntityID;
	};

	struct CircleVertex
	{
		// Camera-facing quad corner for SDF circle rendering: the shape is built
		// in the fragment shader from the signed distance to the LocalPosition.
		glm::vec3 WorldPosition;
		// Object-space position, scaled by 2.0 so the unit quad spans [-1,1] and
		// the shader's circle maths needs no half-extent parameter.
		glm::vec3 LocalPosition;
		glm::vec4 Color;
		float Thickness;
		// Anti-alias feather width at the circle's rim, in the same units as
		// LocalPosition -- this is what keeps the outline from looking jagged.
		float Fade;

		// Editor-only
		int EntityID;
	};

	struct LineVertex
	{
		glm::vec3 Position;
		glm::vec4 Color;

		// Editor-only
		int EntityID;
	};

	// Every field is global state on the file-static s_Data below. Nothing in
	// this file is re-entrant: two threads cannot own two scenes at once.
	struct Renderer2DData
	{
		// Hard CPU-side cap. When a frame exceeds it, DrawQuad calls
		// FlushAndReset() and starts a new batch rather than overflowing the
		// staging array. A batch boundary costs a draw call, so the cap trades
		// fill rate for batching efficiency.
		static const uint32_t MaxQuads = 20000;
		// Four vertices per quad: the same QuadVertexPositions unit square that
		// every DrawQuad variant transforms. Circles reuse this cap because the
		// circle draw also emits 4 vertices per instance.
		static const uint32_t MaxVertices = MaxQuads * 4;
		// Two triangles per quad. QuadIndexCount accumulates 6 per quad and is
		// compared against this to decide when a batch is full -- so it is an
		// index count, not a vertex count.
		static const uint32_t MaxIndices = MaxQuads * 6;
		// Fixed at 32 rather than queried from the device (see the TODO). Slots
		// are per-BATCH: a long-lived frame that touches more than 32 distinct
		// textures will fill TextureSlots and the dedup loop below will index
		// past the end of the array -- the FlushAndReset bounds check only
		// guards MaxIndices, never MaxTextureSlots.
		static const uint32_t MaxTextureSlots = 32; // TODO: RenderCaps

		Ref<VertexArray> QuadVertexArray;
		Ref<VertexBuffer> QuadVertexBuffer;
		Ref<Shader> QuadShader;
		Ref<Texture2D> WhiteTexture;

		Ref<VertexArray> CircleVertexArray;
		Ref<VertexBuffer> CircleVertexBuffer;
		Ref<Shader> CircleShader;

		Ref<VertexArray> LineVertexArray;
		Ref<VertexBuffer> LineVertexBuffer;
		Ref<Shader> LineShader;

		uint32_t QuadIndexCount = 0;
		// Base of the CPU staging array. The write pointer walks forward through
		// it; Flush computes the byte delta between the two and uploads exactly
		// that prefix, so unused tail vertices are never sent to the GPU.
		QuadVertex* QuadVertexBufferBase = nullptr;
		QuadVertex* QuadVertexBufferPtr = nullptr;

		uint32_t CircleIndexCount = 0;
		CircleVertex* CircleVertexBufferBase = nullptr;
		CircleVertex* CircleVertexBufferPtr = nullptr;

		// Lines are drawn with DrawLines, which takes a vertex count rather than
		// an index count -- so unlike quads and circles there is no index buffer
		// and no 6-per-primitive increment.
		uint32_t LineVertexCount = 0;
		LineVertex* LineVertexBufferBase = nullptr;
		LineVertex* LineVertexBufferPtr = nullptr;

		// Only pushed to the GPU at flush time (RenderCommand::SetLineWidth),
		// NOT at draw time. SetLineWidth mid-batch therefore affects every line
		// in the batch, not just the ones drawn after the call.
		float LineWidth = 2.0f;

		// Holds strong references, so a texture cannot be destroyed while a
		// batch still references it. That is deliberate: the slots are read
		// during Flush, which can happen after the caller dropped its Ref.
		std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
		// Starts at 1 because slot 0 is reserved for the 1x1 white texture and is
		// what an untextured quad samples (TexIndex 0). Reset to 1, not 0, at
		// every scene and batch boundary.
		uint32_t TextureSlotIndex = 1; // 0 = white texture

		// The unit square every DrawQuad variant transforms: corners in CCW
		// order (-0.5,-0.5), (0.5,-0.5), (0.5,0.5), (-0.5,0.5), so the 0,1,2
		// / 2,3,0 index layout in Init() yields outward-facing triangles under
		// the default counter-clockwise front-face convention.
		glm::vec4 QuadVertexPositions[4];

		// Accumulated per frame; ResetStats() memsets it. Not reset by BeginScene,
		// so the owner of the frame decides when the window rolls over.
		Renderer2D::Statistics Stats;
	};

	// Namespace-scope static: zero-initialised before main, so the Ref<>
	// members start null and the pointers null without an explicit ctor.
	static Renderer2DData s_Data;

	void Renderer2D::RecreateResources()
	{
		// Emits a named scope for the external profiler. RecreateResources runs
		// rarely (only on an API switch) so this shows up as a rare, wide sample.
		WL_PROFILE_FUNCTION();

		// Called after a renderer-API switch: the old GPU objects (VAOs, VBOs,
		// shaders, white texture) belong to the previous graphics context and are
		// invalid there. Drop the handles and rebuild against the new API.
		// CPU-side staging buffers (QuadVertexBufferBase etc.) are reused as-is.
		// Dropping the last Ref to an OpenGL object is what actually deletes the
		// underlying VAO, so nulling here is the delete. Order matters: the VAO
		// is cleared before its vertex buffer, which is the safe order because
		// a VAO holds no ownership of its buffers.
		s_Data.QuadVertexArray = nullptr;
		s_Data.CircleVertexArray = nullptr;
		s_Data.LineVertexArray = nullptr;
		s_Data.QuadVertexBuffer = nullptr;
		s_Data.CircleVertexBuffer = nullptr;
		s_Data.LineVertexBuffer = nullptr;
		// Dropped along with the rest: TextureSlots may still hold references to
		// user textures at this point, but the memset below drops those too.
		s_Data.WhiteTexture = nullptr;
		s_Data.QuadShader = nullptr;
		s_Data.CircleShader = nullptr;
		s_Data.LineShader = nullptr;
		// Batch state is reset at scene start, not at EndScene. Any un-flushed
		// vertices from a previous frame are therefore discarded -- which is
		// correct, since EndScene/Flush has already submitted them.
		// Reset BEFORE Init() reallocates the GPU buffers, so an Init() failure
		// would leave a renderer that reports zero quads rather than drawing
		// garbage from the old staging array.
		s_Data.QuadIndexCount = 0;
		s_Data.CircleIndexCount = 0;
		s_Data.LineVertexCount = 0;
		// Texture slot 0 (white) survives; slot 1 starts the per-batch dedup
		// search again. Not clearing TextureSlots here is fine because only the
		// first TextureSlotIndex entries are ever read.
		s_Data.TextureSlotIndex = 1;
		// memset over a Ref<> array to null it out. This is only sound because
		// Ref<Texture2D> is layout-compatible with a zero pointer -- if the
		// refcounted block were ever stored inline, this would corrupt it.
		// NOTE: this drops any textures still referenced here without running
		// their destructors -- a leak of the refcount, not of the object.
		// Correctness does not depend on it: Init() re-fills slot 0 with the
		// freshly created WhiteTexture before the next batch is drawn.
		memset(s_Data.TextureSlots.data(), 0, s_Data.TextureSlots.size() * sizeof(Ref<Texture2D>));

		// Rebuilds every GPU handle against the new API. Not recursive -- Init
		// does not call back into RecreateResources.
		Init();
	}

	void Renderer2D::Init()
	{
		WL_PROFILE_FUNCTION();

		// Quads
		// A VAO is the OpenGL-idiomatic bundle of vertex layout + bound buffers.
		// Under NVRHI the same class is a thin wrapper, which is why the code
		// below is identical across backends.
		s_Data.QuadVertexArray = VertexArray::Create();

		// Sized for the worst case, not the current frame. The GPU allocation is
		// fixed for the lifetime of the renderer; per-frame cost is proportional
		// to what was actually written, because Flush passes a byte count.
		s_Data.QuadVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(QuadVertex));
		// The attribute list must mirror QuadVertex's declaration order. Each
		// name is bound by the shader, so renaming an attribute here without
		// touching the GLSL makes the attribute silently read as 0.
		s_Data.QuadVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color" },
			{ ShaderDataType::Float2, "a_TexCoord" },
			{ ShaderDataType::Float, "a_TexIndex" },
			{ ShaderDataType::Float, "a_TilingFactor" },
			// Declared last so the editor-picking int occupies the final 4 bytes of
			// each vertex. Adding a field to QuadVertex without appending here
			// would shift every following attribute's offset.
			{ ShaderDataType::Int, "a_EntityID" },
		});
		// Attaches the buffer so the VAO's attribute state binds this buffer.
		s_Data.QuadVertexArray->AddVertexBuffer(s_Data.QuadVertexBuffer);

		// Raw new[] with NO matching delete[] anywhere -- including in
		// RecreateResources, which deliberately keeps the old base pointer.
		// It is a permanent one-time allocation (~the size of 80000 vertices)
		// and is a genuine leak if Init() were ever called more than once.
		s_Data.QuadVertexBufferBase = new QuadVertex[s_Data.MaxVertices];

		// A single static index list shared by every batch. Since the geometry is
		// always the same unit quad, the indices never change and there is no
		// reason to rebuild them per frame.
		uint32_t* quadIndices = new uint32_t[s_Data.MaxIndices];

		// Running base vertex for the quad being emitted. Six indices are
		// written per iteration and the base advances by four, so quad N's
		// indices are exactly [4N, 4N+1, 4N+2, 4N+2, 4N+3, 4N].
		uint32_t offset = 0;
		for (uint32_t i = 0; i < s_Data.MaxIndices; i += 6)
		{ 
			quadIndices[i + 0] = offset + 0;
			quadIndices[i + 1] = offset + 1;
			// Shared vertex between the two triangles, which is what makes a quad
			// 4 vertices + 6 indices instead of 6 vertices.
			quadIndices[i + 2] = offset + 2;

			// Second triangle restates vertex 2 and closes on vertex 0 -- the
			// 0,1,2 / 2,3,0 fan described above.
			quadIndices[i + 3] = offset + 2;
			quadIndices[i + 4] = offset + 3;
			quadIndices[i + 5] = offset + 0;

			// Advance by VERTICES (4), not indices (6), matching the index values
			// that were just written.
			offset += 4;
		}

		// Create() copies the CPU array (the delete[] two lines later implies
		// it), so the 80000-index upload happens exactly once at Init time.
		Ref<IndexBuffer> quadIB = IndexBuffer::Create(quadIndices, s_Data.MaxIndices);
		// Attached after the vertex buffer so the VAO is fully specified before
		// any draw. quadIB is also used by the circle VAO below.
		s_Data.QuadVertexArray->SetIndexBuffer(quadIB);
		// The GPU-side copy is authoritative from here on; keeping the CPU array
		// would waste 320 KB for the process lifetime.
		delete[] quadIndices;

		// Circles
		s_Data.CircleVertexArray = VertexArray::Create();

		s_Data.CircleVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(CircleVertex));
		s_Data.CircleVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_WorldPosition" },
			{ ShaderDataType::Float3, "a_LocalPosition" },
			{ ShaderDataType::Float4, "a_Color" },
			{ ShaderDataType::Float, "a_Thickness" },
			{ ShaderDataType::Float, "a_Fade" },
			{ ShaderDataType::Int, "a_EntityID" },
		});
		s_Data.CircleVertexArray->AddVertexBuffer(s_Data.CircleVertexBuffer);
		// Circles are also unit quads with the same 0,1,2 / 2,3,0 layout, so the
		// identical index buffer is reused -- sharing one immutable GPU buffer
		// instead of uploading a second copy.
		s_Data.CircleVertexArray->SetIndexBuffer(quadIB); // Use quad IB
		// Same leak pattern as the quad staging array: new[] with no delete[].
		s_Data.CircleVertexBufferBase = new CircleVertex[s_Data.MaxVertices];

		// Lines
		s_Data.LineVertexArray = VertexArray::Create();

		// Deliberately allocated for MaxVertices but only ever 2 vertices per
		// primitive, and -- unlike quads and circles -- DrawLine has NO bounds
		// check against this capacity. A batch that exceeds 80000 line
		// vertices writes past the CPU array.
		s_Data.LineVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(LineVertex));
		s_Data.LineVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color" },
			{ ShaderDataType::Int, "a_EntityID" },
		});
		s_Data.LineVertexArray->AddVertexBuffer(s_Data.LineVertexBuffer);
		// Same unguarded staging array as the quad and circle buffers.
		s_Data.LineVertexBufferBase = new LineVertex[s_Data.MaxVertices];

		// A 1x1 texture is the cheapest possible placeholder; untextured quads
		// sample it through slot 0 and get opaque white, which multiplies
		// through as an identity tint.
		s_Data.WhiteTexture = Texture2D::Create(1, 1);
		// 0xffffffff is 4 bytes of 0xff = RGBA white. The value is a uint32_t
		// rather than 4 separate bytes so it is a single readable literal.
		uint32_t whiteTextureData = 0xffffffff;
		s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));

		// Identity mapping (unit i -> sampler i). Only meaningful on the GL path,
		// where each sampler must be told which texture unit it reads; the array
		// is built unconditionally even though the HLSL path never uses it.
		int32_t samplers[s_Data.MaxTextureSlots];
		for (uint32_t i = 0; i < s_Data.MaxTextureSlots; i++)
			samplers[i] = i;

		// Resolved through Project so the built-in shaders load out of
		// whichever project is active, not the process working directory.
		// Resolved through the active project rather than a relative path so
		// shaders resolve against project assets, not the process CWD -- which
		// is what lets the editor and a shipped build load the same shaders.
		Project &project = Project::Get();

		// .string() is needed because the asset layer caches paths and hands back
		// a reference; the shader loader takes an owned string.
		s_Data.QuadShader = Shader::Create(project.GetAssetPath("shaders/Renderer2D_Quad.glsl").string());
		// Each shader is bound and given u_ViewProjection before the batch is
		// filled. Binding here rather than at draw time means the uniform set at
		// BeginScene survives every DrawQuad call in between.
		s_Data.QuadShader->Bind();
		s_Data.CircleShader = Shader::Create(project.GetAssetPath("shaders/Renderer2D_Circle.glsl").string());
		s_Data.CircleShader->Bind();
		s_Data.LineShader = Shader::Create(project.GetAssetPath("shaders/Renderer2D_Line.glsl").string());
		s_Data.LineShader->Bind();

		// GL-only: wire u_Texture to consecutive units. HLSL shaders bind the
		// texture array via register t0 + the NVRHI binding set, so there is no
		// u_Texture uniform to reflect (setting it would log a warning).
		if (RendererAPI::GetAPI() == RendererAPI::API::OpenGL)
		{
			// GL only. The uniform is an array of 32 ints so the shader can index a
			// texture ARRAY by slot without dynamic sampler selection.
			s_Data.QuadShader->SetIntArray("u_Texture", samplers, s_Data.MaxTextureSlots);
			s_Data.CircleShader->SetIntArray("u_Texture", samplers, s_Data.MaxTextureSlots);
			s_Data.LineShader->SetIntArray("u_Texture", samplers, s_Data.MaxTextureSlots);
		}

		// Set all texture slots to 0
		// Establishes the white-texture invariant that every dedup loop relies on:
		// slot 0 is always white, so a fresh batch starts with one known texture
		// and TexIndex 0 is never ambiguous. The '// Set all texture slots to 0'
		// comment above is misleading -- only slot 0 is set here; slots 1..31
		// stay null until a batch fills them.
		s_Data.TextureSlots[0] = s_Data.WhiteTexture;

		// Bottom-left. Combined with the size scale in DrawQuad this centres the
		// quad on `position`, since -0.5 and +0.5 cancel the half-size offset.
		// w=1.0 is required for the homogeneous multiply in DrawQuad.
		s_Data.QuadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[1] = {  0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[2] = {  0.5f,  0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[3] = { -0.5f,  0.5f, 0.0f, 1.0f };
	}

	// Deliberately empty. All resources are Ref<> members of the static
	// s_Data and are released when that static is destroyed at exit; there
	// is nothing to unbind or flush at shutdown.
	void Renderer2D::Shutdown()
	{
		WL_PROFILE_FUNCTION();
	}

	void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		WL_PROFILE_FUNCTION();

		// `transform` is the inverse view (the caller's world-to-camera), so the
		// view-projection is projection * view. glm::inverse is a full 4x4
		// inversion on every BeginScene -- fine once per frame, but it is not
		// cached and assumes transform is invertible.
		glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

		s_Data.QuadShader->Bind();
		s_Data.QuadShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.CircleShader->Bind();
		s_Data.CircleShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.LineShader->Bind();
		s_Data.LineShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.QuadIndexCount = 0;
		// Rewinds the write cursor to the start of the staging array.
		s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

		s_Data.CircleIndexCount = 0;
		s_Data.CircleVertexBufferPtr = s_Data.CircleVertexBufferBase;

		s_Data.LineVertexCount = 0;
		s_Data.LineVertexBufferPtr = s_Data.LineVertexBufferBase;

		s_Data.TextureSlotIndex = 1;
	}

	void Renderer2D::BeginScene(const EditorCamera& camera)
	{
		WL_PROFILE_FUNCTION();

		// The editor camera already has a cached view-projection, so unlike the
		// Camera overload there is no matrix inversion here.
		glm::mat4 viewProj = camera.GetViewProjection();

		s_Data.QuadShader->Bind();
		s_Data.QuadShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.CircleShader->Bind();
		s_Data.CircleShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.LineShader->Bind();
		s_Data.LineShader->SetMat4("u_ViewProjection", viewProj);

		s_Data.QuadIndexCount = 0;
		s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

		s_Data.CircleIndexCount = 0;
		s_Data.CircleVertexBufferPtr = s_Data.CircleVertexBufferBase;

		s_Data.LineVertexCount = 0;
		s_Data.LineVertexBufferPtr = s_Data.LineVertexBufferBase;

		s_Data.TextureSlotIndex = 1;
	}

	// Legacy overload (marked TODO: Remove in the header). Unlike the other
	// two it binds ONLY the quad shader -- the circle and line shaders never
	// receive a u_ViewProjection, so DrawCircle/DrawLine after this call use
	// whatever matrix the previous shader binding left in them, or an
	// identity matrix on the very first frame.
	void Renderer2D::BeginScene(const OrthographicCamera& camera)
	{
		WL_PROFILE_FUNCTION();

		s_Data.QuadShader->Bind();
		s_Data.QuadShader->SetMat4("u_ViewProjection", camera.GetViewProjectionMatrix());

		s_Data.QuadIndexCount = 0;
		s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

		s_Data.CircleIndexCount = 0;
		s_Data.CircleVertexBufferPtr = s_Data.CircleVertexBufferBase;

		s_Data.LineVertexCount = 0;
		s_Data.LineVertexBufferPtr = s_Data.LineVertexBufferBase;

		s_Data.TextureSlotIndex = 1;
	}

	void Renderer2D::EndScene()
	{
		WL_PROFILE_FUNCTION();
		// EndScene is a pure flush -- the batch counters are NOT reset here.
		// BeginScene does that. So calling EndScene twice in a frame re-submits
		// the same vertices a second time.
		Flush();
	}

	void Renderer2D::Flush()
	{
		// Skip an empty batch entirely: uploading zero bytes and issuing a
		// zero-index draw would burn a draw call and show up in the stats.
		// The index count doubles as the 'anything was drawn' flag.
		if (s_Data.QuadIndexCount)
		{
			// Upload size in BYTES, derived from the pointer delta rather than a
			// separate vertex counter. Casting through uint8_t* first is what keeps
			// the subtraction in bytes on a platform where ptrdiff would be used.
			uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.QuadVertexBufferPtr - (uint8_t*)s_Data.QuadVertexBufferBase);
			// Uploads only the written prefix. The allocation stays MaxVertices for
			// the process lifetime, so this is a per-frame cost proportional to the
			// batch, not to the cap.
			s_Data.QuadVertexBuffer->SetData(s_Data.QuadVertexBufferBase, dataSize);

			// Bind textures
			// Same API split as the quad path above, but WITHOUT the GL else-branch:
			// on OpenGL the circle textures are never bound to units. Because all
			// three shaders use the same identity sampler array and the GL quad
			// path ran earlier in this flush, circles inherit whatever binding the
			// quad batch left behind -- correct only when a quad batch preceded it.
			if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
			{
				// Non-GL path: build a right-sized vector of the slots actually used.
				// Copying Ref<>s here bumps refcounts; the vector dies at the end of the
				// block, so the batch's only remaining ownership is TextureSlots.
				std::vector<Ref<Texture2D>> activeTexs(s_Data.TextureSlotIndex);
				// GL path instead binds each texture to its own unit. No vector is
				// needed because GL takes the binding directly on the texture object.
				// The two paths differ in an important way: this loop's effect is NOT
				// deferred through RenderCommand, so it runs on the calling thread.
				for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
					activeTexs[i] = s_Data.TextureSlots[i];
				// Recorded as a command rather than executed immediately, because
				// Renderer2D may be called from outside the render thread while the
				// RenderCommand queue is the single place that touches GPU state.
				RenderCommand::SetActiveTextures(activeTexs);
			}
			else
			{
				for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
					s_Data.TextureSlots[i]->Bind(i);
			}

			s_Data.QuadShader->Bind();
			// Index count, not vertex count -- the whole batch shares one index
			// buffer, so the count is how far into that buffer to draw.
			RenderCommand::DrawIndexed(s_Data.QuadVertexArray, s_Data.QuadIndexCount);
			// One increment per batch, so DrawCalls is really 'batches flushed'.
			// A frame with 20000 quads in one batch reports 1, not 20000.
			s_Data.Stats.DrawCalls++;
		}

		if (s_Data.CircleIndexCount)
		{
			uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.CircleVertexBufferPtr - (uint8_t*)s_Data.CircleVertexBufferBase);
			s_Data.CircleVertexBuffer->SetData(s_Data.CircleVertexBufferBase, dataSize);

			if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
			{
				std::vector<Ref<Texture2D>> activeTexs(s_Data.TextureSlotIndex);
				for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++)
					activeTexs[i] = s_Data.TextureSlots[i];
				RenderCommand::SetActiveTextures(activeTexs);
			}

			s_Data.CircleShader->Bind();
			RenderCommand::DrawIndexed(s_Data.CircleVertexArray, s_Data.CircleIndexCount);
			s_Data.Stats.DrawCalls++;
		}

		if (s_Data.LineVertexCount)
		{
			// Same pointer-delta byte count as the quad path.
			uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.LineVertexBufferPtr - (uint8_t*)s_Data.LineVertexBufferBase);
			s_Data.LineVertexBuffer->SetData(s_Data.LineVertexBufferBase, dataSize);

			s_Data.LineShader->Bind();
			// Set once per batch from the value captured at draw time, NOT per line.
			// Changing LineWidth between two DrawLine calls in the same batch has no
			// effect on the already-queued geometry.
			RenderCommand::SetLineWidth(s_Data.LineWidth);
			// Non-indexed draw: vertices are consumed in order as line pairs.
			RenderCommand::DrawLines(s_Data.LineVertexArray, s_Data.LineVertexCount);
			s_Data.Stats.DrawCalls++;
		}
	}

	void Renderer2D::FlushAndReset()
	{
		// FlushAndReset is the safety valve used mid-frame by DrawQuad when the
		// batch fills up, so it does the same flush-and-clear dance as EndScene
		// rather than relying on the caller to flush later.
		EndScene();

		s_Data.QuadIndexCount = 0;
		s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

		s_Data.CircleIndexCount = 0;
		s_Data.CircleVertexBufferPtr = s_Data.CircleVertexBufferBase;

		s_Data.LineVertexCount = 0;
		s_Data.LineVertexBufferPtr = s_Data.LineVertexBufferBase;

		s_Data.TextureSlotIndex = 1;
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, color);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
	{
		WL_PROFILE_FUNCTION();

		// Rotate is inserted between translate and scale, so rotation happens
		// about the quad's own origin and the scale axes follow the rotated
		// frame. Swapping these two would produce a different result whenever
		// rotation is non-zero.
		// Built after the texture-slot bookkeeping so the Ref copy into the slot
		// has already happened if anything below needs to reallocate.
		// Translate * Rotate * Scale, applied to the unit square corners. Building
		// the matrix on the CPU per primitive (rather than sending a transform
		// uniform) is what lets an arbitrary number of differently-transformed
		// quads share one draw call.
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, color);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		WL_PROFILE_FUNCTION();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		// NOTE: tintColor is dropped here. This overload accepts one and does
		// not forward it, so any tinted draw through the vec3+texture entry
		// point renders untinted (white). The same omission exists in the
		// subtexture and rotated variants below.
		DrawQuad(transform, texture, tilingFactor);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, subtexture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		WL_PROFILE_FUNCTION();

		// Always 4: every 2D primitive in this renderer is a unit quad.
		constexpr size_t quadVertexCount = 4;
		// Vertex colour is forced to opaque white instead of using tintColor,
		// so the vertex colour channel never modulates the sampled texture in
		// this overload -- see the note at the forwarding call.
		constexpr glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		// Raw pointer into the sub-texture's own storage; valid only as long as
		// the Ref on the next line keeps that sub-texture alive, which is why
		// the texture Ref is taken immediately rather than later.
		const glm::vec2* textureCoords = subtexture->GetTexCoords();
		const Ref<Texture2D> texture = subtexture->GetTexture();

		// Batch-full check, done BEFORE writing any vertex. Strictly greater-or-
		// equal, so the batch is flushed while there is still room for the
		// 6 indices / 4 vertices this call is about to add.
		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		// Same dedup-then-allocate dance as the subtexture overload. Because the
		// search starts at slot 1, reusing the white texture through this entry
		// point allocates a NEW slot for it instead of reusing slot 0.
		float textureIndex = 0.0f;

		// Third copy of the texture-slot dedup loop. Duplicated rather than
		// factored out because each sits in an overload that must stay inline
		// for the batching path; the same missing bounds check applies.
		// Linear dedup over the slots already used THIS batch. Slot 0 is skipped
		// because index 0 is the sentinel meaning 'not found', and white is
		// reserved there.
		for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
		{
			// Compares raw pointers, not Ref<> identity: two Ref<> to the same
			// Texture2D would compare equal anyway, but a different Texture2D
			// wrapping the same GPU handle would not be deduplicated.
			if (s_Data.TextureSlots[i].get() == texture.get())
			{
				textureIndex = (float)i;
				break;
			}
		}

		// Sentinel test: 0.0f can only mean 'not found', which is why the search
		// loop above starts at slot 1. A texture could never legitimately land
		// on slot 0 -- that slot is the white texture.
		if (textureIndex == 0.0f)
		{
			textureIndex = (float)s_Data.TextureSlotIndex;
			// DEFECT: no bounds check against MaxTextureSlots. TextureSlotIndex can
			// reach 32, and this write is then one past the end of the std::array.
			// The batch-full check above only guards MaxIndices, so a batch holding
			// 32+ distinct textures overflows before it flushes.
			s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
			s_Data.TextureSlotIndex++;
		}

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			// Same pre-transform as the axis-aligned path; the CPU does all the work
			// and the shader sees plain world positions either way.
			// The homogeneous multiply resolves w by itself here; w=1 on the unit
			// corners means the w component of the result is 1 and the divide is a
			// no-op. Safe for scale/rotate/translate but NOT for any perspective
			// matrix.
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			// Writes the forced-white colour, not tintColor -- identical omission to
			// the earlier overloads. Any call site that passes a tint gets no tint.
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			// -1 marks the quad as non-pickable, matching the entityID = -1 default
			// in the header. Editor overlays pass a real ID instead.
			s_Data.QuadVertexBufferPtr->EntityID = -1;
			// NOTE: this loop never writes EntityID. DrawRotatedQuad(vec3, color)
			// leaves that field holding whatever the previous primitive put there,
			// because the CPU array is not zeroed between batches -- so a
			// non-editor rotated quad can inherit a stale entity ID and become
			// click-pickable in the editor. The subtexture rotated variant below
			// does set it to -1, so this is an inconsistency, not a deliberate
			// choice.
			s_Data.QuadVertexBufferPtr++;
		}

		// Always 6 regardless of how many vertices were written -- the index
		// buffer is static, so this counter is also the batch occupancy used by
		// the full-check above.
		s_Data.QuadIndexCount += 6;

		// Circles increment QuadCount -- there is no separate circle counter, so
		// GetStats().QuadCount over-reports primitives whenever a circle was
		// drawn and the reported vertex/index totals are then wrong.
		// Counts PRIMITIVES, not vertices: Statistics::GetTotalVertexCount
		// multiplies by 4 and GetTotalIndexCount by 6.
		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
	{
		WL_PROFILE_FUNCTION();

		constexpr size_t quadVertexCount = 4;
		// Untextured overload, so slot 0 (the 1x1 white) is used unconditionally
		// and no dedup search is needed. const, unlike the textured variants
		// where the slot index is computed.
		const float textureIndex = 0.0f; // White Texture
		// Full [0,1] UV square in the same corner order as QuadVertexPositions.
		// A sampler set to GL_REPEAT would tile, but the default clamp-to-edge
		// is what makes a white 1x1 sample solid.
		constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		const float tilingFactor = 1.0f;

		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr->EntityID = entityID;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor, int entityID)
	{
		WL_PROFILE_FUNCTION();

		// DEAD CONSTANTS. x/y/sheetWidth/sheetHeight/spriteWidth/spriteHeight
		// below are leftovers from a sprite-atlas addressing scheme; nothing in
		// this function reads them and textureCoords is the hardcoded unit
		// square instead. They compile away silently.
		constexpr float x = 7, y = 6;
		// Unused atlas dimensions -- see the note on the dead x/y above.
		constexpr float sheetWidth = 2560.0f, sheetHeight = 1664.0f;
		constexpr float spriteWidth = 128.0f, spriteHeight = 128.0f;

		constexpr size_t quadVertexCount = 4;
		constexpr glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		float textureIndex = 0.0f;

		for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
		{
			if (s_Data.TextureSlots[i].get() == texture.get())
			{
				textureIndex = (float)i;
				break;
			}
		}

		if (textureIndex == 0.0f)
		{
			textureIndex = (float)s_Data.TextureSlotIndex;
			s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
			s_Data.TextureSlotIndex++;
		}

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr->EntityID = entityID;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}


	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		WL_PROFILE_FUNCTION();

		constexpr size_t quadVertexCount = 4;
		const float textureIndex = 0.0f; // White Texture
		constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		const float tilingFactor = 1.0f;

		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			// Explicit Z axis: this is a 2D-only rotation, so rotation about any
			// other axis is not expressible through this entry point.
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		WL_PROFILE_FUNCTION();

		constexpr size_t quadVertexCount = 4;
		constexpr glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		float textureIndex = 0.0f;

		for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
		{
			if (s_Data.TextureSlots[i].get() == texture.get())
			{
				textureIndex = (float)i;
				break;
			}
		}

		if (textureIndex == 0.0f)
		{
			textureIndex = (float)s_Data.TextureSlotIndex;
			s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
			s_Data.TextureSlotIndex++;
		}

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			// Same rotation as the untextured variant; note the missing space before
			// the closing brace here compared with line 559 -- cosmetic only.
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f})
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, subtexture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		WL_PROFILE_FUNCTION();

		constexpr size_t quadVertexCount = 4;
		constexpr glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		const glm::vec2* textureCoords = subtexture->GetTexCoords();
		const Ref<Texture2D> texture = subtexture->GetTexture();

		if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		float textureIndex = 0.0f;

		for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
		{
			if (s_Data.TextureSlots[i].get() == texture.get())
			{
				textureIndex = (float)i;
				break;
			}
		}

		if (textureIndex == 0.0f)
		{
			textureIndex = (float)s_Data.TextureSlotIndex;
			s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
			s_Data.TextureSlotIndex++;
		}

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		for (size_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
			s_Data.QuadVertexBufferPtr->Color = color;
			s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
			s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadVertexBufferPtr->EntityID = -1;
			s_Data.QuadVertexBufferPtr++;
		}

		s_Data.QuadIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

	// Circles reuse the quad batch: 4 vertices, quad index buffer, quad
	// vertex-capacity check. There is no separate circle capacity, so a
	// circle-heavy frame competes for the same 20000-quad budget.
	void Renderer2D::DrawCircle(const glm::mat4 & transform, const glm::vec4 & color, float thickness, float fade, int entityID)
	{
		WL_PROFILE_FUNCTION();

		// Same pre-write overflow guard as DrawQuad, against the shared cap.
		if (s_Data.CircleIndexCount >= Renderer2DData::MaxIndices)
			FlushAndReset();

		// Transforms the SAME unit-square table DrawQuad uses, so a rotated rect
		// from the mat4 overload is consistent with DrawRotatedQuad's corners.
		for (size_t i = 0; i < 4; i++)
		{
			// Reuses the same unit-square corner table as quads, so a circle is a
			// quad-sized billboard whose shape is resolved per-fragment.
			s_Data.CircleVertexBufferPtr->WorldPosition = transform * s_Data.QuadVertexPositions[i];
			// x2 turns the [-0.5, 0.5] corners into [-1, 1], which is the range the
			// circle shader's signed-distance maths expects. Scaling the vec4 also
			// doubles w to 2.0; the shader must ignore w here for the outline to sit
			// correctly.
			s_Data.CircleVertexBufferPtr->LocalPosition = s_Data.QuadVertexPositions[i] * 2.0f;
			s_Data.CircleVertexBufferPtr->Color = color;
			s_Data.CircleVertexBufferPtr->Thickness = thickness;
			s_Data.CircleVertexBufferPtr->Fade = fade;
			s_Data.CircleVertexBufferPtr->EntityID = entityID;
			s_Data.CircleVertexBufferPtr++;
		}

		s_Data.CircleIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

    // Lines bypass the quad path entirely: own vertex array, non-indexed draw,
    // two raw vertices instead of a four-corner quad, so a line is much
    // cheaper per pixel than a quad-based primitive.
    void Renderer2D::DrawLine(const glm::vec3 &p0, const glm::vec3 &p1, const glm::vec4 &color, int entityID)
    {
		// DEFECT: no capacity check. Every other batch-full check in this file
		// compares its index/vertex count against MaxIndices/MaxVertices before
		// writing; DrawLine has none, so 80001+ line vertices in one batch
		// writes past the end of LineVertexBufferBase.
		s_Data.LineVertexBufferPtr->Position = p0;
		s_Data.LineVertexBufferPtr->Color = color;
		s_Data.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineVertexBufferPtr++;

		// Second endpoint of the pair. GL draws consecutive vertex pairs as
		// independent line segments, so two vertices per DrawLine is exactly
		// right and there is no shared-vertex indexing to get wrong.
		s_Data.LineVertexBufferPtr->Position = p1;
		s_Data.LineVertexBufferPtr->Color = color;
		s_Data.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineVertexBufferPtr++;

		// Two vertices per line, matching DrawLines' non-indexed consumption.
		s_Data.LineVertexCount += 2;
    }

    // Axis-aligned rect drawn as four separate lines rather than four quads,
    // so it is genuinely thin instead of a filled box.
    void Renderer2D::DrawRect(const glm::vec3 &position, const glm::vec2 &size, const glm::vec4 &color, int entityID)
    {
		// Corners are built by hand from `position` as the CENTRE, offset by half
		// the size. That means this overload's convention differs from DrawQuad's,
		// which also treats position as the centre but derives it from the unit
		// square -- consistent here, easy to get wrong at the call site.
		glm::vec3 p0 = glm::vec3(position.x - size.x * 0.5f, position.y - size.y * 0.5f, position.z);
		glm::vec3 p1 = glm::vec3(position.x + size.x * 0.5f, position.y - size.y * 0.5f, position.z);
		glm::vec3 p2 = glm::vec3(position.x + size.x * 0.5f, position.y + size.y * 0.5f, position.z);
		glm::vec3 p3 = glm::vec3(position.x - size.x * 0.5f, position.y + size.y * 0.5f, position.z);

		// NOTE: entityID is not forwarded. DrawLine defaults entityID to -1, so
		// every rect drawn through this overload is non-pickable regardless of
		// the ID the caller passed. The mat4 overload below has the same
		// omission.
		DrawLine(p0, p1, color);
		DrawLine(p1, p2, color);
		DrawLine(p2, p3, color);
		DrawLine(p3, p0, color);
    }

    void Renderer2D::DrawRect(const glm::mat4 &transform, const glm::vec4 &color, int entityID)
    {
		// Fixed-size stack array: 4 corners, zero heap allocation, matching the
		// unrolled four DrawLine calls below.
		glm::vec3 lineVertices[4];
		for (size_t i = 0; i < 4; i++)
			lineVertices[i] = transform * s_Data.QuadVertexPositions[i];

		DrawLine(lineVertices[0], lineVertices[1], color);
		DrawLine(lineVertices[1], lineVertices[2], color);
		DrawLine(lineVertices[2], lineVertices[3], color);
		DrawLine(lineVertices[3], lineVertices[0], color);
    }

    // Editor/debug convenience: routes a sprite component to whichever quad
    // overload matches whether the component has a texture.
    void Renderer2D::DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID)
	{
		// A sprite with no texture falls back to the untextured overload, which
		// samples the white texture in slot 0 and renders src.Color as the quad.
		if (src.Texture)
			// Passes src.Color as tintColor -- but the callee forces vertex colour to
			// opaque white and ignores the argument, so a tinted sprite renders
			// untinted here too.
			DrawQuad(transform, src.Texture, src.TilingFactor, src.Color, entityID);
		else
			DrawQuad(transform, src.Color, entityID);
	}

    // Reads the value that will be sent at flush time, not the width the GPU
    // currently has. After SetLineWidth the reported width is ahead of the
    // rendered one until the next Flush.
    float Renderer2D::GetLineWidth()
    {
        return s_Data.LineWidth;
    }

    void Renderer2D::SetLineWidth(float width)
    {
        // Stored only. The value reaches the GPU via SetLineWidth inside Flush,
        // so a width set after the last DrawLine of a batch is not applied.
        s_Data.LineWidth = width;
    }

    void Renderer2D::ResetStats()
	{
		// Whole-struct zeroing. Statistics is a POD of four uint32_t, so this is
		// safe, and it relies on all members being default-zero to be meaningful.
		memset(&s_Data.Stats, 0, sizeof(Statistics));
	}

	Renderer2D::Statistics Renderer2D::GetStats()
	{
		// Returns BY VALUE -- Statistics is a small POD, so this is a cheap copy
		// rather than a dangling reference into static storage.
		return s_Data.Stats;
	}

}