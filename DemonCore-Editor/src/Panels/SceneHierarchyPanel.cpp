#include "SceneHierarchyPanel.h"

#include <imgui/imgui.h>

#include <Wasteland/Scene/Components.h>

#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui_internal.h>
#include <cstring>

#include <filesystem>
#include <Wasteland/Renderer/Texture.h>
#include <Wasteland/Renderer/Renderer3D.h>
#include <Wasteland/Project/Project.h>

/* The Microsoft C++ compiler is not-compatible with the C++ standard and needs
the following definition to disable a security warning on std::strncpy().
*/
#ifdef _MSVC_LANG
#define _CRT_SECURE_NO_WARNINGS
#endif

namespace Wasteland
{

	// Resolves a CONTENT_BROWSER_ITEM drop payload to an absolute path in
	// the active project. The payload is a NUL-terminated UTF-8
	// project-relative path; it was previously a wchar_t*, which is 4 bytes
	// wide on Linux and would have been truncated there.
	// TRANSLATES AN ImGui DRAG-AND-DROP PAYLOAD INTO AN ABSOLUTE ASSET PATH.
	//
	// THE PAYLOAD IS EXPECTED TO BE A NUL-TERMINATED RELATIVE PATH STRING, not ImGui's
	// usual FILE payload struct -- there is no size field, so the producer and consumer
	// must agree on the format out of band. Resolving through Project keeps asset
	// lookups relative to the active project rather than the process working
	// directory, which is what lets a project be opened from anywhere.
	//
	// Note this is the ONLY place the payload is decoded; every drop target in this
	// file calls it rather than re-reading payload->Data, so the assumed format has
	// exactly one definition.
	static std::filesystem::path ResolveContentBrowserDrop(const ImGuiPayload *payload)
	{
		const char *relative = static_cast<const char *>(payload->Data);
		return Project::Get().GetAssetPath(relative);
	}

	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene> &context)
	{
		SetContext(context);
	}

	// SWAPPING THE SCENE CLEARS THE SELECTION AS A SIDE EFFECT.
	//
	// m_SelectionContext is reset to an empty Entity. That is necessary rather than
	// incidental: an Entity holds a raw pointer to its scene, so retaining a selection
	// across a context change would leave it pointing at the previous scene's
	// registry -- and any subsequent HasComponent or GetComponent would dereference
	// freed memory.
	//
	// The same reasoning applies whenever the selection is cleared elsewhere, such as
	// on a background click in OnImGuiRender.
	void SceneHierarchyPanel::SetContext(const Ref<Scene> &context)
	{
		m_Context = context;
		m_SelectionContext = {};
	}

	// THE PANEL ROOT, AND EVERYTHING BELOW IT ASSUMES THIS FUNCTION RAN.
	//
	// The hierarchy list, the properties inspector and the AddComponent menu are all
	// driven from here in one pass, rather than being separate On* calls. That ordering
	// is what lets the properties inspector draw immediately after the selection changes
	// -- clicking an entity and seeing its components update in the same frame needs
	// both to live in one function.
	//
	// The registry view is FLAT: it iterates every entity with a Transform and Tag
	// directly, with no parent-child relationship. The tree nodes exist for the ImGui
	// affordance only; see the hardcoded child-node ID note in DrawEntityNode.
	void SceneHierarchyPanel::OnImGuiRender()
	{
		if (m_HierarchyVisible)
		{
			ImGui::Begin("Scene Hierarchy", &m_HierarchyVisible);

		if (m_Context)
		{
			// THE VIEW REQUIRES BOTH Transform AND Tag, so an entity missing either is
			// INVISIBLE in the hierarchy.
			//
			// This is a deliberate filter rather than an oversight -- Tag is what has a name to
			// display -- but it has a consequence worth stating: deleting a Tag component, or
			// an entity being constructed without one, makes it vanish from the hierarchy with
			// no indication that it still exists in the scene.
			//
			// The lambda captures by reference and takes both components by reference, but
			// NEITHER IS USED -- DrawEntityNode re-fetches the tag itself. The parameters are
			// required by EnTT's view signature, not by the body.
			m_Context->m_Registry.view<TransformComponent, TagComponent>().each([&](entt::entity entityID, auto &transform, auto &tag)
																				{
					// THE WRAPPER CARRIES A RAW SCENE POINTER, not a Ref.
					//
					// Entity is a lightweight handle: an EnTT entity plus the scene it belongs to, so
					// HasComponent and GetComponent can reach the right registry. It does NOT keep the
					// scene alive.
					//
					// That is why m_SelectionContext must be cleared whenever the context changes (see
					// SetContext) -- a stale Entity would dereference a destroyed scene. The same
					// reasoning is why the selection is checked for validity before use rather than
					// being assumed live.
					Entity entity{ entityID, m_Context.get() };
					DrawEntityNode(entity); });

			// A LEFT-CLICK ON EMPTY PANEL SPACE CLEARS THE SELECTION, and the test is on
			// IsMouseDown rather than IsMouseClicked.
			//
			// Using IsMouseDown means the selection clears as soon as the button goes down
			// over empty space, which is slightly more immediate than waiting for release.
			// It also means the behaviour fires even if the press began outside the window.
			//
			// IsWindowHovered alone is sufficient here -- no IsAnyItemHovered check is needed,
			// because a click on a tree node is already handled as a selection change above,
			// and this branch only runs for clicks that reached no item.
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
		}

		if (m_PropertiesVisible)
		{
			ImGui::Begin("Properties", &m_PropertiesVisible);
		if (m_SelectionContext)
		{
			// THE SELECTION IS RE-VALIDATED EVERY FRAME, not just on click.
			//
			// An EnTT handle is only an integer index; it stays numerically valid after an
			// entity is destroyed, and the registry may recycle that slot for a different entity.
			// Checking m_Registry.valid() therefore catches both cases --
			//
			//   * a handle to a deleted entity, which would otherwise index a live slot belonging
			//     to something else and silently edit the WRONG entity, and
			//   * any handle left behind after the scene itself was destroyed, guarded by the
			//     m_Context null check.
			//
			// This is what makes clearing the selection on delete sufficient rather than merely
			// tidy. It runs unconditionally each frame because deletion can happen from anywhere
			// -- the hierarchy's own Delete key, a script through the bindings, or a scene load.
			//
			// m_SelectionContext is assigned a default-constructed Entity, which is falsy, so the
			// enclosing `if (m_SelectionContext)` below then skips both the redraw and the menu.
			if (!m_Context || !m_Context->m_Registry.valid(m_SelectionContext))
				m_SelectionContext = {};
		}

		if (m_SelectionContext)
		{
			DrawComponents(m_SelectionContext);

			if (ImGui::Button("Add Component"))
				ImGui::OpenPopup("AddComponent");

			// ONE POPUP SPANS THE WHOLE COMPONENT MENU -- around 29 MenuItem blocks, each
			// guarded by a HasComponent check.
			//
			// The guard means a component already present is not offered, so the menu shrinks
			// as an entity accumulates components. That is the whole mechanism: there is no
			// separate disabled-state rendering, the entry simply is not created.
			//
			// Each branch calls CloseCurrentPopup after adding, so the menu closes on selection
			// rather than requiring an explicit dismissal.
			//
			// NOTE the popup is opened once and closed once at the very end, so it spans a large
			// block of the function. An EndPopup for one branch is NOT what balances these --
			// there is exactly one, matching the single Begin.
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
						// THE 2D RENDERERS: SPRITE, CIRCLE, CUBE AND SPHERE.
						//
						// Same guard-and-add shape as every other entry -- a component already present is not
						// offered -- and the one-at-a-time rule is deliberate for renderers too, since two
						// renderer components on one entity would both submit geometry for the same
						// transform.
						//
						// The 2D renderers draw as flat quads/circles in the scene's XY plane; the 3D set
						// further down (Plane, Grid, Circle3D, IcoSphere, Cylinder, Cone, Torus) draws as
						// world-space geometry. Mixing both on one entity is possible and produces overlapping
						// submissions -- nothing prevents it, since the guards are per-type rather than
						// per-dimension.
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

				// --- Blender "Add > Mesh" primitives ---
				// Same one-at-a-time guard as the primitives above: adding a
				// second renderer component to one entity is not meaningful.
				if (!m_SelectionContext.HasComponent<PlaneRendererComponent>())
				{
					if (ImGui::MenuItem("Plane Renderer"))
					{
						m_SelectionContext.AddComponent<PlaneRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<GridRendererComponent>())
				{
					if (ImGui::MenuItem("Grid Renderer"))
					{
						// THE PROCEDURAL PRIMITIVES FOLLOW, ADDED AS A GROUP.
						//
						// Plane, Grid, Circle3D, IcoSphere, Cylinder, Cone and Torus are all generated meshes
						// from ProceduralMeshes rather than loaded assets. They share the tessellation and
						// texture-drop conventions documented in the drawable blocks below, and their segment
						// sliders are the ones that disagree with their generators in both directions -- see
						// the icosphere and grid notes there.
						//
						// Note MaterialComponent follows the shape blocks and is a separate concern: it holds
						// no geometry of its own, so an entity may carry one alongside any renderer.
						m_SelectionContext.AddComponent<GridRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<Circle3DRendererComponent>())
				{
					if (ImGui::MenuItem("Circle 3D Renderer"))
					{
						m_SelectionContext.AddComponent<Circle3DRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<IcoSphereRendererComponent>())
				{
					if (ImGui::MenuItem("Ico Sphere Renderer"))
					{
						m_SelectionContext.AddComponent<IcoSphereRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<CylinderRendererComponent>())
				{
					if (ImGui::MenuItem("Cylinder Renderer"))
					{
						m_SelectionContext.AddComponent<CylinderRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<ConeRendererComponent>())
				{
					if (ImGui::MenuItem("Cone Renderer"))
					{
						m_SelectionContext.AddComponent<ConeRendererComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<TorusRendererComponent>())
				{
					if (ImGui::MenuItem("Torus Renderer"))
					{
						m_SelectionContext.AddComponent<TorusRendererComponent>();
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
						// PHYSICS: 2D RIGIDBODY THEN ITS TWO COLLIDERS, THEN THE 3D EQUIVALENTS.
						//
						// A rigidbody and a collider are separate components and can be added independently --
						// there is no check that an entity has a collider before it gets a rigidbody, nor the
						// reverse. An entity with a rigidbody and no collider is valid: it is a body that
						// collides with nothing and is only moved by scripts or transforms.
						//
						// That freedom also means an entity can hold several colliders at once, since each
						// type is guarded separately. OnPhysics2DStart creates one Box2D body per entity and
						// attaches every collider as a fixture on it, so multiple colliders compose into one
						// compound body rather than competing.
						//
						// The 2D and 3D sets are entirely separate components with separate worlds, so an
						// entity may hold both a 2D rigidbody and a 3D one simultaneously.
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

				if (!m_SelectionContext.HasComponent<RigidBody3DComponent>())
				{
					if (ImGui::MenuItem("Rigidbody 3D"))
					{
						m_SelectionContext.AddComponent<RigidBody3DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<BoxCollider3DComponent>())
				{
					if (ImGui::MenuItem("Box Collider 3D"))
					{
						m_SelectionContext.AddComponent<BoxCollider3DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<SphereCollider3DComponent>())
				{
					if (ImGui::MenuItem("Sphere Collider 3D"))
					{
						m_SelectionContext.AddComponent<SphereCollider3DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<CapsuleCollider3DComponent>())
				{
					if (ImGui::MenuItem("Capsule Collider 3D"))
					{
						m_SelectionContext.AddComponent<CapsuleCollider3DComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<VolumetricFogComponent>())
				{
					if (ImGui::MenuItem("Volumetric Fog"))
					{
						m_SelectionContext.AddComponent<VolumetricFogComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<VolumetricCloudsComponent>())
				{
					if (ImGui::MenuItem("Volumetric Clouds"))
					{
						m_SelectionContext.AddComponent<VolumetricCloudsComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<DirectionalLightComponent>())
				{
					if (ImGui::MenuItem("Directional Light"))
					{
						m_SelectionContext.AddComponent<DirectionalLightComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<PointLightComponent>())
				{
					if (ImGui::MenuItem("Point Light"))
					{
						m_SelectionContext.AddComponent<PointLightComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<SpotLightComponent>())
				{
					if (ImGui::MenuItem("Spot Light"))
					{
						m_SelectionContext.AddComponent<SpotLightComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<AreaLightComponent>())
				{
					if (ImGui::MenuItem("Area Light"))
					{
						m_SelectionContext.AddComponent<AreaLightComponent>();
						ImGui::CloseCurrentPopup();
					}
				}

				if (!m_SelectionContext.HasComponent<WaterComponent>())
				{
					if (ImGui::MenuItem("Water (Lake / River / Ocean)"))
					{
						m_SelectionContext.AddComponent<WaterComponent>();
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
			// THE ENTITY HANDLE IS CASTS TO uint64_t AND THEN NARROWED BACK TO uint32_t
			// BEFORE BECOMING AN ImGui ID POINTER.
			//
			// The round trip through uint32_t matches EnTT's underlying handle type. Passing
			// the handle directly as a pointer would give ImGui an ID derived from a different
			// width than the one the rest of the panel uses, so two entities could collide on
			// the same ImGui ID and share state -- a subtle failure where one node's open or
			// closed state appears on another.
			//
			// ImGui only uses this value as an opaque identity key; it is never dereferenced.
			bool opened = ImGui::TreeNodeEx((void *)(uint64_t)(uint32_t)entity, flags, tag.c_str());
			// ANY CLICK SELECTS, WITH NO MODIFIER REQUIRED.
			//
			// So clicking a node's expand arrow also selects it, since IsItemClicked covers
			// the whole tree-node item. That is the conventional behaviour for a hierarchy
			// panel, so it is worth stating rather than assuming.
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
				// THE CHILD NODE USES A HARDCODED CONSTANT ID, 9817239.
				//
				// It labels the child group that would hold nested entities. Children are not
				// implemented -- there is no hierarchy traversal anywhere in this file, and the
				// registry view is flat -- so this exists to reserve and balance the tree node.
				//
				// THE CONSTANT IS THE SAME FOR EVERY ENTITY. Because ImGui keys node state by ID,
				// every parent therefore shares ONE child-node open/closed state: expanding a
				// child under one entity expands it under all of them in the same frame. That is
				// harmless while children are always empty, but it is a live bug the moment
				// anything is drawn inside -- the fix would be to derive the ID from the parent,
				// for example (void *)(uintptr_t)entity + 1.
				//
				// Two TreeNodeEx calls must be balanced by two TreePop calls, which the code does;
				// the inner one is inside `if (opened)` because it is only pushed when drawn.
				bool opened = ImGui::TreeNodeEx((void *)9817239, flags, tag.c_str());
				if (opened)
					ImGui::TreePop();
				ImGui::TreePop();
			}

			if (entityDeleted)
			{
				// DELETION IS DEFERRED UNTIL AFTER THE TREE NODE HAS BEEN POPPED.
				//
				// The entityDeleted flag is captured from the context menu, but DestroyEntity is
				// not called until every TreePop for this node has run. Destroying mid-traversal
				// would invalidate the handle the registry view is currently iterating over, and
				// the node's own ImGui ID would dangle.
				//
				// The selection is cleared afterwards only if it still refers to the deleted
				// entity -- so deleting an unrelated entity does not disturb the current selection.
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

		// THE PER-AXIS RESET BUTTONS ARE COLOUR-CODED RED / GREEN / BLUE, MATCHING THE
		// CONVENTIONAL AXIS PALETTE.
		//
		// Each button sets its own component back to resetValue, so they are quick-reset
		// affordances rather than toggles.
		//
		// NOTE ImGuiCol_Button IS PUSHED TWICE PER AXIS -- once before ButtonHovered and
		// once after -- so three pushes back three pops. The second push is redundant,
		// overwriting the identical colour pushed first. Harmless, and evidently a
		// copy-paste artifact rather than intent, but it means there are three pops for
		// what is really two distinct states.
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

			// THE TAG IS COPIED INTO A FIXED 256-BYTE BUFFER FOR ImGui, then copied back only
			// if the user edited it.
			//
			// ImGui::InputText needs a mutable char buffer, while the component holds a
			// std::string, so a round trip through a local buffer is the standard approach.
			//
			// THE MEMSET IS REDUNDANT. strcpy_s already writes a NUL terminator, and InputText
			// only reads up to that terminator, so pre-zeroing 256 bytes buys nothing. It is
			// harmless but is not the safety measure it looks like.
			//
			// THE REAL LIMIT IS 255 CHARACTERS, and exceeding it is NOT handled. strcpy_s with
			// a destination size invokes the invalid parameter handler on overflow -- which by
			// default ABORTS the process. So a tag longer than 255 characters does not merely
			// truncate, it can take down the editor. A tag that long is unusual but a pasted or
			// generated one is not impossible, and the failure is a crash rather than a clamp.
			//
			// The same pattern is repeated for the script name near the end of this file, with
			// the same 256-byte limit and the same unhandled overflow.
			//
			// A truncating copy, or an explicit length check before copying, would make this a
			// non-issue.
			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			strcpy_s(buffer, sizeof(buffer), tag.c_str());
			// THE COMPONENT IS ONLY WRITTEN BACK WHEN InputText REPORTS A CHANGE.
			//
			// InputText returns true on edit, so the std::string is reassigned only then. That
			// matters because writing unconditionally would mark the tag dirty every frame,
			// defeating change detection downstream.
			//
			// Note the buffer is re-copied from the component on every frame, so an edit is
			// visible immediately in the field and any external change to the tag is reflected
			// here too.
			if (ImGui::InputText("Tag", buffer, sizeof(buffer)))
			{
				tag = std::string(buffer);
			}
		}

		// FLAGS SHARED BY EVERY COMPONENT BLOCK: DEFAULT-OPEN AND ALLOWING ITEM OVERLAP.
		//
		// DefaultOpen means every component section is expanded without the user having to
		// click, which is why the inspector is dense rather than collapsible-by-default.
		//
		// AllowItemOverlap is what lets the "+" button sit on the same line as the tree node
		// label, to the right. Without it ImGui would claim the row for the node and the button
		// would be unreachable.
		const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowItemOverlap;

		if (entity.HasComponent<TransformComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			// THE TREE NODE ID IS typeid(...).hash_code() -- ONE ID PER COMPONENT TYPE, SHARED
			// BY EVERY ENTITY.
			//
			// All 29 component blocks use this pattern. It is SAFE HERE, and worth explaining
			// why, because it looks like the same mistake as the hardcoded 9817239 child-node
			// ID in DrawEntityNode.
			//
			// The difference: DrawComponents is called EXACTLY ONCE per frame, for the current
			// selection only (see the call site in OnImGuiRender). Only one entity's components
			// are ever drawn, so a per-type ID can never collide with another entity's.
			//
			// The hierarchy child node, by contrast, is drawn once for EVERY entity in one
			// pass, so a shared constant there does collide.
			//
			// THE FRAGILITY IS THE SAME IN BOTH CASES. This relies on a single-selection panel
			// with one properties view. A future multi-select inspector, or drawing components
			// inline in the hierarchy, would immediately make these IDs collide -- and the
			// failure would be subtle, since ImGui would simply share open/closed state between
			// entities.
			//
			// Note hash_code() is compiler-specific and not stable across builds, which is fine
			// here because the value only needs to be internally consistent within a single run.
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
				// TRANSFORM IS SPECIAL: ITS REMOVAL IS PERMITTED, WHICH IS UNUSUAL.
				//
				// Most components are removable through the shared "+" popup, but removing a
				// Transform would leave the entity invisible -- the hierarchy view requires BOTH
				// TransformComponent and TagComponent, so the entity would silently vanish from the
				// panel while still existing in the scene.
				//
				// Allowing it anyway offers an action whose result looks like data loss. Refusing to
				// offer removal, or warning first, would be safer.
				//
				// Note the Tag block does NOT offer removal, so the inconsistency between Transform
				// and Tag is not accidental in at least one direction.
				entity.RemoveComponent<TransformComponent>();
		}

		// THE START OF A COMPONENT BLOCK -- AND ALL 29 FOLLOW THIS EXACT SHAPE.
		//
		// The repeated structure, once per component type:
		//    if (entity.HasComponent<T>())           only draw what exists
		//      TreeNodeEx(typeid(T).hash_code(), ..) collapsible section header
		//      "+" button -> OpenPopup("ComponentSettings")
		//      BeginPopup / "Remove Component" -> EndPopup
		//      if (open) { the component's fields }
		//      removeComponent -> entity.RemoveComponent<T>()
		//
		// Duplication across 29 blocks is the obvious thing to fix, and a macro or templated
		// helper would remove most of it. Left as-is deliberately: the blocks differ enough
		// in their field sets and special cases -- camera projection switching, water body
		// types, collider dimension pairs -- that a generic helper would need per-type
		// customization anyway, and the uniform shape is at least easy to read.
		//
		// The popup ID "ComponentSettings" is shared by all 29 blocks, which is safe only
		// because DrawComponents runs once per frame for the selection alone. See the
		// tree-node ID note for why that distinction matters.
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

				// FIRST OF FOUR COMBO BOXES THAT ALL SHARE ONE PATTERN -- this one plus the three
				// "Body Type" combos for the 2D and 3D rigidbodies and for water.
				//
				// IN EVERY ONE OF THEM THE SELECTION TEST COMPARES POINTERS, NOT STRINGS:
				//
				//    bool isSelected = currentProjectionTypeString == projectionTypeStrings[i];
				//
				// This works only because each entry is a string literal -- the compiler pools
				// identical literals so each distinct string has one address -- AND because
				// currentProjectionTypeString is assigned from this same array, guaranteeing the
				// address is one of these. Both conditions hold in all four sites today.
				//
				// It would break silently if the table were built at runtime, for example from a
				// localisation table: the comparison would then be between unrelated pointers and
				// NOTHING would appear selected, with no error. Comparing the INDEX would be
				// correct in every case.
				//
				// Note the three rigidbody/water combos reuse the SAME local variable names
				// (-- currentBodyTypeString, bodyTypeStrings) in three different scopes. That is legal
				// because each is function-local to its own HasComponent block, but it means the same
				// three tables are declared three times with the same shape and only the string
				// contents differ.
				//
				// The loop bound is hardcoded 3 rather than derived from the array size, so adding
				// an enumerator means editing the table, the loop, and ApplyPreset in step.
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

				// PERSPECTIVE AND ORTHOGRAPHIC FIELDS ARE SHOWN IN SEPARATE BRANCHES, AND THE
				// PROJECTION TYPE ITSELF IS NOT SWITCHED HERE.
				//
				// Both are plain `if` rather than if/else, which is harmless because the two types
				// are mutually exclusive -- but it means a third projection type would show neither
				// set of fields with no indication anything is wrong.
				//
				// Each branch re-queries GetProjectionType() rather than reusing a local, so the
				// test reflects the live value even if something else changed it mid-frame.
				//
				// BOTH BRANCHES LABEL A FIELD "Near" AND A FIELD "Far". ImGui identifies widgets by
				// ID, which for these is the label string plus position -- and the two are at
				// different positions in mutually exclusive branches, so they do not collide in
				// practice. They do rely on position, though, not on a unique ID: if these fields
				// were ever reordered within a branch, or the two branches drawn together, the
				// values would bleed into each other.
				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
				{
					// FOV IS ROUND-TRIPPED THROUGH DEGREES FOR DISPLAY, AND BACK TO RADIANS ON WRITE.
					//
					// The camera stores the vertical FOV in radians, but a field labelled "Vertical
					// FOV" that showed 0.0174 would be unusable, so it is converted for the widget and
					// converted back when the user drags it.
					//
					// Note this happens even when the widget is not touched: the conversion is above
					// the DragFloat call, and the write is guarded by its return value. So an unedited
					// field is read, converted and discarded -- no precision loss, but a small
					// redundancy repeated for every camera field.
					//
					// THE CONVERSION IS LOSSY ACROSS THE ROUND TRIP in principle, but since the write
					// only happens on an actual edit, the stored value is untouched otherwise. A build
					// with different degrees/radians rounding would show a tiny drift only for values
					// the user edits, which is acceptable.
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
					// ORTHOGRAPHIC SIZE IS THE HALF-HEIGHT, NOT A FULL EXTENT.
					//
					// The label says "Size" and the default is 10, which reads like a full height but
					// produces a 20-unit tall view. Worth knowing before someone sets a value expecting
					// a specific visible height and gets double it.
					//
					// Every setter here calls RecalculateProjection() internally, so no explicit
					// rebuild is needed after an edit.
					float orthoSize = camera.GetOrthographicSize();
					if (ImGui::DragFloat("Size", &orthoSize))
						camera.SetOrthographicSize(orthoSize);

					float orthoNear = camera.GetOrthographicNearClip();
					if (ImGui::DragFloat("Near", &orthoNear))
						camera.SetOrthographicNearClip(orthoNear);

					float orthoFar = camera.GetOrthographicFarClip();
					if (ImGui::DragFloat("Far", &orthoFar))
						camera.SetOrthographicFarClip(orthoFar);

					// THIS CHECKBOX IS INSIDE THE ORTHOGRAPHIC BRANCH, AND THAT IS CORRECT -- the flag
					// is only ever consumed for orthographic cameras.
					//
					// Scene::OnViewportResize skips SetViewportSize for cameras with this set, and
					// Scene::OnSceneLoad forces a resize of the non-fixed ones so their matrices pick
					// up the new scene bounds.
					//
					// So it means "do not letterbox or stretch this camera when the viewport aspect
					// changes" -- useful for a fixed-ratio isometric or tile view where stretching
					// would distort the art. The flag is also serialized and exposed to Python.
					//
					// IT APPEARS ONLY UNDER ORTHOGRAPHIC because a perspective camera's projection is
					// rebuilt from its FOV on resize regardless, so there is nothing for the flag to
					// suppress there.
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
				// ALL 12 TEXTURE DROP TARGETS SHARE THIS SHAPE, AND THE PAYLOAD TYPE STRING MUST
				// MATCH THE SOURCE EXACTLY.
				//
				// "CONTENT_BROWSER_ITEM" is the contract between the content browser that starts the
				// drag and this panel that accepts it. A mismatch is silent -- AcceptDragDropPayload
				// returns nullptr and the drop is ignored, with no error.
				//
				// The body is identical in all 12 sites: resolve the relative path through
				// ResolveContentBrowserDrop, create the texture, assign it. Only the target
				// component variable differs.
				//
				// NO FILE-TYPE FILTER IS PASSED, so dropping a non-image asset attempts to load it
				// as a texture and the failure surfaces from the loader rather than being prevented
				// here.
				//
				// Texture creation happens on the UI thread inside the render callback, so a large
				// texture load stalls the frame -- worth knowing when a drop appears to freeze the
				// editor briefly.
				auto &src = entity.GetComponent<SpriteRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(src.Color));

				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
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
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						crc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				// THIS FIELD HAS NO CONSUMER ANYWHERE IN THE ENGINE.
				//
				// A repository-wide search for TextureIndex finds only: these 13 panel widgets, the
				// per-component `TextureIndex = 0` initialisers, and two serializer lines that write
				// and read it as YAML. No renderer, no shader, and no scene submission path ever
				// reads it.
				//
				// The component documents the intent clearly -- "Which face of a texture atlas this
				// cube samples. Meaningless when Texture is null or is not an atlas. Present so a
				// whole tileable atlas can be assigned to many cubes with one texture bound." -- so
				// the atlas binding was designed but never implemented.
				//
				// CONSEQUENCE: editing this field in the inspector changes the stored value, which
				// serializes and round-trips through save and load, but has NO effect on what is
				// rendered. Every cube samples the whole texture.
				//
				// Note DragInt here has no min or max, so a large or negative atlas index is
				//accepted -- which would be an out-of-range access if the sampling were ever
				// implemented.
				//
				// This is the first of the 13 identical Texture Index widgets; the same comment
				//covers every other one, including the MaterialComponent's.
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
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
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

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<PlaneRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(PlaneRendererComponent).hash_code(), treeNodeFlags, "Plane Renderer");
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
				auto &prc = entity.GetComponent<PlaneRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(prc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						prc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				// THE PLANE'S 0..16 RANGE IS THE ONE THAT MATCHES ITS GENERATOR -- and 0 is
				// legitimately meaningful here.
				//
				// ProceduralMeshes::Plane clamps to 0..64, so every value the slider can produce is
				// honoured. Unlike the icosphere, where the panel reached 12 levels past the
				// generator's cap of 4, nothing here is silently discarded. The panel's ceiling of
				// 16 is simply more conservative than the 64 the mesh could take.
				//
				// ZERO IS VALID for a plane and uniquely so among these generators. A plane with no
				// subdivisions is a single quad, which is a real mesh; the tube shapes need at least
				// three segments to close at all, which is why their sliders start at 3.
				//
				// The identical 0..16 bounds appear on the icosphere slider a few blocks below, where
				// they are wrong. Same numbers, different generator, different verdict -- worth
				// keeping in mind before treating a matching bound pair as evidence of agreement.
				//
				// The plane also carries a TilingFactor, since a subdivided plane is usually used as
				// a tiled ground or backdrop rather than a single textured quad.
				ImGui::DragInt("Subdivisions", &prc.Subdivisions, 1.0f, 0.0f, 16.0f);
				ImGui::DragInt("Texture Index", &prc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<PlaneRendererComponent>();
		}

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<GridRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(GridRendererComponent).hash_code(), treeNodeFlags, "Grid Renderer");
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
				auto &grc = entity.GetComponent<GridRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(grc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						grc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				// THE PANEL CAPS THIS AT 64 BUT THE GENERATOR ACCEPTS 128, so THE UPPER HALF OF
				// THE GENERATOR'S RANGE IS UNREACHABLE FROM THE UI.
				//
				// ProceduralMeshes::Grid clamps both axes to 1..128 and caches on the pair, so the
				// grid is genuinely anisotropic -- a dense ground plane along one axis only is a
				// supported configuration. The slider just cannot express the top half of it.
				//
				// THIS IS THE OPPOSITE MISMATCH FROM THE ICOSPHERE, where the panel is wider than
				// the consumer. Here the consumer is wider than the panel, so no value the user can
				// set is silently discarded -- the limitation is merely that a useful range is out
				// of reach. Worth noting both directions exist in this file, which suggests the
				// bounds were chosen per-widget rather than derived from the generators.
				//
				// The component defaults to 10 x 10, matching Blender's grid default: 100 cells and
				// 200 triangles.
				//
				// THE FIELD NAMED Y SUBDIVIDES THE Z AXIS. The grid lies flat, so "Y subdivisions"
				// follows Blender's naming for the flat grid's two in-plane axes, not world Y. That
				// is genuinely confusing next to a scene where Y is up, and the component documents
				// it -- but the label in this panel reads "Y Subdivisions" with no hint that it
				// controls Z.
				ImGui::DragInt("X Subdivisions", &grc.XSubdivisions, 1.0f, 0.0f, 64.0f);
				ImGui::DragInt("Y Subdivisions", &grc.YSubdivisions, 1.0f, 0.0f, 64.0f);
				ImGui::DragInt("Texture Index", &grc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<GridRendererComponent>();
		}

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<Circle3DRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(Circle3DRendererComponent).hash_code(), treeNodeFlags, "Circle 3D Renderer");
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
				auto &c3c = entity.GetComponent<Circle3DRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(c3c.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						c3c.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Radius", &c3c.Radius, 0.1f, 0.0f, 100.0f);
				ImGui::DragInt("Segments", &c3c.Segments, 1.0f, 3.0f, 100.0f);
				ImGui::DragInt("Texture Index", &c3c.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<Circle3DRendererComponent>();
		}

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<IcoSphereRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(IcoSphereRendererComponent).hash_code(), treeNodeFlags, "Ico Sphere Renderer");
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
				auto &isc = entity.GetComponent<IcoSphereRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(isc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						isc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Radius", &isc.Radius, 0.1f, 0.0f, 100.0f);
				// THE PANEL ALLOWS 0..16 BUT THE GENERATOR CAPS AT 4 -- EVERYTHING ABOVE 4 IS A
				// NO-OP.
				//
				// ProceduralMeshes::IcoSphere clamps with ClampCount(subdivisions, 0, 4). Triangle
				// count grows as 20 * 4^n, so level 4 is about 5,120 triangles; the cap exists to
				// stay inside the batcher's 100k-vertex budget.
				//
				// The panel should match. As written, dragging Subdivisions past 4 changes the number
				// on screen and the stored component value, but the mesh is identical -- there is no
				// clamp feedback and no visual indication that the extra levels are discarded.
				// Scenes serialized at such a value will reload with the number intact and still
				// render at level 4.
				//
				// The component value also round-trips through YAML, so the mismatch persists across
				// save and load rather than self-correcting.
				//
				// This is the same class of defect as the water Segments slider permitting 128 while
				// the renderer clamps to 64 -- a UI range wider than the consumer's. Note the
				// PLANE renderer's Subdivisions slider below uses the same 0..16 range and its
				// generator really does clamp to 0..64, so that one is fine; the bound was copied
				// across without checking the target.
				ImGui::DragInt("Subdivisions", &isc.Subdivisions, 1.0f, 0.0f, 16.0f);
				ImGui::DragInt("Texture Index", &isc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<IcoSphereRendererComponent>();
		}

		// A FIND-AND-REPLACE MARKER, NOT A COMMENT.
		//
		// These delimit the shape blocks in this file, presumably left behind from a bulk
		// edit when the component blocks were generated. Unlike the rest of the comment
		// block here they use lowercase-after-slash naming and are terse rather than
		// explanatory.
		//
		// Worth deleting before this file is read by anyone new, since a reader could take
		// them for meaningful section headers.
		// ====SHAPE_BLOCK====
		if (entity.HasComponent<CylinderRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CylinderRendererComponent).hash_code(), treeNodeFlags, "Cylinder Renderer");
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
				auto &cyc = entity.GetComponent<CylinderRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(cyc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						cyc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Radius", &cyc.Radius, 0.1f, 0.0f, 100.0f);
				ImGui::DragFloat("Height", &cyc.Height, 0.1f, 0.0f, 100.0f);
				// SEGMENT SLIDERS SHARE A 3..100 RANGE ACROSS CYLINDER, CONE, CIRCLE3D AND TORUS,
				// and the 3 minimum is what the generators require.
				//
				// Unlike the icosphere, these match their generators: Cylinder, Cone and Circle3D all
				// clamp to 3..100, and the torus major/minor pair likewise. Three segments is the
				// minimum that closes a tube.
				//
				// The sphere renderer has no segment control at all -- Renderer3D::DrawSphere builds
				// its mesh at a fixed resolution, so a SphereRendererComponent cannot be tessellated
				// from this panel. IcoSphereRendererComponent is the tessellable alternative, subject
				// to its own 0..4 cap above.
				ImGui::DragInt("Segments", &cyc.Segments, 1.0f, 3.0f, 100.0f);
				ImGui::DragInt("Texture Index", &cyc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CylinderRendererComponent>();
		}

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<ConeRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(ConeRendererComponent).hash_code(), treeNodeFlags, "Cone Renderer");
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
				auto &coc = entity.GetComponent<ConeRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(coc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						coc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Radius", &coc.Radius, 0.1f, 0.0f, 100.0f);
				ImGui::DragFloat("Height", &coc.Height, 0.1f, 0.0f, 100.0f);
				ImGui::DragInt("Segments", &coc.Segments, 1.0f, 3.0f, 100.0f);
				ImGui::DragInt("Texture Index", &coc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<ConeRendererComponent>();
		}

		// ====SHAPE_BLOCK====
		if (entity.HasComponent<TorusRendererComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(TorusRendererComponent).hash_code(), treeNodeFlags, "Torus Renderer");
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
				auto &trc = entity.GetComponent<TorusRendererComponent>();
				ImGui::ColorEdit4("Color", glm::value_ptr(trc.Color));
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						trc.Texture = Texture2D::Create(texturePath.string());
					}
					ImGui::EndDragDropTarget();
				}
				ImGui::DragFloat("Major Radius", &trc.MajorRadius, 0.1f, 0.0f, 100.0f);
				// MINOR RADIUS DOES NOT INDEPENDENTLY CONTROL THE TUBE THICKNESS.
				//
				// It looks fully editable here, and the value round-trips through YAML and the
				// serializer, but the resulting geometry only honours it in one direction.
				//
				// The cached torus mesh is authored once at 0.375 / 0.125 -- a fixed tube/ring ratio
				// of 1/3 -- and DrawTorus rescales it to reach the requested pair. Scaling X/Z by
				// R/0.375 sets the ring radius correctly, but scales the tube's RADIAL half-thickness
				// to R/3 regardless of the requested minorRadius. Only the tube's VERTICAL half-height
				// follows `r`.
				//
				// So a non-default minorRadius produces an ELLIPTICAL tube: the requested height with
				// a radial thickness of R/3. It is a true circular torus only when r == R/3, i.e. at
				// the default ratio. With R = 1.0, requesting r = 0.1 gives a tube 0.1 tall and 0.333
				// thick; requesting 0.5 gives one 0.5 tall and still 0.333 thick.
				//
				// The panel offers no indication of this, and there is no warning. Renderer3D
				// documents the limitation at the scaling site in detail.
				//
				// DrawTorus additionally clamps r >= R down to R * 0.99, so a tube can never be as
				// thick as the ring -- a horn or spindle torus has a degenerate normal field that
				// tessellation cannot fix. That clamp makes the panel's 0..100 ranges safe to reach.
				//
				// Both radii are 0..100 here with no relationship enforced between them, matching the
				// component's own note that nothing guards MinorRadius exceeding MajorRadius.
				ImGui::DragFloat("Minor Radius", &trc.MinorRadius, 0.05f, 0.0f, 100.0f);
				ImGui::DragInt("Major Segments", &trc.MajorSegments, 1.0f, 3.0f, 256.0f);
				ImGui::DragInt("Minor Segments", &trc.MinorSegments, 1.0f, 3.0f, 256.0f);
				ImGui::DragInt("Texture Index", &trc.TextureIndex);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<TorusRendererComponent>();
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
				ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const std::filesystem::path texturePath = ResolveContentBrowserDrop(payload);
						mc.Texture = Texture2D::Create(texturePath.string());
						// Stored project-relative so the scene survives the
						// project directory being moved or the machine changing.
						mc.TexturePath = Project::Get().MakeProjectRelative(texturePath);
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::DragInt("Texture Index", &mc.TextureIndex);

				ImGui::Text("Auto-Generate Maps");

				if (ImGui::Button("Generate Material Maps", ImVec2(-1.0f, 0.0f)))
				{
					if (mc.Texture && !mc.TexturePath.empty())
					{
						// THE FLAG IS SET UNCONDITIONALLY, EVEN WHEN GENERATION DID NOTHING.
						//
						// GenerateMaterialMaps returns void and has several silent no-op paths. Most
						// importantly it returns immediately when no ray-tracing backend is available:
						//     if(!Renderer3DGLAvailable()) return;
						// after the DX11 and DX12 branches have had their chance. In that case nothing is
						// generated, yet HasGeneratedMaps becomes true and both sliders relabel themselves to
						// "(Map Multiplier)" -- implying maps exist that do not.
						//
						// The panel cannot tell the difference because the function reports no success. The
						// button is also silently disabled in effect: the guard requires both a Texture and a
						// non-empty TexturePath, and when either is missing the click does nothing at all
						// without any feedback.
						//
						// ONE MORE SHARED-TARGET CAVEAT: the GL path writes into a single
						// s_Data.GeneratedTextureID image texture, so generating maps for one material
						// overwrites whatever the previous material produced. The D3D paths dispatch through
						// the Nova backend instead.
						//
						// ROUGHNESS IS PASSED AS roughBias, and it doubles as the multiplier for the generated
						// roughness map once HasGeneratedMaps is set -- which is why its label changes.
						//
						// Called on the UI thread from the render callback, so a dispatch here stalls the
						// frame.
						Renderer3D::GenerateMaterialMaps(mc.TexturePath, mc.NormalStrength, mc.Roughness);
						mc.HasGeneratedMaps = true; // Flag that we have maps now!
					}
				}

				ImGui::Text("Material Intensity Settings");

				// THE LABELS DEPEND ON THE FLAG, WHICH IS SET WITHOUT CONFIRMING ANY MAPS EXIST.
				//
				// Both this and the roughness label below are built as std::string and passed to
				// DragFloat, because ImGui takes the label as a C string. The ternary is evaluated
				// every frame from HasGeneratedMaps, so the two fields present themselves differently
				// depending on whether generation is believed to have happened.
				//
				// As noted at the generate button, that belief is not verified -- the renderer returns
				// void and can no-op with no backend, so these can read "(Map Multiplier)" over
				// nothing.
				//
				// One consequence of building the label into a std::string: ImGui derives a widget ID
				// from the label, so the ID CHANGES when the flag flips. The value survives because
				// it is read and written through the address, but any ImGui state tied to the widget
				// -- hover, active -- resets on that frame. Harmless here, worth knowing if these
				// widgets ever gain persistent state.
				std::string normalLabel = mc.HasGeneratedMaps ? "Normal Strength (Map Multiplier)" : "Normal Strength (Base)";
				ImGui::DragFloat(normalLabel.c_str(), &mc.NormalStrength, 0.05f, 0.0f, 5.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Controls bump mapping strength in the ray tracer and\nrelief-normal strength in the raster preview (Basic shader).\nPerturbs surface normals based on texture luminance gradients.\n0 = off in raster; ray tracer uses 1.0 automatic when textured.\nHigher values = stronger surface detail from bump mapping.");

				ImGui::DragFloat("Metallic", &mc.Metallic, 0.05f, 0.0f, 1.0f);

				std::string roughLabel = mc.HasGeneratedMaps ? "Roughness (Map Multiplier)" : "Roughness (Base)";
				ImGui::DragFloat(roughLabel.c_str(), &mc.Roughness, 0.05f, 0.0f, 1.0f);

				ImGui::ColorEdit4("Emission Color", glm::value_ptr(mc.EmissionColor));
				// FLT_MAX AS AN UPPER BOUND MEANS NO UPPER BOUND IN PRACTICE.
				//
				// The field is min-clamped at 0 but has no meaningful ceiling -- the user can type or
				// drag a value large enough that multiplying it by EmissionColor overflows to
				// infinity in a half-float target. The same unbounded pattern is used for the 3D
				// rigidbody's GravityScale, LinearDamping and AngularDamping.
				//
				// Emission is additive rather than a physical unit, so no natural maximum exists --
				// but a practical cap, or a tone-mapping-aware limit, would prevent the blowout.
				//
				// Contrast DisplacementScale immediately below, which is capped 0..2.
				ImGui::DragFloat("Emission Intensity", &mc.EmissionIntensity, 0.05f, 0.0f, FLT_MAX);

				ImGui::DragFloat("Displacement Scale", &mc.DisplacementScale, 0.01f, 0.0f, 2.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Displaces the surface position along the normal based on\ntexture luminance (ray tracer + raster geometric displacement).\nShape-preserving: features rise AND recess around the base\nsurface, which keeps its original shape.\nThis is geometric displacement, separate\nfrom bump mapping (Normal Strength).\n0 = no position displacement.\nRequires a texture to be assigned.");

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
					for (int i = 0; i < 3; i++)
					{
						bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
						if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
						{
							currentBodyTypeString = bodyTypeStrings[i];
							// THE LOOP INDEX IS CAST DIRECTLY TO BodyType, so THE STRING TABLE ORDER MUST
							// MATCH THE ENUMERATOR ORDER.
							//
							// Rigidbody2DComponent::BodyType is { Static = 0, Dynamic, Kinematic } and the
							// table is { "Static", "Dynamic", "Kinematic" }, so index i maps to the right
							// enumerator today.
							//
							// Nothing enforces that correspondence. Reordering the enum -- easy to do without
							// noticing, since C++ permits any order -- would silently make the combo label the
							// wrong physics behaviour: a body labelled "Static" created as Dynamic. No compile
							// error, no assertion.
							//
							// This is the same positional coupling as the volumetric enum in the renderer, and
							// worth remembering whenever either enum gains a member.
							//
							// The 2D and 3D rigidbody blocks each declare their OWN bodyTypeStrings table --
							// same name, same three entries, different scopes -- because the two BodyType enums
							// are separate declarations even though their members match.
							//
							// A switch mapping name to enumerator would decouple the two, at the cost of a
							// little verbosity.
							rigidbody2D.Type = (Rigidbody2DComponent::BodyType)i;
						}

						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				// FIXED ROTATION LOCKS THE BODY'S ANGLE WHILE STILL ALLOWING IT TO MOVE.
				//
				// The standard platformer control: a character that translates but never tips over
				// walking into a slope.
				//
				// It is the only tunable on either rigidbody beyond body type -- no gravity scale,
				// friction, damping or mass controls are exposed here, so those come from the
				// component defaults or must be set through scripting.
				//
				// RigidBody3DComponent has the equivalent checkbox under its own block, so the two
				// dimensions are tuned independently.
				//
				// BodyType defaults to Static, so adding a rigidbody does NOT by itself make an
				// entity move -- the user must switch it to Dynamic.
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

				// THE 2D CIRCLE IS THE ONLY 2D COLLIDER THAT HONOURS ITS Offset.
				//
				// OnPhysics2DStart reads it directly into the Box2D shape:
				//     circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
				// Box2D takes m_p as the centre offset in local body space, so a circle can be
				// pushed off-centre -- useful for a hitbox smaller than its sprite.
				//
				// CONTRAST THE 2D BOX, whose Offset is silently ignored at shape creation and whose
				// Size is half-extents rather than a full width. So within 2D, box and circle
				// disagree about both offset handling and what the size numbers mean.
				//
				// Note this offset is baked in at shape creation and is not scaled by the entity
				// transform, unlike the collider dimensions, which are.
				ImGui::DragFloat2("Offset", glm::value_ptr(circleCollider2D.Offset));
				// THE SLIDER RANGE IS CLAMPED TO 0..1, WHICH IS NARROWER THAN THE TYPE ALLOWS.
				//
				// Radius is stored in local units and scaled at shape creation, so a collider larger
				// than 1 unit requires a scaled entity rather than a larger Radius. That works, but
				// it couples the collider size to the transform scale in a way the panel does not
				// make obvious.
				//
				// THE SCALE HANDLING IS DELIBERATELY INCONSISTENT WITH 3D, and Scene.cpp says so at
				// the creation site: the 2D circle takes a single axis (Scale.x) while
				// SphereCollider3DComponent uses max(x, y, z) and the capsule uses max(x, z). Under
				// non-uniform scale a 2D circle therefore gets an arbitrary radius from one axis.
				//
				// Density, Friction and Restitution are all Box2D fixture parameters and map
				// straight through. RestitutionThreshold is Box2D's collision speed threshold below
				// which restitution is not applied, which is what stops a resting object jittering.
				ImGui::DragFloat("Radius", &circleCollider2D.Radius, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Density", &circleCollider2D.Density, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Friction", &circleCollider2D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &circleCollider2D.Restitution, 0.01f, 0.0f, 1.0f);
				// THIS IS THE ONLY FIELD IN THE BLOCK WITH NO UPPER BOUND.
				//
				// DragFloat is given a step and a min of 0.0f, but no max -- so unlike every
				// neighbouring Radius/Density/Friction/Restitution field, which is clamped 0..1, the
				// threshold has no ceiling.
				//
				// A negative value cannot be entered, which is the important part: Box2D compares
				// the approach speed against this threshold, and a negative one would make
				// restitution apply even to a body at rest -- exactly the jittering this field
				// exists to prevent. The missing max is far less consequential, since a very high
				// threshold just disables bounce entirely.
				//
				// The BoxCollider2D block uses the same unbounded-above form for its own threshold.
				ImGui::DragFloat("Restitution Threshold", &circleCollider2D.RestitutionThreshold, 0.01f, 0.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CircleCollider2DComponent>();
		}

		if (entity.HasComponent<RigidBody3DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(RigidBody3DComponent).hash_code(), treeNodeFlags, "Rigidbody 3D");
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
				auto &rigidbody3D = entity.GetComponent<RigidBody3DComponent>();

				const char *bodyTypeStrings[] = {"Static", "Dynamic", "Kinematic"};
				const char *currentBodyTypeString = bodyTypeStrings[(int)rigidbody3D.Type];
				if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
				{
					for (int i = 0; i < 3; i++)
					{
						bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
						if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
						{
							currentBodyTypeString = bodyTypeStrings[i];
							rigidbody3D.Type = (RigidBody3DType)i;
						}
						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				// THE 3D RIGIDBODY OFFERS MORE CONTROL THAN THE 2D ONE, WHICH HAS ONLY BODY TYPE
				// AND FIXED ROTATION.
				//
				// GravityScale multiplies the world gravity for this body, so 0 floats and 2 falls
				// twice as fast. Linear and Angular Damping are the velocity and spin decay terms.
				//
				// ALL THREE TAKE FLT_MAX AS THE MAXIMUM, i.e. NO CEILING. Damping in particular is
				// usually an exponential decay coefficient where values above a few are already
				// effectively infinite damping; the slider allows the full float range.
				//
				// Note this contradicts the 2D rigidbody block, which exposes neither gravity nor
				// damping at all -- those must come from the Box2D defaults there.
				//
				// Per-axis rotation locks follow: FixedRotationX/Y/Z independently suppress rotation
				// on each axis, which the 2D rigidbody cannot express since it has a single boolean.
				ImGui::DragFloat("Gravity Scale", &rigidbody3D.GravityScale, 0.05f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Linear Damping", &rigidbody3D.LinearDamping, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Angular Damping", &rigidbody3D.AngularDamping, 0.01f, 0.0f, FLT_MAX);
				ImGui::Checkbox("Fixed Rotation X", &rigidbody3D.FixedRotationX);
				ImGui::Checkbox("Fixed Rotation Y", &rigidbody3D.FixedRotationY);
				ImGui::Checkbox("Fixed Rotation Z", &rigidbody3D.FixedRotationZ);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<RigidBody3DComponent>();
		}

		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(BoxCollider3DComponent).hash_code(), treeNodeFlags, "Box Collider 3D");
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
				auto &boxCollider3D = entity.GetComponent<BoxCollider3DComponent>();

				ImGui::DragFloat3("Offset", glm::value_ptr(boxCollider3D.Offset));
				ImGui::DragFloat3("Half Extents", glm::value_ptr(boxCollider3D.HalfExtents));
				ImGui::DragFloat("Density", &boxCollider3D.Density, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Friction", &boxCollider3D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &boxCollider3D.Restitution, 0.01f, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<BoxCollider3DComponent>();
		}

		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(SphereCollider3DComponent).hash_code(), treeNodeFlags, "Sphere Collider 3D");
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
				auto &sphereCollider3D = entity.GetComponent<SphereCollider3DComponent>();

				ImGui::DragFloat3("Offset", glm::value_ptr(sphereCollider3D.Offset));
				ImGui::DragFloat("Radius", &sphereCollider3D.Radius, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Density", &sphereCollider3D.Density, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Friction", &sphereCollider3D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &sphereCollider3D.Restitution, 0.01f, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<SphereCollider3DComponent>();
		}

		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(CapsuleCollider3DComponent).hash_code(), treeNodeFlags, "Capsule Collider 3D");
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
				auto &capsuleCollider3D = entity.GetComponent<CapsuleCollider3DComponent>();

				ImGui::DragFloat3("Offset", glm::value_ptr(capsuleCollider3D.Offset));
				ImGui::DragFloat("Radius", &capsuleCollider3D.Radius, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Height", &capsuleCollider3D.Height, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Density", &capsuleCollider3D.Density, 0.01f, 0.0f, FLT_MAX);
				ImGui::DragFloat("Friction", &capsuleCollider3D.Friction, 0.01f, 0.0f, 1.0f);
				ImGui::DragFloat("Restitution", &capsuleCollider3D.Restitution, 0.01f, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<CapsuleCollider3DComponent>();
		}

		if (entity.HasComponent<VolumetricFogComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(VolumetricFogComponent).hash_code(), treeNodeFlags, "Volumetric Fog");
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
				auto &fog = entity.GetComponent<VolumetricFogComponent>();

				// FOG IS A PER-ENTITY VOLUME WHOSE EXTENT COMES FROM THE TRANSFORM, not a global.
				//
				// The density tooltip says the entity Transform defines the fog box: the component
				// carries no position or size of its own, so moving or scaling the entity moves or
				// resizes the volume. That also means the transform is doing double duty, as it does
				// for lights.
				//
				// The tooltip also notes the transform is TRANSLATION-ONLY for culling purposes, so a
				// non-zero scale still expands the marched region.
				//
				// Renderer3D clamps MaxSteps to 1..32 here, matching the slider exactly, so unlike
				// the icosphere subdivisions there is no unreachable range in this block.
				ImGui::Checkbox("Enabled", &fog.Enabled);
				ImGui::ColorEdit3("Color", glm::value_ptr(fog.Color));
				ImGui::DragFloat("Density", &fog.Density, 0.005f, 0.0f, 2.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Base extinction. The entity Transform defines the fog box:\nTranslation = center, Scale = full box size.");
				// ANISOTROPY IS SIGNED AND SYMMETRIC: positive scatters forward, negative backward.
				//
				// This is the Henyey-Greenstein phase function's g parameter, so 0 is isotropic,
				// +0.9 is strongly forward-scattering (sun visible through the haze around it) and
				// -0.9 is the mirror case.
				//
				// THE BOUNDS STOP JUST SHORT OF 1 DELIBERATELY. At exactly g = 1 the phase function
				// degenerates into a delta -- all scattering collapses onto the exact forward
				// direction -- which is physically a limit rather than a usable value, so the range
				// caps at 0.9. Negative values have the same theoretical bound at -1 for the same
				// reason.
				//
				// This is the only signed slider in the file; everything else is 0..1 or positive.
				ImGui::SliderFloat("Anisotropy", &fog.Anisotropy, -0.9f, 0.9f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Henyey-Greenstein phase: forward (+) vs backward (-) scattering.\nHigher values glow more when looking toward the sun.");
				ImGui::SliderFloat("Height Falloff", &fog.HeightFalloff, 0.0f, 1.0f);
				ImGui::SliderFloat("Noise Strength", &fog.NoiseStrength, 0.0f, 1.0f);
				ImGui::DragFloat("Noise Scale", &fog.NoiseScale, 0.01f, 0.001f, 4.0f);
				ImGui::DragFloat("Wind Speed", &fog.WindSpeed, 0.01f, 0.0f, 2.0f);
				// MARCH STEPS IS THE PER-VOLUME MARCHING QUALITY, AND IT IS NOT THE FINAL COUNT.
				//
				// The renderer uploads MaxSteps * VolStepScale * stepFactor, clamped to 1..32. So
				// raising VolStepScale globally, or the per-frame step factor used for dynamic
				// resolution, can push a volume's effective step count past what this slider set --
				// and the 32 cap then truncates it, discarding the extra steps rather than reducing
				// quality elsewhere.
				//
				// Conversely, a low VolStepScale can reduce a volume below its authored value even
				// though the slider still reads what the user chose.
				//
				// Higher is better quality and linearly more expensive: the raymarch cost is
				// proportional to the step count, so 32 is roughly 32x the work of 1.
				//
				// The volumetric clouds block below uses the same 1..32 range and the same clamp.
				ImGui::DragInt("March Steps", &fog.MaxSteps, 1.0f, 1, 32);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<VolumetricFogComponent>();
		}

		if (entity.HasComponent<VolumetricCloudsComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(VolumetricCloudsComponent).hash_code(), treeNodeFlags, "Volumetric Clouds");
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
				auto &clouds = entity.GetComponent<VolumetricCloudsComponent>();

				ImGui::Checkbox("Enabled", &clouds.Enabled);
				ImGui::ColorEdit3("Color", glm::value_ptr(clouds.Color));
				ImGui::ColorEdit3("Ambient Tint", glm::value_ptr(clouds.AmbientTint));
				ImGui::SliderFloat("Coverage", &clouds.Coverage, 0.0f, 1.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("0 = clear sky, 1 = overcast.\nThe entity Transform defines the cloud layer box:\nTranslation = center, Scale = full box size.");
				ImGui::DragFloat("Density", &clouds.Density, 0.02f, 0.0f, 5.0f);
				ImGui::DragFloat("Noise Scale", &clouds.NoiseScale, 0.005f, 0.001f, 1.0f);
				ImGui::SliderFloat("Detail", &clouds.DetailAmount, 0.0f, 1.0f);
				ImGui::DragFloat2("Wind Direction", glm::value_ptr(clouds.WindDirection), 0.05f);
				ImGui::DragFloat("Wind Speed", &clouds.WindSpeed, 0.005f, 0.0f, 1.0f);
				ImGui::SliderFloat("Silver Lining", &clouds.SilverLining, 0.0f, 1.0f);
				ImGui::SliderFloat("Self Shadow", &clouds.ShadowStrength, 0.0f, 1.0f);
				ImGui::DragInt("March Steps", &clouds.MaxSteps, 1.0f, 1, 32);
				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<VolumetricCloudsComponent>();
		}

		if (entity.HasComponent<DirectionalLightComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(DirectionalLightComponent).hash_code(), treeNodeFlags, "Directional Light");
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
				auto &light = entity.GetComponent<DirectionalLightComponent>();

				ImGui::ColorEdit3("Color", glm::value_ptr(light.Color));
				// A DIRECTIONAL LIGHT HAS ONLY COLOR AND INTENSITY HERE -- no direction field.
				//
				// Its aim comes from the ENTITY'S ROTATION, which is what the tooltip says: the
				// light travels along the entity's local -Z. So the Transform component doubles as
				// light orientation here, and rotating the entity to reorient something else also
				// swings the sun.
				//
				// The tooltip is attached via IsItemHovered on the widget ABOVE it -- a pattern that
				// silently stops matching if a widget is inserted between the two.
				//
				// The 0..20 range suits a sun. The other light types use different ceilings because
				// they fall off with distance: the point light allows 0..50 and the spot light 0..60.
				//
				// There is no shadow toggle or colour-temperature control on any of the four light
				// blocks in this panel.
				ImGui::DragFloat("Intensity", &light.Intensity, 0.05f, 0.0f, 20.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Aim with the entity rotation: light travels along local -Z.");

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<DirectionalLightComponent>();
		}

		if (entity.HasComponent<PointLightComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(PointLightComponent).hash_code(), treeNodeFlags, "Point Light");
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
				auto &light = entity.GetComponent<PointLightComponent>();

				ImGui::ColorEdit3("Color", glm::value_ptr(light.Color));
				// THE POINT LIGHT'S INTENSITY RANGE IS 0..50, larger than the directional's 0..20.
				//
				// Inverse-square attenuation means a point light needs a higher intensity than a
				// directional to read at the same brightness, since there is no distance term for the
				// sun. The spot light's 0..60 continues the pattern, tracking how much of its
				// intensity is concentrated.
				//
				// This anchor is deliberately the point-light intensity line rather than a Range or
				// Falloff line: those are byte-identical across the point, spot and area blocks, so
				// only the intensity ceilings differ between them.
				//
				// The two fields below it -- Range and Falloff -- are documented here because this is
				// the first block that offers them.
				ImGui::DragFloat("Intensity", &light.Intensity, 0.1f, 0.0f, 50.0f);
				ImGui::DragFloat("Range", &light.Range, 0.1f, 0.0f, 100.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("0 = infinite range.");
				ImGui::SliderFloat("Falloff", &light.Falloff, 0.5f, 8.0f);

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<PointLightComponent>();
		}

		if (entity.HasComponent<SpotLightComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(SpotLightComponent).hash_code(), treeNodeFlags, "Spot Light");
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
				auto &light = entity.GetComponent<SpotLightComponent>();

				ImGui::ColorEdit3("Color", glm::value_ptr(light.Color));
				ImGui::DragFloat("Intensity", &light.Intensity, 0.1f, 0.0f, 60.0f);
				ImGui::DragFloat("Range", &light.Range, 0.1f, 0.0f, 100.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("0 = infinite range.");
				ImGui::SliderFloat("Falloff", &light.Falloff, 0.5f, 8.0f);
				// THE INNER AND OUTER ANGLE SLIDERS CAN CROSS, AND THE RENDERER CORRECTS FOR IT.
				//
				// These are independent sliders with independent ranges, so setting Inner to 80 and
				// then Outer to 10 is perfectly reachable through the UI. The panel neither prevents
				// nor corrects it.
				//
				// IT IS HANDLED DOWNSTREAM. Renderer3D forces the outer angle open when it would sit
				// inside the inner one:
				//     if (outer < inner) outer = inner;
				// Without it the cone would invert and the spot would be brightest at the rim of its
				// own beam. The two clamps make the correction always satisfiable -- the outer may
				// reach 90 while the inner stops at 89.5.
				//
				// The 89.5 / 0.5 bounds are identical in the slider and the renderer clamp, so the
				// UI cannot produce a value the renderer then rejects.
				//
				// Defaults are Inner 25 / Outer 40, a 15-degree penumbra between them.
				ImGui::SliderFloat("Inner Angle", &light.InnerAngle, 0.0f, 89.5f);
				ImGui::SliderFloat("Outer Angle", &light.OuterAngle, 0.5f, 90.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Aim with the entity rotation: cone opens along local -Z.");

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<SpotLightComponent>();
		}

		if (entity.HasComponent<AreaLightComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(AreaLightComponent).hash_code(), treeNodeFlags, "Area Light");
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
				auto &light = entity.GetComponent<AreaLightComponent>();

				ImGui::ColorEdit3("Color", glm::value_ptr(light.Color));
				ImGui::DragFloat("Intensity", &light.Intensity, 0.1f, 0.0f, 50.0f);
				ImGui::DragFloat("Width", &light.Width, 0.05f, 0.01f, 50.0f);
				ImGui::DragFloat("Height", &light.Height, 0.05f, 0.01f, 50.0f);
				// RANGE 0 MEANS NO LIMIT, which the tooltip directly below states.
				//
				// An attenuation calculation normally divides by the range, so zero has to be
				// checked before that division. A light left at 0 therefore costs nothing extra to
				// evaluate, unlike a very large one.
				//
				// This same line appears in the point, spot and area light blocks with identical
				// bounds and the same zero-means-infinite convention -- the fields are uniform across
				// the three, which is why only the point light's copy is annotated. The spot and
				// area blocks add no further range documentation of their own.
				//
				// Falloff, immediately below, is byte-identical across the same three blocks.
				ImGui::DragFloat("Range", &light.Range, 0.1f, 0.0f, 100.0f);
				ImGui::Checkbox("Double Sided", &light.DoubleSided);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Rect lies in the local XY plane and emits along local -Z.\nSingle-sample approximation (no soft area shadows).");

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<AreaLightComponent>();
		}

		if (entity.HasComponent<WaterComponent>())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{4, 4});
			bool open = ImGui::TreeNodeEx((void *)typeid(WaterComponent).hash_code(), treeNodeFlags, "Water");
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
				auto &water = entity.GetComponent<WaterComponent>();

				ImGui::Checkbox("Enabled", &water.Enabled);

				// THE COMBO LABEL TABLE, AND THE SELECTION TEST BELOW COMPARES POINTERS, NOT
				// STRINGS.
				//
				// `currentBodyTypeString == bodyTypeStrings[i]` is a pointer comparison. It works
				// ONLY because every entry is a string literal, so the compiler pools identical
				// literals and each distinct literal has one address -- and because currentBodyType
				// String is assigned FROM this same array, so the addresses are guaranteed to be
				// one of these three.
				//
				// If the table were ever built at runtime -- say from a localised string table --
				// this test would become a genuine bug, comparing unrelated pointers and leaving
				// nothing selected. The correct test is to compare the INDEX, not the string.
				//
				// Three entries, matching the three WaterBodyType enumerators. The loop bound is a
				// hardcoded 3 rather than a count of the array, so adding an enumerator requires
				// editing the table, the loop and ApplyPreset together.
				const char *bodyTypeStrings[] = {"Lake", "River", "Ocean"};
				const char *currentBodyTypeString = bodyTypeStrings[(int)water.Type];
				if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
				{
					for (int i = 0; i < 3; i++)
					{
						bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
						if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
						{
							// CHOOSING A TYPE OVERWRITES TWELVE AUTHORED FIELDS, SILENTLY AND WITHOUT UNDO.
							//
							// ApplyPreset assigns Type and then a table of values: both colours, WaterDepth,
							// the four wave parameters, Chop, Steepness, flow direction and speed, FoamAmount,
							// ShoreFoam, ShoreWidth, Transparency and Roughness. Any of those the user had
							// tuned by hand are lost the moment they pick a different body type -- there is no
							// confirmation, no undo, and no record of the previous values.
							//
							// THE COMPONENT ITSELF DOCUMENTS THIS: "ApplyPreset overwrites the authored fields
							// from a table, so the type and those fields can disagree if either is edited
							// afterwards." So the divergence between Type and the fields is a known, accepted
							// state rather than an oversight -- but it means the combo is better understood as
							// "load these values" than as "change the behaviour".
							//
							// The switch inside ApplyPreset has no default case. It is exhaustive over the three
							// enumerators today; adding a fourth without a case leaves that preset entirely
							// untouched, with no compiler warning.
							WaterComponent::ApplyPreset(water, (WaterBodyType)i);
							if (entity.HasComponent<TransformComponent>())
							{
								auto &tc = entity.GetComponent<TransformComponent>();
								// THE SURFACE IS GROWN ONLY IF IT IS STILL AT (OR BELOW) DEFAULT SIZE.
								//
								// A water plane is authored as a 1x1 XZ quad scaled by Transform -- so an ocean at
								// scale 60 is a 60-unit surface. Without this growth a default-size plane would show
								// an ocean preset on a one-unit puddle.
								//
								// THE GUARD IS `< 1.0f`, NOT `== 1.0f`, and the two behave very differently.
								// With `== 1.0f` an exact float comparison, re-picking a preset on an already-scaled
								// ocean would do nothing -- correct, but brittle. With `< 1.0f` the growth is
								// irreversible: once a plane is scaled to 60, re-selecting any preset will NEVER
								// shrink it, because 60 is not less than 1.0.
								//
								// So this is one-way. There is no path in the UI back to a smaller surface short of
								// editing the Transform scale by hand. For a user who mis-clicks Ocean on a small
								// pond, the pond is now 60x60 permanently.
								//
								// The OR is deliberate in one respect -- it triggers if EITHER axis is small, so a
								// 60 x 0.5 plane still gets widened rather than being left as a ribbon.
								if (tc.Scale.x < 1.0f || tc.Scale.z < 1.0f)
								{
									// Oceans/rivers usually span large areas; grow a
									// default-size plane so the preset is visible.
									if ((WaterBodyType)i == WaterBodyType::Ocean)
									{
										// PER-PRESET TARGET EXTENTS: OCEAN 60x60, RIVER 30x6, LAKE 10x10.
										//
										// The river is the only anisotropic one -- 30 along flow by 6 across -- which matches
										// its directional nature and is why the guard ORs the two axes rather than treating
										// them as one value.
										//
										// glm::max rather than assignment, so a plane already larger than the target keeps
										// its size. That is what makes this safe to run against an existing body, and it is
										// also why nothing can be shrunk -- see the guard note above.
										//
										// These are scale factors, not world units directly: the plane's local footprint is
										// 1x1, so scale IS the extent in both axes.
										tc.Scale.x = glm::max(tc.Scale.x, 60.0f);
										tc.Scale.z = glm::max(tc.Scale.z, 60.0f);
									}
									else if ((WaterBodyType)i == WaterBodyType::River)
									{
										tc.Scale.x = glm::max(tc.Scale.x, 30.0f);
										tc.Scale.z = glm::max(tc.Scale.z, 6.0f);
									}
									else
									{
										tc.Scale.x = glm::max(tc.Scale.x, 10.0f);
										tc.Scale.z = glm::max(tc.Scale.z, 10.0f);
									}
								}
							}
						}

						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Switching type applies that body's preset values.\nThe entity Transform defines the surface:\nTranslation.y = still water level, Scale.x/z = extents.");

				if (ImGui::CollapsingHeader("Appearance"))
				{
					ImGui::ColorEdit3("Shallow Color", glm::value_ptr(water.ShallowColor));
					ImGui::ColorEdit3("Deep Color", glm::value_ptr(water.DeepColor));
					ImGui::ColorEdit3("Foam Color", glm::value_ptr(water.FoamColor));
					ImGui::DragFloat("Water Depth", &water.WaterDepth, 0.1f, 0.1f, 50.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Absorption depth for the shallow/deep gradient.\nDeeper water tints toward Deep Color.");
					ImGui::SliderFloat("Transparency", &water.Transparency, 0.0f, 1.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Opaque pipeline: tints the raster albedo toward\nshallow (true alpha blending is not supported yet).");
					ImGui::SliderFloat("Roughness", &water.Roughness, 0.0f, 1.0f);
					ImGui::SliderFloat("Metallic", &water.Metallic, 0.0f, 1.0f);
				}

				if (ImGui::CollapsingHeader("Waves"))
				{
					ImGui::DragFloat("Amplitude", &water.WaveAmplitude, 0.01f, 0.0f, 5.0f);
					ImGui::DragFloat("Length", &water.WaveLength, 0.1f, 0.1f, 100.0f);
					ImGui::DragFloat("Speed", &water.WaveSpeed, 0.05f, 0.0f, 10.0f);
					ImGui::DragFloat2("Direction", glm::value_ptr(water.WaveDirection), 0.05f);
					ImGui::SliderFloat("Chop", &water.Chop, 0.0f, 1.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Secondary detail waves. 0 = glassy single swell,\n1 = fully choppy. Same field drives gameplay\nheight queries and the rendered mesh.");
					ImGui::SliderFloat("Steepness", &water.Steepness, 0.0f, 1.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Gerstner horizontal chop: bunches vertices toward\ncrests for sharp peaked waves (0 = sine swell).");
				}

				if (ImGui::CollapsingHeader("Flow & Foam"))
				{
					ImGui::DragFloat2("Flow Direction", glm::value_ptr(water.FlowDirection), 0.05f);
					ImGui::DragFloat("Flow Speed", &water.FlowSpeed, 0.05f, 0.0f, 20.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("River current: advects the wave field and is\nreported by GetWaterHeightAt for gameplay drift.");
					ImGui::SliderFloat("Foam Amount", &water.FoamAmount, 0.0f, 1.0f);
					ImGui::DragFloat("Foam Scale", &water.FoamScale, 0.05f, 0.01f, 10.0f);
					ImGui::SliderFloat("Shore Foam", &water.ShoreFoam, 0.0f, 1.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Foamline along the footprint edges (banks/shore).\nAgitated by flow and surf; rivers foam at banks,\nlake edges lap gently.");
					ImGui::DragFloat("Shore Width", &water.ShoreWidth, 0.05f, 0.05f, 10.0f);
				}

				if (ImGui::CollapsingHeader("Simulation & Mesh"))
				{
					ImGui::DragInt("Segments", &water.Segments, 1.0f, 1, 128);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Surface grid resolution per side.\nClamped to 64 in the renderer for batch/CPU cost.");
					ImGui::DragFloat("Time Scale", &water.TimeScale, 0.05f, 0.0f, 10.0f);
					ImGui::DragFloat("Buoyancy", &water.Buoyancy, 0.05f, 0.0f, 10.0f);
					ImGui::DragFloat("Density", &water.WaterDensity, 0.05f, 0.0f, 10.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Buoyancy/Density are gameplay scales read by\nscripts via GetWaterHeightAt / IsUnderwater.");
					ImGui::Text("Sim Time: %.2f s", (double)water.Time);
				}

				ImGui::TreePop();
			}

			if (removeComponent)
				entity.RemoveComponent<WaterComponent>();
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
				// THE SECOND 256-BYTE BUFFER IN THIS FILE, with the same redundant memset and the
				// same unhandled overflow.
				//
				// ScriptName is round-tripped through a local char array because ImGui::InputText
				// needs mutable storage while the component holds a std::string.
				//
				// THE LIMIT IS 255 CHARACTERS AND EXCEEDING IT IS NOT HANDLED. strcpy_s with a
				// destination size invokes the invalid parameter handler, which by default ABORTS
				// the process -- so typing or pasting a name over 255 characters can take down the
				// editor rather than failing gracefully.
				//
				// A class name that long is unrealistic, so this is low severity in practice; but
				// it is a crash rather than a clamp, and the tag buffer earlier in this file has
				// the identical issue with a much more plausible trigger (a pasted tag).
				//
				// The memset is unnecessary -- strcpy_s already terminates, and InputText reads only
				// to the terminator.
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
						const std::filesystem::path scriptPath = ResolveContentBrowserDrop(payload);

						// THE CLASS NAME IS DERIVED FROM THE FILE STEM, so the file name and the Python
						// class name MUST MATCH EXACTLY.
						//
						// ScriptEngine looks the class up by name inside the imported module:
						//     scriptModule.attr(sc.ScriptName.c_str())
						// So dropping assets/enemy.py sets ScriptName to "enemy" and the engine will look
						// for a class called `enemy`. If the file declares `class Enemy:` -- the normal
						// Python convention -- the attribute lookup fails and construction is caught and
						// logged rather than crashing.
						//
						// This is the panel's opinion about the script's contents, and it is worth stating
						// because nothing validates it: the dropdown looks correct while the script silently
						// refuses to instantiate.
						//
						// Both fields are then set -- the class name here, and the module path below -- and
						// they are used independently from that point on.
						//
						// Note the stem drops the .py extension, which is correct for the module name, and
						// also drops any directory information, which is why ScriptPath is needed
						// separately.
						scriptComponent.ScriptName = scriptPath.stem().string();

						// Stored relative to the asset root, which is the form
						// ScriptEngine::GetAssetPath and the scene serializer
						// both expect.
						std::string pathStr = Project::Get().MakeProjectRelative(scriptPath);
						const std::string prefix = "assets/";
						// THE "assets/" PREFIX IS STRIPPED AFTER MAKING THE PATH PROJECT-RELATIVE, because
						// ScriptEngine::GetAssetPath and the serializer both expect that form.
						//
						// compare(0, len, prefix) == 0 is an exact prefix test -- equivalent to
						// pathStr.rfind(prefix, 0) == 0, and clearer about intent. Note it compares a
						// fixed-length span, so it cannot match a shorter path.
						//
						// THE STRIPPING IS CONDITIONAL, and silently skipped when the prefix is absent. That
						// is the safe behaviour for an asset already stored relative to the assets root,
						// but it means a script dropped from OUTSIDE the assets directory keeps its full
						// project-relative path -- and would then fail to resolve at load time, with no
						// feedback here.
						if (pathStr.compare(0, prefix.size(), prefix) == 0)
							pathStr = pathStr.substr(prefix.size());

						scriptComponent.ScriptPath = pathStr;
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