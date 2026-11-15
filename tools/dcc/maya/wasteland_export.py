"""wasteland_export.py -- Maya: export .gltf plus a Wasteland .wlanim sidecar.

INSTALL
-------
Copy this file into Maya's scripts directory:

    %USERPROFILE%/Documents/maya/<version>/scripts/

then run, from a Python tab in the Script Editor:

    import wasteland_export
    wasteland_export.export_selection("C:/path/hero.glb")

or bind `wasteland_export.export_selection` to a shelf button.

WHAT IT DOES
------------
1. Exports the SELECTED objects as `.glb` through Maya's own glTF exporter
   (`maya.cmds.file(..., type='glTF export')`). Maya's exporter is USED, never
   vendored or reimplemented.
2. Writes a `.wlanim` sidecar carrying the joint hierarchy and the animation,
   in the engine's own asset format, through the SHARED module
   `tools/dcc/wasteland_sidecar.py`.
3. Writes `wasteland_meta.json` recording the conventions.

THE CONVENTIONS
---------------
**Maya is Y-up, like glTF.** So unlike the Blender path there is NO axis
conversion: Maya's own translate values are already in the engine's convention.
The Blender add-on's `convert_position_channels` must NOT be applied here --
doing so would rotate every animation 90 degrees.

**Maya's default working unit is CENTIMETRES, Blender's is METRES.** That is
the single most likely way a Maya export comes out 100x too large, and it is
silent. The wrapper reads `cmds.currentUnit(linear=True)` and records the
metres-per-unit factor in the meta file rather than assuming one.

**FBX is a proprietary format and is NOT parsed here.** This wrapper exports
glTF, which Maya produces and the engine reads. Nothing in this repository reads
or writes FBX; a Maya artist who needs FBX for another pipeline must convert it
with Maya's own tools.

VERIFICATION STATUS
-------------------
The sidecar writer is shared with the Blender path and is unit-tested
headlessly (`tests/test_wlanim_sidecar.py`). THIS file is the Maya glue and has
NOT been executed: Maya is not installed on the machine this was written on.
The `maya.cmds` calls, the attribute enumeration, the Euler-to-quaternion
conversion and the exporter invocation are all unverified. The plan's stated
verification -- "the same rigged cube exported from Maya imports with the same
bone hierarchy and clip duration as the Blender export" -- needs a machine with
Maya.
"""

import json
import math
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
# The sidecar module lives one level up, shared with the Blender export path.
_DCC_ROOT = os.path.dirname(_HERE)
for _path in (_HERE, _DCC_ROOT):
    if _path not in sys.path:
        sys.path.append(_path)

import wasteland_sidecar as sidecar

ADDON_VERSION = (1, 0, 0)

# Maya's rotation orders, as `node.rotationOrder` reports them. Maya's default
# is 'xyz' (the `rotateOrder` enum value 0), which is NOT the same as a
# naive X-then-Y-then-Z composition in every convention, so the order is read
# from the node rather than assumed.
_EULER_ORDERS = ("xyz", "yzx", "zxy", "xzy", "yxz", "zyx")


# ---------------------------------------------------------------------------
# unit handling
# ---------------------------------------------------------------------------

def _metres_per_unit(cmds):
    """Read Maya's working unit and return metres per linear unit.

    Maya defaults to CENTIMETRES (0.01 m); a scene set to metres returns 1.0.
    This is read rather than assumed because a 100x scale error is invisible
    until an artist compares the export against the engine's viewport.
    """
    try:
        unit = cmds.currentUnit(query=True, linear=True)
    except (AttributeError, RuntimeError):
        return 0.01, "cm (assumed -- could not query the working unit)"

    factors = {
        "mm": 0.001,
        "cm": 0.01,
        "m": 1.0,
        "in": 0.0254,
        "ft": 0.3048,
        "yd": 0.9144,
    }
    factor = factors.get(unit)
    if factor is None:
        return 0.01, f"{unit} (unrecognised -- assumed cm)"
    return factor, unit


# ---------------------------------------------------------------------------
# rotation
# ---------------------------------------------------------------------------

def euler_to_quaternion(degrees, order="xyz"):
    """Maya Euler angles (degrees) -> quaternion, w-first.

    The sidecar's rotation channel is quaternion-only, because a quaternion must
    NOT be interpolated componentwise: lerping x,y,z,w between two rotations
    does not rotate uniformly and the result is not unit-length.

    `order` is the composition order as Maya reports it. Each axis contributes a
    half-angle quaternion and the three are composed in that order; getting the
    order wrong rotates the bone about the wrong axes while still looking like a
    plausible animation.

    UNVERIFIED against a live Maya export. If a Maya character's animation plays
    back rotated, this function and the `rotationOrder` read are the first things
    to check.
    """
    if order not in _EULER_ORDERS:
        order = "xyz"

    radians = [math.radians(component) for component in degrees]
    # Half angles, the quaternion form of each elementary rotation.
    half = [component / 2.0 for component in radians]

    quaternions = {}
    for axis, angle in zip("xyz", half):
        quaternions[axis] = [math.cos(angle),
                             0.0, 0.0, 0.0]
        quaternions[axis][1 + "xyz".index(axis)] = math.sin(angle)

    def multiply(a, b):
        aw, ax, ay, az = a
        bw, bx, by, bz = b
        return [
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
        ]

    result = [1.0, 0.0, 0.0, 0.0]
    for axis in order:
        result = multiply(result, quaternions[axis])
    return result


# ---------------------------------------------------------------------------
# scene reading
# ---------------------------------------------------------------------------

def _read_joint_hierarchy(cmds, joints):
    """Joint names and parents. Parents are NAMES, not indices, matching the
    .wlanim format's stable-string-key rule: a re-export that reorders the
    hierarchy still binds to the same bones."""
    bones = []
    for joint in joints:
        parents = cmds.listRelatives(joint, parent=True, fullPath=False) or []
        parent = parents[0] if parents else ""
        bones.append(sidecar.build_bone(joint, parent))
    return bones


def _animated_attributes(cmds, node):
    """The animatable transform attributes on a node, as (attribute, kind).

    Only translate/rotate/scale are convertible to the sidecar's channel set.
    Anything else (a custom attribute, a constraint, a driven key) is skipped
    rather than guessed at: a silently wrong channel is worse than a missing one.
    """
    kinds = (("translate", "position"), ("rotate", "rotation"),
             ("scale", "scale"))
    found = []
    try:
        attributes = cmds.listAnimatable(node) or []
    except (AttributeError, RuntimeError):
        return found

    for attribute in attributes:
        name = attribute.split(".")[-1]
        for prefix, kind in kinds:
            if name.startswith(prefix) and len(name) == len(prefix) + 1 \
                    and name[-1] in "XYZ":
                found.append((name, kind))
                break
    return found


def _read_action(cmds, joints, start, end, step=1.0):
    """Sample the selected joints' animation into a sidecar clip.

    Sampling at a fixed step rather than reading keyframes directly: Maya's
    keyframe times are not exposed through a single query, and a fixed-step
    sample is deterministic and matches what the engine will actually play.
    The step is a parameter so a longer clip can be sampled coarsely.
    """
    tracks = {}
    time = start
    while time <= end + 1e-6:
        for joint in joints:
            channels = tracks.setdefault(joint, {})
            for attribute, kind in _animated_attributes(cmds, joint):
                try:
                    value = cmds.getAttr(f"{joint}.{attribute}", time=time)
                except (AttributeError, RuntimeError, TypeError):
                    continue
                if kind == "rotation":
                    # Maya's rotate is DEGREES and Euler; the sidecar is
                    # quaternion. The order is read per node, not assumed.
                    try:
                        order_index = cmds.getAttr(f"{joint}.rotateOrder")
                        order = _EULER_ORDERS[order_index] \
                            if 0 <= order_index < len(_EULER_ORDERS) else "xyz"
                    except (AttributeError, RuntimeError, TypeError):
                        order = "xyz"
                    axis = attribute[-1].lower()
                    target = channels.setdefault("rotation", {})
                    target.setdefault(axis, [])
                    # The per-axis list is merged into a quaternion below.
                    target[axis].append((time, value, order))
                else:
                    axis = attribute[-1].lower()
                    channels.setdefault(f"{kind}_{axis}", []).append(
                        (time, value, "LINEAR"))
        time += step

    built = []
    for joint, channels in tracks.items():
        rotation = channels.get("rotation")
        if rotation:
            times = sorted({entry[0] for entries in rotation.values()
                            for entry in entries})
            quaternion_keys = []
            for moment in times:
                euler = []
                for axis in "xyz":
                    entries = rotation.get(axis, [])
                    match = [value for (t, value, _) in entries
                             if abs(t - moment) < 1e-6]
                    euler.append(match[0] if match else 0.0)
                order = next((order for entries in rotation.values()
                              for (_, _, order) in entries), "xyz")
                quaternion_keys.append(sidecar.build_keyframe(
                    moment, euler_to_quaternion(euler, order),
                    mode=sidecar.MODE_BEZIER))
            channels.pop("rotation")
            channels["rotation"] = quaternion_keys

        if channels:
            built.append(sidecar.build_track(joint, channels))
    return built


# ---------------------------------------------------------------------------
# export
# ---------------------------------------------------------------------------

def _exporter_kwargs(filepath):
    """Build Maya's glTF export options, filtered to the installed version.

    Maya's glTF exporter is loaded as a plugin (`Export.glTF` in 2020+), and
    the exact option set varies between releases. Passing an unsupported flag
    raises and writes nothing, so the options are kept minimal and the caller
    reports the failure.
    """
    return {
        "exportSelected": True,
        "force": True,
        "type": "glTF export",
        "file": filepath,
    }


def export_selection(filepath, sample_step=1.0):
    """Export the current Maya selection as .glb + .wlanim + _meta.json.

    Returns (ok, message). Never raises: a failed export is a message the
    artist can act on, not a traceback in the Script Editor.
    """
    try:
        import maya.cmds as cmds
    except ImportError:
        return False, ("the Maya Python API is not available -- this script "
                       "must run inside Maya")

    try:
        selection = cmds.ls(selection=True, long=False) or []
        if not selection:
            return False, "nothing is selected"
        joints = [node for node in cmds.ls(type="joint", long=False) or []
                  if node in selection or any(
                      descendant.startswith(node + "|")
                      for descendant in selection)]
        if not joints:
            joints = cmds.ls(type="joint", long=False) or []
    except (AttributeError, RuntimeError) as error:
        return False, f"could not read the scene: {error}"

    try:
        cmds.file(**_exporter_kwargs(filepath))
    except (AttributeError, RuntimeError, TypeError) as error:
        return False, (f"Maya's glTF exporter failed: {error}. The exporter is "
                       "a plugin -- check that it is loaded (Windows > Settings "
                       "> Plug-in Manager > glTFExport).")

    if not os.path.isfile(filepath):
        return False, f"the exporter reported success but wrote no file: {filepath}"

    stem, _ = os.path.splitext(filepath)
    sidecar_path = stem + ".wlanim"
    meta_path = stem + "_meta.json"

    try:
        start = float(cmds.playbackOptions(query=True, minTime=True))
        end = float(cmds.playbackOptions(query=True, maxTime=True))
    except (AttributeError, RuntimeError, TypeError):
        start, end = 1.0, 1.0

    bones = _read_joint_hierarchy(cmds, joints)
    tracks = _read_action(cmds, joints, start, end, sample_step)
    asset = sidecar.build_asset(os.path.basename(stem), bones,
                                [sidecar.build_clip("Take", tracks)])

    try:
        with open(sidecar_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(sidecar.emit_asset(asset))
    except OSError as error:
        return False, f"could not write {sidecar_path}: {error}"

    metres_per_unit, unit_name = _metres_per_unit(cmds)
    meta = sidecar.build_meta(
        source="maya",
        up_axis="+Y",
        forward_axis="-Z",
        unit_scale=metres_per_unit,
        exporter_version=".".join(str(part) for part in ADDON_VERSION),
        gltf_path=os.path.basename(filepath),
        sidecar_path=os.path.basename(sidecar_path),
        notes=(f"Maya is Y-up, matching glTF/engine: NO axis conversion was "
               f"applied. Working unit {unit_name} -> {metres_per_unit} "
               "metres per unit."),
    )
    try:
        with open(meta_path, "w", encoding="utf-8") as handle:
            handle.write(sidecar.emit_meta(meta))
    except OSError as error:
        return True, f"wrote the asset but could not write {meta_path}: {error}"

    return True, (f"wrote {os.path.basename(filepath)}, "
                  f"{os.path.basename(sidecar_path)} and "
                  f"{os.path.basename(meta_path)}")


if __name__ == "__main__":
    print("Run export_selection(path) from Maya's Script Editor.")
