// Precompiled header first: supplies the STL and project-wide defines.
#include "wlpch.h"
#include "WindowsInput.h"

// Needed for the Application::IsInitialized() / Get() singleton access
// used to reach the one native window in the app.
#include "Wasteland/Core/Application.h"
// The actual platform input API. GLFW is the windowing layer, so all
// five backend hooks below are thin wrappers over glfw* calls.
#include <GLFW/glfw3.h>

namespace Wasteland
{

// Preprocessor gate: this file compiles to nothing on other platforms.
// It sits INSIDE namespace Wasteland (opened line 7) but #if is a
// preprocessor directive, so namespace nesting does not apply to it.
#if defined(WL_PLATFORM_WINDOWS)
	// Static member definition; Input::s_Instance is private, so only this
	// one line can assign it. This is how a backend "claims" the facade.
	// DEFECT: raw new with no matching delete, and the pointer is never
	// replaced, so a second backend would silently leak the first. The
	// process-lifetime allocation is intentional in practice, but nothing
	// here enforces or documents that.
	// Static initialisation order matters: this runs before main(), so it
	// must not touch anything that is itself a static in another TU.
	Input *Input::s_Instance = new WindowsInput();
#endif

	// override of the pure virtual declared in Input.h. Called through
	// Input::IsKeyPressed, never directly -- the base keeps the hooks
	// protected.
	bool WindowsInput::IsKeyPressedImpl(int keycode)
	{
		// Guard 1: no Application singleton yet means no window exists to query.
		// Returning false (nothing pressed) is a safe answer; Input::IsKeyPressed
		// itself dereferences s_Instance unguarded, so this check is the only
		// protection against that null.
		if (!Application::IsInitialized())
			return false;

		// Guard 2: the Application exists but its window has not been created,
		// or has already been destroyed. Checked on the raw handle rather than
		// the Window object, so it also covers a torn-down window.
		// Note this guard is repeated as `if (!window)` in the other three
		// hooks below; only this one tests before the cast instead of after.
		if (!Application::Get().GetWindow().GetNativeWindow())
			return false;

		// static_cast, not dynamic_cast: GLFW exposes no RTTI here and the
		// Window is already typed to GLFW by the WindowsWindow backend.
		auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
		// Polls current key state rather than subscribing to callbacks, so the
		// answer is a snapshot at call time. int, not an enum, by GLFW's API.
		// No window->focus check: keys still read as pressed while the window
		// is unfocused, which is a common source of "stuck input" bugs.
		auto state = glfwGetKey(window, keycode);
		// GLFW_REPEAT is the key-repeat event state and counts as pressed, which
		// is what makes holding a key still read true every frame.
		// GLFW_RELEASE and the initial GLFW_RELEASE/idle state both read false,
		// so this is level-triggered exactly as Input.h documents.
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	// Same shape as the key hook, but there is no REPEAT state for mouse
	// buttons, so line 36 only tests GLFW_PRESS.
	bool WindowsInput::IsMouseButtonPressedImpl(int button)
	{
		if (!Application::IsInitialized())
			return false;

		// DEFECT (minor): the cast happens before the null check here, whereas
		// lines 19-20 check the handle first. static_cast of a null pointer is
		// well-defined, so this is a style inconsistency rather than a bug --
		// but it is the same pattern written two different ways in one file.
		auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
		if (!window)
			return false;

		// No GLFW_REPEAT equivalent for buttons, so the only pressed state is
		// GLFW_PRESS.
		auto state = glfwGetMouseButton(window, button);
		return state == GLFW_PRESS;
	}

	// Returns by value in window coordinates. Input.h notes the pair return
	// is deliberate so callers can use structured bindings.
	std::pair<float, float> WindowsInput::GetMousePositionImpl()
	{
		// Guard: Application::Get() is null and the native window is null before the
		// app boots (and after it shuts down). Dereferencing either segfaults.
		// Same guard as line 16, and the comment above this line already
		// documents why it is needed.
		// DEFECT: the comment says GetNativeWindow() is also null before boot,
		// which is right, but it is stated as if the two are simultaneous; the
		// Application check and the window check are independent.
		if (!Application::IsInitialized())
			// Braced return for the std::pair. Reports the window origin rather
			// than signalling failure, so a caller cannot distinguish "no window"
			// from a cursor genuinely at (0,0).
			return {0.0f, 0.0f};

		auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
		if (!window)
			return {0.0f, 0.0f};

		// GLFW reports cursor coordinates as double. Declared uninitialised and
		// written by the call on line 52.
		double xpos, ypos;
		// Out-parameters rather than a return value, hence the locals above.
		// DEFECT: the GLFW return value (GLFW_TRUE on success) is ignored. On
		// failure xpos/ypos are left untouched, so line 54 would return
		// uninitialised doubles narrowed to float. Not fixed here.
		glfwGetCursorPos(window, &xpos, &ypos);

		// Narrowing double -> float. Acceptable for pixel coordinates but it
		// does lose precision on very large or HiDPI-scaled surfaces.
		return {(float)xpos, (float)ypos};
	}

	// Exists so a backend can answer X without building a pair (Input.h
	// line 139-141 explains the separate hook).
	float WindowsInput::GetMouseXImpl()
	{
		// C++17 structured binding on the returned pair.
		// DEFECT (minor): y is bound but never used, which warns under
		// /W4 or -Wunused-variable. The same pair is then discarded by the
		// caller anyway.
		auto [x, y] = GetMousePositionImpl();
		// Reuses the full-pair implementation rather than querying the X axis
		// alone, so this hook is not actually the optimisation Input.h
		// describes.
		return x;
	}

	// Mirror of GetMouseXImpl, with the same discarded-pair behaviour.
	float WindowsInput::GetMouseYImpl()
	{
		// x unused, same as line 59.
		auto [x, y] = GetMousePositionImpl();
		// Returns only the Y component; the X sample is thrown away.
		return y;
	}

}