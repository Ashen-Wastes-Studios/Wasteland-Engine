// Precompiled header first, by engine convention.
#include "wlpch.h"
// This class's own declaration. Included second so wlpch.h has already
// supplied the GL and GLFW types its members reference.
#include "LinuxWindow.h"

// Platform guard. WL_PLATFORM_LINUX is defined by Core.h on this target, so
// the whole body below compiles to nothing elsewhere -- which is what lets
// this file exist in a cross-platform tree without an #ifdef per symbol.
#if defined(WL_PLATFORM_LINUX)

// The three event headers are needed because the GLFW callbacks below
// construct KeyPressedEvent, KeyReleasedEvent, KeyTypedEvent,
// WindowResizeEvent, WindowCloseEvent, and the mouse events.

// Angle-bracket includes for engine headers -- a deliberate inconsistency
// with the quoted platform includes immediately below.
#include <Wasteland/Events/KeyEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/ApplicationEvent.h>

// The GL context object. Created in Init() and used to swap buffers each
// frame in OnUpdate(), which is where the per-frame presentation cost lands.
#include "Platform/OpenGL/OpenGLContext.h"

#include <glad/glad.h>

namespace Wasteland
{

    // File-scope guard so glfwInit() runs once no matter how many windows are
    // created.

    // DEFECT: a plain `bool` with no synchronisation. GLFW itself requires
    // glfwInit/terminate to be called from the main thread, and today every
    // window is built on that thread during startup, so this is safe as written.
    // It stops being safe the moment window creation moves off-thread, and there
    // is nothing in the type that would say so.

    // Also note the asymmetry: this is set to true in Init() but never set back
    // to false in Shutdown(). GLFW is never terminated by this class at all, so
    // the process keeps its GLFW state for its lifetime -- see the note at
    // Shutdown() below.
    static bool s_GLFWInitialized = false;

    // Installed as GLFW's global error handler during Init. Static and free
    // rather than a member so it can be passed to glfwSetErrorCallback, which
    // takes a plain function pointer.
    static void GLFWErrorCallback(int error, const char *description)
    {
        // Routes GLFW errors into the engine logger rather than stderr, so they land
        // in the same log as everything else.
        WL_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    // The platform factory. Always returns a LinuxWindow here, unconditionally
    // -- the WL_PLATFORM_LINUX guard above means this function does not exist at
    // all on other targets.

    // Returns a raw `Window*`, not a Ref/Scope. That is deliberate at the
    // factory level (the caller in Application owns the lifetime) but means
    // nothing here prevents a leak if the caller forgets.
    Window *Window::Create(const WindowProps &props)
    {
        // Raw new, handed straight to the caller. Compare Framebuffer::Create and
        // Shader::Create, which both return Ref<>. The asymmetry means ownership of
        // a Window is established entirely by convention at the call site.
        return new LinuxWindow(props);
    }

    // All work is delegated to Init() rather than done in the body, so there is
    // exactly one place window setup happens.
    LinuxWindow::LinuxWindow(const WindowProps &props)
    {
        WL_PROFILE_FUNCTION();

        // By value/reference pass-through; the props are copied into m_Data inside,
        // so the caller's WindowProps is not aliased.
        Init(props);
    }

    // Delegates teardown to Shutdown(), mirroring the constructor.
    LinuxWindow::~LinuxWindow()
    {
        WL_PROFILE_FUNCTION();

        // Note: Shutdown() destroys the window but never deletes m_Context and never
        // calls glfwTerminate. Both are leaks on the shutdown path. Whether that
        // matters depends on the process exiting immediately afterwards, which is the
        // usual case for an engine.
        Shutdown();
    }


    // WindowProps width/height of 0 means "use the primary monitor".
    // Queried here rather than in Application because GLFW must already be
    // initialised, and the platform windows are what initialise it.
    // Resolves WindowProps 0x0 into the primary monitor's current mode size.

    // Out-parameters by reference, so the caller's existing values are both read
    // and conditionally overwritten -- see the fallback below, which only
    // substitutes when a dimension is still 0.

    // Lives here rather than in Application because GLFW must already be
    // initialised, and this class is what initialises it.
    static void ResolveMonitorSize(uint32_t &width, uint32_t &height)
    {
        // The primary monitor, which under X11/Wayland is the desktop's main output.
        // Can return null on a headless machine, hence the null check below.
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor)
        {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            // Guards against a null mode pointer AND against a degenerate 0x0 mode. Both
            // checks are needed: a zero-sized window is rejected by GLFW and, as the
            // comment below notes, fails an assert later.
            if (mode && mode->width > 0 && mode->height > 0)
            {
                // Explicit cast from GLFW's int. mode->width is positive at this point (
                // checked above), so the narrowing to uint32_t cannot lose data.
                width = static_cast<uint32_t>(mode->width);
                height = static_cast<uint32_t>(mode->height);
                return;
            }
        }
        // No display (headless/CI), or GLFW could not name one: keep the
        // historical default rather than creating a zero-sized window, which
        // is invalid and would fail an assert later.
        // Fallback: headless or no monitor. 1280x720 rather than 0x0, because a
        // zero-sized window is invalid.

        // The two ifs are separate rather than one combined test, so a caller
        // supplying e.g. 1920x0 gets the monitor's height substituted for 0 while
        // keeping 1920 -- each dimension is resolved independently.
        if (width == 0)
            width = 1280;
        if (height == 0)
            height = 720;
    }

    // The whole window bring-up: store properties, resolve 0x0, init GLFW once,
    // create the window, create the GL context, install every callback.

    // One long function with no error handling on the GLFW calls:
    // glfwCreateWindow's result is assigned to m_Window without a null check, so
    // a failure (bad size, no display) leaves m_Window null and every
    // glfwSet*Callback call below then operates on a null window.
    void LinuxWindow::Init(const WindowProps &props)
    {
        WL_PROFILE_FUNCTION();

        // Copying the title into m_Data, which is also what gets published as the
        // GLFW window user pointer a few lines below. That pointer is the only route
        // the C callbacks have back to engine state.
        m_Data.Title = props.Title;
        m_Data.Width = props.Width;
        m_Data.Height = props.Height;

        // 0 means "the user's monitor".
        // Either dimension being 0 means "use the monitor", so the guard is an OR and
        // the resolver handles each dimension separately.
        if (m_Data.Width == 0 || m_Data.Height == 0)
            // Mutates m_Data in place through the reference parameters.

            // The WL_CORE_INFO on the next line logs props.Width/props.Height, not
            // m_Data -- so a 0x0 request is logged as "0, 0" rather than the resolved
            // size. Confusing, and the resolved values are the interesting ones.
            ResolveMonitorSize(m_Data.Width, m_Data.Height);

        // Logs the REQUESTED size. DEFECT (cosmetic): as noted above, this should
        // log m_Data.Width/m_Data.Height to show the size actually used.
        WL_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

        // One-time library init. Deliberately not guarded by a "did a previous window
        // fail" check -- the first window does the work and later windows skip it.
        if (!s_GLFWInitialized)
        {
            WL_PROFILE_SCOPE("glfwInit");
            // GLFW library init. The return value is captured and asserted below rather
            // than checked immediately, so the error callback installed on the NEXT line
            // is not yet registered when glfwInit fails -- meaning an init failure logs
            // through GLFW's default handler, not the engine logger.
            int success = glfwInit();
            // Installed after glfwInit, per the note above. From here on every GLFW error
            // goes to the engine log.
            glfwSetErrorCallback(GLFWErrorCallback);
            // Debug-only. In release the macro vanishes entirely (Core.h), so a failed
            // glfwInit leaves success == 0 and execution continues straight into
            // glfwCreateWindow -- which will then fail too, with no diagnostic from here.
            // This is the same release-build assert gap noted elsewhere in the batch.
            WL_CORE_ASSERT(success, "Could not initialize GLFW!");

            s_GLFWInitialized = true;
        }

        // A bare scope block, used purely so WL_PROFILE_SCOPE's timer is destroyed at
        // the closing brace. It makes the profiled region exactly the GLFW call and
        // nothing else -- a readability device, not a control-flow construct.
        {
            WL_PROFILE_SCOPE("glfwCreateWindow");
            // The three nullptrs: no monitor (share screen), no share context (no
            // resource sharing with another window).

            // Width and height are cast to int for GLFW even though m_Data holds
            // uint32_t. A dimension above INT_MAX would become negative and GLFW would
            // reject it -- unreachable in practice for a window.

            // DEFECT: the return value is not null-checked. Every glfwSet*Callback call
            // below would then dereference null.
            m_Window = glfwCreateWindow((int)m_Data.Width, (int)m_Data.Height, m_Data.Title.c_str(), nullptr, nullptr);
        }

        // The GL context wraps the GLFW window rather than replacing it -- GLFW owns
        // the window handle, this owns the GL objects.

        // Raw new again, and nothing deletes it. See the destructor note.
        m_Context = new OpenGLContext(m_Window);
        // Makes this context current. No return check, so a context-creation failure
        // is invisible until the first draw fails.
        m_Context->Init();

        // The bridge to the C callbacks below. GLFW stores this opaque pointer per
        // window and hands it back via glfwGetWindowUserPointer; every lambda
        // recovers m_Data from it.

        // This is why m_Data must outlive the window and why its address is stable
        // -- it is a member of *this, not a local.
        glfwSetWindowUserPointer(m_Window, &m_Data);
        SetVSync(true);

        // Non-capturing lambdas converted to C function pointers. They cannot touch
        // `this`, which is precisely why m_Data is reached through the GLFW user
        // pointer instead.
        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
                                  {
                // Recovers engine state from GLFW. This cast-and-dereference is repeated
                // verbatim in all seven callbacks below with no null check -- if the user
                // pointer were never set (or were cleared by GLFW during window recreation),
                // every one of these would dereference null.
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
                data.Width = width;
                data.Height = height;

                // The engine's own event type, forwarded so application code reacts to
                // resizes through the normal event path rather than by polling.
                WindowResizeEvent event(width, height);
                // DEFECT: a leftover debug trace. Every window resize logs at WARN severity
                // with a bare "1920, 1080" and no context. A user dragging a window edge
                // floods the log at warning level and buries real warnings.
                WL_CORE_WARN("{0}, {1}", width, height);
                // Invoke, then close the lambda. This trailing `});` shape is how each
                // callback registration ends.
                data.EventCallback(event); });

        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
                                   {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
                WindowCloseEvent event;
                data.EventCallback(event); });

        // Keyboard callback. `scancode` and `mods` are received and ignored -- no
        // modifier-aware key bindings and no physical-vs-virtual key distinction, so
        // bindings are layout-dependent.
        glfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
                           {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                switch (action)
                {
                    case GLFW_PRESS:
                    {
                        // The second argument is the repeat count. A fresh press reports 0 here.
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
                    // OS key-repeat is synthesised into a second KeyPressedEvent with a repeat
                    // count of 1.

                    // DEFECT (minor): a count of 1 cannot distinguish the first repeat from the
                    // tenth. Text input driven by key-repeat therefore needs to count events
                    // itself rather than reading the repeat field. Using the literal 1 also
                    // means "was this a repeat" and "how many repeats" are conflated in one
                    // small integer.
                    case GLFW_REPEAT:
                    {
                        KeyPressedEvent event(key, 1);
                        data.EventCallback(event);
                        break;
                    }
                } });

        // Character (text) input, kept separate from the key callback on purpose:
        // this delivers the character the keyboard layout produced, which is what a
        // text field wants, while the key callback delivers the physical key.
        glfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
                            {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                KeyTypedEvent event(keycode);
                data.EventCallback(event); });

        // Mouse buttons. `mods` ignored. No double-click detection: every press
        // raises an identical MouseButtonPressedEvent.

        // Note GLFW_PRESS/GLFW_RELEASE are the only two actions for this callback,
        // unlike the key callback which also has GLFW_REPEAT.
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

        // Scroll wheel / trackpad. GLFW reports doubles and the cast to float below
        // discards precision for the high-resolution scrolling devices GLFW supports.
        glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
                              {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                // The narrowing double->float matches MouseScrolledEvent's float parameters.
                MouseScrolledEvent event((float)xOffset, (float)yOffset);
                data.EventCallback(event); });

        // Cursor movement. This fires on every motion event, so it is the highest-
        // frequency callback installed here.
        glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
                                 {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                MouseMovedEvent event((float)xPos, (float)yPos);
                data.EventCallback(event); });
    }

    // Destroys the GLFW window. That is all it does.
    void LinuxWindow::Shutdown()
    {
        WL_PROFILE_FUNCTION();

        // DEFECT: no glfwTerminate(), so the library initialised in Init() is never
        // torn down, and no delete of m_Context, so the OpenGLContext is leaked. Both
        // are only harmless if the process is about to exit, which is the normal case
        // but not guaranteed -- an editor or test host that creates and destroys
        // windows repeatedly would accumulate GL contexts.

        // Also unverified: m_Window is not null-checked, so double-shutdown faults.
        glfwDestroyWindow(m_Window);
    }

    // One frame of platform work: pump OS events, then present.
    void LinuxWindow::OnUpdate()
    {
        WL_PROFILE_FUNCTION();

        // Event pump. Synchronous: this BLOCKS until at least one event is available
        // or timeout, which on some platforms means the frame rate is capped by the
        // window manager rather than by the render loop. glfwWaitEventsTimeout would
        // be the explicitly-bounded alternative.
        glfwPollEvents();
        // Presents the back buffer. Unchecked null, and the return value (buffer
        // swap status) is discarded.
        m_Context->SwapBuffers();
    }

    // Vsync is a bool here but an integer interval in GLFW: true becomes 1,
    // false becomes 0. There is no way to express a fractional or multi-vsync
    // interval through this API.
    void LinuxWindow::SetVSync(bool enabled)
    {
        WL_PROFILE_FUNCTION();

        // Split rather than passing the bool through: glfwSwapInterval takes an int,
        // and GLFW treats 0 as "disable" and any positive value as a frame interval.
        if (enabled)
            // Sync to one refresh. Under a compositor this is usually ignored in favour
            // of the compositor's own policy, so this is not a guarantee of 60 Hz.
            glfwSwapInterval(1);
        else
            glfwSwapInterval(0);

        // Caches the setting so IsVSync() can report it without a GLFW query. Note
        // this records what was REQUESTED, which may differ from what the driver
        // actually applied.
        m_Data.VSync = enabled;
    }

    // Reads the cached value, not GLFW. Cheap, but see the note above: it can
    // disagree with reality.
    bool LinuxWindow::IsVSync() const
    {
        // The cached bool set by SetVSync.
        return m_Data.VSync;
    }

}

#endif // WL_PLATFORM_LINUX
