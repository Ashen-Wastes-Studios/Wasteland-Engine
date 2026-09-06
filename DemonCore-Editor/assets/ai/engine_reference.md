# Wasteland engine — Python scripting reference

Use this when writing game scripts. Scripts are plain Python files, usually in
`assets/scripts/`. Only the API below exists — do not invent other functions.

## How scripts attach to entities

1. Add a `ScriptComponent` to an entity in the Scene Hierarchy.
2. Set `ScriptPath` to the file (e.g. `assets/scripts/Player.py`).
3. Set `ScriptName` to the class name (e.g. `Player`).
4. Press Play. The engine constructs `ScriptName(entity)` once, then calls
   `OnUpdateEntity(dt)` every frame. `dt` is a `Timestep`.

## Minimal script

```python
import Wasteland

class Mover:
    def __init__(self, entity):
        self.entity = entity
        self.speed = 5.0

    def OnUpdateEntity(self, dt):
        t = self.entity.GetTransform()
        t.Translation.x += self.speed * dt.GetSeconds()
```

## `Wasteland` module API

- `Vec2(x, y)`, `Vec3(x, y, z)`, `Vec4(x, y, z, w)` — `.x/.y/.z/.w` read/write.
- `Entity` (passed to your constructor):
  - `IsValid()`, `HasTransform()`, `GetTransform()` -> `TransformComponent`
  - `HasTag()`, `GetTag()` -> `TagComponent` (`.Tag` string)
- `TransformComponent`: `Translation`, `Rotation` (degrees XYZ), `Scale`
  (all `Vec3`); `GetTransform()` / `SetTransform(t, r, s)`.
- `Timestep dt`: `dt.GetSeconds()`, `dt.GetMilliseconds()`; `dt * speed`
  returns seconds scaled (e.g. `move = dt * self.speed`).
- Input: `Wasteland.IsKeyPressed(Wasteland.WL_KEY_W)` etc. Key names:
  `WL_KEY_A..Z`, `WL_KEY_0..9`, `WL_KEY_SPACE`, `WL_KEY_LEFT_SHIFT`,
  `WL_KEY_LEFT_CONTROL`, `WL_KEY_ESCAPE`, `WL_KEY_ENTER`, arrows
  `WL_KEY_UP/DOWN/LEFT/RIGHT`, `WL_KEY_F1..F12`.
- Mouse: `Wasteland.IsMouseButtonPressed(Wasteland.WL_MOUSE_BUTTON_LEFT)`,
  `Wasteland.GetMousePosition()` -> `(x, y)` tuple,
  `Wasteland.GetMouseDelta()` -> `(dx, dy)` tuple per call.
- `Scene`: `CreateEntity(name)`, `DestroyEntity(entity)` (rarely needed).

## Rules

- Always `import Wasteland` at the top.
- Guard with `if not self.entity.IsValid(): return`.
- Rotation is in degrees; Y is up; move with `dt.GetSeconds()`.
- Keep `__init__` light; do per-frame work in `OnUpdateEntity`.
- Answer with complete script files, then a one-line note on which
  `ScriptName` to put in the ScriptComponent.
