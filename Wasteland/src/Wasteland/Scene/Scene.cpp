#include "wlpch.h"
#include "Scene.h"

#include "Components.h"
#include "Wasteland/Renderer/Renderer2D.h"
#include "Wasteland/Renderer/Renderer3D.h"

#include <glm/glm.hpp>

#include "Entity.h"
#include "ScriptableEntity.h"
#include "Wasteland/Scripting/ScriptEngine.h"

// Box2D
#include "box2d/b2_world.h"
#include "box2d/b2_body.h"
#include "box2d/b2_fixture.h"
#include "box2d/b2_polygon_shape.h"
#include "box2d/b2_circle_shape.h"

// Box3D
#include <box3d/box3d.h>
#include <box3d/collision.h>

namespace Wasteland
{

	static b2BodyType Rigidbody2DTypeToBox2DBody(Rigidbody2DComponent::BodyType bodyType)
	{
		switch (bodyType)
		{
		case Rigidbody2DComponent::BodyType::Static:
			return b2_staticBody;
		case Rigidbody2DComponent::BodyType::Dynamic:
			return b2_dynamicBody;
		case Rigidbody2DComponent::BodyType::Kinematic:
			return b2_kinematicBody;
		}

		WL_CORE_ASSERT(false, "Unknown body type");
		return b2_staticBody;
	}

	static b3BodyType RigidBody3DTypeToBox3D(RigidBody3DType type)
	{
		switch (type)
		{
		case RigidBody3DType::Static:
			return b3_staticBody;
		case RigidBody3DType::Dynamic:
			return b3_dynamicBody;
		case RigidBody3DType::Kinematic:
			return b3_kinematicBody;
		}
		return b3_staticBody;
	}

	static b3Quat EulerToB3Quat(const glm::vec3 &eulerRadians)
	{
		glm::quat q(eulerRadians);
		b3Quat result;
		result.v = {q.x, q.y, q.z};
		result.s = q.w;
		return result;
	}

	static glm::vec3 B3QuatToEuler(const b3Quat &q)
	{
		glm::quat gq(q.s, q.v.x, q.v.y, q.v.z);
		return glm::eulerAngles(gq);
	}

	Scene::Scene()
	{
	}

	Scene::~Scene()
	{
	}

	template <typename T>
	static void CopyComponent(entt::registry &dst, entt::registry &src, const std::unordered_map<UUID, entt::entity> &enttMap)
	{
		auto view = src.view<T>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			UUID uuid = src.get<IDComponent>(e).ID;
			auto it = enttMap.find(uuid);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &component = src.get<T>(e);

			dst.emplace_or_replace<T>(dstEnttID, component);
		}
	}

	template <>
	static void CopyComponent<NativeScriptComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<UUID, entt::entity> &enttMap)
	{
		auto view = src.view<NativeScriptComponent>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			UUID uuid = src.get<IDComponent>(e).ID;
			auto it = enttMap.find(uuid);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<NativeScriptComponent>(e);

			// Emplace a clean script component container
			auto &dstComponent = dst.emplace_or_replace<NativeScriptComponent>(dstEnttID);

			// Explicitly copy only the function pointers, keeping Instance safely null
			dstComponent.InstantiateScript = srcComponent.InstantiateScript;
			dstComponent.DestroyScript = srcComponent.DestroyScript;
			dstComponent.Instance = nullptr;
		}
	}

	template <>
	static void CopyComponent<ScriptComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<UUID, entt::entity> &enttMap)
	{
		auto scriptView = src.view<ScriptComponent>();
		for (auto e : scriptView)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			UUID uuid = src.get<IDComponent>(e).ID;
			auto it = enttMap.find(uuid);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<ScriptComponent>(e);

			WL_CORE_INFO("Copying Script: '{0}' from {1}", srcComponent.ScriptPath, srcComponent.ScriptName);

			// Emplace a clean script component container in the destination scene
			auto &dstComponent = dst.emplace_or_replace<ScriptComponent>(dstEnttID);

			// Explicitly copy the data members
			dstComponent.ScriptPath = srcComponent.ScriptPath;
			dstComponent.ScriptName = srcComponent.ScriptName;
		}
	}

	template <>
	static void CopyComponent<CameraComponent>(entt::registry &dst, entt::registry &src, const std::unordered_map<UUID, entt::entity> &enttMap)
	{
		auto view = src.view<CameraComponent>();
		for (auto e : view)
		{
			if (!src.valid(e) || !src.all_of<IDComponent>(e))
				continue;

			UUID uuid = src.get<IDComponent>(e).ID;
			auto it = enttMap.find(uuid);
			if (it == enttMap.end())
				continue;

			entt::entity dstEnttID = it->second;
			auto &srcComponent = src.get<CameraComponent>(e);

			// Force create a completely fresh, brand new camera component container
			auto &dstComponent = dst.emplace_or_replace<CameraComponent>(dstEnttID);

			// Manually copy individual properties to trigger any internal setter functions properly
			dstComponent.Primary = srcComponent.Primary;
			dstComponent.FixedAspectRatio = srcComponent.FixedAspectRatio;

			// Copy standard camera configuration details (adjust these names if your class names vary)
			dstComponent.Camera = srcComponent.Camera;
		}
	}

	template <typename T>
	static void CopyComponentIfExists(Entity dst, Entity src)
	{
		if (src.HasComponent<T>())
			dst.AddOrReplaceComponent<T>(src.GetComponent<T>());
	}

	Ref<Scene> Scene::Copy(Ref<Scene> other)
	{
		Ref<Scene> newScene = CreateRef<Scene>();

		newScene->m_ViewportWidth = other->m_ViewportWidth;
		newScene->m_ViewportHeight = other->m_ViewportHeight;

		auto &srcSceneRegistry = other->m_Registry;
		auto &dstSceneRegistry = newScene->m_Registry;
		std::unordered_map<UUID, entt::entity> enttMap;

		// Create entities in new scene
		auto idView = srcSceneRegistry.view<IDComponent>();
		for (auto e : idView)
		{
			UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
			const auto &name = srcSceneRegistry.get<TagComponent>(e).Tag;
			Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
			enttMap[uuid] = (entt::entity)newEntity;
		}

		// Copy components (except IDComponent and TagComponent)
		CopyComponent<TransformComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SpriteRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CircleRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CubeRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<SphereRendererComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<MaterialComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CameraComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<Rigidbody2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<BoxCollider2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<CircleCollider2DComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);

		CopyComponent<NativeScriptComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);
		CopyComponent<ScriptComponent>(dstSceneRegistry, srcSceneRegistry, enttMap);

		{
			auto view = dstSceneRegistry.view<CameraComponent>();
			for (auto entity : view)
			{
				auto &cameraComponent = view.get<CameraComponent>(entity);
				if (!cameraComponent.FixedAspectRatio)
				{
					// Set it to an alternate temporary size first to clear dirty flags
					cameraComponent.Camera.SetViewportSize(100, 100);
					// Restore its actual matrix bounds
					cameraComponent.Camera.SetViewportSize(newScene->m_ViewportWidth, newScene->m_ViewportHeight);
				}
			}
		}

		return newScene;
	}

	Entity Scene::CreateEntity(const std::string &name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string &name)
	{
		Entity entity = {m_Registry.create(), this};
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<TransformComponent>();
		auto &tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		m_Registry.destroy(entity);
	}

	void Scene::OnRuntimeStart()
	{
		OnPhysics2DStart();
		OnPhysics3DStart();
	}

	void Scene::OnRuntimeStop()
	{
		OnPhysics2DStop();
		OnPhysics3DStop();
	}

	void Scene::OnUpdateRuntime(Timestep ts)
	{
		// Update scripts
		{
			m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto &nsc)
														  {
					// TODO: Move to Scene::OnScenePlay
					if (!nsc.Instance)
					{
						nsc.Instance = nsc.InstantiateScript();
						nsc.Instance->m_Entity = Entity{ entity, this };
						nsc.Instance->OnCreate();
					}

					nsc.Instance->OnUpdate(ts); });

			auto view = m_Registry.view<ScriptComponent>();
			for (auto entity : view)
			{
				Entity e = {entity, this};

				ScriptEngine::OnUpdateEntity(e, ts);
			}
		}

		// Physics
		{
			const int32_t velocityIterations = 6;
			const int32_t positionIterations = 2;
			m_PhysicsWorld->Step(ts, velocityIterations, positionIterations);

			// Retrieve transform from Box2D
			auto view = m_Registry.view<Rigidbody2DComponent>();
			for (auto e : view)
			{
				Entity entity = {e, this};
				auto &transform = entity.GetComponent<TransformComponent>();
				auto &rb2d = entity.GetComponent<Rigidbody2DComponent>();

				b2Body *body = (b2Body *)rb2d.RuntimeBody;
				const auto &position = body->GetPosition();
				transform.Translation.x = position.x;
				transform.Translation.y = position.y;
				transform.Rotation.z = body->GetAngle();
			}
		}

		// 3D Physics (Box3D)
		if (b3World_IsValid(m_Physics3DWorld))
		{
			b3World_Step(m_Physics3DWorld, ts, 4);

			auto view3d = m_Registry.view<RigidBody3DComponent>();
			for (auto e : view3d)
			{
				Entity entity = {e, this};
				auto &transform = entity.GetComponent<TransformComponent>();
				auto &rb3d = entity.GetComponent<RigidBody3DComponent>();

				b3BodyId bodyId;
				std::memcpy(&bodyId, &rb3d.RuntimeBody, sizeof(b3BodyId));

				if (!b3Body_IsValid(bodyId))
					continue;

				b3Pos pos = b3Body_GetPosition(bodyId);
				b3Quat rot = b3Body_GetRotation(bodyId);

				transform.Translation.x = (float)pos.x;
				transform.Translation.y = (float)pos.y;
				transform.Translation.z = (float)pos.z;
				transform.Rotation = B3QuatToEuler(rot);
			}
		}

		// Render 2D
		Camera *mainCamera = nullptr;
		glm::mat4 cameraTransform;
		{
			auto view = m_Registry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = transform.GetTransform();
					break;
				}
			}
		}

		if (mainCamera)
		{
			Renderer2D::BeginScene(mainCamera->GetProjection(), cameraTransform);

			// Draw sprites
			{
				auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
				for (auto entity : group)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

					Renderer2D::DrawSprite(transform.GetTransform(), sprite, (int)entity);
				}
			}

			// Draw circles
			{
				auto view = m_Registry.view<TransformComponent, CircleRendererComponent>();
				for (auto entity : view)
				{
					auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

					Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, (int)entity);
				}
			}

			Renderer2D::EndScene();

			// Render 3D
			Renderer3D::BeginScene(mainCamera->GetProjection(), cameraTransform);

			// Draw Cubes
			{
				auto cubeView = m_Registry.view<TransformComponent, CubeRendererComponent, MaterialComponent>();
				for (auto entity : cubeView)
				{
					auto &tc = cubeView.get<TransformComponent>(entity);
					auto &crc = cubeView.get<CubeRendererComponent>(entity);
					auto &mc = cubeView.get<MaterialComponent>(entity);

					// Pass the 3D entity data to the new batch renderer
					Renderer3D::DrawCube(tc.GetTransform(), crc.Color, mc, (int)entity);
				}
			}

			// Draw Spheres
			{
				auto sphereView = m_Registry.view<TransformComponent, SphereRendererComponent, MaterialComponent>();
				for (auto entity : sphereView)
				{
					auto &tc = sphereView.get<TransformComponent>(entity);
					auto &src = sphereView.get<SphereRendererComponent>(entity);
					auto &mc = sphereView.get<MaterialComponent>(entity);

					Renderer3D::DrawSphere(
						tc.GetTransform(),
						src.Color,
						src.Radius,
						src.Sectors,
						src.Stacks,
						mc,
						(int)entity);
				}
			}

			Renderer3D::EndScene();
		}
	}

	void Scene::OnSimulationStart()
	{
		OnPhysics2DStart();
		OnPhysics3DStart();
	}

	void Scene::OnSimulationStop()
	{
		OnPhysics2DStop();
		OnPhysics3DStop();
	}

	void Scene::OnUpdateSimulation(Timestep ts, EditorCamera &camera)
	{
		// Physics
		{
			const int32_t velocityIterations = 6;
			const int32_t positionIterations = 2;
			m_PhysicsWorld->Step(ts, velocityIterations, positionIterations);

			// Retrieve transform from Box2D
			auto view = m_Registry.view<Rigidbody2DComponent>();
			for (auto e : view)
			{
				Entity entity = {e, this};
				auto &transform = entity.GetComponent<TransformComponent>();
				auto &rb2d = entity.GetComponent<Rigidbody2DComponent>();

				b2Body *body = (b2Body *)rb2d.RuntimeBody;
				const auto &position = body->GetPosition();
				transform.Translation.x = position.x;
				transform.Translation.y = position.y;
				transform.Rotation.z = body->GetAngle();
			}
		}

		// Render
		RenderScene(camera);
	}

	void Scene::OnUpdateEditor(Timestep ts, EditorCamera &camera)
	{
		// Render
		RenderScene(camera);
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;

		// Resize out non-FixedAspectRatio cameras
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			auto &cameraComponent = view.get<CameraComponent>(entity);
			if (!cameraComponent.FixedAspectRatio)
				cameraComponent.Camera.SetViewportSize(width, height);
		}
	}

	void Scene::DuplicateEntity(Entity entity)
	{
		std::string name = entity.GetName();
		Entity newEntity = CreateEntity(name);

		CopyComponentIfExists<TransformComponent>(newEntity, entity);
		CopyComponentIfExists<SpriteRendererComponent>(newEntity, entity);
		CopyComponentIfExists<CircleRendererComponent>(newEntity, entity);
		CopyComponentIfExists<CubeRendererComponent>(newEntity, entity);
		CopyComponentIfExists<SphereRendererComponent>(newEntity, entity);
		CopyComponentIfExists<MaterialComponent>(newEntity, entity);
		CopyComponentIfExists<CameraComponent>(newEntity, entity);
		CopyComponentIfExists<Rigidbody2DComponent>(newEntity, entity);
		CopyComponentIfExists<BoxCollider2DComponent>(newEntity, entity);
		CopyComponentIfExists<CircleCollider2DComponent>(newEntity, entity);

		CopyComponentIfExists<NativeScriptComponent>(newEntity, entity);
		CopyComponentIfExists<ScriptComponent>(newEntity, entity);
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		auto view = m_Registry.view<CameraComponent>();
		for (auto entity : view)
		{
			const auto &camera = view.get<CameraComponent>(entity);
			if (camera.Primary)
				return Entity{entity, this};
		}
		return {};
	}

	bool Scene::IsEntityValid(Entity entity) const
	{
		return m_Registry.valid(entity);
	}

	void Scene::OnPhysics2DStart()
	{
		m_PhysicsWorld = new b2World({0.0f, -9.8f});

		auto view = m_Registry.view<Rigidbody2DComponent>();
		for (auto e : view)
		{
			Entity entity = {e, this};
			auto &transform = entity.GetComponent<TransformComponent>();
			auto &rb2d = entity.GetComponent<Rigidbody2DComponent>();

			b2BodyDef bodyDef;
			bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
			bodyDef.position.Set(transform.Translation.x, transform.Translation.y);
			bodyDef.angle = transform.Rotation.z;

			b2Body *body = m_PhysicsWorld->CreateBody(&bodyDef);
			body->SetFixedRotation(rb2d.FixedRotation);
			rb2d.RuntimeBody = body;

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto &bc2d = entity.GetComponent<BoxCollider2DComponent>();

				b2PolygonShape boxShape;
				boxShape.SetAsBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &boxShape;
				fixtureDef.density = bc2d.Density;
				fixtureDef.friction = bc2d.Friction;
				fixtureDef.restitution = bc2d.Restitution;
				fixtureDef.restitutionThreshold = bc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}

			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				auto &cc2d = entity.GetComponent<CircleCollider2DComponent>();

				b2CircleShape circleShape;
				circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
				circleShape.m_radius = transform.Scale.x * cc2d.Radius;

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &circleShape;
				fixtureDef.density = cc2d.Density;
				fixtureDef.friction = cc2d.Friction;
				fixtureDef.restitution = cc2d.Restitution;
				fixtureDef.restitutionThreshold = cc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}
		}
	}

	void Scene::OnPhysics2DStop()
	{
		delete m_PhysicsWorld;
		m_PhysicsWorld = nullptr;

		ScriptEngine::Shutdown();
	}

	void Scene::OnPhysics3DStart()
	{
		b3WorldDef worldDef = b3DefaultWorldDef();
		worldDef.gravity = {0.0f, -9.81f, 0.0f};
		m_Physics3DWorld = b3CreateWorld(&worldDef);

		auto view = m_Registry.view<RigidBody3DComponent>();
		for (auto e : view)
		{
			Entity entity = {e, this};
			auto &transform = entity.GetComponent<TransformComponent>();
			auto &rb3d = entity.GetComponent<RigidBody3DComponent>();

			b3BodyDef bodyDef = b3DefaultBodyDef();
			bodyDef.type = RigidBody3DTypeToBox3D(rb3d.Type);
			bodyDef.position = {(float)transform.Translation.x, (float)transform.Translation.y, (float)transform.Translation.z};
			bodyDef.rotation = EulerToB3Quat(transform.Rotation);
			bodyDef.gravityScale = rb3d.GravityScale;
			bodyDef.linearDamping = rb3d.LinearDamping;
			bodyDef.angularDamping = rb3d.AngularDamping;
			bodyDef.motionLocks.angularX = rb3d.FixedRotationX;
			bodyDef.motionLocks.angularY = rb3d.FixedRotationY;
			bodyDef.motionLocks.angularZ = rb3d.FixedRotationZ;

			b3BodyId bodyId = b3CreateBody(m_Physics3DWorld, &bodyDef);

			// Store body ID
			static_assert(sizeof(b3BodyId) == sizeof(uint64_t));
			std::memcpy(&rb3d.RuntimeBody, &bodyId, sizeof(b3BodyId));

			// Box collider
			if (entity.HasComponent<BoxCollider3DComponent>())
			{
				auto &bc = entity.GetComponent<BoxCollider3DComponent>();
				b3BoxHull boxHull = b3MakeBoxHull(
					bc.HalfExtents.x * transform.Scale.x,
					bc.HalfExtents.y * transform.Scale.y,
					bc.HalfExtents.z * transform.Scale.z);

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = bc.Density;
				shapeDef.baseMaterial.friction = bc.Friction;
				shapeDef.baseMaterial.restitution = bc.Restitution;

				b3ShapeId shapeId = b3CreateHullShape(bodyId, &shapeDef, &boxHull.base);
				std::memcpy(&bc.RuntimeShape, &shapeId, sizeof(b3ShapeId));
			}

			// Sphere collider
			if (entity.HasComponent<SphereCollider3DComponent>())
			{
				auto &sc = entity.GetComponent<SphereCollider3DComponent>();
				b3Sphere sphere;
				sphere.center = {sc.Offset.x, sc.Offset.y, sc.Offset.z};
				sphere.radius = sc.Radius * std::max({transform.Scale.x, transform.Scale.y, transform.Scale.z});

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = sc.Density;
				shapeDef.baseMaterial.friction = sc.Friction;
				shapeDef.baseMaterial.restitution = sc.Restitution;

				b3ShapeId shapeId = b3CreateSphereShape(bodyId, &shapeDef, &sphere);
				std::memcpy(&sc.RuntimeShape, &shapeId, sizeof(b3ShapeId));
			}

			// Capsule collider
			if (entity.HasComponent<CapsuleCollider3DComponent>())
			{
				auto &cc = entity.GetComponent<CapsuleCollider3DComponent>();
				float halfH = cc.Height * 0.5f * transform.Scale.y;
				float r = cc.Radius * std::max(transform.Scale.x, transform.Scale.z);

				b3Capsule capsule;
				capsule.center1 = {cc.Offset.x, cc.Offset.y - halfH, cc.Offset.z};
				capsule.center2 = {cc.Offset.x, cc.Offset.y + halfH, cc.Offset.z};
				capsule.radius = r;

				b3ShapeDef shapeDef = b3DefaultShapeDef();
				shapeDef.density = cc.Density;
				shapeDef.baseMaterial.friction = cc.Friction;
				shapeDef.baseMaterial.restitution = cc.Restitution;

				b3ShapeId shapeId = b3CreateCapsuleShape(bodyId, &shapeDef, &capsule);
				std::memcpy(&cc.RuntimeShape, &shapeId, sizeof(b3ShapeId));
			}
		}
	}

	void Scene::OnPhysics3DStop()
	{
		if (b3World_IsValid(m_Physics3DWorld))
		{
			b3DestroyWorld(m_Physics3DWorld);
			m_Physics3DWorld = B3_NULL_ID;
		}
	}

	void Scene::RenderScene(EditorCamera &camera)
	{
		// Render 2D
		Renderer2D::BeginScene(camera);

		// Draw sprites
		{
			auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
			for (auto entity : group)
			{
				auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

				Renderer2D::DrawSprite(transform.GetTransform(), sprite, (int)entity);
				// Renderer2D::DrawRect(transform.Translation, transform.Scale, glm::vec4(1.0f), (int)entity);
			}
		}

		// Draw circles
		{
			auto view = m_Registry.view<TransformComponent, CircleRendererComponent>();
			for (auto entity : view)
			{
				auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

				Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, (int)entity);
			}
		}

		// Renderer2D::DrawLine(glm::vec3(2.0f), glm::vec3(5.0f), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
		// Renderer2D::DrawRect(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

		Renderer2D::EndScene();

		// Render 3D
		Renderer3D::BeginScene(camera);

		// Draw Cubes
		{
			auto cubeView = m_Registry.view<TransformComponent, CubeRendererComponent, MaterialComponent>();
			for (auto entity : cubeView)
			{
				auto &tc = cubeView.get<TransformComponent>(entity);
				auto &crc = cubeView.get<CubeRendererComponent>(entity);
				auto &mc = cubeView.get<MaterialComponent>(entity);

				// Pass the 3D entity data to the new batch renderer
				Renderer3D::DrawCube(tc.GetTransform(), crc.Color, mc, (int)entity);
			}
		}

		// Draw Spheres
		{
			auto sphereView = m_Registry.view<TransformComponent, SphereRendererComponent, MaterialComponent>();
			for (auto entity : sphereView)
			{
				auto &tc = sphereView.get<TransformComponent>(entity);
				auto &src = sphereView.get<SphereRendererComponent>(entity);
				auto &mc = sphereView.get<MaterialComponent>(entity);

				Renderer3D::DrawSphere(
					tc.GetTransform(),
					src.Color,
					src.Radius,
					src.Sectors,
					src.Stacks,
					mc,
					(int)entity);
			}
		}

		Renderer3D::EndScene();
	}

	template <typename T>
	void Scene::OnComponentAdded(Entity entity, T &component)
	{
		static_assert(false);
	}

	template <>
	void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent &component)
	{
		component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}

	template <>
	void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CircleRendererComponent>(Entity entity, CircleRendererComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CubeRendererComponent>(Entity entity, CubeRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
	}

	template <>
	void Scene::OnComponentAdded<SphereRendererComponent>(Entity entity, SphereRendererComponent &component)
	{
		if (!entity.HasComponent<TransformComponent>())
		{
			entity.AddComponent<TransformComponent>();
		}

		component.TextureIndex = 0;
		component.TilingFactor = 1.0f;
		component.Radius = 0.5f;
		component.Sectors = 20;
		component.Stacks = 20;
	}

	template <>
	void Scene::OnComponentAdded<MaterialComponent>(Entity entity, MaterialComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<ScriptComponent>(Entity entity, ScriptComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<Rigidbody2DComponent>(Entity entity, Rigidbody2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<BoxCollider2DComponent>(Entity entity, BoxCollider2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CircleCollider2DComponent>(Entity entity, CircleCollider2DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<RigidBody3DComponent>(Entity entity, RigidBody3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<BoxCollider3DComponent>(Entity entity, BoxCollider3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<SphereCollider3DComponent>(Entity entity, SphereCollider3DComponent &component)
	{
	}

	template <>
	void Scene::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent &component)
	{
	}

}