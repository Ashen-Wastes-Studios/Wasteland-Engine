#pragma once

#include "Wasteland/Core/Layer.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <Wasteland/Events/ApplicationEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/KeyEvent.h>

struct ID3D11ShaderResourceView;

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
		// Attempt to fall back to OpenGL. Returns false if no valid GL context
		// exists (e.g. when switching away from OpenGL — the context is gone).
		bool FallbackToOpenGL();
	public:
		void ShutdownBackendForWindowRecreate();
		
		bool m_BlockEvents = true;
		float m_Time = 0.0f;
		RendererAPI::API m_CurrentBackend = RendererAPI::API::None;
		RendererAPI::API m_PendingBackendSwitch = RendererAPI::API::None;
		bool m_BackendSwitchPending = false;
		bool m_GlfwInitialized = false;
		bool m_FrameStarted = false;

		// Neutral 1x1 white SRV (DX11): substituted for non-font ImTextureIDs
		// (panels submit GL texture IDs) so DrawIndexed can't fault on bogus pointers.
		ID3D11ShaderResourceView *m_D3D11WhiteSRV = nullptr;

		public:
		bool HasBackend() const { return m_CurrentBackend != RendererAPI::API::None; }
	};

}