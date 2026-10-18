// Wasteland::Age -- the guardian UI's decisions, with no ImGui in sight.
//
// GuardianToolsLayer draws this model. Everything a compliance reviewer would
// want to check about the guardian UI lives HERE, so it can be tested without
// a window, a GPU, or a frame loop -- the same reason GuardianPolicy was kept
// free of ImGui in the first place.
//
// A view model rather than drawing code also means the ImGui layer cannot
// quietly diverge from the policy: it has no values to invent, only the ones
// supplied here.
//
// WHAT THIS FILE IS NOT
//
// It is not a safety control. Nothing here restricts a game feature -- that is
// SafetyProfile's job. This decides what the guardian panel SAYS, and the
// state machine decides what is ENFORCED. If the UI lies, the player is
// misinformed; if the UI cannot influence enforcement at all, a lie cannot make
// a minor less protected. That asymmetry is deliberate: the view model is the
// least security-critical part of the module, which is exactly why it is the
// part most worth testing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Wasteland/Age/GuardianPolicy.h"

namespace Wasteland::Age
{

// What a control is, so the ImGui layer knows which widget to draw.
enum class ControlKind
{
	Toggle,   // on/off
	Value,    // numeric, e.g. a playtime limit
	Action,   // a button the player can press
};

// Who is meant to be operating this panel. Drives the wording: a seven-year-old
// must not be told "adjust your limits".
enum class ResponsibleParty
{
	Guardian, // a grown-up is in charge of these settings
	Self,     // the player is in charge of their own settings
};

// One row of the guardian panel.
struct ControlState
{
	std::string Label;
	ControlKind Kind = ControlKind::Toggle;

	// Can the user interact with it? False only while an action is
	// unavailable, never as a way of hiding a restriction.
	bool Enabled = true;

	// Toggle rows: the value that is actually ENFORCED. Not the resting
	// setting and not the requested one -- see the header note in the .cpp on
	// why a pending relaxation must not move the tick mark.
	bool Toggling = false;

	// Value rows: the enforced number, in the row's own units (minutes).
	int Value = 0;

	// True when a pending change differs from the resting setting for this
	// row. Drives the "waiting for review" marker.
	bool PendingChange = false;
};

// Everything the panel needs to draw itself.
struct GuardianToolsModel
{
	std::string Greeting;

	// Art 8(1) indicator. Up whenever protections are in force, down when they
	// are not. "Whenever", not "usually" -- an indicator that appears
	// intermittently is not an indicator.
	bool GuardianIndicatorVisible = false;
	std::string IndicatorText;

	ResponsibleParty ResponsibleParty = ResponsibleParty::Self;
	GuardianState State = GuardianState::Unconfigured;

	bool IsMinor = true;
	bool NeedsSetup = false;
	bool HasPendingChange = false;

	std::vector<ControlState> Controls;

	// What is restricted right now, and why. Derived from the EFFECTIVE
	// settings, so a row that is not actually restricted is never listed.
	std::vector<std::string> RestrictionNotes;
};

// Reminder cadence, so a signal that has gone stale is re-established rather
// than trusted forever. Art 20 wants assurance to stay live rather than be a
// one-off at signup.
struct Reminders
{
	// 14 days. Chosen to be well inside the window in which Google Play
	// refreshes a cached signal, so a stale signal is re-asked for before the
	// store would have updated it anyway.
	static constexpr std::uint64_t kIntervalSeconds = 14ull * 24ull * 60ull * 60ull;
};

// Build the panel's contents from a policy.
//
// Reads EffectiveSettings(), not Settings(), so what is shown is what is
// enforced. During a pending review that means the stricter combination -- a
// pending relaxation must not make the panel claim protection the player does
// not actually have.
GuardianToolsModel BuildGuardianToolsView(const GuardianPolicy &policy);

// How often this band should be reminded, in seconds. Zero means never.
std::uint64_t ReminderIntervalFor(AgeBand band);

// Should a reminder be shown now?
//
// `hasAcknowledged` is false when the player has never acknowledged one, which
// always means "remind" -- the first prompt is not nagging.
//
// `now` and `lastAcknowledged` are seconds from an arbitrary epoch; the only
// thing that matters is their difference, so the caller may use whatever clock
// it likes.
bool ShouldRemind(std::uint64_t now, std::uint64_t lastAcknowledged, bool hasAcknowledged);

} // namespace Wasteland::Age