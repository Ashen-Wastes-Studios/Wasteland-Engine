#include "wlpch.h"
#include "Entity.h"

namespace Wasteland {

	Entity::Entity(entt::entity handle, Scene* scene)
		: m_EntityHandle(handle), m_Scene(scene)
	{

	}


	bool Entity::GetWaterHeightAt(const glm::vec3 &worldPos, float &outHeight, glm::vec2 *outFlow)
	{
		if (!m_Scene)
			return false;
		return m_Scene->GetWaterHeightAt(worldPos, outHeight, outFlow);
	}


	bool Entity::IsUnderwater(const glm::vec3 &worldPos, float margin)
	{
		if (!m_Scene)
			return false;
		return m_Scene->IsUnderwater(worldPos, margin);
	}

}