# Wasteland Release build — working recipe (verified 2026-10-01)

## Why this file exists

The default build path (`make config=release`, or MSBuild on the .slnx) does not
work reliably on this machine. Three separate problems, all diagnosed:

1. **`make` (GCC) cannot build MSVC projects.** `WastelandPython.make` invokes
   GCC, which does not understand MSVC's `/utf-8` flag. Premake generates both
   a `.make` (GCC) and a `.vcxproj` (MSVC); only the `.vcxproj` is usable here.
   **Use MSBuild, never `make`.**

2. **The build needs `/FS`.** The projects set `/Zi` without `/FS`, so two
   `cl.exe` processes writing the same PDB collide with **C1041**. This is not
   a stray-process problem — it is a missing flag. Parallel builds always fail;
   serial (`-m:1`) builds usually get lucky. Do not fight it by killing
   processes; pass `/FS`.

3. **PDB/PCH lock contention.** `Wasteland.pdb` and `Wasteland.pch` can be left
   locked by an aborted build, and a locked `.pch` fails with a *misleading*
   error — `C1083: Cannot open compiler intermediate file ... Invalid argument`.
   "Invalid argument" reads like a bad path, but it is really a locked file.
   Delete the file; if it cannot be deleted, a process still holds it.

## MSBuild location

VS **18** Community (not 2022, not 17 — those paths do not exist here):

```
C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe
```

`amd64\MSBuild.exe` is required; the non-`amd64` one works but is slower.

## The working two-step Release build

`Wasteland.vcxproj` is a **StaticLib** and `WastelandPython.vcxproj` is the
**SharedLib** (`.pyd`) that links it. Build them in that order. Skip vendor
projects via `BuildProjectReferences=false` — their `.lib` files are already
built and do not need rebuilding:

```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe"
INT='C:\Users\rtoue\AppData\Local\hermes\cache\scratch\intW'
rm -rf "$INT" && mkdir -p "$INT"

# Step 1: engine static lib
"$MSB" Wasteland/Wasteland.vcxproj \
  -p:Configuration=Release -p:Platform=x64 \
  -p:BuildProjectReferences=false \
  -p:IntDir="$INT\\" \
  -p:OutDir="C:\dev\Wasteland\bin\Release-windows-x86_64\Wasteland\\" \
  -p:AdditionalOptions="/FS /Fd$INT\\W.pdb" \
  -m:1 -v:minimal -nologo

# Step 2: python module (.pyd)
"$MSB" WastelandPython.vcxproj \
  -p:Configuration=Release -p:Platform=x64 \
  -p:BuildProjectReferences=false \
  -m:1 -v:minimal -nologo
```

Output lands at
`bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.pyd`.

### Why redirect IntDir

The default `IntDir` (`bin-int/Release-windows-x86_64/Wasteland/`) is where the
wedged `.pch` lives. Redirecting to a scratch dir sidesteps it without needing
to delete a locked file.

## Timeouts

The engine lib is a **single `cl.exe` compiling ~64 translation units in one
invocation** (the project sets `MultiProcessorCompilation` off / unity-style
batching). This takes **several minutes and exceeds the 600 s foreground
limit**. Run it with `background=true` or it will be killed mid-compile and
leave another locked `.pch` behind.

## Python interpreter

The module targets **Python 3.14** and needs that exact interpreter:

```
C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe
```

`PYTHONPATH` does **not** work (embedded `._pth`), so tests must insert the
module directory into `sys.path` themselves — see `tests/headless/wlpath.py`.

## Running the headless tests

```bash
cd C:/dev/Wasteland/tests/headless
"C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe" runner.py
```

No pytest — `runner.py` is a hand-rolled harness (no pytest in this
interpreter).

## Visual Blocks / behavior-tree verification order

Use CPython 3.14.5 and a fresh probe module for regression tests. Before each
build, confirm no `cl.exe`, `link.exe`, `MSBuild.exe`, or editor build is active;
serialize every MSBuild invocation and never terminate shared compiler workers
by image name. The supported Release gate is:

1. `bash tools/buildpy-probe.sh Release` builds a probe at
   `.buildlogs/linkprobe/Wasteland.pyd` without overwriting the canonical module.
   Require `MSBUILD_RC=0`, a fresh non-empty `MZ` artifact, and no stale-object
   warning in the probe report.
2. Set `WASTELAND_MODULE_DIR` to `C:/dev/Wasteland/.buildlogs/linkprobe` and run
   the targeted headless and structural suites against that artifact.
3. Promote only after validation with `bash tools/buildpy-canonical.sh`, then
   verify the canonical module is byte-identical to the tested probe.
4. Build editor/ToolServer changes serially after the module build:

   ```bash
   bash tools/msb.sh C:/dev/Wasteland/DemonCore-Editor/DemonCore-Editor.vcxproj -p:BuildProjectReferences=false
   ```
5. Run `bash tools/run_all_suites.py` last so it can cross-check discovered
   suite totals. Any nonzero process exit is a failure, even if test summaries
   were printed before shutdown.

Example no-build verification after step 2, from the repository root:

```bash
PY='C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe'
MOD='C:/dev/Wasteland/.buildlogs/linkprobe'
export WASTELAND_MODULE_DIR="$MOD"

# Source-only aggregate-counter tests do not need the engine module.
"$PY" -u tests/test_run_all_suites.py
"$PY" -u tests/test_visual_blocks_schema.py

# NB / registry-emitter, flow, and enemy-tree behavior suites.
"$PY" -u tests/visualblocks/test_wlbtree_compiler.py
"$PY" -u tests/visualblocks/test_node_library.py
"$PY" -u tests/visualblocks/test_visualblocks.py
"$PY" -u tests/visualblocks/test_enemy_tree.py
"$PY" -u tests/test_visual_blocks_flow.py

# Finally include shipped assets and Scene/BehaviorTreeSystem integration.
"$PY" -u tests/headless/runner.py test_shipped_graphs test_tree_attach test_behavior_trace test_script_shutdown test_inventory test_audio_scene
"$PY" -u tools/run_all_suites.py
```

The aggregate pins promoted suite check totals to prevent silent omissions;
unrecognized output is `UNCOUNTED`, not a green zero. It reports counted checks
separately from uncounted suites, and treats every nonzero process exit as a
failure—even when all individual assertions printed as passing.

### Structural coverage boundaries

The promoted suites exercise tree compilation, deterministic serialization,
registry taxonomy, per-node behavior, flow control/math/logic, runtime Scene
effects, and real enemy-tree ticks/read-backs. `test_shipped_graphs.py` discovers
and loads/validates `.wlbtree` and `.wlblocks` assets and checks generated Python
parses and compiles deterministically for both graph kinds. The flow suite also
compiles and instantiates shipped flow samples, but does not tick each sample's
event handler.

Still not claimed as behavior coverage: the `Log` node's externally visible
runtime side effect; registry re-registration/idempotence; flow `ForEach` and
graph-variable identifier sanitization/collision cases; and every flow node in a
live Scene. Current flow tests cover representative control flow, expressions,
variable read/write, and several real Scene operations, but node registration or
generated-source inspection alone does not prove an unexercised emitter works at
runtime. Keep those gaps explicit until a no-build runtime assertion can exercise
them against a current module artifact.