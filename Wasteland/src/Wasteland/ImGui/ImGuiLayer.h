// Include guard for the ImGui integration layer.
#pragma once

// The Layer base this class derives from.
#include "Wasteland/Core/Layer.h"
// RendererAPI::API, used throughout the backend-switching interface below.

// Included for the enum alone -- the full Renderer/RendererAPI.h would drag
// in VertexArray.h and Texture.h.
#include "Wasteland/Renderer/RendererAPI.h"

// Event headers, so this class's OnEvent(Event&) can inspect the concrete
// event types it reacts to.

// Angle-bracket includes for engine headers -- the same inconsistency seen
// in Platform/Linux/LinuxWindow.cpp, which mixes both spellings.
#include <Wasteland/Events/ApplicationEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/KeyEvent.h>

// Forward declaration rather than an include of d3d11.h.

// This is what keeps the DX11 header out of every translation unit that
// includes ImGuiLayer.h -- including the Linux and Vulkan builds, where d3d11
// does not exist at all. The pointer member below only needs the incomplete
// type to be declared.
struct ID3D11ShaderResourceView;

namespace Wasteland {

	// The ImGui layer: pumps ImGui frames, routes events, and switches renderer
	// backends when the engine's API changes.

	// WL_API is the export/import decoration (dllexport when building the engine,
	// dllimport when consuming it). A client overriding any of these virtuals
	// needs the exported vtable, which is what the decoration provides.
	class WL_API ImGuiLayer : public Layer
	{
	public:
		// Constructor, defined in the .cpp. Starts with no backend: m_CurrentBackend
		// is initialised to API::None in the member declaration below, and the
		// backend is attached later by OnAttach or a deferred switch.
		ImGuiLayer();
		// Declared, not defaulted, because it has to shut the backend down.

		// Spelling `~ImGuiLayer()` with no `override` or `virtual`: it does override
		// Layer's virtual destructor, but the absence of the keyword means a base
		// signature change would not be caught here.
		~ImGuiLayer();

		// Called when the layer joins the stack. Creates the ImGui context and
		// initialises the platform backend.
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		// Draws the panels. Invoked between Begin() and End().
		virtual void OnImGuiRender() override;

		// Event sink. Its job is to mark events Handled when ImGui wants the input,
		// so a click consumed by a widget does not also reach gameplay code.
		void OnEvent(Event& event) override;

		// Opens an ImGui frame: performs any deferred backend switch, then calls
		// the matching ImGui_Impl<Backend>_NewFrame, ImGui_ImplGlfw_NewFrame,
		// ImGui::NewFrame and ImGuizmo::BeginFrame.

		// Sets m_FrameStarted so End() knows a frame is open -- see the field's
		// note below.
		void Begin();
		// Closes the frame begun above.

		// Checks m_FrameStarted and returns early if no frame is open, so a
		// mismatched Begin/End pair cannot assert inside ImGui.
		void End();

		// One-line setter, inline in the header.

		// When true (the default, see the field below) events are marked Handled if
		// ImGui reports it wants the mouse or keyboard, which is what keeps a click
		// inside a text field from also firing a gameplay action.
		void BlockEvents(bool block) { m_BlockEvents = block; }
		
		// Switch ImGui backend based on current renderer API
		// Requests a backend change.

		// DEFERRED, not immediate: this records the request (m_PendingBackendSwitch,
		// m_BackendSwitchPending) rather than switching. The switch is performed by
		// PerformBackendSwitch() from Begin() on the next frame. Doing it inline
		// would mean tearing down a GL context from inside whatever code noticed the
		// API change, which may itself be mid-draw.

		// Any state the old backend needs to carry over is handled there.
		void SwitchBackend(RendererAPI::API api);
		
	private:
		// Creates and initialises the ImGui backend for `api`. Private: backend
		// creation is an implementation detail of SwitchBackend.
		void InitBackend(RendererAPI::API api);
		// Destroys the current ImGui backend. Private, same reasoning.
		void ShutdownBackend();
		// The actual switch: shuts the old backend down and brings the new one up.
		// Called from Begin(), which is why the switch costs a frame.
		void PerformBackendSwitch();
		// Attempt to fall back to OpenGL. Returns false if no valid GL context
		// exists (e.g. when switching away from OpenGL — the context is gone).
		// Attempts to fall back to OpenGL, returning false when no valid GL context
		// exists -- notably when switching AWAY from OpenGL, since that destroys
		// the context the fallback would need.

		// Documented in the existing comment above as the reason the signature
		// reports failure; the caller is expected to handle it rather than assume
		// the switch succeeded.
		bool FallbackToOpenGL();
	public:
		// Tears the backend down without destroying the layer, for the window-
		// recreation path where the GL context is about to be lost.

		// Not private, unlike the three helpers above -- the window-recreation code
		// lives outside this class and calls it directly.
		void ShutdownBackendForWindowRecreate();
		
		// PUBLIC, and initialised to true: input is swallowed by ImGui by default.
		// Games that want gameplay to see clicks regardless call BlockEvents(false).

		// DEFECT (encapsulation): this is in the public section along with the five
		// fields below it, so any caller can read and write the layer's internal
		// state directly. A private section with accessors would keep the class
		// changeable; as it stands, renaming any of these is a breaking API change.
		bool m_BlockEvents = true;
		// Accumulated time, presumably advanced per frame. Public for the same
		// encapsulation reason as m_BlockEvents.
		float m_Time = 0.0f;
		// The backend actually in use. Starts as None, which HasBackend() below
		// treats as "no backend" -- so a freshly constructed layer reports false
		// until OnAttach or a switch runs.
		RendererAPI::API m_CurrentBackend = RendererAPI::API::None;
		// The backend that was requested but not yet applied. Meaningless unless
		// m_BackendSwitchPending is true.
		RendererAPI::API m_PendingBackendSwitch = RendererAPI::API::None;
		// The flag that makes the pending value meaningful.

		// Two fields rather than one optional because RendererAPI::API::None is a
		// legal enum value; using it as the sentinel would make "switch to none"
		// inexpressible and indistinguishable from "no switch requested".
		bool m_BackendSwitchPending = false;
		// Whether GLFW has been brought up for ImGui.
		bool m_GlfwInitialized = false;
		// Whether a frame is currently open.

		// Set true at the end of Begin() and false at the end of End(). This is the
		// guard that makes a stray End() without a Begin() harmless instead of an
		// assert inside ImGui.

		// Note Begin() sets it false again on the early-return path where no backend
		// exists, so the flag tracks "a frame is genuinely open" rather than merely
		// the Begin was called state.
		bool m_FrameStarted = false;

		// Neutral 1x1 white SRV (DX11): substituted for non-font ImTextureIDs
		// (panels submit GL texture IDs) so DrawIndexed can't fault on bogus pointers.
		// A 1x1 white shader-resource view, used to stand in for image handles that
		// are not SRVs.

		// The problem it solves: under DX11 an ImTextureID must be an
		// ID3D11ShaderResourceView*, but most panels submit OpenGL texture IDs (plain
		// integers). Drawing one of those as a pointer faults, so this neutral white
		// SRV is substituted and the draw succeeds showing white instead of
		// crashing. A consequence worth knowing: under DX11 a panel showing a
		// texture renders as a blank white rectangle rather than an error.

		// Initialised to nullptr, so it must be created during backend init; nothing
		// in this header enforces that, and a draw before it exists would submit the
		// null it already is.
		ID3D11ShaderResourceView *m_D3D11WhiteSRV = nullptr;

		// A second public section, re-opening access after the private block above.

		// Combined with the field block above this makes the class effectively
		// half-public: only the four method declarations between lines 35 and 40
		// are actually encapsulated.
		public:
		// Whether a backend is live. Inline: a single comparison.

		// The correct way for callers to ask "can ImGui draw this frame?" -- reading
		// m_CurrentBackend directly would also work but would bypass this
		// abstraction, and the None sentinel could then change without callers
		// being updated.
		bool HasBackend() const { return m_CurrentBackend != RendererAPI::API::None; }
	};

}