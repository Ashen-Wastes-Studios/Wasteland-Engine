// The precompiled header: the first include in essentially every engine
// translation unit.
//
// Its whole purpose is to make <iostream> and friends arrive once at build
// time rather than being parsed in every one of the hundreds of .cpp files.
// The cost is that it must stay small: anything added here is paid for by
// every TU, including ones that need none of it.
//
// It is a header rather than a forced-include setting, which is why every
// engine source opens with #include "wlpch.h". A few files do not --
// tests/test_age_service.cpp uses a bare <cstdio> instead -- and those pay
// the full parse cost.
#pragma once

// iostream, not the narrower headers. It is the heaviest include here and
// the one most TUs do not need; but splitting it out has evidently not been
// considered worth the build complexity.
#include <iostream>
// std::unique_ptr and std::shared_ptr. Engine types hold Ref<> wrappers, but
// the raw smart pointers still appear at the API boundaries (for example
// AgeService::SetProvider takes a std::unique_ptr).
#include <memory>
// std::move and std::forward. Needed by nearly every by-value pass-through
// in the engine, which is why it is here rather than included locally.
#include <utility>
// The full algorithm header. Heavier than it looks: this pulls in sorting,
// searching, numeric and heap algorithms.
//
// DEFECT (documented, not fixed): a PCH this broad defeats its own purpose
// for TUs that need none of it, and it makes every engine compile pay for
// the widest include. The trade is deliberate -- the comment at the top of
// this file says so -- but a narrower PCH plus a small number of explicit
// per-TU includes would compile the same code faster.
#include <algorithm>
// std::function, used for the completion callbacks on animation instances
// (AnimationInstance::OnFinished is a std::function).
#include <functional>

// std::string. Unavoidable: it appears in nearly every interface.
#include <string>
// stringstream. Heavier than <string> and needed by comparatively few TUs --
// mostly the ToString() implementations on events, which build their text
// through a local stream. This is a reasonable candidate to move out of the
// PCH.
#include <sstream>
// std::vector, another effectively universal dependency.
#include <vector>
// The two unordered containers. Used for name-keyed lookups throughout the
// registry and the animation state machine.
#include <unordered_map>
#include <unordered_set>

// The engine's own headers, in angle brackets so the include path is
// searched rather than this directory.
//
// Log.h supplies the WL_CORE_INFO/WARN/ERROR macros, which is why the
// renderer factories can report a failed lookup. (WL_CORE_ASSERT is NOT
// here -- that one is in Core.h, which Log.h pulls in on its own.)
//
// DEFECT (documented, not fixed): Log.h itself includes "wlpch.h" at its
// own line 13, so this is a CIRCULAR include. It is harmless only because
// #pragma once on line 1 of this file stops the recursion, and it resolves
// because this header is reached first. Include the other way round -- a TU
// that includes Log.h before wlpch.h -- would silently get the un-populated
// PCH instead, and the file would compile differently depending on include
// order.
#include <Wasteland/Core/Log.h>

// The profiler. Included unconditionally because WL_PROFILE_FUNCTION and
// WL_PROFILE_SCOPE appear at the top of many hot functions.
//
// DEFECT (documented, not fixed): Instrumentor.h defines WL_PROFILE_FUNCTION
// TWICE -- once active and once as an empty macro -- behind its own switch,
// so the macros themselves cost nothing when profiling is off. But the HEADER
// is still parsed by every TU that takes this PCH. Guarding the include with
// the same switch would keep it out of release builds entirely.
#include <Wasteland/Debug/Instrumentor.h>

// Windows.h behind a platform guard, and it must stay guarded: the header
// defines macros such as min, max, near, far and TRUE that collide with
// identifiers the rest of the engine uses.
//
// DEFECT (documented, not fixed): including Windows.h before anything else
// in a translation unit is the well-known cause of the min/max collision
// problem, and it is why the engine uses its own WL_MOUSE_BUTTON_*
// constants rather than GLFW's. Every non-Windows platform skips this
// include, so the collision is paid for on one platform and not the others.
#ifdef WL_PLATFORM_WINDOWS
#include <Windows.h>
// End of the guarded Windows include. Note there is no matching #undef for
// the macros Windows.h introduces -- the engine works around them at the
// call sites instead, which is more fragile than undefining them here.
#endif