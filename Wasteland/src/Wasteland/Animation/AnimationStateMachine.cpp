// PCH first: this TU gets the engine-wide include set before anything else,
// which is why the two headers below need no <string>/<vector> of their own.
#include "wlpch.h"
#include "Wasteland/Animation/AnimationStateMachine.h"

// std::find, used by the reachability walk in Validate.
#include <algorithm>
// DEFECT (documented, not fixed): <cmath> appears to be an UNUSED include.
// No cmath facility is called anywhere in this translation unit -- the only
// maths here is comparison of floats. The Animator, which does need fmod,
// includes it itself. Harmless but misleading: a reader looking for the
// rounding/fmod behaviour of the fade arithmetic is sent to the wrong file.
#include <cmath>

// Two nested namespaces matching the header. Wasteland is the engine root;
// Animation groups the animator, clips, skeleton and state machine so they
// can be added or removed as a unit.
namespace Wasteland {

		// The animation subsystem, as opposed to the core namespace one level up.
	namespace Animation {

				// Registers a state, or replaces one that already has this name.
		//
				// Returns a const reference to the STORED name rather than to the argument,
				// so the caller can hold it while the machine keeps growing: m_States is an
				// unordered_map and rehashes do not invalidate references to elements, only
				// iterators, so the reference stays valid. Returning the parameter by
				// reference would dangle as soon as the caller reused or cleared its name.
		const std::string &AnimationStateMachine::AddState(const AnimationState &state)
		{
			// An unnamed state can never be transitioned to by name, so it
			// would be unreachable forever. Rejecting it up front beats
			// authoring a machine that silently never changes state.
						// Rejects the unnamed state rather than storing it. Transitions address
						// states by name, so an unnamed one is unreachable and would also collide
						// with any other unnamed state under operator[].
			if (state.Name.empty())
			{
								// Function-local static, so every failed call returns the SAME object.
								// A non-static local would dangle the instant AddState returned, and the
								// signature promises a reference.
				static const std::string invalid;
				return invalid;
			}

						// operator[], not insert(): assignment on an existing key overwrites the
						// state in place rather than adding a second one with the same identity.
			m_States[state.Name] = state;
						// Second lookup to produce the reference. Redundant hashing, but it is what
						// makes the return value a reference to storage this object owns rather
						// than to the caller's temporary.
			return m_States[state.Name].Name;
		}

				// Convenience overload for the common authoring call: a name and a clip,
				// with Speed/Looping/TransitionDuration left at their struct defaults
				// (1.0, looping, 0.15s crossfade). Anything else needs the full overload.
		const std::string &AnimationStateMachine::AddState(const std::string &name,
														   const std::shared_ptr<AnimationClip> &clip)
		{
						// Default-constructed, which is what supplies the playback defaults; the
						// two fields set here are the only ones the caller passed.
			AnimationState state;
						// A shared_ptr copy, not a move: the caller keeps its handle and the state
						// holds its own, so dropping the caller's reference cannot kill the clip
						// mid-playback.
			state.Name = name;
			state.Clip = clip;
						// Delegated rather than duplicated, so the empty-name rejection and the
						// replace-on-duplicate behaviour cannot be implemented twice and drift.
			return AddState(state);
		}

				// Adds a transition. No error reporting: see the note on the guard below.
		void AnimationStateMachine::AddTransition(const AnimationTransition &transition)
		{
						// DEFECT (documented, not fixed): a transition naming an empty From or To
						// state is SILENTLY DROPPED. Nothing is logged and nothing is recorded, so
						// Validate -- the one place a studio would look for authoring errors --
						// cannot report it either, because the transition was never stored. An
						// authoring typo on exactly one string disappears with no diagnostic
						// anywhere. Validate would catch it as "state X is unreachable" in some
						// cases, but a transition dropped for an empty DESTINATION produces no such
						// symptom at all.
			if (transition.FromState.empty() || transition.ToState.empty())
				return;
						// A plain vector append: order is authoring order, and AddState/AddTransition
						// are the only mutators, so there is no reallocation bookkeeping here.
			m_Transitions.push_back(transition);
		}

				// Arms a trigger for the next Update. Stores true rather than a count, so
				// arming the same trigger twice in one frame is idempotent.
		void AnimationStateMachine::SetTrigger(const std::string &trigger)
		{
						// Empty names are rejected for the same reason empty state names are:
						// ClearTrigger could never remove the entry, so it would latch forever.
			if (!trigger.empty())
				m_Triggers[trigger] = true;
		}

				// erase, not assign false: a cleared trigger is absent, which is what makes
				// the "consume every trigger" clear() in Update unnecessary for this path.
		void AnimationStateMachine::ClearTrigger(const std::string &trigger)
		{
			m_Triggers.erase(trigger);
		}

				// Both halves of the test are needed. find() alone would report true for a
				// trigger that was explicitly cleared to false -- the map holds bools, not
				// a set, so the value is part of the state.
		bool AnimationStateMachine::IsTriggerSet(const std::string &trigger) const
		{
			auto it = m_Triggers.find(trigger);
			return it != m_Triggers.end() && it->second;
		}

				// Existence probe. find() rather than operator[] because a const method
				// cannot insert, and because inserting would change the answer.
		bool AnimationStateMachine::HasState(const std::string &stateName) const
		{
			return m_States.find(stateName) != m_States.end();
		}

				// Returns nullptr rather than a reference to a default state, so a caller
				// cannot accidentally play a nameless state with default playback settings.
		const AnimationState *AnimationStateMachine::FindState(const std::string &stateName) const
		{
			auto it = m_States.find(stateName);
						// Pointer into the map's storage. Held across an AddState call it stays
						// valid (unordered_map references survive rehash) but across an erase of
						// that same name it dangles; both callers below use it immediately.
			return (it == m_States.end()) ? nullptr : &it->second;
		}

				// Hard cut: no blend with whatever was playing. This is the respawn /
				// teleport entry point; TransitionTo is the blended one.
		bool AnimationStateMachine::SetState(const std::string &stateName)
		{
						// Unknown name is a false return, not an exception and not a silent no-op,
						// so a caller can detect a typo in a gameplay script.
			const AnimationState *state = FindState(stateName);
			if (!state)
				return false;

			// A hard cut must also clear any in-flight crossfade, or the old
			// instance would keep fading in the background after the caller
			// explicitly asked for an instant change.
						// Cancel any in-flight crossfade. The HasInstance guard matters because
						// AddInstance returns -1 for a null or empty clip, and m_FadingInstance
						// keeps an id from an earlier frame.
			if (m_FadingInstance >= 0 && m_Animator.HasInstance(m_FadingInstance))
				m_Animator.RemoveInstance(m_FadingInstance);
						// Reset unconditionally, not only inside the guard above: the handle may
						// already be stale, and the timer must not keep running either way.
			m_FadingInstance = -1;
						// Zeroes IsTransitioning() as a side effect, which is correct for a hard
						// cut -- there is nothing in flight once the old instance is gone.
			m_TransitionTimer = 0.0f;

						// Previous is only recorded from a real prior state, so the first SetState
						// leaves m_PreviousState empty rather than aliasing the new name.
						// NOTE: setting the SAME state twice makes previous == current. Nothing
						// checks for that, so a consumer distinguishing them by comparison will
						// see them as equal after a redundant SetState.
			if (!m_CurrentState.empty())
				m_PreviousState = m_CurrentState;
			m_CurrentState = stateName;

						// The outgoing instance is removed only if it is still live. An id from an
						// earlier frame whose instance the animator already dropped is tolerated.
			if (m_CurrentInstance >= 0 && m_Animator.HasInstance(m_CurrentInstance))
				m_Animator.RemoveInstance(m_CurrentInstance);

						// May return -1: AddInstance rejects a null or EMPTY clip, and a state may
						// legitimately hold one (see the empty-clip check in Validate).
			m_CurrentInstance = m_Animator.AddInstance(state->Clip);

						// Playback settings applied only on success. A -1 handle would silently
						// redirect SetSpeed/SetLooping onto whichever instance owns that id.
			if (m_CurrentInstance >= 0)
			{
								// Speed and Looping come from the STATE, overriding the clip's own
								// authored values, so one clip can play differently per state.
				m_Animator.SetSpeed(m_CurrentInstance, state->Speed);
				m_Animator.SetLooping(m_CurrentInstance, state->Looping);
			}

			return true;
		}

				// Crossfades to a state. The duration parameter is the per-call override;
				// the state's own TransitionDuration is the fallback.
		bool AnimationStateMachine::TransitionTo(const std::string &stateName, float duration)
		{
			const AnimationState *state = FindState(stateName);
			if (!state)
				return false;

						// Negative means "not specified", which is why the default in the header is
						// -1.0f rather than 0.0f: zero is a legitimate request for a hard cut.
						// DEFECT (documented, not fixed): only NEGATIVE is treated as unspecified.
						// A caller passing a NaN gets NaN through -- the comparison is false, so
						// fadeDuration becomes the state default, but any caller that passes NaN
						// intending "hard cut" is silently given the state default instead.
			float fadeDuration = (duration >= 0.0f) ? duration : state->TransitionDuration;

			// Fade the outgoing state instead of removing it, so the two
			// overlap. Removing outright is a hard cut, which is what
			// SetState is for.
						// The outgoing instance is FADED rather than removed, so it overlaps the
						// incoming one. This is the whole difference from SetState above.
			if (m_CurrentInstance >= 0 && m_Animator.HasInstance(m_CurrentInstance))
			{
								// Epsilon guard: a fade of effectively zero seconds would leave the
								// outgoing instance alive at a weight nothing ever reduces, so a
								// zero-length fade degrades to the immediate removal two lines below.
				if (fadeDuration > 1e-6f)
				{
					m_Animator.FadeOutInstance(m_CurrentInstance, fadeDuration);
										// The handle is remembered so Update can clear it when the fade expires.
										// DEFECT (documented, not fixed): this OVERWRITES the handle of a fade
										// started by an earlier TransitionTo that is still running. Starting a
										// second transition mid-crossfade loses track of the first outgoing
										// instance. Nothing leaks -- the Animator removes it when its own weight
										// reaches zero -- but IsTransitioning() then describes only the newest
										// fade, and the machine no longer knows an older instance is still alive.
					m_FadingInstance = m_CurrentInstance;
				}
				else
				{
										// Zero-duration path: remove rather than fade, for the reason given at the
										// epsilon guard above.
					m_Animator.RemoveInstance(m_CurrentInstance);
					m_FadingInstance = -1;
				}
			}

						// Same "only from a real prior state" rule as SetState.
			if (!m_CurrentState.empty())
				m_PreviousState = m_CurrentState;
			m_CurrentState = stateName;

						// Adds the incoming instance. AddInstance (rather than replacing) is what
						// makes the two instances coexist for the animator to blend.
			m_CurrentInstance = m_Animator.AddInstance(state->Clip);
			if (m_CurrentInstance >= 0)
			{
				m_Animator.SetSpeed(m_CurrentInstance, state->Speed);
				m_Animator.SetLooping(m_CurrentInstance, state->Looping);
			}

						// DEFECT (documented, not fixed): m_TransitionDuration is WRITE-ONLY.
						// A search of the whole tree finds exactly two occurrences -- its
						// declaration in the header and this assignment -- and no read anywhere.
						// The engine has no accessor for it either. The authoritative value is
						// m_TransitionTimer, the countdown that drives IsTransitioning(); this
						// field records the original length and is never consulted.
			m_TransitionDuration = fadeDuration;
						// Starts the countdown AT the full duration, so IsTransitioning() is true
						// for the whole blend. With fadeDuration 0 the timer is 0 and the machine
						// reports no transition in progress, matching the hard-cut path.
			m_TransitionTimer = fadeDuration;

			return true;
		}

				// Returns the destination name for the first armed trigger that has a
				// transition out of fromState, or an empty string for "stay put".
				// Empty string doubles as the failure value, which is why a state may not
				// be renamed to "" -- AddState already rejects that.
		std::string AnimationStateMachine::FindTriggeredTransition(const std::string &fromState) const
		{
			// Check every armed trigger rather than scanning transitions
			// outer: a trigger set this frame should be able to fire whichever
			// transition it belongs to.
						// Triggers outer, transitions inner -- deliberate, so a trigger armed this
						// frame can fire whichever transition belongs to it rather than only the
						// transition that happens to be checked first.
			//
						// DEFECT (documented, not fixed): this makes PRIORITY NON-DETERMINISTIC.
						// m_Triggers is an unordered_map, so when two transitions leave the same
						// state under two different triggers and both are armed in one frame,
						// which one wins depends on the hash iteration order, and that can differ
						// between runs or between platforms. m_Transitions (authoring order) is
						// the inner loop and cannot break the tie. A machine that relies on "Hit"
						// taking priority over "Move" is relying on this by accident.
			for (const auto &trigger : m_Triggers)
			{
				if (!trigger.second)
					continue;

								// Linear scan of the transitions for each armed trigger: O(triggers x
								// transitions) per frame. Fine at the scale this engine targets, and it
								// keeps the data structures to two maps and a vector.
				for (const AnimationTransition &transition : m_Transitions)
				{
										// Source-state filter. Transitions from other states are ignored rather
										// than rejected, so one machine definition can serve every state.
					if (transition.FromState != fromState)
						continue;
					if (transition.Trigger != trigger.first)
						continue;
										// Copy of the destination name into the return value. The transition stays
										// in m_Transitions, so returning a reference would be safe, but by value
										// keeps the "empty means none" contract obvious.
					return transition.ToState;
				}
			}
			return std::string();
		}

				// The per-frame entry point: resolve a transition, consume triggers, advance
				// the animator, and age the fade bookkeeping.
		void AnimationStateMachine::Update(float deltaSeconds)
		{
						// A negative delta (a clock that went backwards, or a debugger stepping)
						// becomes zero rather than playing time in reverse. Clamping instead of
						// returning early, so a paused frame still re-evaluates the pose.
			float dt = (deltaSeconds > 0.0f) ? deltaSeconds : 0.0f;

			// Transitions are evaluated BEFORE the animator advances, so a
			// trigger armed this frame takes effect this frame. The other
			// order costs a frame of latency on every reaction, which reads as
			// the hit animation being late.
						// Transitions are only evaluated FROM a current state. With none, an armed
						// trigger has no source to match and is simply consumed below.
			if (!m_CurrentState.empty())
			{
								// By value: FindTriggeredTransition returns a std::string, and this is a
								// local that dies at the end of the block.
				std::string target = FindTriggeredTransition(m_CurrentState);

				if (!target.empty())
				{
										// The transition is looked up AGAIN here, because FindTriggeredTransition
										// returned only the destination name and the per-transition Duration
										// override was not carried back with it. Two scans where one would do.
					// Find the transition again to pick up its per-transition
					// duration override.
										// -1.0f, not 0.0f: the sentinel that tells TransitionTo to use the
										// destination state's own TransitionDuration. Initialize to the sentinel
										// so a transition that is not found on the second scan still behaves.
					float duration = -1.0f;
					for (const AnimationTransition &transition : m_Transitions)
					{
												// Matching on BOTH ends, so a second transition between the same pair can
												// override the first -- if two exist, the first in authoring order wins and
												// the second is silently ignored.
						if (transition.FromState == m_CurrentState &&
							transition.ToState == target)
						{
							duration = transition.Duration;
							break;
						}
					}

										// The actual state change, with the fade the transition asked for.
					TransitionTo(target, duration);
				}
			}

						// DEFECT (documented, not fixed): clearing the WHOLE map discards triggers
						// that did not fire and were aimed at a state the machine is not in. A
						// gameplay system that arms "Hit" one frame before the animation reaches
						// the hit-receiving state loses the trigger entirely -- there is no
						// latch-until-consumed path. This is the deliberate one-shot trade-off
						// described above, but the cost lands on cross-state arms specifically.
			// Consume every trigger, armed or not. A trigger is a one-shot
			// edge; leaving it armed makes the transition re-fire on every
			// subsequent frame, which for "Hit" means a hit animation that
			// restarts forever.
			m_Triggers.clear();

			// Update into a local pose, then copy into the animator's cached
			// result. Passing GetLastPose() straight back in would require a
			// mutable reference to its own member and alias the output with
			// the input.
						// A LOCAL pose, then copied into the animator's cache by Update itself
						// (Animator::Update ends with m_LastPose = outPose). The local exists so
						// this call cannot alias its own output -- see the note above.
			Pose pose;
						// Advances every instance and blends into the local pose.
						// DEFECT (documented, not fixed): the resulting `pose` local is never
						// read after this line. Animator::Update already caches it into m_LastPose,
						// which is what GetLastPose() returns, so the local pose is a per-frame
						// allocation-and-copy that nothing consumes -- Pose holds a vector of bone
						// transforms. The justification in the comment above (that GetLastPose()
						// cannot be passed back in) does not hold: GetLastPose() is on the
						// ANIMATOR, and AnimationStateMachine holds no pose member, so there is no
						// aliasing to avoid.
			m_Animator.Update(dt, pose);

			if (m_TransitionTimer > 0.0f)
			{
								// Counts the fade down. Driven by the caller's delta rather than the
								// animator's own fade state, so the two can disagree if the animator
								// refuses to remove an instance for any reason.
				m_TransitionTimer -= dt;
				if (m_TransitionTimer <= 0.0f)
				{
					m_TransitionTimer = 0.0f;
					// The fade has run out, so drop the outgoing instance. The
					// animator already removed it when its weight hit zero, so
					// this only clears the stale handle.
										// Bookkeeping only. Animator::Update already erased the instance when its
										// weight reached zero, so this cannot leak -- at worst it clears the
										// handle a few frames late.
					m_FadingInstance = -1;
				}
			}
		}

				// Authoring-time validation. Reports the FIRST problem and stops, so a
				// studio fixing a machine sees one error per run rather than a list.
		bool AnimationStateMachine::Validate(std::string &outError) const
		{
						// Cleared first so a caller that ignores the bool return still gets a sane
						// empty string rather than whatever was in the variable before.
			outError.clear();

						// A machine with no states is an error rather than a harmless no-op: it
						// would silently never animate anything.
			if (m_States.empty())
			{
				outError = "state machine has no states";
				return false;
			}

			for (const auto &entry : m_States)
			{
								// Only clips that are PRESENT and EMPTY are flagged. A null clip passes
								// this check and then fails in SetState/TransitionTo instead, where
								// AddInstance returns -1 and no instance plays.
				if (entry.second.Clip && entry.second.Clip->IsEmpty())
				{
					outError = "state '" + entry.first + "' has an empty clip";
					return false;
				}
			}

						// Both endpoints must name states that exist, in either direction of the
						// check below.
			for (const AnimationTransition &transition : m_Transitions)
			{
				if (!HasState(transition.FromState))
				{
					outError = "transition from unknown state '" +
							   transition.FromState + "'";
					return false;
				}
				if (!HasState(transition.ToState))
				{
					outError = "transition to unknown state '" +
							   transition.ToState + "'";
					return false;
				}
			}

			// Every state must be reachable, otherwise a typo in a transition
			// leaves a state that can never play and nothing reports it.
			// Reachability is measured from the first state, since the machine
			// has no separately designated start state.
						// Reachability from an arbitrary start state. DEFECT (documented, not
						// fixed): m_States is an unordered_map, so "the first state" is whatever
						// hashes first -- it is not the authored first state and not stable across
						// builds. A machine with two legitimate entry points therefore reports the
						// states reachable only from the second as UNREACHABLE, and which ones get
						// reported can change with insertion order or platform.
			std::vector<std::string> reachable;
			std::vector<std::string> pending;
						// The implicit start state. The header documents that the machine has no
						// designated start, so this stand-in is arbitrary -- see the note above.
			pending.push_back(m_States.begin()->first);

			while (!pending.empty())
			{
								// Copied by value rather than moved: the pop would otherwise invalidate the
								// slot, and a std::move here would need the string to be reset anyway.
				std::string current = pending.back();
				pending.pop_back();

								// std::find over a vector, so the walk is O(n^2) in the number of states.
								// Irrelevant at this scale; would not be at hundreds of states.
				if (std::find(reachable.begin(), reachable.end(), current) != reachable.end())
					continue;
				reachable.push_back(current);

				for (const AnimationTransition &transition : m_Transitions)
				{
					if (transition.FromState == current)
												// Edges are pushed without checking whether the destination is already
												// queued, so the pending list can hold duplicates. Harmless -- the
												// reachable test on the next pop discards them.
						pending.push_back(transition.ToState);
				}
			}

						// Every state must appear in the reachable set. Compared against entry
						// .first, which is the map KEY, so a state whose Name field disagrees with
						// its key would be reported under the key -- AddState keeps them in sync by
						// keying on Name, so they cannot actually diverge.
			for (const auto &entry : m_States)
			{
				if (std::find(reachable.begin(), reachable.end(), entry.first) ==
					reachable.end())
				{
					outError = "state '" + entry.first + "' is unreachable";
					return false;
				}
			}

						// True when every check above passed. outError holds the reason otherwise.
			return true;
		}

		// Closing the inner namespace; see the note at the top of the file.
	} // namespace Animation

// Closing the engine root.
} // namespace Wasteland
