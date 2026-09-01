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
			// Set window hints based on initial renderer API
			// Vulkan and DirectX require NO_API, OpenGL requires OPENGL_API
			RendererAPI::API initialAPI = RendererAPI::GetAPI();
			glfwDefaultWindowHints();
			if (initialAPI == RendererAPI::API::NVRHI_Vulkan ||
				initialAPI == RendererAPI::API::NVRHI_DX11 ||
				initialAPI == RendererAPI::API::NVRHI_DX12)
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			}
			else
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
			}
			glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
			glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

			m_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
			WL_CORE_ASSERT(m_Window, "Failed to create GLFW window!");
		}

		// Create all contexts
		CreateContexts();

		// Initialize the current API context
		RendererAPI::API currentAPI = RendererAPI::GetAPI();
		SwitchRendererAPI(currentAPI);

		glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVSync(true);

		// Set GLFW callbacks
		SetupCallbacks();
	}

	void WindowsWindow::CreateContexts()
	{
		WL_PROFILE_FUNCTION();

		RendererAPI::API api = RendererAPI::GetAPI();
		bool needGL = (api == RendererAPI::API::OpenGL);
		// Only create OpenGL context if we are starting with OpenGL
		// For NO_API windows (Vulkan/DX), OpenGL context would fail
		if (needGL)
		{
			m_OpenGLContext = new OpenGLContext(m_Window);
			m_OpenGLContext->Init();
			WL_CORE_INFO("Created OpenGL context");
		}
		else
		{
			m_OpenGLContext = nullptr;
			WL_CORE_INFO("Skipping OpenGL context creation for NO_API window");
		}

		// Create NVRHI context (will be initialized on first switch)
		// We don't initialize it here to avoid creating DX device when not needed
		m_NVRHIContext = nullptr;
	}

	void WindowsWindow::SetupCallbacks()
	{
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
								  {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				if (data.SuppressCloseEvent)
					return;
				data.Width = width;
				data.Height = height;

				WindowResizeEvent event(width, height);
				WL_CORE_WARN("{0}, {1}", width, height);
				data.EventCallback(event); });

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
								   {
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				if (data.SuppressCloseEvent)
					return;
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
		if (m_PendingSwitch && m_PendingAPI == api)
			return;

		m_PendingAPI = api;
		m_PendingSwitch = true;

		// If called during init (no current API or app not fully initialized), switch immediately
		// Otherwise defer to next BeginFrame (safe, outside ImGui frame)
		if (!Application::IsInitialized() || m_CurrentAPI == RendererAPI::API::None || m_Window == nullptr)
		{
			ProcessPendingSwitch();
		}
	}

	void WindowsWindow::ProcessPendingSwitch()
	{
		if (!m_PendingSwitch)
			return;
		RendererAPI::API api = m_PendingAPI;
		m_PendingSwitch = false;
		ExecuteSwitch(api);
	}

	void WindowsWindow::ExecuteSwitch(RendererAPI::API api)
	{
		WL_PROFILE_FUNCTION();

		if (api == m_CurrentAPI && m_CurrentContext)
			return;
		WL_CORE_INFO("Switching renderer API to {0}", (int)api);
		// Check if window needs recreation due to GLFW_CLIENT_API mismatch
		// OpenGL requires OPENGL_API, Vulkan/DX require NO_API
		// At startup m_CurrentAPI is None, window was just created with correct hints for 'api', so no recreate needed
		bool isNewNoAPI = (api == RendererAPI::API::NVRHI_Vulkan ||
						   api == RendererAPI::API::NVRHI_DX11 ||
						   api == RendererAPI::API::NVRHI_DX12);
		bool isOldNoAPI = (m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan ||
						   m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
						   m_CurrentAPI == RendererAPI::API::NVRHI_DX12);
		bool needWindowRecreate = false;
		if (m_CurrentAPI != RendererAPI::API::None && m_Window != nullptr)
			needWindowRecreate = (isNewNoAPI != isOldNoAPI);

		if (needWindowRecreate)
		{
			WL_CORE_INFO("Recreating GLFW window for API switch (GL <-> NO_API)");
			m_Data.SuppressCloseEvent = true;
			if (m_Window)
			{
				glfwSetWindowCloseCallback(m_Window, nullptr);
				glfwSetWindowSizeCallback(m_Window, nullptr);
			}
			// Save window properties
			WindowProps props(m_Data.Title, m_Data.Width, m_Data.Height);

			// Clear NVRHI cached resources before destroying contexts
			if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
			{
				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
					nvrhiAPI->ClearCachedResources();
			}

			// Shutdown ImGui backend before window destruction (it holds GLFW window references)
			if (Application::IsInitialized())
			{
				ImGuiLayer *imguiLayer = Application::Get().GetImGuiLayer();
				if (imguiLayer)
					imguiLayer->ShutdownBackendForWindowRecreate();
			}

			// Destroy contexts that are tied to old window
			DestroyContexts();
			glfwDestroyWindow(m_Window);
			m_Window = nullptr;

			// Set hints for new API
			glfwDefaultWindowHints();
			if (isNewNoAPI)
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			}
			else
			{
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
			}
			glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
			glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

			m_Window = glfwCreateWindow((int)props.Width, (int)props.Height, props.Title.c_str(), nullptr, nullptr);
			WL_CORE_ASSERT(m_Window, "Failed to recreate GLFW window for API switch!");

			// Recreate contexts for new window
			if (api == RendererAPI::API::OpenGL)
			{
				m_OpenGLContext = new OpenGLContext(m_Window);
				m_OpenGLContext->Init();
				m_NVRHIContext = nullptr;
			}
			else
			{
				m_OpenGLContext = nullptr;
				m_NVRHIContext = nullptr; // will be created below
			}

			m_Data.Width = props.Width;
			m_Data.Height = props.Height;
			glfwSetWindowUserPointer(m_Window, &m_Data);
			SetupCallbacks();
			m_Data.SuppressCloseEvent = false;
			// Set VSync directly without calling glfwSwapInterval for NO_API windows
			// glfwSwapInterval is only valid for OpenGL contexts
			m_Data.VSync = true;
			if (!isNewNoAPI)
			{
				// Only call glfwSwapInterval for OpenGL windows
				glfwMakeContextCurrent(m_Window);
				glfwSwapInterval(1);
			}
			SetupCallbacks();

			// Reset current context tracking so the switch below creates the right context
			m_CurrentContext = nullptr;
			m_CurrentAPI = RendererAPI::API::None;
		}

		// Ensure global RenderCommand API matches target api before creating contexts
		// This makes the switch atomic (previously EditorLayer called SetAPI separately causing 1-frame mismatch)
		if (RendererAPI::GetAPI() != api)
		{
			// Clear cached resources from old NVRHI API before deleting it
			if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
			{
				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
					nvrhiAPI->ClearCachedResources();
			}
			RenderCommand::SetAPI(api);
		}
		else if (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
				 m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
				 m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
		{
			// Clear cached resources when switching away from NVRHI but API enum already matches
			NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
				nvrhiAPI->ClearCachedResources();
		}

		// Create NVRHI context if needed and not already created
		if ((api == RendererAPI::API::NVRHI_DX11 ||
			 api == RendererAPI::API::NVRHI_DX12 ||
			 api == RendererAPI::API::NVRHI_Vulkan) &&
			!m_NVRHIContext)
		{
			m_NVRHIContext = new NVRHIContext(m_Window, api);
			m_NVRHIContext->Init();

			// Set the context on the renderer API
			NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
			if (nvrhiAPI)
			{
				nvrhiAPI->SetContext(static_cast<NVRHIContext *>(m_NVRHIContext));
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

				NVRHIRendererAPI *nvrhiAPI = dynamic_cast<NVRHIRendererAPI *>(RenderCommand::GetRendererAPI());
				if (nvrhiAPI)
				{
					nvrhiAPI->SetContext(static_cast<NVRHIContext *>(m_NVRHIContext));
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
			ImGuiLayer *imguiLayer = Application::Get().GetImGuiLayer();
			if (imguiLayer != nullptr)
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

	void WindowsWindow::BeginFrame()
	{
		WL_PROFILE_FUNCTION();
		if (m_PendingSwitch)
			ProcessPendingSwitch();
		if (m_CurrentContext)
			m_CurrentContext->BeginFrame();
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

		m_Data.VSync = enabled;
		// glfwSwapInterval is only valid for OpenGL contexts
		// For NO_API windows (Vulkan/DX), it's a no-op and may generate GLFW error
		if (m_Window && glfwGetWindowAttrib(m_Window, GLFW_CLIENT_API) == GLFW_OPENGL_API)
		{
			if (enabled)
				glfwSwapInterval(1);
			else
				glfwSwapInterval(0);
		}
	}

	bool WindowsWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

}