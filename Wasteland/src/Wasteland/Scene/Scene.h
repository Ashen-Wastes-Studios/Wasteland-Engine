#pragma once

#include <entt/entt.hpp>

#include "Wasteland/Core/Timestep.h"
#include "Wasteland/Renderer/EditorCamera.h"
#include "Wasteland/Core/UUID.h"

class b2World;

namespace Wasteland
{

	class Entity;

	class Scene
	{
	public:
		Scene();
		~Scene();

		static Ref<Scene> Copy(Ref<Scene> other);

		Entity CreateEntity(const std::string &name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string &name = std::string());
		void DestroyEntity(Entity entity);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();
		void OnUpdateSimulation(Timestep ts, EditorCamera &camera);

		void OnUpdateEditor(Timestep ts, EditorCamera &camera);
		void OnUpdateRuntime(Timestep ts);
		void OnViewportResize(uint32_t width, uint32_t height);

		void DuplicateEntity(Entity entity);

		Entity GetPrimaryCameraEntity();
		bool IsEntityValid(Entity entity) const;

		template <typename... T>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<T...>();
		}

	private:
		template <typename T>
		void OnComponentAdded(Entity entity, T &component);

		void OnPhysics2DStart();
		void OnPhysics2DStop();

		void RenderScene(EditorCamera &camera);

	private:
		entt::registry m_Registry;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		b2World *m_PhysicsWorld = nullptr;

		friend class Entity;
		friend class SceneSerializer;
		friend class SceneHierarchyPanel;
	};

}