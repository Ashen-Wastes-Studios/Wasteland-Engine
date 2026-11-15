"""wasteland_export.py -- Blender add-on: export a Wasteland .gltf + .wlanim pair.

INSTALL
-------
Copy this file and `wasteland_sidecar.py` into Blender's add-ons directory
(Edit > Preferences > Add-ons > Install, or
`%APPDATA%/Blender Foundation/Blender/<version>/scripts/addons/`), then enable
"Wasteland Export". The sidecar module MUST sit beside this file -- Blender's
add-on loader puts the add-on's directory on `sys.path`, which is how the import
below resolves.

WHAT IT DOES
------------
1. Exports the SELECTED objects as `.gltf`/`.glb` by CALLING Blender's built-in
   glTF exporter (`bpy.ops.export_scene.gltf`). That exporter is MIT-licensed and
   is invoked, never vendored or copied.
2. Writes a `.wlanim` sidecar carrying the armature's bone hierarchy and the
   actions' clips, in the engine's own asset format.
3. Writes `wasteland_meta.json` recording the axis and unit conventions.

THE AXIS CONVENTION (read this before reporting "my model is lying on its side")
--------------------------------------------------------------------------
Blender is Z-up; glTF and the Wasteland engine are Y-up. The exporter performs
that conversion when `+Y up` is enabled, and this add-on ALWAYS enables it. The
conversion applied to a point is a +90 degree rotation about X:

    (x, y, z)_blender  ->  (x, -z, y)_gltf

The sidecar's bone HIERARCHY (names and parents) is convention-independent and
is written verbatim. Clip keys ARE converted, because they must live in the same
space as the skeleton the engine builds from the glTF file -- which the exporter
has already converted.

VERIFICATION STATUS
-------------------
The sidecar writer (`wasteland_sidecar.py`) is pure Python and is unit-tested
headlessly. THIS file is the Blender glue and has NOT been executed: Blender is
not installed on the machine this was written on. Every Blender-facing code path
here is therefore unverified, and the parameter introspection in
`_exporter_kwargs` exists specifically so the add-on adapts to whichever
parameter names the installed exporter uses rather than failing on a version
mismatch.
"""

import bpy
from bpy_extras.io_utils import ExportHelper
from bpy.props import StringProperty, BoolProperty, FloatProperty

import json
import os
import sys

# The sidecar module sits beside this file. Blender puts the add-on's directory
# on sys.path, but an explicit append keeps this working when the file is run
# from a script editor or installed as a single .zip.
_HERE = os.path.dirname(os.path.abspath(__file__))
# The sidecar module lives one level up, shared with the Maya export path: both
# DCCs write the SAME .wlanim format, and two implementations of it would be
# two things to keep in step.
_DCC_ROOT = os.path.dirname(_HERE)
for _path in (_HERE, _DCC_ROOT):
    if _path not in sys.path:
        sys.path.append(_path)

import wasteland_sidecar as sidecar

ADDON_VERSION = (1, 0, 0)

bl_info = {
    "name": "Wasteland Export",
    "author": "Ashen Wastes Studios",
    "version": ADDON_VERSION,
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > Wasteland",
    "description": "Export .gltf plus a Wasteland .wlanim animation sidecar.",
    "category": "Import-Export",
}


# ---------------------------------------------------------------------------
# axis conversion
# ---------------------------------------------------------------------------
# The conversion itself lives in `wasteland_sidecar` (pure Python, unit-tested).
# This module is the adapter: it reads Blender data and hands plain lists to
# those functions. Keeping the maths out of here means there is exactly one
# statement of the convention, and it is the one that is tested.


# ---------------------------------------------------------------------------
# Blender scene reading
# ---------------------------------------------------------------------------

def _read_bones(armature_object):
    """The armature's bone hierarchy, parents by NAME.

    Names, not indices: the engine's .wlanim format uses stable string keys
    everywhere, so a re-export that reorders bones still binds correctly.
    """
    bones = []
    for bone in armature_object.data.bones:
        parent = bone.parent.name if bone.parent else ""
        bones.append(sidecar.build_bone(bone.name, parent))
    return bones


def _fcurve_channels(action):
    """Group an action's f-curves by (bone, channel).

    Blender stores a location as three f-curves (`array_index` 0/1/2) sharing
    one data_path; a quaternion as four. The engine's format is per-axis for
    position and scale and a single quaternion channel for rotation, so the
    grouping has to happen before the track is built.
    """
    grouped = {}
    for fcurve in action.fcurves:
        data_path = fcurve.data_path
        # Only pose-channel curves are animation we can express. Object-level
        # transforms, constraints and drivers are skipped rather than guessed
        # at: a silently wrong channel is worse than a missing one.
        if not data_path.startswith("pose.bones"):
            continue

        try:
            bone_name = data_path.split('"')[1]
        except IndexError:
            continue

        if data_path.endswith(".location"):
            channel = ("position", fcurve.array_index)
        elif data_path.endswith(".scale"):
            channel = ("scale", fcurve.array_index)
        elif data_path.endswith(".rotation_quaternion"):
            channel = ("rotation", fcurve.array_index)
        elif data_path.endswith(".rotation_euler"):
            channel = ("rotation_euler", fcurve.array_index)
        else:
            continue

        grouped.setdefault((bone_name, channel), []).append(fcurve)

    return grouped


def _read_action(action, bone_names):
    """One Blender action as a Wasteland clip, or None if it has no pose data.

    Returns None for an action with no pose f-curves -- exporting an empty clip
    would produce a trackless .wlanim entry that plays nothing and looks like a
    bug rather than an empty action.
    """
    grouped = _fcurve_channels(action)
    if not grouped:
        return None

    by_bone = {}
    for (bone_name, (kind, index)), fcurves in grouped.items():
        if bone_name not in bone_names:
            continue
        points = []
        for fcurve in fcurves:
            for key in fcurve.keyframe_points:
                points.append((key.co[0], key.co[1], key.interpolation))
        if not points:
            continue
        points.sort(key=lambda point: point[0])
        by_bone.setdefault(bone_name, {})[(kind, index)] = points

    tracks = []
    for bone_name, channels in by_bone.items():
        track_channels = {}

        # Position and scale: per-axis scalar channels, converted to Y-up.
        #
        # The conversion permutes the axes, so it cannot be applied channel by
        # channel -- `convert_position_channels` builds all three from the
        # grouped axis lists, which is the only correct way.
        for kind in ("position", "scale"):
            per_axis = [channels.get((kind, index)) for index in range(3)]
            if any(per_axis):
                converted = sidecar.convert_position_channels(per_axis)
                for channel_name, keyframes in converted.items():
                    track_channels[f"{kind}_{channel_name.split('_')[1]}"] = \
                        keyframes

        # Rotation: one quaternion channel. Euler curves are converted here
        # because Blender stores a bone's rotation in whatever mode the rig was
        # authored with, and the engine's format is quaternion-only.
        rotation_points = channels.get(("rotation", 0))
        if rotation_points:
            quaternions = []
            for index in range(4):
                axis_points = channels.get(("rotation", index))
                if axis_points:
                    quaternions.append({t: v for (t, v, _) in axis_points})
            times = sorted(quaternions[0].keys()) if quaternions else []
            if times:
                track_channels["rotation"] = [
                    sidecar.build_keyframe(
                        t,
                        sidecar.convert_quaternion_blender_to_gltf([quaternions[i][t] for i in range(4)]),
                        mode=sidecar.MODE_BEZIER,
                    )
                    for t in times
                ]
        else:
            euler = [channels.get(("rotation_euler", i)) for i in range(3)]
            if any(euler):
                from mathutils import Euler, Quaternion
                times = sorted(euler[0].keys()) if euler[0] else []
                track_channels["rotation"] = []
                for t in times:
                    euler_values = []
                    for index in range(3):
                        points = euler[index]
                        if points:
                            euler_values.append(
                                next(v for (pt, v, _) in points if pt == t))
                        else:
                            euler_values.append(0.0)
                    quaternion = Euler(euler_values, "XYZ").to_quaternion()
                    track_channels["rotation"].append(
                        sidecar.build_keyframe(
                            t,
                            sidecar.convert_quaternion_blender_to_gltf(
                                [quaternion.w, quaternion.x,
                                 quaternion.y, quaternion.z]),
                            mode=sidecar.MODE_BEZIER,
                        )
                    )

        if track_channels:
            tracks.append(sidecar.build_track(bone_name, track_channels))

    if not tracks:
        return None

    frame_start, frame_end = action.frame_range
    return sidecar.build_clip(action.name, tracks, looping=True)


def _read_scene_animation(selection):
    """Bones and clips from the selected armatures and their actions."""
    armatures = [obj for obj in selection if obj.type == "ARMATURE"]
    bones = []
    for armature in armatures:
        bones.extend(_read_bones(armature))
    if not bones:
        return [], []

    bone_names = {bone["name"] for bone in bones}
    clips = []
    for action in bpy.data.actions:
        clip = _read_action(action, bone_names)
        if clip is not None:
            clips.append(clip)
    return bones, clips


# ---------------------------------------------------------------------------
# exporter invocation
# ---------------------------------------------------------------------------

def _exporter_kwargs(filepath, use_selection):
    """Build the glTF exporter's keyword arguments for this Blender version.

    The exporter's parameter names have changed across Blender releases (most
    notably `export_yup` vs the older `export_yup`/`export_format` pair), and a
    TypeError from an unknown kwarg aborts the export with no file written.
    Introspecting the operator's RNA means the add-on works across versions
    instead of pinning one.
    """
    supported = set()
    try:
        operator = bpy.ops.export_scene.gltf
        supported = {item.identifier for item in
                     operator.get_rna_type().properties}
    except (AttributeError, RuntimeError):
        # No introspection available; fall back to the modern names. An
        # unsupported kwarg still raises, which the caller reports.
        supported = set()

    wanted = {
        "filepath": filepath,
        "export_format": "GLB",
        "use_selection": use_selection,
        # THE axis convention. `+Y up` is what converts Blender's Z-up scene
        # into glTF's Y-up, and it must never be left at its default.
        "export_yup": True,
        # Apply modifiers so the exported mesh matches the viewport.
        "export_apply": True,
        # Skins and animations, or the .gltf half of the pair is inert.
        "export_skins": True,
        "export_animations": True,
        "export_def_bones": True,
    }

    if supported:
        return {key: value for key, value in wanted.items() if key in supported}
    return wanted


def _export_gltf(filepath, use_selection):
    """Call Blender's glTF exporter. Returns (ok, message)."""
    try:
        bpy.ops.export_scene.gltf(**_exporter_kwargs(filepath, use_selection))
    except TypeError as error:
        return False, (f"the installed glTF exporter rejected an argument: {error}. "
                       "The exporter's parameter names differ from the ones this "
                       "add-on knows about.")
    except RuntimeError as error:
        return False, f"Blender's glTF exporter failed: {error}"

    if not os.path.isfile(filepath):
        return False, f"the exporter reported success but wrote no file: {filepath}"
    return True, filepath


# ---------------------------------------------------------------------------
# operator
# ---------------------------------------------------------------------------

class WASTELAND_OT_export(bpy.types.Operator, ExportHelper):
    """Export the selection as .gltf plus a Wasteland .wlanim sidecar."""

    bl_idname = "wasteland.export"
    bl_label = "Export Wasteland Asset"
    bl_options = {"PRESET", "UNDO"}

    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb;*.gltf", options={"HIDDEN"})

    use_selection: BoolProperty(
        name="Selected Objects Only",
        description="Export only the selected objects. Off exports the whole scene.",
        default=True,
    )
    write_meta: BoolProperty(
        name="Write wasteland_meta.json",
        description="Record the axis and unit conventions beside the export.",
        default=True,
    )
    unit_scale: FloatProperty(
        name="Unit Scale",
        description="Metres per Blender unit. Blender's default is 1.0.",
        default=1.0,
        min=0.0001,
    )

    def execute(self, context):
        selection = context.selected_objects if self.use_selection else \
            list(context.scene.objects)

        ok, message = _export_gltf(self.filepath, self.use_selection)
        if not ok:
            self.report({"ERROR"}, message)
            return {"CANCELLED"}

        gltf_path = message
        stem, _ = os.path.splitext(gltf_path)
        sidecar_path = stem + ".wlanim"
        meta_path = stem + "_meta.json"

        bones, clips = _read_scene_animation(selection)
        asset = sidecar.build_asset(
            os.path.basename(stem), bones, clips)
        try:
            with open(sidecar_path, "w", encoding="utf-8") as handle:
                handle.write(sidecar.emit_asset(asset))
        except OSError as error:
            self.report({"ERROR"}, f"could not write {sidecar_path}: {error}")
            return {"CANCELLED"}

        if self.write_meta:
            meta = sidecar.build_meta(
                source="blender",
                up_axis="+Y",
                forward_axis="-Z",
                unit_scale=self.unit_scale,
                exporter_version=".".join(str(part) for part in ADDON_VERSION),
                gltf_path=os.path.basename(gltf_path),
                sidecar_path=os.path.basename(sidecar_path),
                notes=("Blender Z-up converted to glTF/engine Y-up by the "
                       "exporter's +Y up option. (x,y,z)_blender -> "
                       "(x,-z,y)_gltf."),
            )
            try:
                with open(meta_path, "w", encoding="utf-8") as handle:
                    handle.write(sidecar.emit_meta(meta))
            except OSError as error:
                # A missing meta file is not a failed export: the .gltf and the
                # .wlanim are both on disk and usable.
                self.report({"WARNING"}, f"could not write {meta_path}: {error}")

        if not bones:
            self.report({"WARNING"},
                        "no armature was selected -- the .gltf was written but "
                        "the .wlanim sidecar carries no skeleton or clips")

        self.report({"INFO"},
                    f"wrote {os.path.basename(gltf_path)} and "
                    f"{os.path.basename(sidecar_path)}")
        return {"FINISHED"}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "use_selection")
        layout.prop(self, "write_meta")
        layout.prop(self, "unit_scale")


class WASTELAND_PT_panel(bpy.types.Panel):
    """The Wasteland export panel: the preset, in one place."""

    bl_label = "Wasteland"
    bl_idname = "WASTELAND_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Wasteland"

    def draw(self, context):
        layout = self.layout
        layout.operator(WASTELAND_OT_export.bl_idname, icon="EXPORT")
        layout.label(text="Up: +Y (glTF/engine)")
        layout.label(text="Blender +Z up is converted on export")


classes = (WASTELAND_OT_export, WASTELAND_PT_panel)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
