#include "wlpch.h"

#include "Wasteland/Project/ProfileAnalyzer.h"

#include "Wasteland/Mesh/Json.h"

#include <algorithm>
#include <cstdio>

namespace Wasteland
{
	namespace
	{
		// The system a trace event counts towards.
		std::string SystemOf(const std::string &name)
		{
			const std::size_t first = name.find("::");
			if (first == std::string::npos)
				return name;
			const std::size_t second = name.find("::", first + 2);
			if (second == std::string::npos)
				return name;
			return name.substr(0, second);
		}
	}

	ProfileReport AnalyzeProfile(const std::string &jsonText,
	                             const std::vector<ProfileBudget> &budgets)
	{
		ProfileReport report;
		const auto fail = [&report](const std::string &message)
		{
			report.Ok = false;
			report.Error = message;
			return report;
		};

		JsonValue root;
		std::string parseError;
		if (!JsonParse(jsonText, root, parseError))
			return fail("could not parse the profile JSON: " + parseError);
		if (!root.IsObject())
			return fail("the profile JSON is not an object");
		const JsonValue *events = root.Find("traceEvents");
		if (!events || !events->IsArray())
			return fail("the profile JSON has no traceEvents array");

		// name -> aggregate. A map only for accumulation; the report is the
		// sorted order below.
		std::vector<ProfileSample> samples;

		for (std::size_t i = 0; i < events->Size(); ++i)
		{
			const JsonValue *event = events->At(i);
			if (!event || !event->IsObject())
				continue;

			// Only complete ("X") events carry a duration. "M" metadata, "B"/"E"
			// async spans and "I" instants have no per-event dur worth budgeting.
			const JsonValue *ph = event->Find("ph");
			if (!ph || !ph->IsString() || ph->StringValue != "X")
				continue;

			double dur = 0.0;
			const JsonValue *durValue = event->Find("dur");
			if (!durValue || !durValue->AsNumber(dur))
				continue;
			dur /= 1000.0; // microseconds -> milliseconds

			const JsonValue *nameValue = event->Find("name");
			if (!nameValue || !nameValue->IsString())
				continue;

			const std::string system = SystemOf(nameValue->StringValue);

			ProfileSample *sample = nullptr;
			for (ProfileSample &candidate : samples)
			{
				if (candidate.System == system)
				{
					sample = &candidate;
					break;
				}
			}
			if (!sample)
			{
				ProfileSample next;
				next.System = system;
				samples.push_back(next);
				sample = &samples.back();
			}

			sample->EventCount += 1;
			sample->TotalMs += dur;
			if (dur > sample->MaxMs)
				sample->MaxMs = dur;
			report.TotalMs += dur;
		}

		// EventCount is the sum of the per-system counts.
		report.EventCount = 0;
		for (const ProfileSample &sample : samples)
			report.EventCount += sample.EventCount;

		// Deterministic sort: total desc, system asc on ties.
		std::sort(samples.begin(), samples.end(),
		          [](const ProfileSample &a, const ProfileSample &b)
		          {
			          if (a.TotalMs != b.TotalMs)
				          return a.TotalMs > b.TotalMs;
			          return a.System < b.System;
		          });
		report.Systems = samples;

		// Budgets: a system that is not budgeted is not warned about; an explicit
		// budget for a system the capture never ran is a different failure (the
		// system was expected and missing), so it is surfaced as a warning too.
		for (const ProfileBudget &budget : budgets)
		{
			const ProfileSample *sample = nullptr;
			for (const ProfileSample &candidate : samples)
			{
				if (candidate.System == budget.System)
				{
					sample = &candidate;
					break;
				}
			}
			if (!sample)
			{
				char buffer[160];
				std::snprintf(buffer, sizeof(buffer),
				              "%s: budgeted %.2f ms but the capture never ran it",
				              budget.System.c_str(), budget.BudgetMs);
				report.Warnings.push_back(buffer);
				continue;
			}
			if (sample->TotalMs > budget.BudgetMs)
			{
				char buffer[192];
				std::snprintf(buffer, sizeof(buffer),
				              "%s: exceeded its %.2f ms budget (total %.2f ms over %lld events)",
				              budget.System.c_str(), budget.BudgetMs, sample->TotalMs,
				              (long long)sample->EventCount);
				report.Warnings.push_back(buffer);
			}
		}

		return report;
	}
}
