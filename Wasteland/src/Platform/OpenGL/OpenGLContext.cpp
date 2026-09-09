#include "wlpch.h"
#include "OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Wasteland {

	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		WL_CORE_ASSERT(windowHandle, "Window handle is null!");
	}


	OpenGLContext::~OpenGLContext()
	{
		// Detach our context from the calling thread if it is current.
		// Destroying a window whose context is current (glfwDestroyWindow in
		// the GL -> NO_API switch path) is forbidden by GLFW and corrupts
		// driver state in nvoglv64.dll — the same binary that implements
		// Vulkan — crashing later Vulkan calls (seen as an AV in
		// nvrhi::vulkan::Queue::Queue's semaphore creation).
		if (m_WindowHandle && glfwGetCurrentContext() == m_WindowHandle)
			glfwMakeContextCurrent(nullptr);
		m_WindowHandle = nullptr;
	}

	void OpenGLContext::Init()
	{
		WL_PROFILE_FUNCTION();

		glfwMakeContextCurrent(m_WindowHandle);
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		WL_CORE_ASSERT(status, "Failed to initialize Glad!");

		WL_CORE_INFO("OpenGL Info:");
		WL_CORE_INFO(" Vendor: {0}", (const char*)glGetString(GL_VENDOR));
		WL_CORE_INFO(" Renderer: {0}", (const char*)glGetString(GL_RENDERER));
		WL_CORE_INFO(" Version: {0}", (const char*)glGetString(GL_VERSION));

#ifdef WL_ENABLE_ASSERTS
		int versionMajor;
		int versionMinor;
		glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
		glGetIntegerv(GL_MINOR_VERSION, &versionMinor);

		WL_CORE_ASSERT(versionMajor > 4 || (versionMajor == 4 && versionMinor >= 5), "Wasteland requires at least OpenGL version 4.5!");
#endif
	}

	void OpenGLContext::SwapBuffers()
	{
		WL_PROFILE_FUNCTION();

		glfwSwapBuffers(m_WindowHandle);
	}

}