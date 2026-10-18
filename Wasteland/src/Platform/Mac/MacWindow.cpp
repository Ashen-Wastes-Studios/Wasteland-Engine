#include "wlpch.h"
#include "MacWindow.h"

// Platform gate. On a non-Mac build this file reduces to just the two
// includes above, so Window::Create on line 24 is not compiled at all.
// Note there is no #else providing a fallback -- a build that defines
// neither WL_PLATFORM_MACOS nor another backend has no Window::Create
// definition at all and would fail at link time.
#if defined(WL_PLATFORM_MACOS)

// The three event headers are all needed by the GLFW callbacks in Init
// below: keys, mouse and window/application events.
// Angle brackets because they resolve from the include path, not from
// this file's directory (Platform/Mac/).
#include <Wasteland/Events/KeyEvent.h>
#include <Wasteland/Events/MouseEvent.h>
#include <Wasteland/Events/ApplicationEvent.h>

// Quoted form, resolved from the include path.
// Supplies OpenGLContext, the m_Context member the class creates on
// line 98 and swaps on line 200.
// Unlike WindowsWindow.h (which holds a GraphicsContext* plus separate
// slots for an OpenGL and an NVRHI context), this backend has a single
// typed context member -- so the Mac path has no runtime backend
// switching.
#include "Platform/OpenGL/OpenGLContext.h"

// The GL loader. Needed for the GL_* state that OpenGLContext sets up.
// Separate from GLFW (line 4's transitive include): GLFW is the window
// system, glad is the function loader.
#include <glad/glad.h>

namespace Wasteland
{

    // file-static, so the flag is private to this translation unit.
    // GLFW initialisation is process-global even though the flag is TU-
    // local: a second window created through a DIFFERENT backend .cpp would
    // not see this flag and would call glfwInit again.
    static bool s_GLFWInitialized = false;

    // Routed to the engine logger so a GLFW failure appears in the log
    // rather than going to stderr, where it would be lost.
    static void GLFWErrorCallback(int error, const char *description)
    {
        // ERROR level, so a GLFW failure counts as a core error. Note the
        // callback is installed on line 87 AFTER glfwInit on line 86, so any
        // error raised by the init call itself is NOT captured here -- GLFW
        // still uses its default behaviour for that one call.
        WL_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    // The platform factory. Out-of-class definition of a static member of
    // the base class, so this is where the choice of backend is made.
    // Each platform backend defines its own Window::Create, and exactly one
    // of those translation units is compiled per build.
    Window *Window::Create(const WindowProps &props)
    {
        // Returns a raw pointer, so the caller owns the Window.
        // DEFECT (leak): no matching delete anywhere in this file. The
        // destructor on line 36 runs Shutdown(), so the class DOES expect to
        // be destroyed -- but nothing here does it. Ownership is presumably
        // handled by the Application, which cannot be confirmed from this file.
        return new MacWindow(props);
    }

    // The initialiser list is EMPTY -- unlike OpenGLTexture2D, which
    // initialises its width/height in the list. Here everything is assigned
    // inside Init.
    MacWindow::MacWindow(const WindowProps &props)
    {
        WL_PROFILE_FUNCTION();

        // The entire constructor body. Init is virtual in the header, so
        // calling it from here does not dispatch polymorphically -- this is a
        // base-class call regardless of the actual runtime type.
        Init(props);
    }

    // Non-virtual is safe HERE only because Window::Create (line 24) never
    // hands back a derived type and nothing else derives from MacWindow.
    // If a subclass ever appeared, deleting it through a Window* would
    // invoke this destructor and skip the subclass's.
    MacWindow::~MacWindow()
    {
        WL_PROFILE_FUNCTION();

        // Destructor delegates to the same Shutdown() Init's counterpart
        // lives in, so setup and teardown are defined in one place each.
        Shutdown();
    }


    // WindowProps width/height of 0 means "use the primary monitor".
    // Queried here rather than in Application because GLFW must already be
    // initialised, and the platform windows are what initialise it.
    // file-static free function, not a member: it needs no window state.
    // References rather than a returned pair, because the function only
    // overwrites the dimensions it could resolve -- a partial success leaves
    // the caller's other value untouched.
    static void ResolveMonitorSize(uint32_t &width, uint32_t &height)
    {
        // The primary (usually built-in) display, as opposed to whatever
        // monitor the window happens to be on.
        // Requires GLFW to be initialised, which is why the comment above this
        // function says it cannot live in Application.
        // Returns null when GLFW is not initialised or there is no display.
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor)
        {
            // The current resolution and refresh rate for that monitor.
            // The GLFWvidmode* is OWNED by GLFW, so this const pointer is valid only
            // until the mode changes -- which is why the width/height are copied out
            // immediately rather than stored.
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            // Triple-guarded: the mode pointer can be null, and a mode can
            // legitimately report 0 dimensions on some headless configurations.
            // The > 0 tests are what keep a 0x0 window from being created.
            if (mode && mode->width > 0 && mode->height > 0)
            {
                // int -> uint32_t. The mode reports int dimensions; a negative width
                // is excluded by the guard above, so the cast cannot wrap.
                width = static_cast<uint32_t>(mode->width);
                height = static_cast<uint32_t>(mode->height);
                return;
            }
        }
        // No display (headless/CI), or GLFW could not name one: keep the
        // historical default rather than creating a zero-sized window, which
        // is invalid and would fail an assert later.
        // Only fills in a dimension the caller left at 0, and only in the
        // fallback path. So "use my monitor" (0, 0) becomes (1280, 720), while
        // an explicit (0, 720) becomes (1280, 720) -- the caller's height wins
        // and the width is filled. That asymmetry is the point of the two
        // separate ifs rather than a single either-branch.
        if (width == 0)
            width = 1280;
        if (height == 0)
            height = 720;
    }

    // The whole window setup: GLFW, the context, and eight callbacks.
    // virtual in the header, but see the note on line 33 -- calling it from
    // the constructor does not dispatch dynamically.
    void MacWindow::Init(const WindowProps &props)
    {
        WL_PROFILE_FUNCTION();

        // Copied into the cached WindowData rather than kept as a reference.
        // Everything the GLFW callbacks need travels through m_Data, because
        // GLFW's C callbacks have no user-data parameter except the single
        // void* registered on line 101.
        m_Data.Title = props.Title;
        m_Data.Width = props.Width;
        m_Data.Height = props.Height;

        // 0 means "the user's monitor".
        // An OR, so a partially-specified size still triggers resolution. The
        // comment above the function describes the intent: 0 means "the user's
        // monitor".
        // DEFECT (minor, log inconsistency): the log on line 81 prints
        // props.Width/props.Height, not the RESOLVED m_Data values. A window
        // created at the default size logs "Creating window X (0, 0)" even
        // though it was made at whatever ResolveMonitorSize returned.
        if (m_Data.Width == 0 || m_Data.Height == 0)
            ResolveMonitorSize(m_Data.Width, m_Data.Height);

        // INFO level for a routine event. Note the {0}/{1} placeholders and
        // the comma-separated arguments -- this is a {}-style formatter, not
        // printf, so %d/%s would print literally.
        WL_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

        // Init-once guard. GLFW initialisation is not reference counted, so a
        // second window in the same process must not re-enter this.
        if (!s_GLFWInitialized)
        {
            WL_PROFILE_SCOPE("glfwInit");
            int success = glfwInit();
            // Installed AFTER glfwInit (line 86), so init's own errors bypass it
            // -- see the note on the callback above.
            // GLFW keeps only ONE global error callback, so a second MacWindow
            // would silently replace it (with an identical function, harmlessly).
            glfwSetErrorCallback(GLFWErrorCallback);
            WL_CORE_ASSERT(success, "Could not initialize GLFW!");

            s_GLFWInitialized = true;
        }

        {
            WL_PROFILE_SCOPE("glfwCreateWindow");
            // The two trailing nullptrs are "no monitor, no share": the window is
            // not fullscreen and shares no GL objects with another context.
            // DEFECT: the result is never null-checked. glfwCreateWindow returns
            // null on failure (out of memory, or an invalid size/hint combination),
            // and everything after this line -- the context on 98, the user pointer
            // on 101, eight callback registrations -- then dereferences or passes
            // that null. This is the most serious omission in the file.
            // The (int) casts are narrowing from uint32_t; a width above INT_MAX
            // would wrap to a negative size and fail.
            m_Window = glfwCreateWindow((int)m_Data.Width, (int)m_Data.Height, m_Data.Title.c_str(), nullptr, nullptr);
        }

        // Raw new, matching the member type.
        // DEFECT (leak): there is no `delete m_Context` in Shutdown() (lines
        // 188-193). The context object leaks even though the GLFW window it wraps
        // is destroyed. Its destructor presumably needs the window still alive,
        // which is likely why it is skipped -- but the leak is real.
        m_Context = new OpenGLContext(m_Window);
        m_Context->Init();

        // The single per-window void* that makes the C callbacks below able
        // to reach C++ state. Every lambda re-derives it with
        // glfwGetWindowUserPointer.
        // Points at m_Data, a MEMBER of this object, so it is only valid while
        // the MacWindow lives -- which matches the window's lifetime.
        glfwSetWindowUserPointer(m_Window, &m_Data);
        // VSync on by default, set through the public setter rather than by
        // calling glfwSwapInterval directly, so m_Data.VSync is kept in step.
        // Must come after the context exists: glfwSwapInterval operates on the
        // current context's swap chain.
        SetVSync(true);

        // All eight callbacks below are captureless lambdas, which makes them
        // convert to plain function pointers for GLFW's C API. They reach state
        // exclusively through the user pointer set on line 101.
        // DEFECT: NONE of them null-check glfwGetWindowUserPointer. If GLFW ever
        // invokes a callback for a window whose user pointer is unset, every
        // lambda dereferences null.
        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow *window, int width, int height)
                                  {
                // A C-style cast back to the type line 101 stored. The `window`
                // parameter is used here -- GLFW hands it back so the callback works for
                // whichever window fired, which is what lets one lambda serve all of them.
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
                data.Width = width;
                data.Height = height;

                // Constructed on the stack and passed by value/reference to the
                // callback below.
                // NOTE: width/height are ints here, whereas the event takes whatever
                // type it declares -- this is the only place the window size is recorded
                // into m_Data, and it is stored into unsigned int fields, so a negative
                // size would wrap.
                WindowResizeEvent event(width, height);
                // DEFECT: left-over debug logging at WARN level. Every single window
                // resize logs a warning, so dragging a window edge produces a stream of
                // warnings in normal use. The message has no context either -- no
                // window name, no old/new dimensions -- so it is not even useful for
                // diagnosis. This looks like a debugging leftover.
                WL_CORE_WARN("{0}, {1}", width, height);
                data.EventCallback(event); });

        // Close request. GLFW does NOT destroy the window itself -- it
        // reports the request and leaves the decision to the app, which is why
        // there is no destroy call in this lambda.
        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window)
                                   {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
                WindowCloseEvent event;
                data.EventCallback(event); });

        // Four parameters beyond the window. DEFECT: `scancode` and `mods` are
        // both named and then unused -- the engine maps only the GLFW key code,
        // so hardware-independent scancodes and the modifier-key state are
        // discarded. A Ctrl+C therefore arrives as an unmodified C.
        glfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode, int action, int mods)
                           {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                // GLFW_PRESS / GLFW_RELEASE / GLFW_REPEAT. No default case, and no
                // action value is rejected -- an unexpected action is silently dropped.
                // DEFECT: with -Wswitch-enum or /W4 this is a missing-enumeration
                // warning; the more substantive point is that GLFW_REPEAT is mapped to
                // a press with a repeat flag rather than being treated distinctly.
                switch (action)
                {
                    case GLFW_PRESS:
                    {
                        // The trailing 0 is the repeat flag: 0 for a genuine press.
                        // So an auto-repeating key produces an indistinguishable-in-kind but
                        // flagged-as-repeat press downstream.
                        KeyPressedEvent event(key, 0);
                        data.EventCallback(event);
                        break;
                    }
                    case GLFW_RELEASE:
                    {
                        // No repeat parameter -- a release is a release.
                        KeyReleasedEvent event(key);
                        data.EventCallback(event);
                        break;
                    }
                    case GLFW_REPEAT:
                    {
                        // Same event type as line 128 with the repeat flag set. The pair of
                        // these two is what lets a caller tell a real press from an auto-repeat.
                        KeyPressedEvent event(key, 1);
                        data.EventCallback(event);
                        break;
                    }
                } });

        // Character input, distinct from key input: this fires with the
        // TEXT a key produced, after the keyboard layout has been applied.
        // A text field needs this; an action binding needs the key callback
        // above. Having both is correct, not redundant.
        glfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode)
                            {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                KeyTypedEvent event(keycode);
                data.EventCallback(event); });

        // `mods` is named and unused, the same modifier loss as the key
        // callback. Ctrl-click is therefore indistinguishable from a plain click.
        // Also absent: any double-click or scroll-state tracking.
        glfwSetMouseButtonCallback(m_Window, [](GLFWwindow *window, int button, int action, int mods)
                                   {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                // Only PRESS and RELEASE are handled. Unlike the key callback there
                // is no REPEAT case, which is correct -- GLFW does not repeat mouse
                // buttons.
                // Still no default case.
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

        // Scroll offsets are per-axis DELTAS, not positions. GLFW gives
        // doubles; the event takes floats, so the narrowing cast is deliberate.
        // There is no accumulation into a position anywhere in this file, so a
        // zoom control driven by this must integrate the deltas itself.
        glfwSetScrollCallback(m_Window, [](GLFWwindow *window, double xOffset, double yOffset)
                              {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                MouseScrolledEvent event((float)xOffset, (float)yOffset);
                data.EventCallback(event); });

        // Positions, not deltas -- the same information Input::GetMousePosition
        // polls on demand. Registering this pushes it as an event instead.
        glfwSetCursorPosCallback(m_Window, [](GLFWwindow *window, double xPos, double yPos)
                                 {
                WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

                MouseMovedEvent event((float)xPos, (float)yPos);
                data.EventCallback(event); });
    }

    // Teardown. Three lines, and each one is a gap.
    void MacWindow::Shutdown()
    {
        WL_PROFILE_FUNCTION();

        // Destroys the window and, with it, the GL context created on top of
        // it.
        // DEFECT (three, in one function):
        //   1. m_Context is never deleted (see line 98) -- it leaks.
        //   2. glfwTerminate() is never called, so GLFW itself stays initialised
        //      for the process lifetime and s_GLFWInitialized stays true.
        //   3. m_Window is not set to null, so a use after Shutdown passes a
        //      destroyed GLFWwindow* to any further call.
        // The missing context delete is probably deliberate -- OpenGLContext
        // would want the window alive in its destructor, and the window dies
        // first here -- but that is exactly the ordering constraint the missing
        // delete should be documented against.
        glfwDestroyWindow(m_Window);
    }

    // The frame pump, called once per frame.
    void MacWindow::OnUpdate()
    {
        WL_PROFILE_FUNCTION();

        // Processes the event queue and invokes the callbacks registered in
        // Init. Note: POLL, not glfwWaitEventsTimeout, so this busy-spins when
        // there is nothing to do rather than blocking.
        // This is what turns native events into the Event objects the engine
        // consumes, so it must run before any renderer reads window state.
        glfwPollEvents();
        // Presents the frame. Must be last in the frame, and must happen on
        // the thread that owns the context.
        // DEFECT: m_Context is dereferenced without a null check, so a
        // glfwCreateWindow failure on line 95 -- which was itself not checked --
        // makes this a null dereference on the very first frame.
        m_Context->SwapBuffers();
    }

    // The setter from the header's inline block, defined out of line here
    // because it touches GLFW.
    void MacWindow::SetVSync(bool enabled)
    {
        WL_PROFILE_FUNCTION();

        if (enabled)
            // 1 = synchronise buffer swaps to the display refresh. The value is a
            // boolean-ish count of vsyncs to wait for, not a divisor.
            // Must be called with the correct context current, so it only has an
            // effect when this is invoked after m_Context->Init().
            glfwSwapInterval(1);
        else
            glfwSwapInterval(0);

        // Caches the intent rather than asking GLFW, so IsVSync() below is a
        // pure member read with no driver round trip.
        // DEFECT (minor): if glfwSwapInterval fails (no VSync available on the
        // current context), this still records the requested value, so IsVSync()
        // can report true when VSync is not actually in effect. The return value
        // of glfwSwapInterval is not checked.
        m_Data.VSync = enabled;
    }

    // const and trivial because of the cache written on line 212.
    bool MacWindow::IsVSync() const
    {
        // Reads the cached flag, never querying GLFW. Contrast with
        // WindowsWindow::IsMouseCaptured, whose header comment explains why a
        // cached value is also the right choice there -- the same reasoning
        // applies here but is not written down in this file.
        return m_Data.VSync;
    }

}

// Named #endif: the guard on line 4 is identified at its close, which
// the WindowsInput.cpp guard is not.
#endif // WL_PLATFORM_MACOS
