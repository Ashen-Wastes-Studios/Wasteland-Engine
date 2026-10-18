#pragma once

// The machine OWNS an Animator by value (see m_Animator below), so this
// is a real dependency rather than a forward declaration.
#include "Wasteland/Animation/Animator.h"

// shared_ptr for AnimationClip: several states may legitimately point at
// one clip (a mirrored pair of animations), so shared ownership is
// the right call over copying clip data per state.
#include <memory>
#include <string>
// States are keyed by name and looked up on every trigger evaluation, so
// a hash map rather than a vector of linear scans.
#include <unordered_map>
#include <vector>

namespace Wasteland {

	namespace Animation {

		// A named animation state in a state machine.
		//
		// A state owns a clip plus the playback rules an animator would
		// otherwise re-implement per character: how fast it plays, whether it
		// loops, and how long the crossfade into it lasts.
		// A value type, stored by value in the map below. Adding a state copies
		// the whole struct including the clip pointer -- cheap.
		struct AnimationState
		{
			// The state stores its own name as well as being the map key. Redundant,
			// but it means a state is self-describing when iterated or
			// serialised. DEFECT (possible divergence, documented not fixed):
			// nothing here enforces that Name matches the key it is stored under.
			std::string Name;
			std::shared_ptr<AnimationClip> Clip;

			// Playback rate multiplier for this state, not a global animator speed:
			// two states in one machine can play at different rates (a fast
			// attack blended against a slow idle).
			float Speed = 1.0f;
			// Defaults to looping because most states in this engine are cycles
			// (idle, walk, run) and one-shot states are the exception.
			bool Looping = true;

			// Seconds to crossfade from the previous state. 0 means a hard
			// cut, which is right for a state change the player must notice
			// (getting shot) and wrong for one they must not (footstep to
			// footstep).
			// The DESTINATION state owns the default blend time, which is why an
			// incoming transition can override it (see AnimationTransition::Duration
			// below). A non-zero default also means a transition that does not set
			// one still blends rather than popping.
			float TransitionDuration = 0.15f;
		};

		// A trigger between two states.
		//
		// Triggers rather than continuous parameter thresholds, because every
		// state change a character actually needs is discrete: took damage,
		// died, started moving. A float parameter with a threshold is strictly
		// more machinery for the same result.
		// Value type in a VECTOR, not a map: transitions are ordered and two
		// transitions from the same state with the same trigger can
		// legitimately exist (a priority list). Nothing in the header says
		// which wins when both are armed.
		struct AnimationTransition
		{
			// A NAME, not a pointer or index: transitions are authored and
			// serialised as data, before the states they reference exist. That is
			// what Validate() below is for.
			std::string FromState;
			// Also a name, and also unvalidated at insert time -- AddTransition above
			// cannot check it because either state may be added afterwards.
			std::string ToState;

			// The condition that permits the transition. Empty means
			// unconditional, which is how "on entering this state, always go to
			// that one" is expressed (a death state leading to a ragdoll).
			// Empty means unconditional, which is how "always leave this state" is
			// written (a death state leading to a ragdoll). The consequence worth
			// knowing: an empty trigger makes the transition fire on EVERY
			// evaluation while its source state is current, so a machine that
			// never leaves the destination state will still take it each frame.
			// Appears intentional for one-way exits.
			std::string Trigger;

			// Seconds to blend. Overrides the destination state's own
			// TransitionDuration when non-negative, so one state can be
			// entered quickly from one context and slowly from another.
			// -1 is the "not set" sentinel and the default argument of TransitionTo
			// below, so the same value means both "defer to the destination state"
			// and "caller did not care". Any negative number works as the sentinel
			// since the test is documented as non-negative.
			float Duration = -1.0f;
		};

		// Transitions states based on triggers and an animator.
		//
		// The layer that makes animation controllable from gameplay without
		// writing code per character: a script sets a trigger ("Hit"), the
		// machine decides which state that means and how to get there.
		//
		// Deliberately small compared with Unity's AnimatorController -- no
		// sub-state machines, no blend trees, no synced layers. Those exist to
		// solve problems this engine does not have yet, and a simpler machine
		// that is understood beats a complete one that is not.
		// A value type, not a singleton: a character owns one, and copying a
		// machine copies its whole state table. That is affordable because
		// the state table is small by design (no blend trees, no layers).
		class AnimationStateMachine
		{
		public:
			// No m_CurrentState initialiser, so it starts EMPTY rather than naming a
			// state that may not exist yet. Every accessor below must therefore
			// tolerate a machine that has never been updated.
			AnimationStateMachine() = default;

			// ---- Authoring ----

			// Adds a state. Returns its name for readability; a duplicate name
			// replaces the previous state so re-authoring an existing machine
			// does not create two states with one identity.
			// Returns a REFERENCE to a string whose lifetime is not obvious: if the
			// map rehashes on a later AddState, the string it referred to may be
			// destroyed. The reference is safe only until the next insertion.
			const std::string &AddState(const AnimationState &state);

			// Convenience: adds a state around a clip.
			// The convenience overload, same reference caveat as above.
			const std::string &AddState(const std::string &name,
										const std::shared_ptr<AnimationClip> &clip);

			// Appends with no validation and no duplicate check: adding the same
			// transition twice makes two identical entries, and with an empty
			// Trigger both fire. Validate() does not check for duplicates.
			void AddTransition(const AnimationTransition &transition);

			// ---- Triggers ----

			// Arms a trigger. Consumed by the next Update.
			//
			// Consumed rather than latched: a trigger that stayed armed would
			// re-fire the transition every frame for as long as the condition
			// held, so a "Hit" set once would replay the hit animation
			// forever.
			// Arms a trigger for exactly one Update. Consumed rather than latched --
			// the reasoning above the declaration is that a latched "Hit" would
			// replay the hit animation every frame while the condition held.
			// DEFECT (no dedup, documented not fixed): nothing checks whether the
			// trigger was already armed, so calling SetTrigger("Hit") twice before
			// one Update is indistinguishable from calling it once. Harmless with
			// a bool-valued map, but it means the trigger carries no count.
			void SetTrigger(const std::string &trigger);

			// Exists so gameplay can cancel an armed trigger before the next Update.
			void ClearTrigger(const std::string &trigger);
			bool IsTriggerSet(const std::string &trigger) const;

			// ---- State control ----

			// Jumps to a state with no blend, for a teleport or a respawn.
			// Returns false if the state does not exist.
			// Hard cut, and the name says so. Returns false for an unknown state and
			// therefore leaves the machine untouched -- the caller can treat
			// false as "nothing happened" without inspecting further.
			bool SetState(const std::string &stateName);

			// Crossfades to a state. Returns false if the state does not exist.
			// The -1 default matches AnimationTransition::Duration's sentinel, so
			// "not specified" means the same thing in both places.
			// DEFECT (duration handling, documented not fixed): the default of
			// -1 is documented for Transition, but a caller passing 0 explicitly
			// gets a hard cut through the crossfade path. Whether m_TransitionTimer
			// is set to 0 or left alone is not visible from this header.
			bool TransitionTo(const std::string &stateName, float duration = -1.0f);

			// Inline because it is read every frame by gameplay queries. Note it
			// returns a reference to a member -- valid for the machine's lifetime.
			const std::string &GetCurrentState() const { return m_CurrentState; }
			const std::string &GetPreviousState() const { return m_PreviousState; }

			// True while a crossfade is still running.
			// A strict greater-than, so a crossfade of exactly 0 seconds reads as
			// NOT transitioning -- consistent with "hard cut is not a
			// transition" elsewhere in this header.
			bool IsTransitioning() const { return m_TransitionTimer > 0.0f; }

			// ---- Update ----

			// Advances the animator by deltaSeconds, first evaluating any
			// transition whose trigger is armed.
			//
			// Evaluating transitions BEFORE advancing means a trigger set this
			// frame takes effect on this frame. The other order costs a frame
			// of latency on every reaction, which is exactly the lag that makes
			// a hit animation feel late.
			// Two-phase: evaluate armed triggers, then advance the animator. The order
			// is the design decision -- see the comment above. No clamping of
			// deltaSeconds here, so a caller passing a huge or negative value (a
			// debugger break, a paused-window delta) is trusted.
			void Update(float deltaSeconds);

			// ---- Queries ----

			// Non-const overload hands out a mutable reference, so a caller can drive
			// the animator directly and bypass the machine's own timing -- e.g.
			// set the time while the machine thinks it is blending.
			Animator &GetAnimator() { return m_Animator; }
			const Animator &GetAnimator() const { return m_Animator; }

			bool HasState(const std::string &stateName) const;
			const AnimationState *FindState(const std::string &stateName) const;
			// size_t returned where the surrounding accessors use other widths;
			// consistent with the container it queries.
			size_t GetStateCount() const { return m_States.size(); }

			// ---- Validation ----

			// Checks that every transition names states that exist and that no
			// state is unreachable from the first. Returns false with a
			// description of the first problem.
			// out-param rather than bool alone: the caller gets the FIRST problem as
			// text, which is what an editor panel needs to show. Returns false and
			// writes a message; on success the message is left untouched (nothing
			// here clears it first, so a caller reusing one string across calls
			// could show a stale message).
			bool Validate(std::string &outError) const;

		private:
			// The transition to take for an armed trigger, or an empty string.
			// Returns an empty string for "no transition applies", which is why the
			// return type is a string rather than a pointer -- and why an empty
			// return is ambiguous with a transition whose fields are empty.
			// The first match in m_Transitions wins, so transition ORDER is the
			// priority, and the author controls priority purely by insertion order.
			std::string FindTriggeredTransition(const std::string &fromState) const;

			// Keyed by name, so AddState's documented "a duplicate name replaces"
			// behaviour falls out of operator[] assignment for free. Iteration
			// order is unspecified, so anything user-visible that walks states
			// must sort first.
			std::unordered_map<std::string, AnimationState> m_States;
			// A vector, so order IS meaningful (see FindTriggeredTransition above).
			// Validate() being the only thing that checks the referenced states
			// exist means a machine can hold dangling names until someone calls it.
			std::vector<AnimationTransition> m_Transitions;

			// Triggers armed this update, consumed by Update.
			// map<string, bool> rather than set<string>: SetTrigger/ClearTrigger/
			// IsTriggerSet are three distinct operations and a bool map expresses
			// all three without a second data structure. Update() is documented as
			// consuming triggers, so entries must be erased rather than merely
			// set false -- IsTriggerSet's meaning depends on that.
			std::unordered_map<std::string, bool> m_Triggers;

			// Owned by value, so a machine is self-contained and copyable. The cost
			// is that copying a machine copies the animator's internal state
			// mid-playback.
			Animator m_Animator;

			std::string m_CurrentState;
			std::string m_PreviousState;

			// Seconds remaining in the current crossfade.
			// Seconds REMAINING, not elapsed -- IsTransitioning tests it against 0,
			// which only reads correctly as a countdown. Update() must decrement it.
			float m_TransitionTimer = 0.0f;
			// The fade length that m_TransitionTimer counts down from. Kept separate
			// from AnimationState::TransitionDuration because a transition-level
			// override changes it per transition.
			float m_TransitionDuration = 0.0f;

			// The instance fading out during a transition, or -1.
			// Animator instance INDICES, -1 for none. The two-instance model: one
			// plays, one fades, and they are swapped when the timer expires. -1 as
			// the sentinel is checked explicitly rather than assumed.
			// DEFECT (no instance-cap check, documented not fixed): the header does
			// not show where these indices come from or what happens if the
			// animator has fewer than two instances, or if a transition is
			// requested while one is already fading.
			int m_FadingInstance = -1;
			int m_CurrentInstance = -1;
		};

	} // namespace Animation

} // namespace Wasteland
