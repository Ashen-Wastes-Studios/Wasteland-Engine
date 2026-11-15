# Wasteland Blender Export

Exports a Blender selection as a `.glb` mesh plus a Wasteland `.wlanim`
animation sidecar, and records the axis convention in `wasteland_meta.json`.

```
selection  ──▶  hero.glb          mesh, skin, and the glTF animation channels
           ──▶  hero.wlanim       the engine's animation asset: rig + clips
           ──▶  hero_meta.json    axis/unit record
```

The `.glb` is written by **Blender's built-in glTF exporter**, which is
MIT-licensed and is *called*, not vendored or copied. Nothing here is a
dependency of the engine binary.

---

## THE AXIS CONVENTION — read this first

This is the single most common source of **"my model is lying on its side"**.

| | Up | Forward |
|---|---|---|
| Blender | **+Z** | −Y |
| glTF / Wasteland engine | **+Y** | −Z |

Blender's glTF exporter converts between them when **`+Y up`** is enabled, and
this add-on **always** enables it. The conversion applied to a point is a +90°
rotation about X:

```
(x, y, z)_blender  ──▶  (x, −z, y)_gltf
```

### What that means for the two files

* **The `.glb` is already Y-up.** The exporter did the conversion.
* **The `.wlanim` bone hierarchy is convention-independent.** Bone *names* and
  *parents* do not change between coordinate systems, so they are written
  verbatim.
* **The `.wlanim` clip keys ARE converted.** They must live in the same space as
  the skeleton the engine builds from the `.glb` — which is already Y-up. The
  conversion **permutes the axes**, so it is not a per-channel sign flip:

  | glTF channel | Blender source |
  |---|---|
  | `position_x` | `location[0]` |
  | `position_y` | `−location[2]` |
  | `position_z` | `location[1]` |

  (and identically for `scale_*`). Rotations are conjugated with the basis
  rotation rather than permuted, because a rotation's *axis* is carried into
  the new basis: `q' = r · q · r⁻¹` where `r` is +90° about X.

* **Rest poses are omitted from the sidecar.** The engine derives them from the
  `.glb`'s skin, which is already correct. Writing a second copy would create a
  second source of truth that can disagree with the skin.

### If a character plays back rotated

Check `wasteland_meta.json` first — if `up_axis` is not `"+Y"`, the export did
not take the conversion path. If it is `"+Y"` and the animation is still
wrong, the quaternion conjugation in `wasteland_export.py::_convert_quaternion`
is the first thing to look at. It is the one part of the axis handling that has
**not** been verified against a live Blender export (see *Verification status*).

---

## Install

`wasteland_sidecar.py` is the SHARED format module, one level up at
`tools/dcc/wasteland_sidecar.py`; the Maya export path uses the same file.
The add-on finds it by looking one directory up.

1. Copy `wasteland_export.py` into Blender's add-ons directory:

   ```
   %APPDATA%\Blender Foundation\Blender\<version>\scripts\addons\wasteland\
   ```

   or use **Edit > Preferences > Add-ons > Install…** and select
   `wasteland_export.py`.
2. Enable **Import-Export: Wasteland Export**.

`wasteland_sidecar.py` is imported by the add-on and carries no Blender
dependency, so it also runs standalone for tooling and tests
(`tests/test_wlanim_sidecar.py`).

## Use

1. Select the armature and any mesh objects to export.
2. **View3D > Sidebar (N) > Wasteland > Export Wasteland Asset**, or run the
   operator `wasteland.export` from the F3 search.
3. Choose the destination. The `.wlanim` and `_meta.json` are written beside the
   `.glb`, named after its stem.

Options in the export dialog:

| Option | Meaning |
|---|---|
| **Selected Objects Only** | Off exports the whole scene. |
| **Write wasteland_meta.json** | Record the axis/unit conventions. |
| **Unit Scale** | Metres per Blender unit. Blender's default is 1.0. |

### Bringing it into the engine

* The `.glb` loads through `MeshComponent` (`Wasteland.Mesh.ImportGltf`).
* The `.wlanim` loads through `AnimationComponent`
  (`Wasteland.Animation.LoadAnimation`).

---

## Verification status

**Verified headlessly:** the sidecar *format* — `wasteland_sidecar.py` builds a
`.wlanim` from a synthetic rig and the engine parses it back, asserting bone
hierarchy, key counts and clip duration. `tests/test_wlanim_sidecar.py`.

**NOT verified:** the Blender glue in `wasteland_export.py`. Blender is not
installed on the machine this was written on, so `bpy.ops.export_scene.gltf`
has never been called by this code, and no screenshot of the export dialog
exists. Concretely unverified:

* the exporter parameter set, including `export_yup` (the add-on introspects the
  operator's RNA and filters to supported names, so it should adapt — but that
  introspection has not run);
* f-curve reading and the `data_path` parsing;
* the quaternion conjugation;
* the export panel and operator registration.

The plan's stated verification — *"export a rigged cube from Blender, import it
in the engine, assert the bone hierarchy and clip duration match; screenshot the
export dialog"* — needs a machine with Blender. Only its engine half is covered
here.
