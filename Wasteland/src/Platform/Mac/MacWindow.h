#pragma once

// Engine-side base. The platform base classes (GLFW window, graphics
// context) are forward-declared below rather than included, which is
// why this header can be parsed by code that has no GLFW available.
#include "Wasteland/Core/Window.h"

// Forward declaration instead of #include <GLFW/glfw3.h>: an opaque
// pointer only needs the type's name, so the vendor header stays out
// of every translation unit that merely holds a MacWindow.
struct GLFWwindow;
class GraphicsContext;

namespace Wasteland
{

    // Four-space indent throughout this file, unlike the tab-indented
    // renderer sources. Consistent within the platform layer.
    class MacWindow : public Window
    {
    public:
        // Takes props BY VALUE, not by const reference -- WindowProps is a
        // small struct of scalars and a string, and copying lets the
        // constructor store what it needs without the caller keeping it
        // alive. Compare SetEventCallback below, which takes a reference.
        MacWindow(const WindowProps &props);
        // virtual rather than `override`: consistent with the codebase's
        // style, and harmless because the signature does match Window.
        virtual ~MacWindow();

        void OnUpdate() override;

        // inline in the header: these are trivial getters, and defining them
        // here avoids a .cpp dependency for accessors that only read a POD.
        inline unsigned int GetWidth() const override { return m_Data.Width; }
        inline unsigned int GetHeight() const override { return m_Data.Height; }

        // Store-only. The callback is invoked by the GLFW poll loop elsewhere;
        // this setter does not install anything with GLFW, so setting it
        // before the window exists is safe and simply replaces the stored
        // std::function.
        inline void SetEventCallback(const EventCallbackFn &callback) override { m_Data.EventCallback = callback; }
        void SetVSync(bool enabled) override;
        bool IsVSync() const override;

        // The bridge to platform code. Returns the raw GLFWwindow*, which is
        // why GLFWwindow is forward-declared at the top: consumers that cast
        // this need the real definition and must include the GLFW header
        // themselves.
        inline virtual void *GetNativeWindow() const override { return m_Window; }

    private:
        // private, not protected: a MacWindow subclass has no legitimate
        // reason to re-run initialisation, and keeping Init unreachable
        // prevents a double-create of the native window.
        virtual void Init(const WindowProps &props);
        virtual void Shutdown();

    private:
        // NOT initialised to nullptr in the declaration list. The ctor's
        // member-init list sets it, so a successful construction is fine --
        // but if Init() throws or returns early on a GLFW failure, this
        // holds an uninitialised pointer and the destructor's Shutdown() will
        // dereference garbage. Same applies to m_Context below and to the
        // uninitialised Width/Height/VSync scalars at line 39.
        GLFWwindow *m_Window;
        // Owned raw pointer to the graphics context. Not a Ref and not a
        // unique_ptr: Shutdown() must run in a fixed order relative to
        // glfwDestroyWindow, so the lifetime is managed by hand rather than
        // by a smart pointer's scope exit.
        GraphicsContext *m_Context;

        struct WindowData
        {
            // Kept even though only SetEventCallback writes it: GetNativeWindow
            // consumers and the platform code read the title back out of this
            // struct rather than from GLFW.
            std::string Title;
            // DEFECT (uninitialised): Width, Height and VSync have no in-class
            // initialisers, unlike WindowData's other members. They are written
            // only if Init() succeeds, so an early return leaves GetWidth() and
            // GetHeight() returning indeterminate values and IsVSync() returning
            // garbage. In-class defaults (= 0 / = false) would close this.
            unsigned int Width, Height;
            bool VSync;

            // std::function, so it is copyable and can be empty. A window with no
            // callback set is legal; the poll loop must test before invoking.
            EventCallbackFn EventCallback;
        };

        WindowData m_Data;
    };

}
