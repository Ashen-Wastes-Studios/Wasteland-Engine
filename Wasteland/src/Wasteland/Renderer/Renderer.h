// The engine's thin 2D renderer facade.
#pragma once

// RenderCommand is the queue everything ultimately goes through; this
// header's Init/Submit exist to feed it.
#include "RenderCommand.h"

// The camera type BeginScene takes. Included rather than forward-declared
// because the parameter is a non-const REFERENCE (see line 16), which a
// forward declaration would also allow -- but BeginScene is small enough
// that inlining the type is simpler than explaining why it is not.
#include "OrthographicCamera.h"
// Shader, for the Submit overload. The shader handle is a Ref, so a draw
// queued through this facade keeps the program alive until it executes.
#include "Shader.h"

// Engine root namespace.
namespace Wasteland {

		// A static-only class: no instances, no state of its own beyond the single
		// static SceneData pointer below. Every method is static, so the type
		// exists only as a namespace.
	class Renderer
	{
	public:
				// Startup. Initialises the command queue and allocates m_SceneData.
		static void Init();
				// Called on resize. DEFECT (documented, not fixed): the header takes width
				// and height but this class has no viewport size member and no camera, so
				// there is nothing here that could consume them -- the projection matrix
				// lives on the OrthographicCamera the caller passes to BeginScene. The
				// parameters exist to satisfy a platform-window callback signature.
		static void OnWindowResize(uint32_t width, uint32_t height);

				// A NON-CONST reference to the camera. DEFECT (documented, not fixed):
				// nothing in this class mutates the camera, and BeginScene is a query of
				// the camera's view-projection -- so the parameter should be const. As
				// written, a caller must hand over a mutable camera to be allowed to
				// render from a read-only one.
		static void BeginScene(OrthographicCamera& camera);
		static void EndScene();

		// Allows external renderers to set the global view-projection matrix
				// Overrides the global view-projection. This is the escape hatch for
				// callers that compute their own matrix rather than using a camera.
		static void SetViewProjection(const glm::mat4& viewProj);
				// Queues one draw: shader, geometry, and an optional model transform.
		//
				// The transform DEFAULT is the identity matrix rather than a null or a
				// bool, so the common case is "no transform" written as an omitted
				// argument. glm::mat4(1.0f) is a real matrix built at every call site that
				// omits it, though the compiler will fold it.
		static void Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform = glm::mat4(1.0f));

				// Passes the question straight through to RendererAPI. It exists so
				// callers do not have to include RendererAPI.h to ask which backend is
				// live.
		static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
	private:
				// The private state struct. A struct rather than bare members because the
				// whole thing is reached through one pointer, and this keeps the shape
				// swappable without touching the class layout.
		struct SceneData
		{
						// The matrix every Submit is interpreted through. DEFECT (documented, not
						// fixed): glm::mat4 with no initialiser, so it is uninitialised until
						// BeginScene runs. A Submit before the first BeginScene would therefore
						// multiply by garbage rather than by a known matrix. BeginScene is
						// expected to run first; nothing enforces it.
			glm::mat4 ViewProjectionMatrix;
		};

				// A raw owning pointer, allocated in Init and freed in Shutdown.
				// DEFECT (documented, not fixed): static pointer-to-struct with manual
				// new/delete where the rest of the engine uses Ref or a value. There is
				// also no Shutdown declaration in this header even though Init exists, so
				// the allocation has no visible owner in the interface.
		static SceneData* m_SceneData;
	};

}