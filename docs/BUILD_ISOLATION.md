# Visual Blocks — build isolation note (18:0x, session)

## Symptom

Building `Wasteland.vcxproj` fails with a rotating set of errors that name a
different file each attempt:

```
error C1083: Cannot open compiler generated file: '...\Bindings.obj': Permission denied
error C1083: Cannot open compiler generated file: '...\SceneSerializer.obj': Permission denied
error C1083: Cannot open compiler generated file: '...\Application.obj': Permission denied
error C1041: cannot open program database '...\Wasteland.pdb'
error MSB6006: "CL.exe" exited with code 1     (no diagnostic at all)
```

## Cause — NOT a code defect

Concurrent MSBuild instances against the same `bin/` and `bin-int/` trees.
Observed at the time of failure:

```
PID 33652  WastelandPython.vcxproj  -p:Configuration=Debug    -p:OutDir=bin\Release-...\Wasteland\
PID 43384  Wasteland/Wasteland.vcxproj -p:Configuration=Release -p:OutDir=bin\Release-...\Wasteland\
```

A **Debug** and a **Release** build writing the same `OutDir`, plus repeated
killed builds leaving `mspdbsrv` handles open on `Wasteland.pdb` and
`*.obj`. Defender real-time protection is also on and scanning the build tree
(exclusions for `bin` / `bin-int` were added, but the handle contention from
overlapping builds is the dominant cause — exclusions did not clear it).

## Why a clean build still fails

MSBuild's incremental check compares source timestamp against `.obj`
timestamp. A build killed mid-write leaves the `.obj` newer than its source
while the file is truncated or absent, so MSBuild skips the compile and the
linker then fails on the missing object:

```
LINK : fatal error LNK1104: cannot open input file '...\Bindings.obj'
```

Fix is `touch` the source (forces recompile) or delete the intermediates.

## How to build cleanly

Stop every other build first — VS Code tasks, other terminals, IDE
background builds. Then, one target at a time, in dependency order:

```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"
cd C:/dev/Wasteland

"$MSB" Wasteland/vendor/GLFW/GLFW.vcxproj        -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/vendor/imgui/ImGui.vcxproj      -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/vendor/Glad/Glad.vcxproj        -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/vendor/yaml-cpp/yaml-cpp.vcxproj -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/vendor/Box2D/Box2D.vcxproj      -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/vendor/Box3D/Box3D.vcxproj      -p:Configuration=Release -p:Platform=x64
"$MSB" Wasteland/Wasteland.vcxproj               -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
"$MSB" WastelandPython.vcxproj                   -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
"$MSB" DemonCore-Editor/DemonCore-Editor.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```

Or just build `Wasteland.slnx` once, which orders the dependencies itself:

```bash
"$MSB" Wasteland.slnx -p:Configuration=Release -p:Platform=x64
```

Do **not** mix Debug and Release into the same `bin/` tree in one pass.

## Recovery from a killed build

```bash
# clear anything the dead build still holds
taskkill /F /IM MSBuild.exe /T
taskkill /F /IM cl.exe /T
taskkill /F /IM mspdbsrv.exe /T

rm -rf bin-int/Release-windows-x86_64/Wasteland
rm -f   bin/Release-windows-x86_64/Wasteland/Wasteland.pdb
```

## `premake5.exe` is NOT PRESENT — regeneration is currently impossible

**MEASURED 2026-10-05.** Neither `premake5.exe` nor a `premake5` on `PATH` exists in this
repo. Only `premake5.lua` and `tests/premake5.lua` are checked in. `find . -iname
"premake5*"` returns two `.lua` files and no executable.

This matters because of the long-standing silent-success trap in this project: `premake5.lua:57-69`
globs `%{prj.name}/src/**.cpp`, so **a new engine `.cpp` file is not added to the translation
unit list until premake regenerates** — and a build after adding one still exits `0` while
silently not compiling it. That trap is currently *unavoidable*, because the tool to avoid it is
missing.

Consequences while it stays missing:

- Adding a new `.cpp` under `Wasteland/src/` requires hand-editing the `.vcxproj` (or installing
  premake5 first) or it will not build, despite `BUILD_EXIT=0`.
- Do **not** read `BUILD_EXIT=0` as "the new file compiled". Verify the `.obj` exists and is
  newer than its source, or check the build log names the translation unit.

Any build script that calls `premake5.exe vs2022` and ignores the result is running a no-op; if
`premake5.exe` is absent the step exits `9009` (command not found). Check that exit code rather
than assuming regeneration happened.

## Verified good in this state

- All Visual Blocks translation units compile clean
  (`Compiler.cpp`, `Validator.obj`, `GraphSerializer`, `NodeRegistry`,
  `NodeLibrary`, `NodeLibraryV1`, `BehaviorTree`).
- `tests/test_visual_blocks.py` — 54/54 green against the Release `.pyd`,
  headless, asserting real read-back values (NPC movement, targeting,
  damage, latent RUNNING, cooldown rate-limiting).
