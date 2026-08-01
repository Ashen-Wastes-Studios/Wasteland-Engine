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

	private:
		Entity m_Entity;
	};

}
