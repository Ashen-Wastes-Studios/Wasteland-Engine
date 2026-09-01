import pathlib
path = pathlib.Path(r"C:/dev/Wasteland/Wasteland/src/Platform/Windows/WindowsWindow.cpp")
text = path.read_text(encoding='utf-8')

new_init = """\tvoid WindowsWindow::Init(const WindowProps &props)
\t{
\t\tWL_PROFILE_FUNCTION();

\t\tm_Data.Title = props.Title;
\t\tm_Data.Width = props.Width;
\t\tm_Data.Height = props.Height;

\t\tWL_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

\t\tif (!s_GLFWInitialized)
\t\t{
\t\t\tWL_PROFILE_SCOPE("glfwInit");
\t\t\t// TODO: glfwTerminate on system shutdown
\t\t\tint success = glfwInit();
\t\t\tglfwSetErrorCallback(GLFWErrorCallback);
\t\t\tWL_CORE_ASSERT(success, "Could not initialize GLFW!");

\t\t\ts_GLFWInitialized = true;
\t\t}

\t\t{
\t\t\tWL_PROFILE_SCOPE("glfwCreateWindow");
\t\t\t// Set window hints based on initial renderer API
\t\t\t// Vulkan and DirectX require NO_API, OpenGL requires OPENGL_API
\t\t\tRendererAPI::API initialAPI = RendererAPI::GetAPI();
\t\t\tglfwDefaultWindowHints();
\t\t\tif (initialAPI == RendererAPI::API::NVRHI_Vulkan ||
\t\t\t    initialAPI == RendererAPI::API::NVRHI_DX11 ||
\t\t\t    initialAPI == RendererAPI::API::NVRHI_DX12)
\t\t\t{
\t\t\t\tglfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tglfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
\t\t\t\tglfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
\t\t\t\tglfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
\t\t\t\tglfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
\t\t\t}
\t\t\tglfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
\t\t\tglfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

\t\t\tm_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
\t\t\tWL_CORE_ASSERT(m_Window, "Failed to create GLFW window!");
\t\t}

\t\t// Create all contexts
\t\tCreateContexts();
\t\t// Initialize the current API context
\t\tRendererAPI::API currentAPI = RendererAPI::GetAPI();
\t\tSwitchRendererAPI(currentAPI);

\t\tglfwSetWindowUserPointer(m_Window, &m_Data);
\t\tSetVSync(true);

\t\t// Set GLFW callbacks
\t\tSetupCallbacks();
\t}"""

old_init = """\tvoid WindowsWindow::Init(const WindowProps &props)
\t{
\t\tWL_PROFILE_FUNCTION();

\t\tm_Data.Title = props.Title;
\t\tm_Data.Width = props.Width;
\t\tm_Data.Height = props.Height;

\t\tWL_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

\t\tif (!s_GLFWInitialized)
\t\t{
\t\t\tWL_PROFILE_SCOPE("glfwInit");
\t\t\t// TODO: glfwTerminate on system shutdown
\t\t\tint success = glfwInit();
\t\t\tglfwSetErrorCallback(GLFWErrorCallback);
\t\t\tWL_CORE_ASSERT(success, "Could not initialize GLFW!");

\t\t\ts_GLFWInitialized = true;
\t\t}

\t\t{
\t\t\tWL_PROFILE_SCOPE("glfwCreateWindow");
\t\t\tm_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
\t\t}

\t\t// Create all contexts
\t\tCreateContexts();
\t\t// Initialize the current API context
\t\tRendererAPI::API currentAPI = RendererAPI::GetAPI();
\t\tSwitchRendererAPI(currentAPI);

\t\tglfwSetWindowUserPointer(m_Window, &m_Data);
\t\tSetVSync(true);

\t\t// Set GLFW callbacks
\t\tglfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
\t\t\t\t\t\t\t\t  {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
\t\t\t\tdata.Width = width;
\t\t\t\tdata.Height = height;

\t\t\t\tWindowResizeEvent event(width, height);
\t\t\t\tWL_CORE_WARN("{0}, {1}", width, height);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
\t\t\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
\t\t\t\tWindowCloseEvent event;
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tswitch (action)
\t\t\t\t{
\t\t\t\t\tcase GLFW_PRESS:
\t\t\t\t\t{
\t\t\t\t\t\tKeyPressedEvent event(key, 0);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_RELEASE:
\t\t\t\t\t{
\t\t\t\t\t\tKeyReleasedEvent event(key);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_REPEAT:
\t\t\t\t\t{
\t\t\t\t\t\tKeyPressedEvent event(key, 1);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t} });

\t\tglfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
\t\t\t\t\t\t\t{
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tKeyTypedEvent event(keycode);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods)
\t\t\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tswitch (action)
\t\t\t\t{
\t\t\t\t\tcase GLFW_PRESS:
\t\t\t\t\t{
\t\t\t\t\t\tMouseButtonPressedEvent event(button);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_RELEASE:
\t\t\t\t\t{
\t\t\t\t\t\tMouseButtonReleasedEvent event(button);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t} });

\t\tglfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
\t\t\t\t\t\t\t  {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tMouseScrolledEvent event((float)xOffset, (float)yOffset);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
\t\t\t\t\t\t\t\t {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tMouseMovedEvent event((float)xPos, (float)yPos);
\t\t\t\tdata.EventCallback(event); });
\t}"""

if old_init in text:
    text = text.replace(old_init, new_init)
    print("patched Init")
else:
    print("Init not found")
    print(text.find("void WindowsWindow::Init"))

# Add SetupCallbacks helper after CreateContexts (?) We'll need to patch CreateContexts and add helper
# Now patch CreateContexts to be conditional
old_create = """\tvoid WindowsWindow::CreateContexts()
\t{
\t\tWL_PROFILE_FUNCTION();

\t\t// Create OpenGL context
\t\tm_OpenGLContext = new OpenGLContext(m_Window);
\t\tm_OpenGLContext->Init();

\t\t// Create NVRHI context (will be initialized on first switch)
\t\t// We don't initialize it here to avoid creating DX device when not needed
\t\tm_NVRHIContext = nullptr;

\t\tWL_CORE_INFO("Created OpenGL context");
\t}"""

new_create = """\tvoid WindowsWindow::CreateContexts()
\t{
\t\tWL_PROFILE_FUNCTION();

\t\tRendererAPI::API api = RendererAPI::GetAPI();
\t\tbool needGL = (api == RendererAPI::API::OpenGL);
\t\t// Only create OpenGL context if we are starting with OpenGL
\t\t// For NO_API windows (Vulkan/DX), OpenGL context would fail
\t\tif (needGL)
\t\t{
\t\t\tm_OpenGLContext = new OpenGLContext(m_Window);
\t\t\tm_OpenGLContext->Init();
\t\t\tWL_CORE_INFO("Created OpenGL context");
\t\t}
\t\telse
\t\t{
\t\t\tm_OpenGLContext = nullptr;
\t\t\tWL_CORE_INFO("Skipping OpenGL context creation for NO_API window");
\t\t}

\t\t// Create NVRHI context (will be initialized on first switch)
\t\t// We don't initialize it here to avoid creating DX device when not needed
\t\tm_NVRHIContext = nullptr;
\t}

\tvoid WindowsWindow::SetupCallbacks()
\t{
\t\tglfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
\t\t\t\t\t\t\t\t  {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
\t\t\t\tdata.Width = width;
\t\t\t\tdata.Height = height;

\t\t\t\tWindowResizeEvent event(width, height);
\t\t\t\tWL_CORE_WARN("{0}, {1}", width, height);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
\t\t\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
\t\t\t\tWindowCloseEvent event;
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tswitch (action)
\t\t\t\t{
\t\t\t\t\tcase GLFW_PRESS:
\t\t\t\t\t{
\t\t\t\t\t\tKeyPressedEvent event(key, 0);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_RELEASE:
\t\t\t\t\t{
\t\t\t\t\t\tKeyReleasedEvent event(key);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_REPEAT:
\t\t\t\t\t{
\t\t\t\t\t\tKeyPressedEvent event(key, 1);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t} });

\t\tglfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
\t\t\t\t\t\t\t{
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tKeyTypedEvent event(keycode);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods)
\t\t\t\t\t\t\t\t   {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tswitch (action)
\t\t\t\t{
\t\t\t\t\tcase GLFW_PRESS:
\t\t\t\t\t{
\t\t\t\t\t\tMouseButtonPressedEvent event(button);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t\tcase GLFW_RELEASE:
\t\t\t\t\t{
\t\t\t\t\t\tMouseButtonReleasedEvent event(button);
\t\t\t\t\t\tdata.EventCallback(event);
\t\t\t\t\t\tbreak;
\t\t\t\t\t}
\t\t\t\t} });

\t\tglfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
\t\t\t\t\t\t\t  {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tMouseScrolledEvent event((float)xOffset, (float)yOffset);
\t\t\t\tdata.EventCallback(event); });

\t\tglfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
\t\t\t\t\t\t\t\t {
\t\t\t\tWindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

\t\t\t\tMouseMovedEvent event((float)xPos, (float)yPos);
\t\t\t\tdata.EventCallback(event); });
\t}"""

if old_create in text:
    text = text.replace(old_create, new_create)
    print("patched CreateContexts")
else:
    print("CreateContexts not found")

# Now patch SwitchRendererAPI to handle window recreation
old_switch = """\tvoid WindowsWindow::SwitchRendererAPI(RendererAPI::API api)
\t{
\t\tWL_PROFILE_FUNCTION();

\t\tif (api == m_CurrentAPI && m_CurrentContext)
\t\t\treturn;

\t\tWL_CORE_INFO("Switching renderer API to {0}", (int)api);"""

new_switch = """\tvoid WindowsWindow::SwitchRendererAPI(RendererAPI::API api)
\t{
\t\tWL_PROFILE_FUNCTION();

\t\tif (api == m_CurrentAPI && m_CurrentContext)
\t\t\treturn;

\t\tWL_CORE_INFO("Switching renderer API to {0}", (int)api);

\t\t// Check if window needs recreation due to GLFW_CLIENT_API mismatch
\t\t// OpenGL requires OPENGL_API, Vulkan/DX require NO_API
\t\tbool isNewNoAPI = (api == RendererAPI::API::NVRHI_Vulkan ||
\t\t                   api == RendererAPI::API::NVRHI_DX11 ||
\t\t                   api == RendererAPI::API::NVRHI_DX12);
\t\tbool isOldNoAPI = (m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan ||
\t\t                   m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
\t\t                   m_CurrentAPI == RendererAPI::API::NVRHI_DX12);
\t\tbool needWindowRecreate = (isNewNoAPI != isOldNoAPI) && m_Window != nullptr;

\t\tif (needWindowRecreate)
\t\t{
\t\t\tWL_CORE_INFO("Recreating GLFW window for API switch (GL <-> NO_API)");
\t\t\t// Save window properties
\t\t\tWindowProps props(m_Data.Title, m_Data.Width, m_Data.Height);

\t\t\t// Clear NVRHI cached resources before destroying contexts
\t\t\tif (m_CurrentAPI == RendererAPI::API::NVRHI_DX11 ||
\t\t\t    m_CurrentAPI == RendererAPI::API::NVRHI_DX12 ||
\t\t\t    m_CurrentAPI == RendererAPI::API::NVRHI_Vulkan)
\t\t\t{
\t\t\t\tNVRHIRendererAPI* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(RenderCommand::GetRendererAPI());
\t\t\t\tif (nvrhiAPI)
\t\t\t\t\tnvrhiAPI->ClearCachedResources();
\t\t\t}

\t\t\t// Shutdown ImGui backend before window destruction (it holds GLFW window references)
\t\t\tif (Application::IsInitialized())
\t\t\t{
\t\t\t\tImGuiLayer* imguiLayer = Application::Get().GetImGuiLayer();
\t\t\t\tif (imguiLayer)
\t\t\t\t\timguiLayer->ShutdownBackendForWindowRecreate();
\t\t\t}

\t\t\t// Destroy contexts that are tied to old window
\t\t\tDestroyContexts();
\t\t\tglfwDestroyWindow(m_Window);
\t\t\tm_Window = nullptr;

\t\t\t// Set hints for new API
\t\t\tglfwDefaultWindowHints();
\t\t\tif (isNewNoAPI)
\t\t\t{
\t\t\t\tglfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tglfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
\t\t\t\tglfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
\t\t\t\tglfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
\t\t\t\tglfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
\t\t\t}
\t\t\tglfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
\t\t\tglfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

\t\t\tm_Window = glfwCreateWindow((int)props.Width, (int)props.Height, props.Title.c_str(), nullptr, nullptr);
\t\t\tWL_CORE_ASSERT(m_Window, "Failed to recreate GLFW window for API switch!");

\t\t\t// Recreate contexts for new window
\t\t\tif (api == RendererAPI::API::OpenGL)
\t\t\t{
\t\t\t\tm_OpenGLContext = new OpenGLContext(m_Window);
\t\t\t\tm_OpenGLContext->Init();
\t\t\t\tm_NVRHIContext = nullptr;
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tm_OpenGLContext = nullptr;
\t\t\t\tm_NVRHIContext = nullptr; // will be created below
\t\t\t}

\t\t\tm_Data.Width = props.Width;
\t\t\tm_Data.Height = props.Height;
\t\t\tglfwSetWindowUserPointer(m_Window, &m_Data);
\t\t\tSetVSync(true);
\t\t\tSetupCallbacks();

\t\t\t// Reset current context tracking so the switch below creates the right context
\t\t\tm_CurrentContext = nullptr;
\t\t\tm_CurrentAPI = RendererAPI::API::None;
\t\t}"""

if old_switch in text:
    text = text.replace(old_switch, new_switch)
    print("patched Switch")
else:
    print("Switch not found")

path.write_text(text, encoding='utf-8')
print("done")
