#pragma once

#include "Wasteland/Scene/Entity.h"

namespace Wasteland
{


	class ScriptInspectorPanel
	{

	public:
		ScriptInspectorPanel() = default;


		void SetEntity(Entity entity) { m_Entity = entity; }
		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		Entity m_Entity;
		bool m_Visible = true;
	};

}
