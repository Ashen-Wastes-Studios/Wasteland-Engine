// PCH first. It brings in <string> and <vector>, both used below without a local
// include.
#include "wlpch.h"

// The class being implemented. Transitively pulls in GuardianToolsView.h (for
// GuardianToolsModel and BuildGuardianToolsView) and GuardianPolicy.h (for
// GuardianSettings), so neither is included here directly.
//
// The header is reached with a full path rather than a relative one, so this
// file's location does not affect the include search.
#include "Wasteland/Age/GuardianToolsLayer.h"

// The ONLY third-party dependency, and the reason this file exists separately
// from GuardianToolsView.cpp.
//
// Everything that decides what is drawn lives in the view model; this file only
// turns that model into widgets. That split is what makes the guardian logic
// testable without a window -- the header's comment calls it out.
#include "imgui.h"

namespace Wasteland::Age
{

// An anonymous namespace, so the two file-static flags and StateName below have
// INTERNAL linkage: no other translation unit can name them, and no symbol
// clashes are possible with a same-named helper elsewhere.
//
// Preferable to `static` on each item because it also covers types and
// constants.
namespace
{

// The header explains why: visibility is a static API (SetVisible/IsVisible are
// static members), so the backing state has to be static too.
//
// That is a deliberate trade. A static means TWO GuardianToolsLayer instances
// share one visibility flag and one indicator flag -- so a second instance
// cannot have its own panel state. It buys an API a game script can call
// without holding a layer pointer, which is the use that matters here.
// Static visibility, because the layer may be driven from a game script or a
// settings screen rather than from a menu the layer itself owns.
// FALSE, so the panel starts closed. The header's reasoning: a panel that opens
// on launch and covers the game is one studios disable, and a disabled panel
// protects nobody.
//
// Zero-initialised explicitly, which for a namespace-scope bool would happen
// anyway. Consistent with s_IndicatorEnabled below.
bool s_Visible = false;
// Also false, and for a different reason than s_Visible: this one is OFF BY
// DEFAULT rather than on, because the module ships into games nobody has
// reviewed yet and a surprise banner over someone's build is a worse first
// impression than a documented one-line opt-in.
//
// So the two flags are both false but mean opposite things -- one is "don't
// start in the player's face", the other is "don't opt them in silently".
// The header documents that EU studios should call SetIndicatorEnabled(true).
//
// Note this gates the INDICATOR only; the panel's visibility is s_Visible.
bool s_IndicatorEnabled = false;

// Maps a state enum to the string shown in the "Status:" line.
//
// Returns a string LITERAL, so there is no lifetime question -- the pointer is
// valid for the life of the program, which is what lets it be returned as a
// bare `const char*` rather than a std::string.
//
// Takes the enum BY VALUE: it is a small scoped enum with no payload, so a copy
// is free and the signature stays simple.
const char *StateName(const GuardianState state)
{
	// A switch rather than a lookup table, because the strings differ in length and
	// a table would need a parallel array of the enum values -- which the compiler
	// cannot check stays in step with the enum.
	//
	// Every case returns, so there is no fallthrough.
	switch (state)
	{
	case GuardianState::Unconfigured: return "Not set up";
	case GuardianState::MinorNeedsSetup: return "Needs setup";
	case GuardianState::Minor: return "Active";
	case GuardianState::Adult: return "Your own settings";
	// The only state whose name mentions the process rather than the outcome --
	// appropriate, because the point of the line is to tell the player their change
	// is not yet in effect.
	//
	// Wording is deliberately non-technical: no mention of policy, approval or
	// state machine, all of which would be meaningless to the audience this panel
	// is for. GuardianToolsView.h notes the ResponsibleParty distinction exists for
	// the same reason.
	case GuardianState::PendingReview: return "Change awaiting review";
	// DEFENCE IN DEPTH, and the only thing keeping this total. If a new
	// GuardianState is added and this switch is not updated, the exhaustive-looking
	// set of cases above would silently map it to "Unknown" -- which is
	// indistinguishable in the UI from a genuinely unrecognised state.
	//
	// The usual stronger form is WL_CORE_ASSERT(false) so a developer build
	// catches the missing case at once; this returns a string instead, so a
	// shipped build shows "Status: Unknown" with no other symptom. A static_assert
	// over an explicit list of enumerators would close the gap at compile time.
	default: return "Unknown";
	}
}

} // namespace

// The layer's name is passed to the Layer base, which uses it for profiling
// scopes and for identification in layer dumps.
//
// `::Wasteland::Layer` is fully qualified because this file sits inside
// `namespace Wasteland::Age` -- without the leading `::`, `Layer` would resolve
// inside Age, where no such name exists.
GuardianToolsLayer::GuardianToolsLayer()
	: ::Wasteland::Layer("GuardianToolsLayer")
{
	// Start from the fail-closed view. A panel built before anyone has called
	// SetPolicy must show restrictions, not an empty adult screen.
	// FAIL-CLOSED initialisation, and the single most important line in this
	// constructor.
	//
	// A default-constructed GuardianPolicy is already Unconfigured with fully
	// restrictive settings, so Restricted() is belt-and-braces -- but naming it
	// explicitly means the intent survives a future change to the default.
	//
	// The consequence: a layer attached and rendered BEFORE anyone calls SetPolicy
	// shows protective settings on, the indicator up, and setup needed. Not an
	// empty adult screen. The header states this is the correct thing to show.
	m_Model = BuildGuardianToolsView(GuardianPolicy::Restricted());

	// The draft starts at the restrictive GuardianSettings defaults, which is
	// what Restricted() implies. SetPolicy replaces it with the real effective
	// settings on the first update.
	// The draft starts at GuardianSettings' own defaults, which are the restrictive
	// values -- so it is consistent with the Restricted() model above rather than
	// with a zero-initialised struct.
	//
	// That consistency matters: the panel's controls read from m_Draft (see the
	// slider case below), so a draft that disagreed with the model would show one
	// value while the status line described another.
	//
	// The first SetPolicy replaces it with the real effective settings.
	m_Draft = GuardianSettings{};
}

// Defaulted, and genuinely nothing to do: the class owns no GPU resources, no
// file handles and no subscriptions. m_Model and m_Draft are plain values.
//
// Defined out of line rather than `= default` in the header only because the
// header declares it (to make it virtual-overriding); the body could equally
// have been in the class.
GuardianToolsLayer::~GuardianToolsLayer() = default;

// A static setter writing the static flag -- matching the static declaration in
// the header.
//
// One-line body, no brace on its own line: the codebase's mixed house style,
// present throughout this file.
//
// Does NOT touch s_IndicatorEnabled. The two are independent by design: the
// indicator must remain visible with the panel closed.
void GuardianToolsLayer::SetVisible(const bool visible) { s_Visible = visible; }
// The matching getter, also static.
//
// DEFECT (minor): not marked const. For a static member function that makes no
// difference to callers, so this is cosmetic.
bool GuardianToolsLayer::IsVisible() { return s_Visible; }
// The opt-in for the persistent Art 8(1) indicator. A studio calls this once at
// startup.
//
// `const` on a by-value bool parameter: no effect on the signature or ABI.
void GuardianToolsLayer::SetIndicatorEnabled(const bool enabled) { s_IndicatorEnabled = enabled; }
// Read by a studio's settings UI to show the current opt-in state.
//
// Distinct from the model's GuardianIndicatorVisible: this is "did the studio
// turn the feature on", while that is "are protections actually in force".
// RenderIndicator requires BOTH before drawing anything.
bool GuardianToolsLayer::IsIndicatorEnabled() { return s_IndicatorEnabled; }

// The layer's only input. Called by whatever owns the AgeService, typically
// once per frame or on change.
//
// Takes a const reference and STORES A COPY via BuildGuardianToolsView -- the
// header notes why: AgeService hands out a fresh GuardianPolicy per call, so
// holding a reference to a temporary would dangle. Nothing here retains a
// reference to `policy` after the function returns.
void GuardianToolsLayer::SetPolicy(const GuardianPolicy &policy)
{
	// The rebuild. Reads policy.EffectiveSettings(), NOT Settings(), which is the
	// module's central invariant: what the panel shows is what is enforced.
	//
	// During a pending review that means a proposed relaxation does not move the
	// tick marks -- the panel keeps showing the stricter combination in force.
	// The header of GuardianToolsView.h states this as the reason the model is
	// derived rather than authored.
	//
	// Unconditional, so the model always tracks the policy even while the user has
	// unsaved edits. The draft is what is protected below, not the model.
	m_Model = BuildGuardianToolsView(policy);

	// The draft follows the model unless the user has unsaved edits --
	// rebuilding the model every frame would otherwise silently discard
	// whatever they were in the middle of typing.
	// The unsaved-edits guard, and the reason the draft is a separate member at all.
	//
	// If the user is mid-edit, the draft is LEFT ALONE. Without this, a per-frame
	// SetPolicy would silently overwrite whatever they were typing.
	//
	// DEFECT (no exit from the state): while m_DraftDirty stays true the draft and
	// the model can diverge, and the panel shows the draft's values (see the Value
	// case below) against the model's status line. That is the intended behaviour --
	// the user's pending intent is what they should see -- but nothing in this file
	// ever clears m_DraftDirty. There is no submit or revert path here, so after
	// the first edit the draft is pinned for the life of the layer unless something
	// outside this file resets it.
	if (!m_DraftDirty)
		// Syncs the draft from EFFECTIVE settings, matching what the model displays.
		//
		// Using EffectiveSettings() rather than Settings() keeps the draft consistent
		// with the enforced state during a pending review: the user edits from what is
		// actually in force, not from the resting setting the policy has moved on
		// from.
		//
		// A copy assignment of a small POD-ish struct -- five fields.
		m_Draft = policy.EffectiveSettings();
}

// Draws the persistent indicator, SEPARATE from the panel so it is unaffected by
// the panel being closed -- the whole point of an Art 8(1) indicator.
//
// Takes the model by const reference rather than reading m_Model directly: it
// makes the dependency explicit and would let a different model be drawn.
// The one caller passes m_Model, so in practice it is the same object.
void GuardianToolsLayer::RenderIndicator(const GuardianToolsModel &model)
{
	// The persistent Art 8(1) indicator.
	//
	// Drawn whenever the model says protections are in force, and INDEPENDENT
	// of whether the panel is open. The whole point of a persistent indicator
	// is that the player does not have to go looking for it.
	// TWO independent conditions, and both are required.
	//
	// s_IndicatorEnabled -- the studio's opt-in. Off by default, so nothing is drawn
	// unless a game explicitly enables it.
	//
	// model.GuardianIndicatorVisible -- whether protections are actually in force.
	// The view derives this rather than the layer deciding it, so the indicator is
	// up whenever and only whenever protections apply. The view's comment is blunt
	// about the requirement: an indicator that appears intermittently is not an
	// indicator.
	//
	// Order is cheap-first: the flag test is a load, the model test a member read
	// behind it.
	if (!s_IndicatorEnabled || !model.GuardianIndicatorVisible)
		return;

	// The OS window's usable area, as opposed to the whole display. WorkPos excludes
	// the taskbar and any menu bar, so positioning against it keeps the indicator
	// clear of system furniture.
	//
	// Const pointer to ImGui's own singleton-owned data -- nothing here owns it and
	// nothing should free it.
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	// A fixed 16-pixel inset from the top-left of the work area.
	//
	// ImGuiCond_Always rather than ImGuiCond_FirstUseEver or ImGuiCond_Once: the
	// indicator must return to that corner after the user drags it away, otherwise
	// a player could move the assurance indicator somewhere it stops functioning as
	// one. Always also overrides anything in ImGui.ini.
	//
	// This is a state-setting call, not a widget: it applies to the NEXT Begin,
	// which is why it sits outside the Begin/End pair below.
	ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + 16.0f), ImGuiCond_Always);
	// Fully transparent background, so the indicator is text floating over the game
	// rather than a box drawn on top of it. Same state-setting discipline as above:
	// it must precede the Begin it applies to.
	ImGui::SetNextWindowBgAlpha(0.0f);

	// Five flags, and each removes something that would undermine a passive
	// indicator:
	//   NoDecoration      -- no title bar, no resize grips, no scrollbar
	//   AlwaysAutoResize  -- hugs the text, no sizing handles
	//   NoSavedSettings   -- position and size are not persisted, so the ImGuiCond_
	//                       Always above always wins
	//   NoFocusOnAppearing-- does not steal keyboard focus, so it cannot interrupt
	//                       gameplay the moment protections come into force
	//   NoNav             -- excluded from keyboard navigation, so a player cannot
	//                       tab into it
	//
	// Together: present, visible, and impossible to mistake for an interactive
	// control or to lose.
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
	                               ImGuiWindowFlags_AlwaysAutoResize |
	                               ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_NoFocusOnAppearing |
	                               ImGuiWindowFlags_NoNav;

	// The window name carries a leading double-hash. That is ImGui's ID-suffix
	// convention: everything from the hashes onward is the internal identifier and
	// the visible title becomes empty, so the text shown is only IndicatorText
	// below. The wl_ prefix keeps the ID out of the namespace of other panels.
	//
	// A null pointer for the close button -- there is none, and passing a pointer
	// would let ImGui write through it on a click that can never happen.
	//
	// The Begin/End pair below is what makes the return value meaningful: Begin
	// returns false when collapsed or clipped, and End must be called either way.
	if (ImGui::Begin("##wl_age_indicator", nullptr, flags))
		// Unformatted, so a percent sign in the text cannot be read as a format
		// specifier. The wording itself comes from the view model, which owns the
		// wording decision.
		//
		// No TextWrapped: a long indicator would be clipped rather than wrapped, which
		// for a single short assurance line is the safer failure.
		ImGui::TextUnformatted(model.IndicatorText.c_str());
	ImGui::End();
}

// The Layer override, called by Application once per frame for every layer.
//
// Two responsibilities, in order: the indicator is unconditional, the panel is
// opt-in. That ordering is load-bearing -- the indicator must be drawn whether
// or not the panel is open, so RenderIndicator comes BEFORE the visibility
// check below.
void GuardianToolsLayer::OnImGuiRender()
{
	// The indicator is unconditional; the panel is opt-in.
	// Before the visibility early-out, deliberately.
	//
	// Putting it after would make the Art 8(1) indicator appear only when the panel
	// is open -- which the header explicitly rejects as not an indicator. The
	// comment at the top of this function records that ordering is the point.
	RenderIndicator(m_Model);

	// The panel's opt-in gate. Returns without calling any ImGui Begin, so no
	// window is created and the frame costs one branch when the panel is closed.
	//
	// Uses the static flag rather than the model's contents: this is a presentation
	// choice, not a policy consequence.
	if (!s_Visible)
		return;

	// The Begin/End discipline that the rest of this function depends on: EVERY
	// Begin must be matched by exactly one End, including on the early-out path.
	// An unmatched Begin corrupts ImGui's internal window stack for the whole
	// frame.
	//
	// The visible title here has no ID suffix, so this string doubles as the
	// window's identity and its label.
	//
	// A user can close this window with the X, which ImGui records internally --
	// so a player who dismisses it may not see it again this session, with no code
	// path to reopen it. SetVisible would not help, because ImGui's own closed flag
	// is what Begin is reporting here.
	if (!ImGui::Begin("Guardian & Age Safety"))
	{
		// Collapsed, or closed by the user. Still consuming the Begin so the
		// matching End below is paired correctly.
		ImGui::End();
		return;
	}

	// The greeting, supplied by the view model rather than composed here.
	//
	// That is the ResponsibleParty distinction at work: GuardianToolsView.h notes
	// the wording must differ because a seven-year-old must not be told "adjust
	// your limits". This layer cannot choose the words, only display them.
	ImGui::TextUnformatted(m_Model.Greeting.c_str());
	ImGui::Separator();

	// The one place StateName is used. Takes the format string separately, so a state
	// name containing a percent sign could not corrupt the output -- unlike
	// TextUnformatted above, where the string IS the format.
	//
	// The %s argument is a const char* to a literal, so no lifetime issue.
	//
	// Worth noting: ImGui also offers an unformatted Text(const char*) overload, so
	// a future edit that drops the argument here would silently compile as a
	// different overload rather than failing.
	ImGui::Text("Status: %s", StateName(m_Model.State));

	// The fail-closed explanation. Gated on the model, so the wording decision stays
	// in the testable layer.
	//
	// Spacing first to separate the paragraph from the status line above --
	// cosmetic, and the only layout handling in this function.
	if (m_Model.NeedsSetup)
	{
		ImGui::Spacing();
		// Fail-closed state: say plainly that settings are restrictive rather
		// than implying the player chose them.
		// Wrapped, because the text is a sentence rather than a label and the panel
		// width is not known here.
		//
		// The two adjacent string literals concatenate at compile time into one
		// argument -- adjacent-literal concatenation, not two arguments.
		//
		// The wording is deliberate and is the compliance-relevant part: it states the
		// settings are protective BY DEFAULT rather than implying the player chose
		// them, and it promises they persist until a signal arrives. That is the
		// honest description of GuardianPolicy::Restricted().
		ImGui::TextWrapped("Protective settings are on because no age signal has been "
		                   "provided. They stay on until one is.");
	}

	// The pending-review notice. Independent of the NeedsSetup block above: both can
	// be true at once (a minor with no signal AND a change queued), in which case both
	// paragraphs appear.
	//
	// The message's substance -- that the MORE PROTECTIVE settings continue to
	// apply -- is the user-facing statement of the module's central invariant, and
	// it is true because the view reads EffectiveSettings().
	if (m_Model.HasPendingChange)
	{
		ImGui::Spacing();
		ImGui::TextWrapped("A change is awaiting review. Until it is approved, the "
		                   "more protective settings continue to apply.");
	}

	ImGui::Spacing();

	// The control loop begins. Preceded by a Spacing() call so the controls are
	// visually separated from the status paragraphs above.
	// Controls.
	//
	// Every value drawn here comes from the model's EFFECTIVE settings. The
	// The load-bearing comment of the whole function, and the architectural claim
	// the module rests on: nothing in this loop writes to enforcement.
	//
	// Every branch below assigns to m_Draft (a local member) and sets m_DraftDirty.
	// There is no call into GuardianPolicy, no mutation of a shared policy, and no
	// path from a widget here to an enforcement decision. GuardianPolicy applies a
	// proposed change only after review.
	//
	// The header states the consequence: the worst this layer can do is MISINFORM
	// the player, never under-protect them. That asymmetry is why this untestable
	// file is considered safe to leave untested.
	// user edits a local draft; submitting proposes it, and GuardianPolicy
	// decides what happens next. Nothing on this panel writes to enforcement.
	// By non-const reference: control is only READ below. Nothing in any of the
	// three cases assigns to a ControlState field -- edits go to m_Draft. So const
	// would be more accurate, and `const ControlState &` would also prevent the
	// mistake of writing to the model in future.
	//
	// Iterating the MODEL's controls, not a separate list: the model decides which
	// rows exist, their order, their labels and their Kind. This layer maps Kind to
	// a widget and nothing more.
	for (ControlState &control : m_Model.Controls)
	{
		// The widget dispatch. A switch over the three ControlKind values, with no
		// default -- so adding a fourth kind would make this silently draw nothing
		// rather than fail to compile. Inconsistent with StateName above, which does
		// have a default.
		//
		// Each case is braced because two of them declare locals named `value`, which
		// would otherwise collide in the same switch scope.
		switch (control.Kind)
		{
		case ControlKind::Toggle:
		{
			// A LOCAL copy seeded from the model's enforced value.
			//
			// control.Toggling is documented in GuardianToolsView.h as the value actually
			// ENFORCED -- not the resting setting and not a pending one. So the checkbox
			// shows the truth, and a pending relaxation does not tick itself on.
			//
			// Copying to a local is required because ImGui::Checkbox writes through the
			// pointer. Passing &control.Toggling would let the UI mutate the model, which
			// is exactly the write this module's design forbids.
			bool value = control.Toggling;

			// BeginDisabled rather than skipping the row: a restriction the
			// player cannot see is not a restriction they can understand.
			// Disables the row rather than skipping it.
			//
			// The pre-existing comment gives the reason and it is the right one: a
			// restriction the player cannot SEE is not a restriction they can understand.
			// Skipping would hide the control entirely; greying it out shows that the
			// setting exists and cannot currently be changed.
			//
			// GuardianToolsView.h adds that Enabled is false only while an action is
			// unavailable, never as a way of hiding a restriction -- so this path is not
			// being used to conceal anything.
			//
			// BeginDisabled/EndDisabled are state, not widgets: the EndDisabled below must
			// pair with this even when the checkbox is not drawn.
			ImGui::BeginDisabled(!control.Enabled);
			// Returns true only on the frame the user clicks, so the body runs once per
			// interaction rather than every frame.
			//
			// control.Label.c_str() doubles as the widget's visible text AND its ImGui ID.
			// That is why the labels below are compared as exact strings: the label is
			// load-bearing, not cosmetic. Changing one breaks the mapping silently -- the
			// checkbox would stop being recognised and edits to that setting would be
			// dropped.
			if (ImGui::Checkbox(control.Label.c_str(), &value))
			{
				// Apply to the DRAFT, not to the model and not to anything the
				// state machine enforces.
				// LABEL-STRING DISPATCH -- the weakest point in this file.
				//
				// The model supplies the label as data and this code re-derives which settings
				// field it means by comparing English text. Nothing ties the two together: the
				// label comes from GuardianToolsView's construction, and these comparisons come
				// from here.
				//
				// DEFECT (silent failure): rename a label in the view model and this branch
				// stops matching. The checkbox still works, the draft still flips m_DraftDirty,
				// and no setting changes -- the row becomes a control that appears to work and
				// does nothing. There is no default branch, so an unmatched label is silently
				// ignored rather than reported.
				//
				// The same applies in reverse: ADD a toggle row to the model and it renders as
				// a checkbox that silently discards edits.
				//
				// The fix would be for ControlState to carry an identifier or enum alongside
				// its Label, so the mapping is data rather than string comparison. As written,
				// the two files must be edited in lockstep and nothing enforces it.
				if (control.Label == "Chat with other players")
					m_Draft.ChatRestricted = value;
				else if (control.Label == "Night-time play curfew")
					m_Draft.NightHoursEnforced = value;
				else if (control.Label == "Purchase limits")
					m_Draft.PurchaseLimitsEnforced = value;
				// Marks the draft as having unsaved edits -- which is what stops SetPolicy from
				// overwriting it (see the guard there).
				//
				// Set unconditionally within the branch, including when the label matched
				// nothing. So a mismatched label leaves the panel permanently dirty with no
				// actual edit recorded: the draft is then pinned against policy updates for
				// the life of the layer, compounding the label defect above.
				//
				// Never cleared anywhere in this file. There is no submit button and no revert,
				// so once set, the draft is frozen unless the layer is destroyed.
				m_DraftDirty = true;
			}
			ImGui::EndDisabled();

			// The "awaiting review" marker, shown only for rows with a pending change.
			//
			// Split across two identical if-statements with SameLine between them rather
			// than one if-block. SameLine itself is a layout call that affects the NEXT
			// item, so it cannot be called inside the same branch that decides whether to
			// draw -- this shape is the standard ImGui idiom for conditional SameLine.
			//
			// The duplicated condition is the cost of that idiom. An alternative would be
			// to compose the suffix into a string and draw it in one call.
			if (control.PendingChange)
				ImGui::SameLine();
			if (control.PendingChange)
				ImGui::TextDisabled("(awaiting review)");
			break;
		}

		// The numeric-control case. Currently exactly one row uses it -- the daily
		// playtime limit -- which the hard-coded slider label below also assumes.
		case ControlKind::Value:
		{
			// WHERE THE DISPLAYED NUMBER COMES FROM, and it differs from the toggle case in
			// a way worth understanding.
			//
			// Pending change  -> show the DRAFT's value, i.e. what the user is editing.
			// No pending change -> show the model's enforced value.
			//
			// So for a value row a pending edit IS visible in the slider, whereas a pending
			// toggle relaxation is NOT reflected in the checkbox. That is arguably the
			// right asymmetry -- a number the user is dragging should track the drag, and
			// the "(awaiting review)" marker below says it is not yet in force -- but it
			// does mean the two row kinds behave differently.
			//
			// Reads DailyPlaytimeMinutes directly rather than through any per-row mapping,
			// which is why this case only works for the playtime row. A second Value row
			// would silently edit the same field.
			int value = control.PendingChange ? m_Draft.DailyPlaytimeMinutes : control.Value;
			// A clamp against the slider's own minimum, which is 30.
			//
			// DEFECT (logic, and it defeats the sentinel): GuardianSettings::kUnlimited is
			// 0 -- the documented "no cap" value -- and the draft legitimately holds it
			// when the user has asked for unlimited. This test treats any non-positive value
			// as invalid and falls back to control.Value.
			//
			// So an unlimited request cannot be represented in the slider: the moment the
			// draft holds kUnlimited, the displayed value snaps back to the enforced one,
			// and there is no way to reach unlimited through this control. The
			// "unlimited" label below is therefore reachable in the draft but never
			// consistent with what the slider shows.
			//
			// The correct guard would be against the slider's minimum specifically, or the
			// unlimited case would need its own widget.
			if (value <= 0)
				value = control.Value;

			ImGui::BeginDisabled(!control.Enabled);
			// The range is 30 to 480 minutes -- half an hour to eight hours.
			//
			// DEFECT (range is not policy-derived): both bounds are HARD-CODED here rather
			// than taken from the policy, so the slider's range and the enforced limits can
			// disagree: a policy with a maximum below 480 still offers the user up to
			// eight hours, and the request would then be clamped or rejected by whatever
			// enforces it.
			//
			// The label is a literal, so unlike the toggle rows this ImGui ID does not come
			// from the model. That is also what makes the hard-coded range safe for a
			// single row -- and what would break if a second Value row were added, since
			// two sliders sharing this label would share an ImGui ID and interfere.
			if (ImGui::SliderInt("Daily playtime limit (minutes)", &value, 30, 480))
			{
				m_Draft.DailyPlaytimeMinutes = value;
				m_DraftDirty = true;
			}
			ImGui::EndDisabled();

			// The same two-if SameLine idiom as the toggle case above, testing the DRAFT
			// rather than the model -- so the label reflects what the user has asked for.
			//
			// Given the clamp above, this state and the slider's displayed value are
			// generally out of step: the draft can say unlimited while the slider shows the
			// enforced number, with both drawn in the same row.
			//
			// Also duplicated: the condition is evaluated twice per frame, which is nothing,
			// and it is the price of SameLine's sequencing.
			if (m_Draft.DailyPlaytimeMinutes == GuardianSettings::kUnlimited)
				ImGui::SameLine();
			if (m_Draft.DailyPlaytimeMinutes == GuardianSettings::kUnlimited)
				ImGui::TextDisabled("unlimited");

			// The pending marker for value rows, identical in shape to the toggle case.
			//
			// Duplicated verbatim rather than factored, because the three cases are in
			// separate switch scopes and the block is three lines.
			if (control.PendingChange)
				ImGui::SameLine();
			if (control.PendingChange)
				ImGui::TextDisabled("(awaiting review)");
			break;
		}

		// The inert-action case, covering the Art 20(2) rights -- report, mute, block.
		case ControlKind::Action:
		{
			// Art 20(2) rights.
			//
			// Deliberately inert. A real report/mute/block flow is the game's
			// own -- moderation, an abuse team, a store's reporting API -- and
			// wiring a stub button that silently does nothing would be worse
			// than not drawing one. Studios replace these with their own
			// handlers; see docs/AGE_ASSURANCE.md.
			//
			// They are drawn even though inert, because the RIGHT to report and
			// block must be visible, and a panel that shows "Report a problem"
			// greyed out forever is not honouring Art 20(2).
			// ALWAYS disabled -- not conditional on control.Enabled. These buttons are
			// unconditionally inert in this build.
			//
			// The pre-existing comment explains the reasoning, and it is a deliberate
			// trade-off: the buttons are drawn (rather than omitted) because the RIGHT to
			// report and block must be VISIBLE, and a permanently greyed "Report a
			// problem" is judged to read as a broken feature rather than a studio's own
			// moderation flow.
			//
			// So the engine ships the affordance and leaves the behaviour to the studio,
			// which replaces these with their own handlers per docs/AGE_ASSURANCE.md.
			// A player on a build where nobody wired them up sees a control that does
			// nothing -- the module's one genuinely user-visible gap, and it is visible
			// rather than hidden by design.
			ImGui::BeginDisabled(true);
			// Unreachable in practice: BeginDisabled(true) means ImGui never reports a click.
			//
			// Kept anyway so the shape matches the other two cases and so a studio replacing
			// this block has the structure already in place.
			//
			// The empty body is the documented intent ("Intentionally empty"), not an
			// oversight. Note BeginDisabled still lets the button RENDER, which is the
			// whole point -- greyed but present.
			if (ImGui::Button(control.Label.c_str()))
			{
				// Intentionally empty. See note above.
			}
			ImGui::EndDisabled();
			break;
		}
		}
	}

	// The restriction list, gated on the model supplying any.
	//
	// The notes are DERIVED from the effective settings by the view, so a row that
	// is not actually restricted is never listed -- the panel cannot display a
	// restriction that is not in force. That is the anti-misinformation property
	// the view model's separation exists to provide.
	//
	// `empty() == false` rather than `!empty()`: longer, and inconsistent with the
	// other conditions in this file. Harmless.
	if (m_Model.RestrictionNotes.empty() == false)
	{
		ImGui::Separator();
		ImGui::TextWrapped("Currently restricted:");
		// By const reference -- BulletText takes a format string and the note's
		// characters are read, so no copy per row.
		//
		// BulletText's %s form is the safe one: a restriction string containing a
		// percent sign would be misread by a bare BulletText(note.c_str()).
		for (const std::string &note : m_Model.RestrictionNotes)
			ImGui::BulletText("%s", note.c_str());
	}

	// The matching End for the Begin at line 97 -- the last statement in the
	// function, reached only when Begin returned true. The false path has its own
	// End inside the early-out.
	//
	// Every ImGui::Begin in this file is matched on all paths, including the
	// early returns: the panel Begin (97 / 99-102 / 223), the indicator Begin
	// (84 / 86), and each BeginDisabled/EndDisabled pair in all three switch cases.
	// An unmatched one corrupts ImGui's window stack for the rest of the frame.
	ImGui::End();
}

} // namespace Wasteland::Age