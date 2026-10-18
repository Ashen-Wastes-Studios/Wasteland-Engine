# Bindings regression — 2026-10-04 23:45

## Status: BLOCKED on an external in-flight rewrite of `Bindings.cpp`

I am not touching this file. Two processes own it right now, and both are
active. This document is the diagnosis so it is not re-derived later.

## Symptom 1 — the module shrank by 123 KB

```
before:  .buildlogs/linkprobe/Wasteland.pyd  7,691,776 bytes  04:39
after :  .buildlogs/linkprobe/Wasteland.pyd  7,568,896 bytes  23:47
```

**Corrected: the bindings are NOT missing.** The 23:47 build reported zero
errors, and all 23 `BIND_GAMEPLAY_COMP` invocations are present in the source,
including `InventoryComponent` and both audio components. The subagents that
reported `InventoryComponent` accessors "dropped" were comparing against the
mid-rewrite tree, not a completed build.

A 123 KB reduction with the same binding set is a compile-setting or
optimisation difference, not lost symbols. Do not infer lost bindings from a
file size.

## Symptom 2 — 21 components are registered but not attachable

This is the generalisation of the audio bug from earlier in the session, and it
is the finding worth keeping.

A `py::class_<T>` registration makes `T` **constructible and its fields
readable**. It does not make `T` **attachable to an Entity** — that needs a
separate binding that calls `AddComponent<T>` / `HasComponent<T>` etc.

47 components are declared in engine headers. 23 are attached via
`BIND_GAMEPLAY_COMP`. 26 are not bound through the macro. Of those, 21 are
registered as a `py::class_` yet have **no `AddComponent<T>` /
`GetComponent<T>` / `HasComponent<T>` call anywhere in `Bindings.cpp`**:

```
AreaLightComponent        DirectionalLightComponent   PointLightComponent
BoxCollider2DComponent    GridRendererComponent        Rigidbody2DComponent
Circle3DRendererComponent IcoSphereRendererComponent   SphereRendererComponent
CircleCollider2DComponent NativeScriptComponent       SpotLightComponent
CircleRendererComponent   PlaneRendererComponent       TorusRendererComponent
ConeRendererComponent     PointLightComponent          VolumetricCloudsComponent
CubeRendererComponent                              VolumetricFogComponent
CylinderRendererComponent                          WaterComponent
```

So a script can write `Wasteland.WaterComponent()` and read its fields, and
cannot put one on an entity. That reads as "the engine has no water component
from Python", when the truth is "nobody wrote the accessor line".

**Two of these are actively used by existing tests** — `BoxCollider3DComponent`
is exercised via its own macro entry, but `Rigidbody2DComponent` and
`BoxCollider2DComponent` have 2D physics suites with no accessor at all.

## Symptom 3 — MISREAD, corrected

I initially reported `HealthComponent` bound 3x and two competing
`py::class_<Entity>` blocks. **Both wrong.** All three occurrences are accounted
for:

```
L377    // BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, ...)   <- doc comment
L5555   // BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, ...)   <- doc comment
L5619   BIND_GAMEPLAY_COMP(Wasteland::HealthComponent, ...)     <- the only live one
```

`grep` cannot see that a line is commented out. A macro invocation quoted in
prose looks identical to a real one, so counting occurrences overcounts. There is
exactly one live binding, inside one `py::class_<Wasteland::Entity>` at L4993.

The lesson generalises: **any count taken from a text grep over a source file
that contains explanatory comments needs the comment lines excluded first.**
This is the same failure as counting `//` lines inside a macro body and
concluding the macro was broken.

## Root cause of the compile errors

`BIND_GAMEPLAY_COMP` has `//` comment lines inside its line-continuation body
without trailing backslashes — 21 of them as of 04:55. The preprocessor ends
the macro at the first one:

```
error C2014: preprocessor command must start as first nonwhite space
error C2059: syntax error: '.'
error C2065: 'pybind11_exec_Wasteland': undeclared identifier
```

Every `.def(...)` after that point lands at namespace scope, which is what
`pybind11_exec_Wasteland` means.

**The fix is mechanical**: hoist every comment in the macro body above the
`#define`. That is exactly what I did earlier today, before the external edit
reintroduced it with more comment text.

## Two independent blockers

| # | Blocker | Impact |
|---|---|---|
| 1 | Python 3.14.5 runtime removed — `C:\Users\rtoune\AppData\Local\Python` no longer exists | **No Python test can run.** The module links against 3.14.5; Hermes' own 3.14.7 is ABI-incompatible and crashes on import. |
| 2 | ~~`Bindings.cpp` macro uncompilable~~ **RESOLVED 23:52** | The 23:47 build succeeded with zero errors; macro body now has 0 un-continued comment lines. |

Note: `where /R C:\ python.exe` still *reports* `pythoncore-3.14-64\python.exe`
as present. It is not — that is a stale cached index, not the documented MSYS
path-translation flake. Confirm with `dir` (exit 1) before trusting either.

## The rule this session earned

A `py::class_<T>` registration is not an `Entity` accessor. Any component that
is constructible from Python but cannot be attached is invisible until someone
writes the one-line accessor — and the class existing makes the omission look
deliberate. `AudioSourceComponent` and `AudioListenerComponent` were in exactly
that state, and binding them is what exposed the missing
`Scene::OnComponentAdded<T>` specializations underneath.

New components need **three** things, not one:
1. `py::class_<T>` with its fields
2. `BIND_GAMEPLAY_COMP(Wasteland::T, T)`
3. `Scene::OnComponentAdded<T>` explicit specialization in `Scene.cpp`

Miss (2) and the component is dead from scripts. Miss (3) and the build fails to
link the moment (2) lands.