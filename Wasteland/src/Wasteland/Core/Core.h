// Core.h -- platform detection, symbol visibility, assertions, and smart-pointer
// aliases.
//
// Included very widely (directly or via the precompiled header), so everything
// here is either a macro or a template. It is the lowest layer of the engine:
// nothing in this file may include another engine header.

#pragma once

#include <memory>

// Platform detection using predefined macros
//
// Exactly one WL_PLATFORM_* macro is defined, or the build stops. Unsupported
// targets stop the build deliberately rather than defining nothing and failing
// later with a confusing error deep in the platform layer.
#ifdef _WIN32
/* Windows x64/x86 */
#ifdef _WIN64
/* Windows x64 */
#define WL_PLATFORM_WINDOWS
#else
/* Windows x64 */
// 32-bit Windows was dropped: the engine assumes 64-bit pointers throughout,
// and a 32-bit build would misbehave at runtime rather than fail cleanly.
// Failing here is the only reliable way to catch it, since almost nothing
// rejects a 32-bit pointer by itself.
#error "x86 Builds are not supported!"
#endif
#elif defined(__APPLE__) || defined(__MACH__)
// TargetConditionals.h is the only supported way to distinguish macOS from
// iOS on Apple platforms; there is no compiler macro that does it.
#include <TargetConditionals.h>
/* TARGET_OS_MAC exists on all platforms
so we must check all of them (in this order)
to ensure that we're running on MAC
and nor some other Apple platform */
#if TARGET_IPHONE_SIMULATOR == 1
// Rejected for the same reason as x86: the engine has no iOS backend, so a
// build here could only ever fail later at link time.
#error "IOS simulator is not supported!"
#elif TARGET_OS_IPHONE == 1
#define WL_PLATFORM_IOS
// WL_PLATFORM_IOS is defined before the error so that any conditional
// compilation mentioning it is syntactically valid while the build is being
// rejected -- avoiding a cascade of secondary errors on top of this one.
#error "IOS is not supported!"
#elif TARGET_OS_MAC == 1
#define WL_PLATFORM_MACOS

#else
// A recognized Apple platform that is none of the above: a new one, most
// likely. Failing loudly is right, since no backend has been written for it.
#error "Unknown Apple platform!"
#endif
/* We also have to check __ANDROID__ before __linux__
since android is based on the linux kernel
it has __ANDROID__ defined */
// Order is load-bearing: Android also defines __linux__, so the Android branch
// must be tested first or Android builds would silently take the Linux path.
#elif defined(__ANDROID__)
#define WL_PLATFORM_ANDROID
// The Linux backend exists but does not build for Android (no Android window
// or input backend has been written), so this would fail confusingly later.
#error "Android is not supported!"
#elif defined(__linux__)
#define WL_PLATFORM_LINUX

#else
/* Unknown compiler/platform */
// The catch-all for an unrecognized target.
#error "Unknown platform!"
#endif // End of platform detection

// DLL support
//
// WL_API expands to the right declspec for the current build. Three outcomes:
//
//   WL_DYNAMIC_LINK + WL_BUILD_DLL  -> dllexport (we are the DLL)
//   WL_DYNAMIC_LINK, no build flag   -> dllimport (we consume the DLL)
//   neither, or non-Windows           -> empty (static linkage)
//
// Both flags come from the build system, not from this header.
#ifdef WL_PLATFORM_WINDOWS
#if WL_DYNAMIC_LINK
#ifdef WL_BUILD_DLL
// Exporting: this build IS the shared library, so its symbols are offered.
#define WL_API __declspec(dllexport)
#else
// Importing: another build produced the DLL, so these symbols come from
// elsewhere. Marking them dllimport lets the compiler resolve indirect calls
// rather than expecting them defined here.
#define WL_API __declspec(dllimport)
#endif
#else
// Static linkage: no visibility annotation is needed, because every symbol is
// available at link time. This is the mode both Sandbox and DemonCore-Editor
// currently use -- they reference Wasteland through a ProjectReference and
// link Wasteland.lib directly, which is why types such as Application can
// omit WL_API without breaking.
#define WL_API
#endif
#else
// On non-Windows platforms (Linux, macOS), visibility is handled with compiler attributes if needed
// Empty for now. ELF/Mach-O would normally use __attribute__((visibility("default")))
// plus a linker version script to export only the public API, but no such
// script is in the tree, so everything is currently visible by default.
#define WL_API
#endif // End of DLL support

// Assertions and the debug break they trigger.
//
// WL_DEBUG is set by the build system for debug configurations. Its presence is
// what makes assertions compile at all -- see the WL_ENABLE_ASSERTS block
// below.
#ifdef WL_DEBUG
#if defined(WL_PLATFORM_WINDOWS)
// Compiler intrinsic rather than a call into a platform API, so it needs no
// include and breaks at the assertion site.
#define WL_DEBUGBREAK() __debugbreak()
#elif defined(WL_PLATFORM_LINUX) || defined(WL_PLATFORM_MACOS)
// A raised SIGTRAP raises a debugger trap under gdb/lldb, which is the POSIX
// equivalent of __debugbreak. The header is included here rather than at the
// top because it is only needed in this configuration.
#include <signal.h>
#define WL_DEBUGBREAK() raise(SIGTRAP)
#else
// A debug configuration on a platform with no break primitive. Better a clear
// build failure than a debugger that silently steps over every assertion.
#error "Platform doesn't support debugbreak yet!"
#endif
// Defined from WL_DEBUG so that the assertion block below can key off a single
// name, and so a target could enable asserts without enabling WL_DEBUG if that
// were ever wanted.
#define WL_ENABLE_ASSERTS
#else
// Release: the break macro expands to nothing. Note this is still defined (as
// empty) rather than left undefined, so a caller that invokes WL_DEBUGBREAK()
// unconditionally still compiles.
#define WL_DEBUGBREAK()
#endif

#ifdef WL_ENABLE_ASSERTS
// Reports a failed assertion and breaks into the debugger.
//
// Logs through WL_ERROR / WL_CORE_ERROR, which are defined in Core/Log.h. That
// header is NOT included here on purpose: the macros expand at their use site,
// so WL_ERROR only needs to be visible where WL_ASSERT is called, not where it
// is defined. Keeping Log.h out of Core.h avoids a circular include, since
// Log.h needs Core.h.
//
// The braces are a deliberate do-while-less block: the macro is a single
// statement, so this is safe in an unbraced if without changing the caller's
// control flow. It is NOT safe inside a loop without braces -- WL_ASSERT inside
// `if (x) WL_ASSERT(y, ""); else ...` would misparse -- so always brace calls
// to it.
//
// @param x      Condition asserted to be true.
// @param ...    Format arguments for the failure message, e.g. WL_ASSERT(n < 10,
//               "index %d out of range", n). Must be non-empty, since the
//               format string is always passed.
#define WL_ASSERT(x, ...) { if(!(x)) { WL_ERROR("Assertion Failed: {0}", __VA_ARGS__); WL_DEBUGBREAK(); } }

// Same, but routed to the engine's core logger rather than the client's. Use
// this inside the engine; use WL_ASSERT in client code.
#define WL_CORE_ASSERT(x, ...) { if(!(x)) { WL_CORE_ERROR("Assertion Failed: {0}", __VA_ARGS__); WL_DEBUGBREAK(); } }
#else
// Release builds: both assertion macros expand to nothing.
//
// Note the arguments are not evaluated at all, not merely discarded. Any
// expression with a side effect passed as `x` will not run in release, which
// is the classic way side-effecting code accidentally ends up debug-only.
#define WL_ASSERT(x, ...)
#define WL_CORE_ASSERT(x, ...)
#endif

// Sets the nth bit to 1, producing 1 << n.
//
// Untyped: the literal 1 is int, so BIT(31) is undefined behaviour (shifting
// into the sign bit) and BIT(n) for n above 30 overflows a 32-bit int. Used
// with EventCategory values, whose highest is BIT(4), which is well within
// range -- but the macro is not safe for general use at the top of an int.
#define BIT(x) (1 << x)

// Binds a member function so it can be passed where a plain callback is
// expected, binding `this` and the event parameter.
//
// Usage, from the standard form of this macro:
//   dispatcher.Dispatch<KeyPressedEvent>(WL_BIND_EVENT_FN(OnKeyPressed));
//
// The class using it must derive from something that provides WL_API and the
// Event types -- in practice a Layer.
//
// Note it hard-codes std::placeholders::_1, so it only works for handlers
// taking exactly one argument. A two-argument handler needs a hand-written
// bind.
#define WL_BIND_EVENT_FN(fn) std::bind(&fn, this, std::placeholders::_1)

namespace Wasteland {

	// Owning single-assignment pointer alias, for types that have exactly one
	// owner.
	//
	// A naming convention rather than a new type: Scope<T> is
	// std::unique_ptr<T>, so the ownership intent is visible at the
	// declaration without depending on a type's spelling.
	template<typename T>
	using Scope = std::unique_ptr<T>;

	// Constructs a T and returns it as a Scope<T>.
	//
	// Exists so construction and ownership can be expressed in one expression
	// at a call site, and so the deduced return type is a Scope rather than a
	// bare unique_ptr.
	//
	// constexpr is what permits this at compile time for literal arguments;
	// it is harmless at runtime. Args are forwarded, so move-only arguments
	// and non-copyable T both work.
	//
	// @param args  Forwarded to T's constructor.
	// @returns A Scope<T> owning the new object.
	template<typename T, typename ... Args>
	constexpr Scope<T> CreateScope(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	// Shared-ownership pointer alias, for types that are genuinely shared.
	//
	// std::shared_ptr<T>. Prefer Scope where ownership is not genuinely shared:
	// shared ownership makes the destruction order of a cycle-dependent object
	// graph implicit and hard to reason about.
	template<typename T>
	using Ref = std::shared_ptr<T>;

	// Constructs a T and returns it as a Ref<T>.
	//
	// The shared-ownership counterpart to CreateScope, with the same rationale
	// and the same forwarding and constexpr behaviour.
	//
	// @param args  Forwarded to T's constructor.
	// @returns A Ref<T> sharing ownership of the new object.
	template<typename T, typename ... Args>
	constexpr Ref<T> CreateRef(Args&& ... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

}