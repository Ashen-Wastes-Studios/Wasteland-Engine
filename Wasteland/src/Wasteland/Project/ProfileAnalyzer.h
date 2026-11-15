// ProfileAnalyzer.h -- per-system budget checks over a profile capture (Phase J.4).
//
// The capture format is the Chrome trace-event JSON the engine already writes
// (`bin/WastelandProfile-Runtime.json`, `-Startup.json`): a `traceEvents` array of
// `{ph:"X", dur, name, ts}` events, `dur` in microseconds.
//
// WHY THIS EXISTS
// The plan's J.4 wants "a per-frame millisecond budget per system, with a warning
// when a scene exceeds it". The PANEL is editor UI; what this type provides is
// the budget DECISION, testable headlessly and usable by the panel later: group a
// capture by system, total each system's CPU time, and warn when a system
// exceeds the budget set for it.
//
// Frame attribution is deliberately NOT invented here. The shipped captures have
// no frame markers, so "per-frame" cannot be computed from them without guessing
// where one frame ends and the next begins -- and a budget that rests on a guess
// is worse than none. This reports per-system AGGREGATE ms over the capture, and
// the budget a caller sets is expressed the same way. When the runtime emits
// labelled frames, a frame-scoped variant is a natural extension, not a rewrite.
//
// Determinism: systems are sorted by total ms descending (ties by name ascending),
// so a report is reproducible for CI comparison.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland
{
	struct ProfileSample
	{
		// The system this event counts towards: the first two namespace segments
		// of the event name ("Wasteland::OpenGLShader::Bind" -> "Wasteland::OpenGLShader"),
		// or the whole name for a function that has no namespace ("glfwInit").
		std::string System;
		std::int64_t EventCount = 0;
		double TotalMs = 0.0;
		double MaxMs = 0.0;
	};

	struct ProfileBudget
	{
		// The system to budget.
		std::string System;
		// The allowed aggregate CPU time in milliseconds over this capture.
		double BudgetMs = 0.0;
	};

	struct ProfileReport
	{
		bool Ok = true;
		std::string Error;

		std::int64_t EventCount = 0;
		double TotalMs = 0.0;

		// Sorted by TotalMs descending, ties by System ascending.
		std::vector<ProfileSample> Systems;

		// One entry per budget whose system exceeded it, naming the system, the
		// budget and the actual total -- the actionable form of "X is slow".
		std::vector<std::string> Warnings;
	};

	// Analyzes a Chrome trace-event JSON capture against the given budgets.
	// `budgets` may be empty (a pure inventory report). A malformed capture is
	// reported as `Ok = false` with `Error`, never a guess.
	ProfileReport AnalyzeProfile(const std::string &jsonText,
	                             const std::vector<ProfileBudget> &budgets);
}
