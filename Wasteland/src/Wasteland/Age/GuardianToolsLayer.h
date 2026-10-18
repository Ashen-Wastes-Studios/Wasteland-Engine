// Wasteland::Age -- the engine-side guardian panel.
//
// GuardianToolsLayer draws GuardianToolsModel and does nothing else. Every
// decision -- what is enforced, what is shown, whether the indicator is up,
// what the wording says -- is made in GuardianToolsView, which is testable
// without a window or a GPU. This file is the part that could not be.
//
// WHY THE MODEL IS SEPARATE
//
// A compliance reviewer asking "does the guardian UI show the enforced
// setting?" is asking a question about logic. If that logic lived inside an
// OnImGuiRender() the only way to answer it would be to launch the editor and
// look. Splitting it means the answer is 82 assertions instead.
//
// THE LAYER CANNOT AFFECT ENFORCEMENT
//
// Nothing here can make a minor less protected. Editing a control produces a
// PROPOSED change, which GuardianPolicy applies only after review -- and which,
// while pending, resolves to the stricter of the two settings regardless. The
// worst this layer can do is misinform the player, never under-protect them.
// That asymmetry is deliberate: the untestable part of the module is the part
// with no ability to cause harm.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

// Include guard, below the file's design essay for the same reason as
// IAgeAssuranceProvider.h: the reasoning is the first thing a compliance
// reviewer should read.
#pragma once

// For the SetPolicy signature's parameter types in the included headers and
// the standard types used through them.
#include <string>

// Supplies GuardianPolicy and GuardianSettings, the types this layer stores
// and displays.

// Angle-bracket includes for engine headers here, while Renderer/
// ImGuiLayer.h uses quoted ones for the same tree. The codebase is
// inconsistent about which project include root is in effect.
#include <Wasteland/Age/GuardianPolicy.h>
// Supplies GuardianToolsModel -- the type that holds every decision about
// what the panel shows.

// This include is what makes the class testable at all: OnImGuiRender() below
// renders a model rather than deciding anything, so the logic can be exercised
// without a window or a GPU.
#include <Wasteland/Age/GuardianToolsView.h>
#include <Wasteland/Core/Layer.h>

// C++17 nested namespace, matching the rest of the Age module.
namespace Wasteland::Age
{

// A dockable panel showing guardian status and controls.
//
// Attach it like any other layer:
//
//     Application::Get().PushLayer(std::make_shared<Age::GuardianToolsLayer>());
//
// Constructing it is harmless. With no AgeService provider it renders the
// fail-closed panel -- protective settings on, indicator up, setup needed --
// which is the correct thing to show.
// The dockable guardian panel. A Layer, so it attaches to the application
// like any other.

// `::Wasteland::Layer` is fully qualified even though we are already inside
// `Wasteland::Age`. That qualification is required, not redundant: inside
// Wasteland::Age an unqualified `Layer` would first look for Age::Layer, and
// only fall back to Wasteland::Layer. If a Wasteland::Age::Layer were ever
// added, this would silently bind to it instead.
class GuardianToolsLayer : public ::Wasteland::Layer
{
public:
	// Defined in the .cpp, because it builds the initial GuardianToolsModel.
	GuardianToolsLayer();
	// Destructor, with `override` and no `virtual`. Correct and modern; note
	// ImGuiLayer.h in the same tree spells its destructor `~ImGuiLayer()` with
	// neither keyword.
	~GuardianToolsLayer() override;

	// Start closed. A panel that opens on launch and covers the game is a
	// panel studios disable, and a disabled panel protects nobody.
	//
	// The persistent ART 8(1) INDICATOR is not gated on this -- it is drawn
	// whether the panel is open or not, because an indicator the player has to
	// open a window to see is not an indicator.
	// Opens or closes the panel. STATIC, so the flag is process-wide rather than
	// per-instance.

	// The header's comment gives the reasoning: a panel that opens on launch and
	// covers the game is a panel studios disable, and a disabled panel protects
	// nobody. So it starts closed.
	static void SetVisible(bool visible);
	// Reads the same process-wide flag.

	// Static state means two GuardianToolsLayer instances cannot be independently
	// shown or hidden -- which is consistent with the design, since there is only
	// ever one guardian panel.
	static bool IsVisible();

	// The persistent indicator. Off by default; a studio turns it on with a
	// single call at startup:
	//
	//     Age::GuardianToolsLayer::SetIndicatorEnabled(true);
	//
	// Off by default rather than on because this ships into games nobody has
	// reviewed yet, and a surprise banner over someone's build is a worse
	// first impression than a documented one-line opt-in. See
	// docs/AGE_ASSURANCE.md -- studios shipping into the EU should enable it.
	// Controls the persistent ART 8(1) INDICATOR, which is drawn whether or not
	// the panel is open.

	// Off by default, per the comment above: this ships into unreviewed builds,
	// and an unexplained banner is a worse first impression than a documented
	// one-line opt-in. Studios shipping into the EU are expected to enable it.

	// DEFECT (minor): static, like SetVisible, so the indicator cannot be scoped
	// per window or per session. Fine for a single-process engine; it is a
	// constraint on reuse rather than a bug today.
	static void SetIndicatorEnabled(bool enabled);
	// Reads the indicator flag.
	static bool IsIndicatorEnabled();

	// Feed the panel a policy to display. The layer stores a copy: AgeService
	// hands out a fresh GuardianPolicy per call, and holding a reference to a
	// temporary would be a dangling read.
	// Supplies the policy to display.

	// Takes a const reference but STORES A COPY. The header explains why:
	// AgeService hands out a fresh GuardianPolicy per call, so holding a
	// reference to the argument would dangle as soon as this call returned.

	// This is the only member that brings policy data in; nothing else on this
	// class reads a live policy, which is what makes the panel unable to affect
	// enforcement.
	void SetPolicy(const GuardianPolicy &policy);

	// The Layer hook. Renders the panel if visible, and the indicator
	// unconditionally -- so a player who never opens the panel still sees the
	// persistent indicator.
	void OnImGuiRender() override;

private:
	// The persistent indicator, drawn outside the panel. Separate from
	// OnImGuiRender's panel so it is unaffected by the panel being closed.
	// Draws the persistent indicator. Private and separate from the panel body
	// precisely so closing the panel cannot suppress it.

	// DEFECT (unused parameter, apparent): it takes the model by const reference
	// but the header gives no reason the indicator would need it -- an indicator
	// is a fixed badge, not a view of policy state. The .cpp would settle whether
	// it actually reads `model`; if it does not, the parameter is dead and could
	// be dropped. Cannot be confirmed from this header alone.
	void RenderIndicator(const GuardianToolsModel &model);

	// The model the panel renders. Holds no decisions of its own -- it is built
	// from a policy and then only read.
	GuardianToolsModel m_Model;

	// Working copy of the settings the user is editing. The panel edits this,
	// not the policy -- nothing here mutates what is enforced.
	// The user's in-progress edits, kept separate from m_Model so that nothing
	// here mutates what is actually enforced.

	// This separation is the safety property the file's header describes: edits
	// produce a PROPOSED change that GuardianPolicy applies only after review,
	// and while pending the stricter of the two settings wins. The worst this
	// layer can do is misinform the player, never under-protect them.
	GuardianSettings m_Draft;

	// Set when the user has changed something in this session but not yet
	// submitted it.
	// Whether the draft differs from the last submitted state. In-class
	// initialiser, so a freshly constructed layer is not dirty.

	// Set when the user changes something and not yet cleared on submit, per the
	// comment above.
	bool m_DraftDirty = false;
};

} // namespace Wasteland::Age