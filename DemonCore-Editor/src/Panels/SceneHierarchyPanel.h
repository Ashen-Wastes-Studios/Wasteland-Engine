#pragma once

#include "Wasteland/Core/Core.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Scene/Scene.h"
#include "Wasteland/Scene/Entity.h"

namespace Wasteland {

	class SceneHierarchyPanel
	{
	public:
		SceneHierarchyPanel() = default;
		SceneHierarchyPanel(const Ref<Scene>& context);

		void SetContext(const Ref<Scene>& context);

		void OnImGuiRender();

		bool IsHierarchyVisible() const { return m_HierarchyVisible; }
		void SetHierarchyVisible(bool visible) { m_HierarchyVisible = visible; }
		bool IsPropertiesVisible() const { return m_PropertiesVisible; }
		void SetPropertiesVisible(bool visible) { m_PropertiesVisible = visible; }

		Entity GetSelectedEntity() const { return m_SelectionContext; }
		void SetSelectedEntity(Entity entity);
	private:
		void DrawEntityNode(Entity entity);
		void DrawComponents(Entity entity);
	private:
		Ref<Scene> m_Context;
		Entity m_SelectionContext;
		bool m_HierarchyVisible = true;
		bool m_PropertiesVisible = true;
	};

}