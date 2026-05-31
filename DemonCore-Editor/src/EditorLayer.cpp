#include "EditorLayer.h"

#include <imgui/imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Wasteland/Scene/Entity.h>

#include "Wasteland/Scene/SceneSerializer.h"

#include "Wasteland/Utils/PlatformUtils.h"

#include "ImGuizmo.h"

#include "Wasteland/Math/Math.h"

#include <fstream>
#include <filesystem>

namespace Wasteland
{

	extern const std::filesystem::path g_AssetPath;

	static const uint32_t s_MapWidth = 24;
	static const char *s_MapTiles =
		"WWWWWWWWWWWWWWWWWWWWWWWW"
		"WWWWWWWDDDDDWWWWWWWWWWWW"
		"WWWWWDDDDDDDDDDWWWWWWWWW"
		"WWWWDDDDDDDDDDDDCDWWWWWW"
		"WWWDDDDDDDDDDDDDDDDDWWWW"
		"WWDDDDWWWDDDDDDDDDDDDWWW"
		"WDDDDDWWWDDDDDDDDDDDDDWW"
		"WWDDDDDDDDDDDDDDDDDDDWWW"
		"WWWWDDDDDDDDDDDDDDDDWWWW"
		"WWWWWDDDDDDDDDDDDDDWWWWW"
		"WWWWWWDDDDDDDDDDDWWWWWWW"
		"WWWWWWWDDDDDDDDDWWWWWWWW"
		"WWWWWWWWWWDDDDWWWWWWWWWW"
		"WWWWWWWWWWWWWWWWWWWWWWWW";

	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_CameraController(1280.0f / 720.0f), m_SquareColor({0.2f, 0.3f, 0.8f, 1.0f})
	{
	}

	void EditorLayer::OnAttach()
	{
		WL_PROFILE_FUNCTION();

		m_CheckerboardTexture = nullptr;
		m_SpriteSheet = nullptr;

		FramebufferSpecification fbSpec;
		fbSpec.Attachments = {FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth};
		fbSpec.Width = 1280;
		fbSpec.Height = 720;
		m_Framebuffer = Framebuffer::Create(fbSpec);

		m_ActiveScene = CreateRef<Scene>();

		m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		m_TextureStairs = nullptr;
		m_TextureTree = nullptr;

		m_MapWidth = s_MapWidth;
		m_MapHeight = strlen(s_MapTiles) / s_MapWidth;

		s_TextureMap['D'] = nullptr;
		s_TextureMap['W'] = nullptr;

		m_CameraController.SetZoomLevel(5.0f);
	}

	void EditorLayer::OnDetach()
	{
		WL_PROFILE_FUNCTION();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WL_PROFILE_FUNCTION();

		// Resize
		if (FramebufferSpecification spec = m_Framebuffer->GetSpecification();
			m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f && // zero sized framebuffer is invalid
			(spec.Width != m_ViewportSize.x || spec.Height != m_ViewportSize.y))
		{
			m_Framebuffer->Resize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_CameraController.OnResize(m_ViewportSize.x, m_ViewportSize.y);
			Renderer3D::ResizeRayTraceTarget((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}

		// Render
		Renderer2D::ResetStats();
		Renderer3D::ResetStats();

		bool isRayTracing = Renderer3D::IsRayTracingEnabled();

		if (!isRayTracing)
		{
			// Standard raster pass needs the framebuffer bound
			m_Framebuffer->Bind();
			RenderCommand::SetClearColor({0.1f, 0.1f, 0.1f, 1.0f});
			RenderCommand::Clear();
			m_Framebuffer->ClearAttachment(1, -1);
		}
		else
		{
			// Ray tracing bypasses the default framebuffer entirely!
			// We just need to clear out your 2D debug overlay stats/pipeline states
			RenderCommand::SetClearColor({0.1f, 0.1f, 0.1f, 1.0f});
		}

		switch (m_SceneState)
		{
		case SceneState::Edit:
		{
			if (m_ViewportFocused)
			{
				m_CameraController.OnUpdate(ts);
			}

			m_EditorCamera.OnUpdate(ts);

			m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
			break;
		}
		case SceneState::Play:
		{
			m_ActiveScene->OnUpdateRuntime(ts);
			break;
		}
		}

		// Handle non-raytraced system layouts (like entity picking and overlays)
		if (!isRayTracing)
		{
			auto [mx, my] = ImGui::GetMousePos();
			mx -= m_ViewportBounds[0].x;
			my -= m_ViewportBounds[0].y;
			glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
			my = viewportSize.y - my;
			int mouseX = (int)mx;
			int mouseY = (int)my;

			if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
			{
				int pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
				m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());
			}

			OnOverlayRender();
			m_Framebuffer->Unbind(); // Safely unbind the active frame buffer
		}
		else
		{
			// Reset entity mouse states when tracing rays to avoid reading dead FBO memory bounds
			m_HoveredEntity = Entity();
		}
	}

	void EditorLayer::OnImGuiRender()
	{
		WL_PROFILE_FUNCTION();

		// Note: Switch this to true to enable dockspace
		static bool dockingEnabled = true;
		if (dockingEnabled)
		{
			static bool dockspaceOpen = true;
			static bool opt_fullscreen = true;
			static bool opt_padding = false;
			static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

			ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
			if (opt_fullscreen)
			{
				const ImGuiViewport *viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::SetNextWindowViewport(viewport->ID);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
				window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
				window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
			}
			else
			{
				dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
			}

			if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
				window_flags |= ImGuiWindowFlags_NoBackground;

			if (!opt_padding)
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);
			if (!opt_padding)
				ImGui::PopStyleVar();

			if (opt_fullscreen)
				ImGui::PopStyleVar(2);

			// Submit the DockSpace
			ImGuiIO &io = ImGui::GetIO();
			if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
			{
				ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
				ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
			}

			if (ImGui::BeginMenuBar())
			{
				if (ImGui::BeginMenu("File"))
				{
					if (ImGui::MenuItem("New"))
						NewScene();

					if (ImGui::MenuItem("Open..."))
						OpenScene();

					if (ImGui::MenuItem("Save As..."))
						SaveSceneAs();

					ImGui::Separator();

					if (ImGui::MenuItem("Exit"))
						Wasteland::Application::Get().Close();
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Scripts"))
				{
					if (ImGui::MenuItem("New"))
						NewScript();

					if (ImGui::MenuItem("Open"))
						OpenScript();

					ImGui::EndMenu();
				}

				ImGui::EndMenuBar();
			}

			m_SceneHierarchyPanel.OnImGuiRender();
			m_ContentBrowserPanel.OnImGuiRender();

			ImGui::Begin("Stats");

			std::string name = "None";
			if (m_HoveredEntity)
			{
				if (m_HoveredEntity.HasComponent<TagComponent>())
					name = m_HoveredEntity.GetComponent<TagComponent>().Tag;
				else
					name = "Unnamed Entity";
			}
			ImGui::Text("Hovered Entity: %s", name.c_str());

			ImGui::Separator();

			auto stats = Wasteland::Renderer2D::GetStats();
			ImGui::Text("Renderer2D Stats:");
			ImGui::Text("Draw Calls: %d", stats.DrawCalls);
			ImGui::Text("Quads: %d", stats.QuadCount);
			ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

			ImGui::Separator();

			auto stats3D = Wasteland::Renderer3D::GetStats();
			ImGui::Text("Renderer3D Stats:");
			ImGui::Text("Draw Calls: %d", stats3D.DrawCalls);
			ImGui::Text("Quads: %d", stats3D.QuadCount);
			ImGui::Text("Vertices: %d", stats3D.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats3D.GetTotalIndexCount());

			ImGui::End();

			ImGui::Begin("Settings");

			ImGui::Checkbox("Show physics colliders", &m_ShowPhysicsColliders);

			bool rtEnabled = Wasteland::Renderer3D::IsRayTracingEnabled();
			if (ImGui::Checkbox("Nova Rendering Pipeline", &rtEnabled))
			{
				Wasteland::Renderer3D::SetRayTracingEnabled(rtEnabled);
			}

			ImGui::End();

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
			ImGui::Begin("Viewport");

			ImVec2 minBound = ImGui::GetCursorScreenPos();

			m_ViewportFocused = ImGui::IsWindowFocused();
			m_ViewportHovered = ImGui::IsWindowHovered();
			Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportFocused && !m_ViewportHovered);

			ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
			m_ViewportSize = {viewportPanelSize.x, viewportPanelSize.y};

			ImVec2 maxBound = {minBound.x + m_ViewportSize.x, minBound.y + m_ViewportSize.y};
			m_ViewportBounds[0] = {minBound.x, minBound.y};
			m_ViewportBounds[1] = {maxBound.x, maxBound.y};

			uint32_t textureID = 0;
			if (Wasteland::Renderer3D::IsRayTracingEnabled())
			{
				textureID = Wasteland::Renderer3D::GetRayTraceTargetID();
			}
			else
			{
				textureID = m_Framebuffer->GetColorAttachmentRendererID();
			}

			ImGui::Image(reinterpret_cast<void *>(static_cast<uintptr_t>(textureID)),
						 ImVec2{m_ViewportSize.x, m_ViewportSize.y},
						 ImVec2{0, 1}, ImVec2{1, 0});

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const wchar_t *path = (const wchar_t *)payload->Data;
					OpenScene(std::filesystem::path(g_AssetPath) / path);
				}

				ImGui::EndDragDropTarget();
			}

			// Gizmos
			Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
			if (m_ActiveScene && selectedEntity && m_GizmoType != -1 && m_ActiveScene->IsEntityValid(selectedEntity))
			{
				ImGuizmo::SetOrthographic(false);
				ImGuizmo::SetDrawlist();
				ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportSize.x, m_ViewportSize.y);

				const glm::mat4 &cameraProjection = m_EditorCamera.GetProjection();
				glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();

				auto &tc = selectedEntity.GetComponent<TransformComponent>();
				glm::mat4 transform = tc.GetTransform();

				bool snap = Input::IsKeyPressed(WL_KEY_LEFT_CONTROL);
				float snapValue = 0.5f;
				if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
					snapValue = 45.0f;

				float snapValues[3] = {snapValue, snapValue, snapValue};

				ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
									 (ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(transform),
									 nullptr, snap ? snapValues : nullptr);

				if (ImGuizmo::IsUsing())
				{
					glm::vec3 translation, rotation, scale;
					Math::DecomposeTransform(transform, translation, rotation, scale);

					glm::vec3 deltaRotation = rotation - tc.Rotation;
					tc.Translation = translation;
					tc.Rotation += deltaRotation;
					tc.Scale = scale;
				}
			}

			ImGui::End();
			ImGui::PopStyleVar();

			UI_Toolbar();

			ImGui::End(); // End DockSpace

			UI_NewScriptModal();
		}
		else
		{
			ImGui::Begin("Settings");

			auto stats = Renderer2D::GetStats();
			ImGui::Text("Renderer2D Stats:");
			ImGui::Text("Draw Calls: %d", stats.DrawCalls);
			ImGui::Text("Quads: %d", stats.QuadCount);
			ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

			ImGui::Separator();

			auto stats3D = Renderer3D::GetStats();
			ImGui::Text("Renderer3D Stats:");
			ImGui::Text("Draw Calls: %d", stats3D.DrawCalls);
			ImGui::Text("Quads: %d", stats3D.QuadCount);
			ImGui::Text("Vertices: %d", stats3D.GetTotalVertexCount());

			bool rtEnabled = Renderer3D::IsRayTracingEnabled();
			if (ImGui::Checkbox("Nova Rendering Pipeline", &rtEnabled))
			{
				Renderer3D::SetRayTracingEnabled(rtEnabled);
			}
			ImGui::Text("Indices: %d", stats3D.GetTotalIndexCount());

			ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));

			uint32_t textureID = m_CheckerboardTexture->GetRendererID();
			ImGui::Image((void *)textureID, ImVec2{1280.0f, 720.0f}, ImVec2{0, 1}, ImVec2{1, 0});
			ImGui::End();
		}
	}

	void EditorLayer::UI_Toolbar()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));

		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		float size = ImGui::GetWindowHeight() - 4.0f;
		float totalWidth = (size * 2.0f) + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SameLine((ImGui::GetWindowContentRegionMax().x * 0.5f) - (totalWidth * 0.5f));
		if (ImGui::Button("Play"))
		{
			if (m_SceneState == SceneState::Edit)
				OnScenePlay();
		}

		ImGui::SameLine();
		if (ImGui::Button("Stop"))
		{
			if (m_SceneState == SceneState::Play)
				OnSceneStop();
		}

		ImGui::PopStyleVar();
		ImGui::PopStyleVar();

		ImGui::End();
	}

	void EditorLayer::UI_NewScriptModal()
	{
		if (m_ShowNewScriptModal)
			ImGui::OpenPopup("New Script");

		if (ImGui::BeginPopupModal("New Script", &m_ShowNewScriptModal, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Enter script name:");
			ImGui::InputText("##ScriptName", m_NewScriptBuffer, IM_ARRAYSIZE(m_NewScriptBuffer));

			if (ImGui::Button("Create", ImVec2(120, 0)))
			{
				if (strlen(m_NewScriptBuffer) > 0)
				{
					std::filesystem::path scriptDir = g_AssetPath / "scripts";
					if (!std::filesystem::exists(scriptDir))
						std::filesystem::create_directories(scriptDir);

					std::string fileName = m_NewScriptBuffer;
					if (fileName.find(".py") == std::string::npos)
						fileName += ".py";

					std::filesystem::path scriptPath = scriptDir / fileName;
					std::ofstream outFile(scriptPath);

					if (outFile.is_open())
					{
						outFile << "# Wasteland Engine Script\n";
						outFile << "class " << m_NewScriptBuffer << ":\n";
						outFile << "    def __init__(self):\n";
						outFile << "		pass\n";
						outFile << "    def OnUpdateEntity(self, dt):\n";
						outFile << "		pass\n";
						outFile.close();
						WL_CORE_INFO("Created new script at: {0}", scriptPath.string());
					}
				}
				m_ShowNewScriptModal = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)))
			{
				m_ShowNewScriptModal = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void EditorLayer::OnEvent(Event &e)
	{
		m_CameraController.OnEvent(e);
		m_EditorCamera.OnEvent(e);

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>(WL_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(WL_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
	}

	bool EditorLayer::OnKeyPressed(KeyPressedEvent &e)
	{
		if (e.GetRepeatCount() > 0)
			return false;

		bool control = Input::IsKeyPressed(WL_KEY_LEFT_CONTROL) || Input::IsKeyPressed(WL_KEY_RIGHT_CONTROL);
		bool shift = Input::IsKeyPressed(WL_KEY_LEFT_SHIFT) || Input::IsKeyPressed(WL_KEY_RIGHT_SHIFT);
		switch (e.GetKeyCode())
		{
		case WL_KEY_N:
		{
			if (control)
				NewScene();
			break;
		}
		case WL_KEY_O:
		{
			if (control)
				OpenScene();
			break;
		}
		case WL_KEY_S:
		{
			if (control)
			{
				if (shift)
					SaveSceneAs();
				else
					SaveScene();
			}
			break;
		}
		case WL_KEY_D:
		{
			if (control)
				OnDuplicateEntity();
			break;
		}
		case WL_KEY_Q:
			m_GizmoType = -1;
			break;
		case WL_KEY_W:
			m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
			break;
		case WL_KEY_E:
			m_GizmoType = ImGuizmo::OPERATION::ROTATE;
			break;
		case WL_KEY_R:
			m_GizmoType = ImGuizmo::OPERATION::SCALE;
			break;
		}
		return false;
	}

	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent &e)
	{
		if (e.GetMouseButton() == WL_MOUSE_BUTTON_LEFT)
		{
			if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(WL_KEY_LEFT_ALT))
				m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
		}
		return false;
	}

	void EditorLayer::OnOverlayRender()
	{
		if (m_SceneState == SceneState::Play)
		{
			Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
			if (!camera)
			{
				return;
			}
			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
		}
		else
		{
			Renderer2D::BeginScene(m_EditorCamera);
		}

		if (m_ShowPhysicsColliders)
		{
			// Box Colliders
			{
				auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation) * glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawRect(transform, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
				}
			}

			// Circle Colliders
			{
				auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawCircle(transform, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.015f);
				}
			}
		}

		Renderer2D::EndScene();
	}

	void EditorLayer::NewScene()
	{
		m_ActiveScene = CreateRef<Scene>();
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		m_EditorScenePath = std::filesystem::path();
	}

	void EditorLayer::OpenScene()
	{
		std::string filepath = FileDialogs::OpenFile("Wasteland Scene (*.wastescene)\0*.wastescene\0");
		if (!filepath.empty())
		{
			OpenScene(filepath);
		}
	}

	void EditorLayer::OpenScene(const std::filesystem::path &path)
	{
		if (m_SceneState != SceneState::Edit)
			OnSceneStop();

		Ref<Scene> newScene = CreateRef<Scene>();
		newScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);

		m_SceneHierarchyPanel.SetContext(newScene);

		SceneSerializer serializer(newScene);
		if (serializer.Deserialize(path.string()))
		{
			m_EditorScene = newScene;
			m_EditorScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_SceneHierarchyPanel.SetContext(m_EditorScene);

			m_ActiveScene = m_EditorScene;
			m_EditorScenePath = path;
		}
	}

	void EditorLayer::SaveScene()
	{
		if (!m_EditorScenePath.empty())
			SerializeScene(m_ActiveScene, m_EditorScenePath);
		else
			SaveSceneAs();
	}

	void EditorLayer::SaveSceneAs()
	{
		std::string filepath = FileDialogs::SaveFile("Wasteland Scene (*.wastescene)\0*.wastescene\0");
		if (!filepath.empty())
		{
			SerializeScene(m_ActiveScene, filepath);

			m_EditorScenePath = filepath;
		}
	}

	void EditorLayer::NewScript()
	{
		m_ShowNewScriptModal = true;
		memset(m_NewScriptBuffer, 0, sizeof(m_NewScriptBuffer));
	}

	void EditorLayer::OpenScript()
	{
		// Open file dialog
		std::string filepath = FileDialogs::OpenFile("Python Script (*.py)\0*.py\0");

		if (!filepath.empty())
		{
			// Construct the command.
			std::string command = "code \"" + filepath + "\"";

			int result = std::system(command.c_str());

			if (result != 0)
			{
				WL_CORE_ERROR("Failed to open script. Ensure 'code' (VS Code) is added to your system PATH. Attempted: {0}", command);
			}
			else
			{
				WL_CORE_INFO("Opened script in VS Code: {0}", filepath);
			}
		}
	}

	void EditorLayer::SerializeScene(Ref<Scene> scene, const std::filesystem::path &path)
	{
		SceneSerializer serializer(scene);
		serializer.Serialize(path.string());
	}

	void EditorLayer::OnScenePlay()
	{
		m_SceneState = SceneState::Play;

		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnRuntimeStart();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnSceneStop()
	{
		m_SceneState = SceneState::Edit;

		m_ActiveScene->OnRuntimeStop();
		m_ActiveScene = m_EditorScene;

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnDuplicateEntity()
	{
		if (m_SceneState != SceneState::Edit)
			return;

		Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		if (selectedEntity)
			m_EditorScene->DuplicateEntity(selectedEntity);
	}
}