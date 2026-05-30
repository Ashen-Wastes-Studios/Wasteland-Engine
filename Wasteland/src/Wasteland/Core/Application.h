#pragma once

#include "Core.h"

#include "Wasteland/Core/Window.h"
#include "Wasteland/Core/LayerStack.h"
#include "Wasteland/Events/Event.h"
#include "Wasteland/Events/ApplicationEvent.h"

#include "Wasteland/Core/Timestep.h"

#include "Wasteland/ImGui/ImGuiLayer.h"

namespace Wasteland
{

	class Application
	{
	public:
		Application(const std::string &name = "Wasteland App");
		virtual ~Application();

		void Run();

		void OnEvent(Event &e);

		void PushLayer(Layer *layer);
		void PushOverlay(Layer *layer);

		inline Window &GetWindow() { return *m_Window; }

		void Close();

		ImGuiLayer *GetImGuiLayer() { return m_ImGuiLayer; }

		inline static Application &Get() { return *s_Instance; }

		static bool IsInitialized() { return s_Instance != nullptr; }

	private:
		bool OnWindowClose(WindowCloseEvent &e);
		bool OnWindowResize(WindowResizeEvent &e);

	private:
		std::unique_ptr<Window> m_Window;
		ImGuiLayer *m_ImGuiLayer;
		bool m_Running = true;
		LayerStack m_LayerStack;
		float m_LastFrameTime = 0.0f;
		bool m_Minimized = false;

	private:
		static Application *s_Instance;
	};

	// To be defined in CLIENT
	Application *CreateApplication();

}
