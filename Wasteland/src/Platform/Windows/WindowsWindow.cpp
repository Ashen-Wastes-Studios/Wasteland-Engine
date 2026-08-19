#include "wlpch.h"
#include "WindowsWindow.h"

#include <Wasteland/Events/KeyEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/ApplicationEvent.h>
#include <Wasteland/Core/Application.h>
#include <Wasteland/ImGui/ImGuiLayer.h>

#include "Platform/OpenGL/OpenGLContext.h"
#include "Platform/NVRHI/NVRHIContext.h"
#include "Platform/NVRHI/NVRHIRendererAPI.h"

#include <Wasteland/Renderer/RenderCommand.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace Wasteland
{

	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char *description)
	{
		WL_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

#if defined(WL_PLATFORM_WINDOWS)
	Window *Window::Create(const WindowProps &props)
	{
		return new WindowsWindow(props);
	}
#endif

	WindowsWindow::WindowsWindow(const WindowProps &props)
	{
		WL_PROFILE_FUNCTION();

		Init(props);
	}

	WindowsWindow::~WindowsWindow()
	{
		WL_PROFILE_FUNCTION();

		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps &props)
	{
		WL_PROFILE_FUNCTION();

		m_Data.Title = props.Title;
		m_Data.Width = props.Width;
		m_Data.Height = props.Height;

		WL_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

		if (!s_GLFWInitialized)
		{
			WL_PROFILE_SCOPE("glfwInit");
			// TODO: glfwTerminate on system shutdown
			int success = glfwInit();
			glfwSetErrorCallback(GLFWErrorCallback);
			WL_CORE_ASSERT(success, "Could not initialize GLFW!");

			s_GLFWInitialized = true;
		}

		{
			WL_PROFILE_SCOPE("glfwCreateWindow");
			m_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
		}

		// Create all contexts
		CreateContexts();

		// Initialize the current API context
		RendererAPI::API currentAPI = RendererAPI::GetAPI();
		SwitchRendererAPI(currentAPI);

		glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVSync(true);

		// Set GLFW callbacks
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
								  {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				data.Width = width;
				data.Height = height;

				WindowResizeEvent event(width, height);
				WL_CORE_WARN("{0}, {1}", width, height);
				data.EventCallback(event); });

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
								   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				WindowCloseEvent event;
				data.EventCallback(event); });

		glfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
						   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				switch (action)
				{
					case GLFW_PRESS:
					{
						KeyPressedEvent event(key, 0);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent event(key);
						data.EventCallback(event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent event(key, 1);
						data.EventCallback(event);
						break;
					}
				} });

		glfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
							{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				KeyTypedEvent event(keycode);
				data.EventCallback(event); });

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods)
								   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				switch (action)
				{
					case GLFW_PRESS:
					{
						MouseButtonPressedEvent event(button);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent event(button);
						data.EventCallback(event);
						break;
					}
				} });

		glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
							  {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				MouseScrolledEvent event((float)xOffset, (float)yOffset);
				data.EventCallback(event); });

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
								 {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

				MouseMovedEvent event((float)xPos, (float)yPos);
				data.EventCallback(event); });
	}

	void WindowsWindow::CreateContexts()
	{
		WL_PROFILE_FUNCTION();

		// Create OpenGL context
		m_OpenGLContext = new OpenGLContext(m_Window);
		m_OpenGLContext->Init();

		// Create NVRHI context (will be initialized on first switch)
		// We don't initialize it here to avoid creating DX device when not needed
		m_NVRHIContext = nullptr;

		WL_CORE_INFO("Created OpenGL context");
	}

	void WindowsWindow::DestroyContexts()
	{
		WL_PROFILE_FUNCTION();

		if (m_OpenGLContext)
		{
			delete m_OpenGLContext;
			m_OpenGLContext = nullptr;
		}

		if (m_NVRHIContext)
		{
			delete m_NVRHIContext;
			m_NVRHIContext = nullptr;
		}

		m_CurrentContext = nullptr;
	}

	void WindowsWindow::SwitchRendererAPI(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		if (api == m_CurrentAPI && m_CurrentContext)
			return;

		WL_CORE_INFO("Switching renderer API to {0}", (int)api);

		// Clear cached resources when switching away from NVRHI
		if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
		    m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
		    m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
		{
			NVRHIRendererAPI* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
			{
				nvrhiAPI->ClearCachedResources();
			}
		}

		// Create NVRHI context if needed and not already created
		if ((api == RendererAPI::API::NVRHI_DX11 ||
		     api == RendererAPI::API::NVRHI_DX12 ||
		     api == RendererAPI::API::NVRHI_Vulkan) && !m_NVRHIContext)
		{
			m_NVRHIContext = new NVRHIContext(m_Window, api);
			m_NVRHIContext->Init();

			// Set the context on the renderer API
			NVRHIRendererAPI* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
			{
				nvrhiAPI->SetContext(static_cast<NVRHIContext*>(m_NVRHIContext));
			}

			WL_CORE_INFO("Created NVRHI context");
		}
		else if (api == RendererAPI::API::NVRHI_DX11 ||
		         api == RendererAPI::API::NVRHI_DX12 ||
		         api == RendererAPI::API::NVRHI_Vulkan)
		{
			// Switching between NVRHI APIs - need to recreate context
			if (m_NVRHIContext)
			{
				delete m_NVRHIContext;
				m_NVRHIContext = new NVRHIContext(m_Window, api);
				m_NVRHIContext->Init();

				NVRHIRendererAPI* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
				{
					nvrhiAPI->SetContext(static_cast<NVRHIContext*>(m_NVRHIContext));
				}
			}
		}

		// Switch to the appropriate context
		if (api == RendererAPI::API::OpenGL)
		{
			m_CurrentContext = m_OpenGLContext;
		}
		else if (api == RendererAPI::API::NVRHI_DX11 ||
		         api == RendererAPI::API::NVRHI_DX12 ||
		         api == RendererAPI::API::NVRHI_Vulkan)
		{
			m_CurrentContext = m_NVRHIContext;
		}

		m_CurrentAPI = api;

		// Switch ImGui backend to match the new renderer API
		if (Application::IsInitialized())
		{
			ImGuiLayer* imguiLayer = Application::Get().GetImGuiLayer();
			if (imguiLayer)
			{
				imguiLayer->SwitchBackend(api);
			}
		}
	}

	void WindowsWindow::Shutdown()
	{
		WL_PROFILE_FUNCTION();

		DestroyContexts();
		glfwDestroyWindow(m_Window);
	}

	void WindowsWindow::OnUpdate()
	{
		WL_PROFILE_FUNCTION();

		glfwPollEvents();
		if (m_CurrentContext)
			m_CurrentContext->SwapBuffers();
	}

	void WindowsWindow::SetVSync(bool enabled)
	{
		WL_PROFILE_FUNCTION();

		if (enabled)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);

		m_Data.VSync = enabled;
	}

	bool WindowsWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

}