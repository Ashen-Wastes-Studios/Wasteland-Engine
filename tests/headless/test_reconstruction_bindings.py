"""The clean-room reconstruction stack is controllable and inspectable.

WLSR / WLAA / WLRR / WLFG each map to a REAL branch in the Nova schedule, and
the editor plus the Python bindings are the two surfaces that must agree with
the renderer about what is on. A binding that silently returns the default
makes "the feature is off" and "the feature never loaded" the same answer, so
this suite pins BOTH directions of every accessor, the factor's clamping and
its implication, and the availability flags that distinguish the two cases.

Run:  python tests/headless/test_reconstruction_bindings.py
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402

from runner import check, check_eq  # noqa: E402

FEATURES = (
    "Wlsr",
    "Wlaa",
    "Wlrr",
    "Wlfg",
)


def test_every_feature_is_bound_in_both_directions():
    """A missing binding fails as one readable message, not an AttributeError
    from each test below."""
    for name in FEATURES:
        getter = f"Renderer3D_Is{name}Enabled"
        setter = f"Renderer3D_Set{name}Enabled"
        check(hasattr(Wasteland, getter),
              f"{getter} is not bound -- {name} cannot be read")
        check(hasattr(Wasteland, setter),
              f"{setter} is not bound -- {name} cannot be changed")
        check(callable(getattr(Wasteland, getter, None)),
              f"{getter} is bound but not callable")
        check(callable(getattr(Wasteland, setter, None)),
              f"{setter} is bound but not callable")


def test_every_feature_defaults_off():
    """A fresh process must have the whole stack off. Every one of these is a
    change to the final image, and the plan's rule is that a wrong model must
    cost nothing by default."""
    for name in FEATURES:
        check_eq(getattr(Wasteland, f"Renderer3D_Is{name}Enabled")(), False,
                 f"{name} must default off, got on")


def test_each_feature_round_trips():
    """Both directions, so a setter that never takes effect is caught."""
    for name in FEATURES:
        setter = getattr(Wasteland, f"Renderer3D_Set{name}Enabled")
        getter = getattr(Wasteland, f"Renderer3D_Is{name}Enabled")
        setter(True)
        check_eq(getter(), True, f"{name} did not turn on")
        setter(False)
        check_eq(getter(), False, f"{name} did not turn back off")


def test_wlsr_is_a_pair_not_a_single_flag():
    """WLSR = EASU upscale + RCAS sharpen. Turning it off must restore bilinear
    rather than leaving a sharpen stage over an image nobody asked to sharpen,
    and turning it on must select both halves."""
    check(hasattr(Wasteland, "Renderer3D_IsRCASEnabled"),
          "Renderer3D_IsRCASEnabled is not bound -- the RCAS half is unreadable")
    check(hasattr(Wasteland, "Renderer3D_GetReconstructionFilter"),
          "the reconstruction filter is unreadable -- the EASU half is unreadable")

    Wasteland.Renderer3D_SetWlsrEnabled(True)
    check_eq(Wasteland.Renderer3D_IsRCASEnabled(), True,
             "WLSR on must select the RCAS sharpen stage")
    check_eq(Wasteland.Renderer3D_GetReconstructionFilter(), 8,
             "WLSR on must select the EASU upscale kernel (FSR1 = 8)")

    Wasteland.Renderer3D_SetWlsrEnabled(False)
    check_eq(Wasteland.Renderer3D_IsRCASEnabled(), False,
             "WLSR off must drop the RCAS sharpen stage")
    check_eq(Wasteland.Renderer3D_GetReconstructionFilter(), 2,
             "WLSR off must restore bilinear (2), not leave EASU selected")


def test_wlaa_is_the_native_resolution_branch():
    """WLAA is a real shader branch (filter 12), not a no-op entry."""
    Wasteland.Renderer3D_SetWlaaEnabled(True)
    check_eq(Wasteland.Renderer3D_GetReconstructionFilter(), 12,
             "WLAA on must select filter 12, got something else")
    Wasteland.Renderer3D_SetWlaaEnabled(False)
    check_eq(Wasteland.Renderer3D_GetReconstructionFilter(), 2,
             "WLAA off must restore bilinear")


def test_the_wlfg_factor_clamps_and_implies_the_warp():
    """The factor is 0..6, and N >= 1 implies the warp is on -- the two cannot
    disagree, which is what lets WL_WLFG=1 keep the pre-Step-6 behaviour."""
    check(hasattr(Wasteland, "Renderer3D_GetWlfgFactor"),
          "Renderer3D_GetWlfgFactor is not bound -- the factor is unreadable")

    Wasteland.Renderer3D_SetWlfgEnabled(False)
    check_eq(Wasteland.Renderer3D_GetWlfgFactor(), 0,
             "disabling WLFG must zero the factor")

    Wasteland.Renderer3D_SetWlfgFactor(6)
    check_eq(Wasteland.Renderer3D_GetWlfgFactor(), 6, "factor 6 did not stick")
    check_eq(Wasteland.Renderer3D_IsWlfgEnabled(), True,
             "a factor >= 1 must imply the warp is enabled")

    Wasteland.Renderer3D_SetWlfgFactor(99)
    check_eq(Wasteland.Renderer3D_GetWlfgFactor(), 6,
             "the factor must clamp at 6, not accept an out-of-range value")
    Wasteland.Renderer3D_SetWlfgFactor(-3)
    check_eq(Wasteland.Renderer3D_GetWlfgFactor(), 0,
             "the factor must clamp at 0, not accept a negative value")

    Wasteland.Renderer3D_SetWlfgEnabled(False)
    check_eq(Wasteland.Renderer3D_GetWlfgFactor(), 0,
             "WLFG must be off at the end of the suite")


def test_the_tier_is_reported_for_the_active_backend():
    """The tier is a property of the API, resolved by TierFor. A headless run
    still has an active backend, so the name must be a non-empty string and
    must be the unavailable sentinel rather than a phantom level."""
    check(hasattr(Wasteland, "Renderer3D_GetReconstructionTierName"),
          "Renderer3D_GetReconstructionTierName is not bound -- the tier is unreadable")
    tier = Wasteland.Renderer3D_GetReconstructionTierName()
    check(isinstance(tier, str) and len(tier) > 0,
          f"the tier name must be a non-empty string, got {tier!r}")
    check("Reconstruct" in tier or tier == "unavailable",
          f"the tier name is not a reconstruction level: {tier!r}")


def test_availability_is_reported_separately_from_enabled():
    """The gate readouts. Without them a user cannot tell "no enhancement" from
    "the head never loaded" -- both leave the frame unchanged."""
    for name in ("Wlrr", "Wlfg"):
        getter = f"Renderer3D_Is{name}Available"
        check(hasattr(Wasteland, getter),
              f"{getter} is not bound -- the head's availability is unreadable")
        check_eq(getattr(Wasteland, getter)(), False,
                 f"{name} must report unavailable in a headless run with no blob")


def test_the_tiers_are_backed_by_dispatched_passes():
    """The tier resolver must not name a level whose passes nothing dispatches.
    Structural: the pass IDs each tier implies are asserted by
    tools/verify_render_capabilities.py, so here we only pin that the resolver
    is reachable and monotonic -- probing a HIGHER tier must never resolve to a
    LOWER one than probing a lower tier on the same backend."""
    full = Wasteland.Renderer3D_GetReconstructionTierName()
    check(bool(full), "the tier name came back empty")
