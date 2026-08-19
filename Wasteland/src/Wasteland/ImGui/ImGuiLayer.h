#pragma once

#include "Wasteland/Core/Layer.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <Wasteland/Events/ApplicationEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/KeyEvent.h>

namespace Wasteland {

	class WL_API ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer();

		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnImGuiRender() override;

		void OnEvent(Event& event) override;

		void Begin();
		void End();

		void BlockEvents(bool block) { m_BlockEvents = block; }
		
		// Switch ImGui backend based on current renderer API
		void SwitchBackend(RendererAPI::API api);
		
	private:
		void InitBackend(RendererAPI::API api);
		void ShutdownBackend();
		void PerformBackendSwitch();
		
		bool m_BlockEvents = true;
		float m_Time = 0.0f;
		RendererAPI::API m_CurrentBackend = RendererAPI::API::None;
		RendererAPI::API m_PendingBackendSwitch = RendererAPI::API::None;
		bool m_BackendSwitchPending = false;
	};

}