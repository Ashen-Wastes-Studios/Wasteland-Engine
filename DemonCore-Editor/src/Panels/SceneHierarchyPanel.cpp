#include "SceneHierarchyPanel.h"

#include <imgui/imgui.h>

#include <Wasteland/Scene/Components.h>

#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui_internal.h>
#include <cstring>

#include <filesystem>
#include <Wasteland/Renderer/Texture.h>
#include <Wasteland/Renderer/Renderer3D.h>

/* The Microsoft C++ compiler is not-compatible with the C++ standard and needs
the following definition to disable a security warning on std::strncpy().
*/
#ifdef _MSVC_LANG
#define _CRT_SECURE_NO_WARNINGS
#endif

namespace Wasteland
{

	extern const std::filesystem::path g_AssetPath;

	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene> &context)
	{
		SetContext(context);
	}

	void SceneHierarchyPanel::SetContext(const Ref<Scene> &context)
	{
		m_Context = context;
		m_SelectionContext = {};
	}

	void SceneHierarchyPanel::OnImGuiRender()
	{
		ImGui::Begin("Scene Hierarchy");

		if (m_Context)
		{
			m_Context->m_Registry.view<TransformComponent, TagComponent>().each([&](entt::entity entityID, auto &transform, auto &tag)
																				{
					Entity entity{ entityID, m_Context.get() };
					DrawEntityNode(entity); });

			if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
				m_SelectionContext = {};

			// Right-click on blank space
			if (!ImGui::IsAnyItemHovered() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				ImGui::OpenPopup("HierarchyContextWindow");
			}

			if (ImGui::BeginPopup("HierarchyContextWindow"))
			{
				if (ImGui::MenuItem("Create Empty Entity"))
				{
					m_Context->CreateEntity("Empty Entity");
				}
				ImGui::EndPopup();
			}
		}
		ImGui::End();

		ImGui::Begin("Properties");
		if (m_SelectionContext)
		{
			if (!m_Context || !m_Context->m_Registry.valid(m_SelectionContext))
				m_SelectionContext = {};
		}

		if (m_SelectionContext)
		{
			DrawComponents(m_SelectionContext);

			if (ImGui::Button("Add Component"))
				ImGui::OpenPopup("AddComponent");

			if (ImGui::BeginPopup("AddComponent"))
			{
				if (!m_SelectionContext.HasComponent<CameraComponent>())
				{
					if (ImGui::MenuItem("Camera"))
					{
						m_SelectionContext.AddComponent<CameraComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<SpriteRendererComponent>())
				{
					if (ImGui::MenuItem("Sprite Renderer"))
					{
						m_SelectionContext.AddComponent<SpriteRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<CircleRendererComponent>())
				{
					if (ImGui::MenuItem("Circle Renderer"))
					{
						m_SelectionContext.AddComponent<CircleRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<CubeRendererComponent>())
				{
					if (ImGui::MenuItem("Cube Renderer"))
					{
						m_SelectionContext.AddComponent<CubeRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<SphereRendererComponent>())
				{
					if (ImGui::MenuItem("Sphere Renderer"))
					{
						m_SelectionContext.AddComponent<SphereRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<MaterialComponent>())
				{
					if (ImGui::MenuItem("Material Component"))
					{
						m_SelectionContext.AddComponent<MaterialComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<Rigidbody2DComponent>())
				{
					if (ImGui::MenuItem("Rigidbody 2D"))
					{
						m_SelectionContext.AddComponent<Rigidbody2DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<BoxCollider2DComponent>())
				{
					if (ImGui::MenuItem("Box Collider 2D"))
					{
						m_SelectionContext.AddComponent<BoxCollider2DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<CircleCollider2DComponent>())
				{
					if (ImGui::MenuItem("Circle Collider 2D"))
					{
						m_SelectionContext.AddComponent<CircleCollider2DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<ScriptComponent>())
				{
					if (ImGui::MenuItem("Script Component"))
					{
						m_SelectionContext.AddComponent<ScriptComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				ImGui::EndPopup();
			}
		}

		ImGui::End();
	}

	void SceneHierarchyPanel::SetSelectedEntity(Entity entity)
	{
		m_SelectionContext = entity;
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto &tag = entity.GetComponent<TagComponent>().Tag;

			ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
			bool opened = ImGui::TreeNodeEx((void *)(uint64_t)(uint32_t)entity, flags, tag.c_str());
			if (ImGui::IsItemClicked())
			{
				m_SelectionContext = entity;
			}

			bool entityDeleted = false;
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Delete Entity"))
					entityDeleted = true;

				ImGui::EndPopup();
			}

			if (opened)
			{
				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
				bool opened = ImGui::TreeNodeEx((void *)9817239, flags, tag.c_str());
				if (opened)
					ImGui::TreePop();
				ImGui::TreePop();
			}

			if (entityDeleted)
			{
				m_Context->DestroyEntity(entity);
				if (m_SelectionContext == entity)
					m_SelectionContext = {};
			}
		}
	}

	static void DrawVec3Control(const std::string &label, glm::vec3 &values, float resetValue = 0.0f, float columnWidth = 100.0f)
	{
		ImGui::PushID(label.c_str());

		ImGui::Columns(2);
		ImGui::SetColumnWidth(0, columnWidth);
		ImGui::Text(label.c_str());
		ImGui::NextColumn();

		ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});

		float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
		ImVec2 buttonSize = {lineHeight + 3.0f, lineHeight};

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
		if (ImGui::Button("X", buttonSize))
			values.x = resetValue;
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##X", &values.x, 0.1f);
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
		if (ImGui::Button("Y", buttonSize))
			values.y = resetValue;
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##Y", &values.y, 0.1f);
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
		if (ImGui::Button("Z", buttonSize))
			values.z = resetValue;
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##Z", &values.z, 0.1f);
		ImGui::PopItemWidth();

		ImGui::PopStyleVar();

		ImGui::Columns(1);

		ImGui::PopID();
	}

	void SceneHierarchyPanel::DrawComponents(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto &tag = entity.GetComponent<TagComponent>().Tag;

			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			strcpy_s(buffer, sizeof(buffer), tag.c_str());
			if (ImGui::InputText("Tag", buffer, sizeof(buffer)))
			{
				tag = std::string(buffer);
			}
		}

		const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowItemOverlap;

		if (entity.HasComponent<TransformComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(TransformComponent).hash_code(), treeNodeFlags, "Transform");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &tc = entity.GetComponent<TransformComponent>();
				DrawVec3Control("Translation", tc.Translation);
				glm::vec3 rotation = glm::degrees(tc.Rotation);
				DrawVec3Control("Rotation", rotation);
				tc.Rotation = glm::radians(rotation);
				DrawVec3Control("Scale", tc.Scale, 1.0f);

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<TransformComponent>();
		}

		if (entity.HasComponent<CameraComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CameraComponent).hash_code(), treeNodeFlags, "Camera");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &cameraComponent = entity.GetComponent<CameraComponent>();
				auto &camera = cameraComponent.Camera;

				ImGui::Checkbox("Primary", &cameraComponent.Primary);

				const char *projectionTypeStrings[] = {"Perspective", "Orthographic"};
				const char *currentProjectionTypeString = projectionTypeStrings[(int)camera.GetProjectionType()];
				if (ImGui::BeginCombo("Projection", currentProjectionTypeString))
				{
					for (int i = 0; i < 2; i++)
					{
						bool isSelected = currentProjectionTypeString == projectionTypeStrings[i];
						if (ImGui::Selectable(projectionTypeStrings[i], isSelected))
						{
							currentProjectionTypeString = projectionTypeStrings[i];
							camera.SetProjectionType((SceneCamera::ProjectionType)i);
						}

						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
				{
					float perspectiveVerticalFOV = glm::degrees(camera.GetPerspectiveVerticalFOV());
					if (ImGui::DragFloat("Vertical FOV", &perspectiveVerticalFOV))
						camera.SetPerspectiveVerticalFOV(glm::radians(perspectiveVerticalFOV));

					float perspectiveNear = camera.GetPerspectiveNearClip();
					if (ImGui::DragFloat("Near", &perspectiveNear))
						camera.SetPerspectiveNearClip(perspectiveNear);

					float perspectiveFar = camera.GetPerspectiveFarClip();
					if (ImGui::DragFloat("Far", &perspectiveFar))
						camera.SetPerspectiveFarClip(perspectiveFar);
				}

				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
				{
					float orthoSize = camera.GetOrthographicSize();
					if (ImGui::DragFloat("Size", &orthoSize))
						camera.SetOrthographicSize(orthoSize);

					float orthoNear = camera.GetOrthographicNearClip();
					if (ImGui::DragFloat("Near", &orthoNear))
						camera.SetOrthographicNearClip(orthoNear);

					float orthoFar = camera.GetOrthographicFarClip();
					if (ImGui::DragFloat("Far", &orthoFar))
						camera.SetOrthographicFarClip(orthoFar);

					ImGui::Checkbox("Fixed Aspect Ratio", &cameraComponent.FixedAspectRatio);
				}

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CameraComponent>();
		}

		if (entity.HasComponent<SpriteRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(SpriteRendererComponent).hash_code(), treeNodeFlags, "Sprite Renderer");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &src = entity.GetComponent<SpriteRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(src.Color));

				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t *path = (const wchar_t *)payload->Data;
						std::filesystem::path texturePath = std::filesystem::path(g_AssetPath) / path;
						src.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::DragFloat("Tiling Factor", &src.TilingFactor, 0.1f, 0.0f, 100.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<SpriteRendererComponent>();
		}

		if (entity.HasComponent<CircleRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CircleRendererComponent).hash_code(), treeNodeFlags, "Circle Renderer");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &crc = entity.GetComponent<CircleRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(crc.Color));
				ImGui::DragFloat("Thickness", &crc.Thickness, 0.025f, 0.0f, 1.0f);
				ImGui::DragFloat("Fade", &crc.Fade, 0.00025f, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CircleRendererComponent>();
		}

		if (entity.HasComponent<CubeRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CubeRendererComponent).hash_code(), treeNodeFlags, "Cube Renderer");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &crc = entity.GetComponent<CubeRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(crc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t *path = (const wchar_t *)payload->Data;
						std::filesystem::path texturePath = std::filesystem::path(g_AssetPath) / path;
						crc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragInt("Texture Index", &crc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CubeRendererComponent>();
		}

		if (entity.HasComponent<SphereRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(SphereRendererComponent).hash_code(), treeNodeFlags, "Sphere Renderer");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &src = entity.GetComponent<SphereRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(src.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t *path = (const wchar_t *)payload->Data;
						std::filesystem::path texturePath = std::filesystem::path(g_AssetPath) / path;
						src.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Radius", &src.Radius, 0.1f, 0.0f, 100.0f);
				ImGui::DragInt("Sectors", &src.Sectors, 1.0f, 3.0f, 100.0f);
				ImGui::DragInt("Stacks", &src.Stacks, 1.0f, 2.0f, 100.0f);
				ImGui::DragInt("Texture Index", &src.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<SphereRendererComponent>();
		}

		if (entity.HasComponent<MaterialComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(MaterialComponent).hash_code(), treeNodeFlags, "Material Component");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &mc = entity.GetComponent<MaterialComponent>();

				ImGui::ColorEdit4("Albedo", glm::value_ptr(mc.Albedo));
				ImGui::DragFloat("Metallic", &mc.Metallic, 0.05f, 0.0f, 1.0f);
				ImGui::DragFloat("Roughness", &mc.Roughness, 0.05f, 0.0f, 1.0f);
				ImGui::ColorEdit4("Emission Color", glm::value_ptr(mc.EmissionColor));
				ImGui::DragFloat("Emission Intensity", &mc.EmissionIntensity, 0.05f, 0.0f, FLT_MAX);

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<MaterialComponent>();
		}

		if (entity.HasComponent<Rigidbody2DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(Rigidbody2DComponent).hash_code(), treeNodeFlags, "Rigidbody 2D");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &rigidbody2D = entity.GetComponent<Rigidbody2DComponent>();
				const char *bodyTypeStrings[] = {"Static", "Dynamic", "Kinematic"};
				const char *currentBodyTypeString = bodyTypeStrings[(int)rigidbody2D.Type];
				if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
				{
					for (int i = 0; i < 2; i++)
					{
						bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
						if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
						{
							currentBodyTypeString = bodyTypeStrings[i];
							rigidbody2D.Type = (Rigidbody2DComponent::BodyType)i;
						}

						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				ImGui::Checkbox("Fixed Rotation", &rigidbody2D.FixedRotation);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<Rigidbody2DComponent>();
		}

		if (entity.HasComponent<BoxCollider2DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(BoxCollider2DComponent).hash_code(), treeNodeFlags, "Box Collider 2D");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &boxCollider2D = entity.GetComponent<BoxCollider2DComponent>();

				ImGui::DragFloat2("Offset", glm::value_ptr(boxCollider2D.Offset));
				ImGui::DragFloat2("Size", glm::value_ptr(boxCollider2D.Size));
				ImGui::DragFloat("Density", &boxCollider2D.Density, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Friction", &boxCollider2D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &boxCollider2D.Restitution, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution Threshold", &boxCollider2D.RestitutionThreshold, 0.01f, 0.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<BoxCollider2DComponent>();
		}

		if (entity.HasComponent<CircleCollider2DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CircleCollider2DComponent).hash_code(), treeNodeFlags, "Circle Collider 2D");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &circleCollider2D = entity.GetComponent<CircleCollider2DComponent>();

				ImGui::DragFloat2("Offset", glm::value_ptr(circleCollider2D.Offset));
				ImGui::DragFloat("Radius", &circleCollider2D.Radius, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Density", &circleCollider2D.Density, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Friction", &circleCollider2D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &circleCollider2D.Restitution, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution Threshold", &circleCollider2D.RestitutionThreshold, 0.01f, 0.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CircleCollider2DComponent>();
		}

		if (entity.HasComponent<ScriptComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(ScriptComponent).hash_code(), treeNodeFlags, "Script Component");
			ImGui::SameLine(ImGui::GetWindowWidth() - 25.0f);
			if (ImGui::Button("+", ImVec2{20, 20}))
			{
				ImGui::OpenPopup("ComponentSettings");
			}
			ImGui::PopStyleVar();

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove Component"))
					removeComponent = true;

				ImGui::EndPopup();
			}

			if (open)
			{
				auto &scriptComponent = entity.GetComponent<ScriptComponent>();

				char buffer[256];
				memset(buffer, 0, sizeof(buffer));
				strcpy_s(buffer, sizeof(buffer), scriptComponent.ScriptName.c_str());

				if (ImGui::InputText("Script Class", buffer, sizeof(buffer)))
				{
					scriptComponent.ScriptName = std::string(buffer);
				}

				ImGui::Button("Drag Script Here", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t *path = (const wchar_t *)payload->Data;
						std::filesystem::path scriptPath = path;

						scriptComponent.ScriptName = scriptPath.stem().string();
						scriptComponent.ScriptPath = std::filesystem::absolute(scriptPath).string();
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<ScriptComponent>();
		}
	}

}