// Precompiled-header include, always first: it supplies the STL, the
// project macros and the compiler flags this translation unit relies on.
// Being first also matters because the precompiled-header mechanism
// requires it to precede every other #include.
#include "wlpch.h"
#include "Renderer.h"

// Angle brackets: found via the include path, not this directory.
// Concrete GL backend header, needed for Submit's dynamic_pointer_cast
// and for UploadUniformMat4, which the Shader base does not declare.
#include <Platform/OpenGL/OpenGLShader.h>
#include "Renderer2D.h"
#include "Renderer3D.h"

namespace Wasteland {

	// Out-of-class definition of the static member declared in Renderer.h.
	// SceneData is a private nested struct, so it is spelled with the
	// enclosing class qualifier here.
	// DEFECT: heap-allocated at static-init time and never freed -- the
	// process exits without delete, so this leaks by construction. A
	// function-local static object would give the same access with a
	// destructor. Not fixed here.
	// Also note ViewProjectionMatrix has no in-class initialiser, so it is
	// default-constructed (glm identity) rather than garbage.
	Renderer::SceneData* Renderer::m_SceneData = new Renderer::SceneData;

	// Static: called once from application startup before any Submit.
	// There is no matching Shutdown, so the allocation above and anything
	// the two Init calls set up are released only by process teardown.
	void Renderer::Init()
	{
		// Macro that opens a scoped profiling region for this function; the
		// companion at scope exit closes it. Must be the first statement so
		// the measured span covers the whole body.
		WL_PROFILE_FUNCTION();

		// Brings up the graphics API through RenderCommand::s_RendererAPI.
		// Note RenderCommand.h dereferences that pointer without a null check,
		// so this call must precede every other RenderCommand entry point.
		RenderCommand::Init();
		// Sets up the 2D pipeline state (blend mode, projection, etc.) that
		// Renderer2D draws expect. Separate from 3D because the two need
		// incompatible depth/blend configurations.
		Renderer2D::Init();
		// Same idea for the 3D path. Initialising both up front means a scene
		// can mix 2D and 3D geometry, at the cost of touching state neither
		// may end up using.
		Renderer3D::Init();
	}

	// uint32_t matches the framebuffer pixel counts GLFW reports; there is
	// no clamping, so a zero-width window would push a degenerate viewport
	// to the API.
	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		// Origin at 0,0 and full size: the viewport is simply reset to the
		// whole window on resize. No Y-flip here, so the origin convention is
		// whatever the GL backend configures elsewhere.
		RenderCommand::SetViewport(0, 0, width, height);
	}

	// Non-const reference even though the body only reads the camera.
	// That is a latent trap: BeginScene could be given a temporary here and
	// it would compile. It does mirror the engine tutorial API it grew
	// out of, where BeginScene later took a non-const camera too.
	void Renderer::BeginScene(OrthographicCamera& camera)
	{
		// Caches the combined camera matrix for the whole scene. Every Submit
		// re-uploads this value, so it is per-scene state rather than per-draw.
		// GetViewProjectionMatrix returns by value (see OrthographicCamera.h),
		// so this is one 4x4 copy per BeginScene.
		m_SceneData->ViewProjectionMatrix = camera.GetViewProjectionMatrix();
	}

	// Deliberately empty: there is no state to restore yet because BeginScene
	// only caches a matrix and does not bind GL state. The paired call is
	// kept so the API has room to grow without changing every call site.
	void Renderer::EndScene()
	{
	}

	// Escape hatch for callers that have a matrix rather than a camera --
	// noted in Renderer.h as being for external renderers. It bypasses the
	// camera entirely, so a later BeginScene would silently overwrite it.
	void Renderer::SetViewProjection(const glm::mat4& viewProj)
	{
		m_SceneData->ViewProjectionMatrix = viewProj;
	}

	// The single draw entry point: shader + geometry + per-object transform.
	// const Ref& throughout keeps the resources alive for the call without
	// bumping the refcount.
	// The transform parameter defaults to identity in Renderer.h, so a
	// world-space draw can omit it.
	void Renderer::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
	{
		// Makes this program current. The GL name-based upload calls that
		// follow depend on it -- they resolve locations against the bound
		// program.
		// DEFECT: no Unbind anywhere in this function, so the last shader used
		// stays bound until something else binds one. Harmless while every
		// Submit rebinds, but it is implicit state leaking across calls.
		shader->Bind();
		// DEFECT: the cast result is dereferenced without a null check. If a
		// non-OpenGL Shader is ever submitted (the RendererAPI enum implies
		// more than one backend exists), dynamic_pointer_cast yields null and
		// this is a null-pointer call. A checked cast would be correct here.
		// The return value of UploadUniformMat4 is not checked either.
		// Also bypasses the virtual Shader::SetMat4 (line 28 of OpenGLShader.h)
		// that exists for exactly this purpose -- see below.
		std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_ViewProjection", m_SceneData->ViewProjectionMatrix);
		// Per-object matrix. Same unchecked dynamic_pointer_cast as line 43.
		// Both names are string literals resolved by OpenGLShader through its
		// m_UniformLocationCache, so a typo here would be a silent no-op
		// rather than a compile error.
		std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_Transform", transform);

		// VAO must be bound before the draw call; on a core-profile context the
		// draw is undefined without it.
		vertexArray->Bind();
		// IndexCount is defaulted to 0 in RenderCommand.h, which the backend
		// reads as "use the whole index buffer" -- so the geometry's own index
		// count is the only thing determining draw size here.
		RenderCommand::DrawIndexed(vertexArray);
	}

}