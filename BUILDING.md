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