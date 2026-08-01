#pragma once

#include "Wasteland.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptInspectorPanel.h"

#include <Wasteland/Renderer/Texture.h>

#include "Wasteland/Renderer/EditorCamera.h"

namespace Wasteland
{

	class EditorLayer : public Layer
	{
	public:
		EditorLayer();
		virtual ~EditorLayer() = default;

		virtual void OnAttach() override;
		virtual void OnDetach() override;

		void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		void OnEvent(Event &e) override;

	private:
		bool OnKeyPressed(KeyPressedEvent &e);
		bool OnMouseButtonPressed(MouseButtonPressedEvent &e);

		void OnOverlayRender();

		void NewScene();
		void OpenScene();
		void OpenScene(const std::filesystem::path &path);
		void SaveScene();
		void SaveSceneAs();

		void NewScript();
		void OpenScript();

		void SerializeScene(Ref<Scene> scene, const std::filesystem::path &path);

		void OnScenePlay();
		void OnSceneSimulate();
		void OnSceneStop();

		void OnDuplicateEntity();

		void UI_Toolbar();

		void UI_NewScriptModal();

	private:
		OrthographicCameraController m_CameraController;

		Ref<Framebuffer> m_Framebuffer;

		Ref<Scene> m_ActiveScene;
		Ref<Scene> m_EditorScene;
		std::filesystem::path m_EditorScenePath;

		Entity m_HoveredEntity;
		glm::ivec2 m_LastMousePixelPos = {-1, -1};
		int m_PickFrameCounter = 0;

		EditorCamera m_EditorCamera;

		Ref<Texture2D> m_CheckerboardTexture;

		glm::vec2 m_ViewportSize = {0.0f, 0.0f};
		glm::vec2 m_ViewportBounds[2];

		bool m_ShowNewScriptModal = false;
		char m_NewScriptBuffer[128] = {0};

		bool m_ViewportFocused = false;
		bool m_ViewportHovered = false;
		bool m_IsPaused = false;
		glm::vec4 m_SquareColor = {0.2f, 0.3f, 0.8f, 1.0f};

		int m_GizmoType = -1;

		bool m_ShowPhysicsColliders = false;

		// Panels
		SceneHierarchyPanel m_SceneHierarchyPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		ScriptEditorPanel m_ScriptEditorPanel;
		ScriptInspectorPanel m_ScriptInspectorPanel;

		enum class SceneState
		{
			Edit = 0,
			Play = 1,
			Simulate = 2
		};

		SceneState m_SceneState = SceneState::Edit;
	};

}