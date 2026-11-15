"""wasteland_sidecar.py -- build .wlanim and wasteland_meta.json from a DCC scene.

PURE PYTHON, NO bpy. This module is deliberately importable outside Blender so
the sidecar format can be unit-tested headlessly; the add-on
(`wasteland_export.py`) is a thin adapter that reads a Blender scene and calls
in here.

WHY A HAND-ROLLED YAML EMITTER
------------------------------
Blender's bundled Python does not ship PyYAML, and the test interpreter on this
machine does not either (only `python3` has it). A DCC add-on that needs a
third-party package is a support burden for every artist who installs it, and a
hard failure for every one who does not. The `.wlanim` format is small enough --
mappings, sequences, scalars and flow-style vector sequences -- that emitting it
directly is ~60 lines and removes the dependency entirely.

The output is VALID YAML with the engine's field names, but it is not
byte-identical to `AnimationSerializer::Emit`: this emitter writes full float
precision where yaml-cpp writes nine significant digits. That is a feature --
`0.0199999996` is a rounded `0.02` -- and it means a sidecar is a more faithful
record of the authored curve than the engine's own re-emission.

THE AXIS CONVENTION
-------------------
Blender is Z-up. glTF and the Wasteland engine are Y-up. Blender's glTF
exporter performs that conversion when `export_yup` (`+Y up`) is set, which the
add-on always sets.

The conversion the exporter applies to a point is a +90 degree rotation about
X:

    (x, y, z)_blender  ->  (x, -z, y)_gltf

The add-on records this in `wasteland_meta.json` and NEVER bakes it into the
sidecar's bone hierarchy, because the hierarchy (names and parents) is
convention-independent. Rest-pose transforms are the one place the convention
matters, and `build_asset` takes them already converted -- the adapter does the
maths, this module just writes what it is given. Keeping the maths in the
adapter means the format code has no opinion about axes and cannot get it wrong
twice.
"""

import json
import math

SCHEMA = 1

# ---------------------------------------------------------------------------
# The axis convention
# ---------------------------------------------------------------------------
# Blender is Z-up; glTF and the Wasteland engine are Y-up. Blender's exporter
# performs the conversion when `+Y up` is set, which the add-on always sets.
#
# The conversion is a +90 degree rotation about X:
#
#     (x, y, z)_blender  ->  (x, -z, y)_gltf
#
# It is a PERMUTATION with one sign, not a per-axis sign flip, so a
# position/scale track cannot be converted channel by channel: glTF's y channel
# carries Blender's NEGATED z, and glTF's z carries Blender's y. Converting each
# channel independently leaves the animation sheared.
#
# These live in the pure module (not the add-on) precisely so they are testable
# without Blender -- they are the part most likely to be subtly wrong and the
# part an artist would notice last.
AXIS_SOURCE = (0, 2, 1)      # glTF axis index <- Blender axis index
AXIS_NEGATE = (False, True, False)


def convert_vector_blender_to_gltf(value):
    """(x, y, z)_blender -> (x, -z, y)_gltf."""
    return [value[AXIS_SOURCE[index]] * (-1.0 if AXIS_NEGATE[index] else 1.0)
            for index in range(3)]


def convert_quaternion_blender_to_gltf(value):
    """Rotate a Blender-space quaternion into glTF space.

    A change of basis, not a vector transform: the rotation's AXIS is carried
    into the new basis by conjugating with the basis rotation r (+90 deg about
    X), giving q' = r * q * r^-1.

    Worked example, by hand: Blender's +90 deg about Z (its up) takes +X to +Y.
    In glTF space Blender's +Y is glTF's +Z, so the same rotation must take +X
    to +Z -- a -90 deg about glTF's Y. Conjugating Blender's (w,x,y,z) =
    (cos45, 0, 0, sin45) by r gives exactly that.

    `value` is w-first, matching both Blender's `rotation_quaternion` and the
    .wlanim file format.
    """
    w, x, y, z = value
    # r = +90 deg about X = (cos45, sin45, 0, 0), w-first.
    rw, rx, ry, rz = math.cos(math.pi / 4.0), math.sin(math.pi / 4.0), 0.0, 0.0

    def multiply(a, b):
        aw, ax, ay, az = a
        bw, bx, by, bz = b
        return [
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
        ]

    def conjugate(q):
        return [q[0], -q[1], -q[2], -q[3]]

    return multiply(multiply([rw, rx, ry, rz], [w, x, y, z]),
                    conjugate([rw, rx, ry, rz]))


def convert_position_channels(per_axis_points, negate=True):
    """Convert three Blender axis point-lists into glTF channel keyframes.

    `per_axis_points` is a 3-element sequence, one list of (t, value,
    interpolation) per Blender axis. Returns a dict of glTF channel name to
    keyframe list, with the permutation and sign applied.

    The channel names are LETTERS (`position_x`, `position_y`, `position_z`),
    matching the engine's own format. An earlier version wrote `position_0`;
    the engine's parser builds its lookup key as "position_" + "x"/"y"/"z", so
    a numeric suffix parses as a track with NO channels and reads back as a
    zero-duration clip rather than as an error.

    Channels with no data are OMITTED rather than written empty: the engine's
    format writes only the channels that were authored, and an empty channel
    would read back as a track that does nothing.
    """
    axis_names = ("x", "y", "z")
    channels = {}
    for gltf_axis in range(3):
        points = per_axis_points[AXIS_SOURCE[gltf_axis]]
        if not points:
            continue
        sign = -1.0 if (negate and AXIS_NEGATE[gltf_axis]) else 1.0
        channels[f"position_{axis_names[gltf_axis]}"] = [
            build_keyframe(
                t,
                sign * v,
                mode=MODE_LINEAR
                if interpolation == "LINEAR"
                else MODE_CONSTANT
                if interpolation == "CONSTANT"
                else MODE_BEZIER,
            )
            for (t, v, interpolation) in points
        ]
    return channels

# The interpolation modes the engine's AnimationClip understands. Blender's
# BEZIER maps to Cubic; LINEAR and CONSTANT map to their same-named modes.
MODE_BEZIER = "Cubic"
MODE_LINEAR = "Linear"
MODE_CONSTANT = "Constant"


# ---------------------------------------------------------------------------
# builders -- plain dicts, so a caller can inspect or serialise them directly
# ---------------------------------------------------------------------------

def build_bone(name, parent=""):
    """One skeleton bone. `parent` is "" for a root bone, matching the
    engine's convention -- an empty string, not a null, because YAML null would
    read back as an absent key."""
    return {"name": name, "parent": parent}


def build_keyframe(t, v, mode=MODE_BEZIER, tangent_in=0.0, tangent_out=0.0):
    """One key on a scalar or quaternion track.

    `tangent_in`/`tangent_out` are only meaningful for Cubic; the engine ignores
    them for the other modes but the format always writes them, so a sidecar
    round-trips through the engine's own Emit unchanged in shape.
    """
    return {"t": float(t), "v": v, "in": float(tangent_in),
            "out": float(tangent_out), "mode": mode}


def build_track(bone, channels):
    """One bone's animation track.

    `channels` maps a channel name to a list of keyframes. The engine's channel
    names are `position_x`/`position_y`/`position_z`, `rotation`, `scale_x`/
    `scale_y`/`scale_z` -- per-axis for position and scale, a single quaternion
    channel for rotation, because a walk cycle keys position and rotation at
    different times and one `v: [x, y, z]` per key would force them together.

    The channels are SIBLINGS of `bone`, not nested under it -- the file shape
    is `- bone: Spine` followed by `position_x:` at the same indent. Nesting
    them under a `channels` key parses as a track with no channels at all,
    which reads back as a zero-duration clip rather than an error.
    """
    track = {"bone": bone}
    track.update(channels)
    return track


def build_clip(name, tracks, looping=True, play_rate=1.0):
    """One animation clip. Duration is DERIVED from the last key by the engine
    (`AnimationClip::GetDuration`), so it is deliberately not a field here --
    storing it would create a second source of truth that can disagree."""
    return {"name": name, "looping": bool(looping),
            "play_rate": float(play_rate), "tracks": list(tracks)}


def build_asset(name, bones, clips, states=None, transitions=None):
    """The whole .wlanim document as a dict.

    `states`/`transitions` are optional; a rig with clips and no machine is
    directly controllable, so an export with no controller produces no
    `state_machine` block rather than an empty one.
    """
    asset = {
        "schema": SCHEMA,
        "name": name,
        "skeleton": {"bones": list(bones), "rest_pose": []},
        "clips": list(clips),
    }
    if states or transitions:
        asset["state_machine"] = {
            "states": list(states or []),
            "transitions": list(transitions or []),
        }
    return asset


def build_meta(source, up_axis, forward_axis, unit_scale, exporter_version,
               gltf_path, sidecar_path, notes=""):
    """wasteland_meta.json -- the axis/unit record that travels with an export.

    Written as JSON rather than YAML because it is machine-read by tools, not
    hand-edited by artists, and every DCC and every language has a JSON reader.
    """
    return {
        "source": source,
        "up_axis": up_axis,
        "forward_axis": forward_axis,
        "unit_scale": float(unit_scale),
        "exporter_version": exporter_version,
        "gltf": gltf_path,
        "sidecar": sidecar_path,
        "notes": notes,
    }


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------

def _format_float(value):
    """Full precision. `repr` on a float is the shortest string that round-trips
    exactly, which is what a curve key needs -- a rounded value is a different
    animation."""
    if value == int(value) and abs(value) < 1e16:
        # Keep integers readable (1.0 -> "1"), matching how the engine writes
        # them and keeping the file diff-friendly.
        return str(int(value))
    return repr(float(value))


def _format_string(value):
    """Quote only when necessary, and always quote the empty string.

    An unquoted empty value is a YAML null, which the engine reads as an ABSENT
    key -- a root bone's `parent: ""` would silently become "no parent" in a
    way that is indistinguishable from a bone that legitimately has none.
    """
    text = str(value)
    if text == "":
        return '""'
    if any(character in text for character in ":#{}[],&*?|-<>=!%@`\"'"):
        return json.dumps(text)
    return text


def _format_scalar(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return _format_float(value)
    if isinstance(value, int):
        return str(value)
    return _format_string(value)


def _format_value(value):
    """A value in FLOW style, used for the fixed-shape vector sequences
    (quaternion keys). Flow is what the engine writes and what keeps a
    four-element quaternion on one line."""
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(_format_value(item) for item in value) + "]"
    return _format_scalar(value)


def _emit_mapping_lines(mapping, indent, lines):
    for key, value in mapping.items():
        prefix = "  " * indent
        if isinstance(value, dict):
            if not value:
                lines.append(f"{prefix}{key}: {{}}")
            else:
                lines.append(f"{prefix}{key}:")
                _emit_mapping_lines(value, indent + 1, lines)
        elif isinstance(value, (list, tuple)):
            if not value:
                lines.append(f"{prefix}{key}: []")
            else:
                lines.append(f"{prefix}{key}:")
                _emit_sequence_lines(value, indent + 1, lines)
        else:
            lines.append(f"{prefix}{key}: {_format_scalar(value)}")


def _emit_sequence_lines(sequence, indent, lines):
    prefix = "  " * indent
    for item in sequence:
        if isinstance(item, dict):
            # A mapping inside a sequence: first key on the dash line, the rest
            # aligned under it. This is the shape Hero.wlanim uses.
            first = True
            for key, value in item.items():
                lead = prefix + "- " if first else " " * (len(prefix) + 2)
                first = False
                if isinstance(value, (dict, list, tuple)) and value:
                    lines.append(f"{lead}{key}:")
                    if isinstance(value, dict):
                        _emit_mapping_lines(value, indent + 2, lines)
                    else:
                        _emit_sequence_lines(value, indent + 2, lines)
                elif isinstance(value, (dict, list, tuple)):
                    lines.append(f"{lead}{key}: {{}}")
                else:
                    lines.append(f"{lead}{key}: {_format_scalar(value)}")
        else:
            lines.append(f"{prefix}- {_format_scalar(item)}")


def emit_asset(asset):
    """The .wlanim document as YAML text."""
    lines = []
    _emit_mapping_lines(asset, 0, lines)
    return "\n".join(lines) + "\n"


def emit_meta(meta):
    """wasteland_meta.json as indented JSON."""
    return json.dumps(meta, indent=2) + "\n"
