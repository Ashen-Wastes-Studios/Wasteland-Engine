// Precompiled header first: it establishes <string> and the WL_CORE_* logging
// macros used below, before anything else declares a symbol from them.
#include "wlpch.h"
// The declaration of the FileDialogs class whose two static methods this file
// defines. Note the functions are NOT marked Windows-only in the header -- the
// guard below is what makes a Linux or macOS build link, and the other two
// platform directories (Linux/Mac) supply their own definitions.
#include "Wasteland/Utils/PlatformUtils.h"

// The entire translation unit is behind this guard, including the includes.
// That is what lets the Linux and Mac ports of PlatformUtils.cpp be selected by
// the build system instead of by #else branches in one file.
//
// Result: on a non-Windows build this file compiles to nothing, and the linker
// resolves FileDialogs::OpenFile/SaveFile from the sibling platform file.
#if defined(WL_PLATFORM_WINDOWS)

// The Win32 common-dialog API: OPENFILENAMEA, GetOpenFileNameA and
// GetSaveFileNameA. The `A` suffix selects the ANSI (non-wide) entry points,
// which is what the `char`-based filter and return types below require.
#include <commdlg.h>
// MUST be defined before glfw3native.h, and it is what that header checks to
// decide whether to declare the Win32-specific entry points. Defining it after
// the include has no effect and glfwGetWin32Window below would not compile.
//
// It also exposes the Win32 types (HWND) through GLFW's headers, which is how
// ofn.hwndOwner below can be assigned from a GLFW call.
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// For Application::Get(), the process-wide singleton, used to reach the window
// so the dialog is owned by (and modal to) the right HWND.
//
// DEFECT (a real ordering hazard): Application::Get() dereferences s_Instance
// unconditionally. Calling either function before the Application has been
// constructed is a null dereference, not a graceful failure. There is no
// IsInitialized() guard here even though that check exists on Application.
#include "Wasteland/Core/Application.h"

namespace Wasteland
{

	// The `filter` argument is a Win32 double-null-terminated string, e.g.
	// "Scene (*.wastescene)\0*.wastescene\0" -- description, pattern, empty
	// pattern, then a final \0. Callers pass string literals of exactly that shape
	// (EditorLayer.cpp: OpenScene passes one for *.wastescene).
	//
	// Returns an EMPTY STRING on cancel, per the header's contract. There is no way
	// to distinguish "user cancelled" from "user chose a file whose path happens
	// to be empty", but the latter cannot happen through this dialog.
	std::string FileDialogs::OpenFile(const char *filter)
	{
		// Stack-allocated, NOT zero-initialised here. The very next line zeroes it via
		// ZeroMemory, which is what makes the uninitialised-field concern moot -- but
		// it does so by size, using sizeof(OPENFILENAME) rather than sizeof(ofn).
		// Since ofn IS an OPENFILENAMEA and OPENFILENAMEA is a typedef of
		// OPENFILENAME, the two sizes are the same; the spelling difference is
		// cosmetic.
		//
		// A large struct (~88 bytes on Windows) on the stack; harmless.
		OPENFILENAMEA ofn;
		// The result buffer. 260 is MAX_PATH -- the classic Win32 limit -- and it is
		// a fixed array, not a std::string.
		// DEFECT: a path longer than 259 characters is silently truncated. There is no
		// OFN_EXPLORER-style long-path handling and no dynamic allocation, so the
		// returned path can be a non-existent prefix of what the user chose. The
		// double initialisation here ({0}) and the ZeroMemory on the previous line's
		// target are also redundant with each other, though neither is wrong.
		CHAR szFile[260] = {0};
		// Required before filling the struct: Win32 reads several fields the caller
		// never sets (lpstrTitle, lpstrInitialDir, nMaxCustFilter, ...) and an
		// uninitialised garbage value in the Flags-adjacent fields changes behaviour.
		// This is the documented requirement for the OPENFILENAME struct.
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		// Mandatory first field. The dialog validates it against its own struct size,
		// which is how a newer comdlg32.dll knows the caller is from a newer SDK.
		// Forgetting it is the classic cause of a dialog that fails to open.
		ofn.lStructSize = sizeof(OPENFILENAME);
		// Chains three layers to reach the dialog's parent window:
		//   Application::Get()  -- the singleton (dereferenced unchecked)
		//   GetWindow()         -- by reference, so *m_Window is the GLFWwindow*
		//   GetNativeWindow()   -- the GLFWwindow* itself
		//   glfwGetWin32Window -- the GLFWwindow* translated to an HWND
		// The owner HWND is what makes the dialog modal to the game window and
		// centres it there.
		//
		// DEFECT: the result is not null-checked. If Application::Get() has run but
		// the platform window has no native handle yet, this silently passes nullptr
		// and the dialog opens unowned rather than reporting the problem.
		//
		// The C-style cast is redundant -- glfwGetWin32Window takes GLFWwindow* and
		// GetNativeWindow() already returns that type -- but harmless.
		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow *)Application::Get().GetWindow().GetNativeWindow());
		// Points the dialog at the caller's buffer. On return it holds the chosen
		// path, which is why the return below reads from ofn.lpstrFile rather than
		// from szFile directly -- same pointer, but reading it through the struct
		// documents where the data came from.
		ofn.lpstrFile = szFile;
		// Buffer capacity IN BYTES including the terminator, not a character count.
		// sizeof on a char array is the element count, so the two coincide here only
		// because szFile is an array rather than a pointer.
		ofn.nMaxFile = sizeof(szFile);
		// Borrowed, not copied, and not freed by the dialog -- so the caller's
		// string literal must outlive the call. It does, being static storage.
		// Passing nullptr here would show an "All files" filter instead.
		ofn.lpstrFilter = filter;
		// 1-based index into the filter's description/pattern pairs, so 1 selects the
		// first entry. Zero would mean "no preselection".
		// Hard-coded in both functions here, so a caller cannot choose which filter
		// starts selected.
		ofn.nFilterIndex = 1;
		// Three flags, all of which change what the dialog will accept:
		//   OFN_PATHMUSTEXIST -- reject a typed directory that does not exist
		//   OFN_FILEMUSTEXIST  -- reject a non-existent file name (GetOpenFileName
		//                        enforces this anyway; GetSaveFileName does NOT
		//                        without it)
		//   OFN_NOCHANGEDIR   -- do not switch the process working directory to the
		//                        chosen folder
		//
		// OFN_NOCHANGEDIR is the load-bearing one for a game: without it the dialog
		// changes the CWD for the whole process, and every relative asset path
		// resolved afterwards would break.
		//
		// Not set, and worth noting: OFN_EXPLORER (modern-style dialog) and
		// OFN_ALLOWMULTISELECT. Single-file selection only.
		// The flags are identical in SaveFile below -- copy-pasted rather than factored
		// into a shared initialiser.
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		// Explicit `== TRUE` rather than using the return as a condition.
		// GetOpenFileNameA returns BOOL (an int), so this is valid but non-idiomatic;
		// it also means a hypothetical non-1 success value would be treated as failure.
		// The idiomatic spelling is `if (GetOpenFileNameA(&ofn))`.
		//
		// The call BLOCKS and pumps the Win32 message loop for the dialog's own window,
		// which is why it must run on the thread that owns the HWND.
		if (GetOpenFileNameA(&ofn) == TRUE)
		{
			// Reading back through the struct field. Implicit std::string construction from
			// the char*, so the buffer is copied -- returning szFile by reference would
			// have been a dangling-pointer bug once the stack unwound.
			//
			// The whole function therefore cannot fail visibly: a cancelled dialog and a
			// successful one differ only by which string is returned.
			return ofn.lpstrFile;
		}
		// The cancel path: an empty std::string, per the header's "these return empty
		// strings if cancelled" contract. Every caller must therefore test .empty()
		// before using the path -- EditorLayer.cpp: OpenScene does exactly that.
		return std::string();
	}

	// Structurally identical to OpenFile -- lines 37 through 46 repeat it exactly,
	// field for field. There is no shared helper (a fillOpenFilename(ofn, filter)
	// would remove the duplication), so the two can drift apart silently.
	//
	// That is not hypothetical: OFN_OVERWRITEPROMPT is what a save dialog normally
	// needs and it is absent here, so choosing an existing file does NOT prompt
	// before overwriting. OpenFile does not need it, which is part of why the
	// duplication is easy to miss.
	std::string FileDialogs::SaveFile(const char *filter)
	{
		// Same uninitialised declaration as in OpenFile, zeroed on the next line.
		OPENFILENAMEA ofn;
		// Same fixed MAX_PATH buffer, with the same silent-truncation limitation.
		//
		// For a SAVE dialog this buffer is also the INPUT: szFile is pre-filled with a
		// suggested name by the caller... except no caller in this codebase pre-fills
		// it, so the dialog always opens with an empty name box.
		CHAR szFile[260] = {0};
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		// Byte-for-byte the same expression as in OpenFile -- same unchecked
		// Application::Get() dereference, same unvalidated HWND.
		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow *)Application::Get().GetWindow().GetNativeWindow());
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		// The save variant. Note the missing OFN_OVERWRITEPROMPT again: the Win32
		// documentation states GetSaveFileName does not itself check for an existing
		// file unless that flag is set, so this dialog will silently accept a name that
		// already exists and the caller's write will clobber it.
		//
		// Like the open call, this blocks and pumps messages on the calling thread.
		if (GetSaveFileNameA(&ofn) == TRUE)
		{
			// Same copy-out as the open path.
			return ofn.lpstrFile;
		}
		// Cancel path, same contract as OpenFile.
		return std::string();
	}

}

#endif // WL_PLATFORM_WINDOWS