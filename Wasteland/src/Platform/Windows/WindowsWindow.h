#pragma once

#include "Wasteland/Core/Window.h"
#include "Wasteland/Renderer/GraphicsContext.h"
#include "Wasteland/Renderer/RendererAPI.h"

#include <GLFW/glfw3.h>

namespace Wasteland
{

	class WindowsWindow : public Window
	{
	public:
		WindowsWindow(const WindowProps &props);
		virtual ~WindowsWindow();

		void OnUpdate() override;
		void BeginFrame() override;

		inline unsigned int GetWidth() const override { return m_Data.Width; }
		inline unsigned int GetHeight() const override { return m_Data.Height; }

		// Window attributes
		inline void SetEventCallback(const EventCallbackFn &callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;

		inline virtual void *GetNativeWindow() const { return m_Window; }

		// Multi-backend support
		void SwitchRendererAPI(RendererAPI::API api) override;
		GraphicsContext *GetCurrentContext() const override { return m_CurrentContext; }

	private:
		virtual void Init(const WindowProps &props);
		virtual void Shutdown();
		void CreateContexts();
		void DestroyContexts();
		void SetupCallbacks();
		void ExecuteSwitch(RendererAPI::API api);
		void ProcessPendingSwitch();

	private:
		GLFWwindow *m_Window;

		// Multiple contexts for different backends
		GraphicsContext *m_OpenGLContext = nullptr;
		GraphicsContext *m_NVRHIContext = nullptr;
		GraphicsContext *m_CurrentContext = nullptr;
		RendererAPI::API m_CurrentAPI = RendererAPI::API::None;
		RendererAPI::API m_PendingAPI = RendererAPI::API::None;
		bool m_PendingSwitch = false;

		struct WindowData
		{
			std::string Title;
			unsigned int Width, Height;
			bool VSync;
			bool SuppressCloseEvent = false;

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};

}
