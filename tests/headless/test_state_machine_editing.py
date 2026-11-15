"""State-machine authoring API -- the operations the Animator panel's canvas
and inspectors call (Task A.6).

The panel itself is ImGui and cannot run headlessly, but every edit it makes is a
call on AnimationStateMachine or AnimationAsset, and those CAN be executed and
asserted here. A drag is RemoveState/AddState + a StatePositions write; a wire is
AddTransition; a delete is RemoveTransition; an inspector field is one of the
setters.

The persistence check matters as much as the in-memory one: node positions are
the only state that lives outside the machine, so a round trip through the
serializer is what proves a drag survives a save.
"""

import os
import sys
import tempfile

from Wasteland import Animation

PASSED = 0
FAILED = []


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def build_machine():
    machine = Animation.AnimationStateMachine()
    machine.AddStateFromClip("Idle", None)
    machine.AddStateFromClip("Walk", None)
    machine.AddStateFromClip("Run", None)
    return machine


def add_wire(machine, from_state, to_state, trigger=""):
    transition = Animation.AnimationTransition()
    transition.FromState = from_state
    transition.ToState = to_state
    transition.Trigger = trigger
    transition.Duration = -1.0
    machine.AddTransition(transition)


def test_remove_state():
    machine = build_machine()

    check(machine.RemoveState("Walk"), "RemoveState reports success for a state that exists")
    check(machine.GetStateCount() == 2, "the state count drops by one, got %d" % machine.GetStateCount())
    check(machine.GetStates().get("Walk") is None, "the removed state is gone from the map")

    check(not machine.RemoveState("Walk"), "removing an absent state returns false")
    check(machine.GetStateCount() == 2, "a failed remove does not change the count")

    # The documented contract: transitions naming the removed state are NOT
    # dropped. Silently deleting authored transitions would lose work the author
    # can see on the canvas, and would make a delete's effect depend on how the
    # machine happens to be wired.
    machine = build_machine()
    add_wire(machine, "Idle", "Walk")
    machine.RemoveState("Walk")
    transitions = machine.GetTransitions()
    check(len(transitions) == 1, "the transition naming the removed state survives, got %d"
          % len(transitions))
    check(transitions[0].ToState == "Walk", "and it still points at the removed state")


def test_remove_transition():
    machine = build_machine()
    add_wire(machine, "Idle", "Walk")
    add_wire(machine, "Idle", "Run", "sprint")

    removed = machine.RemoveTransition("Idle", "Walk", "")
    check(removed, "RemoveTransition matches on the full triple")
    remaining = machine.GetTransitions()
    check(len(remaining) == 1, "one transition remains, got %d" % len(remaining))
    check(remaining[0].ToState == "Run", "and it is the sprint one")

    # An empty trigger is a REAL value here, not a wildcard: unconditional
    # transitions are a documented state, and a wildcard would make it impossible
    # to remove one without removing the other.
    check(not machine.RemoveTransition("Idle", "Run", ""),
          "an empty trigger does not match a transition with a trigger")
    check(not machine.RemoveTransition("Idle", "Run", "missing"),
          "a wrong trigger does not match")


def test_transition_priority_is_insertion_order():
    """GetTransitions returns insertion order, and that order IS the priority.

    The panel indexes transitions by position, so this order is what the list
    shows and what the machine evaluates.
    """
    machine = build_machine()
    add_wire(machine, "Idle", "Walk", "go")
    add_wire(machine, "Idle", "Run", "go")

    transitions = machine.GetTransitions()
    check([t.ToState for t in transitions] == ["Walk", "Run"],
          "insertion order is preserved, got %s" % [t.ToState for t in transitions])


def test_state_setters():
    machine = build_machine()
    machine.SetStateSpeed("Idle", 0.5)
    machine.SetStateLooping("Idle", False)
    machine.SetStateTransitionDuration("Idle", 0.25)

    state = machine.GetStates()["Idle"]
    check(abs(state.Speed - 0.5) < 1e-6, "SetStateSpeed wrote through, got %s" % state.Speed)
    check(state.Looping is False, "SetStateLooping wrote through")
    check(abs(state.TransitionDuration - 0.25) < 1e-6,
          "SetStateTransitionDuration wrote through, got %s" % state.TransitionDuration)

    # Each returns false for an unknown state, so a UI can treat "nothing
    # happened" the same way SetState already does.
    check(not machine.SetStateSpeed("Nope", 1.0), "SetStateSpeed reports an unknown state")
    check(not machine.SetStateLooping("Nope", True), "SetStateLooping reports an unknown state")
    check(not machine.SetStateTransitionDuration("Nope", 0.0),
          "SetStateTransitionDuration reports an unknown state")
    check(machine.SetStateClip("Nope", None) is False, "SetStateClip reports an unknown state")


def test_transition_setters():
    machine = build_machine()
    add_wire(machine, "Idle", "Walk")
    add_wire(machine, "Idle", "Run", "sprint")

    check(machine.SetTransitionTrigger(1, "dash"), "SetTransitionTrigger by index")
    check(machine.GetTransitions()[1].Trigger == "dash", "the trigger changed")
    check(machine.GetTransitions()[0].Trigger == "", "and only that one changed")

    check(machine.SetTransitionDuration(0, 0.75), "SetTransitionDuration by index")
    check(abs(machine.GetTransitions()[0].Duration - 0.75) < 1e-6, "the duration changed")

    check(not machine.SetTransitionTrigger(99, "x"), "an out-of-range index is refused")
    check(not machine.SetTransitionDuration(99, 1.0), "an out-of-range index is refused")


def test_state_positions_round_trip():
    """Node positions are the one piece of state outside the machine, so this is
    what proves a drag survives a save."""
    machine = build_machine()
    asset = Animation.AnimationAsset()
    asset.StateMachine = machine
    asset.HasStateMachine = True

    # The accessors, not StatePositions[...] = : def_readwrite on a container
    # hands Python a COPY, so item assignment silently no-ops.
    asset.SetStatePosition("Idle", 10.0, 20.0)
    asset.SetStatePosition("Run", 300.0, -40.0)

    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "machine.wlanim")
        error = Animation.SaveAnimation(path, asset)
        check(error is None, "save with state positions succeeds, got %r" % (error,))

        loaded = Animation.AnimationAsset()
        error = Animation.LoadAnimation(path, loaded)
        check(error is None, "load succeeds, got %r" % (error,))

        check(loaded.GetStatePositionCount() == 2,
              "both positions survived, got %d" % loaded.GetStatePositionCount())

        idle = loaded.GetStatePosition("Idle")
        check(idle is not None, "Idle's position came back")
        if idle is not None:
            check(abs(idle[0] - 10.0) < 1e-6 and abs(idle[1] - 20.0) < 1e-6,
                  "Idle's position is exact, got (%s, %s)" % (idle[0], idle[1]))

        run = loaded.GetStatePosition("Run")
        check(run is not None, "Run's position came back")
        if run is not None:
            check(abs(run[0] - 300.0) < 1e-6 and abs(run[1] + 40.0) < 1e-6,
                  "Run's position is exact, got (%s, %s)" % (run[0], run[1]))

        check(loaded.GetStatePosition("Missing") is None,
              "an unknown name returns None rather than a zeroed position")

        # The machine itself must be untouched by the round trip: positions are
        # an EDITOR concern and must not leak into the domain struct.
        check(loaded.HasStateMachine, "the machine flag survived")
        check(loaded.StateMachine.GetStateCount() == 3,
              "all three states survived, got %d" % loaded.StateMachine.GetStateCount())


def test_empty_positions_emit_no_section():
    """A file that has never been opened in the Animator must be byte-for-byte
    what it was before the editor section existed, so a round trip does not
    invent layout for a machine nobody has arranged."""
    machine = build_machine()
    asset = Animation.AnimationAsset()
    asset.StateMachine = machine
    asset.HasStateMachine = True

    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "plain.wlanim")
        check(Animation.SaveAnimation(path, asset) is None, "save succeeds")

        text = open(path, encoding="utf-8").read()
        check("editor:" not in text,
              "no editor section is emitted when there are no positions")


def test_positions_are_sorted_on_save():
    """The map's iteration order changes between runs, so an unsorted emission
    would rewrite the file on every save even when nothing moved."""
    asset = Animation.AnimationAsset()
    machine = build_machine()
    asset.StateMachine = machine
    asset.HasStateMachine = True
    for name in ("Zebra", "Alpha", "Middle"):
        asset.SetStatePosition(name, 1.0, 2.0)

    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "sorted.wlanim")
        Animation.SaveAnimation(path, asset)
        text = open(path, encoding="utf-8").read()

        order = [text.index("name: " + n) for n in ("Alpha", "Middle", "Zebra")]
        check(order == sorted(order),
              "positions are emitted in name order, got %s" % order)


def main():
    test_remove_state()
    test_remove_transition()
    test_transition_priority_is_insertion_order()
    test_state_setters()
    test_transition_setters()
    test_state_positions_round_trip()
    test_empty_positions_emit_no_section()
    test_positions_are_sorted_on_save()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
