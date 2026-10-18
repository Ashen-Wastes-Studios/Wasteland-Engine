// Header guard. One of the per-platform Window headers; each declares the
        // same LinuxWindow/WindowsWindow/MacWindow shape behind the
        // backend-agnostic Window base.
#pragma once

// The base class and, through it, WindowProps, EventCallbackFn and the
        // Event types. Also brings <string>, which m_Data.Title below needs
        // without a local include.
#include "Wasteland/Core/Window.h"

// A forward declaration in the GLOBAL namespace, which is where GLFW
        // actually declares it -- the real definition lives in glfw3.h. It
// is all this header needs: the pointer is stored and passed, never
// dereferenced. Keeping GLFW out of this header means a consumer
// does not need GLFW's include path.
struct GLFWwindow;
// Same pattern for the graphics context. Forward-declared rather than
        // included so that selecting a platform does not drag in Vulkan,
// DX12 or GL headers -- the .cpp decides which context type to build.
class GraphicsContext;

namespace Wasteland
{

    // A concrete backend implementation of the abstract Window interface. The
    // application code above it is written against the base and never
    // names this type, which is what makes the platform swappable.
    class LinuxWindow : public Window
    {
    public:
        LinuxWindow(const WindowProps &props);
        // DEFECT (non-virtual destructor, documented not fixed): the base Window is
                // an abstract interface, this destructor is declared `virtual` at the
                // DECLARATION but nothing here guarantees the BASE has a virtual
                // destructor. Deleting through a Window* is undefined behaviour unless
                // Window declares one -- which this header cannot show, because
                // Window.h is not in this batch. Worth confirming there; if the base
        // destructor is non-virtual, every Window owner must hold a Ref.
                // Compare NVRHITexture.h, where the destructor is not marked virtual at
                // all.
        virtual ~LinuxWindow();

        // The per-frame pump, and the ONLY overridable that is not a getter or
        // setter: this is where the event queue is drained and the callbacks
        // below are invoked. Declared out of line because it is the platform-
        // specific half.
        void OnUpdate() override;

        // inline in the header because they are one-line field reads on a hot path
        // (the renderer queries size every frame). unsigned int rather than
        // uint32_t: on every target platform here they are the same width, but
        // the base class interface dictates the type, not this implementation.
        inline unsigned int GetWidth() const override { return m_Data.Width; }
        inline unsigned int GetHeight() const override { return m_Data.Height; }

        // Stores a std::function. Note this is INLINE and does no null check:
        // passing an empty callback is accepted, and the event pump must
        // handle being called with an empty target. Nothing here reports a
        // failure to install a callback.
        inline void SetEventCallback(const EventCallbackFn &callback) override { m_Data.EventCallback = callback; }
        // NOT inline, unlike its neighbours: VSync is a glfwSwapInterval call, so
        // it lives in the .cpp where GLFW is actually included.
        void SetVSync(bool enabled) override;
        bool IsVSync() const override;

        // `virtual ... override` together: redundant (override implies virtual) but
        // harmless, and consistent with the rest of the block.
        // Returns void* -- the same untyped escape hatch used across the
        // renderer headers. The caller must know this is a GLFWwindow*; on
        // other platforms the same call returns a different native type.
        inline virtual void *GetNativeWindow() const override { return m_Window; }

    private:
        // Init/Shutdown split out of the ctor/dtor rather than inlined into them.
        // That is the pattern that lets a subclass initialise differently and
        // keeps the ctor able to do work after the base is constructed --
        // though the `virtual` here only has an effect if the base declares
        // these as virtual, and it is unclear from this header that it does.
        virtual void Init(const WindowProps &props);
        virtual void Shutdown();

    private:
        // DEFECT (uninitialised, documented not fixed): a raw pointer with no
        // in-class initialiser. Between construction and Init() assigning it,
        // this member holds an indeterminate value. It is private and the
        // accessors above are not callable before the ctor completes, so this
        // is narrow -- but if Init throws partway, or a subclass's ctor body
        // calls an accessor before calling Init, it reads garbage.
        // `= nullptr` would cost nothing.
        GLFWwindow *m_Window;
        // Same uninitialised-pointer concern as m_Window above. A context also
        // usually has to be DESTROYED before the window it renders into, so
        // the dtor ordering here matters and is not expressed in this header.
        GraphicsContext *m_Context;

        // A nested struct grouping the plain state. Worth noting WHY: it is what
        // the event system copies around, so a callback receiving WindowData
        // does not need the window object at all.
        struct WindowData
        {
            // Owned by value rather than kept in sync with GLFW's copy, so the window
            // title is a C++-side value that must be re-applied through GLFW after
            // any change. Nothing in this header exposes a SetTitle, so it is set
            // once from WindowProps at Init and then static.
            std::string Title;
            unsigned int Width, Height;
            // DEFECT (uninitialised, documented not fixed): a bool with no in-class
            // initialiser, unlike m_Width/m_Height it is not in a brace list here
            // either. IsVSync() above reads it, so a window constructed without
            // props applying it returns an indeterminate bool.
            bool VSync;

            // Default-constructed, so an EMPTY std::function rather than an undefined
            // one. Calling it would throw std::bad_function_call, so the event pump
            // must test it before invoking -- that check lives in the .cpp.
            EventCallbackFn EventCallback;
        };

        // The only non-pointer member group. Note the two access specifiers in this
        // class: one for the API surface, one for the backend's own helpers,
        // and one again for the data. Three sections for eleven declarations,
        // which is a readability cost rather than a defect.
        WindowData m_Data;
    };

}
