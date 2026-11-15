# Wasteland Maya Export

Exports the Maya selection as a `.glb` mesh plus a Wasteland `.wlmaterial`-free
`.wlanim` animation sidecar, and records the conventions in
`wasteland_meta.json`.

```
selection  ──▶  hero.glb          mesh, skin, and the glTF animation channels
           ──▶  hero.wlanim       the engine's animation asset: rig + clips
           ──▶  hero_meta.json    axis/unit record
```

The `.glb` is written by **Maya's own glTF exporter**, which is *called*, not
vendored or reimplemented. The `.wlanim` is written by the **shared** module
`tools/dcc/wasteland_sidecar.py` — the same code the Blender add-on uses, so the
two paths cannot produce different formats.

---

## THE CONVENTIONS

### Maya is already Y-up — no axis conversion

| | Up | Forward | Default unit |
|---|---|---|---|
| **Maya** | **+Y** | −Z | **centimetres** |
| **Blender** | +Z | −Y | metres |
| glTF / Wasteland engine | +Y | −Z | metres |

This is the opposite of the Blender situation. Maya and glTF share the up axis,
so **no axis conversion is applied to the animation data**. The Blender path's
`convert_position_channels` (which permutes `(x, y, z) → (x, −z, y)`) must
**not** be applied to Maya data — doing so would rotate every animation 90
degrees.

### Maya's default unit is centimetres

A Maya scene exported without a unit conversion comes out **100× too large** in
the engine, and it is completely silent: the mesh is there, the rig is there, it
is just the size of a building. The wrapper reads `cmds.currentUnit(linear=True)`
and records the metres-per-unit factor in `wasteland_meta.json` rather than
assuming one.

| Maya unit | Metres per unit |
|---|---|
| mm | 0.001 |
| **cm** (default) | **0.01** |
| m | 1.0 |
| in | 0.0254 |
| ft | 0.3048 |
| yd | 0.9144 |

### FBX is not read here

**FBX is a proprietary format and nothing in this repository parses it.** This
wrapper exports glTF, which Maya produces and the engine reads. An artist who
needs FBX for another pipeline must convert it with Maya's own tools.

### Bringing it into the engine

* The `.glb` loads through `MeshComponent` (`Wasteland.Mesh.ImportGltf`).
* The `.wlanim` loads through `AnimationComponent`
  (`Wasteland.Animation.LoadAnimation`).

---

## Install

Copy `wasteland_export.py` into Maya's scripts directory:

```
%USERPROFILE%\Documents\maya\<version>\scripts\
```

`wasteland_sidecar.py` must be reachable — the wrapper looks in its own
directory and one level up (`tools/dcc/`), so the recommended layout is to keep
the repository checked out and copy just this file, then point the wrapper at
the checkout.

## Use

1. Select the joints and any mesh objects to export.
2. From Maya's Script Editor (Python tab):

   ```python
   import wasteland_export
   wasteland_export.export_selection("C:/path/hero.glb")
   ```

   or bind that call to a shelf button.

The `.wlanim` and `_meta.json` are written beside the `.glb`, named after its
stem.

---

## Verification status

**Verified headlessly:** the sidecar *format* — `wasteland_sidecar.py` builds a
`.wlanim` from a rig and the engine parses it back, asserting bone hierarchy and
clip duration. `tests/test_wlanim_sidecar.py`. This is shared code, so the Maya
and Blender paths are verified to the same degree.

**NOT verified:** the Maya glue in `wasteland_export.py`. Maya is not installed
on the machine this was written on, so none of the `maya.cmds` calls have ever
run. Concretely unverified:

* the glTF export invocation and its option names;
* the joint/attribute enumeration;
* the fixed-step animation sampling;
* the Euler-to-quaternion conversion and the `rotateOrder` read;
* the working-unit query.

The plan's stated verification — *"the same rigged cube exported from Maya
imports with the same bone hierarchy and clip duration as the Blender export"* —
needs a machine with Maya. Only its engine half is covered here.
