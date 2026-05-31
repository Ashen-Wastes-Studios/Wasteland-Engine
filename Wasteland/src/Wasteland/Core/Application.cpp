#include "wlpch.h"
#include "Application.h"

#include "Wasteland/Core/Log.h"

#include "Wasteland/Renderer/Renderer.h"

#include "Input.h"

#include <GLFW/glfw3.h>

#include <pybind11/pybind11.h>
#include <Python.h>

namespace Wasteland
{

#define BIND_EVENT_FN(x) std::bind(&Application::x, this, std::placeholders::_1)

	Application *Application::s_Instance = nullptr;

	extern "C" PyObject *PyInit_Wasteland(void);

	Application::Application(const std::string &name)
	{
		WL_PROFILE_FUNCTION();

		PyImport_AppendInittab("Wasteland", &PyInit_Wasteland);

		Py_SetPythonHome(L"C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64");

		Py_Initialize();

		PyRun_SimpleString("import sys; print('Python sys.path:', sys.path)");

		WL_CORE_ASSERT(!s_Instance, "Application already exists");
		s_Instance = this;

		m_Window = std::unique_ptr<Window>(Window::Create(WindowProps(name)));
		m_Window->SetEventCallback(BIND_EVENT_FN(OnEvent));

		Renderer::Init();

		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	Application::~Application()
	{
		WL_PROFILE_FUNCTION();

		Py_Finalize();
	}

	void Application::PushLayer(Layer *layer)
	{
		WL_PROFILE_FUNCTION();

		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer *layer)
	{
		WL_PROFILE_FUNCTION();

		m_LayerStack.PushOverlay(layer);
		layer->OnAttach();
	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::OnEvent(Event &e)
	{
		WL_PROFILE_FUNCTION();

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FN(OnWindowResize));

		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin();)
		{
			(*--it)->OnEvent(e);
			if (e.Handled)
				break;
		}
	}

	void Application::Run()
	{
		WL_PROFILE_FUNCTION();

		while (m_Running)
		{
			WL_PROFILE_SCOPE("RunLoop");

			float time = (float)glfwGetTime();
			Timestep timestep = time - m_LastFrameTime;
			m_LastFrameTime = time;

			if (!m_Minimized)
			{
				{
					WL_PROFILE_SCOPE("LayerStack OnUpdate");

					for (Layer *layer : m_LayerStack)
						layer->OnUpdate(timestep);
				}

				m_ImGuiLayer->Begin();
				{
					WL_PROFILE_SCOPE("LayerStack OnImGuiRender");

					for (Layer *layer : m_LayerStack)
						layer->OnImGuiRender();
				}
				m_ImGuiLayer->End();
			}

			m_Window->OnUpdate();
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent &e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent &e)
	{
		WL_PROFILE_FUNCTION();

		if (e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			m_Minimized = true;
			return false;
		}

		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());

		return false;
	}

}