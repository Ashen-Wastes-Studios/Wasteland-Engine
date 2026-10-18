// Editor layer: the top-level application layer that owns the dockspace,
// the editor viewport, and every editor panel.
#pragma once

// Engine-side umbrella header. Everything below is included
// transitively from here, which is why no standard-library header
// is included directly in this file.
#include "Wasteland.h"
// Panel headers are included explicitly rather than through a single
// "all panels" barrel: each member below names its panel type in its
// declaration, so the include list documents what the layer builds.
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptInspectorPanel.h"
#include "Panels/NPCBehaviorPanel.h"
#include "Panels/VisualBlocksPanel.h"
#include "Panels/AvbGraphPanel.h"
#include "Panels/TerrainAssetPanel.h"
#include "Panels/AIChatPanel.h"
#include "Panels/ModelDownloaderPanel.h"

// Renderer-owned types used by value or by Ref<> below. These are the
// engine's public renderer interface, so the editor compiles against
// whichever backend (OpenGL / NVRHI) the engine selects at runtime.
#include <Wasteland/Renderer/Texture.h>
#include <Wasteland/Project/Project.h>

#include "Wasteland/Renderer/EditorCamera.h"

// Everything is inside the Wasteland namespace, matching the engine's
// convention: the editor is a client of the engine, not a separate root.
namespace Wasteland
{

	// A named arrangement of every editor panel.
	//
	// Hardcoded on purpose. The panel set is fixed and small, so a curated
	// layout per workflow gives better results than a saved ImGui ini the user
	// has to tune by hand -- and it is reproducible, which a per-machine saved
	// layout is not.
	enum class Workflow
	{
		Scene,     // Level design: viewport dominant, asset browser under it.
		Scripting, // Hand-written Python and its inspector.
		Visual,    // Visual Blocks graphs and behaviour trees.
		Review,    // AI chat and inspection; no big viewport to steal space.
	};

	// Derived from Layer, so the engine's layer stack drives its lifetime:
	// OnAttach once at startup, OnUpdate and OnImGuiRender every frame,
	// OnDetach at shutdown. The layer owns its panels by value -- they are
	// members, not pointers, so their construction order is the member order
	// below and no panel can outlive the layer that drives it.
	class EditorLayer : public Layer
	{
	public:
		// Explicit ctor (not defaulted) because the camera controller needs its
		// aspect ratio at construction; see the .cpp initialiser list.
		EditorLayer();
		// Destructor defaulted rather than user-provided: every member's own
		// destructor releases what it holds, and a hand-written one here would
		// risk releasing members before the panels that reference them.
		virtual ~EditorLayer() = default;

		// OnAttach acquires editor-only global state (API keys, the AI tool
		// bridge, the panel set); OnDetach must give it back.
		virtual void OnAttach() override;
		virtual void OnDetach() override;

		// Not virtual: OnUpdate is reached through the base-class pointer by the
		// application loop, and this layer is the only Layer that overrides it,
		// so no sibling needs a different implementation.
		void OnUpdate(Timestep ts) override;
		// The bulk of the editor: dockspace, panels, viewport, menus. Runs every
		// frame on the UI thread, which is why any work queued from another
		// thread has to be drained here (see m_PendingFileOpens below).
		virtual void OnImGuiRender() override;
		// Event handlers return void: the layer records what it wants and applies
		// it on the next update, rather than mutating editor state mid-dispatch
		// while another handler is still reading it.
		void OnEvent(Event &e) override;

	private:
		// Return bool: false means "not consumed", letting the event propagate to
		// layers below. Scoped to the editor rather than the engine because
		// shortcut handling is editor policy, not application behaviour.
		bool OnKeyPressed(KeyPressedEvent &e);
		// Same propagation contract as the key handler, and the same reason:
		// picking an entity is editor policy that must not shadow a lower layer.
		bool OnMouseButtonPressed(MouseButtonPressedEvent &e);

		// Route a path to the panel that owns its extension. One dispatcher,
		// shared by the content browser and the AI-tool bridge, because
		// those two had drifted: the bridge queued only .py, and its drain
		// ignored the extension entirely.
		void OpenVisualFile(const std::string &path);
		// Rebuild the dockspace into a workflow's arrangement.
		//
		// ONLY ever called from the Layout menu, on click. Calling this from
		// OnImGuiRender tears down and rebuilds the whole dock tree every frame,
		// which is what made the editor layout unusable earlier: every panel
		// moved continuously and nothing could be dropped anywhere.
		void ApplyLayout(Workflow workflow);
		// Static and header-declared only: it maps the enum to a menu label and
		// is needed by the Layout menu, which is the sole caller of ApplyLayout.
		static const char *WorkflowName(Workflow workflow);

		// Drawn in a separate pass from the panels so the overlay composites
		// on top of the completed viewport image rather than into it.
		void OnOverlayRender();

		// In-game HUD drawn over the viewport while the scene is in Play.
		// Reads HealthComponent / WeaponComponent off the primary camera
		// entity, so it reflects live script-driven state rather than keeping
		// its own copy. No-ops when that entity lacks the components.
		void RenderGameplayHUD();

		// Scene and script commands are grouped: the file dialogs are opened here
		// and the actual load/save work happens in the handlers below, so the
		// menu items stay free of I/O.
		void NewScene();
		void OpenScene();
		void OpenScene(const std::filesystem::path &path);
		void SaveScene();
		void SaveSceneAs();

		// Script commands are deliberately separate from the scene ones above:
		// a script is a project asset that lives in the content browser, while a
		// scene is the document being edited, and they have different lifetimes.
		void NewScript();
		void OpenScript();
		void NewTerrainAsset();

		// Project lifecycle. Switching projects re-roots every panel that
		// resolves assets, so it goes through OnProjectChanged().
		void NewProject();
		void OpenProject();
		void OpenProject(const std::filesystem::path &path);

		// Re-roots asset-dependent state after the active project changes:
		// content browser, the open scene, and the tool server.
		// Push the project's persisted render settings into the renderer.
		// Called when a project is opened so a reopened project keeps its
		// render scale, post-processing chain and ray-tracing toggle.
		void ApplyRenderSettings();

		// Read the renderer's current render settings back into the project,
		// immediately before a save.
		void CaptureRenderSettings();

		void OnProjectChanged();

		// Serialization is a static-looking operation on an arbitrary scene,
		// because SaveSceneAs needs it for a scene that is not the active one.
		bool SerializeScene(Ref<Scene> scene, const std::filesystem::path &path);

		// Play / Simulate / Stop are three distinct runtime modes, not two:
		// Simulate runs the simulation without the gameplay HUD or player input,
		// which is what makes scene tweaking possible while a scene is live.
		void OnScenePlay();
		void OnSceneSimulate();
		void OnSceneStop();

		// Delegated to the scene rather than duplicated here: the entity copy
		// needs the scene's component registry, which is the scene's to own.
		void OnDuplicateEntity();

		// The toolbar is the editor's primary action row, separate from the
		// menu bar so the common commands stay reachable without a click.
		void UI_Toolbar();

		// Modal state is two members, not one struct: each modal needs its own
		// text buffer and its own error string, and sharing them would let a
		// failed New Project leave its error showing over the New Script dialog.
		void UI_NewScriptModal();
		void UI_NewProjectModal();
		void UI_ProjectMenu();

	// Second private section: everything below is state, with no behaviour.
	// Split from the methods above so the class reads as "what it does"
	// followed by "what it remembers".
	private:
		// Owned by value, not by Ref: the controller is pure maths with no GPU
		// or scene resources, so there is nothing to share and a controller that
		// outlived its layer would have nothing meaningful to drive.
		OrthographicCameraController m_CameraController;

		// The off-screen render target the viewport draws into. Ref<> because the
		// viewport and the picking code both read the same attachment.
		Ref<Framebuffer> m_Framebuffer;
		// Frame index for the WL_SCREENSHOT_FRAME hands-free capture hook.
		int m_FrameCounter = 0;
		// Reads colour attachment 0 of the bound framebuffer to a PNG. Returns
		// false on failure. OpenGL only -- see the implementation for why.
		bool CaptureFramebufferPNG(const std::string &path);
		// Records which backend created m_Framebuffer, so a runtime API switch
		// can detect that the framebuffer and its native resources belong to a
		// backend that is no longer current. Reset to None on the next recreate.
		RendererAPI::API m_FramebufferAPI = RendererAPI::API::None;

		// Two scene handles, not one. m_ActiveScene is what the user is editing
		// OR playing; m_EditorScene is the editor-side instance. They are usually
		// the same object (see OnAttach) and diverge when a separate scene is
		// loaded for play-in-editor, which is why both exist.
		Ref<Scene> m_ActiveScene;
		// Owned by Ref so the scene survives a swap in m_ActiveScene: closing a
		// scene must not destroy the one still being rendered against.
		Ref<Scene> m_EditorScene;
		// The on-disk path of m_EditorScene, kept so SaveScene can write back to
		// where the scene came from and so the title bar can show it. Empty for a
		// scene that has never been saved, which is what makes Save vs SaveAs differ.
		std::filesystem::path m_EditorScenePath;

		// Entity is a lightweight handle (id + owning scene), so an invalid
		// handle is cheap and "nothing hovered" needs no separate flag: the
		// default-constructed value is falsy.
		Entity m_HoveredEntity;
		// Last mouse position in viewport pixels, NOT screen pixels. Compared
		// against the current position to decide whether a GPU readback is
		// worth doing, so it must live in the same space as the pick. Seeded to
		// (-1,-1) so the first frame always counts as "the mouse moved".
		glm::ivec2 m_LastMousePixelPos = {-1, -1};
		// Frames since the last pick readback. A mouse that is perfectly still
		// over an animated scene would otherwise freeze hover highlighting
		// forever; this forces a periodic re-pick. Reset to 0 after every pick,
		// so the interval counts frames since the last completed readback.
		int m_PickFrameCounter = 0;

		// The camera the viewport renders from, distinct from the walk-camera
		// controller above: one flies, one is driven by the input controller,
		// and they are not interchangeable during Play.
		EditorCamera m_EditorCamera;

		// Ref<> because the same texture is sampled every viewport frame and by
		// the picking blit; recreating it per frame would churn GPU memory.
		Ref<Texture2D> m_CheckerboardTexture;

		// Viewport size in PIXELS as floats, not the ImGui panel size it comes
		// from: the fractional part is dropped when the framebuffer is resized,
		// so keeping the float here means the next frame recomputes the same
		// integer size and does not resize again on a rounding artefact.
		glm::vec2 m_ViewportSize = {0.0f, 0.0f};
		// Viewport rectangle in SCREEN coordinates: index 0 is the top-left
		// corner, index 1 the bottom-right. Screen space (not viewport space)
		// because both consumers transform into it -- the pick converts a
		// mouse position, and the gizmo wants the rect in screen pixels.
		// Uninitialised: only read after the first frame has set both corners.
		glm::vec2 m_ViewportBounds[2];

		// Modal open/closed flag and its text buffer are adjacent, not bundled,
		// so the two modals below can be reasoned about independently.
		bool m_ShowNewScriptModal = false;
		// Fixed 128 bytes with a value-initialised (not just default) start, so
		// the buffer is empty on the first frame the modal opens. A plain default
		// would leave indeterminate bytes and ImGui would render garbage.
		char m_NewScriptBuffer[128] = {0};

		// The workflow whose layout is currently on screen, defaulting to Scene.
		// Distinct from m_PendingWorkflow below: this is the applied state,
		// that one is the requested state waiting for the next DockSpace call.
		Workflow m_Workflow = Workflow::Scene;

		// One-shot: the default layout is applied on the first frame
		// that dockspace exists, so panels are tiled from startup instead
		// of floating until the user picks one from the Layout menu.
		// Without it the editor restored whatever imgui.ini happened to
		// hold, which is how panels ended up loose on screen.
		bool m_LayoutApplied = false;

		// A layout chosen in the menu is only RECORDED here; it is applied
		// on the next frame, immediately before ImGui::DockSpace().
		// Building the dock tree after DockSpace has already been submitted
		// for that frame leaves the two disagreeing, and the result is that
		// switching layouts twice leaves panels undocked.
		Workflow m_PendingWorkflow = Workflow::Scene;

		// The project modal's three members mirror the script modal's two above.
		bool m_ShowNewProjectModal = false;
		char m_NewProjectBuffer[128] = {0};
		// Error string from the last failed project open/create, rendered in the
		// modal. Cleared on each attempt, so it never shows a stale failure from
		// an earlier open that the user has since retried.
		std::string m_ProjectError;

		// Focus and hover are tracked separately because they gate different
		// behaviour: focus drives the walk-camera update, while hover decides
		// whether the viewport swallows events at all (see the .cpp).
		bool m_ViewportFocused = false;
		bool m_ViewportHovered = false;
		bool m_IsPaused = false;
		// Debug-tint colour for the solid-colour draw, exposed through a
		// ColorEdit4 in the settings panel. Initialised in BOTH the member
		// initialiser and the constructor's initialiser list -- redundant, but
		// harmless since the two agree.
		glm::vec4 m_SquareColor = {0.2f, 0.3f, 0.8f, 1.0f};

		// -1 means "no gizmo", which is why the sentinel is not 0: ImGuizmo's
		// TRANSLATE operation is 0, so 0 would silently select the move gizmo
		// instead of deselecting. The toolbar sets it to an ImGuizmo operation
		// and the viewport clears it back to -1.
		int m_GizmoType = -1;

		// Debug-view toggles for the remaining viewport panels. Each defaults on
		// except the collider overlay, which is expensive and off until asked.
		bool m_ShowPhysicsColliders = false;

		bool m_ShowViewport = true;
		bool m_ShowStats = true;
		bool m_ShowSettings = true;

		// Files written by the AI tool bridge from another thread. The queue is
		// the hand-off: the bridge pushes under the mutex, and OnImGuiRender
		// drains it on the UI thread, because ImGui is single-threaded and
		// calling into it from the bridge thread is not safe.
		// Pending file opens from AI tool calls (drained on UI thread).
		// Guarded by the mutex below. Cleared by the drain, not by the pusher, so
		// the bridge cannot observe a partially-drained frame.
		std::vector<std::string> m_PendingFileOpens;
		// std::mutex, not a RecursiveMutex or an atomic: the critical section is
		// a single push_back, and the UI-side drain swaps the vector under the
		// same lock so no element is read while a push is in flight.
		std::mutex m_PendingFileOpensMutex;

		// Panels as by-value members: each is stateless with respect to the layer
		// except for its own contents, so composition beats pointers here and
		// construction order is well defined.
		// Panels
		// The two panels that are always docked in the left column of every
		// workflow layout -- which is why leftRatio never varies in ApplyLayout.
		SceneHierarchyPanel m_SceneHierarchyPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		ScriptEditorPanel m_ScriptEditorPanel;
		// The NPC behaviour panel is the other half of the Visual workflow.
		ScriptInspectorPanel m_ScriptInspectorPanel;
	NPCBehaviorPanel m_NPCBehaviorPanel;
	// General visual scripting (.wlblocks). Separate from the
	// behaviour panel on purpose: a graph's structure is wiring and
	// flow, a tree's is priority order. The two share the runtime
	// registry, validator and serializer.
	VisualBlocksPanel m_VisualBlocksPanel;
	AvbGraphPanel m_AvbGraphPanel;
	TerrainAssetPanel m_TerrainAssetPanel;
		AIChatPanel m_AIChatPanel;
		// The model downloader is editor-only tooling, but it lives in the engine
		// target's dependency set rather than being conditionally compiled out,
		// so its code is present in every build.
		ModelDownloaderPanel m_ModelDownloaderPanel;

		// Play mode has three states rather than a bool because Simulate is a
		// third thing, not a variant of Play: it advances the simulation without
		// the player, so the HUD and player input stay inactive.
		enum class SceneState
		{
			Edit = 0,
			Play = 1,
			Simulate = 2
		};

		// Defaults to Edit, matching the default workflow above: the editor
		// opens in Scene layout and not running.
		SceneState m_SceneState = SceneState::Edit;
	};

}