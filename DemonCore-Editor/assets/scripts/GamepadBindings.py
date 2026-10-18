"""Standard gamepad control bindings for the FPS blockout.

A named action layer rather than raw button indices scattered through the
gameplay scripts. Scripts ask "is FIRE held?" and this module decides that is
the right trigger; remapping for a different pad style then touches one file
instead of every script that opens fire.

The values match the engine's GLFW-compatible constants, so no translation
happens here -- this is a naming layer, not a remapping one.
"""
import Wasteland

G = Wasteland.Gamepad

# --- Actions ---
# Named so a script reads as intent ("if bindings.is_held(bindings.FIRE)")
# rather than as a device detail ("if pad.button(5)").
FIRE = "fire"
AIM = "aim"
SPRINT = "sprint"
JUMP = "jump"
CROUCH = "crouch"
RELOAD = "reload"
INTERACT = "interact"
NEXT_WEAPON = "next_weapon"
PAUSE = "pause"

# --- Default map, on the Xbox-style layout the engine's GLFW codes follow ---
# GLFW's gamepad codes are defined against the Xbox controller layout, so
# A/B/X/Y are the face buttons and the triggers are LT/RT by index.
#
# Each entry is (axis, button): an ANALOG action fills `axis` and leaves
# `button` None; a DIGITAL action fills `button` and leaves `axis` None. The
# two halves must not be crossed -- putting a button code in the axis slot
# makes the action silently read a stick that never moves, so it appears
# permanently unpressed with no error anywhere.
DEFAULT_MAP = {
    # Face buttons: A is the primary action on an Xbox pad.
    FIRE: (G.AXIS_RIGHT_TRIGGER, None),
    AIM: (G.AXIS_LEFT_TRIGGER, None),
    JUMP: (None, G.BUTTON_A),
    INTERACT: (None, G.BUTTON_X),
    RELOAD: (None, G.BUTTON_Y),
    SPRINT: (None, G.BUTTON_LEFT_THUMB),
    CROUCH: (None, G.BUTTON_RIGHT_THUMB),
    NEXT_WEAPON: (None, G.BUTTON_RIGHT_BUMPER),
    PAUSE: (None, G.BUTTON_START),
}

# Which actions are analog (trigger) versus digital (button). An analog action
# needs a pull threshold and a level test; a digital one is a plain press.
ANALOG_ACTIONS = (FIRE, AIM)

# How far an analog trigger must be pulled to count as held. Matches
# Player.py's and Weapon.py's gamepadFireThreshold so aim and fire agree on
# the pull point.
TRIGGER_THRESHOLD = 0.5


def validate():
    """Return a list of malformed bindings.

    A crossed entry -- a button code in the axis slot or vice versa -- is
    silent: the action simply never reads as pressed, and the player sees a
    controller that does not work. Checking it here turns that into an
    obvious, reportable error.
    """
    problems = []
    for action, (axis, button) in DEFAULT_MAP.items():
        if axis is not None and button is not None:
            problems.append("{}: both axis and button set".format(action))
        elif axis is None and button is None:
            problems.append("{}: neither axis nor button set".format(action))
        elif axis is not None and action not in ANALOG_ACTIONS:
            problems.append(
                "{}: reads an axis but is not in ANALOG_ACTIONS".format(action))
        elif button is not None and action in ANALOG_ACTIONS:
            problems.append(
                "{}: reads a button but is listed in ANALOG_ACTIONS".format(action))
    return problems


def is_held(action, slot=0, threshold=TRIGGER_THRESHOLD):
    """True while the action's control is held.

    Digital actions are a plain button read. Analog ones compare the trigger
    against a threshold rather than testing for nonzero -- a resting trigger
    reports a small positive value, so `> 0` would hold the action forever.
    """
    binding = DEFAULT_MAP.get(action)
    if binding is None:
        return False
    axis, button = binding
    if button is not None:
        return G.IsButtonPressed(button, slot)
    if axis is None:
        return False
    return G.GetTrigger(axis, slot) > threshold


def was_just_pressed(action, slot=0, previous=None, threshold=TRIGGER_THRESHOLD):
    """Edge detection: held now, not held last frame.

    `previous` is the prior frame's result for the same action, so the caller
    owns the state. Self-contained rather than a GamepadBindings object with
    hidden state, because the existing scripts already carry their own
    `wasReload`/`wasSprint` edge flags and a second source of truth would
    drift out of sync with them.
    """
    current = is_held(action, slot, threshold)
    if previous is None:
        return current
    return current and not previous


def axis_value(action, slot=0):
    """The 0..1 pull amount for an analog action, else 0.0.

    For a digital action this is 1.0 when held, so callers can treat every
    action uniformly as a 0..1 amount.
    """
    binding = DEFAULT_MAP.get(action)
    if binding is None:
        return 0.0
    axis, button = binding
    if button is not None:
        return 1.0 if G.IsButtonPressed(button, slot) else 0.0
    if axis is None:
        return 0.0
    return G.GetTrigger(axis, slot)


def stick_move(slot=0, deadzone=None):
    """Analog movement direction from the left stick, or (0, 0) with no pad."""
    if not G.IsConnected(slot):
        return (0.0, 0.0)
    if deadzone is None:
        return G.GetLeftStick(slot)
    return G.GetLeftStick(slot, deadzone)


def stick_look(slot=0, deadzone=None):
    """Analog look direction from the right stick, or (0, 0) with no pad."""
    if not G.IsConnected(slot):
        return (0.0, 0.0)
    x = G.GetAxisDeadzone(G.AXIS_RIGHT_X, slot) if deadzone is None \
        else G.GetAxisDeadzone(G.AXIS_RIGHT_X, slot, deadzone)
    y = G.GetAxisDeadzone(G.AXIS_RIGHT_Y, slot) if deadzone is None \
        else G.GetAxisDeadzone(G.AXIS_RIGHT_Y, slot, deadzone)
    # Not GetLeftStick: that reads the LEFT axes. The right stick needs its
    # own pair, and Y is passed through un-inverted (up positive) to match.
    return (x, y)
